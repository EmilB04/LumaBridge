// Game controller input: what a Sony DualSense or DualShock 4 sends, read as plain data. Pure C++,
// tested. Layouts follow the Linux kernel's hid-playstation driver (DualSense) and the documented
// DS4Windows / hid-sony layout (DualShock 4); only the DualSense is confirmed on real hardware here.
//
// DualSense input report: 0x01 over USB (64 bytes), 0x31 over Bluetooth (78 bytes, the same
// data two bytes later after a sequence byte). From the first data byte (offset c):
//   c+0..5 left X, left Y, right X, right Y, L2, R2 (0..255, sticks 128 = centered)
//   c+6 counter, c+7 buttons 0 (d-pad in the low nibble, face buttons above), c+8 buttons 1
//   (L1 R1 L2 R2 create options L3 R3), c+9 buttons 2 (PS, touchpad, mute), c+10 buttons 3
//   c+15..20 gyroscope, c+21..26 accelerometer (3 x s16), c+27..30 sensor time (u32)
//   c+32..35 and c+36..39 the two touches, c+52 battery (low nibble charge in tenths, high
//   nibble: 0 discharging, 1 charging, 2 full)
// DualShock 4: 0x01 over USB (64), 0x11 over Bluetooth (the data two bytes later). From the first
// data byte (offset c): c+0..3 sticks, c+4 buttons 0 (d-pad low nibble, face buttons above), c+5
// buttons 1 (L1 R1 L2 R2 share options L3 R3), c+6 PS / touchpad bits, c+7 L2, c+8 R2, c+12..17
// gyroscope, c+18..23 accelerometer, c+29 battery (low nibble level, bit 4 cable plugged in).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace luma::app::pad {

constexpr uint16_t kSony = 0x054C;
constexpr uint16_t kDualSense = 0x0CE6, kDualSenseEdge = 0x0DF2, kDualShock4V1 = 0x05C4, kDualShock4V2 = 0x09CC;

enum class Model { None, DualSense, DualShock4 };

inline Model ModelOf(uint16_t vid, uint16_t pid) {
    if (vid != kSony) return Model::None;
    if (pid == kDualSense || pid == kDualSenseEdge) return Model::DualSense;
    if (pid == kDualShock4V1 || pid == kDualShock4V2) return Model::DualShock4;
    return Model::None;
}

inline const char* ModelName(Model m) {
    return m == Model::DualSense ? "DualSense" : m == Model::DualShock4 ? "DualShock 4" : "Controller";
}

// One bit per button. The names are the PlayStation ones.
enum Button : int {
    kCross, kCircle, kSquare, kTriangle, kL1, kR1, kL2, kR2, kCreate, kOptions, kL3, kR3, kPs, kTouchpad, kMute,
    kUp, kDown, kLeft, kRight, kButtonCount
};

inline const char* ButtonName(int b) {
    static const char* names[] = {"Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Create",
                                  "Options", "L3", "R3", "PS", "Touchpad", "Mute", "D-pad up", "D-pad down",
                                  "D-pad left", "D-pad right"};
    return b >= 0 && b < kButtonCount ? names[b] : "";
}
// The DualShock 4 calls Create "Share"; the rest are the same.
inline const char* ButtonName(Model m, int b) { return m == Model::DualShock4 && b == kCreate ? "Share" : ButtonName(b); }

inline uint32_t Bit(int b) { return 1u << b; }

struct Touch {
    bool down = false;
    int x = 0, y = 0;  // 0..1919, 0..1079 on a DualSense
};

struct State {
    Model model = Model::None;
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    Touch touch[2];
    int16_t gyro[3] = {0, 0, 0}, accel[3] = {0, 0, 0};
    uint32_t sensorTime = 0;
    int battery = -1;  // percent, -1 unknown
    bool charging = false, full = false;
    bool Down(int b) const { return (buttons & Bit(b)) != 0; }
};

