// The notification center: what on this PC needs the user's attention, as a list built from plain
// facts (so the rules are testable). The UI gathers the facts, shows the list under the bell in the
// header, and lets the user dismiss a notice (it comes back if the problem goes away and returns).
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace luma::app::notify {

enum class Severity { Info, Warning, Error };

// Where "Open" goes. ReplaceRuntime is an action instead: it replaces the vendor's runtime for
// the integration in `arg` (as on the Integrations page), then opens that page for the result.
enum class Where { None, Lighting, Devices, DevicesHardware, DevicesController, Integrations, GamesList, Settings, ReplaceRuntime };

struct Notice {
    std::string id;  // stable: the same problem has the same id every time
    Severity severity = Severity::Info;
    std::string title, detail;
    Where where = Where::None;
    std::string arg;  // Where::GamesList: the game's name, to open its page; ReplaceRuntime: the integration
    std::string argKey;  // ... and its profile key
    std::string action;  // button label, "" = "Open"
};

struct IntegrationFact {
    std::string id, name, detail;
    bool problem = false;  // built in but not working
    bool vendorPresent = false;  // the vendor's own runtime is installed where LumaBridge's would go
};

struct GameFact {
    std::string key, title;
    bool running = false;
    bool portBusy = false;  // its telemetry port is taken by another program
    int port = 0;
    bool silent = false;    // running for a while, yet nothing arrives from its feed
    bool blocked = false;   // its anti-cheat keeps LumaBridge out of the game
};

struct Facts {
    bool pausedAfterCrash = false, pausedByUser = false;
    bool asusBoard = false, auraRunning = false, auraConnected = false;
    bool sensorsMissing = false;  // fan speeds, temperatures and power need Hardware access
    int windowsProblems = 0;      // devices Windows reports a problem for
    std::vector<IntegrationFact> integrations;
    bool padConnected = false;
    int padBattery = -1;
    bool padCharging = false;
    std::string padName = "Controller";
    bool dualsenseWriteFailed = false;
    std::vector<GameFact> games;
};

inline std::vector<Notice> Collect(const Facts& f) {
    std::vector<Notice> out;
    auto add = [&](std::string id, Severity s, std::string title, std::string detail, Where w, std::string action = {}) {
        Notice n;
        n.id = std::move(id);
        n.severity = s;
        n.title = std::move(title);
        n.detail = std::move(detail);
        n.where = w;
        n.action = std::move(action);
        out.push_back(std::move(n));
        return &out.back();
    };

    if (f.pausedAfterCrash)
        add("paused-crash", Severity::Warning, "Lighting control is paused",
            "LumaBridge closed unexpectedly the last time it controlled your lights, so it is leaving them alone. Resume "
            "when you are ready.",
            Where::Devices);
    if (f.asusBoard && f.auraRunning && !f.auraConnected && !f.pausedAfterCrash && !f.pausedByUser)
        add("aura-missing", Severity::Warning, "The Aura controller was not found",
            "Armoury Crate or LightingService may be busy. Other lighting devices can still work. Rescan on the Devices "
            "page.",
            Where::Devices);
    for (const auto& i : f.integrations)
        if (i.problem)
            add("integration-" + i.id, Severity::Error, i.name + " needs attention",
                i.detail.empty() ? "It is built in but not working." : i.detail, Where::Integrations);
    for (const auto& i : f.integrations)
        if (i.vendorPresent && !i.problem)
            add("vendor-" + i.id, Severity::Info, i.name + ": vendor software present",
                "Its maker's own runtime is installed, so games using it talk to that instead of LumaBridge. Replacing it "
                "backs up the original (Remove on the Integrations page puts it back) and needs administrator approval.",
                Where::ReplaceRuntime, "Replace vendor runtime")->arg = i.id;
    if (f.windowsProblems > 0)
        add("windows-problems", Severity::Warning,
            std::to_string(f.windowsProblems) + (f.windowsProblems == 1 ? " device has a Windows problem" : " devices have a Windows problem"),
            "Windows reports a problem code for them. See the All hardware list, and Device Manager for the cause.",
            Where::DevicesHardware);
    if (f.padConnected && f.padBattery >= 0 && f.padBattery <= 20 && !f.padCharging)
        add("pad-battery", f.padBattery <= 10 ? Severity::Error : Severity::Warning,
            f.padName + " battery is low (" + std::to_string(f.padBattery) + "%)", "Charge it soon to avoid losing it mid-game.",
            Where::DevicesController);
    if (f.dualsenseWriteFailed)
        add("dualsense-write", Severity::Warning, "The DualSense lightbar stopped answering",
            "It was connected, then a write failed: unplugged, or out of Bluetooth range. LumaBridge keeps trying.",
            Where::DevicesController);
    for (const auto& g : f.games) {
        if (!g.running && !g.portBusy) continue;
        if (g.portBusy)
            add("port-" + g.key, Severity::Warning, g.title + ": port " + std::to_string(g.port) + " is taken",
                "Another program (a telemetry app?) uses it. Pick another port on the game's page and in the game.",
                Where::GamesList, "Open game")->arg = g.title;
        else if (g.running && g.silent)
            add("silent-" + g.key, Severity::Info, g.title + " is running, but sends nothing yet",
                "Its telemetry or lighting feed may be off. The game's page says how to switch it on.", Where::GamesList,
                "Open game")->arg = g.title;
        if (g.running && g.blocked)
            add("blocked-" + g.key, Severity::Info, g.title + " keeps LumaBridge out",
                "Its anti-cheat blocks LumaBridge's lighting. Other devices show your idle choice, or the screen's colors "
                "if you pick them on the game's page.",
                Where::GamesList, "Open game")->arg = g.title;
    }
    if (f.sensorsMissing)
        add("sensors", Severity::Info, "Fan speeds and temperatures are not available",
            "They need Hardware access (the helper or LibreHardwareMonitor). Set it up on the Devices page.", Where::Devices);

    // Errors first, then warnings, then notes; the order of the rules above within each.
    std::stable_sort(out.begin(), out.end(),
                     [](const Notice& a, const Notice& b) { return static_cast<int>(a.severity) > static_cast<int>(b.severity); });
    return out;
}

// Fills `arg` with the game's profile key as well, so the UI can open its page.
inline void WithGameKeys(std::vector<Notice>* notices, const std::vector<GameFact>& games) {
    for (auto& n : *notices)
        for (const auto& g : games)
            if (n.where == Where::GamesList && n.arg == g.title) n.argKey = g.key;
}

// The notices still shown: the dismissed ones drop out, and a dismissal is forgotten once its
// notice is gone (so the problem returning is noticed again). Updates `dismissed`.
inline std::vector<Notice> Visible(const std::vector<Notice>& all, std::vector<std::string>* dismissed) {
    dismissed->erase(std::remove_if(dismissed->begin(), dismissed->end(),
                                    [&](const std::string& id) {
                                        return std::none_of(all.begin(), all.end(), [&](const Notice& n) { return n.id == id; });
                                    }),
                     dismissed->end());
    std::vector<Notice> out;
    for (const auto& n : all)
        if (std::find(dismissed->begin(), dismissed->end(), n.id) == dismissed->end()) out.push_back(n);
    return out;
}

// The most serious level in the list (Info when empty), for the bell's badge color.
inline Severity Worst(const std::vector<Notice>& notices) {
    Severity s = Severity::Info;
    for (const auto& n : notices)
        if (static_cast<int>(n.severity) > static_cast<int>(s)) s = n.severity;
    return s;
}

// The bell's badge: the count, with "9+" past nine so it stays one small circle.
inline std::string BadgeText(size_t count) { return count > 9 ? "9+" : std::to_string(count); }

// One line under the list's heading, e.g. "1 problem, 2 warnings, 1 note".
inline std::string Summary(const std::vector<Notice>& notices) {
    int counts[3] = {0, 0, 0};  // Info, Warning, Error
    for (const auto& n : notices) ++counts[static_cast<int>(n.severity)];
    std::string out;
    auto part = [&](int count, const char* one, const char* many) {
        if (count == 0) return;
        if (!out.empty()) out += ", ";
        out += std::to_string(count) + " " + (count == 1 ? one : many);
    };
    part(counts[2], "problem", "problems");
    part(counts[1], "warning", "warnings");
    part(counts[0], "note", "notes");
    return out;
}

}  // namespace luma::app::notify
