// Reading one of G HUB's settings over its local WebSocket (127.0.0.1:9010, sub-protocol
// "json", the connection G HUB's own window uses; see docs/peripherals.md). Read-only.
#pragma once

#include <optional>

namespace luma::app {

// G HUB's "turn off lighting on inactivity" (GET /lighting/turn_off_for_inactivity); nullopt
// when G HUB isn't running or didn't answer. Blocks up to ~3 s: call it in the background.
std::optional<bool> GHubTurnsOffOnInactivity();

}  // namespace luma::app
