#include "game_feeds.h"

#include <ws2tcpip.h>
#include <winhttp.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
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

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
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
    leagueThread_ = std::thread(&GameFeeds::LeagueLoop, this);
    forzaThread_ = std::thread(&GameFeeds::ForzaLoop, this);
    msfsThread_ = std::thread(&GameFeeds::FlightSimLoop, this);
    dcsThread_ = std::thread(&GameFeeds::DcsLoop, this);
}

void GameFeeds::Stop() {
    if (!cs2Thread_.joinable() && !rlThread_.joinable() && !wtThread_.joinable() && !leagueThread_.joinable() &&
        !forzaThread_.joinable() && !msfsThread_.joinable() && !dcsThread_.joinable())
        return;
    stop_ = true;
    if (cs2Listen_ != INVALID_SOCKET) {
        closesocket(cs2Listen_);  // ends accept()
        cs2Listen_ = INVALID_SOCKET;
    }
    for (std::thread* t : {&cs2Thread_, &rlThread_, &wtThread_, &leagueThread_, &forzaThread_, &msfsThread_, &dcsThread_})
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

GameFeeds::Feed GameFeeds::Dota2(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{dota_.Active(now), dota_.Current(now)};
}

GameFeeds::Feed GameFeeds::League(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{league_.Active(now), league_.Current(now)};
}

GameFeeds::Feed GameFeeds::Forza(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{forza_.Active(now), forza_.Current()};
}

GameFeeds::Feed GameFeeds::FlightSim(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{msfs_.Active(now), msfs_.Current()};
}

GameFeeds::Feed GameFeeds::Dcs(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    return Feed{dcs_.Active(now), dcs_.Current()};
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
                // CS2 and Dota 2 both post here; their app id tells them apart.
                std::lock_guard<std::mutex> lock(mutex_);
                if (j["provider"]["appid"].Number(0) == 570) {
                    if (dota_.OnState(j, GetTickCount64(), kCs2Token) && !dotaSeen_.exchange(true))
                        LUMA_INFO("games: Dota 2 is sending its game state");
                } else if (cs2_.OnState(j, GetTickCount64(), kCs2Token) && !cs2Seen_.exchange(true)) {
                    LUMA_INFO("games: Counter-Strike 2 is sending its game state");
                }
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

// ---- League of Legends ----------------------------------------------------------------------

namespace {
// GET https://127.0.0.1:2999<path> from the League client's own API. Its certificate is Riot's
// own, issued to "127.0.0.1" by Riot's local CA: accepted as is (loopback only).
std::string LeagueGet(HINTERNET session, const wchar_t* path) {
    std::string body;
    HINTERNET con = WinHttpConnect(session, L"127.0.0.1", 2999, 0);
    if (!con) return body;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    if (req && WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof flags) &&
        WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
        DWORD avail = 0;
        while (status == 200 && WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < (4u << 20)) {
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
}  // namespace

void GameFeeds::LeagueLoop() {
    HINTERNET session = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000);
    bool inMatch = false;
    while (!stop_) {
        if (!leagueRunning_) {
            if (inMatch) {
                std::lock_guard<std::mutex> lock(mutex_);
                league_ = games::LeagueLighting{};  // the next match starts fresh
            }
            inMatch = false;
            Nap(stop_, 1000);
            continue;
        }
        const std::string body = LeagueGet(session, L"/liveclientdata/allgamedata");
        Json j;
        if (body.empty() || !Json::Parse(body, &j) || !j["activePlayer"].IsObject()) {
            Nap(stop_, 1000);  // loading screen, or the client without a match
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            league_.OnGameData(j, GetTickCount64());
            if (!inMatch) league_.Prime();  // the first answer: the events so far are history
        }
        if (!inMatch) {
            inMatch = true;
            if (!leagueSeen_.exchange(true)) LUMA_INFO("games: League of Legends is answering (Live Client Data API)");
        }
        Nap(stop_, 200);
    }
    WinHttpCloseHandle(session);
}

// ---- Forza ----------------------------------------------------------------------------------

void GameFeeds::ForzaLoop() {
    SOCKET s = INVALID_SOCKET;
    int boundPort = 0;
    while (!stop_) {
        if (!forzaRunning_ || boundPort != forzaPort_) {
            if (s != INVALID_SOCKET) {
                closesocket(s);
                s = INVALID_SOCKET;
                boundPort = 0;
            }
            if (!forzaRunning_) {
                forzaBusy_ = false;
                Nap(stop_, 1000);
                continue;
            }
        }
        if (s == INVALID_SOCKET) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL exclusive = TRUE;
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(static_cast<u_short>(forzaPort_.load()));
            if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                if (!forzaBusy_.exchange(true))
                    LUMA_WARN("games: UDP port %d is taken (another telemetry app?) - Forza lighting unavailable",
                              forzaPort_.load());
                closesocket(s);
                s = INVALID_SOCKET;
                Nap(stop_, 3000);
                continue;
            }
            forzaBusy_ = false;
            boundPort = forzaPort_;
            DWORD timeout = 500;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            LUMA_INFO("games: listening for Forza's Data Out on 127.0.0.1:%d", boundPort);
        }
        uint8_t buf[1500];
        const int n = recv(s, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (n > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (forza_.OnPacket(buf, static_cast<size_t>(n), GetTickCount64()) && !forzaSeen_.exchange(true))
                LUMA_INFO("games: Forza is sending its Data Out telemetry");
        }
    }
    if (s != INVALID_SOCKET) closesocket(s);
}

// ---- Microsoft Flight Simulator (SimConnect) ------------------------------------------------
// SimConnect.dll is loaded at run time from where Microsoft's free Flight Simulator SDK puts it
// (or next to LumaBridge.exe), so nothing of it ships with LumaBridge.

namespace {

// The few SimConnect calls used (SimConnect.h, the SDK's C API).
struct SimConnectApi {
    HMODULE dll = nullptr;
    HRESULT(__stdcall* open)(HANDLE*, LPCSTR, HWND, DWORD, HANDLE, DWORD) = nullptr;
    HRESULT(__stdcall* close)(HANDLE) = nullptr;
    HRESULT(__stdcall* addToDataDefinition)(HANDLE, DWORD, const char*, const char*, DWORD, float, DWORD) = nullptr;
    HRESULT(__stdcall* requestDataOnSimObject)(HANDLE, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD) = nullptr;
    HRESULT(__stdcall* subscribeToSystemEvent)(HANDLE, DWORD, const char*) = nullptr;
    HRESULT(__stdcall* getNextDispatch)(HANDLE, void**, DWORD*) = nullptr;
};

// SimConnect.h's values.
constexpr DWORD kScFloat64 = 4;             // SIMCONNECT_DATATYPE_FLOAT64
constexpr DWORD kScUnused = 0xFFFFFFFF;     // SIMCONNECT_UNUSED
constexpr DWORD kScUserObject = 0;          // SIMCONNECT_OBJECT_ID_USER
constexpr DWORD kScPeriodSimFrame = 3;      // SIMCONNECT_PERIOD_SIM_FRAME
constexpr DWORD kScRecvQuit = 3, kScRecvEvent = 4, kScRecvSimObjectData = 8;
constexpr DWORD kScDataOffset = 40;         // SIMCONNECT_RECV_SIMOBJECT_DATA::dwData
constexpr DWORD kScEventDataOffset = 20;    // SIMCONNECT_RECV_EVENT::dwData

std::vector<std::wstring> SimConnectCandidates() {
    std::vector<std::wstring> out;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        out.push_back(dir + L"SimConnect.dll");
    }
    for (const wchar_t* var : {L"MSFS2024_SDK", L"MSFS_SDK"}) {
        wchar_t v[MAX_PATH] = {};
        if (GetEnvironmentVariableW(var, v, MAX_PATH)) {
            std::wstring d = v;
            if (!d.empty() && d.back() != L'\\') d += L'\\';
            out.push_back(d + L"SimConnect SDK\\lib\\SimConnect.dll");
        }
    }
    out.push_back(L"C:\\MSFS 2024 SDK\\SimConnect SDK\\lib\\SimConnect.dll");
    out.push_back(L"C:\\MSFS SDK\\SimConnect SDK\\lib\\SimConnect.dll");
    return out;
}

bool LoadSimConnect(SimConnectApi* api) {
    if (api->dll) return true;
    for (const auto& path : SimConnectCandidates()) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!m) continue;
        // Through void*: GetProcAddress returns a generic function pointer.
        auto get = [m](const char* name) { return reinterpret_cast<void*>(GetProcAddress(m, name)); };
        api->open = reinterpret_cast<decltype(api->open)>(get("SimConnect_Open"));
        api->close = reinterpret_cast<decltype(api->close)>(get("SimConnect_Close"));
        api->addToDataDefinition = reinterpret_cast<decltype(api->addToDataDefinition)>(get("SimConnect_AddToDataDefinition"));
        api->requestDataOnSimObject =
            reinterpret_cast<decltype(api->requestDataOnSimObject)>(get("SimConnect_RequestDataOnSimObject"));
        api->subscribeToSystemEvent =
            reinterpret_cast<decltype(api->subscribeToSystemEvent)>(get("SimConnect_SubscribeToSystemEvent"));
        api->getNextDispatch = reinterpret_cast<decltype(api->getNextDispatch)>(get("SimConnect_GetNextDispatch"));
        if (api->open && api->close && api->addToDataDefinition && api->requestDataOnSimObject &&
            api->subscribeToSystemEvent && api->getNextDispatch) {
            api->dll = m;
            LUMA_INFO("games: using %s for Flight Simulator", Narrow(path).c_str());
            return true;
        }
        FreeLibrary(m);
    }
    return false;
}

}  // namespace

