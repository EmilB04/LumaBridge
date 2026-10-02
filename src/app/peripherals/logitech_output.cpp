#include "logitech_output.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <optional>
#include <string>
#include <vector>

#include "log.h"
#include "device_sleep.h"
#include "logitech_hidpp.h"
#include "real_logiled.h"

namespace luma::app {
namespace {

constexpr int kDeviceTypeAll = 7;  // LOGI_DEVICETYPE_MONOCHROME | RGB | PERKEY_RGB
constexpr DWORD kFrameMs = 20;  // quick enough to keep up with the fans and keyboard (game flashes)

int Percent(uint8_t v) { return (v * 100 + 127) / 255; }

// The device paths of every Logitech HID++ long-report interface (usage page 0xFF00, 20-byte
// reports): receivers and devices on their cables.
std::vector<std::wstring> HidppInterfaces() {
    std::vector<std::wstring> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == hidpp::kVendor;
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == 0xFF00 && caps.OutputReportByteLength == 20;
            if (pre) HidD_FreePreparsedData(pre);
        }
        CloseHandle(q);
        if (match) out.push_back(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// A Logitech mouse that runs its own effects (HID++ RGB effects), reached through its
// receiver or its cable, next to G HUB (which keeps its own connection).
class HidppMouse {
public:
    explicit HidppMouse(std::function<bool()> cancelled = {}) : cancelled_(std::move(cancelled)) {}
    ~HidppMouse() { Close(); }
    bool open() const { return h_ != INVALID_HANDLE_VALUE; }
    const hidpp::Layout& layout() const { return layout_; }
    const std::string& name() const { return name_; }

    // Looks through every Logitech HID++ interface for a device with RGB effects.
    bool Find() {
        Close();
        for (const std::wstring& path : HidppInterfaces()) {
            if (!OpenPath(path)) continue;
            if (Probe()) return true;
            Close();
        }
        return false;
    }

    bool OpenPath(const std::wstring& path) {
        Close();
        h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        return open();
    }

    // Every device behind this interface (a receiver's paired devices, or the one on the cable).
    void List(std::vector<LogitechDevice>* out) {
        for (uint8_t dev : {hidpp::kWired, uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{5}, uint8_t{6}}) {
            hidpp::Report r;
            // Root.GetFeature(name): only a HID++ 2.0 device answers.
            if (!Send(hidpp::Request(dev, 0x00, 0, {hidpp::kFeatureName >> 8, hidpp::kFeatureName & 0xFF}), &r, 400))
                continue;
            LogitechDevice d;
            if (const uint8_t nameFeature = r[4]) {
                d.name = ReadName(dev, nameFeature);
                if (Send(hidpp::Request(dev, nameFeature, 2, {}), &r)) d.type = r[4];
            }
            for (uint16_t id : hidpp::kLightingFeatures)
                if (!d.rgb && Send(hidpp::Request(dev, 0x00, 0, {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)}), &r) &&
                    r[4])
                    d.rgb = true;
            out->push_back(d);
        }
    }

    bool Set(const hidpp::Effect& e) { return Send(hidpp::SetEffect(device_, feature_, layout_, e), nullptr); }
    bool StartPerKey() { return Send(hidpp::StartPerKey(device_, feature_, layout_), nullptr); }
    // One frame: a color per LED along the strip (layout().strip.size() colors).
    // The frame's LED reports go out without waiting for each answer (one round trip per frame
    // instead of one per report, so the mouse keeps up with the other devices); the last report
    // (the frame end) waits, which still notices a mouse that stopped answering.
    bool Frame(const Rgb* colors) {
        const auto reports = hidpp::PerKeyFrame(device_, layout_, colors);
        for (size_t i = 0; i < reports.size(); ++i)
            if (!(i + 1 < reports.size() ? Write(reports[i]) : Send(reports[i], nullptr, 500))) return false;
        return true;
    }

    void Close() {
        if (open()) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    // Which device index answers with RGB effects, and where its effects are.
    bool Probe() {
        for (uint8_t dev : {hidpp::kWired, uint8_t{1}, uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{5}, uint8_t{6}}) {
            hidpp::Report r;
            if (!Send(hidpp::Request(dev, 0x00, 0, {hidpp::kRgbEffects >> 8, hidpp::kRgbEffects & 0xFF}), &r, 400) || !r[4])
                continue;
            device_ = dev;
            feature_ = r[4];
            name_.clear();
            if (Send(hidpp::Request(dev, 0x00, 0, {hidpp::kFeatureName >> 8, hidpp::kFeatureName & 0xFF}), &r) && r[4]) {
                const uint8_t nameFeature = r[4];
                name_ = ReadName(dev, nameFeature);
                if (Send(hidpp::Request(dev, nameFeature, 2, {}), &r) && r[4] != 3) continue;
            }
            layout_ = hidpp::Layout{};
            for (uint8_t e = 0; e < 16; ++e) {
                if (!Send(hidpp::Request(dev, feature_, 0, {0x00, e}), &r)) break;
                const uint16_t id = static_cast<uint16_t>(r[6] << 8 | r[7]);
                if (id == hidpp::kIdFixed) layout_.fixed = e;
                if (id == hidpp::kIdBreathing) layout_.breathing = e;
                if (id == hidpp::kIdCycle) layout_.cycle = e;
            }
            // The whole-mouse color wave and the LED order are only known for the G502 X Plus.
            const bool g502x = name_ == "G502 X PLUS";
            layout_.colorWave = g502x;
            if (g502x && Send(hidpp::Request(dev, 0x00, 0, {hidpp::kPerKeyLighting >> 8, hidpp::kPerKeyLighting & 0xFF}), &r) && r[4]) {
                layout_.perKeyFeature = r[4];
                for (uint8_t e = 0; e < 16; ++e) {
                    if (!Send(hidpp::Request(dev, feature_, 0, {0xFF, e}), &r)) break;
                    if ((r[6] << 8 | r[7]) == hidpp::kIdPerKey) layout_.perKeyEffect = e;
                }
                layout_.strip.assign(hidpp::kG502XPlusStrip.begin(), hidpp::kG502XPlusStrip.end());
            }
            if (!layout_.Has(hidpp::Kind::Fixed) && !layout_.perKey() && !layout_.colorWave) continue;
            LUMA_INFO("Logitech %s: its own effects over HID++ (device %u, fixed %d, breathing %d, cycle %d, wave %s, "
                      "every LED %s)",
                      name_.c_str(), dev, layout_.fixed, layout_.breathing, layout_.cycle, layout_.colorWave ? "yes" : "no",
                      layout_.perKey() ? "yes" : "no");
            return true;
        }
        return false;
    }

    std::string ReadName(uint8_t dev, uint8_t nameFeature) {
        std::string name;
        hidpp::Report r;
        if (!Send(hidpp::Request(dev, nameFeature, 0, {}), &r)) return name;
        const uint8_t len = r[4];
        while (name.size() < len && Send(hidpp::Request(dev, nameFeature, 1, {static_cast<uint8_t>(name.size())}), &r))
            for (size_t k = 4; k < r.size() && name.size() < len; ++k) name += static_cast<char>(r[k]);
        return name;
    }

    // Sends a request and waits for its answer (other reports are skipped).
    // Writes a request without waiting for its answer (Send reads past it later).
    bool Write(const hidpp::Report& req) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(h_, req.data(), static_cast<DWORD>(req.size()), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(h_, &ov, &n, FALSE);
        if (!ok) CancelIo(h_);
        CloseHandle(ov.hEvent);
        return ok;
    }

    bool Send(const hidpp::Report& req, hidpp::Report* reply, DWORD timeoutMs = 1000) {
        if (cancelled_ && cancelled_()) return false;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = WriteFile(h_, req.data(), static_cast<DWORD>(req.size()), &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0 && GetOverlappedResult(h_, &ov, &n, FALSE);
        if (!ok) CancelIo(h_);
        bool answered = false;
        const uint64_t until = GetTickCount64() + timeoutMs;
        while (ok && !answered) {
            const uint64_t now = GetTickCount64();
            if (now >= until) break;
            hidpp::Report r{};
            ResetEvent(ov.hEvent);
            BOOL got = ReadFile(h_, r.data(), static_cast<DWORD>(r.size()), &n, &ov);
            if (!got && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ov.hEvent, static_cast<DWORD>(until - now)) == WAIT_OBJECT_0)
                    got = GetOverlappedResult(h_, &ov, &n, FALSE);
                else
                    CancelIo(h_);
            }
            if (!got) break;
            if (hidpp::Refuses(r, req)) break;
            if (hidpp::Answers(r, req)) {
                if (reply) *reply = r;
                answered = true;
            }
        }
        CloseHandle(ov.hEvent);
        return answered;
    }

    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint8_t device_ = 0, feature_ = 0;
    hidpp::Layout layout_;
    std::function<bool()> cancelled_;
    std::string name_;
};

}  // namespace

std::vector<LogitechDevice> ScanLogitechDevices(const std::function<bool()>& cancelled) {
    std::vector<LogitechDevice> out;
    HidppMouse link(cancelled);
    for (const std::wstring& path : HidppInterfaces()) {
        if (cancelled && cancelled()) break;
        if (link.OpenPath(path)) link.List(&out);
    }
    // The same device by cable and through its receiver: once.
    std::vector<LogitechDevice> unique;
    for (const LogitechDevice& d : out) {
        bool seen = false;
        for (LogitechDevice& u : unique)
            if (!d.name.empty() && u.name == d.name) {
                u.rgb = u.rgb || d.rgb;
                seen = true;
            }
        if (!seen) unique.push_back(d);
    }
    for (const LogitechDevice& d : unique)
        LUMA_INFO("Logitech device: %s (%s, %s)", d.name.empty() ? "?" : d.name.c_str(), hidpp::DeviceTypeName(d.type),
                  d.rgb ? "RGB lighting" : "no RGB lighting");
    return unique;
}

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

void LogitechOutput::Set(const fx::Params& effect, double brightness, bool own, bool inStep, bool asleep) {
    inStep_ = inStep;
    std::lock_guard<std::mutex> lock(mutex_);
    brightness_ = brightness < 0 ? 0 : brightness > 1 ? 1 : brightness;
    const bool changed = effect.kind != effect_.kind || effect.speed != effect_.speed;
    effect_ = effect;
    own_ = own;
    asleep_ = asleep;
    if (changed) effectSince_ = GetTickCount64();
}

std::wstring LogitechOutput::dllPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dll_;
}

