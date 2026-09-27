// Built-in SteelSeries GameSense server (the part of SteelSeries GG that games talk to):
// a loopback-only HTTP server plus the coreProps.json that tells games where it is.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "gamesense_engine.h"

namespace luma::gamesense {

class Server {
public:
    struct Snapshot {
        bool active = false;  // a GameSense game is currently sending events
        uint64_t version = 0;
        std::optional<Ambient> ambient;
    };

    ~Server() { Stop(); }

    // Opens the socket, writes coreProps.json and starts serving. `onActivity` runs on a
    // server thread after every request (use it to wake the consumer).
    bool Start(const Config& cfg, std::function<void()> onActivity);
    void Stop();  // also restores / removes coreProps.json

    bool IsRunning() const { return running_; }
    int Port() const { return port_; }
    // True when SteelSeries GG's own coreProps.json was found (and backed up).
    bool FoundSteelSeriesGG() const { return foundGG_; }
    // False when coreProps.json couldn't be written, i.e. games can't find the server.
    bool CorePropsWritten() const { return corePropsOk_; }

    Snapshot Poll(uint64_t nowMs);  // advances timers; cheap

private:
    bool Listen(int preferredPort);
    void AcceptLoop();
    void Serve(SOCKET s);
    bool WriteCoreProps(const std::wstring& overridePath);
    void RestoreCoreProps();

    std::mutex engineMutex_;
    Engine engine_;
    std::function<void()> onActivity_;

    std::atomic<bool> stop_{false};
    bool running_ = false;
    bool wsaStarted_ = false;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread acceptThread_;
    std::mutex clientsMutex_;
    std::vector<SOCKET> clients_;  // open game connections, shut down on Stop()

    std::wstring corePropsPath_;
    bool corePropsBackedUp_ = false;
    bool foundGG_ = false;
    bool corePropsOk_ = false;
};

}  // namespace luma::gamesense
