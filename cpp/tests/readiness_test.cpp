#include "readiness.h"

#include <iostream>

namespace {

bool test_java_blockers() {
    aml::readiness::Report report;
    report.checks.push_back({"bridge", "Bridge", "ready", aml::readiness::State::Ready, true});
    report.checks.push_back({"bedrock", "Bedrock", "optional", aml::readiness::State::Optional, false});
    if (!report.java_play_ready() || report.attention_count() != 0) return false;
    report.checks.push_back({"account", "Account", "sign in", aml::readiness::State::Attention, true});
    return !report.java_play_ready() && report.attention_count() == 1;
}

bool test_state_names() {
    return std::string(aml::readiness::state_name(aml::readiness::State::Ready)) == "READY" &&
           std::string(aml::readiness::state_name(aml::readiness::State::Attention)) == "ACTION" &&
           std::string(aml::readiness::state_name(aml::readiness::State::Optional)) == "OPTIONAL";
}

}  // namespace

int main() {
    if (!test_java_blockers() || !test_state_names()) {
        std::cerr << "FAILED: readiness\n";
        return 1;
    }
    return 0;
}