void GameFeeds::FlightSimLoop() {
    SimConnectApi api;
    HANDLE sim = nullptr;
    bool loggedMissing = false;
    auto disconnect = [&] {
        if (sim) api.close(sim);
        sim = nullptr;
        msfsConnected_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        msfs_.OnDisconnected();
    };
    while (!stop_) {
        if (!msfsRunning_) {
            if (sim) disconnect();
            Nap(stop_, 1000);
            continue;
        }
        if (!LoadSimConnect(&api)) {
            msfsDll_ = 0;
            if (!loggedMissing) LUMA_INFO("games: Flight Simulator runs, but SimConnect.dll wasn't found (install the MSFS SDK)");
            loggedMissing = true;
            Nap(stop_, 5000);
            continue;
        }
        msfsDll_ = 1;
        if (!sim) {
            if (FAILED(api.open(&sim, "LumaBridge", nullptr, 0, nullptr, 0))) {
                sim = nullptr;  // still loading: try again
                Nap(stop_, 3000);
                continue;
            }
            int count = 0;
            const games::SimVar* vars = games::FlightSimVars(&count);
            for (int i = 0; i < count; ++i) api.addToDataDefinition(sim, 1, vars[i].name, vars[i].unit, kScFloat64, 0, kScUnused);
            api.requestDataOnSimObject(sim, 1, 1, kScUserObject, kScPeriodSimFrame, 0, 0, 2, 0);
            api.subscribeToSystemEvent(sim, 1, "Sim");
            msfsConnected_ = true;
            LUMA_INFO("games: connected to Flight Simulator (SimConnect)");
        }
        bool any = false;
        for (int k = 0; k < 64 && sim; ++k) {
            void* data = nullptr;
            DWORD size = 0;
            if (FAILED(api.getNextDispatch(sim, &data, &size)) || !data || size < 12) break;
            any = true;
            const auto* bytes = static_cast<const uint8_t*>(data);
            DWORD id;
            std::memcpy(&id, bytes + 8, 4);
            if (id == kScRecvQuit) {
                LUMA_INFO("games: Flight Simulator closed SimConnect");
                disconnect();
                break;
            }
            if (id == kScRecvEvent && size >= kScEventDataOffset + 4) {
                DWORD running;
                std::memcpy(&running, bytes + kScEventDataOffset, 4);
                std::lock_guard<std::mutex> lock(mutex_);
                msfs_.OnSimRunning(running != 0);
            } else if (id == kScRecvSimObjectData) {
                int count = 0;
                games::FlightSimVars(&count);
                if (size < kScDataOffset + 8u * static_cast<DWORD>(count)) continue;
                double v[16];
                std::memcpy(v, bytes + kScDataOffset, 8u * static_cast<size_t>(count));
                std::lock_guard<std::mutex> lock(mutex_);
                msfs_.OnState(games::FlightSimFromValues(v, count), GetTickCount64());
                if (!msfsSeen_.exchange(true)) LUMA_INFO("games: Flight Simulator is sending your aircraft's state");
            }
        }
        if (!any) Sleep(20);
    }
    if (sim) api.close(sim);
    if (api.dll) FreeLibrary(api.dll);
}

