#include "game_feeds.h"

#include <ws2tcpip.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#include "http_parser.h"
#include "json.h"
#include "log.h"

namespace luma::app {
namespace {

std::string ReadText(const std::wstring& path) {
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (1u << 20)) out.append(buf, n);
    fclose(f);
    return out;
}

// GET http://127.0.0.1:<port><path>; "" on failure.
std::string HttpGet(HINTERNET session, int port, const wchar_t* path) {
    std::string body;
    HINTERNET con = WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!con) return body;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, nullptr, nullptr, 0);
    if (req && WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < (1u << 20)) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    return body;
}

SOCKET ConnectLoopback(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// Sleeps in small steps so Stop() isn't held up.
void Nap(const std::atomic<bool>& stop, int ms) {
    for (int t = 0; t < ms && !stop; t += 100) Sleep(100);
}

}  // namespace

// ---- lifecycle ----------------------------------------------------------------------------

void GameFeeds::Start() {
    if (cs2Thread_.joinable() || rlThread_.joinable()) return;
    stop_ = false;
    WSADATA wsa;
    wsa_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;

    cs2Listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cs2Listen_ != INVALID_SOCKET) {
        BOOL exclusive = TRUE;
        setsockopt(cs2Listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                   sizeof exclusive);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from the network
        a.sin_port = htons(static_cast<u_short>(kCs2Port));
        if (bind(cs2Listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(cs2Listen_, 8) != 0) {
            LUMA_WARN("games: port %d is taken - Counter-Strike 2 lighting unavailable", kCs2Port);
            closesocket(cs2Listen_);
            cs2Listen_ = INVALID_SOCKET;
        } else {
            LUMA_INFO("games: listening for Counter-Strike 2 on 127.0.0.1:%d", kCs2Port);
            cs2Thread_ = std::thread(&GameFeeds::Cs2Accept, this);
        }
    }
    rlThread_ = std::thread(&GameFeeds::RocketLeagueLoop, this);
    wtThread_ = std::thread(&GameFeeds::WarThunderLoop, this);
}

void GameFeeds::Stop() {
    if (!cs2Thread_.joinable() && !rlThread_.joinable() && !wtThread_.joinable()) return;
    stop_ = true;
    if (cs2Listen_ != INVALID_SOCKET) {
        closesocket(cs2Listen_);  // ends accept()
        cs2Listen_ = INVALID_SOCKET;
    }
    for (std::thread* t : {&cs2Thread_, &rlThread_, &wtThread_})
        if (t->joinable()) t->join();
    if (wsa_) WSACleanup();
    wsa_ = false;
}

GameFeeds::Feed GameFeeds::Cs2(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{cs2_.Active(now), cs2_.Current(now)};
}

GameFeeds::Feed GameFeeds::RocketLeague(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{rl_.Active(now), rl_.Current(now)};
}

GameFeeds::Feed GameFeeds::WarThunder(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{wt_.Active(now), wt_.Current(now)};
}

// ---- Counter-Strike 2 ----------------------------------------------------------------------

void GameFeeds::Cs2Accept() {
    while (!stop_) {
        SOCKET c = accept(cs2Listen_, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (stop_) break;
            Sleep(50);
            continue;
        }
        Cs2Serve(c);  // CS2 is the only client and keeps one connection: serve it inline
    }
}

void GameFeeds::Cs2Serve(SOCKET s) {
    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    http::Parser parser;
    char buf[16384];
    bool open = true;
    while (open && !stop_) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) {
            if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) continue;  // idle keep-alive
            break;
        }
        parser.Feed(buf, static_cast<size_t>(n));
        http::Request req;
        http::Parser::Status st;
        while ((st = parser.Next(&req)) == http::Parser::Status::Done) {
            Json j;
            if (req.method == "POST" && Json::Parse(req.body, &j)) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cs2_.OnState(j, GetTickCount64(), kCs2Token) && !cs2Seen_.exchange(true))
                    LUMA_INFO("games: Counter-Strike 2 is sending its game state");
            }
            const std::string out = http::BuildResponse(200, "", req.keepAlive);
            send(s, out.data(), static_cast<int>(out.size()), 0);
            if (!req.keepAlive) {
                open = false;
                break;
            }
        }
        if (st == http::Parser::Status::Error) break;
    }
    closesocket(s);
}

// ---- Rocket League --------------------------------------------------------------------------

void GameFeeds::RlHandle(std::string* buffer) {
    std::vector<std::string> messages;
    games::TakeJsonObjects(buffer, &messages);
    for (const auto& m : messages) {
        Json j;
        if (!Json::Parse(m, &j)) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        rl_.OnMessage(j, GetTickCount64());
    }
}

