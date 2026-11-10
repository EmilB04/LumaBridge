// Windows integration checks: actual workers and temporary INI/coreProps files only.
// No device output, driver, registry startup setting or service is changed.
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include "app_settings.h"
#include "controller.h"
#include "game_feeds.h"
#include "gamesense_server.h"
#include "worker_cancellation.h"
#include "log.h"

static int failures;
#define VERIFY(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
static std::wstring TempFile() {
    wchar_t directory[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, directory); GetTempFileNameW(directory, L"LBR", 0, file);
    return file;
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    luma::log::SetConsoleEcho(true);
    std::puts("settings round trip");
    using namespace luma;
    using namespace luma::app;
    WSADATA wsa; VERIFY(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    const auto ini = TempFile();
    const auto props = TempFile(); DeleteFileW(props.c_str());
    const auto configDirectory = TempFile(); DeleteFileW(configDirectory.c_str()); CreateDirectoryW(configDirectory.c_str(), nullptr);
    const auto coreIni = configDirectory + L"\\LumaBridge.ini";
    WritePrivateProfileStringW(L"Color", L"Gamma", L"nan", coreIni.c_str());
    WritePrivateProfileStringW(L"Aura", L"MaxUpdateHz", L"1e99", coreIni.c_str());
    const auto core = LoadConfig(configDirectory);
    VERIFY(std::isfinite(core.auraCorrection.gamma) && core.maxUpdateHz >= 1 && core.maxUpdateHz <= 120);
    DeleteFileW(coreIni.c_str()); RemoveDirectoryW(configDirectory.c_str());
    WritePrivateProfileStringW(L"App", L"ManualSpeed", L"nan", ini.c_str());
    WritePrivateProfileStringW(L"Effects", L"Speed", L"nan", ini.c_str());
    auto prefs = LoadPrefs(ini);
    VERIFY(std::isfinite(prefs.speedHz) && std::isfinite(prefs.setupSpeed));
    prefs.handoffColorEnabled = true; prefs.handoffColor = {10, 20, 30};
    prefs.dualsensePlayers.mask = 0x15; prefs.dualsensePlayers.mode = 4;
    Zone zone; zone.id = "z1"; zone.name = "Desk"; zone.manual = false; zone.devices = {device::kController};
    prefs.zones.push_back(zone);
    Config cfg; cfg.gameSensePort = 0; cfg.gameSenseCoreProps = props; cfg.gameSenseForwardToGG = false;
    SaveAll(ini, prefs, cfg);
    auto loaded = LoadPrefs(ini);
    VERIFY(loaded.handoffColorEnabled && loaded.handoffColor == prefs.handoffColor);
    VERIFY(loaded.dualsensePlayers.mask == 0x15 && loaded.dualsensePlayers.mode == 4);
    VERIFY(loaded.zones.size() == 1 && !loaded.zones.front().manual);
    DeleteZone(loaded.zones, "z1"); SaveAll(ini, loaded, cfg);
    VERIFY(LoadPrefs(ini).zones.empty());
    for (int run = 0; run < 3; ++run) {
        std::printf("GameSense lifecycle %d\n", run);
        gamesense::Server server;
        VERIFY(server.Start(cfg, [] {}));
        std::puts("server started");
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<u_short>(server.Port()));
        VERIFY(connect(client, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        Sleep(100);  // exercise an accepted idle connection, not only the listener
        std::puts("stopping server");
        const auto start = GetTickCount64(); server.Stop();
        std::puts("server stopped");
        VERIFY(GetTickCount64() - start < 2000);
        closesocket(client);
        VERIFY(!server.IsRunning());
    }
    Controller::Output output;
    output.stopped = false;
    output.released = DevicesOutsideZones({zone});
    VERIFY(output.Controls(device::kController) && !output.Controls(device::kMouse));
    output.stopped = true;
    VERIFY(!output.Controls(device::kController));
    // CS2 listener used to depend on cross-thread closesocket waking accept().
    for (int run = 0; run < 2; ++run) {
        std::printf("game feeds lifecycle %d\n", run);
        GameFeeds feeds; feeds.Start(); Sleep(100);
        const auto start = GetTickCount64(); feeds.RequestStop(); feeds.Stop();
        VERIFY(GetTickCount64() - start < 3000);
    }
    // A real synchronous ReadFile that has no writer must be cancellable before join.
    std::puts("synchronous I/O cancellation");
    HANDLE reader = nullptr, writer = nullptr;
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, FALSE};
    VERIFY(CreatePipe(&reader, &writer, &security, 0));
    WorkerCancellation cancellation;
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread thread([&] {
        WorkerCancellation::Scope scope(cancellation);
        SetEvent(started);
        char c; DWORD read; ReadFile(reader, &c, 1, &read, nullptr);
    });
    WaitForSingleObject(started, 1000); Sleep(50);
    const auto cancelStart = GetTickCount64(); cancellation.Cancel(); thread.join();
    VERIFY(GetTickCount64() - cancelStart < 1000);
    CloseHandle(reader); CloseHandle(writer); CloseHandle(started);
    DeleteFileW(ini.c_str()); DeleteFileW(props.c_str()); WSACleanup();
    std::printf("Windows integration checks: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
