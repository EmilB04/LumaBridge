#pragma once

#include <string_view>

namespace luma::app {

enum class IntegrationState { Active, NotInstalled, Conflict, PerGame, Problem };

// Game folders need an explicit choice. Hardware support must survive game-lighting off.
inline bool IncludeInIntegrationBatch(std::string_view id, IntegrationState state, bool enabled) {
    if (id == "corsair") return false;
    if (id == "gamesense")
        return enabled && (state == IntegrationState::Problem || state == IntegrationState::NotInstalled);
    const bool support = id == "helper" || id == "handback";
    return enabled ? state != IntegrationState::Active : !support && state == IntegrationState::Active;
}

}  // namespace luma::app
