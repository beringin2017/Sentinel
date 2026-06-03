#include "pch.h"

// Include order matters: winsock2.h before windows.h sub-headers,
// sspi.h before schannel.h. All three defines (WIN32_LEAN_AND_MEAN,
// NOMINMAX, SECURITY_WIN32) live in pch.h and are stamped before
// the PCH binary is built, so they apply to every header below.
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>           // getaddrinfo, freeaddrinfo
#pragma comment(lib, "ws2_32.lib")
#include <sspi.h>               // CredHandle, CtxtHandle, SecBuffer etc.
#include <schannel.h>           // Schannel TLS constants
#pragma comment(lib, "secur32.lib")
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")

#include <iostream>
#include <sstream>              // std::ostringstream
#include <string>
#include <cstdint>              // uintptr_t
#include <cstring>              // strlen
#include <ctime>                // localtime_s, strftime
#include <vector>
#include <iterator>             // std::istreambuf_iterator
#include <filesystem>
#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>            // std::sort, std::find
#include <mutex>
#include <condition_variable>
#include <queue>
#include <system_error>         // std::error_code

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config {
    std::string cameraName = "Integrated Camera";
    std::string audioName = "";
    int         segmentDurationS = 60;
    int         preTriggerMins = 5;
    int         postTriggerMins = 10;
    int         maxRecordingMins = 30;
    int         quickUnlockS = 3;
    std::string baseDir = "monitor_sessions\\";

    std::string armDays = "Mon,Tue,Wed,Thu,Fri";
    std::string armStart = "09:00";
    std::string armEnd = "18:00";

    std::string smtpHost = "";
    int         smtpPort = 465;
    std::string smtpUser = "";
    std::string smtpPass = "";
    std::string emailTo = "";
    int         maxAttachMB = 25;
};

static Config g_cfg;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string Trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

static std::string GetExeDir()
{
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p(buf);
    const auto sl = p.find_last_of("\\/");
    return (sl != std::string::npos) ? p.substr(0, sl + 1) : ".\\";
}

static std::tm LocalNow()
{
    auto t = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm = {};
    localtime_s(&tm, &t);
    return tm;
}

// ---------------------------------------------------------------------------
// Schedule check
// ---------------------------------------------------------------------------
static int ParseHHMM(const std::string& s)
{
    if (s.size() != 5 || s[2] != ':') return -1;
    try { return std::stoi(s.substr(0, 2)) * 60 + std::stoi(s.substr(3, 2)); }
    catch (...) { return -1; }
}

