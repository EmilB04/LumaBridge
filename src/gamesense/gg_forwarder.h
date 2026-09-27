// Passes GameSense requests on to SteelSeries GG, so GG keeps working (SteelSeries devices,
// Moments auto-clips) while LumaBridge answers games and lights Aura.
//
// Requests are queued and sent in order from one background thread: the game always gets
// LumaBridge's immediate answer and never waits on GG. When GG is unreachable, requests are
// dropped and sending is retried after a short back-off.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace luma::gamesense {

class GgForwarder {
public:
    ~GgForwarder() { Stop(); }

    void Start();
    void Stop();

    // Where GG listens (from its coreProps.json). Port 0 disables forwarding.
    void SetTarget(const std::string& host, int port);
    int TargetPort() const;
    bool LastSendOk() const { return lastOk_; }

    void Enqueue(const std::string& path, const std::string& body);

private:
    struct Item {
        std::string path, body;
    };
    void Run();
    // Sends over the cached connection, (re)opening it for a new host/port.
    bool Send(const Item& item, const std::wstring& host, int port);
    void CloseConnection();

    // Used only by the worker thread.
    void* session_ = nullptr;  // HINTERNET
    void* conn_ = nullptr;     // HINTERNET
    std::wstring connHost_;
    int connPort_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::wstring host_;
    int port_ = 0;
    bool stop_ = true;
    std::thread thread_;
    std::atomic<bool> lastOk_{true};

    static constexpr size_t kMaxQueue = 512;  // GG gone: drop rather than grow forever
};

}  // namespace luma::gamesense
