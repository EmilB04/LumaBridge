// Nollie ARGB hubs, lit directly over USB HID (nollie_protocol.h): each channel with the
// effect of its own look, as a strip or as a chain of fans (nollie_layout.h). Only colors are
// sent; a hub with the OS2 firmware is also asked once what it keeps on its own, for the log
// (nollie_onboard.h), and can write the hub's own boot effect on request. When LumaBridge lets
// go it stops sending, and the hub keeps showing the last frame until it loses power.
// Runs on its own thread; looks for new hubs every 15 seconds.
#pragma once
#include "delivery_receipt.h"
#include "worker_cancellation.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "effects.h"
#include "nollie_hub.h"
#include "nollie_layout.h"
#include "nollie_onboard.h"
#include "nollie_sequence.h"

namespace luma::app {

struct NollieDevice {
    std::string id;       // lighting::Identity of the HID path: stays the same for the same port
    std::string name;     // "Nollie 8CH"
    uint16_t vid = 0, pid = 0;
    int channels = 0, maxLeds = 0;
    std::string problem;  // why it can't be lit ("" when it can)
    double fps = 0;       // frames sent in the last second (only changed frames are sent)
    std::string path;
};

// What one part of a channel (a fan on its own, or the whole channel) should show.
struct NollieSegmentPlan {
    fx::Params effect;
    double brightness = 1.0;
    // The boot effect on this part, and where the part sits along the travel through the hub
    // (0..1, over NollieChannelPlan::bootTravelSeconds).
    fx::Params bootEffect;
    double bootOffset = 0;
    // Smooth travel: how far the part's last LED runs behind its first, in seconds
    // (nollie::Segment::lag), for the effect and the boot effect.
    double lag = 0, bootLag = 0;
};

// Per hub: how long its boot effect plays (0: none) and how often frames go out.
struct NollieHubPlan {
    double bootSeconds = 0;
    int refreshHz = 60;
};

// What one channel should show right now.
struct NollieChannelPlan {
    nollie::Channel channel;
    std::vector<NollieSegmentPlan> segments;  // nollie::Channel::Segments() of them
    bool test = false;  // the test pattern instead of the effect
    bool lit = true;    // false: handed back to the hub's own app, nothing sent
    double bootTravelSeconds = 0;  // 0: the boot effect doesn't travel
    // A sequence across the hub's parts instead of the effect (kind None: the effect). The
    // channel's first part is step `firstPart` of `parts`; `reverse` fills its rings the other way.
    nollie::SequenceParams sequence;
    int firstPart = 0, parts = 1;
    bool reverse = false;
    bool backward = false;  // the sequence goes last to first: this channel's fans count down
    // A gradient along the hub in place of the effect's main color; the channel's first part is
    // step `gradientFirst` of `parts` in channel order.
    nollie::Gradient gradient;
    int gradientFirst = 0;
};

// The boot effect the hub keeps on its own (nollie_onboard.h), to write to one hub.
struct NollieBootSave {
    std::string deviceId;
    nollie::onboard::BootPlan plan;
    std::vector<int> ledCounts;  // by zone: LEDs on the channel, 0 for a channel with nothing on it
};

class NollieOutput {
public:
    enum class State { Off, Running };

    ~NollieOutput() { Stop(); }
    void Start();
    void RequestStop() { stop_ = true; cancellation_.Cancel(); }
    uint64_t delivered() const { return delivery_.value(); }
    void Stop();

    // `plans`: the channels by nollie::Key(deviceId, zone); a channel with no plan stays dark.
    // `own = false`: nothing is sent. `skip`: identities of hubs LumaBridge leaves alone.
    // `hubs`: per hub (identity), its boot effect and refresh rate; a hub not listed: no boot
    // effect, 60 frames a second.
    // `correction`: the color calibration applied to every LED (its brightness is ignored).
    void Set(bool own, const std::map<std::string, NollieChannelPlan>& plans, const std::vector<std::string>& skip,
             const std::map<std::string, NollieHubPlan>& hubs, const ColorCorrection& correction = {});
    // Plays the boot effect of this hub (identity) now.
    void PlayBoot(const std::string& deviceId);
    // Writes the boot effect to the hub itself (on the output's thread). The outcome, in words
    // for the user, is in bootResult() ("" before the first save, "Saving..." meanwhile).
    void SaveBoot(const NollieBootSave& request);
    std::string bootResult(const std::string& deviceId) const;
    // Plays the boot effect the hub holds (not unsaved changes): the colors stop for a few
    // seconds while it plays. The outcome is in bootResult().
    void PlayHubBoot(const std::string& deviceId);
    // Starts the hub's sequence over.
    void RestartSequence(const std::string& deviceId);

    State state() const { return state_; }
    std::vector<NollieDevice> devices() const;

private:
    luma::DeliveryReceipt delivery_;
    void Run();

    WorkerCancellation cancellation_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<State> state_{State::Off};
    mutable std::mutex mutex_;
    bool own_ = false;
    std::map<std::string, NollieChannelPlan> plans_;
    std::vector<std::string> skip_;
    std::map<std::string, NollieHubPlan> hubs_;
    ColorCorrection correction_;
    std::vector<std::string> replay_, restart_;
    std::vector<NollieBootSave> saves_;
    std::vector<std::string> hubPlays_;
    std::map<std::string, std::string> bootResults_;
    std::vector<NollieDevice> devices_;
};

}  // namespace luma::app
