// A precise clock and a steady frame beat for the lighting outputs (Windows only).
//
// GetTickCount64 and Sleep move in steps of the system timer, about 15.6 ms: an output that
// sleeps "33 ms" really waits 31 or 47 ms, and an effect timed by GetTickCount64 jumps in 16 ms
// steps, so animations stutter. PreciseNowMs reads the performance counter on GetTickCount64's
// scale (the effects' shared clock, fx::Params::epoch), and FramePacer waits on a
// high-resolution timer to a steady beat (each frame at start + n * period, not "now + period",
// which drifts by however long the frame took).
#pragma once

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <mutex>

namespace luma {

// Milliseconds on GetTickCount64's scale, to a fraction of a millisecond. The two are separate
// clocks, so it follows GetTickCount64 again whenever they drift more than 50 ms apart.
inline double PreciseNowMs() {
    static std::mutex m;
    static LARGE_INTEGER freq{}, q0{};
    static double base = 0;
    std::lock_guard<std::mutex> lock(m);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const double tick = static_cast<double>(GetTickCount64());
    double now = base + static_cast<double>(q.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    if (!q0.QuadPart || std::fabs(now - tick) > 50) {
        q0 = q;
        base = tick;
        now = tick;
    }
    return now;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Frames at `hz` on a steady beat. High-resolution timer on Windows 10 1803 and later; older
// Windows falls back to the system timer's steps (precise() false).
class FramePacer {
public:
    explicit FramePacer(double hz = 60) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        precise_ = timer_ != nullptr;
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        SetRate(hz);
        next_ = PreciseNowMs();
    }
    ~FramePacer() {
        if (timer_) CloseHandle(timer_);
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    void SetRate(double hz) { period_ = 1000.0 / (hz < 1 ? 1 : hz > 240 ? 240 : hz); }
    double period() const { return period_; }
    bool precise() const { return precise_; }

    // The moment of the next frame, one period after the last; when far behind (a slow device,
    // a rescan), the beat starts over from now instead of rushing to catch up.
    double Next() {
        const double now = PreciseNowMs();
        next_ += period_;
        if (now - next_ > period_) next_ = now + period_;
        return next_;
    }

    // Waits for the next frame.
    void Wait() { WaitUntil(Next()); }

    // Waits until `atMs` (PreciseNowMs's scale).
    void WaitUntil(double atMs) {
        const double wait = atMs - PreciseNowMs();
        if (wait <= 0) return;
        if (Arm(wait)) {
            WaitForSingleObject(timer_, static_cast<DWORD>(wait) + 50);
            return;
        }
        Sleep(static_cast<DWORD>(wait));
    }

    // Sets the timer to fire in `ms`, for callers that wait on it together with other handles
    // (MsgWaitForMultipleObjectsEx). False when there's no timer.
    bool Arm(double ms) {
        if (!timer_) return false;
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((ms < 0 ? 0 : ms) * 10000.0);  // 100 ns units, relative
        return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != 0;
    }
    HANDLE timer() const { return timer_; }

private:
    HANDLE timer_ = nullptr;
    bool precise_ = false;
    double period_ = 1000.0 / 60;
    double next_ = 0;
};

}  // namespace luma
