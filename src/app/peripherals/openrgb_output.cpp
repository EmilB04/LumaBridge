#include "openrgb_output.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>

#include "log.h"
#include "openrgb_protocol.h"

namespace luma::app {
namespace {

constexpr DWORD kFrameMs = 33;
constexpr uint64_t kRefreshMs = 5000;  // every LED again now and then
constexpr uint64_t kRetryMs = 5000;    // OpenRGB not running: look again

// One connection to OpenRGB's SDK server.
class Link {
public:
    ~Link() { Close(); }
    bool open() const { return s_ != INVALID_SOCKET; }

    bool Connect(uint16_t port) {
        Close();
        s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            Close();
            return false;
        }
        const BOOL noDelay = TRUE;
        setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);
        return true;
    }

    void Close() {
        if (open()) closesocket(s_);
        s_ = INVALID_SOCKET;
    }

    bool Send(uint32_t device, uint32_t id, const openrgb::Bytes& data = {}) {
        const openrgb::Bytes p = openrgb::Packet(device, id, data);
        size_t sent = 0;
        while (sent < p.size()) {
            const int n = send(s_, reinterpret_cast<const char*>(p.data() + sent), static_cast<int>(p.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Waits up to `ms` for a packet with this ID (others are skipped; a device list update is
    // noted). False when none came or the connection broke (see broken()).
    bool Receive(uint32_t id, openrgb::Bytes* data, DWORD ms) {
        const uint64_t until = GetTickCount64() + ms;
        for (;;) {
            const uint64_t now = GetTickCount64();
            if (now >= until) return false;
            openrgb::Header h;
            openrgb::Bytes d;
            if (!ReadPacket(&h, &d, static_cast<DWORD>(until - now))) return false;
            if (h.id == openrgb::kDeviceListUpdated) listChanged_ = true;
            if (h.id == id) {
                *data = std::move(d);
                return true;
            }
        }
    }

    // Packets the server sent on its own (device list updates), without waiting.
    void Poll() {
        openrgb::Header h;
        openrgb::Bytes d;
        while (Readable(0) && ReadPacket(&h, &d, 200))
            if (h.id == openrgb::kDeviceListUpdated) listChanged_ = true;
    }

    bool broken() const { return broken_; }
    bool TakeListChanged() {
        const bool c = listChanged_;
        listChanged_ = false;
        return c;
    }

private:
    bool Readable(DWORD ms) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s_, &r);
        timeval tv{static_cast<long>(ms / 1000), static_cast<long>((ms % 1000) * 1000)};
        return select(0, &r, nullptr, nullptr, &tv) > 0;
    }
    bool ReadExact(uint8_t* p, size_t n, DWORD ms) {
        const uint64_t until = GetTickCount64() + ms;
        size_t got = 0;
        while (got < n) {
            const uint64_t now = GetTickCount64();
            if (now >= until || !Readable(static_cast<DWORD>(until - now))) return false;
            const int k = recv(s_, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
            if (k <= 0) {
                broken_ = true;
                return false;
            }
            got += static_cast<size_t>(k);
        }
        return true;
    }
    bool ReadPacket(openrgb::Header* h, openrgb::Bytes* d, DWORD ms) {
        uint8_t head[openrgb::kHeaderSize];
        if (!ReadExact(head, sizeof head, ms)) return false;
        auto parsed = openrgb::ParseHeader(head, sizeof head);
        if (!parsed || parsed->size > (16u << 20)) {
            broken_ = true;  // out of step: start over
            return false;
        }
        *h = *parsed;
        d->assign(h->size, 0);
        // The data follows right away; give a big controller description time to arrive.
        return h->size == 0 || ReadExact(d->data(), d->size(), std::max<DWORD>(ms, 2000));
    }

    SOCKET s_ = INVALID_SOCKET;
    bool broken_ = false;
    bool listChanged_ = false;
};

}  // namespace

void OpenRgbOutput::Start(uint16_t port) {
    if (thread_.joinable()) return;
    port_ = port ? port : openrgb::kDefaultPort;
    stop_ = false;
    thread_ = std::thread(&OpenRgbOutput::Run, this);
}

void OpenRgbOutput::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    thread_.join();
    state_ = State::Off;
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
}

void OpenRgbOutput::Set(const fx::Params& effect, double brightness, bool own, const std::vector<std::string>& skip) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (effect.kind != effect_.kind || effect.speed != effect_.speed) effectSince_ = GetTickCount64();
    effect_ = effect;
    brightness_ = brightness;
    own_ = own;
    skip_ = skip;
}

std::vector<OpenRgbDevice> OpenRgbOutput::devices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

void OpenRgbOutput::Run() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    Link link;
    uint32_t version = 0;
    std::vector<openrgb::Controller> ctrls;
    std::vector<bool> custom;                 // LumaBridge has it in direct mode
    std::vector<std::vector<Rgb>> lastSent;   // per controller
    std::vector<uint64_t> sentAt;
    uint64_t nextTry = 0;
    bool loggedMissing = false;

