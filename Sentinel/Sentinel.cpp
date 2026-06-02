#include <windows.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <iostream>
#include <string>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <vector>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <system_error>

namespace fs = std::filesystem;

struct Config {
    std::string cameraName = "Integrated Camera";
    int         segmentDurationS = 60;
    int         preTriggerMins = 5;
    int         postTriggerMins = 10;
    int         maxRecordingMins = 30;
    int         quickUnlockS = 3;
    std::string baseDir = "monitor_sessions\\";
};

static Config g_cfg;

static std::string Trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static void WriteDefaultConfig(const std::string& iniPath)
{
    std::ofstream f(iniPath);
    if (!f.is_open()) {
        std::cerr << "[Config] Could not create sentinel.ini at " << iniPath << "\n";
        return;
    }
    f << "; Sentinel configuration\n"
        << "; Place this file in the same directory as Sentinel.exe\n"
        << "; To find your camera name, run: ffmpeg -list_devices true -f dshow -i dummy\n"
        << "\n"
        << "; DirectShow device name of your webcam\n"
        << "CameraName=Integrated Camera\n"
        << "\n"
        << "; Minutes of footage to keep before the trigger event\n"
        << "PreTriggerMins=5\n"
        << "\n"
        << "; Minutes of footage to keep after the trigger event\n"
        << "PostTriggerMins=10\n"
        << "\n"
        << "; Rolling window: segments older than this are pruned while locked and untriggered\n"
        << "MaxRecordingMins=30\n"
        << "\n"
        << "; Unlock within this many seconds with no trigger -> treated as owner, footage deleted\n"
        << "QuickUnlockSecs=3\n";
    std::cout << "[Config] Created sentinel.ini with defaults. "
        << "Set CameraName before use.\n";
}

static void LoadConfig(const std::string& iniPath)
{
    std::ifstream f(iniPath);
    if (!f.is_open()) {
        WriteDefaultConfig(iniPath);
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const auto key = Trim(line.substr(0, eq));
        const auto val = Trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        if (key == "CameraName")       g_cfg.cameraName = val;
        else if (key == "PreTriggerMins")    g_cfg.preTriggerMins = std::stoi(val);
        else if (key == "PostTriggerMins")   g_cfg.postTriggerMins = std::stoi(val);
        else if (key == "MaxRecordingMins")  g_cfg.maxRecordingMins = std::stoi(val);
        else if (key == "QuickUnlockSecs")   g_cfg.quickUnlockS = std::stoi(val);
    }

    std::cout << "[Config] Loaded sentinel.ini\n"
        << "         Camera         : " << g_cfg.cameraName << "\n"
        << "         PreTrigger     : " << g_cfg.preTriggerMins << " min\n"
        << "         PostTrigger    : " << g_cfg.postTriggerMins << " min\n"
        << "         MaxRecording   : " << g_cfg.maxRecordingMins << " min\n"
        << "         QuickUnlock    : " << g_cfg.quickUnlockS << " s\n";
}

static std::string GetExeDir()
{
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string path(buf);
    const auto slash = path.find_last_of("\\/");
    return (slash != std::string::npos) ? path.substr(0, slash + 1) : ".\\";
}

enum class SessionCmd { Lock, Unlock };

static std::queue<SessionCmd>    g_cmdQueue;
static std::mutex                g_cmdMutex;
static std::condition_variable   g_cmdCv;

static void PostSessionCmd(SessionCmd cmd)
{
    {
        std::lock_guard<std::mutex> lk(g_cmdMutex);
        g_cmdQueue.push(cmd);
    }
    g_cmdCv.notify_one();
}

std::atomic<bool> g_isLocked{ false };
std::atomic<bool> g_isRecording{ false };
std::atomic<bool> g_isTriggered{ false };
std::atomic<bool> g_shutdown{ false };

std::chrono::system_clock::time_point g_triggerTime;
std::chrono::system_clock::time_point g_lockTime;
std::string g_currentSessionDir;

std::mutex g_segmentMutex;
std::vector<std::chrono::system_clock::time_point> g_segmentStartTimes;

HANDLE    g_hFFmpegProcess = NULL;
HANDLE    g_hStdInWrite = NULL;
HANDLE    g_hMonitorThread = NULL;
HINSTANCE g_hInstance = NULL;
HWND      g_hSessionWnd = NULL;
HWND      g_hRawInputWnd = NULL;

static bool LaunchProcess(const std::string& cmd,
    HANDLE* outProcess,
    HANDLE  hStdIn = NULL,
    HANDLE  hStdOut = NULL,
    HANDLE  hStdErr = NULL)
{
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    if (hStdIn || hStdOut || hStdErr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = hStdIn ? hStdIn : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hStdOut ? hStdOut : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = hStdErr ? hStdErr : GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi))
    {
        std::cerr << "[Error] CreateProcess failed: " << cmd
            << " (error " << GetLastError() << ")\n";
        return false;
    }

    if (outProcess) *outProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}