static const char* kDayAbbr[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

static bool IsArmed()
{
    const std::tm now = LocalNow();

    // FIX (Bug 5): check day and time independently.
    // An empty field means "no restriction on that axis", not "fully disarmed".
    if (!g_cfg.armDays.empty()) {
        const std::string abbr = kDayAbbr[now.tm_wday];
        if (g_cfg.armDays.find(abbr) == std::string::npos) return false;
    }

    const int startMin = ParseHHMM(g_cfg.armStart);
    const int endMin = ParseHHMM(g_cfg.armEnd);
    if (startMin >= 0 && endMin >= 0) {
        const int nowMin = now.tm_hour * 60 + now.tm_min;
        if (nowMin < startMin || nowMin >= endMin) return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Config I/O
// ---------------------------------------------------------------------------
static void WriteDefaultConfig(const std::string& path)
{
    std::ofstream f(path);
    if (!f) { std::cerr << "[Config] Cannot create sentinel.ini\n"; return; }

    f << "; Sentinel configuration\n"
        << "; To find device names: ffmpeg -list_devices true -f dshow -i dummy\n\n"

        << "[Recording]\n"
        << "; DirectShow video device name\n"
        << "CameraName=Integrated Camera\n"
        << "; DirectShow audio device name (leave blank to record video only)\n"
        << "AudioName=\n"
        << "PreTriggerMins=5\n"
        << "PostTriggerMins=10\n"
        << "MaxRecordingMins=30\n"
        << "; Unlock faster than this (seconds) with no trigger -> owner assumed\n"
        << "QuickUnlockSecs=3\n\n"

        << "[Schedule]\n"
        << "; Comma-separated day abbreviations: Mon,Tue,Wed,Thu,Fri,Sat,Sun\n"
        << "; Leave ArmDays blank to arm every day\n"
        << "ArmDays=Mon,Tue,Wed,Thu,Fri\n"
        << "; 24-hour HH:MM. Leave both blank to arm all day\n"
        << "ArmStart=09:00\n"
        << "ArmEnd=18:00\n\n"

        << "[Email]\n"
        << "; Gmail: SmtpHost=smtp.gmail.com, SmtpPort=465\n"
        << "; Use an App Password, not your Google account password\n"
        << "SmtpHost=smtp.gmail.com\n"
        << "SmtpPort=465\n"
        << "SmtpUser=\n"
        << "SmtpPass=\n"
        << "EmailTo=\n"
        << "; Skip footage attachment if file exceeds this size (MB)\n"
        << "MaxAttachMB=25\n";

    std::cout << "[Config] Created sentinel.ini with defaults. "
        << "Set CameraName and email credentials before use.\n";
}

static void LoadConfig(const std::string& path)
{
    if (!fs::exists(path))
        WriteDefaultConfig(path);

    std::ifstream f(path);
    if (!f) {
        std::cerr << "[Config] Cannot open sentinel.ini -> using built-in defaults.\n";
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '[') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = Trim(line.substr(0, eq));
        const auto val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        // FIX (Bug 6): guard all numeric stoi calls against empty values.
        if (key == "CameraName")       g_cfg.cameraName = val;
        else if (key == "AudioName")         g_cfg.audioName = val;
        else if (key == "PreTriggerMins" && !val.empty()) g_cfg.preTriggerMins = std::stoi(val);
        else if (key == "PostTriggerMins" && !val.empty()) g_cfg.postTriggerMins = std::stoi(val);
        else if (key == "MaxRecordingMins" && !val.empty()) g_cfg.maxRecordingMins = std::stoi(val);
        else if (key == "QuickUnlockSecs" && !val.empty()) g_cfg.quickUnlockS = std::stoi(val);
        else if (key == "ArmDays")           g_cfg.armDays = val;
        else if (key == "ArmStart")          g_cfg.armStart = val;
        else if (key == "ArmEnd")            g_cfg.armEnd = val;
        else if (key == "SmtpHost")          g_cfg.smtpHost = val;
        else if (key == "SmtpPort" && !val.empty()) g_cfg.smtpPort = std::stoi(val);
        else if (key == "SmtpUser")          g_cfg.smtpUser = val;
        else if (key == "SmtpPass")          g_cfg.smtpPass = val;
        else if (key == "EmailTo")           g_cfg.emailTo = val;
        else if (key == "MaxAttachMB" && !val.empty()) g_cfg.maxAttachMB = std::stoi(val);
    }

    const bool emailOk = !g_cfg.smtpHost.empty() && !g_cfg.smtpUser.empty()
        && !g_cfg.smtpPass.empty() && !g_cfg.emailTo.empty();

    std::cout << "[Config] sentinel.ini loaded\n"
        << "         Camera      : " << g_cfg.cameraName << "\n"
        << "         Audio       : "
        << (g_cfg.audioName.empty() ? "(none)" : g_cfg.audioName) << "\n"
        << "         Schedule    : " << g_cfg.armDays << "  "
        << g_cfg.armStart << " - " << g_cfg.armEnd << "\n"
        << "         Email       : "
        << (emailOk ? g_cfg.emailTo : "(disabled)") << "\n";

    if (!emailOk)
        std::cout << "\n[Email] Notifications are disabled.\n"
        << "        To enable, open sentinel.ini and fill in:\n"
        << "          SmtpUser  = your.address@gmail.com\n"
        << "          SmtpPass  = your-app-password\n"
        << "          EmailTo   = recipient@example.com\n"
        << "        Changes take effect immediately without restarting.\n"
        << "        Get a Gmail App Password at:\n"
        << "          myaccount.google.com -> Security -> App Passwords\n\n";
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------
static std::string Base64Encode(const unsigned char* data, size_t len)
{
    static const char* kT =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int v = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<unsigned int>(data[i + 2]);
        out += kT[(v >> 18) & 0x3F];
        out += kT[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? kT[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? kT[v & 0x3F] : '=';
    }
    return out;
}

static std::string Base64Encode(const std::string& s)
{
    return Base64Encode(
        reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
// TLS / Schannel
// ---------------------------------------------------------------------------
struct TlsConn {
    SOCKET                    sock = INVALID_SOCKET;
    CredHandle                hCred = {};
    CtxtHandle                hCtx = {};
    SecPkgContext_StreamSizes sizes = {};
    bool                      credInit = false;
    bool                      ctxInit = false;
    std::vector<char>         readBuf;
};

static bool TlsConnect(TlsConn& c, const std::string& host, int port)
{
    // FIX (Bug 2): WSAStartup is paired with WSACleanup in TlsClose so the
    // reference count stays balanced across multiple SendEmail calls.
    WSADATA wsd = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsd) != 0) {
        std::cerr << "[Email] WSAStartup failed.\n";
        return false;
    }

    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        WSACleanup();
        return false;
    }

    c.sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c.sock == INVALID_SOCKET) { freeaddrinfo(res); WSACleanup(); return false; }

    if (connect(c.sock, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        freeaddrinfo(res); WSACleanup(); return false;
    }
    freeaddrinfo(res);

    SCHANNEL_CRED sc = {};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;

    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        NULL, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
        NULL, &sc, NULL, NULL, &c.hCred, NULL);
    if (ss != SEC_E_OK) { WSACleanup(); return false; }
    c.credInit = true;

    std::vector<char> inBuf(16384, 0);
    int  inLen = 0;
    bool firstCall = true;

    while (true) {
        SecBuffer    outBuffers[1] = {};
        outBuffers[0].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc outDesc = { SECBUFFER_VERSION, 1, outBuffers };

        SecBuffer    inBuffers[2] = {};
        inBuffers[0].pvBuffer = inBuf.data();
        inBuffers[0].cbBuffer = static_cast<ULONG>(inLen);
        inBuffers[0].BufferType = SECBUFFER_TOKEN;
        inBuffers[1].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc inDesc = { SECBUFFER_VERSION, 2, inBuffers };

        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
            ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
            ISC_REQ_STREAM;

        ss = InitializeSecurityContextA(
            &c.hCred,
            firstCall ? NULL : &c.hCtx,
            firstCall ? const_cast<char*>(host.c_str()) : NULL,
            flags, 0, 0,
            firstCall ? NULL : &inDesc,
            0, firstCall ? &c.hCtx : NULL,
            &outDesc, &flags, NULL);
        firstCall = false;

        if (outBuffers[0].pvBuffer && outBuffers[0].cbBuffer > 0) {
            send(c.sock, static_cast<char*>(outBuffers[0].pvBuffer),
                static_cast<int>(outBuffers[0].cbBuffer), 0);
            FreeContextBuffer(outBuffers[0].pvBuffer);
        }

        if (ss == SEC_E_OK) {
            c.ctxInit = true;
            QueryContextAttributesA(&c.hCtx, SECPKG_ATTR_STREAM_SIZES, &c.sizes);
            return true;
        }
        if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_I_INCOMPLETE_CREDENTIALS) {
            int r = recv(c.sock, inBuf.data() + inLen,
                static_cast<int>(inBuf.size() - inLen), 0);
            if (r <= 0) return false;
            inLen += r;
            continue;
        }
        return false;
    }
}

static bool TlsSend(TlsConn& c, const std::string& data)
{
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t remaining = data.size() - offset;
        const size_t maxMsg = static_cast<size_t>(c.sizes.cbMaximumMessage);
        const size_t chunk = remaining < maxMsg ? remaining : maxMsg;

        std::vector<char> msg(
            static_cast<size_t>(c.sizes.cbHeader) + chunk +
            static_cast<size_t>(c.sizes.cbTrailer));

        memcpy(msg.data() + c.sizes.cbHeader, data.data() + offset, chunk);

        SecBuffer bufs[3] = {};
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;
        bufs[0].pvBuffer = msg.data();
        bufs[0].cbBuffer = c.sizes.cbHeader;
        bufs[1].BufferType = SECBUFFER_DATA;
        bufs[1].pvBuffer = msg.data() + c.sizes.cbHeader;
        bufs[1].cbBuffer = static_cast<ULONG>(chunk);
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;
        bufs[2].pvBuffer = msg.data() + c.sizes.cbHeader + chunk;
        bufs[2].cbBuffer = c.sizes.cbTrailer;

        SecBufferDesc desc = { SECBUFFER_VERSION, 3, bufs };
        if (EncryptMessage(&c.hCtx, 0, &desc, 0) != SEC_E_OK) return false;

        size_t total = static_cast<size_t>(bufs[0].cbBuffer)
            + static_cast<size_t>(bufs[1].cbBuffer)
            + static_cast<size_t>(bufs[2].cbBuffer);
        if (send(c.sock, msg.data(), static_cast<int>(total), 0) <= 0) return false;
        offset += chunk;
    }
    return true;
}

