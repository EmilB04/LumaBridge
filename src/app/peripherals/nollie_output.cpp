#include "nollie_output.h"

#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cmath>
#include <memory>

#include "device_lighting.h"
#include "frame_clock.h"
#include "lighting_identity.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr uint64_t kRescanMs = 15000;
constexpr uint64_t kRefreshMs = 2000;  // the same frame again now and then

std::string Narrow(const wchar_t* w) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string b(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b.data(), size, nullptr, nullptr);
    b.pop_back();
    return b;
}

// The USB interface a HID path belongs to ("...&mi_02#..."), -1 for a device with one.
int InterfaceOf(std::string path) {
    for (char& c : path) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    const size_t at = path.find("&mi_");
    if (at == std::string::npos || at + 6 > path.size()) return -1;
    return static_cast<int>(strtol(path.substr(at + 4, 2).c_str(), nullptr, 16));
}

struct Found {
    std::wstring path;
    const nollie::Model* model = nullptr;
    DWORD outLen = 0;
    USHORT usagePage = 0;
};

// Every HID interface of a Nollie hub that can carry the lighting reports.
std::vector<Found> FindHubs() {
    std::vector<Found> out;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof iface;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &iface); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, need, nullptr, nullptr)) continue;
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof attr;
        if (HidD_GetAttributes(q, &attr))
            if (const nollie::Model* m = nollie::Find(attr.VendorID, attr.ProductID)) {
                Found f;
                f.path = detail->DevicePath;
                f.model = m;
                PHIDP_PREPARSED_DATA pre = nullptr;
                HIDP_CAPS caps{};
                if (HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
                    f.outLen = caps.OutputReportByteLength;
                    f.usagePage = caps.UsagePage;
                }
                if (pre) HidD_FreePreparsedData(pre);
                const int mi = InterfaceOf(Narrow(detail->DevicePath));
                // A composite device names its lighting interface; the others: any that takes
                // output reports.
                if (m->interfaceNumber >= 0 ? mi == m->interfaceNumber : f.outLen > 0) out.push_back(f);
            }
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

class Hub {
public:
    ~Hub() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

    bool Open(const Found& f) {
        path_ = f.path;
        model_ = f.model;
        outLen_ = f.outLen;
        info_.path = Narrow(f.path.c_str());
        info_.id = lighting::Identity({"nollie", info_.path});
        info_.name = f.model->name;
        info_.vid = f.model->vid;
        info_.pid = f.model->pid;
        info_.channels = f.model->channels;
        info_.maxLeds = f.model->maxLeds;
        h_ = CreateFileW(f.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            info_.problem = "Another program holds it (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        LUMA_INFO("Nollie: %s opened (USB %04X:%04X, interface %d, usage page 0x%04X, output report %lu bytes)",
                  info_.name.c_str(), info_.vid, info_.pid, InterfaceOf(info_.path), f.usagePage,
                  static_cast<unsigned long>(f.outLen));
        return true;
    }

    // Sends the reports in order. False on an I/O failure.
    bool Send(const std::vector<nollie::Packet>& packets) {
        for (const nollie::Packet& p : packets) {
            std::vector<uint8_t> buf = p.bytes;
            if (buf.size() < outLen_) buf.resize(outLen_, 0);  // the report as the hub declares it
            DWORD written = 0;
            if (!WriteFile(h_, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr)) {
                lastError_ = GetLastError();
                return false;
            }
            if (p.delayMs > 0) Sleep(p.delayMs);
        }
        return true;
    }

    const NollieDevice& info() const { return info_; }
    void SetRate(double fps) { info_.fps = fps; }
    const nollie::Model& model() const { return *model_; }
    const std::wstring& path() const { return path_; }
    DWORD lastError() const { return lastError_; }

    // What the hub shows now, to send again only when it changes.
    uint64_t delivered = 0;
    std::vector<std::vector<Rgb>> last;
    std::vector<int> configSent;
    bool configKnown = false;
    double sentAt = 0, since = 0;
    bool lit = false;
    double bootStart = 0, bootUntil = 0;  // the boot effect plays in between
    double nextFrame = 0;                 // when the next frame is due
    double sequenceStart = 0;             // when the sequence started (over)
    nollie::SequenceParams sequence;      // the one running
    int framesCounted = 0;                // frames sent since countFrom
    double countFrom = 0;

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
    const nollie::Model* model_ = nullptr;
    DWORD outLen_ = 0;
    DWORD lastError_ = 0;
    NollieDevice info_;
};

}  // namespace

void NollieOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    state_ = State::Running;
    thread_ = std::thread(&NollieOutput::Run, this);
}

void NollieOutput::Stop() {
    if (!thread_.joinable()) return;
    RequestStop();
    thread_.join();
    state_ = State::Off;
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
}

void NollieOutput::Set(bool own, const std::map<std::string, NollieChannelPlan>& plans,
                       const std::vector<std::string>& skip, const std::map<std::string, NollieHubPlan>& hubs,
                       const ColorCorrection& correction) {
    std::lock_guard<std::mutex> lock(mutex_);
    own_ = own;
    plans_ = plans;
    skip_ = skip;
    hubs_ = hubs;
    correction_ = correction;
    correction_.brightness = 1.0;
}

void NollieOutput::PlayBoot(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    replay_.push_back(deviceId);
}

void NollieOutput::RestartSequence(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    restart_.push_back(deviceId);
}

std::vector<NollieDevice> NollieOutput::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void NollieOutput::Run() {
    WorkerCancellation::Scope cancellable(cancellation_);
    std::vector<std::unique_ptr<Hub>> hubs;
    std::vector<std::pair<std::wstring, NollieDevice>> unusable;
    std::vector<std::wstring> seen;
    FramePacer pacer(60);
    LUMA_INFO("Nollie: frame timer %s", pacer.precise() ? "high resolution" : "in system timer steps (older Windows)");
    double nextScan = 0;
    auto publish = [&] {
        std::vector<NollieDevice> list;
        for (const auto& h : hubs) list.push_back(h->info());
        for (const auto& [path, info] : unusable) list.push_back(info);
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = list;
    };
    while (!stop_) {
        double now = PreciseNowMs();
        if (now >= nextScan) {
            nextScan = now + kRescanMs;
            const auto found = FindHubs();
            auto present = [&](const std::wstring& p) {
                return std::any_of(found.begin(), found.end(), [&](const Found& f) { return f.path == p; });
            };
            const size_t before = hubs.size();
            // Access can come back when another lighting program closes: try those again.
            const bool hadProblems = !unusable.empty();
            for (const auto& failed : unusable) seen.erase(std::remove(seen.begin(), seen.end(), failed.first), seen.end());
            unusable.clear();
            hubs.erase(std::remove_if(hubs.begin(), hubs.end(), [&](const auto& h) { return !present(h->path()); }), hubs.end());
            seen.erase(std::remove_if(seen.begin(), seen.end(), [&](const std::wstring& p) { return !present(p); }), seen.end());
            bool changed = hubs.size() != before || hadProblems;
            for (const Found& f : found) {
                if (std::find(seen.begin(), seen.end(), f.path) != seen.end()) continue;
                seen.push_back(f.path);
                auto hub = std::make_unique<Hub>();
                if (hub->Open(f)) {
                    hub->since = hub->countFrom = now;
                    hubs.push_back(std::move(hub));
                } else {
                    LUMA_WARN("Nollie: %s can't be lit: %s", hub->info().name.c_str(), hub->info().problem.c_str());
                    unusable.emplace_back(f.path, hub->info());
                }
                changed = true;
            }
            if (changed || (hubs.empty() && unusable.empty())) publish();
            now = PreciseNowMs();  // the scan takes a moment
        }

        bool own;
        std::map<std::string, NollieChannelPlan> plans;
        std::vector<std::string> skip, replay, restart;
        std::map<std::string, NollieHubPlan> hubPlans;
        ColorCorrection cc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            own = own_;
            plans = plans_;
            skip = skip_;
            hubPlans = hubs_;
            cc = correction_;
            replay.swap(replay_);
            restart.swap(restart_);
        }
        int fastest = 30;
        uint64_t deliveredMinimum = UINT64_MAX;
        bool deliveryWanted = false;
        bool rates = false;  // a hub's measured rate changed: show it
        for (auto it = hubs.begin(); it != hubs.end();) {
            Hub& hub = **it;
            const nollie::Model& m = hub.model();
            const auto hp = hubPlans.find(hub.info().id);
            const NollieHubPlan hubPlan = hp != hubPlans.end() ? hp->second : NollieHubPlan{};
            const int hz = nollie::ClampRefresh(hubPlan.refreshHz);
            fastest = std::max(fastest, hz);
            // Frames sent in the last second, for the hub's page.
            if (now - hub.countFrom >= 1000) {
                const double fps = hub.framesCounted * 1000.0 / (now - hub.countFrom);
                if (std::fabs(fps - hub.info().fps) >= 0.5) rates = true;
                hub.SetRate(fps);
                hub.framesCounted = 0;
                hub.countFrom = now;
            }
            const bool wanted = own && std::find(skip.begin(), skip.end(), hub.info().id) == skip.end();
            if (!wanted) {
                hub.lit = false;
                hub.last.clear();  // shown again as soon as it is wanted
                ++it;
                continue;
            }
            // Each hub at its own rate; a frame up to half a millisecond early counts as on time.
            if (now + 0.5 < hub.nextFrame) {
                ++it;
                continue;
            }
            const double period = 1000.0 / hz;
            hub.nextFrame = now - hub.nextFrame > period ? now + period : hub.nextFrame + period;
            // The boot effect starts when the hub starts being lit (again), or on request.
            const bool replayed = std::find(replay.begin(), replay.end(), hub.info().id) != replay.end();
            if (hubPlan.bootSeconds > 0 && (!hub.lit || replayed)) {
                hub.bootStart = now;
                hub.bootUntil = now + hubPlan.bootSeconds * 1000.0;
            }
            if (hubPlan.bootSeconds <= 0) hub.bootUntil = 0;
            const bool booting = now < hub.bootUntil;
            // A new sequence, or one asked to start over, starts from its beginning.
            {
                nollie::SequenceParams running;
                for (int z = 0; z < m.channels && running.kind == nollie::Sequence::None; ++z) {
                    auto plan = plans.find(nollie::Key(hub.info().id, z));
                    if (plan != plans.end()) running = plan->second.sequence;
                }
                if (running.kind != hub.sequence.kind || running.partSeconds != hub.sequence.partSeconds ||
                    std::find(restart.begin(), restart.end(), hub.info().id) != restart.end())
                    hub.sequenceStart = now;
                hub.sequence = running;
            }
            std::vector<std::vector<Rgb>> zones(static_cast<size_t>(m.channels));
            std::vector<int> counts(static_cast<size_t>(m.channels), 0);
            for (int z = 0; z < m.channels; ++z) {
                auto plan = plans.find(nollie::Key(hub.info().id, z));
                if (plan == plans.end() || !plan->second.lit) continue;
                const NollieChannelPlan& p = plan->second;
                const bool channelBooting = booting && std::none_of(p.segments.begin(), p.segments.end(),
                    [](const auto& segment) { return segment.effect.delivery != 0; });
                std::vector<nollie::Segment> parts;
                for (const NollieSegmentPlan& seg : p.segments) {
                    fx::Params effect = seg.effect;
                    double lag = seg.lag;
                    if (channelBooting) {
                        effect = seg.bootEffect;
                        effect.epoch = static_cast<uint64_t>(hub.bootStart);  // every part from the same moment
                        effect = Flowed(effect, seg.bootOffset, p.bootTravelSeconds);
                        lag = seg.bootLag;
                    }
                    // Seconds into the effect to the fraction of a millisecond.
                    const double from = effect.epoch ? static_cast<double>(effect.epoch) : hub.since;
                    parts.push_back({effect, now > from ? (now - from) / 1000.0 : 0.0, seg.brightness, lag});
                }
                nollie::GradientPlace gradient;
                gradient.gradient = &p.gradient;
                gradient.first = p.gradientFirst;
                gradient.parts = p.parts;
                gradient.t = (now - hub.since) / 1000.0;
                if (p.test) {
                    zones[static_cast<size_t>(z)] = nollie::TestChannel(p.channel, m);
                } else if (!channelBooting && p.sequence.kind != nollie::Sequence::None) {
                    std::vector<double> levels;
                    for (const NollieSegmentPlan& seg : p.segments) levels.push_back(seg.brightness);
                    zones[static_cast<size_t>(z)] = nollie::RenderSequence(p.channel, m, p.sequence, (now - hub.sequenceStart) / 1000.0,
                                                                           p.firstPart, p.parts, levels, p.reverse, p.backward, gradient);
                } else {
                    zones[static_cast<size_t>(z)] = nollie::RenderSegments(p.channel, m, parts, gradient);
                }
                if (channelBooting && !p.test) {
                    // Keep the selected boot colors; the normal gradient belongs to the destination.
                    zones[static_cast<size_t>(z)] = nollie::RenderSegments(p.channel, m, parts);
                    const double fadeSeconds = std::min(0.8, hubPlan.bootSeconds * 0.2);
                    const double blend = std::clamp(1.0 - (hub.bootUntil - now) / (fadeSeconds * 1000.0), 0.0, 1.0);
                    if (blend > 0) {
                        std::vector<nollie::Segment> normal;
                        std::vector<double> levels;
                        for (const NollieSegmentPlan& seg : p.segments) {
                            normal.push_back({seg.effect, fx::SecondsAt(seg.effect, now, hub.since), seg.brightness, seg.lag});
                            levels.push_back(seg.brightness);
                        }
                        const auto destination = p.sequence.kind != nollie::Sequence::None
                            ? nollie::RenderSequence(p.channel, m, p.sequence, (now - hub.sequenceStart) / 1000.0,
                                                    p.firstPart, p.parts, levels, p.reverse, p.backward, gradient)
                            : nollie::RenderSegments(p.channel, m, normal, gradient);
                        auto& frame = zones[static_cast<size_t>(z)];
                        for (size_t led = 0; led < frame.size() && led < destination.size(); ++led)
                            frame[led] = fx::detail::Lerp(frame[led], destination[led], blend);
                    }
                }
                if (cc.gainR != 1.0 || cc.gainG != 1.0 || cc.gainB != 1.0 || cc.gamma != 1.0)
                    for (Rgb& c : zones[static_cast<size_t>(z)]) c = ApplyCorrection(cc, c);
                counts[static_cast<size_t>(z)] = static_cast<int>(zones[static_cast<size_t>(z)].size());
            }
            uint64_t token = 0;
            for (int z = 0; z < m.channels; ++z) {
                auto plan = plans.find(nollie::Key(hub.info().id, z));
                if (plan != plans.end() && plan->second.lit)
                    for (const auto& segment : plan->second.segments) token = std::max(token, segment.effect.delivery);
            }
            bool ok = true;
            // Nothing set up on any channel: the hub is left alone, not even blanked.
            const bool anything = std::any_of(counts.begin(), counts.end(), [](int n) { return n > 0; });
            if (anything && (zones != hub.last || now - hub.sentAt >= kRefreshMs || token > hub.delivered)) {
                std::vector<nollie::Packet> packets;
                if (!hub.configKnown || counts != hub.configSent)
                    packets = nollie::ConfigPackets(m, hub.configKnown ? hub.configSent : std::vector<int>{}, counts);
                for (nollie::Packet& p : nollie::Frame(m, zones)) packets.push_back(std::move(p));
                ok = hub.Send(packets);
                if (ok) {
                    hub.delivered = token;
                    hub.last = zones;
                    hub.configSent = counts;
                    hub.configKnown = true;
                    hub.sentAt = now;
                    ++hub.framesCounted;
                    if (!hub.lit) LUMA_INFO("Nollie: lighting %s at up to %d frames a second", hub.info().name.c_str(), hz);
                    hub.lit = true;
                }
            }
            if (token) { deliveryWanted = true; deliveredMinimum = std::min(deliveredMinimum, hub.delivered); }
            if (!ok) {
                LUMA_WARN("Nollie: %s stopped taking colors (error %lu) - unplugged?", hub.info().name.c_str(),
                          static_cast<unsigned long>(hub.lastError()));
                seen.erase(std::remove(seen.begin(), seen.end(), hub.path()), seen.end());
                it = hubs.erase(it);
                nextScan = now + 3000;
                publish();
                continue;
            }
            ++it;
        }
        if (rates) publish();
        // The next tick at the fastest hub's rate, on a steady beat.
        if (deliveryWanted) delivery_.Confirm(deliveredMinimum);
        pacer.SetRate(fastest);
        pacer.Wait();
    }
}

}  // namespace luma::app