    // Gives a device its own effect back (the mode it had before LumaBridge).
    auto release = [&](size_t i) {
        if (i < custom.size() && custom[i] && link.open() && ctrls[i].activeMode >= 0 && !ctrls[i].activeModeData.empty())
            link.Send(static_cast<uint32_t>(i), openrgb::kUpdateMode,
                      openrgb::UpdateMode(ctrls[i].activeMode, ctrls[i].activeModeData));
        if (i < custom.size()) custom[i] = false;
    };
    auto enumerate = [&] {
        ctrls.clear();
        openrgb::Bytes d;
        if (!link.Send(0, openrgb::kRequestControllerCount) || !link.Receive(openrgb::kRequestControllerCount, &d, 2000) ||
            d.size() < 4)
            return false;
        const uint32_t count = d[0] | d[1] << 8 | d[2] << 16 | static_cast<uint32_t>(d[3]) << 24;
        std::vector<OpenRgbDevice> list;
        for (uint32_t i = 0; i < count && i < 256; ++i) {
            openrgb::Controller c;
            if (link.Send(i, openrgb::kRequestControllerData, version ? openrgb::U32Data(version) : openrgb::Bytes{}) &&
                link.Receive(openrgb::kRequestControllerData, &d, 3000)) {
                if (auto parsed = openrgb::ParseController(d.data(), d.size(), version)) c = *parsed;
                else LUMA_WARN("OpenRGB: couldn't read device %u's description - left out", i);
            }
            if (link.broken()) return false;
            ctrls.push_back(c);  // keeps the indexes in step with OpenRGB's
            if (c.leds) list.push_back(OpenRgbDevice{c.name, c.vendor, c.type, c.leds});
            LUMA_INFO("OpenRGB device %u: %s (%s, %u LEDs)", i, c.name.c_str(), openrgb::TypeName(c.type), c.leds);
        }
        custom.assign(ctrls.size(), false);
        lastSent.assign(ctrls.size(), {});
        sentAt.assign(ctrls.size(), 0);
        std::lock_guard<std::mutex> lock(mutex_);
        devices_ = list;
        return true;
    };
    auto drop = [&] {
        link.Close();
        ctrls.clear();
        custom.clear();
        state_ = State::NotRunning;
        std::lock_guard<std::mutex> lock(mutex_);
        devices_.clear();
    };

    while (!stop_) {
        Sleep(kFrameMs);
        const uint64_t now = GetTickCount64();
        if (!link.open()) {
            if (now < nextTry) continue;
            nextTry = now + kRetryMs;
            if (!link.Connect(port_)) {
                if (!loggedMissing) LUMA_INFO("OpenRGB: not running (no SDK server on port %u)", port_);
                loggedMissing = true;
                state_ = State::NotRunning;
                continue;
            }
            // Protocol version: an old server doesn't answer (version 0).
            openrgb::Bytes d;
            version = 0;
            if (link.Send(0, openrgb::kRequestProtocolVersion, openrgb::U32Data(openrgb::kProtocolVersion)) &&
                link.Receive(openrgb::kRequestProtocolVersion, &d, 1000) && d.size() >= 4)
                version = std::min<uint32_t>(openrgb::kProtocolVersion, d[0] | d[1] << 8 | d[2] << 16);
            link.Send(0, openrgb::kSetClientName, openrgb::ClientName("LumaBridge"));
            if (!enumerate()) {
                LUMA_WARN("OpenRGB: connected, but couldn't list its devices");
                drop();
                continue;
            }
            LUMA_INFO("OpenRGB: connected (protocol %u), %d device(s)", version, static_cast<int>(ctrls.size()));
            loggedMissing = false;
            state_ = State::Connected;
        }

        link.Poll();
        if (link.broken()) {
            LUMA_INFO("OpenRGB: connection closed");
            drop();
            continue;
        }
        if (link.TakeListChanged()) {
            for (size_t i = 0; i < ctrls.size(); ++i) release(i);
            if (!enumerate()) {
                drop();
                continue;
            }
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
        const double t = fx::Seconds(effect, now, since);
        for (size_t i = 0; i < ctrls.size(); ++i) {
            const openrgb::Controller& c = ctrls[i];
            const bool wanted = own && c.leds && std::find(skip.begin(), skip.end(), c.name) == skip.end();
            if (!wanted) {
                release(i);
                continue;
            }
            // Each zone shows the effect along its LEDs (LEDs outside any zone: the last color).
            std::vector<Rgb> colors;
            colors.reserve(c.leds);
            for (const openrgb::Zone& z : c.zones)
                for (uint32_t k = 0; k < z.leds && colors.size() < c.leds; ++k)
                    colors.push_back(Scale(fx::Render(effect, t, static_cast<int>(k), static_cast<int>(z.leds)), level));
            while (colors.size() < c.leds) colors.push_back(colors.empty() ? Scale(fx::Render(effect, t, 0, 1), level) : colors.back());
            if (!custom[i]) {
                link.Send(static_cast<uint32_t>(i), openrgb::kSetCustomMode);  // direct control
                custom[i] = true;
                lastSent[i].clear();
            }
            if (colors == lastSent[i] && now - sentAt[i] < kRefreshMs) continue;
            if (!link.Send(static_cast<uint32_t>(i), openrgb::kUpdateLeds, openrgb::UpdateLeds(colors))) break;
            lastSent[i] = colors;
            sentAt[i] = now;
        }
    }
    for (size_t i = 0; i < ctrls.size(); ++i) release(i);
    link.Close();
    WSACleanup();
}

}  // namespace luma::app
