#pragma once

#include <cstdint>
#include <string>

namespace aml::ui {

// Publish requests may finish after the active Amalgam account, session
// generation, or selected project has changed. Workers receive only this
// copied scope; the render thread must validate it again before exposing a
// result or applying an identifier to the Publish form.
struct PublishAsyncScope {
    std::string account_identity;
    uint64_t session_generation = 0;
    std::string project_id;
};

inline bool publish_async_identity_matches(const PublishAsyncScope& scope,
                                           const std::string& account_identity,
                                           uint64_t session_generation) {
    return !scope.account_identity.empty() &&
           scope.account_identity == account_identity &&
           scope.session_generation == session_generation;
}

inline bool publish_async_scope_matches(const PublishAsyncScope& scope,
                                        const std::string& account_identity,
                                        uint64_t session_generation,
                                        const std::string& project_id) {
    return publish_async_identity_matches(scope, account_identity, session_generation) &&
           scope.project_id == project_id;
}

}  // namespace aml::ui