inline uint32_t DpadBits(uint8_t nibble) {
    switch (nibble & 0x0F) {
    case 0: return Bit(kUp);
    case 1: return Bit(kUp) | Bit(kRight);
    case 2: return Bit(kRight);
    case 3: return Bit(kDown) | Bit(kRight);
    case 4: return Bit(kDown);
    case 5: return Bit(kDown) | Bit(kLeft);
    case 6: return Bit(kLeft);
    case 7: return Bit(kUp) | Bit(kLeft);
    default: return 0;  // 8: released
    }
}

inline int16_t S16(const uint8_t* p) { return static_cast<int16_t>(p[0] | p[1] << 8); }

inline Touch DualSenseTouch(const uint8_t* p) {
    Touch t;
    t.down = !(p[0] & 0x80);
    t.x = p[1] | (p[2] & 0x0F) << 8;
    t.y = (p[2] >> 4) | p[3] << 4;
    return t;
}

// A DualSense input report, USB (0x01, 64 bytes) or Bluetooth (0x31, 78 bytes). False for
// anything else (the short Bluetooth report included: it has no battery, motion or touch data).
inline bool ParseDualSense(const uint8_t* r, size_t n, State* out) {
    size_t c;
    if (n >= 64 && r[0] == 0x01) c = 1;
    else if (n >= 78 && r[0] == 0x31) c = 2;
    else return false;
    const uint8_t* d = r + c;
    State s;
    s.model = Model::DualSense;
    s.lx = d[0]; s.ly = d[1]; s.rx = d[2]; s.ry = d[3]; s.l2 = d[4]; s.r2 = d[5];
    const uint8_t b0 = d[7], b1 = d[8], b2 = d[9];
    s.buttons = DpadBits(b0);
    if (b0 & 0x10) s.buttons |= Bit(kSquare);
    if (b0 & 0x20) s.buttons |= Bit(kCross);
    if (b0 & 0x40) s.buttons |= Bit(kCircle);
    if (b0 & 0x80) s.buttons |= Bit(kTriangle);
    const int bits1[] = {kL1, kR1, kL2, kR2, kCreate, kOptions, kL3, kR3};
    for (int i = 0; i < 8; ++i)
        if (b1 & (1 << i)) s.buttons |= Bit(bits1[i]);
    if (b2 & 1) s.buttons |= Bit(kPs);
    if (b2 & 2) s.buttons |= Bit(kTouchpad);
    if (b2 & 4) s.buttons |= Bit(kMute);
    for (int i = 0; i < 3; ++i) {
        s.gyro[i] = S16(d + 15 + 2 * i);
        s.accel[i] = S16(d + 21 + 2 * i);
    }
    s.sensorTime = static_cast<uint32_t>(d[27] | d[28] << 8 | d[29] << 16 | static_cast<uint32_t>(d[30]) << 24);
    s.touch[0] = DualSenseTouch(d + 32);
    s.touch[1] = DualSenseTouch(d + 36);
    const int level = d[52] & 0x0F, status = d[52] >> 4;
    s.charging = status == 1;
    s.full = status == 2;
    s.battery = status == 2 ? 100 : std::min(100, level * 10 + 5);
    *out = s;
    return true;
}

// A DualShock 4 input report, USB (0x01) or Bluetooth (0x11).
inline bool ParseDualShock4(const uint8_t* r, size_t n, State* out) {
    size_t c;
    if (n >= 64 && r[0] == 0x01) c = 1;
    else if (n >= 78 && r[0] == 0x11) c = 3;
    else return false;
    const uint8_t* d = r + c;
    State s;
    s.model = Model::DualShock4;
    s.lx = d[0]; s.ly = d[1]; s.rx = d[2]; s.ry = d[3]; s.l2 = d[7]; s.r2 = d[8];
    const uint8_t b0 = d[4], b1 = d[5], b2 = d[6];
    s.buttons = DpadBits(b0);
    if (b0 & 0x10) s.buttons |= Bit(kSquare);
    if (b0 & 0x20) s.buttons |= Bit(kCross);
    if (b0 & 0x40) s.buttons |= Bit(kCircle);
    if (b0 & 0x80) s.buttons |= Bit(kTriangle);
    const int bits1[] = {kL1, kR1, kL2, kR2, kCreate, kOptions, kL3, kR3};
    for (int i = 0; i < 8; ++i)
        if (b1 & (1 << i)) s.buttons |= Bit(bits1[i]);
    if (b2 & 1) s.buttons |= Bit(kPs);
    if (b2 & 2) s.buttons |= Bit(kTouchpad);
    for (int i = 0; i < 3; ++i) {
        s.gyro[i] = S16(d + 12 + 2 * i);
        s.accel[i] = S16(d + 18 + 2 * i);
    }
    const bool cable = d[29] & 0x10;
    const int level = d[29] & 0x0F;
    s.charging = cable && level < 11;
    s.full = cable && level >= 11;
    s.battery = cable ? std::min(100, level * 100 / 11) : std::min(100, level * 100 / 8);
    *out = s;
    return true;
}