// ---- DCS World ------------------------------------------------------------------------------

void GameFeeds::DcsLoop() {
    SOCKET s = INVALID_SOCKET;
    while (!stop_) {
        if (!dcsRunning_) {
            if (s != INVALID_SOCKET) closesocket(s);
            s = INVALID_SOCKET;
            dcsBusy_ = false;
            Nap(stop_, 1000);
            continue;
        }
        if (s == INVALID_SOCKET) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            BOOL exclusive = TRUE;
            setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(static_cast<u_short>(games::DcsLighting::kPort));
            if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
                if (!dcsBusy_.exchange(true))
                    LUMA_WARN("games: UDP port %d is taken - DCS World lighting unavailable", games::DcsLighting::kPort);
                closesocket(s);
                s = INVALID_SOCKET;
                Nap(stop_, 3000);
                continue;
            }
            dcsBusy_ = false;
            DWORD timeout = 500;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
            LUMA_INFO("games: listening for DCS World on 127.0.0.1:%d", games::DcsLighting::kPort);
        }
        char buf[2048];
        const int n = recv(s, buf, sizeof buf - 1, 0);
        if (n > 0) {
            Json j;
            if (!Json::Parse(std::string(buf, static_cast<size_t>(n)), &j)) continue;
            std::lock_guard<std::mutex> lock(mutex_);
            if (dcs_.OnPacket(j, GetTickCount64()) && !dcsSeen_.exchange(true))
                LUMA_INFO("games: DCS World is sending your aircraft's state");
        }
    }
    if (s != INVALID_SOCKET) closesocket(s);
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
        // Which team is yours (the camera's target), in the log; if the game never says, the
        // fields it does send, once, to see why.
        if (rl_.myTeam() != rlLoggedTeam_) {
            rlLoggedTeam_ = rl_.myTeam();
            if (rlLoggedTeam_ >= 0) LUMA_INFO("games: Rocket League: your team is %s", rlLoggedTeam_ ? "orange" : "blue");
            rlUpdatesWithoutTeam_ = 0;
        }
        if (rlLoggedTeam_ < 0 && m.find("UpdateState") != std::string::npos && ++rlUpdatesWithoutTeam_ == 300)
            LUMA_INFO("games: Rocket League doesn't say which team is yours (no Game.Target) - both team colors. "
                      "An update: %.600s",
                      m.c_str());
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

