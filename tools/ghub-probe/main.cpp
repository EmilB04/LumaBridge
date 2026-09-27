// READ-ONLY probe of Logitech G HUB's local connection. Question it answers: does G HUB tell
// local programs what lighting games send it (so LumaBridge could mirror e.g. Battlefield 1
// from outside the game, where anti-cheat has nothing to object to)?
//
//   ghub-probe [seconds=90] [extra/path ...]
//
// What it does, all read-only:
//   1. lists G HUB's processes, the TCP ports they listen on and their named pipes;
//   2. connects to each listening port as a WebSocket client (sub-protocol "json", as G HUB's
//      own window does) and logs everything G HUB sends;
//   3. sends only "GET" and "SUBSCRIBE" requests (never "SET"), for device / lighting paths
//      plus any extra paths given on the command line;
//   4. keeps listening for `seconds` - play and take damage meanwhile - and logs every message
//      with a timestamp.
// Output: the console and %LOCALAPPDATA%\LumaBridge\ghub-probe.txt.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

FILE* g_out = nullptr;
std::atomic<int> g_messages{0};

void Log(const char* fmt, ...) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    char buf[8192];
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    printf("%s%s\n", stamp, buf);
    if (g_out) {
        fprintf(g_out, "%s%s\n", stamp, buf);
        fflush(g_out);
    }
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IsGHubProcess(const std::wstring& exe) {
    const std::wstring n = Lower(exe);
    return n.find(L"lghub") != std::wstring::npos || n.find(L"logi") != std::wstring::npos;
}

std::map<DWORD, std::wstring> GHubProcesses() {
    std::map<DWORD, std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
        if (IsGHubProcess(pe.szExeFile)) out[pe.th32ProcessID] = pe.szExeFile;
    CloseHandle(snap);
    return out;
}

// Loopback / any-address TCP listeners owned by `pids`.
std::set<unsigned> ListeningPorts(const std::map<DWORD, std::wstring>& pids) {
    std::set<unsigned> ports;
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<BYTE> buf(size + 4096);
    size = static_cast<DWORD>(buf.size());
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) == NO_ERROR) {
        auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& row = table->table[i];
            auto it = pids.find(row.dwOwningPid);
            if (it == pids.end()) continue;
            const unsigned port = ntohs(static_cast<u_short>(row.dwLocalPort));
            const BYTE* ip = reinterpret_cast<const BYTE*>(&row.dwLocalAddr);
            Log("  %s (pid %lu) listens on %u.%u.%u.%u:%u", Narrow(it->second).c_str(), row.dwOwningPid, ip[0], ip[1],
                ip[2], ip[3], port);
            ports.insert(port);
        }
    }
    return ports;
}

