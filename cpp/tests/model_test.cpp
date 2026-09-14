#include "json.h"
#include "model.h"

#include <iostream>
#include <string>

namespace {

bool test_rank() {
    // rank = major*1000 + minor*10 + patch
    return aml::model::rank("1.21.11") == 1221 && aml::model::rank("1.12.2") == 1122 &&
           aml::model::rank("1.20.1") == 1201 && aml::model::rank("1.8") == 1080 &&
           aml::model::rank("1.7.10") == 1080 && aml::model::rank("bogus") == 0;
}

bool test_java_major() {
    return aml::model::default_java_major(1221) == 21 && aml::model::default_java_major(1204) == 17 &&
           aml::model::default_java_major(1182) == 17 && aml::model::default_java_major(1171) == 16 &&
           aml::model::default_java_major(1122) == 8;
}

bool test_rules_allow() {
    using J = aml::Json;
    J empty = J::arr();
    if (!aml::model::rules_allow(empty)) return false;
    J catch_all_allow = J::arr();
    J rule0 = J::obj();
    rule0.set("action", J::str("allow"));
    catch_all_allow.push(rule0);
    if (!aml::model::rules_allow(catch_all_allow)) return false;
    J linux_only = J::arr();
    J rule = J::obj();
    rule.set("action", J::str("allow"));
    J os = J::obj();
    os.set("name", J::str("linux"));
    rule.set("os", os);
    linux_only.push(rule);
    // windows does not match a linux-only rule -> default disallow
    if (aml::model::rules_allow(linux_only)) return false;
    J win = J::arr();
    J rule2 = J::obj();
    rule2.set("action", J::str("allow"));
    J os2 = J::obj();
    os2.set("name", J::str("windows"));
    rule2.set("os", os2);
    win.push(rule2);
    if (!aml::model::rules_allow(win)) return false;
    J feature_gated = J::arr();
    J rule3 = J::obj();
    rule3.set("action", J::str("allow"));
    J features = J::obj();
    features.set("is_demo_user", J::boolean(true));
    rule3.set("features", features);
    feature_gated.push(rule3);
    // features never apply to Amalgam launches -> no match -> disallow
    if (aml::model::rules_allow(feature_gated)) return false;
    J disallow_win = J::arr();
    J rule4 = J::obj();
    rule4.set("action", J::str("disallow"));
    rule4.set("os", os2);
    disallow_win.push(rule4);
    if (aml::model::rules_allow(disallow_win)) return false;
    return true;
}

bool test_parse_version_args() {
    using J = aml::Json;
    std::string err;
    J j = J::parse(R"({
        "id":"test",
        "mainClass":"net.minecraft.client.main.Main",
        "assets":"1.21",
        "arguments":{
            "game":[
                "--username","${auth_player_name}",
                {"rules":[{"action":"allow","features":{"is_demo_user":true}}],
                 "value":"--demo"},
                {"rules":[{"action":"allow"}],"value":["--quickPlayMultiplayer","srv"]}
            ],
            "jvm":[
                "-Dminecraft.launcher.brand=vanilla",
                {"rules":[{"action":"disallow"}],"value":"-Xmx2G"}
            ]
        }
    })", &err);
    aml::model::VersionJson vj;
    if (!aml::model::parse_version(j, vj)) return false;
    bool has_username = false, has_demo = false, has_quick = false;
    for (const std::string& a : vj.game_args) {
        if (a == "--username") has_username = true;
        if (a == "--demo") has_demo = true;
        if (a == "--quickPlayMultiplayer") has_quick = true;
    }
    bool has_brand = false, has_disallowed = false;
    for (const std::string& a : vj.jvm_args) {
        if (a == "-Dminecraft.launcher.brand=vanilla") has_brand = true;
        if (a == "-Xmx2G") has_disallowed = true;
    }
    return has_username && !has_demo && has_quick && has_brand && !has_disallowed;
}

}  // namespace

int main() {
    struct { const char* name; bool (*fn)(); } tests[] = {
        {"rank", test_rank},                 {"java_major", test_java_major},
        {"rules_allow", test_rules_allow},   {"parse_version_args", test_parse_version_args},
    };
    bool ok = true;
    for (const auto& t : tests) {
        if (!t.fn()) {
            std::cerr << "FAILED: " << t.name << "\n";
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