std::wstring Dota2ConfigPath(const std::wstring& gameDir) {
    return gameDir.empty() ? L"" : gameDir + L"\\game\\dota\\cfg\\gamestate_integration\\gamestate_integration_lumabridge.cfg";
}

bool Dota2ConfigInstalled(const std::wstring& gameDir) {
    const std::wstring p = Dota2ConfigPath(gameDir);
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string Dota2ConfigText() {
    return "\"LumaBridge\"\n"
           "{\n"
           "  \"uri\" \"http://127.0.0.1:" + std::to_string(GameFeeds::kCs2Port) + "/\"\n"
           "  \"timeout\" \"5.0\"\n"
           "  \"buffer\" \"0.05\"\n"
           "  \"throttle\" \"0.1\"\n"
           "  \"heartbeat\" \"10.0\"\n"
           "  \"data\"\n"
           "  {\n"
           "    \"provider\" \"1\"\n"
           "    \"map\" \"1\"\n"
           "    \"player\" \"1\"\n"
           "    \"hero\" \"1\"\n"
           "  }\n"
           "  \"auth\"\n"
           "  {\n"
           "    \"token\" \"" + std::string(GameFeeds::kCs2Token) + "\"\n"
           "  }\n"
           "}\n";
}

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
    // Its folders first (Dota 2's gamestate_integration folder may not exist yet).
    for (size_t i = path.find_first_of(L"\\/", 3); i != std::wstring::npos; i = path.find_first_of(L"\\/", i + 1))
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
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


// ---- DCS World setup ------------------------------------------------------------------------

std::vector<std::wstring> DcsSavedGames() {
    std::vector<std::wstring> out;
    PWSTR saved = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)) || !saved) return out;
    const std::wstring base = saved;
    CoTaskMemFree(saved);
    for (const wchar_t* name : {L"DCS", L"DCS.openbeta", L"DCS.release"}) {
        const std::wstring dir = base + L"\\" + name;
        const DWORD attr = GetFileAttributesW(dir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir);
    }
    return out;
}

bool DcsSetUp() {
    for (const auto& dir : DcsSavedGames())
        if (games::DcsHasHook(ReadText(dir + L"\\Scripts\\Export.lua")) &&
            GetFileAttributesW((dir + L"\\Scripts\\LumaBridge.lua").c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
    return false;
}

std::string DcsSetUpScripts(bool remove) {
    const auto dirs = DcsSavedGames();
    if (dirs.empty()) return "DCS World's Saved Games folder wasn't found. Start DCS once, then try again.";
    for (const auto& dir : dirs) {
        const std::wstring scripts = dir + L"\\Scripts";
        const std::wstring exportLua = scripts + L"\\Export.lua", ours = scripts + L"\\LumaBridge.lua";
        const std::string text = ReadText(exportLua);
        if (remove) {
            if (games::DcsHasHook(text) && !WriteTextFile(exportLua, games::DcsExportWithoutHook(text)))
                return "Couldn't write " + Narrow(exportLua);
            DeleteFileW(ours.c_str());
            continue;
        }
        if (!WriteTextFile(ours, games::DcsScriptText())) return "Couldn't write " + Narrow(ours);
        if (!games::DcsHasHook(text) && !WriteTextFile(exportLua, games::DcsExportWithHook(text)))
            return "Couldn't write " + Narrow(exportLua);
        LUMA_INFO("games: DCS World export set up in %s", Narrow(scripts).c_str());
    }
    return "";
}

}  // namespace luma::app
