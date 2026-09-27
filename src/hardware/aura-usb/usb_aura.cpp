#include "usb_aura.h"

#include <string>

#include "log.h"

namespace luma::aurausb {
namespace {


std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
    return s;
}

}  // namespace

std::vector<AuraDeviceInfo> ProbeDevices(const std::vector<uint16_t>& productIds, int argbLeds) {
    std::vector<AuraDeviceInfo> out;
    for (uint16_t pid : productIds) {
        for (const auto& found : FindControllers(kVendorAsus, pid)) {
            Device dev;
            if (!dev.Open(found.path)) continue;
            Report reply;
            ConfigTable cfg;
            if (dev.Transact(ConfigRequest(), 0x30, &reply) && ParseConfig(reply, &cfg))
                for (const auto& c : BuildChannels(cfg, argbLeds))
                    out.push_back(AuraDeviceInfo{c.name, c.auraType, c.leds, 0, 0});
        }
    }
    return out;
}

UsbAura::UsbAura(std::vector<uint16_t> productIds, int argbLeds) : pids_(std::move(productIds)), argbLeds_(argbLeds) {}

bool UsbAura::Connect() {
    Disconnect(false);
    for (uint16_t pid : pids_) {
        auto found = FindControllers(kVendorAsus, pid);
        if (found.empty()) continue;
        if (!dev_.Open(found[0].path)) {
            LUMA_WARN("Aura USB: found 0B05:%04X but could not open it (error %lu)", pid, GetLastError());
            continue;
        }
        Report reply;
        std::string fw = "?";
        if (dev_.Transact(FirmwareRequest(), 0x02, &reply)) ParseFirmware(reply, &fw);
        ConfigTable cfg;
        if (!dev_.Transact(ConfigRequest(), 0x30, &reply) || !ParseConfig(reply, &cfg)) {
            LUMA_WARN("Aura USB: 0B05:%04X (firmware %s) did not return its configuration", pid, fw.c_str());
            dev_.Close();
            continue;
        }
        channels_ = BuildChannels(cfg, argbLeds_);
        selected_.assign(channels_.size(), true);
        infos_.clear();
        for (const auto& c : channels_) infos_.push_back(AuraDeviceInfo{c.name, c.auraType, c.leds, 0, 0});
        LUMA_INFO("Aura USB: controller 0B05:%04X firmware %s: %d ARGB header(s), %d board LED(s)", pid, fw.c_str(),
                  cfg.ArgbHeaders(), cfg.MainboardLeds());
        for (const auto& c : channels_)
            LUMA_INFO("Aura USB:   \"%s\" direct channel %d, %d LEDs", Narrow(c.name).c_str(), c.directChannel, c.leds);
        directMode_ = false;
        return true;
    }
    LUMA_WARN("Aura USB: no Aura motherboard controller found");
    return false;
}

void UsbAura::Disconnect(bool releaseControl) {
    if (dev_.IsOpen() && releaseControl) {
        // The controller has no "give control back" command, and ASUS's lighting service
        // doesn't re-apply its profile on its own. Lights keep the last color until
        // Armoury Crate writes its effect again.
        LUMA_INFO("Aura USB: stopped controlling the lights (they keep their last color until "
                  "Armoury Crate re-applies its lighting)");
    }
    dev_.Close();
    channels_.clear();
    selected_.clear();
    infos_.clear();
}

void UsbAura::SetSelected(size_t index, bool selected) {
    if (index >= selected_.size() || selected_[index] == selected) return;
    selected_[index] = selected;
    LUMA_INFO("Aura USB: \"%s\" %s", Narrow(channels_[index].name).c_str(), selected ? "enabled" : "disabled (off)");
}

bool UsbAura::EnterDirectMode() {
    for (uint8_t ch = 0; ch < kEffectChannels; ++ch)
        if (!dev_.Write(SetModeRequest(ch, kModeDirect))) return false;
    directMode_ = true;
    return true;
}

bool UsbAura::SetAll(uint32_t auraColor) {
    const Rgb color = FromAuraColor(auraColor);
    std::vector<std::vector<Rgb>> frames;
    for (const auto& c : channels_) frames.emplace_back(static_cast<size_t>(c.leds), color);
    return SetFrames(frames);
}

bool UsbAura::SetFrames(const std::vector<std::vector<Rgb>>& frames) {
    if (!dev_.IsOpen()) return false;
    // Direct mode is entered once per connection: re-sending the mode command blanks the
    // LEDs for an instant, which showed as a flicker every couple of seconds on a B550-F.
    // Frames themselves are re-sent regularly by the caller.
    if (!directMode_ && !EnterDirectMode()) {
        LUMA_WARN("Aura USB: write failed (controller unplugged?)");
        return false;
    }
    for (size_t i = 0; i < channels_.size(); ++i) {
        // A switched-off device is dark while LumaBridge controls the lights: every channel
        // is in direct mode, so there is no Armoury Crate effect to leave running.
        std::vector<Rgb> frame(static_cast<size_t>(channels_[i].leds), Rgb{});
        if (selected_[i] && i < frames.size())
            for (size_t k = 0; k < frame.size(); ++k)
                frame[k] = frames[i].empty() ? Rgb{} : frames[i][k < frames[i].size() ? k : frames[i].size() - 1];
        for (const Report& r : DirectColorRequests(channels_[i].directChannel, frame)) {
            if (!dev_.Write(r)) {
                LUMA_WARN("Aura USB: write failed (controller unplugged?)");
                return false;
            }
        }
    }
    return true;
}

}  // namespace luma::aurausb