void ListPipes() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"\\\\.\\pipe\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = Lower(fd.cFileName);
        if (n.find(L"lghub") != std::wstring::npos || n.find(L"logi") != std::wstring::npos ||
            n.find(L"lgs") != std::wstring::npos)
            Log("  pipe: %s", Narrow(fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

struct Socket {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr, ws = nullptr;
    unsigned port = 0;
    ~Socket() {
        if (ws) {
            WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
        }
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

bool Open(Socket* s, unsigned port, const wchar_t* protocol) {
    s->port = port;
    s->session = WinHttpOpen(L"LumaBridge-ghub-probe", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!s->session) return false;
    WinHttpSetTimeouts(s->session, 2000, 2000, 2000, 2000);
    s->connect = WinHttpConnect(s->session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!s->connect) return false;
    s->request = WinHttpOpenRequest(s->connect, L"GET", L"/", nullptr, nullptr, nullptr, 0);
    if (!s->request) return false;
    if (!WinHttpSetOption(s->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) return false;
    std::wstring headers = L"Origin: file://\r\n";
    if (protocol) headers += std::wstring(L"Sec-WebSocket-Protocol: ") + protocol + L"\r\n";
    if (!WinHttpSendRequest(s->request, headers.c_str(), static_cast<DWORD>(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(s->request, nullptr)) {
        Log("  port %u: no HTTP answer (error %lu)", port, GetLastError());
        return false;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(s->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                        nullptr);
    s->ws = WinHttpWebSocketCompleteUpgrade(s->request, 0);
    if (!s->ws) {
        Log("  port %u: HTTP %lu, not a WebSocket (protocol %s)", port, status, protocol ? Narrow(protocol).c_str() : "none");
        return false;
    }
    WinHttpCloseHandle(s->request);
    s->request = nullptr;
    Log("  port %u: WebSocket connected (protocol %s)", port, protocol ? Narrow(protocol).c_str() : "none");
    return true;
}

void Send(Socket* s, const std::string& text) {
    const DWORD rc = WinHttpWebSocketSend(s->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                          const_cast<char*>(text.data()), static_cast<DWORD>(text.size()));
    Log("port %u >> %s%s", s->port, text.c_str(), rc == NO_ERROR ? "" : "  (send failed)");
}

void ReceiveLoop(Socket* s, std::atomic<bool>* stop) {
    std::string message;
    std::vector<char> buf(64 * 1024);
    while (!*stop) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        const DWORD rc = WinHttpWebSocketReceive(s->ws, buf.data(), static_cast<DWORD>(buf.size()), &got, &type);
        if (rc != NO_ERROR) {
            if (!*stop) Log("port %u: connection ended (error %lu)", s->port, rc);
            return;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            Log("port %u: G HUB closed the connection", s->port);
            return;
        }
        message.append(buf.data(), got);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;
        ++g_messages;
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            Log("port %u << (binary, %zu bytes)", s->port, message.size());
        else
            Log("port %u << %s", s->port, message.c_str());
        message.clear();
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 90;
    if (seconds <= 0) seconds = 90;
    std::vector<std::string> extraPaths;
    for (int i = 2; i < argc; ++i) extraPaths.push_back(Narrow(argv[i]));

    wchar_t appData[MAX_PATH];
    std::wstring outPath = L"ghub-probe.txt";
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appData, MAX_PATH)) {
        std::wstring dir = std::wstring(appData) + L"\\LumaBridge";
        CreateDirectoryW(dir.c_str(), nullptr);
        outPath = dir + L"\\ghub-probe.txt";
    }
    g_out = _wfopen(outPath.c_str(), L"w");

    Log("LumaBridge G HUB probe (read-only: GET / SUBSCRIBE only)");
    Log("G HUB processes:");
    const auto procs = GHubProcesses();
    for (const auto& [pid, exe] : procs) Log("  %s (pid %lu)", Narrow(exe).c_str(), pid);
    if (procs.empty()) Log("  none running - start G HUB first");
    Log("Listening TCP ports:");
    std::set<unsigned> ports = ListeningPorts(procs);
    ports.insert(9010);  // where G HUB's own window is known to connect
    Log("Named pipes:");
    ListPipes();

    Log("Connecting:");
    std::vector<std::unique_ptr<Socket>> sockets;
    for (unsigned port : ports) {
        for (const wchar_t* protocol : {L"json", static_cast<const wchar_t*>(nullptr)}) {
            auto s = std::make_unique<Socket>();
            if (Open(s.get(), port, protocol)) {
                sockets.push_back(std::move(s));
                break;
            }
        }
    }
    if (sockets.empty()) {
        Log("No WebSocket found on G HUB's ports. Output saved to %s", Narrow(outPath).c_str());
        if (g_out) fclose(g_out);
        return 1;
    }

    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (auto& s : sockets) readers.emplace_back(ReceiveLoop, s.get(), &stop);

    // Read-only requests. Paths are guesses based on what G HUB's own UI asks for; unknown
    // ones just get an error reply, which is useful to see too.
    const char* gets[] = {"/devices/list", "/devices/state", "/lighting/state", "/lighting/current",
                          "/applications", "/applications/list", "/sdk/state", "/games/state",
                          "/profiles/active", "/updates/status"};
    const char* subs[] = {"/devices/state/changed", "/lighting/state/changed", "/lighting/changed",
                          "/sdk/state/changed", "/applications/changed", "/profiles/active/changed",
                          "/battery/state/changed", "/games/state/changed"};
    int id = 0;
    for (auto& s : sockets) {
        for (const char* p : subs)
            Send(s.get(), "{\"msgId\":\"probe-" + std::to_string(++id) + "\",\"verb\":\"SUBSCRIBE\",\"path\":\"" + p + "\"}");
        for (const char* p : gets)
            Send(s.get(), "{\"msgId\":\"probe-" + std::to_string(++id) + "\",\"verb\":\"GET\",\"path\":\"" + p + "\"}");
        for (const auto& p : extraPaths) {
            Send(s.get(), "{\"msgId\":\"probe-" + std::to_string(++id) + "\",\"verb\":\"SUBSCRIBE\",\"path\":\"" + p + "\"}");
            Send(s.get(), "{\"msgId\":\"probe-" + std::to_string(++id) + "\",\"verb\":\"GET\",\"path\":\"" + p + "\"}");
        }
    }

    Log("Listening for %d s. Play now: let the game change your mouse's color (e.g. lose health).", seconds);
    for (int left = seconds; left > 0; --left) {
        Sleep(1000);
        if (left % 15 == 0) Log("... %d s left, %d message(s) so far", left, g_messages.load());
    }
    stop = true;
    sockets.clear();  // closes the sockets, which ends the blocking receives
    for (auto& t : readers) t.join();
    Log("Done: %d message(s). Output saved to %s", g_messages.load(), Narrow(outPath).c_str());
    if (g_out) fclose(g_out);
    return 0;
}
