#include "app/Options.h"

#include <cstdio>
#include <cstdlib>

namespace arena {

namespace {

bool parseNumber(const char *text, double lo, double hi, double &out) {
    char *end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    out = v;
    return true;
}

} // namespace

const char *Options::usage() {
    return "Panda Arena client\n"
           "\n"
           "usage: arena_client [options]\n"
           "\n"
           "connection:\n"
           "  --host ADDR          server address (default 127.0.0.1)\n"
           "  --port N             server port (default 4400)\n"
           "  --name NAME          player name (max 16 bytes)\n"
           "  --color N            player color 0-11\n"
           "  --no-reconnect       do not reconnect after losing the server\n"
           "  --pack FILE          resource pack (default: arena.pak next to the client)\n"
           "  --dc-dir DIR         plain direct.dc and arena.dc instead (development)\n"
           "\n"
           "graphics:\n"
           "  --size WxH           window size (default 1280x720)\n"
           "  --fullscreen\n"
           "  --no-shadows         disable shadow mapping\n"
           "  --no-msaa            disable multisampling\n"
           "  --no-vsync\n"
           "  --low                fixed-function lighting, no shadows, no MSAA\n"
           "  --zoom F             initial camera distance, 0.55 (close) to 1.6 (far)\n"
           "  --touch              phone controls: connect screen (unless --host), on-screen\n"
           "                       stick and dash button; the mouse is the finger\n"
           "  --ui-scale F         size of the 2D interface, 0.8-2 (default 1)\n"
           "\n"
           "automation:\n"
           "  --headless           no window (bots, tests, load generation)\n"
           "  --bot                the computer plays\n"
           "  --duration SEC       quit after SEC seconds\n"
           "  --screenshot FILE    save a screenshot and quit (see --screenshot-after)\n"
           "  --screenshot-after S seconds after joining (default 6)\n"
           "  --expect-players N   exit code 3 unless N other players were seen\n"
           "  --say TEXT           send TEXT to the chat once after joining\n"
           "  --trace FILE         log true/displayed positions (server/tests/trace_stats.py)\n"
           "  --verbose            debug logging\n"
           "\n"
           "controls: WASD/arrows move, Space dash, Enter chat, mouse wheel zoom,\n"
           "          F1 help, F3 network stats, F11 fullscreen, Esc quit\n";
}

bool Options::parse(int argc, char **argv, std::string &error, bool &showHelp) {
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                error = arg + " needs " + what;
                return nullptr;
            }
            return argv[++i];
        };
        double num = 0.0;
        if (arg == "-h" || arg == "--help") {
            showHelp = true;
            return false;
        } else if (arg == "--host") {
            const char *v = value("an address");
            if (!v) return false;
            host = v;
            hostGiven = true;
        } else if (arg == "--port") {
            const char *v = value("a port");
            if (!v || !parseNumber(v, 1, 65535, num)) {
                if (error.empty()) error = "invalid port";
                return false;
            }
            port = static_cast<uint16_t>(num);
        } else if (arg == "--name") {
            const char *v = value("a name");
            if (!v) return false;
            name = v;
        } else if (arg == "--color") {
            const char *v = value("0-11");
            if (!v || !parseNumber(v, 0, 11, num)) {
                if (error.empty()) error = "color must be 0-11";
                return false;
            }
            color = static_cast<int>(num);
        } else if (arg == "--pack") {
            const char *v = value("a file");
            if (!v) return false;
            pack = v;
        } else if (arg == "--dc-dir") {
            const char *v = value("a folder");
            if (!v) return false;
            dcDir = v;
        } else if (arg == "--size") {
            const char *v = value("WxH");
            int w = 0, h = 0;
            if (!v || std::sscanf(v, "%dx%d", &w, &h) != 2 || w < 320 || h < 240 || w > 16384 || h > 16384) {
                if (error.empty()) error = "size must look like 1280x720";
                return false;
            }
            width = w;
            height = h;
        } else if (arg == "--fullscreen") {
            fullscreen = true;
        } else if (arg == "--no-shadows") {
            shadows = false;
        } else if (arg == "--no-msaa") {
            msaa = false;
        } else if (arg == "--no-vsync") {
            vsync = false;
        } else if (arg == "--zoom") {
            const char *v = value("a number");
            float z = 0.0f;
            if (!v || std::sscanf(v, "%f", &z) != 1 || z < 0.55f || z > 1.6f) {
                if (error.empty()) error = "zoom must be 0.55-1.6";
                return false;
            }
            zoom = z;
        } else if (arg == "--touch") {
            touch = true;
        } else if (arg == "--ui-scale") {
            const char *v = value("a number");
            if (!v || !parseNumber(v, 0.8, 2.0, num)) {
                if (error.empty()) error = "ui scale must be 0.8-2";
                return false;
            }
            uiScale = static_cast<float>(num);
        } else if (arg == "--low") {
            lowQuality = true;
            shadows = false;
            msaa = false;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--bot") {
            bot = true;
        } else if (arg == "--duration") {
            const char *v = value("seconds");
            if (!v || !parseNumber(v, 0, 1e7, num)) {
                if (error.empty()) error = "invalid duration";
                return false;
            }
            duration = num;
        } else if (arg == "--screenshot") {
            const char *v = value("a file name");
            if (!v) return false;
            screenshot = v;
        } else if (arg == "--screenshot-after") {
            const char *v = value("seconds");
            if (!v || !parseNumber(v, 0, 3600, num)) {
                if (error.empty()) error = "invalid screenshot delay";
                return false;
            }
            screenshotAfter = num;
        } else if (arg == "--expect-players") {
            const char *v = value("a count");
            if (!v || !parseNumber(v, 0, 1000, num)) {
                if (error.empty()) error = "invalid player count";
                return false;
            }
            expectPlayers = static_cast<int>(num);
        } else if (arg == "--say") {
            const char *v = value("a message");
            if (!v) return false;
            say = v;
        } else if (arg == "--trace") {
            const char *v = value("a file name");
            if (!v) return false;
            trace = v;
        } else if (arg == "--no-reconnect") {
            reconnect = false;
        } else if (arg == "--verbose") {
            verbose = true;
        } else {
            error = "unknown option " + arg;
            return false;
        }
    }
    if (headless) {
        bot = true;  // nobody to press keys
    }
    return true;
}

} // namespace arena
