// Lifecycle contract for the Amalgam account services that Essentials relies
// on: they start on sign-in, stop promptly on sign-out, expose no fixture data
// in the normal path, and fail closed when no backend is configured.
#include "essentials_manager.h"
#include "essentials_session.h"
#include "supabase.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace aml::essentials;

namespace {

template <typename Fn>
bool completes_within(int seconds, Fn fn) {
    const auto start = std::chrono::steady_clock::now();
    fn();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    return elapsed <= seconds;
}

bool test_services_start_and_stop_promptly() {
    // No Supabase backend is configured in this process, so every network call
    // is a no-op; the managers must still behave correctly and shut down fast.
    auto& friends = FriendsManager::instance();
    auto& presence = PresenceManager::instance();
    auto& invites = InviteManager::instance();
    auto& sessions = SessionManager::instance();

    assert(friends.initialize());
    assert(friends.initialize());  // idempotent
    assert(presence.initialize());
    assert(presence.initialize());
    assert(invites.initialize());
    assert(invites.initialize());
    assert(sessions.initialize());

    // Let every worker thread reach its polling sleep, so shutdown has to
    // interrupt a sleep already in progress rather than exiting before it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Shutdown must not wait out a polling interval (60s friends, 30s presence,
    // 10s invites).
    if (!completes_within(3, [&] {
            invites.shutdown();
            sessions.shutdown();
            presence.shutdown();
            friends.shutdown();
        })) {
        std::printf("FAIL: shutdown blocked on a polling interval\n");
        return false;
    }

    // Stopping an already-stopped service stays a no-op.
    invites.shutdown();
    presence.shutdown();
    friends.shutdown();
    return true;
}

bool test_no_fixture_state_in_normal_path() {
    // The real path starts empty until the backend answers. Fixture friends and
    // notifications are seeded only by the visual-review snapshot mode.
    auto& friends = FriendsManager::instance();
    assert(friends.get_friends().empty());
    assert(friends.get_pending_requests().empty());
    assert(friends.get_blocked_users().empty());
    assert(SessionManager::instance().get_notifications().empty());
    return true;
}

bool test_backend_calls_are_safe_without_configuration() {
    // Unconfigured Supabase must fail closed instead of dereferencing a null client.
    auto& supabase = aml::supabase::SupabaseManager::instance();
    assert(!supabase.is_authenticated());
    assert(supabase.get_friends().empty());
    assert(supabase.get_friend_requests().empty());
    assert(supabase.get_friends_presence().empty());
    assert(!supabase.update_presence("offline"));

    // The account entry points must name the missing configuration. Returning an
    // empty failure leaves the sign-in form reporting an empty string, which
    // reads to the user as a wrong email or password; humanize_error() maps this
    // wording to the account-services-not-configured sentence instead.
    const auto sign_in = supabase.sign_in("someone@example.com", "correct-horse-battery");
    const auto sign_up = supabase.sign_up("someone@example.com", "correct-horse-battery", {});
    const auto reset = supabase.request_password_reset("someone@example.com");
    for (const auto* response : {&sign_in, &sign_up, &reset}) {
        if (response->success ||
            response->error.find("Supabase") == std::string::npos ||
            response->error.find("not initialized") == std::string::npos) {
            std::printf("FAIL: unconfigured account call did not name the missing backend: '%s'\n",
                        response->error.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    if (!test_services_start_and_stop_promptly()) return 1;
    if (!test_no_fixture_state_in_normal_path()) return 1;
    if (!test_backend_calls_are_safe_without_configuration()) return 1;
    return 0;
}
