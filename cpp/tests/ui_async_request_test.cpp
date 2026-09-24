#include "ui_async_request.h"
#include "account_passive_cache.h"
#include "publish_async_scope.h"

#include <cassert>

int main() {
    using namespace aml::ui;

    AsyncUiRequestState request;
    uint64_t first = 0;
    assert(begin_async_ui_request(request, "friend-request", &first));
    assert(first == 1);
    assert(!begin_async_ui_request(request, "other", nullptr));

    auto pending = snapshot_async_ui_request(request);
    assert(pending.working);
    assert(pending.action == "friend-request");
    assert(!pending.has_result);

    // An obsolete worker cannot publish or clear the current request.
    AsyncUiRequestResult stale;
    stale.success = true;
    stale.title = "Stale";
    complete_async_ui_request(request, "friend-request", first + 1, stale);
    assert(snapshot_async_ui_request(request).working);
    assert(!snapshot_async_ui_request(request).has_result);

    AsyncUiRequestResult failure;
    failure.success = false;
    failure.title = "Request failed";
    failure.detail = "Try again";
    complete_async_ui_request(request, "friend-request", first, failure);

    auto completed = snapshot_async_ui_request(request);
    assert(!completed.working);
    assert(completed.has_result);
    assert(!completed.result.success);
    assert(completed.result.detail == "Try again");
    assert(async_ui_request_is_reserved(request));

    AsyncUiRequestSnapshot delivered;
    assert(take_async_ui_request_result(request, &delivered));
    assert(delivered.result.title == "Request failed");
    assert(!take_async_ui_request_result(request, nullptr));
    // Delivery only suppresses duplicate notices; it retains the error for an
    // inline retry state until the user starts a new request.
    assert(snapshot_async_ui_request(request).has_result);
    assert(!async_ui_request_is_reserved(request));

    uint64_t second = 0;
    assert(begin_async_ui_request(request, "friend-request", &second));
    assert(second == 2);
    assert(!snapshot_async_ui_request(request).has_result);
    assert(async_ui_request_is_reserved(request));

    invalidate_async_ui_request(request);
    assert(snapshot_async_ui_request(request).working);
    AsyncUiRequestResult obsolete;
    obsolete.success = true;
    complete_async_ui_request(request, "friend-request", second, obsolete);
    const auto invalidated = snapshot_async_ui_request(request);
    assert(!invalidated.has_result);
    assert(invalidated.action.empty());
    assert(!invalidated.working);
    assert(!async_ui_request_is_reserved(request));

    uint64_t retry = 0;
    assert(begin_async_ui_request(request, "friend-request", &retry));
    assert(retry == 4);

    // A local-only passive lane is independent of the serialized auth lane:
    // its completion must never replace an account-mutation result.
    AsyncUiRequestState auth_lane;
    AsyncUiRequestState stats_lane;
    uint64_t auth_generation = 0;
    uint64_t stats_generation = 0;
    assert(begin_async_ui_request(auth_lane, "account-password-change",
                                  &auth_generation));
    assert(begin_async_ui_request(stats_lane, "account-passive-stats",
                                  &stats_generation));
    AsyncUiRequestResult local_stats;
    local_stats.success = true;
    local_stats.number_a = 3;
    complete_async_ui_request(stats_lane, "account-passive-stats",
                              stats_generation, local_stats);
    assert(snapshot_async_ui_request(auth_lane).working);
    assert(snapshot_async_ui_request(auth_lane).action == "account-password-change");
    assert(snapshot_async_ui_request(stats_lane).has_result);

    // Passive account values are account-scoped rather than process-scoped.
    // A new user OR a new credential generation must discard the old cache so
    // a late worker can never expose the prior account's security/activity.
    AccountPassiveCache passive;
    assert(passive.bind("amalgam:alpha", 7));
    passive.stats_state.attempted = true;
    passive.stats_state.has_value = true;
    passive.stats.total_sessions = 3;
    passive.stats.total_instances = 2;
    passive.stats.total_modpacks = 1;
    passive.security_state.has_value = true;
    passive.security.two_factor_enabled = true;
    passive.activity_state.has_value = true;
    passive.activity.push_back({"event-a", "login", "Signed in", 42});

    assert(!passive.bind("amalgam:alpha", 7));
    assert(passive.stats.total_sessions == 3);
    assert(account_passive_scope_matches(passive.scope, "amalgam:alpha", 7));

    assert(passive.bind("amalgam:bravo", 7));
    assert(!passive.stats_state.attempted);
    assert(!passive.stats_state.has_value);
    assert(passive.stats.total_instances == 0);
    assert(passive.stats.total_modpacks == 0);
    assert(!passive.security_state.has_value);
    assert(passive.activity.empty());
    assert(!account_passive_scope_matches(passive.scope, "amalgam:alpha", 7));

    passive.stats_state.has_value = true;
    passive.stats.total_sessions = 4;
    assert(passive.bind("amalgam:bravo", 8));
    assert(!passive.stats_state.has_value);
    assert(passive.stats.total_sessions == 0);
    assert(account_passive_scope_matches(passive.scope, "amalgam:bravo", 8));

    // Stateful publishing uses the same account + session generation
    // guard, and also refuses to apply a completion to a different project.
    PublishAsyncScope publish_scope{"amalgam:alpha", 11, "project-alpha"};
    assert(publish_async_identity_matches(publish_scope, "amalgam:alpha", 11));
    assert(!publish_async_identity_matches(publish_scope, "amalgam:bravo", 11));
    assert(!publish_async_identity_matches(publish_scope, "amalgam:alpha", 12));
    assert(publish_async_scope_matches(publish_scope, "amalgam:alpha", 11,
                                       "project-alpha"));
    assert(!publish_async_scope_matches(publish_scope, "amalgam:alpha", 11,
                                        "project-bravo"));
    PublishAsyncScope create_scope{"amalgam:alpha", 11, ""};
    assert(publish_async_scope_matches(create_scope, "amalgam:alpha", 11, ""));
    assert(!publish_async_scope_matches(create_scope, "amalgam:alpha", 11,
                                        "project-alpha"));

    // Signing out clears both the bound identity and every visible value.
    passive.security_state.has_value = true;
    passive.activity_state.has_value = true;
    passive.clear();
    assert(passive.scope.account_identity.empty());
    assert(!passive.stats_state.has_value);
    assert(!passive.security_state.has_value);
    assert(!passive.activity_state.has_value);
    return 0;
}
