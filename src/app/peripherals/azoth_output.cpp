#include "azoth_output.h"

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <string>
#include <cstdio>
#include <vector>

#include "azoth_layout.h"
#include "azoth_protocol.h"
#include "azoth_power.h"
#include "azoth_oled_effects.h"
#include "azoth_oled_upload.h"
#include "azoth_music_source.h"
#include "log.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 40;           // ~25 updates per second at most (only changed keys are sent)
constexpr uint64_t kWirelessFrameMs = 50;   // through the Omni receiver: ~20 updates per second
constexpr uint64_t kRefreshMs = 5000;    // re-send every key now and then (a keyboard waking up)

struct ControlDevices {
    std::wstring wired, receiver;
};

// Descriptor queries only: polling this while asleep does not send reports or wake a
// wireless keyboard. Match the actual control collection, including its input size.
ControlDevices FindAzothControls() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return {};
    ControlDevices found;
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
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        bool match = HidD_GetAttributes(q, &a) && a.VendorID == azoth::kVendor &&
                     (a.ProductID == azoth::kProductWired || a.ProductID == azoth::kProductReceiver);
        const auto link = a.ProductID == azoth::kProductWired ? azoth::Link::Wired : azoth::Link::Wireless;
        match = match && azoth::ControlPath(link, detail->DevicePath);
        if (match) {
            PHIDP_PREPARSED_DATA pre = nullptr;
            HIDP_CAPS caps{};
            match = HidD_GetPreparsedData(q, &pre) && HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == azoth::kUsagePage && caps.OutputReportByteLength == azoth::ReportSize(link) &&
                    caps.InputReportByteLength == azoth::ReportSize(link);
            if (pre) HidD_FreePreparsedData(pre);
        }
        CloseHandle(q);
        if (match) (link == azoth::Link::Wired ? found.wired : found.receiver) = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

HANDLE OpenAzoth(const std::wstring& path) {
    if (path.empty()) return INVALID_HANDLE_VALUE;
    return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

// Each transfer has a deadline. A cancelled operation must finish before its buffer and
// OVERLAPPED go out of scope, including while the keyboard is unplugged or the app exits.
bool Transfer(HANDLE dev, bool write, BYTE* data, DWORD size, DWORD timeout, DWORD& done) {
    done = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL ok = write ? WriteFile(dev, data, size, &done, &ov) : ReadFile(dev, data, size, &done, &ov);
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok && error == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(ov.hEvent, timeout);
        if (wait == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(dev, &ov, &done, FALSE);
            error = ok ? ERROR_SUCCESS : GetLastError();
        } else {
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(dev, &ov);
            GetOverlappedResult(dev, &ov, &done, TRUE);
        }
    }
    CloseHandle(ov.hEvent);
    SetLastError(error);
    return ok != FALSE;
}

bool WriteReport(HANDLE dev, const azoth::Report& report, azoth::Link link) {
    if (azoth::IsSave(report)) { SetLastError(ERROR_INVALID_DATA); return false; }
    DWORD done = 0;
    const DWORD size = static_cast<DWORD>(azoth::ReportSize(link));
    if (!Transfer(dev, true, const_cast<BYTE*>(report.data()), size, 300, done)) return false;
    if (done != size) { SetLastError(ERROR_WRITE_FAULT); return false; }
    return true;
}

bool OledExchange(HANDLE dev, const azoth::Report& command, azoth::Link link, azoth::Report& reply,
                  DWORD* replySize = nullptr, DWORD timeout = 300) {
    const DWORD size = static_cast<DWORD>(azoth::ReportSize(link));
    DWORD done = 0;
    if (replySize) *replySize = 0;
    // RGB acknowledgments and notifications can still be queued on this shared interface.
    for (int i = 0; i < 64; ++i)
        if (!Transfer(dev, false, reply.data(), size, 0, done)) break;
    reply = {};
    if (!WriteReport(dev, command, link)) return false;
    const uint64_t deadline = GetTickCount64() + timeout;
    for (int i = 0; i < 64; ++i) {
        const uint64_t now = GetTickCount64();
        if (now >= deadline) break;
        reply = {};
        if (!Transfer(dev, false, reply.data(), size, static_cast<DWORD>(deadline - now), done)) return false;
        if (replySize) *replySize = done;
        const auto result = azoth::ClassifyOledReply(command, reply, done);
        if (result == azoth::OledReply::Accepted) return true;
        if (result == azoth::OledReply::Rejected) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    }
    SetLastError(ERROR_TIMEOUT);
    return false;
}

