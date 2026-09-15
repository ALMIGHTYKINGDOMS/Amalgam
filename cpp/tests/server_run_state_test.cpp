// The supervised process owns local run state. A stage restored from
// servers.json is only a hint, and one no process backs must never read as a
// server that is actually up (otherwise a restart shows a RUNNING badge, live
// metrics chrome and a Stop button for a server that is not there).
#include "server_types.h"

#include <cstdio>

using aml::server::reconciled_stage;
using aml::server::ServerConfig;
using aml::server::ServerStage;

namespace {

int failures = 0;

void expect(ServerStage persisted, bool supervised, ServerStage expected) {
    ServerConfig server;
    server.stage = persisted;
    const ServerStage actual = reconciled_stage(server, supervised);
    if (actual != expected) {
        std::printf("FAIL: persisted stage %d supervised=%d read as %d, expected %d\n",
                    static_cast<int>(persisted), supervised ? 1 : 0,
                    static_cast<int>(actual), static_cast<int>(expected));
        ++failures;
    }
}

}  // namespace

int main() {
    // The restart case: a server that was running when the launcher closed.
    expect(ServerStage::Running, false, ServerStage::Stopped);
    // A supervised process that is still up keeps reading as running.
    expect(ServerStage::Running, true, ServerStage::Running);
    // Every other stage passes through untouched, supervised or not.
    for (const bool supervised : {false, true}) {
        expect(ServerStage::NotInstalled, supervised, ServerStage::NotInstalled);
        expect(ServerStage::Installing, supervised, ServerStage::Installing);
        expect(ServerStage::Ready, supervised, ServerStage::Ready);
        expect(ServerStage::Stopped, supervised, ServerStage::Stopped);
        expect(ServerStage::Error, supervised, ServerStage::Error);
        expect(ServerStage::Crashed, supervised, ServerStage::Crashed);
    }

    if (failures > 0) return 1;
    std::printf("server run state: reconciled stage contract verified\n");
    return 0;
}
