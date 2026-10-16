// Command-line options of arena_client.
#pragma once

#include <cstdint>
#include <string>

namespace arena {

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 4400;
    bool hostGiven = false;    // --host was on the command line
    std::string name;          // empty = random "Player123"
    int color = -1;            // -1 = random
    std::string pack;          // empty = <exe dir>/arena.pak
    std::string dcDir;         // plain DC files instead of the pack (development)
    // Set by the app: where resources come from (Panda virtual file system paths).
    std::string resourceDcDir, resourceShaderDir;

    // Window / graphics.
    int width = 1280, height = 720;
    bool fullscreen = false;
    bool shadows = true;
    bool msaa = true;
    bool vsync = true;
    bool lowQuality = false;   // fixed-function lighting, no shadows, no MSAA
    float zoom = 1.0f;         // initial camera zoom (0.55 close .. 1.6 far)

    // Touch screens (always on Android): on-screen stick and dash button, and
    // without --host the connect screen first. On a PC the mouse is the finger.
    bool touch = false;
    float uiScale = 0.0f;      // 2D interface size; 0 = the platform's choice

    // Automation (tests, load, screenshots).
    bool headless = false;     // no window at all
    bool bot = false;          // computer-controlled player
    double duration = 0.0;     // quit after N seconds (0 = never)
    std::string screenshot;    // save a screenshot after screenshotAfter s, then quit
    double screenshotAfter = 6.0;
    int expectPlayers = -1;    // exit code 3 unless we saw this many other players
    std::string say;           // send this chat line once after joining
    std::string trace;         // log true and displayed positions (netcode analysis)
    bool reconnect = true;
    bool verbose = false;

    // Returns false (and fills error, or sets showHelp) if parsing failed.
    bool parse(int argc, char **argv, std::string &error, bool &showHelp);
    static const char *usage();
};

} // namespace arena