std::string ExchangeDetails(azoth::Link link, const azoth::Report& command, const azoth::Report& reply,
                            DWORD size, DWORD error) {
    char prefix[128];
    std::snprintf(prefix, sizeof prefix, "%s command %02X %02X, Windows error %lu. Reply (%lu bytes):",
                  link == azoth::Link::Wired ? "USB" : "Omni", command[1], command[2], error, size);
    std::string out = prefix;
    if (!size) out += " none";
    for (size_t i = 0; i < std::min<size_t>(size, 16); ++i) {
        char hex[4];
        std::snprintf(hex, sizeof hex, " %02X", reply[i]);
        out += hex;
    }
    return out;
}

azoth::OledTime LocalOledTime() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return {time.wYear, static_cast<uint8_t>(time.wMonth), static_cast<uint8_t>(time.wDay),
            static_cast<uint8_t>(time.wHour), static_cast<uint8_t>(time.wMinute)};
}

}  // namespace

azoth::MusicSnapshot AzothOutput::musicSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return musicSnapshot_;
}

void AzothOutput::Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread(&AzothOutput::Run, this);
}

void AzothOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
    oledState_ = OledState::Vendor;
    connection_ = azoth::Connection::Unknown;
    if (uploadState_ == UploadState::Pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        uploadEffect_ = -1;
        uploadState_ = UploadState::Cancelled;
        uploadMessage_ = "Upload cancelled because LumaBridge stopped.";
    }
}

void AzothOutput::Set(const fx::Params& effect, double brightness, bool own, bool asleep) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
    asleep_ = asleep;
}

void AzothOutput::SetOled(azoth::OledSettings settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings = azoth::NormalizeOled(settings);
    if (settings == oled_) return;
    if (settings.direct != oled_.direct) rescan_ = true;
    oled_ = settings;
    ++oledRevision_;
    oledState_ = settings.direct ? OledState::Pending : OledState::Vendor;
    oledError_ = 0;
    oledFailureDetails_.clear();
}

void AzothOutput::ReapplyOled() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++oledRevision_;
    rescan_ = true;
    if (oled_.direct) oledState_ = OledState::Pending;
}

bool AzothOutput::UploadOledEffect(int effect) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (uploadState_ == UploadState::Pending || uploadState_ == UploadState::Uploading) return false;
    if (effect < 0 || effect >= static_cast<int>(azoth::kLumaAnimationNames.size()) || asleep_ ||
        !oled_.direct || !oled_.enabled || oled_.content != azoth::OledContent::Animation ||
        oled_.animationSource != azoth::OledAnimationSource::LumaBridge || oled_.lumaAnimation != effect ||
        connection_ != azoth::Connection::Wired) {
        uploadMessage_ = "Connect the awake Azoth by USB and turn its screen on before uploading.";
        uploadState_ = UploadState::Failed;
        return false;
    }
    uploadEffect_ = effect;
    cancelUpload_ = false;
    uploadProgress_ = 0;
    uploadMessage_ = "Preparing the animation...";
    uploadState_ = UploadState::Pending;
    return true;
}

void AzothOutput::CancelOledUpload() { cancelUpload_ = true; }

std::string AzothOutput::uploadMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return uploadMessage_;
}

std::string AzothOutput::oledFailureDetails() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return oledFailureDetails_;
}