std::string LogitechOutput::directName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return directName_;
}

void LogitechOutput::Run() {
    bool sdkLoaded = real::Load(L"", proxyPath_) && real::LogiLedSetLighting;
    if (!sdkLoaded) {
        LUMA_INFO("Logitech devices: G HUB's LED SDK not found - trying direct HID++ control");
        state_ = State::NoGHub;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dll_ = real::LoadedPath();
    }
    bool inited = false;
    uint64_t nextInitTry = 0;
    int last[3] = {-1, -1, -1};
    uint64_t lastSent = 0;
    HidppMouse mouse;
    uint64_t nextMouseFind = 0, lastEffectSent = 0;
    std::optional<hidpp::Effect> onMouse;  // the mouse's own effect LumaBridge set, if any
    bool perKeyOn = false;                  // the mouse shows LumaBridge's frames, LED by LED
    std::vector<Rgb> lastFrame;
    uint64_t lastFrameSent = 0;
    bool wasAsleep = false;
    sleep::OutputSleep idleSleep;
    uint64_t wokeAt = 0;
    auto sendDark = [&] {
        // Black per-LED frames don't stop every onboard effect. Use a fixed black effect
        // when supported, and darken the SDK too so G HUB doesn't retain an old color.
        if (inited && !real::LogiLedSetLighting(0, 0, 0))
            LUMA_WARN("Logitech: couldn't send the SDK dark frame before sleep");
        if (mouse.open()) {
            bool dark = false;
            if (mouse.layout().Has(hidpp::Kind::Fixed)) {
                hidpp::Effect fixed;
                fixed.kind = hidpp::Kind::Fixed;
                dark = mouse.Set(fixed);
            } else if (mouse.layout().perKey() && (perKeyOn || mouse.StartPerKey())) {
                const std::vector<Rgb> black(mouse.layout().strip.size());
                dark = mouse.Frame(black.data());
            }
            if (!dark) LUMA_WARN("Logitech: couldn't stop the mouse effect before sleep");
        }
        // Keep the SDK session: Shutdown would let G HUB put its profile back.
        mouse.Close();
        nextMouseFind = 0;
        onMouse.reset();
        perKeyOn = false;
        lastFrame.clear();
        last[0] = last[1] = last[2] = -1;
        mouseEffect_ = false;
        wasAsleep = true;
    };
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        double level;
        bool own;
        bool asleep;
        uint64_t since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            level = brightness_;
            own = own_;
            asleep = asleep_;
            since = effectSince_;
        }
        const uint64_t now = GetTickCount64();
        if (powerSuspend_.Requested()) {
            if (!powerSuspend_.Acknowledged()) {
                if (own) sendDark();
                powerSuspend_.Acknowledge();
            }
            continue;
        }
        if (!own) {
            idleSleep.Update(false);
            if (inited) {
                real::LogiLedShutdown();  // G HUB takes its profile back (its lighting replaces the mouse's effect)
                inited = false;
                LUMA_INFO("Logitech devices: handed back to G HUB");
            }
            sdkActive_ = false;
            mouse.Close();
            nextMouseFind = 0;
            onMouse.reset();
            perKeyOn = false;
            mouseEffect_ = false;
            state_ = State::Released;
            continue;
        }
        // Become quiet as soon as the fade reaches black. Even without a connection,
        // stop SDK initialization and HID++ discovery: probing can wake a sleeping mouse.
        const auto sleepAction = idleSleep.Update(asleep || level <= 0);
        if (sleepAction != sleep::OutputSleep::Action::Awake) {
            if (sleepAction == sleep::OutputSleep::Action::Dark) {
                const bool connected = inited || mouse.open();
                sendDark();
                if (connected) state_ = State::Active;
                LUMA_INFO("Logitech devices: lighting off - no SDK or HID++ traffic until used again");
            }
            continue;
        }
        if (wasAsleep) {
            // Used again: everything again, and a mouse that went quiet looked for right away
            // (it may take a moment to reconnect).
            wasAsleep = false;
            wokeAt = now;
            if (!mouse.open()) nextMouseFind = now;
            onMouse.reset();
            perKeyOn = false;
            lastFrame.clear();
            last[0] = last[1] = last[2] = -1;
        }
        if (!inited && now >= nextInitTry) {
            // Retry discovery too: G HUB can be installed or started while the app runs.
            if (!sdkLoaded) sdkLoaded = real::Load(L"", proxyPath_) && real::LogiLedSetLighting;
            inited = sdkLoaded && (real::LogiLedInitWithName ? real::LogiLedInitWithName("LumaBridge")
                                               : (real::LogiLedInit && real::LogiLedInit()));
            if (!inited) {
                nextInitTry = now + 5000;
            } else {
                sdkActive_ = true;
                sdkAvailable_ = true;
                if (real::LogiLedSetTargetDevice) real::LogiLedSetTargetDevice(kDeviceTypeAll);
                LUMA_INFO("Logitech devices: controlling them through G HUB");
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    dll_ = real::LoadedPath();
                }
                // G HUB may have replaced the direct effect as it connected.
                onMouse.reset();
                perKeyOn = false;
                last[0] = last[1] = last[2] = -1;
            }
        }

        // A Logitech mouse LumaBridge can reach directly (HID++) shows the effect itself: the
        // mouse's own effect when it has a matching one (smooth, no traffic), otherwise every
        // LED its own color, frame by frame. The SDK stays connected meanwhile, so G HUB
        // doesn't put its own lighting back.
        if (!mouse.open() && now >= nextMouseFind) {
            nextMouseFind = now + 30000;
            if (!mouse.Find()) LUMA_INFO("Logitech devices: no mouse to light directly found (HID++)");
            std::lock_guard<std::mutex> lock(mutex_);
            directName_ = mouse.open() ? mouse.name() : "";
        }
        auto lostMouse = [&](const char* what) {
            LUMA_WARN("Logitech %s: %s - %s", mouse.name().c_str(), what,
                      inited ? "back to one color through G HUB" : "retrying direct control");
            mouse.Close();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                directName_.clear();
            }
            nextMouseFind = now + (now - wokeAt < 15000 ? 2000 : 30000);  // just woken: it's reconnecting
            onMouse.reset();
            perKeyOn = false;
            last[0] = last[1] = last[2] = -1;
        };
        // The mouse's own effects run on its own clock: fine for your own lighting, but a game's
        // lighting goes LED by LED, in step with the other devices. Also not while fading out for
        // sleep (level < 1): re-sending the effect with a falling intensity restarts its own
        // animation from the start each time (up to 4 times a second), which flickers instead of
        // dimming - the scaled per-key/fixed-color path below fades smoothly instead.
        std::optional<hidpp::Effect> want =
            mouse.open() && !inStep_ && level >= 1.0 ? hidpp::ForEffect(effect, mouse.layout()) : std::nullopt;
        if (want) want->intensity = static_cast<uint8_t>(level * 100 + 0.5);
        if (want) {
            // Slider drags change the speed many times a second: at most 4 sends a second.
            if ((!onMouse || *onMouse != *want) && now - lastEffectSent >= 250) {
                lastEffectSent = now;
                if (mouse.Set(*want)) {
                    onMouse = want;
                    perKeyOn = false;
                } else {
                    lostMouse("the mouse didn't take its effect");
                }
            }
            if (onMouse) {
                state_ = State::Active;
                mouseEffect_ = true;
                continue;
            }
        } else if (mouse.open() && mouse.layout().perKey()) {
            if (!perKeyOn) {
                if (mouse.StartPerKey()) {
                    perKeyOn = true;
                    onMouse.reset();
                    lastFrameSent = 0;
                } else {
                    lostMouse("the mouse didn't switch to per-LED colors");
                }
            }
            if (perKeyOn) {
                const double t = fx::Seconds(effect, now, since);
                const size_t n = mouse.layout().strip.size();
                std::vector<Rgb> frame(n);
                for (size_t i = 0; i < n; ++i)
                    frame[i] = Scale(fx::Render(effect, t, static_cast<int>(i), static_cast<int>(n)), level);
                // Changes right away; the same frame again now and then (the mouse may have slept).
                if (frame != lastFrame || now - lastFrameSent > 5000) {
                    if (mouse.Frame(frame.data())) {
                        lastFrame = frame;
                        lastFrameSent = now;
                    } else {
                        lostMouse("the mouse stopped taking colors");
                    }
                }
                if (perKeyOn) {
                    state_ = State::Active;
                    mouseEffect_ = true;
                    continue;
                }
            }
        }
        mouseEffect_ = false;
        const Rgb c = Scale(fx::Render(effect, fx::Seconds(effect, now, since), 0, 1), level);
        if (onMouse) {
            // Back from the mouse's own effect: a plain color on it, then the SDK's colors again.
            onMouse.reset();
            hidpp::Effect fixed;
            fixed.kind = hidpp::Kind::Fixed;
            fixed.color = c;
            if (mouse.open() && mouse.layout().Has(hidpp::Kind::Fixed)) mouse.Set(fixed);
            last[0] = last[1] = last[2] = -1;
        }
        const int p[3] = {Percent(c.r), Percent(c.g), Percent(c.b)};
        if (!inited) {
            // Mice without a known LED map can still follow games and effects as one color.
            if (mouse.open() && mouse.layout().Has(hidpp::Kind::Fixed)) {
                if (p[0] != last[0] || p[1] != last[1] || p[2] != last[2] || now - lastSent > 2000) {
                    hidpp::Effect fixed;
                    fixed.color = c;
                    if (!mouse.Set(fixed)) {
                        lostMouse("the mouse stopped taking its fixed color");
                        state_ = sdkLoaded ? State::Waiting : State::NoGHub;
                        continue;
                    }
                    std::copy(std::begin(p), std::end(p), last);
                    lastSent = now;
                }
                state_ = State::Active;
            } else {
                state_ = sdkLoaded ? State::Waiting : State::NoGHub;
            }
            continue;
        }
        // Send changes right away, and the same color again every 2 s (G HUB may have been
        // restarted or had another program in between).
        if (p[0] != last[0] || p[1] != last[1] || p[2] != last[2] || now - lastSent > 2000) {
            if (!real::LogiLedSetLighting(p[0], p[1], p[2])) {
                real::LogiLedShutdown();
                inited = false;
                sdkActive_ = false;
                sdkAvailable_ = false;
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
    sdkActive_ = false;
    sdkAvailable_ = false;
    std::lock_guard<std::mutex> lock(mutex_);
    directName_.clear();
}

}  // namespace luma::app