void GameFeeds::RocketLeagueLoop() {
    bool loggedNoApi = false;
    while (!stop_) {
        if (!rlRunning_) {
            Nap(stop_, 1000);
            continue;
        }
        const int port = rlPort_;
        // 1) Plain TCP: the game streams JSON as soon as we connect.
        SOCKET s = ConnectLoopback(port);
        if (s != INVALID_SOCKET) {
            DWORD timeout = 2000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            std::string buffer;
            char buf[16384];
            bool gotData = false;
            while (!stop_ && rlRunning_) {
                const int n = recv(s, buf, sizeof buf, 0);
                if (n > 0) {
                    if (!gotData) {
                        gotData = true;
                        rlConnected_ = true;
                        LUMA_INFO("games: connected to Rocket League's Stats API on port %d", port);
                    }
                    buffer.append(buf, static_cast<size_t>(n));
                    RlHandle(&buffer);
                } else if (n < 0 && WSAGetLastError() == WSAETIMEDOUT && gotData) {
                    continue;  // quiet moment (menus)
                } else {
                    break;
                }
            }
            closesocket(s);
            if (gotData) {
                rlConnected_ = false;
                LUMA_INFO("games: Rocket League's Stats API connection ended");
                continue;
            }
            // 2) Connected but silent: it may be a WebSocket server that waits for a handshake.
            HINTERNET session = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
            HINTERNET con = session ? WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0) : nullptr;
            HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", L"/", nullptr, nullptr, nullptr, 0) : nullptr;
            HINTERNET ws = nullptr;
            if (req && WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr))
                ws = WinHttpWebSocketCompleteUpgrade(req, 0);
            if (ws) {
                rlConnected_ = true;
                LUMA_INFO("games: connected to Rocket League's Stats API (WebSocket) on port %d", port);
                std::string buffer;
                std::vector<char> frame(64 * 1024);
                while (!stop_ && rlRunning_) {
                    DWORD got = 0;
                    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                    if (WinHttpWebSocketReceive(ws, frame.data(), static_cast<DWORD>(frame.size()), &got, &type) != NO_ERROR ||
                        type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                        break;
                    buffer.append(frame.data(), got);
                    RlHandle(&buffer);
                }
                rlConnected_ = false;
                WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
                WinHttpCloseHandle(ws);
                LUMA_INFO("games: Rocket League's Stats API connection ended");
            }
            if (req) WinHttpCloseHandle(req);
            if (con) WinHttpCloseHandle(con);
            if (session) WinHttpCloseHandle(session);
        } else if (!loggedNoApi) {
            loggedNoApi = true;
            LUMA_INFO("games: Rocket League is running but its Stats API isn't answering on port %d "
                      "(switch it on under Integrations, then restart the game)", port);
        }
        Nap(stop_, 3000);
    }
}

// ---- War Thunder ---------------------------------------------------------------------------

void GameFeeds::WarThunderLoop() {
    HINTERNET session = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 500, 500, 500, 500);
    while (!stop_) {
        if (!wtRunning_) {
            Nap(stop_, 1000);
            continue;
        }
        const std::string ind = HttpGet(session, 8111, L"/indicators");
        Json j;
        if (!ind.empty() && Json::Parse(ind, &j)) {
            const uint64_t now = GetTickCount64();
            if (!wtSeen_.exchange(true)) LUMA_INFO("games: reading War Thunder's local status page");
            Json state;
            const std::string st = j["valid"].Bool(false) && j["army"].String() != "tank"
                                       ? HttpGet(session, 8111, L"/state") : std::string();
            std::lock_guard<std::mutex> lock(mutex_);
            wt_.OnIndicators(j, now);
            if (!st.empty() && Json::Parse(st, &state)) wt_.OnState(state, now);
            Sleep(250);
        } else {
            Nap(stop_, 2000);  // menu / loading: the page isn't up
        }
    }
    WinHttpCloseHandle(session);
}

// ---- setup helpers --------------------------------------------------------------------------

std::string Cs2ConfigText() {
    char text[1024];
    snprintf(text, sizeof text,
             "\"LumaBridge\"\n"
             "{\n"
             "  \"uri\" \"http://127.0.0.1:%d/cs2\"\n"
             "  \"timeout\" \"1.1\"\n"
             "  \"buffer\" \"0.05\"\n"
             "  \"throttle\" \"0.1\"\n"
             "  \"heartbeat\" \"30.0\"\n"
             "  \"auth\"\n"
             "  {\n"
             "    \"token\" \"%s\"\n"
             "  }\n"
             "  \"data\"\n"
             "  {\n"
             "    \"provider\" \"1\"\n"
             "    \"map\" \"1\"\n"
             "    \"round\" \"1\"\n"
             "    \"player_id\" \"1\"\n"
             "    \"player_state\" \"1\"\n"
             "  }\n"
             "}\n",
             GameFeeds::kCs2Port, GameFeeds::kCs2Token);
    return text;
}

std::wstring Cs2ConfigPath(const std::wstring& gameDir) {
    return gameDir.empty() ? L"" : gameDir + L"\\game\\csgo\\cfg\\gamestate_integration_lumabridge.cfg";
}

bool Cs2ConfigInstalled(const std::wstring& gameDir) {
    return !gameDir.empty() && ReadText(Cs2ConfigPath(gameDir)) == Cs2ConfigText();
}

std::wstring RocketLeagueStatsIni(const std::wstring& gameDir) {
    return gameDir.empty() ? L"" : gameDir + L"\\TAGame\\Config\\DefaultStatsAPI.ini";
}

bool RocketLeagueStatsEnabled(const std::wstring& gameDir) {
    const std::string ini = ReadText(RocketLeagueStatsIni(gameDir));
    return !ini.empty() && games::StatsIniValue(ini, "PacketSendRate", 0) > 0;
}

int RocketLeagueStatsPort(const std::wstring& gameDir) {
    const int port = games::StatsIniValue(ReadText(RocketLeagueStatsIni(gameDir)), "Port", 49123);
    return port > 0 && port < 65536 ? port : 49123;
}

std::string RocketLeagueStatsText(const std::wstring& gameDir, bool enable) {
    const std::string ini = ReadText(RocketLeagueStatsIni(gameDir));
    if (ini.empty()) return "";
    return games::WithPacketSendRate(ini, enable ? 30 : 0);
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    const DWORD err = GetLastError();
    CloseHandle(f);
    SetLastError(err);
    return ok;
}

}  // namespace luma::app