static std::string TlsReadLine(TlsConn& c)
{
    std::vector<char> wire(
        static_cast<size_t>(c.sizes.cbHeader) +
        static_cast<size_t>(c.sizes.cbMaximumMessage) +
        static_cast<size_t>(c.sizes.cbTrailer));

    while (true) {
        auto it = std::find(c.readBuf.begin(), c.readBuf.end(), '\n');
        if (it != c.readBuf.end()) {
            std::string line(c.readBuf.begin(), it + 1);
            c.readBuf.erase(c.readBuf.begin(), it + 1);
            return line;
        }

        int r = recv(c.sock, wire.data(), static_cast<int>(wire.size()), 0);
        if (r <= 0) return {};

        SecBuffer bufs[4] = {};
        bufs[0].pvBuffer = wire.data();
        bufs[0].cbBuffer = static_cast<ULONG>(r);
        bufs[0].BufferType = SECBUFFER_DATA;
        bufs[1].BufferType = SECBUFFER_EMPTY;
        bufs[2].BufferType = SECBUFFER_EMPTY;
        bufs[3].BufferType = SECBUFFER_EMPTY;
        SecBufferDesc desc = { SECBUFFER_VERSION, 4, bufs };

        SECURITY_STATUS ss = DecryptMessage(&c.hCtx, &desc, 0, NULL);
        if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) return {};

        for (int i = 0; i < 4; ++i)
            if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0)
                c.readBuf.insert(c.readBuf.end(),
                    static_cast<char*>(bufs[i].pvBuffer),
                    static_cast<char*>(bufs[i].pvBuffer) + bufs[i].cbBuffer);
    }
}