static void StartFFmpegRecording(const std::string& outputDir)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE hStdInRead = NULL;
    if (!CreatePipe(&hStdInRead, &g_hStdInWrite, &sa, 0)) {
        std::cerr << "[Error] CreatePipe failed (" << GetLastError() << ")\n";
        return;
    }
    SetHandleInformation(g_hStdInWrite, HANDLE_FLAG_INHERIT, 0);

    std::string cmd =
        "ffmpeg.exe -f dshow -i video=\"" + g_cfg.cameraName + "\""
        " -f segment -segment_time " + std::to_string(g_cfg.segmentDurationS) +
        " -segment_format mp4 -reset_timestamps 1"
        " \"" + outputDir + "\\seg_%03d.mp4\"";

    if (!LaunchProcess(cmd, &g_hFFmpegProcess,
        hStdInRead,
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE)))
    {
        CloseHandle(hStdInRead);
        CloseHandle(g_hStdInWrite);
        g_hStdInWrite = NULL;
        return;
    }

    CloseHandle(hStdInRead);
}

static void StopFFmpegRecording()
{
    if (!g_hFFmpegProcess || !g_hStdInWrite) return;

    const char* quitCmd = "q\n";
    DWORD written = 0;
    WriteFile(g_hStdInWrite, quitCmd, (DWORD)strlen(quitCmd), &written, NULL);

    if (WaitForSingleObject(g_hFFmpegProcess, 10000) == WAIT_TIMEOUT) {
        std::cerr << "[Warning] FFmpeg did not exit in 10 s; terminating.\n";
        TerminateProcess(g_hFFmpegProcess, 1);
    }

    CloseHandle(g_hFFmpegProcess);
    CloseHandle(g_hStdInWrite);
    g_hFFmpegProcess = NULL;
    g_hStdInWrite = NULL;
}

static void StartSession()
{
    if (g_isRecording) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm localTime = {};
    localtime_s(&localTime, &t);

    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", &localTime);

    g_currentSessionDir = g_cfg.baseDir + "session_" + timeStr;
    fs::create_directories(g_currentSessionDir);

    {
        std::lock_guard<std::mutex> lock(g_segmentMutex);
        g_segmentStartTimes.clear();
    }

    g_isTriggered = false;
    g_isRecording = true;
    g_lockTime = std::chrono::system_clock::now();

    std::cout << "[Session] Laptop locked. Recording to "
        << g_currentSessionDir << "\n";
    StartFFmpegRecording(g_currentSessionDir);
}

static void EndSession()
{
    if (!g_isRecording) return;

    std::cout << "[Session] Laptop unlocked. Stopping recording.\n";
    StopFFmpegRecording();
    g_isRecording = false;

    auto lockedDuration = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - g_lockTime).count();

    if (!g_isTriggered) {
        if (lockedDuration <= g_cfg.quickUnlockS)
            std::cout << "[Session] Quick unlock (" << lockedDuration
            << " s) — owner detected. Deleting footage.\n";
        else
            std::cout << "[Session] No activity detected. Deleting raw footage.\n";
        fs::remove_all(g_currentSessionDir);
        return;
    }

    std::cout << "[Session] Activity detected. Trimming and merging footage...\n";

    auto preBoundary = g_triggerTime - std::chrono::minutes(g_cfg.preTriggerMins);
    auto postBoundary = g_triggerTime + std::chrono::minutes(g_cfg.postTriggerMins);

    std::vector<fs::path> keptFiles;
    std::vector<fs::path> allSegments;

    for (const auto& entry : fs::directory_iterator(g_currentSessionDir))
        if (entry.path().extension() == ".mp4")
            allSegments.push_back(entry.path());

    std::sort(allSegments.begin(), allSegments.end());

    {
        std::lock_guard<std::mutex> lock(g_segmentMutex);
        for (size_t i = 0; i < allSegments.size(); ++i) {
            std::chrono::system_clock::time_point segStart;
            if (i < g_segmentStartTimes.size() &&
                g_segmentStartTimes[i] != std::chrono::system_clock::time_point{}) {
                segStart = g_segmentStartTimes[i];
            }
            else {
                auto ftime = fs::last_write_time(allSegments[i]);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                segStart = sctp - std::chrono::seconds(g_cfg.segmentDurationS);
            }

            auto segEnd = segStart + std::chrono::seconds(g_cfg.segmentDurationS);
            if (segStart < postBoundary && segEnd > preBoundary)
                keptFiles.push_back(allSegments[i]);
            else
                fs::remove(allSegments[i]);
        }
    }

    if (keptFiles.empty()) {
        std::cout << "[Error] No relevant video segments found.\n";
        return;
    }

    std::string concatListPath = g_currentSessionDir + "\\concat.txt";
    {
        std::ofstream listFile(concatListPath);
        if (!listFile) {
            std::cerr << "[Error] Could not write concat list.\n";
            return;
        }
        for (const auto& f : keptFiles)
            listFile << "file '" << f.filename().string() << "'\n";
    }

    std::string finalVideo = g_currentSessionDir + "\\incident.mp4";
    std::string mergeCmd =
        "ffmpeg.exe -y -f concat -safe 0"
        " -i \"" + concatListPath + "\""
        " -c copy \"" + finalVideo + "\"";

    HANDLE hMerge = NULL;
    std::cout << "[Processing] Merging " << keptFiles.size() << " segments...\n";
    if (LaunchProcess(mergeCmd, &hMerge)) {
        WaitForSingleObject(hMerge, 30000);
        CloseHandle(hMerge);
    }

    for (const auto& f : keptFiles) fs::remove(f);
    fs::remove(concatListPath);

    std::cout << "[Success] Incident saved to: " << finalVideo << "\n";
}

