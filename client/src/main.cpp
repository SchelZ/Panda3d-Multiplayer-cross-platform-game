// Panda Arena client entry point.
#include "app/App.h"
#include "app/Options.h"

#include <cstdio>
#include <string>

int main(int argc, char **argv) {
    arena::Options options;
    std::string error;
    bool showHelp = false;
    if (!options.parse(argc, argv, error, showHelp)) {
        if (showHelp) {
            std::fputs(arena::Options::usage(), stdout);
            return 0;
        }
        std::fprintf(stderr, "error: %s\n\n%s", error.c_str(), arena::Options::usage());
        return arena::kExitInitFailed;
    }
    return arena::runApp(options, argc, argv);
}