static void TlsClose(TlsConn& c)
{
    if (c.ctxInit)  DeleteSecurityContext(&c.hCtx);
    if (c.credInit) FreeCredentialsHandle(&c.hCred);
    if (c.sock != INVALID_SOCKET) { closesocket(c.sock); c.sock = INVALID_SOCKET; }
    // FIX (Bug 2): balance each WSAStartup in TlsConnect with a WSACleanup here.
    WSACleanup();
}

static std::string SmtpCmd(TlsConn& c, const std::string& cmd)
{
    TlsSend(c, cmd + "\r\n");
    return TlsReadLine(c);
}

// ---------------------------------------------------------------------------
// Email
// ---------------------------------------------------------------------------
static bool SendEmail(const std::string& subject,
    const std::string& body,
    const std::string& attachPath = "")
{
    if (g_cfg.smtpHost.empty() || g_cfg.smtpUser.empty()
        || g_cfg.smtpPass.empty() || g_cfg.emailTo.empty()) {
        std::cout << "[Email] Not configured -> skipping notification.\n";
        return false;
    }

    TlsConn c;
    if (!TlsConnect(c, g_cfg.smtpHost, g_cfg.smtpPort)) {
        std::cerr << "[Email] TLS connect failed.\n";
        TlsClose(c);
        return false;
    }

    TlsReadLine(c);     // server greeting

    auto resp = SmtpCmd(c, "EHLO sentinel");
    while (!resp.empty() && resp.size() > 3 && resp[3] == '-')
        resp = TlsReadLine(c);

    SmtpCmd(c, "AUTH LOGIN");
    SmtpCmd(c, Base64Encode(g_cfg.smtpUser));
    resp = SmtpCmd(c, Base64Encode(g_cfg.smtpPass));
    if (resp.substr(0, 3) != "235") {
        std::cerr << "[Email] Authentication failed: " << resp << "\n";
        TlsClose(c);
        return false;
    }

    SmtpCmd(c, "MAIL FROM:<" + g_cfg.smtpUser + ">");
    SmtpCmd(c, "RCPT TO:<" + g_cfg.emailTo + ">");
    SmtpCmd(c, "DATA");

    const std::string boundary = "sentinel_boundary_xK9mP2";
    std::ostringstream msg;
    auto now = LocalNow();
    char dateStr[64] = {};
    std::strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y %H:%M:%S +0000", &now);

    msg << "From: Sentinel <" << g_cfg.smtpUser << ">\r\n"
        << "To: " << g_cfg.emailTo << "\r\n"
        << "Subject: " << subject << "\r\n"
        << "Date: " << dateStr << "\r\n"
        << "MIME-Version: 1.0\r\n"
        << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\r\n\r\n"
        << "--" << boundary << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        << body << "\r\n";

    bool attached = false;
    if (!attachPath.empty() && fs::exists(attachPath)) {
        const auto fileSizeMB = static_cast<int>(
            fs::file_size(attachPath) / (1024 * 1024));
        if (fileSizeMB <= g_cfg.maxAttachMB) {
            std::ifstream af(attachPath, std::ios::binary);
            if (af) {
                std::vector<unsigned char> fileData(
                    (std::istreambuf_iterator<char>(af)),
                    std::istreambuf_iterator<char>());
                msg << "--" << boundary << "\r\n"
                    << "Content-Type: video/mp4\r\n"
                    << "Content-Transfer-Encoding: base64\r\n"
                    << "Content-Disposition: attachment; "
                    "filename=\"incident.mp4\"\r\n\r\n"
                    << Base64Encode(fileData.data(), fileData.size()) << "\r\n";
                attached = true;
            }
        }
        else {
            std::cout << "[Email] Attachment too large (" << fileSizeMB
                << " MB > " << g_cfg.maxAttachMB
                << " MB limit) -> sending text-only.\n";
        }
    }

    msg << "--" << boundary << "--\r\n.\r\n";
    TlsSend(c, msg.str());
    resp = TlsReadLine(c);
    SmtpCmd(c, "QUIT");
    TlsClose(c);

    if (resp.substr(0, 3) == "250") {
        std::cout << "[Email] Sent"
            << (attached ? " with footage attached" : "") << ".\n";
        return true;
    }
    std::cerr << "[Email] Server rejected message: " << resp << "\n";
    return false;
}

