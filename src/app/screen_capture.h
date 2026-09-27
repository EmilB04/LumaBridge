// "Screen colors": watches the primary monitor with the Desktop Duplication API (the same
// mechanism screen recorders use; it only sees the finished image, never the game) and
// summarises it ~12 times a second. The GPU shrinks each frame to a tiny mip level, so only
// a few thousand pixels are read back. Runs only while a game uses Screen colors.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

#include "screen_colors.h"

namespace luma::app {

class ScreenCapture {
public:
    ~ScreenCapture() { Stop(); }
    void Start();
    void Stop();
    bool Running() const { return thread_.joinable(); }

    // The latest colors; false before the first frame.
    bool Latest(games::ScreenColors* out) const;

private:
    void Run();

    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    games::ScreenColors latest_{};
    bool have_ = false;
};

}  // namespace luma::app
