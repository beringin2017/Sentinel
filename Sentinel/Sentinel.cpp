#include <windows.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <iostream>
#include <string>
#include <cstring>
#include <ctime>
#include <vector>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

const std::string CAMERA_NAME = "Integrated Camera";
const int SEGMENT_DURATION_SECONDS = 60;
const int PRE_TRIGGER_MINS = 5;
const int POST_TRIGGER_MINS = 10;
const std::string BASE_DIR = "monitor_sessions\\";

std::atomic<bool> g_isLocked{ false };
std::atomic<bool> g_isRecording{ false };
std::atomic<bool> g_isTriggered{ false };
std::atomic<bool> g_shutdown{ false };

std::chrono::system_clock::time_point g_triggerTime;
std::string g_currentSessionDir;

std::mutex g_segmentMutex;
std::vector<std::chrono::system_clock::time_point> g_segmentStartTimes;

HANDLE g_hFFmpegProcess = NULL;
HANDLE g_hStdInWrite = NULL;
HANDLE g_hMonitorThread = NULL;
HINSTANCE g_hInstance = NULL;
HWND g_hSessionWnd = NULL;

static bool LaunchProcess(const std::string& cmd,
    HANDLE* outProcess,
    HANDLE  hStdIn = NULL,
    HANDLE  hStdOut = NULL,
    HANDLE  hStdErr = NULL)
{
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    if (hStdIn || hStdOut || hStdErr) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = hStdIn ? hStdIn : GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hStdOut ? hStdOut : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = hStdErr ? hStdErr : GetStdHandle(STD_ERROR_HANDLE);
    }

    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
        NULL, NULL, &si, &pi))
    {
        std::cerr << "[Error] CreateProcess failed for: " << cmd
            << "  (error " << GetLastError() << ")\n";
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
        "ffmpeg.exe -f dshow -i video=\"" + CAMERA_NAME + "\""
        " -f segment -segment_time " + std::to_string(SEGMENT_DURATION_SECONDS) +
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

    DWORD waitResult = WaitForSingleObject(g_hFFmpegProcess, 10000);
    if (waitResult == WAIT_TIMEOUT) {
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

    g_currentSessionDir = BASE_DIR + "session_" + timeStr;
    fs::create_directories(g_currentSessionDir);

    {
        std::lock_guard<std::mutex> lock(g_segmentMutex);
        g_segmentStartTimes.clear();
    }

    g_isTriggered = false;
    g_isRecording = true;

    std::cout << "[Session] Laptop locked. Recording to " << g_currentSessionDir << "\n";
    StartFFmpegRecording(g_currentSessionDir);
}

static void EndSession()
{
    if (!g_isRecording) return;

    std::cout << "[Session] Laptop unlocked. Stopping recording.\n";
    StopFFmpegRecording();
    g_isRecording = false;

    if (!g_isTriggered) {
        std::cout << "[Session] No activity detected. Deleting raw footage.\n";
        fs::remove_all(g_currentSessionDir);
        return;
    }

    std::cout << "[Session] Activity detected. Trimming and merging footage...\n";

    auto preBoundary = g_triggerTime - std::chrono::minutes(PRE_TRIGGER_MINS);
    auto postBoundary = g_triggerTime + std::chrono::minutes(POST_TRIGGER_MINS);

    std::vector<fs::path> keptFiles;
    std::vector<fs::path> allSegments;

    for (const auto& entry : fs::directory_iterator(g_currentSessionDir)) {
        if (entry.path().extension() == ".mp4")
            allSegments.push_back(entry.path());
    }
    std::sort(allSegments.begin(), allSegments.end());

    {
        std::lock_guard<std::mutex> lock(g_segmentMutex);
        for (size_t i = 0; i < allSegments.size(); ++i) {
            std::chrono::system_clock::time_point segStart;
            if (i < g_segmentStartTimes.size()) {
                segStart = g_segmentStartTimes[i];
            }
            else {
                auto ftime = fs::last_write_time(allSegments[i]);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                segStart = sctp - std::chrono::seconds(SEGMENT_DURATION_SECONDS);
            }

            auto segEnd = segStart + std::chrono::seconds(SEGMENT_DURATION_SECONDS);
            if (segStart < postBoundary && segEnd > preBoundary) {
                keptFiles.push_back(allSegments[i]);
            }
            else {
                fs::remove(allSegments[i]);
            }
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

static DWORD WINAPI SegmentWatcherThread(LPVOID)
{
    size_t knownCount = 0;

    while (!g_shutdown) {
        if (g_isRecording && !g_currentSessionDir.empty()) {
            std::vector<fs::path> segments;
            std::error_code ec;
            for (const auto& entry :
                fs::directory_iterator(g_currentSessionDir, ec)) {
                if (entry.path().extension() == ".mp4")
                    segments.push_back(entry.path());
            }
            std::sort(segments.begin(), segments.end());

            if (segments.size() > knownCount) {
                auto now = std::chrono::system_clock::now();
                std::lock_guard<std::mutex> lock(g_segmentMutex);
                while (g_segmentStartTimes.size() < segments.size())
                    g_segmentStartTimes.push_back(now);
                knownCount = segments.size();
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
            StartSession();
        }
        else if (wParam == WTS_SESSION_UNLOCK) {
            g_isLocked = false;
            EndSession();
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI MessageLoopThread(LPVOID)
{
    g_hSessionWnd = CreateWindowExA(0, "SessionMonitorClass", "", 0,
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

static HWND g_hRawInputWnd = NULL;

static LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT && g_isLocked && g_isRecording && !g_isTriggered) {
        std::cout << "[Trigger] Physical activity detected on locked machine!\n";
        g_isTriggered = true;
        g_triggerTime = std::chrono::system_clock::now();
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
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
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, g_hInstance, NULL);

    if (!g_hRawInputWnd) {
        std::cerr << "[Error] CreateWindowExW failed (" << GetLastError() << ")\n";
        return 1;
    }

    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = 0x01;
    rid[0].usUsage = 0x02;
    rid[0].dwFlags = RIDEV_INPUTSINK;
    rid[0].hwndTarget = g_hRawInputWnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage = 0x06;
    rid[1].dwFlags = RIDEV_INPUTSINK;
    rid[1].hwndTarget = g_hRawInputWnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)))
        std::cerr << "[Warning] RegisterRawInputDevices failed (" << GetLastError() << ")\n";

    MSG msg;
    while (!g_shutdown && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hRawInputWnd) DestroyWindow(g_hRawInputWnd);
    return 0;
}

int main()
{
    g_hInstance = GetModuleHandle(NULL);
    fs::create_directories(BASE_DIR);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInstance;
    wc.lpszClassName = "SessionMonitorClass";
    RegisterClassExA(&wc);

    HANDLE hMsgLoop = CreateThread(NULL, 0, MessageLoopThread, NULL, 0, NULL);
    HANDLE hSegWatcher = CreateThread(NULL, 0, SegmentWatcherThread, NULL, 0, NULL);
    g_hMonitorThread = CreateThread(NULL, 0, MonitorThread, NULL, 0, NULL);

    std::cout << "=== Session Monitor Active ===\n";
    std::cout << "Waiting for laptop to be locked (Win+L)...\n";
    std::cout << "Press Ctrl+C to exit.\n";

    while (!g_shutdown) {
        Sleep(10000);
    }

    g_shutdown = true;
    if (hMsgLoop) { PostThreadMessage(GetThreadId(hMsgLoop), WM_QUIT, 0, 0); WaitForSingleObject(hMsgLoop, 3000); CloseHandle(hMsgLoop); }
    if (g_hMonitorThread) { PostMessage(g_hRawInputWnd, WM_QUIT, 0, 0);              WaitForSingleObject(g_hMonitorThread, 3000); CloseHandle(g_hMonitorThread); }
    if (hSegWatcher) { WaitForSingleObject(hSegWatcher, 3000); CloseHandle(hSegWatcher); }

    return 0;
}