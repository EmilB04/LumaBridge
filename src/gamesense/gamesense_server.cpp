#include "gamesense_server.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "http_parser.h"
#include "log.h"
#include "module_bootstrap.h"

namespace luma::gamesense {
namespace {

constexpr DWORD kRecvTimeoutMs = 30000;

std::wstring DefaultCorePropsPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"PROGRAMDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf) + L"\\SteelSeries\\SteelSeries Engine 3\\coreProps.json";
}

void CreateParentDirs(const std::wstring& file) {
    for (size_t pos = file.find(L'\\', 3); pos != std::wstring::npos; pos = file.find(L'\\', pos + 1))
        CreateDirectoryW(file.substr(0, pos).c_str(), nullptr);
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

bool Server::Start(const Config& cfg, std::function<void()> onActivity) {
    if (running_) return true;
    onActivity_ = std::move(onActivity);
    {
        std::lock_guard<std::mutex> lock(engineMutex_);
        engine_ = Engine(cfg.bitmapReduce);
    }
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    wsaStarted_ = true;
    if (!Listen(cfg.gameSensePort)) {
        LUMA_ERROR("GameSense: could not open a listening socket");
        Stop();
        return false;
    }
    corePropsOk_ = WriteCoreProps(cfg.gameSenseCoreProps);
    stop_ = false;
    running_ = true;
    acceptThread_ = std::thread(&Server::AcceptLoop, this);
    return true;
}

void Server::Stop() {
    if (running_) {
        stop_ = true;
        closesocket(listen_);  // unblocks accept()
        listen_ = INVALID_SOCKET;
        if (acceptThread_.joinable()) acceptThread_.join();
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            for (SOCKET c : clients_) shutdown(c, SD_BOTH);  // Serve() threads then exit
        }
        RestoreCoreProps();
        running_ = false;
        LUMA_INFO("GameSense: server stopped");
    } else if (listen_ != INVALID_SOCKET) {
        closesocket(listen_);
        listen_ = INVALID_SOCKET;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}

Server::Snapshot Server::Poll(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    engine_.Tick(nowMs);
    return Snapshot{engine_.AnyActive(), engine_.Version(), engine_.Current()};
}

bool Server::Listen(int preferredPort) {
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_ == INVALID_SOCKET) return false;
    BOOL exclusive = TRUE;
    setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof exclusive);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from the network
    addr.sin_port = htons(static_cast<u_short>(preferredPort));
    if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        LUMA_WARN("GameSense: port %d unavailable (error %d), using a free one", preferredPort,
                  WSAGetLastError());
        addr.sin_port = 0;
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return false;
    }
    if (listen(listen_, SOMAXCONN) != 0) return false;
    int len = sizeof addr;
    getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    LUMA_INFO("GameSense: listening on 127.0.0.1:%d", port_);
    return true;
}

void Server::AcceptLoop() {
    while (!stop_) {
        SOCKET c = accept(listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (stop_) break;
            Sleep(50);
            continue;
        }
        // One thread per connection: games keep a connection alive for the whole session,
        // and there are only ever a handful.
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.push_back(c);
        }
        std::thread(&Server::Serve, this, c).detach();
    }
}

void Server::Serve(SOCKET s) {
    DWORD timeout = kRecvTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);

    http::Parser parser;
    char buf[8192];
    bool open = true;
    while (open && !stop_) {
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        parser.Feed(buf, static_cast<size_t>(n));

        http::Request req;
        http::Parser::Status st;
        while ((st = parser.Next(&req)) == http::Parser::Status::Done) {
            Engine::Response resp;
            if (req.method == "POST") {
                std::lock_guard<std::mutex> lock(engineMutex_);
                resp = engine_.Handle(req.path, req.body, GetTickCount64());
            } else if (req.method != "GET") {
                resp = {404, "{\"error\":\"unsupported method\"}"};
            }
            LUMA_DEBUG("GameSense: %s %s -> %d", req.method.c_str(), req.path.c_str(), resp.status);
            if (resp.status != 200)
                LUMA_WARN("GameSense: %s -> %d %s", req.path.c_str(), resp.status, resp.body.c_str());
            if (onActivity_) onActivity_();

            std::string out = http::BuildResponse(resp.status, resp.body, req.keepAlive);
            send(s, out.data(), static_cast<int>(out.size()), 0);
            if (!req.keepAlive) {
                open = false;
                break;
            }
        }
        if (st == http::Parser::Status::Error) {
            std::string out = http::BuildResponse(400, "{\"error\":\"bad request\"}", false);
            send(s, out.data(), static_cast<int>(out.size()), 0);
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
    }
    closesocket(s);
}

bool Server::WriteCoreProps(const std::wstring& overridePath) {
    corePropsPath_ = overridePath.empty() ? DefaultCorePropsPath() : overridePath;
    if (corePropsPath_.empty()) return false;
    CreateParentDirs(corePropsPath_);

    const std::wstring backup = corePropsPath_ + L".lumabridge-backup";
    if (Exists(corePropsPath_) && !Exists(backup)) {
        // Ours are restored/deleted on exit, so an existing file belongs to SteelSeries GG.
        if (CopyFileW(corePropsPath_.c_str(), backup.c_str(), TRUE))
            LUMA_WARN("GameSense: SteelSeries GG's coreProps.json backed up; restored on exit. "
                      "GG and LumaBridge can't both serve GameSense.");
    }
    corePropsBackedUp_ = Exists(backup);
    foundGG_ = corePropsBackedUp_;

    char json[160];
    snprintf(json, sizeof json, "{\"address\":\"127.0.0.1:%d\",\"lumabridge\":true}", port_);
    HANDLE f = CreateFileW(corePropsPath_.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LUMA_ERROR("GameSense: cannot write %s (error %lu). Use Games > SteelSeries > Repair "
                   "(needs admin once) to create the folder with user write access.",
                   NarrowForLog(corePropsPath_).c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    WriteFile(f, json, static_cast<DWORD>(std::strlen(json)), &written, nullptr);
    CloseHandle(f);
    LUMA_INFO("GameSense: wrote %s -> %s", NarrowForLog(corePropsPath_).c_str(), json);
    return true;
}

void Server::RestoreCoreProps() {
    if (corePropsPath_.empty()) return;
    const std::wstring backup = corePropsPath_ + L".lumabridge-backup";
    if (corePropsBackedUp_) {
        MoveFileExW(backup.c_str(), corePropsPath_.c_str(), MOVEFILE_REPLACE_EXISTING);
        LUMA_INFO("GameSense: restored SteelSeries GG's coreProps.json");
    } else {
        DeleteFileW(corePropsPath_.c_str());
    }
}

}  // namespace luma::gamesense