// ---------------------------------------------------------------------------
// Session command queue
// ---------------------------------------------------------------------------
enum class SessionCmd { Lock, Unlock };

static std::queue<SessionCmd>  g_cmdQueue;
static std::mutex              g_cmdMutex;
static std::condition_variable g_cmdCv;

static void PostSessionCmd(SessionCmd cmd)
{
    { std::lock_guard<std::mutex> lk(g_cmdMutex); g_cmdQueue.push(cmd); }
    g_cmdCv.notify_one();
}

// ---------------------------------------------------------------------------
// Global State
// ---------------------------------------------------------------------------
std::atomic<bool> g_isLocked{ false };
std::atomic<bool> g_isRecording{ false };
std::atomic<bool> g_isTriggered{ false };
std::atomic<bool> g_shutdown{ false };

std::chrono::system_clock::time_point g_triggerTime;
std::chrono::system_clock::time_point g_lockTime;

// FIX (Bug 4): g_currentSessionDir is written by SessionWorkerThread and
// read by SegmentWatcherThread. Protected by g_sessionDirMutex.
std::mutex  g_sessionDirMutex;
std::string g_currentSessionDir;

std::mutex g_segmentMutex;
std::vector<std::chrono::system_clock::time_point> g_segmentStartTimes;

HANDLE    g_hFFmpegProcess = NULL;
HANDLE    g_hStdInWrite = NULL;
HANDLE    g_hMonitorThread = NULL;
HINSTANCE g_hInstance = NULL;
HWND      g_hSessionWnd = NULL;
HWND      g_hRawInputWnd = NULL;

// ---------------------------------------------------------------------------
// FFmpeg
// ---------------------------------------------------------------------------
static bool LaunchProcess(const std::string& cmd,
    HANDLE* outProcess,
    HANDLE hStdIn = NULL,
    HANDLE hStdOut = NULL,
    HANDLE hStdErr = NULL)
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
        NULL, NULL, &si, &pi)) {
        std::cerr << "[Error] CreateProcess failed: " << cmd
            << " (" << GetLastError() << ")\n";
        return false;
    }
    if (outProcess) *outProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}

