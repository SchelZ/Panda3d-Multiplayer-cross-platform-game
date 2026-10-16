// What the operating system tells the app besides Panda's keyboard and mouse:
// touches, the back button, and whether there is a screen to draw to. Android
// fills these from its input and lifecycle events (AndroidMain.cpp); on the
// desktop, --touch turns the mouse into a finger (App.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arena::platform {

struct Touch {
    enum class Kind : uint8_t { Down, Move, Up, Cancel };
    Kind kind;
    int id;      // stays the same while the finger is down; Cancel with -1: all
    float x, y;  // window pixels from the top-left corner
};

void pushTouch(const Touch &touch);
std::vector<Touch> takeTouches();  // in order, since the last call

void pushBack();  // the back button / gesture
bool takeBack();

// False while the app is in the background (Android: no window surface).
void setVisible(bool visible);
bool visible();

// Hooks into the window once it is open (Android: input and lifecycle), and
// each frame. No-ops on the desktop.
void attachWindow();
void frame();
bool quitRequested();  // the system is closing the app (Android)

// Physical screen size class: how much to enlarge the 2D interface so text
// and buttons are big enough for fingers (1 = desktop).
float uiScale();

// Where the app may keep its settings (empty: nowhere).
std::string settingsFile();

// A file shipped inside the app package (Android: the APK's assets). False
// if there is none (the desktop: files sit next to the executable).
bool readBundledFile(const std::string &name, std::string &data);

} // namespace arena::platform