static DWORD WINAPI SessionWorkerThread(LPVOID)
{
    while (!g_shutdown) {
        std::unique_lock<std::mutex> lk(g_cmdMutex);
        g_cmdCv.wait(lk, [] {
            return !g_cmdQueue.empty() || g_shutdown.load();
            });

        while (!g_cmdQueue.empty()) {
            const SessionCmd cmd = g_cmdQueue.front();
            g_cmdQueue.pop();
            lk.unlock();

            if (cmd == SessionCmd::Lock)   StartSession();
            else if (cmd == SessionCmd::Unlock) EndSession();

            lk.lock();
        }
    }
    return 0;
}

static DWORD WINAPI SegmentWatcherThread(LPVOID)
{
    size_t knownCount = 0;

    while (!g_shutdown) {
        if (g_isRecording && !g_currentSessionDir.empty()) {
            std::vector<fs::path> segments;
            std::error_code ec;
            for (const auto& entry :
                fs::directory_iterator(g_currentSessionDir, ec))
                if (entry.path().extension() == ".mp4")
                    segments.push_back(entry.path());

            std::sort(segments.begin(), segments.end());

            if (segments.size() > knownCount) {
                auto now = std::chrono::system_clock::now();
                std::lock_guard<std::mutex> lock(g_segmentMutex);
                while (g_segmentStartTimes.size() < segments.size())
                    g_segmentStartTimes.push_back(now);
                knownCount = segments.size();
            }

            if (!g_isTriggered) {
                auto cutoff = std::chrono::system_clock::now()
                    - std::chrono::minutes(g_cfg.maxRecordingMins);
                std::lock_guard<std::mutex> lock(g_segmentMutex);
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (i < g_segmentStartTimes.size()
                        && g_segmentStartTimes[i] != std::chrono::system_clock::time_point{}
                        && g_segmentStartTimes[i] < cutoff)
                    {
                        std::error_code removeEc;
                        fs::remove(segments[i], removeEc);
                        g_segmentStartTimes[i] = std::chrono::system_clock::time_point{};
                    }
                }
            }
        }
        else {
            knownCount = 0;
        }
        Sleep(500);
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_WTSSESSION_CHANGE) {
        if (wParam == WTS_SESSION_LOCK) {
            g_isLocked = true;
            PostSessionCmd(SessionCmd::Lock);
        }
        else if (wParam == WTS_SESSION_UNLOCK) {
            g_isLocked = false;
            PostSessionCmd(SessionCmd::Unlock);
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT && g_isLocked && g_isRecording && !g_isTriggered) {
        std::cout << "[Trigger] Physical activity detected on locked machine!\n";
        g_isTriggered = true;
        g_triggerTime = std::chrono::system_clock::now();
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI MessageLoopThread(LPVOID lpParam)
{
    ATOM sessionClass = static_cast<ATOM>(reinterpret_cast<uintptr_t>(lpParam));

    g_hSessionWnd = CreateWindowExW(0, MAKEINTATOM(sessionClass), L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInstance, NULL);
    if (!g_hSessionWnd) {
        std::cerr << "[Error] Failed to create session monitor window ("
            << GetLastError() << ")\n";
        return 1;
    }
    WTSRegisterSessionNotification(g_hSessionWnd, NOTIFY_FOR_THIS_SESSION);

    MSG msg;
    while (!g_shutdown && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WTSUnRegisterSessionNotification(g_hSessionWnd);
    return 0;
}

static DWORD WINAPI MonitorThread(LPVOID)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RawInputWndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = L"RawInputReceiver";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        std::cerr << "[Error] RegisterClassExW failed (" << GetLastError() << ")\n";
        return 1;
    }

    g_hRawInputWnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        MAKEINTATOM(atom), L"",
        WS_POPUP, 0, 0, 1, 1,
        NULL, NULL, g_hInstance, NULL);
    if (!g_hRawInputWnd) {
        std::cerr << "[Error] CreateWindowExW failed (" << GetLastError() << ")\n";
        return 1;
    }

    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = g_hRawInputWnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = g_hRawInputWnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
        std::cerr << "[Warning] RegisterRawInputDevices failed ("
        << GetLastError() << ")\n";

    MSG msg;
    while (!g_shutdown && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hRawInputWnd) DestroyWindow(g_hRawInputWnd);
    return 0;
}

