// Window, input and main loop (PandaFramework), plus headless mode.
#pragma once

#include "app/Options.h"

namespace arena {

// Exit codes, so scripts and tests can tell what happened.
enum ExitCode {
    kExitOk = 0,
    kExitInitFailed = 1,
    kExitNeverJoined = 2,
    kExitExpectationFailed = 3,
    kExitRejected = 4,
};

int runApp(Options options, int argc, char **argv);

} // namespace arena
