#pragma once

#include "mods.h"

#include <string>
#include <vector>

namespace aml::readiness {

enum class State {
    Ready,
    Attention,
    Optional,
};

struct Check {
    std::string id;
    std::string label;
    std::string detail;
    State state = State::Optional;
    // A false value means the item is useful or edition-specific, but does not
    // stop a signed-in player from starting Java Edition.
    bool blocks_java = false;
};

struct Report {
    std::vector<Check> checks;

    bool java_play_ready() const {
        for (const auto& check : checks) {
            if (check.blocks_java && check.state == State::Attention) return false;
        }
        return true;
    }

    int attention_count() const {
        int count = 0;
        for (const auto& check : checks) {
            if (check.state == State::Attention) ++count;
        }
        return count;
    }
};

struct Options {
    std::wstring launcher_dir;
    std::wstring java_cache_dir;
    mods::ApiCfg provider_api;
    // Provider requests are only made when the caller explicitly asks for a
    // live check. The normal report stays local and does not consume APIs.
    bool verify_providers = false;
};

inline const char* state_name(State state) {
    switch (state) {
        case State::Ready: return "READY";
        case State::Attention: return "ACTION";
        case State::Optional: return "OPTIONAL";
    }
    return "OPTIONAL";
}

Report run(const Options& options);

}  // namespace aml::readiness