static std::string g_iniPath;

static DWORD WINAPI ConfigWatcherThread(LPVOID)
{
    const std::string watchDir = GetExeDir();

    HANDLE hDir = CreateFileA(
        watchDir.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        NULL);

    if (hDir == INVALID_HANDLE_VALUE) {
        std::cerr << "[Config] Watcher could not open directory ("
            << GetLastError() << "). Live reload disabled.\n";
        return 1;
    }

    alignas(DWORD) char buf[2048] = {};
    DWORD bytesReturned = 0;

    while (!g_shutdown) {
        BOOL ok = ReadDirectoryChangesW(
            hDir,
            buf, sizeof(buf),
            FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned,
            NULL, NULL);

        if (!ok || g_shutdown) break;
        if (bytesReturned == 0) continue;

        const auto* record = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf);
        bool iniChanged = false;
        for (;;) {
            char narrow[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0,
                record->FileName,
                static_cast<int>(record->FileNameLength / sizeof(WCHAR)),
                narrow, MAX_PATH, NULL, NULL);

            if (_stricmp(narrow, "sentinel.ini") == 0)
                iniChanged = true;

            if (record->NextEntryOffset == 0) break;
            record = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const char*>(record) + record->NextEntryOffset);
        }

        if (!iniChanged) continue;

        if (g_isRecording) {
            std::cout << "[Config] sentinel.ini changed — will reload after session ends.\n";
            while (g_isRecording && !g_shutdown)
                Sleep(500);
            if (g_shutdown) break;
        }

        std::cout << "[Config] sentinel.ini changed — reloading...\n";
        LoadConfig(g_iniPath);
    }

    CloseHandle(hDir);
    return 0;
}

int main()
{
    g_hInstance = GetModuleHandle(NULL);

    g_iniPath = GetExeDir() + "sentinel.ini";
    LoadConfig(g_iniPath);

    fs::create_directories(g_cfg.baseDir);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = "SessionMonitorClass";
    ATOM sessionClass = RegisterClassExA(&wc);
    if (!sessionClass) {
        std::cerr << "[Error] Failed to register window class ("
            << GetLastError() << ")\n";
        return 1;
    }

    HANDLE hMsgLoop = CreateThread(NULL, 0, MessageLoopThread,
        reinterpret_cast<LPVOID>(
            static_cast<uintptr_t>(sessionClass)),
        0, NULL);
    HANDLE hWorker = CreateThread(NULL, 0, SessionWorkerThread, NULL, 0, NULL);
    HANDLE hSegWatcher = CreateThread(NULL, 0, SegmentWatcherThread, NULL, 0, NULL);
    HANDLE hCfgWatcher = CreateThread(NULL, 0, ConfigWatcherThread, NULL, 0, NULL);
    g_hMonitorThread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);

    std::cout << "=== Sentinel Active ===\n";
    std::cout << "Waiting for laptop to be locked (Win+L)...\n";
    std::cout << "Press Ctrl+C to exit.\n";

    while (!g_shutdown)
        Sleep(10000);

    g_shutdown = true;
    g_cmdCv.notify_all();

    if (hMsgLoop) {
        PostThreadMessage(GetThreadId(hMsgLoop), WM_QUIT, 0, 0);
        WaitForSingleObject(hMsgLoop, 3000); CloseHandle(hMsgLoop);
    }
    if (g_hMonitorThread) {
        PostMessage(g_hRawInputWnd, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hMonitorThread, 3000); CloseHandle(g_hMonitorThread);
    }
    if (hSegWatcher) { WaitForSingleObject(hSegWatcher, 3000); CloseHandle(hSegWatcher); }
    if (hWorker) { WaitForSingleObject(hWorker, 3000); CloseHandle(hWorker); }
    if (hCfgWatcher) { WaitForSingleObject(hCfgWatcher, 3000); CloseHandle(hCfgWatcher); }

    return 0;
}