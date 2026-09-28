#include "lamparray_output.h"

#include <windows.h>
#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <map>
#include <memory>

#include "lamparray.h"
#include "log.h"

namespace luma::app {
namespace {

namespace la = lamparray;

constexpr int kColumns = 32;              // effects run across a device in this many steps
constexpr uint64_t kRescanMs = 15000;     // new devices
constexpr uint64_t kRefreshMs = 5000;     // every lamp again now and then
constexpr DWORD kFrameMs = 33;

std::string Narrow(const wchar_t* w) {
    char b[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
    return b;
}

// Every HID interface whose top-level collection is a LampArray.
std::vector<std::wstring> LampArrayPaths() {
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
        PHIDP_PREPARSED_DATA pre = nullptr;
        HIDP_CAPS caps{};
        if (HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
            caps.UsagePage == la::kPage && caps.Usage == la::kLampArray)
            out.push_back(detail->DevicePath);
        if (pre) HidD_FreePreparsedData(pre);
        CloseHandle(q);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// One LampArray device, open.
class Device {
public:
    ~Device() { Close(); }

    // Opens it and reads what it is and where its lamps sit. False (with info().problem set)
    // when it can't be lit.
    bool Open(const std::wstring& path) {
        path_ = path;
        h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            // Read-only, to name it at least.
            h_ = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            ReadNames();
            info_.problem = "Another program holds it (Windows' own Dynamic Lighting?)";
            return false;
        }
        ReadNames();
        if (!HidD_GetPreparsedData(h_, &pre_) || HidP_GetCaps(pre_, &caps_) != HIDP_STATUS_SUCCESS) {
            info_.problem = "Its description couldn't be read";
            return false;
        }
        report_.assign(caps_.FeatureReportByteLength, 0);
        // Where each field is: (link collection usage, usage) -> its value caps.
        USHORT n = caps_.NumberFeatureValueCaps;
        std::vector<HIDP_VALUE_CAPS> vc(n);
        if (n) HidP_GetValueCaps(HidP_Feature, vc.data(), &n, pre_);
        for (USHORT i = 0; i < n; ++i) {
            if (vc[i].UsagePage != la::kPage) continue;
            const USAGE u = vc[i].IsRange ? vc[i].Range.UsageMin : vc[i].NotRange.Usage;
            fields_[{vc[i].LinkUsage, u}] = vc[i];
        }
        const HIDP_VALUE_CAPS* count = Field(la::kAttributesReport, la::kLampCount);
        const HIDP_VALUE_CAPS* ids = Field(la::kMultiUpdateReport, la::kLampId);
        if (!count || !ids || !Field(la::kControlReport, la::kAutonomousMode) ||
            !Field(la::kAttributesRequestReport, la::kLampId) || !Field(la::kAttributesResponseReport, la::kLampId)) {
            info_.problem = "It doesn't describe the lighting reports LumaBridge needs";
            return false;
        }
        perReport_ = std::max<USHORT>(1, ids->ReportCount);
        for (Usage u : {la::kLampId, la::kRedChannel, la::kGreenChannel, la::kBlueChannel, la::kIntensityChannel}) {
            const HIDP_VALUE_CAPS* c = Field(la::kMultiUpdateReport, u);
            if (!c || (c->ReportCount > 1 && c->BitSize % 8)) {
                info_.problem = "Its lamp updates are laid out in a way LumaBridge can't fill";
                return false;
            }
        }
        // The array's attributes.
        if (!Get(la::kAttributesReport)) {
            info_.problem = "It didn't answer";
            return false;
        }
        info_.lamps = Value(la::kAttributesReport, la::kLampCount);
        info_.kind = Value(la::kAttributesReport, la::kLampArrayKind);
        minIntervalUs_ = Value(la::kAttributesReport, la::kMinUpdateInterval);
        if (!info_.lamps || info_.lamps > 4096) {
            info_.problem = "It reports no lamps";
            return false;
        }
        // Each lamp: where it sits, and its color levels. Asked for one by one.
        lamps_.clear();
        for (uint32_t id = 0; id < info_.lamps; ++id) {
            if (!Begin(la::kAttributesRequestReport) || !SetValue(la::kAttributesRequestReport, la::kLampId, id) ||
                !Put() || !Get(la::kAttributesResponseReport))
                break;
            Lamp l;
            l.id = static_cast<uint16_t>(Value(la::kAttributesResponseReport, la::kLampId));
            l.x = Value(la::kAttributesResponseReport, la::kPositionX);
            if (lamps_.empty()) {
                levels_[0] = Value(la::kAttributesResponseReport, la::kRedLevelCount);
                levels_[1] = Value(la::kAttributesResponseReport, la::kGreenLevelCount);
                levels_[2] = Value(la::kAttributesResponseReport, la::kBlueLevelCount);
                levels_[3] = Value(la::kAttributesResponseReport, la::kIntensityLevelCount);
            }
            lamps_.push_back(l);
        }
        if (lamps_.empty()) {
            info_.problem = "Its lamps couldn't be read";
            return false;
        }
        minX_ = maxX_ = lamps_[0].x;
        for (const Lamp& l : lamps_) {
            minX_ = std::min(minX_, l.x);
            maxX_ = std::max(maxX_, l.x);
        }
        LUMA_INFO("LampArray: %s (%04X:%04X, %s, %u lamps, %u per update)", info_.name.c_str(), info_.vid, info_.pid,
                  la::KindName(info_.kind), info_.lamps, perReport_);
        return true;
    }

    // 1: the device runs its own effect; 0: LumaBridge sets the lamps.
    bool Autonomous(bool on) {
        return Begin(la::kControlReport) && SetValue(la::kControlReport, la::kAutonomousMode, on ? 1 : 0) && Put();
    }

    // Every lamp's color for this frame (an effect across the device, left to right).
    bool Frame(const fx::Params& p, double t, double level, bool force) {
        std::vector<std::pair<uint16_t, Rgb>> lamps;
        std::vector<Rgb> colors;
        for (const Lamp& l : lamps_) {
            const Rgb c = Scale(fx::Render(p, t, la::Column(l.x, minX_, maxX_, kColumns), kColumns), level);
            lamps.emplace_back(l.id, c);
            colors.push_back(c);
        }
        const uint64_t now = GetTickCount64();
        if (!force && colors == last_ && now - sentAt_ < kRefreshMs) return true;
        const auto batches = la::Batches(lamps, perReport_);
        for (size_t b = 0; b < batches.size(); ++b)
            if (!SendBatch(batches[b], b + 1 == batches.size())) return false;
        last_ = colors;
        sentAt_ = now;
        return true;
    }

    uint32_t minIntervalMs() const { return minIntervalUs_ / 1000; }
    const LampArrayDevice& info() const { return info_; }
    const std::wstring& path() const { return path_; }
    bool controlling = false;

private:
    struct Lamp {
        uint16_t id = 0;
        uint32_t x = 0;
    };
    using Usage = la::Usage;

    void ReadNames() {
        if (h_ == INVALID_HANDLE_VALUE) return;
        wchar_t s[128] = {};
        if (HidD_GetProductString(h_, s, sizeof s)) info_.name = Narrow(s);
        wchar_t m[128] = {};
        if (HidD_GetManufacturerString(h_, m, sizeof m)) {
            const std::string maker = Narrow(m);
            if (!maker.empty() && info_.name.rfind(maker, 0) != 0) info_.name = maker + " " + info_.name;
        }
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        if (HidD_GetAttributes(h_, &a)) {
            info_.vid = a.VendorID;
            info_.pid = a.ProductID;
        }
        if (info_.name.empty()) info_.name = "Lighting device";
    }
    const HIDP_VALUE_CAPS* Field(USAGE report, USAGE usage) const {
        auto it = fields_.find({report, usage});
        return it == fields_.end() ? nullptr : &it->second;
    }
    bool Begin(USAGE report) {
        const HIDP_VALUE_CAPS* c = nullptr;
        for (const auto& [key, caps] : fields_)
            if (key.first == report) c = &caps;
        if (!c) return false;
        std::fill(report_.begin(), report_.end(), 0);
        return HidP_InitializeReportForID(HidP_Feature, c->ReportID, pre_, reinterpret_cast<PCHAR>(report_.data()),
                                          static_cast<ULONG>(report_.size())) == HIDP_STATUS_SUCCESS;
    }
    bool SetValue(USAGE report, USAGE usage, ULONG v) {
        const HIDP_VALUE_CAPS* c = Field(report, usage);
        return c && HidP_SetUsageValue(HidP_Feature, la::kPage, c->LinkCollection, usage, v, pre_,
                                       reinterpret_cast<PCHAR>(report_.data()),
                                       static_cast<ULONG>(report_.size())) == HIDP_STATUS_SUCCESS;
    }
    // A field that holds several values (ReportCount > 1); `values` padded with 0.
    bool SetArray(USAGE report, USAGE usage, const std::vector<ULONG>& values) {
        const HIDP_VALUE_CAPS* c = Field(report, usage);
        if (!c) return false;
        if (c->ReportCount <= 1) return SetValue(report, usage, values.empty() ? 0 : values[0]);
        const size_t bytes = c->BitSize / 8;
        std::vector<char> packed(bytes * c->ReportCount, 0);
        for (size_t i = 0; i < values.size() && i < c->ReportCount; ++i)
            for (size_t k = 0; k < bytes; ++k) packed[i * bytes + k] = static_cast<char>(values[i] >> (8 * k));
        return HidP_SetUsageValueArray(HidP_Feature, la::kPage, c->LinkCollection, usage, packed.data(),
                                       static_cast<USHORT>(packed.size()), pre_, reinterpret_cast<PCHAR>(report_.data()),
                                       static_cast<ULONG>(report_.size())) == HIDP_STATUS_SUCCESS;
    }
    ULONG Value(USAGE report, USAGE usage) const {
        const HIDP_VALUE_CAPS* c = Field(report, usage);
        ULONG v = 0;
        if (c)
            HidP_GetUsageValue(HidP_Feature, la::kPage, c->LinkCollection, usage, &v, pre_,
                               reinterpret_cast<PCHAR>(const_cast<uint8_t*>(report_.data())), static_cast<ULONG>(report_.size()));
        return v;
    }
    bool Get(USAGE report) {
        if (!Begin(report)) return false;
        return HidD_GetFeature(h_, report_.data(), static_cast<ULONG>(report_.size()));
    }
    bool Put() { return HidD_SetFeature(h_, report_.data(), static_cast<ULONG>(report_.size())); }

    bool SendBatch(const la::Batch& batch, bool last) {
        std::vector<ULONG> ids, r, g, b, in;
        for (const auto& [id, c] : batch) {
            ids.push_back(id);
            r.push_back(la::Level(c.r, levels_[0]));
            g.push_back(la::Level(c.g, levels_[1]));
            b.push_back(la::Level(c.b, levels_[2]));
            in.push_back(levels_[3] > 1 ? levels_[3] - 1 : 1);  // full intensity: brightness is in the colors
        }
        return Begin(la::kMultiUpdateReport) &&
               SetValue(la::kMultiUpdateReport, la::kLampCount, static_cast<ULONG>(batch.size())) &&
               SetValue(la::kMultiUpdateReport, la::kUpdateFlags, last ? la::kUpdateComplete : 0) &&
               SetArray(la::kMultiUpdateReport, la::kLampId, ids) && SetArray(la::kMultiUpdateReport, la::kRedChannel, r) &&
               SetArray(la::kMultiUpdateReport, la::kGreenChannel, g) &&
               SetArray(la::kMultiUpdateReport, la::kBlueChannel, b) &&
               (Field(la::kMultiUpdateReport, la::kIntensityChannel) == nullptr ||
                SetArray(la::kMultiUpdateReport, la::kIntensityChannel, in)) &&
               Put();
    }
    void Close() {
        if (pre_) HidD_FreePreparsedData(pre_);
        pre_ = nullptr;
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

    std::wstring path_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    PHIDP_PREPARSED_DATA pre_ = nullptr;
    HIDP_CAPS caps_{};
    std::map<std::pair<USAGE, USAGE>, HIDP_VALUE_CAPS> fields_;
    std::vector<uint8_t> report_;
    LampArrayDevice info_;
    std::vector<Lamp> lamps_;
    uint32_t minX_ = 0, maxX_ = 0;
    uint32_t levels_[4] = {256, 256, 256, 256};
    uint32_t minIntervalUs_ = 0;
    USHORT perReport_ = 1;
    std::vector<Rgb> last_;
    uint64_t sentAt_ = 0;
};

}  // namespace

void LampArrayOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    state_ = State::Running;
    thread_ = std::thread(&LampArrayOutput::Run, this);
}

void LampArrayOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
}

void LampArrayOutput::Set(const fx::Params& effect, double brightness, bool own, const std::vector<std::string>& skip) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
    skip_ = skip;
}

