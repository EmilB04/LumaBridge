// "Screen colors": watches the primary monitor with the Desktop Duplication API (the same
// mechanism screen recorders use; it only sees the finished image, never the game) and
// summarises it ~12 times a second. The GPU shrinks each frame to a tiny mip level, so only
// a few thousand pixels are read back. Runs only while a game uses Screen colors.
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    // The latest tiny frame itself (BGRA, w x h) and its number, for reading a game's HUD;
    // false before the first frame.
    bool LatestFrame(std::vector<uint8_t>* bgra, int* w, int* h, uint64_t* seq) const;
    // Why the screen isn't being read right now ("" while it is): capture refused, or the
    // screen reads as black (a game hidden from capture).
    std::string problem() const;

private:
    void Run();

    std::atomic<bool> stop_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    games::ScreenColors latest_{};
    bool have_ = false;
    std::string problem_;
    std::vector<uint8_t> frame_;
    int frameW_ = 0, frameH_ = 0;
    uint64_t frameSeq_ = 0;
};

}  // namespace luma::app
