// coreProps.json: the file SteelSeries GG writes so games can find its GameSense server,
// e.g. {"address":"127.0.0.1:51248","encrypted_address":"127.0.0.1:51249"}.
// LumaBridge writes its own copy (marked "lumabridge": true) and forwards to GG's address.
// Pure C++, unit tested.
#pragma once

#include <cstdlib>
#include <string>

#include "json.h"

namespace luma::gamesense {

struct CoreProps {
    bool valid = false;
    bool ours = false;  // written by LumaBridge
    std::string host;   // "127.0.0.1"
    int port = 0;
};

inline CoreProps ParseCoreProps(const std::string& text) {
    CoreProps p;
    Json j;
    if (!Json::Parse(text, &j) || !j.IsObject()) return p;
    p.ours = j["lumabridge"].Bool(false);
    const std::string& addr = j["address"].String();
    const size_t colon = addr.rfind(':');
    if (colon == std::string::npos || colon == 0) return p;
    p.host = addr.substr(0, colon);
    char* end = nullptr;
    long port = std::strtol(addr.c_str() + colon + 1, &end, 10);
    if (*end != '\0' || port <= 0 || port > 65535) return p;
    p.port = static_cast<int>(port);
    p.valid = true;
    return p;
}

// Only loopback targets are forwarded to: coreProps.json is writable by users, and
// LumaBridge must not be turned into a relay to another machine.
inline bool IsLoopbackHost(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

}  // namespace luma::gamesense
