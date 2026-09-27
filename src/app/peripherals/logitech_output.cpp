#include "logitech_output.h"

#include "log.h"
#include "real_logiled.h"

namespace luma::app {
namespace {

constexpr int kDeviceTypeAll = 7;  // LOGI_DEVICETYPE_MONOCHROME | RGB | PERKEY_RGB
constexpr DWORD kFrameMs = 50;

int Percent(uint8_t v) { return (v * 100 + 127) / 255; }

}  // namespace

void LogitechOutput::Start(const std::wstring& proxyPath) {
    if (thread_.joinable()) return;
    proxyPath_ = proxyPath;
    stop_ = false;
    thread_ = std::thread(&LogitechOutput::Run, this);
}

void LogitechOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
}

void LogitechOutput::Set(const fx::Params& effect, bool own) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = effect.kind != effect_.kind || effect.speed != effect_.speed;
    effect_ = effect;
    own_ = own;
    if (changed) effectSince_ = GetTickCount64();
}

std::wstring LogitechOutput::dllPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dll_;
}

void LogitechOutput::Run() {
    if (!real::Load(L"", proxyPath_) || !real::LogiLedSetLighting) {
        LUMA_INFO("Logitech devices: G HUB's LED SDK not found - Logitech lighting unavailable");
        state_ = State::NoGHub;
        while (!stop_) Sleep(200);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dll_ = real::LoadedPath();
    }
    bool inited = false;
    uint64_t nextInitTry = 0;
    int last[3] = {-1, -1, -1};
    uint64_t lastSent = 0;
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        bool own;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            own = own_;
            since = effectSince_;
        }
        const uint64_t now = GetTickCount64();
        if (!own) {
            if (inited) {
                real::LogiLedShutdown();  // G HUB takes its profile back
                inited = false;
                LUMA_INFO("Logitech devices: handed back to G HUB");
            }
            state_ = State::Released;
            continue;
        }
        if (!inited) {
            if (now < nextInitTry) continue;
            inited = real::LogiLedInitWithName ? real::LogiLedInitWithName("LumaBridge")
                                               : (real::LogiLedInit && real::LogiLedInit());
            if (!inited) {
                state_ = State::Waiting;  // G HUB not running (yet)
                nextInitTry = now + 5000;
                continue;
            }
            if (real::LogiLedSetTargetDevice) real::LogiLedSetTargetDevice(kDeviceTypeAll);
            LUMA_INFO("Logitech devices: controlling them through G HUB");
            last[0] = last[1] = last[2] = -1;
        }
        const Rgb c = fx::Render(effect, static_cast<double>(now - since) / 1000.0, 0, 1);
        const int p[3] = {Percent(c.r), Percent(c.g), Percent(c.b)};
        // Send changes right away, and the same color again every 2 s (G HUB may have been
        // restarted or had another program in between).
        if (p[0] != last[0] || p[1] != last[1] || p[2] != last[2] || now - lastSent > 2000) {
            if (!real::LogiLedSetLighting(p[0], p[1], p[2])) {
                real::LogiLedShutdown();
                inited = false;
                nextInitTry = now + 5000;
                state_ = State::Waiting;
                continue;
            }
            last[0] = p[0];
            last[1] = p[1];
            last[2] = p[2];
            lastSent = now;
        }
        state_ = State::Active;
    }
    if (inited) real::LogiLedShutdown();
}

}  // namespace luma::app