void AzothOutput::Run() {
    azoth::MusicSource music;
    uint64_t musicRevision = UINT64_MAX, nextMusic = 0;
    std::wstring lastSongTitle, lastSongArtist;
    int64_t lastSongSecond = -1, lastSongLength = -1;
    bool songSent = false;
    bool musicNeedsSetup = true;
    HANDLE dev = INVALID_HANDLE_VALUE;
    azoth::Link link = azoth::Link::Wired;
    std::wstring openedPath;
    ControlDevices devices;
    uint64_t nextFind = 0, nextScan = 0, nextHealth = 0, lastSent = 0;
    std::vector<Rgb> lastKeys;  // per key, as last sent
    uint64_t appliedRevision = UINT64_MAX, attemptedRevision = UINT64_MAX, nextOled = 0;
    azoth::Report lastClock{};
    bool oledProbed = false, versionChecked = false, keyboardReady = false, wasAsleep = false;
    azoth::IdleTimeoutOverride idleTimeout;
    uint64_t nextIdleApply = 0;
    auto applyIdleTimeout = [&](bool control) {
        const auto target = idleTimeout.Desired(control);
        if (!target || dev == INVALID_HANDLE_VALUE) return;
        azoth::Report reply{};
        DWORD size = 0;
        const auto command = azoth::SetIdleTimeout(link, *target);
        if (OledExchange(dev, command, link, reply, &size)) {
            sleepTimerError_ = 0;
            idleTimeout.Applied(*target, control);
            LUMA_INFO("ROG Azoth: firmware sleep timer %s (%02X)", control ? "overridden by LumaBridge" : "restored", *target);
        } else {
            sleepTimerError_ = GetLastError();
            LUMA_WARN("ROG Azoth: firmware sleep timer: %s", ExchangeDetails(link, command, reply, size, GetLastError()).c_str());
        }
        nextIdleApply = GetTickCount64() + 5000;
    };
    auto resetSession = [&] {
        music.Reset();
        musicRevision = UINT64_MAX;
        nextMusic = 0;
        songSent = false;
        musicNeedsSetup = true;
        lastSent = 0;
        lastKeys.clear();
        appliedRevision = attemptedRevision = UINT64_MAX;
        nextOled = 0;
        lastClock = {};
        oledProbed = false;
        oledAnimation_ = -1;
        uploadedEffect_ = -1;
        oledError_ = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        musicSnapshot_ = {};
        oledFailureDetails_.clear();
        if (uploadState_ == UploadState::Pending) {
            uploadEffect_ = -1;
            uploadMessage_ = "The USB connection changed. Click Upload to Azoth to try again.";
            uploadState_ = UploadState::Failed;
        }
    };
    auto closeDevice = [&] {
        applyIdleTimeout(false);
        if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
        dev = INVALID_HANDLE_VALUE;
        openedPath.clear();
        versionChecked = keyboardReady = false;
        nextHealth = 0;
        idleTimeout = {};
        nextIdleApply = 0;
        resetSession();
    };
    while (!stop_) {
        Sleep(kFrameMs);
        fx::Params effect;
        double brightness;
        bool own;
        bool asleep;
        uint64_t since;
        azoth::OledSettings oled;
        uint64_t revision;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            effect = effect_;
            brightness = brightness_;
            own = own_;
            asleep = asleep_;
            since = effectSince_;
            oled = oled_;
            revision = oledRevision_;
        }
        const uint64_t now = GetTickCount64();
        const bool liveMusic = oled.direct && oled.enabled && !asleep && dev != INVALID_HANDLE_VALUE &&
                               link == azoth::Link::Wired &&
                               (oled.content == azoth::OledContent::Equalizer || oled.content == azoth::OledContent::SongInfo);
        const auto media = music.Poll(liveMusic ? oled.content : azoth::OledContent::Keep);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            musicSnapshot_ = media;
        }
        // Never carry a queued upload across a disconnect or sleep into a later session.
        if (uploadState_ == UploadState::Pending &&
            (cancelUpload_ || asleep || !oled.direct || !oled.enabled)) {
            std::lock_guard<std::mutex> lock(mutex_);
            uploadEffect_ = -1;
            uploadMessage_ = "Upload cancelled. Click Upload to Azoth to try again.";
            uploadState_ = UploadState::Cancelled;
        }
        const bool forcedScan = rescan_.exchange(false);
        if (forcedScan || now >= nextScan) {
            devices = FindAzothControls();
            nextScan = now + 1000;
            const auto& preferred = !devices.wired.empty() ? devices.wired : devices.receiver;
            if (dev != INVALID_HANDLE_VALUE && openedPath != preferred) {
                LUMA_INFO("ROG Azoth: USB connection changed; selecting the current control interface");
                closeDevice();
                nextFind = 0;
            }
            if (forcedScan) nextHealth = nextFind = 0;
        }
        connection_ = azoth::SelectConnection(!devices.wired.empty(), !devices.receiver.empty(), keyboardReady, asleep);
        if (!oled.direct) {
            oledState_ = OledState::Vendor;
            appliedRevision = UINT64_MAX;
            oledProbed = false;
            lastClock = {};
        }
        if (!own && !oled.direct) {
            if (dev != INVALID_HANDLE_VALUE) closeDevice();
            connection_ = azoth::SelectConnection(!devices.wired.empty(), !devices.receiver.empty(), false, asleep);
            state_ = State::Released;
            nextFind = 0;
            continue;
        }
        if (!own) { state_ = State::Released; lastSent = 0; }
        if (devices.wired.empty() && devices.receiver.empty()) {
            if (uploadState_ == UploadState::Pending) {
                std::lock_guard<std::mutex> lock(mutex_);
                uploadEffect_ = -1;
                uploadMessage_ = "The keyboard disconnected before the upload started.";
                uploadState_ = UploadState::Failed;
            }
            if (own) state_ = State::NotFound;
            if (oled.direct) oledState_ = OledState::NotFound;
            lastWriteError_ = 0;
            continue;
        }
        // Asleep: nothing goes out (the dark frame went out before), so the keyboard can sleep.
        // Used again: every key again (lastSent 0 forces a full refresh).
        if (asleep) {
            if (!wasAsleep && dev != INVALID_HANDLE_VALUE) {
                // Explicitly darken both outputs. Merely stopping updates leaves the OLED
                // running until ASUS's independent timer fires.
                if (own) {
                    std::vector<azoth::KeyColor> black;
                    for (const auto& key : azoth::IsoKeys()) black.push_back({static_cast<uint8_t>(key.led), Rgb{}});
                    for (const auto& report : azoth::KeyColors(black, link))
                        if (!WriteReport(dev, report, link)) {
                            LUMA_WARN("ROG Azoth: couldn't send the final sleep RGB frame (error %lu)", GetLastError());
                            break;
                        }
                }
                if (oled.direct) {
                    azoth::Report reply{};
                    if (!OledExchange(dev, azoth::OledCommand(link, 0x69, 0), link, reply))
                        LUMA_WARN("ROG Azoth: couldn't turn the OLED off for sleep (error %lu)", GetLastError());
                }
                applyIdleTimeout(false);
            }
            lastSent = 0;
            if (oled.direct) oledState_ = OledState::Asleep;
            appliedRevision = UINT64_MAX;
            lastClock = {};
            wasAsleep = true;
            continue;
        }
        if (wasAsleep) { nextHealth = nextIdleApply = 0; wasAsleep = false; }
        if (dev == INVALID_HANDLE_VALUE) {
            if (now < nextFind) continue;
            // Reconsider the cable even if the receiver is still plugged in and writable.
            link = devices.wired.empty() ? azoth::Link::Wireless : azoth::Link::Wired;
            openedPath = link == azoth::Link::Wired ? devices.wired : devices.receiver;
            dev = OpenAzoth(openedPath);
            if (dev == INVALID_HANDLE_VALUE) {
                lastWriteError_ = GetLastError();
                if (uploadState_ == UploadState::Pending) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    uploadEffect_ = -1;
                    uploadMessage_ = "Couldn't open the USB connection. Reconnect the keyboard and retry the upload.";
                    uploadState_ = UploadState::Failed;
                }
                if (own) state_ = State::NotFound;
                if (oled.direct) oledState_ = OledState::NotFound;
                nextFind = now + 2000;
                continue;
            }
            link_ = link;
            lastWriteError_ = 0;
            resetSession();
        }
        if (!versionChecked) {
            azoth::Report reply{};
            DWORD size = 0;
            const auto command = azoth::StatusQuery(link, 0);
            const bool exchanged = OledExchange(dev, command, link, reply, &size);
            if (!exchanged || !azoth::VersionReply(link, reply, size)) {
                const DWORD error = exchanged ? ERROR_NOT_SUPPORTED : GetLastError();
                const auto detail = ExchangeDetails(link, command, reply, size, error);
                closeDevice();
                lastWriteError_ = error;
                if (own) state_ = State::NotFound;
                if (oled.direct) {
                    oledState_ = OledState::Failed;
                    oledError_ = error;
                    std::lock_guard<std::mutex> lock(mutex_);
                    oledFailureDetails_ = detail;
                }
                LUMA_WARN("ROG Azoth: control interface not ready: %s", detail.c_str());
                nextFind = now + 2000;
                continue;
            }
            versionChecked = true;
            if (link == azoth::Link::Wired) LUMA_INFO("ROG Azoth: connected (wired USB, interface confirmed)");
        }
        {
            if (now >= nextHealth) {
                nextHealth = now + 2000;
                azoth::Report reply{};
                DWORD size = 0;
                const bool ready = OledExchange(dev, azoth::StatusQuery(link, 1), link, reply, &size) &&
                                   azoth::PowerReply(link, reply, size);
                if (ready) idleTimeout.Observe(reply[7]);
                if (ready && !azoth::ValidIdleTimeout(reply[7])) {
                    if (sleepTimerError_ != ERROR_NOT_SUPPORTED)
                        LUMA_WARN("ROG Azoth: unrecognised firmware sleep timer %02X", reply[7]);
                    sleepTimerError_ = ERROR_NOT_SUPPORTED;
                }
                if (link == azoth::Link::Wireless && ready != keyboardReady) {
                    LUMA_INFO("ROG Azoth: %s", ready ? "keyboard connected through Omni (power reply confirmed)"
                                                   : "receiver present; keyboard stopped answering");
                    resetSession();
                }
                keyboardReady = ready;
            }
            if (link == azoth::Link::Wireless)
                connection_ = keyboardReady ? azoth::Connection::Wireless : azoth::Connection::ReceiverOnly;
            if (link == azoth::Link::Wireless && !keyboardReady) {
                if (own) state_ = State::NotFound;
                if (oled.direct) oledState_ = OledState::NotFound;
                oledError_ = 0;
                continue;
            }
        }
        if (now >= nextIdleApply) applyIdleTimeout(true);
        if (uploadState_ == UploadState::Pending) {
            int uploadEffect;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                uploadEffect = uploadEffect_;
                uploadEffect_ = -1;
                uploadState_ = UploadState::Uploading;
                uploadMessage_ = "Uploading to Azoth. Keep the USB cable connected.";
            }
            bool cancelled = false;
            auto interrupted = [&] {
                std::lock_guard<std::mutex> lock(mutex_);
                cancelled = stop_ || cancelUpload_ || asleep_ || !oled_.direct || !oled_.enabled ||
                            oledRevision_ != revision;
                return cancelled;
            };
            azoth::Report reply{}, failedCommand{};
            DWORD size = 0, error = ERROR_SUCCESS;
            auto exchange = [&](const azoth::Report& command, DWORD timeout) {
                if (interrupted()) return false;
                if (OledExchange(dev, command, link, reply, &size, timeout)) return true;
                error = GetLastError();
                failedCommand = command;
                return false;
            };
            bool ok = link == azoth::Link::Wired;
            std::vector<azoth::OledUploadFrame> frames;
            if (ok) {
                frames.reserve(azoth::kOledEffectFrames);
                for (int i = 0; i < azoth::kOledEffectFrames && !interrupted(); ++i)
                    frames.push_back({azoth::RenderOledEffect(uploadEffect, i * azoth::kOledEffectDelay / 100.0),
                                      azoth::kOledEffectDelay * 10});
            }
            const auto data = cancelled ? std::vector<uint8_t>{} : azoth::OledUploadData(frames);
            ok = ok && !data.empty();
            if (ok) ok = exchange(azoth::OledUploadBegin(data), 1500);
            const size_t chunks = azoth::OledUploadChunks(data);
            // The ASUS SDK advances only after each acknowledgment. Do not retry an
            // ambiguous chunk or activate partial data. RGB and other OLED writes wait.
            for (size_t i = 0; ok && i < chunks; ++i) {
                ok = exchange(azoth::OledUploadPart(data, i), 1000);
                if (ok) uploadProgress_ = static_cast<int>((i + 1) * 99 / chunks);
            }
            if (ok) ok = exchange(azoth::OledUploadShow(), 1500);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (ok) {
                    uploadedEffect_ = uploadEffect;
                    uploadProgress_ = 100;
                    uploadMessage_ = "Upload completed and acknowledged by the keyboard.";
                    uploadState_ = UploadState::Complete;
                } else {
                    uploadedEffect_ = -1;
                    uploadMessage_ = cancelled ? "Upload cancelled. Click Upload to Azoth to start again."
                        : link != azoth::Link::Wired ? "Direct GIF uploads require a USB cable."
                        : error ? "Upload stopped: " + ExchangeDetails(link, failedCommand, reply, size, error)
                                : "Couldn't prepare the animation for upload.";
                    uploadState_ = cancelled ? UploadState::Cancelled : UploadState::Failed;
                }
                LUMA_INFO("ROG Azoth GIF: %s", uploadMessage_.c_str());
            }
            lastSent = 0;
            nextHealth = 0;
            if (stop_) break;
            continue;
        }
        if (oled.direct && (revision != attemptedRevision || now >= nextOled)) {
            const auto time = LocalOledTime();
            const auto clock = azoth::OledClock(link, time, oled.clock12Hour);
            const bool changed = revision != appliedRevision;
            const bool clockDue = oled.enabled && oled.content == azoth::OledContent::Clock && clock != lastClock;
            if (changed || clockDue) {
                attemptedRevision = revision;
                azoth::Report reply{};
                DWORD replySize = 0;
                bool ok = true;
                if (!oledProbed) {
                    // Reading the previous animation is optional. A firmware that cannot
                    // report it must not prevent screen power or brightness controls.
                    if (OledExchange(dev, azoth::ReadOledAnimation(link), link, reply, &replySize)) {
                        oledAnimation_ = reply[5] < 6 ? reply[5] : -1;
                    }
                    oledProbed = true;
                }
                const auto commands = changed ? azoth::OledCommands(oled, link, time)
                                              : std::vector<azoth::Report>{clock};
                bool superseded = false;
                azoth::Report failedCommand{};
                DWORD exchangeError = ERROR_SUCCESS;
                for (const auto& command : commands) {
                    if (!ok || stop_) break;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        superseded = revision != oledRevision_;
                    }
                    if (superseded) break;
                    ok = OledExchange(dev, command, link, reply, &replySize);
                    if (!ok) { failedCommand = command; exchangeError = GetLastError(); }
                }
                if (stop_) break;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    superseded = revision != oledRevision_;
                }
                if (superseded) continue;
                if (ok) {
                    appliedRevision = revision;
                    lastClock = clock;
                    if (oled.enabled && oled.content == azoth::OledContent::Animation &&
                        oled.animationSource == azoth::OledAnimationSource::Asus) oledAnimation_ = oled.animation;
                    if (oled.enabled && (oled.content == azoth::OledContent::Clock ||
                        (oled.content == azoth::OledContent::Animation && oled.animationSource == azoth::OledAnimationSource::Asus)))
                        uploadedEffect_ = -1;
                    oledError_ = 0;
                    oledState_ = OledState::Active;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        oledFailureDetails_.clear();
                    }
                    nextOled = 0;
                } else {
                    const DWORD error = exchangeError;
                    const auto detail = ExchangeDetails(link, failedCommand, reply, replySize, error);
                    oledError_ = error;
                    oledState_ = OledState::Failed;
                    nextOled = GetTickCount64() + 5000;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        oledFailureDetails_ = detail;
                    }
                    LUMA_WARN("ROG Azoth OLED: %s", detail.c_str());
                    if (link == azoth::Link::Wireless) nextHealth = 0;
                    if (error != ERROR_TIMEOUT && error != ERROR_NOT_SUPPORTED) {
                        // Reopen after an unplug even when the RGB output is disabled.
                        closeDevice();
                        nextFind = now + 2000;
                        if (own) state_ = State::NotFound;
                        oledState_ = OledState::NotFound;
                        continue;
                    }
                }
            }
        }
        if (liveMusic && appliedRevision == revision && (musicRevision != revision || now >= nextMusic)) {
            const bool spectrum = oled.content == azoth::OledContent::Equalizer;
            const bool setup = musicRevision != revision || musicNeedsSetup;
            // The progress bar and clock change once a second while a song plays.
            const bool trackChanged = !songSent || media.title != lastSongTitle || media.artist != lastSongArtist ||
                                      media.positionMs / 1000 != lastSongSecond || media.durationMs / 1000 != lastSongLength;
            azoth::Report reply{}, failedCommand{};
            DWORD replySize = 0, error = ERROR_SUCCESS;
            bool superseded = false;
            auto exchange = [&](const azoth::Report& command) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    superseded = stop_ || asleep_ || revision != oledRevision_;
                }
                if (superseded) return false;
                if (OledExchange(dev, command, link, reply, &replySize, 1000)) return true;
                failedCommand = command; error = GetLastError(); return false;
            };
            bool ok = true;
            if (spectrum) {
                if (setup) ok = exchange(azoth::OledMusicMode(true));
                // ASUS sends spectrum updates without waiting for an acknowledgment.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    superseded = stop_ || asleep_ || revision != oledRevision_;
                }
                if (ok && !superseded) {
                    failedCommand = azoth::OledSpectrum(media.levels);
                    ok = WriteReport(dev, failedCommand, link);
                    if (!ok) error = GetLastError();
                }
            } else if (setup || trackChanged) {
                const auto pixels = azoth::RenderSong(media);
                const auto data = azoth::OledSongPixels(pixels);
                if (data.empty()) { ok = false; error = ERROR_NOT_ENOUGH_MEMORY; }
                else ok = exchange(azoth::OledMusicMode(false, data.size()));
                for (size_t i = 0; ok && i < (data.size() + 59) / 60; ++i)
                    ok = exchange(azoth::OledSongPart(data, i));
                if (ok) {
                    lastSongTitle = media.title; lastSongArtist = media.artist; songSent = true;
                    lastSongSecond = media.positionMs / 1000; lastSongLength = media.durationMs / 1000;
                }
            }
            if (superseded) { musicRevision = UINT64_MAX; continue; }
            if (ok) {
                musicRevision = revision;
                musicNeedsSetup = false;
                uploadedEffect_ = -1;
                nextMusic = GetTickCount64() + (spectrum ? 100 : 500);
                oledState_ = OledState::Active;
                oledError_ = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                oledFailureDetails_.clear();
            } else {
                // Back off after a rejected live mode; ordinary RGB still continues.
                musicRevision = revision;
                musicNeedsSetup = true;
                nextMusic = GetTickCount64() + 5000;
                songSent = false;
                oledState_ = OledState::Failed;
                oledError_ = error;
                const auto details = ExchangeDetails(link, failedCommand, reply, replySize, error);
                std::lock_guard<std::mutex> lock(mutex_);
                oledFailureDetails_ = details;
                LUMA_WARN("ROG Azoth music: %s", details.c_str());
            }
        }
        if (!own) continue;
        const double t = fx::Seconds(effect, now, since);
        auto lost = [&] {
            const DWORD err = GetLastError();
            LUMA_WARN("ROG Azoth: write failed (error %lu) - unplugged?", err);
            lastWriteError_ = err;
            closeDevice();
            rescan_ = true;
            nextFind = now + 2000;
            connection_ = azoth::SelectConnection(!devices.wired.empty(), !devices.receiver.empty(), false, false);
            state_ = State::NotFound;
            if (oled.direct) oledState_ = OledState::NotFound;
            lastKeys.clear();
        };
        // Every key its own color, by cable or through the Omni receiver. Over the receiver at
        // most 10 updates a second, sparing the 2.4 GHz link and the keyboard's battery.
        if (link == azoth::Link::Wireless && lastSent && now - lastSent < kWirelessFrameMs) continue;
        const std::vector<Rgb> keys = azoth::RenderKeys(effect, t, brightness);
        const bool refresh = !lastSent || now - lastSent >= kRefreshMs;  // everything again now and then
        if (keys == lastKeys && !refresh) {
            state_ = State::Active;
            continue;
        }
        std::vector<azoth::KeyColor> kc;
        const auto& layout = azoth::IsoKeys();
        for (size_t i = 0; i < layout.size(); ++i)
            if (lastKeys.empty() || refresh || keys[i] != lastKeys[i])
                kc.push_back({static_cast<uint8_t>(layout[i].led), keys[i]});
        bool ok = true;
        for (const azoth::Report& r : azoth::KeyColors(kc, link)) {
            if (!WriteReport(dev, r, link)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            lost();
            continue;
        }
        lastKeys = keys;
        lastSent = now;
        state_ = State::Active;
    }
    closeDevice();
}

}  // namespace luma::app
