#include "gg_forwarder.h"

#include <winhttp.h>

#include "log.h"

namespace luma::gamesense {
namespace {

constexpr DWORD kBackoffMs = 5000;

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

}  // namespace

void GgForwarder::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_) return;
    stop_ = false;
    thread_ = std::thread(&GgForwarder::Run, this);
}

void GgForwarder::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void GgForwarder::SetTarget(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring h = Widen(host);
    if (h == host_ && port == port_) return;
    host_ = h;
    port_ = port;
    lastOk_ = true;
    if (port) LUMA_INFO("GameSense: forwarding to SteelSeries GG at %s:%d", host.c_str(), port);
    else LUMA_INFO("GameSense: not forwarding (SteelSeries GG not found)");
}

int GgForwarder::TargetPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return port_;
}

void GgForwarder::Enqueue(const std::string& path, const std::string& body) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || port_ == 0) return;
        if (queue_.size() >= kMaxQueue) queue_.pop_front();
        queue_.push_back(Item{path, body});
    }
    cv_.notify_one();
}

void GgForwarder::Run() {
    uint64_t pausedUntil = 0;
    for (;;) {
        Item item;
        std::wstring host;
        int port;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) {
                lock.unlock();
                CloseConnection();
                return;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
            host = host_;
            port = port_;
        }
        if (port == 0 || GetTickCount64() < pausedUntil) continue;  // GG down: drop

        const bool ok = Send(item, host, port);
        if (ok != lastOk_.exchange(ok)) {
            if (ok) LUMA_INFO("GameSense: SteelSeries GG reachable again");
            else LUMA_WARN("GameSense: SteelSeries GG not reachable on port %d - retrying every %lus", port,
                           kBackoffMs / 1000);
        }
        if (!ok) pausedUntil = GetTickCount64() + kBackoffMs;
    }
}

void GgForwarder::CloseConnection() {
    if (conn_) WinHttpCloseHandle(conn_);
    if (session_) WinHttpCloseHandle(session_);
    conn_ = session_ = nullptr;
    connPort_ = 0;
}

bool GgForwarder::Send(const Item& item, const std::wstring& host, int port) {
    if (!conn_ || host != connHost_ || port != connPort_) {
        CloseConnection();
        session_ = WinHttpOpen(L"LumaBridge", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return false;
        // Short timeouts: this is loopback, and a hung GG must not back up the queue.
        WinHttpSetTimeouts(session_, 1000, 1000, 2000, 2000);
        conn_ = WinHttpConnect(session_, host.c_str(), static_cast<INTERNET_PORT>(port), 0);
        if (!conn_) {
            CloseConnection();
            return false;
        }
        connHost_ = host;
        connPort_ = port;
    }

    bool ok = false;
    const std::wstring path = Widen(item.path);
    if (HINTERNET req = WinHttpOpenRequest(conn_, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0)) {
        static const wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(req, kHeaders, static_cast<DWORD>(-1L), const_cast<char*>(item.body.data()),
                               static_cast<DWORD>(item.body.size()), static_cast<DWORD>(item.body.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            // Drain the body so WinHTTP can reuse the connection.
            char buf[512];
            DWORD read = 0;
            while (WinHttpReadData(req, buf, sizeof buf, &read) && read) {
            }
            ok = true;  // GG answered; its own 4xx (e.g. unknown event) is GG's business
            if (status >= 400) LUMA_DEBUG("GameSense: GG answered %lu to %s", status, item.path.c_str());
        }
        WinHttpCloseHandle(req);
    }
    if (!ok) CloseConnection();
    return ok;
}

}  // namespace luma::gamesense