std::vector<LampArrayDevice> LampArrayOutput::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void LampArrayOutput::Run() {
    std::vector<std::unique_ptr<Device>> devs;   // can be lit
    std::vector<std::pair<std::wstring, LampArrayDevice>> unusable;  // found, but can't be lit (with why)
    std::vector<std::wstring> seen;              // paths looked at already
    uint64_t nextScan = 0;
    auto publish = [&] {
        std::vector<LampArrayDevice> list;
        for (const auto& d : devs) list.push_back(d->info());
        for (const auto& [path, info] : unusable) list.push_back(info);
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = list;
    };
    while (!stop_) {
        uint64_t now = GetTickCount64();
        if (now >= nextScan) {
            nextScan = now + kRescanMs;
            const auto paths = LampArrayPaths();
            // Unplugged: drop (their handles are gone anyway).
            devs.erase(std::remove_if(devs.begin(), devs.end(),
                                      [&](const auto& d) { return std::find(paths.begin(), paths.end(), d->path()) == paths.end(); }),
                       devs.end());
            seen.erase(std::remove_if(seen.begin(), seen.end(),
                                      [&](const std::wstring& p) { return std::find(paths.begin(), paths.end(), p) == paths.end(); }),
                       seen.end());
            bool changed = false;
            const size_t before = unusable.size();
            unusable.erase(std::remove_if(unusable.begin(), unusable.end(),
                                          [&](const auto& u) { return std::find(paths.begin(), paths.end(), u.first) == paths.end(); }),
                           unusable.end());
            changed = unusable.size() != before;
            for (const std::wstring& p : paths) {
                if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
                seen.push_back(p);
                auto d = std::make_unique<Device>();
                if (d->Open(p)) {
                    devs.push_back(std::move(d));
                } else {
                    LUMA_INFO("LampArray: %s can't be lit: %s", d->info().name.c_str(), d->info().problem.c_str());
                    unusable.emplace_back(p, d->info());
                }
                changed = true;
            }
            if (changed || devs.empty()) publish();
        }

        fx::Params effect;
        double level;
        bool own;
        uint64_t since;
        std::vector<std::string> skip;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            level = brightness_;
            own = own_;
            since = effectSince_;
            skip = skip_;
        }
        const double t = static_cast<double>(now - since) / 1000.0;
        DWORD wait = kFrameMs;
        for (auto it = devs.begin(); it != devs.end();) {
            Device& d = **it;
            const bool wanted = own && std::find(skip.begin(), skip.end(), d.info().name) == skip.end();
            bool ok = true;
            if (!wanted) {
                if (d.controlling) ok = d.Autonomous(true);  // its own effect again
                d.controlling = false;
            } else {
                const bool first = !d.controlling;
                if (first) ok = d.Autonomous(false);
                d.controlling = ok;
                if (ok) ok = d.Frame(effect, t, level, first);
                wait = std::max<DWORD>(wait, d.minIntervalMs());
            }
            if (!ok) {
                LUMA_WARN("LampArray: %s stopped answering - unplugged?", d.info().name.c_str());
                seen.erase(std::remove(seen.begin(), seen.end(), d.path()), seen.end());  // tried again next scan
                it = devs.erase(it);
                nextScan = now + 3000;
                publish();
                continue;
            }
            ++it;
        }
        Sleep(wait);
    }
    for (auto& d : devs)
        if (d->controlling) d->Autonomous(true);
}

}  // namespace luma::app