inline bool ParseReport(Model model, const uint8_t* r, size_t n, State* out) {
    return model == Model::DualSense ? ParseDualSense(r, n, out) : model == Model::DualShock4 ? ParseDualShock4(r, n, out) : false;
}

// A stick as -1..1, with the dead zone (0..1 of the throw) taken out and the rest stretched back.
inline float StickAxis(uint8_t v, float deadzone) {
    const float x = (static_cast<int>(v) - 128) / 127.f;
    const float a = std::fabs(x);
    if (a <= deadzone) return 0.f;
    const float t = std::min(1.f, (a - deadzone) / (1.f - deadzone));
    return x < 0 ? -t : t;
}

// ---- Report timing ---------------------------------------------------------------------------
// How often the controller reports, and how evenly: the time between its input reports. A
// DualSense sends about every 4 ms over USB; Bluetooth is slower and less even. This is the
// controller's own report rate, not the delay to the game.
class ReportStats {
public:
    static constexpr size_t kHistory = 120;

    void Reset() { *this = ReportStats(); }

    // `nowUs`: a steady clock in microseconds.
    void OnReport(uint64_t nowUs) {
        if (last_) {
            const double ms = static_cast<double>(nowUs - last_) / 1000.0;
            history_[head_] = static_cast<float>(ms);
            head_ = (head_ + 1) % kHistory;
            count_ = std::min(count_ + 1, kHistory);
            windowSum_ += ms;
            windowN_ += 1;
            if (!windowStart_) windowStart_ = nowUs;
            if (nowUs - windowStart_ >= 1000000) {
                rate_ = windowN_ * 1e6 / static_cast<double>(nowUs - windowStart_);
                windowStart_ = nowUs;
                windowN_ = 0;
                windowSum_ = 0;
            }
        }
        last_ = nowUs;
    }

    double rateHz() const { return rate_; }  // reports a second, over the last full second
    // Over the last kHistory intervals (ms).
    double averageMs() const { return Fold(0); }
    double maxMs() const { return Fold(1); }
    double jitterMs() const { return Fold(2); }  // standard deviation
    // The intervals, oldest first.
    std::vector<float> History() const {
        std::vector<float> out;
        const size_t start = count_ < kHistory ? 0 : head_;
        for (size_t i = 0; i < count_; ++i) out.push_back(history_[(start + i) % kHistory]);
        return out;
    }
    size_t samples() const { return count_; }

private:
    double Fold(int what) const {
        if (!count_) return 0;
        double sum = 0, mx = 0;
        for (size_t i = 0; i < count_; ++i) {
            sum += history_[i];
            mx = std::max<double>(mx, history_[i]);
        }
        const double mean = sum / static_cast<double>(count_);
        if (what == 0) return mean;
        if (what == 1) return mx;
        double var = 0;
        for (size_t i = 0; i < count_; ++i) var += (history_[i] - mean) * (history_[i] - mean);
        return std::sqrt(var / static_cast<double>(count_));
    }

    std::array<float, kHistory> history_{};
    size_t head_ = 0, count_ = 0, windowN_ = 0;
    uint64_t last_ = 0, windowStart_ = 0;
    double windowSum_ = 0, rate_ = 0;
};

}  // namespace luma::app::pad
