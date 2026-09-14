#include <cctype>

#include "essentials.h"
#include "server_types.h"

#include <cassert>
#include <string>

namespace {

bool test_friend_code() {
    using aml::essentials::generate_friend_code;
    // AMG-XXXX-XXXX format, uppercase, deterministic.
    const std::string code = generate_friend_code("1234abcd-ef56-7890");
    assert(code.size() == 13);
    assert(code.rfind("AMG-", 0) == 0);
    // "AMG-XXXX-XXXX" is 13 chars; the separators are at indices 3 and 8.
    assert(code[3] == '-' && code[8] == '-');
    assert(code == generate_friend_code("1234abcd-ef56-7890"));
    // Short ids are zero-padded to the minimum 8 chars.
    assert(generate_friend_code("ab") == "AMG-AB00-0000");
    return true;
}

bool test_essentials_enum_names() {
    using namespace aml::essentials;
    assert(std::string(session_privacy_name(SessionPrivacy::InviteOnly)) == "Invite Only");
    assert(std::string(session_privacy_name(SessionPrivacy::FriendsOfFriends)) == "Friends of Friends");
    assert(std::string(session_state_name(SessionState::Online)) == "Online");
    assert(std::string(session_state_name(SessionState::Crashed)) == "Crashed");
    assert(std::string(friend_status_name(FriendStatus::Playing)) == "Playing");
    assert(std::string(friend_status_name(FriendStatus::Away)) == "Away");
    assert(std::string(compat_level_name(CompatibilityLevel::Match)) == "Match");
    assert(std::string(compat_level_name(CompatibilityLevel::MajorMismatch)) == "Major Mismatch");
    assert(std::string(compat_level_name(CompatibilityLevel::Incompatible)) == "Incompatible");
    return true;
}

bool test_server_software() {
    using namespace aml::server;
    assert(std::string(server_software_name(ServerSoftware::Vanilla)) == "Vanilla");
    assert(std::string(server_software_name(ServerSoftware::Paper)) == "Paper");
    assert(std::string(server_software_name(ServerSoftware::BedrockDedicatedServer)) ==
           "Bedrock Dedicated Server");
    assert(is_modded_software(ServerSoftware::Fabric));
    assert(is_modded_software(ServerSoftware::NeoForge));
    assert(is_modded_software(ServerSoftware::Forge));
    assert(is_modded_software(ServerSoftware::Quilt));
    assert(!is_modded_software(ServerSoftware::Vanilla));
    assert(!is_modded_software(ServerSoftware::Paper));
    assert(!is_modded_software(ServerSoftware::BedrockDedicatedServer));
    return true;
}

}  // namespace

int main() {
    if (!test_friend_code() || !test_essentials_enum_names() || !test_server_software()) {
        return 1;
    }
    return 0;
}