static void StartFFmpegRecording(const std::string& outputDir)
{
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdInRead = NULL;
    if (!CreatePipe(&hStdInRead, &g_hStdInWrite, &sa, 0)) {
        std::cerr << "[Error] CreatePipe failed (" << GetLastError() << ")\n";
        return;
    }
    SetHandleInformation(g_hStdInWrite, HANDLE_FLAG_INHERIT, 0);

    std::string inputs = "-f dshow -i video=\"" + g_cfg.cameraName + "\"";
    std::string maps;
    if (!g_cfg.audioName.empty()) {
        inputs += " -f dshow -i audio=\"" + g_cfg.audioName + "\"";
        maps = " -map 0:v -map 1:a";
    }

    std::string cmd =
        "ffmpeg.exe " + inputs + maps +
        " -f segment -segment_time " + std::to_string(g_cfg.segmentDurationS) +
        " -segment_format mp4 -reset_timestamps 1"
        " \"" + outputDir + "\\seg_%03d.mp4\"";

    if (!LaunchProcess(cmd, &g_hFFmpegProcess,
        hStdInRead,
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE))) {
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
    const char* q = "q\n";
    DWORD       written = 0;
    WriteFile(g_hStdInWrite, q, static_cast<DWORD>(strlen(q)), &written, NULL);
    if (WaitForSingleObject(g_hFFmpegProcess, 10000) == WAIT_TIMEOUT) {
        std::cerr << "[Warning] FFmpeg did not exit in 10 s; terminating.\n";
        TerminateProcess(g_hFFmpegProcess, 1);
    }
    CloseHandle(g_hFFmpegProcess);
    CloseHandle(g_hStdInWrite);
    g_hFFmpegProcess = NULL;
    g_hStdInWrite = NULL;
}

// ---------------------------------------------------------------------------
// Session Management  (runs on SessionWorkerThread)
// ---------------------------------------------------------------------------
static void StartSession()
{
    if (g_isRecording) return;

    if (!IsArmed()) {
        std::cout << "[Session] Outside schedule -> not arming.\n";
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm localTime = {};
    localtime_s(&localTime, &t);
    char timeStr[64] = {};
    std::strftime(timeStr, sizeof(timeStr), "%Y%m%d_%H%M%S", &localTime);

    {
        std::lock_guard<std::mutex> lk(g_sessionDirMutex);
        g_currentSessionDir = g_cfg.baseDir + "session_" + timeStr;
        fs::create_directories(g_currentSessionDir);
    }
    { std::lock_guard<std::mutex> lk(g_segmentMutex); g_segmentStartTimes.clear(); }

    g_isTriggered = false;
    g_isRecording = true;
    g_lockTime = std::chrono::system_clock::now();

    std::string sessionDir;
    { std::lock_guard<std::mutex> lk(g_sessionDirMutex); sessionDir = g_currentSessionDir; }
    std::cout << "[Session] Laptop locked. Recording to " << sessionDir << "\n";
    StartFFmpegRecording(sessionDir);
}

static void EndSession()
{
    if (!g_isRecording) return;

    std::cout << "[Session] Laptop unlocked. Stopping recording.\n";
    StopFFmpegRecording();
    g_isRecording = false;

    const auto lockedSecs = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - g_lockTime).count();

    std::string sessionDir;
    { std::lock_guard<std::mutex> lk(g_sessionDirMutex); sessionDir = g_currentSessionDir; }

    if (!g_isTriggered) {
        if (lockedSecs <= g_cfg.quickUnlockS)
            std::cout << "[Session] Quick unlock (" << lockedSecs
            << " s) -> owner. Deleting footage.\n";
        else
            std::cout << "[Session] No activity detected. Deleting footage.\n";
        fs::remove_all(sessionDir);
        return;
    }

    std::cout << "[Session] Activity detected. Trimming and merging footage...\n";

    const auto preBoundary = g_triggerTime - std::chrono::minutes(g_cfg.preTriggerMins);
    const auto postBoundary = g_triggerTime + std::chrono::minutes(g_cfg.postTriggerMins);

    std::vector<fs::path> allSegments, keptFiles;
    for (const auto& e : fs::directory_iterator(sessionDir))
        if (e.path().extension() == ".mp4")
            allSegments.push_back(e.path());
    std::sort(allSegments.begin(), allSegments.end());

    {
        std::lock_guard<std::mutex> lk(g_segmentMutex);
        for (size_t i = 0; i < allSegments.size(); ++i) {
            std::chrono::system_clock::time_point segStart;
            if (i < g_segmentStartTimes.size() &&
                g_segmentStartTimes[i] != std::chrono::system_clock::time_point{}) {
                segStart = g_segmentStartTimes[i];
            }
            else {
                auto ft = fs::last_write_time(allSegments[i]);
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                segStart = sctp - std::chrono::seconds(g_cfg.segmentDurationS);
            }
            const auto segEnd = segStart + std::chrono::seconds(g_cfg.segmentDurationS);
            if (segStart < postBoundary && segEnd > preBoundary)
                keptFiles.push_back(allSegments[i]);
            else
                fs::remove(allSegments[i]);
        }
    }

    if (keptFiles.empty()) {
        std::cout << "[Error] No relevant segments found.\n";
        return;
    }

    const std::string concatPath = sessionDir + "\\concat.txt";
    {
        std::ofstream lf(concatPath);
        if (!lf) { std::cerr << "[Error] Could not write concat list.\n"; return; }
        for (const auto& f : keptFiles) lf << "file '" << f.filename().string() << "'\n";
    }

    const std::string finalVideo = sessionDir + "\\incident.mp4";
    std::string mergeCmd =
        "ffmpeg.exe -y -f concat -safe 0 -i \"" + concatPath +
        "\" -c copy \"" + finalVideo + "\"";

    HANDLE hMerge = NULL;
    std::cout << "[Processing] Merging " << keptFiles.size() << " segments...\n";
    if (LaunchProcess(mergeCmd, &hMerge)) {
        WaitForSingleObject(hMerge, 30000);
        CloseHandle(hMerge);
    }
    for (const auto& f : keptFiles) fs::remove(f);
    fs::remove(concatPath);
    std::cout << "[Success] Incident saved to: " << finalVideo << "\n";

    SendEmail(
        "Sentinel: Incident footage ready",
        "The incident recording has been processed and is attached.\n\n"
        "File: " + finalVideo + "\n",
        finalVideo);
}

