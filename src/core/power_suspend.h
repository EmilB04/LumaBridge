// A sleep request stays in force until Windows reports a resume. The worker acknowledges
// after sending its dark frame, so the window can wait briefly before Windows suspends it.
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace luma {

class PowerSuspend {
public:
    void Request(bool suspend) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_ != suspend) acknowledged_ = false;
        requested_ = suspend;
        ready_.notify_all();
    }
    bool Requested() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requested_;
    }
    bool Acknowledged() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return acknowledged_;
    }
    void Acknowledge() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requested_) acknowledged_ = true;
        ready_.notify_all();
    }
    bool Wait(unsigned milliseconds) {
        std::unique_lock<std::mutex> lock(mutex_);
        return ready_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                               [&] { return !requested_ || acknowledged_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool requested_ = false, acknowledged_ = false;
};

}  // namespace luma