// ---------------------------------------------------------------------------
// Session Worker Thread
// ---------------------------------------------------------------------------
static DWORD WINAPI SessionWorkerThread(LPVOID)
{
    while (!g_shutdown) {
        std::unique_lock<std::mutex> lk(g_cmdMutex);
        g_cmdCv.wait(lk, [] { return !g_cmdQueue.empty() || g_shutdown.load(); });
        while (!g_cmdQueue.empty()) {
            SessionCmd cmd = g_cmdQueue.front(); g_cmdQueue.pop();
            lk.unlock();
            if (cmd == SessionCmd::Lock)   StartSession();
            else if (cmd == SessionCmd::Unlock) EndSession();
            lk.lock();
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Segment Watcher Thread
// ---------------------------------------------------------------------------
static DWORD WINAPI SegmentWatcherThread(LPVOID)
{
    size_t knownCount = 0;
    while (!g_shutdown) {
        std::string sessionDir;
        {
            std::lock_guard<std::mutex> lk(g_sessionDirMutex);
            sessionDir = g_currentSessionDir;
        }

        if (g_isRecording && !sessionDir.empty()) {
            std::vector<fs::path> segments;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(sessionDir, ec))
                if (e.path().extension() == ".mp4")
                    segments.push_back(e.path());
            std::sort(segments.begin(), segments.end());

            if (segments.size() > knownCount) {
                auto now = std::chrono::system_clock::now();
                std::lock_guard<std::mutex> lk(g_segmentMutex);
                while (g_segmentStartTimes.size() < segments.size())
                    g_segmentStartTimes.push_back(now);
                knownCount = segments.size();
            }

            if (!g_isTriggered) {
                auto cutoff = std::chrono::system_clock::now()
                    - std::chrono::minutes(g_cfg.maxRecordingMins);
                std::lock_guard<std::mutex> lk(g_segmentMutex);
                for (size_t i = 0; i < segments.size(); ++i) {
                    if (i < g_segmentStartTimes.size()
                        && g_segmentStartTimes[i] != std::chrono::system_clock::time_point{}
                        && g_segmentStartTimes[i] < cutoff) {
                        std::error_code re;
                        fs::remove(segments[i], re);
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

// ---------------------------------------------------------------------------
// Window Procedures
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_WTSSESSION_CHANGE) {
        if (wParam == WTS_SESSION_LOCK) { g_isLocked = true;  PostSessionCmd(SessionCmd::Lock); }
        else if (wParam == WTS_SESSION_UNLOCK) { g_isLocked = false; PostSessionCmd(SessionCmd::Unlock); }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT && g_isLocked && g_isRecording && !g_isTriggered) {
        std::cout << "[Trigger] Physical activity detected on locked machine!\n";
        g_isTriggered = true;
        g_triggerTime = std::chrono::system_clock::now();

        // Fire alert email on a detached thread so the raw-input loop never blocks.
        // FIX (Bug 3): close the thread handle immediately — we don't join it.
        if (!g_cfg.smtpHost.empty() && !g_cfg.smtpUser.empty() && !g_cfg.emailTo.empty()) {
            HANDLE h = CreateThread(NULL, 0, [](LPVOID) -> DWORD {
                SendEmail(
                    "Sentinel: Activity detected on locked machine",
                    "Physical interaction was detected on your locked machine.\n"
                    "Sentinel is recording. You will receive the footage once "
                    "the session ends.\n");
                return 0;
                }, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Message Loop Thread
// ---------------------------------------------------------------------------
static DWORD WINAPI MessageLoopThread(LPVOID lpParam)
{
    ATOM sessionClass = static_cast<ATOM>(reinterpret_cast<uintptr_t>(lpParam));
    g_hSessionWnd = CreateWindowExW(0, MAKEINTATOM(sessionClass), L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInstance, NULL);
    if (!g_hSessionWnd) {
        std::cerr << "[Error] Failed to create session window ("
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

// ---------------------------------------------------------------------------
// Monitor Thread  (raw input)
// ---------------------------------------------------------------------------
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
        MAKEINTATOM(atom), L"", WS_POPUP,
        0, 0, 1, 1, NULL, NULL, g_hInstance, NULL);
    if (!g_hRawInputWnd) {
        std::cerr << "[Error] CreateWindowExW failed (" << GetLastError() << ")\n";
        return 1;
    }

    RAWINPUTDEVICE rid[2] = {};
    rid[0] = { 0x01, 0x02, RIDEV_INPUTSINK, g_hRawInputWnd }; // Mouse
    rid[1] = { 0x01, 0x06, RIDEV_INPUTSINK, g_hRawInputWnd }; // Keyboard
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

// ---------------------------------------------------------------------------
// Config Watcher Thread
// ---------------------------------------------------------------------------
static std::string g_iniPath;

static DWORD WINAPI ConfigWatcherThread(LPVOID)
{
    const std::string watchDir = GetExeDir();
    HANDLE hDir = CreateFileA(
        watchDir.c_str(), FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hDir == INVALID_HANDLE_VALUE) {
        std::cerr << "[Config] Watcher could not open directory. "
            "Live reload disabled.\n";
        return 1;
    }

    alignas(DWORD) char buf[2048] = {};
    DWORD bytesReturned = 0;

    while (!g_shutdown) {
        BOOL ok = ReadDirectoryChangesW(
            hDir, buf, sizeof(buf), FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE,
            &bytesReturned, NULL, NULL);
        if (!ok || g_shutdown) break;
        if (bytesReturned == 0) continue;

        const auto* rec = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf);
        bool changed = false;
        for (;;) {
            char narrow[MAX_PATH] = {};
            WideCharToMultiByte(CP_ACP, 0,
                rec->FileName,
                static_cast<int>(rec->FileNameLength / sizeof(WCHAR)),
                narrow, MAX_PATH, NULL, NULL);
            if (_stricmp(narrow, "sentinel.ini") == 0) changed = true;
            if (rec->NextEntryOffset == 0) break;
            rec = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<const char*>(rec) + rec->NextEntryOffset);
        }
        if (!changed) continue;

        if (g_isRecording) {
            std::cout << "[Config] sentinel.ini changed -> "
                "will reload after session ends.\n";
            while (g_isRecording && !g_shutdown) Sleep(500);
            if (g_shutdown) break;
        }
        std::cout << "[Config] sentinel.ini changed -> reloading...\n";
        LoadConfig(g_iniPath);
    }
    CloseHandle(hDir);
    return 0;
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
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

    std::cout << "=== Sentinel Active ===\n"
        << "Waiting for laptop to be locked (Win+L)...\n"
        << "Press Ctrl+C to exit.\n";

    while (!g_shutdown) Sleep(10000);

    g_shutdown = true;
    g_cmdCv.notify_all();

    if (hMsgLoop) {
        PostThreadMessage(GetThreadId(hMsgLoop), WM_QUIT, 0, 0);
        WaitForSingleObject(hMsgLoop, 3000);
        CloseHandle(hMsgLoop);
    }
    if (g_hMonitorThread) {
        PostMessage(g_hRawInputWnd, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hMonitorThread, 3000);
        CloseHandle(g_hMonitorThread);
    }
    if (hSegWatcher) { WaitForSingleObject(hSegWatcher, 3000); CloseHandle(hSegWatcher); }
    if (hWorker) { WaitForSingleObject(hWorker, 3000); CloseHandle(hWorker); }
    if (hCfgWatcher) { WaitForSingleObject(hCfgWatcher, 3000); CloseHandle(hCfgWatcher); }

    return 0;
}