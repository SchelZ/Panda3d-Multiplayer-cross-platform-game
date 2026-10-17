// Android: what the system tells the app besides Panda's keyboard and mouse:
// touches, the back button, typing on the phone's keyboard, and whether there
// is a screen to draw to. AndroidMain.cpp fills these from Android's input and
// lifecycle events; App.cpp reads them. Not part of the PC build (only the
// Touch type is shared, with the unit tests).
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

// The phone's keyboard, shown while the player types in a text field.
// Typed text arrives as printable ASCII, '\b' (delete) and '\n' (enter).
void showKeyboard(bool show);
bool keyboardShown();
void pushTyped(char c);
std::string takeTyped();  // since the last call

// False while the app is in the background (paused, or no window surface).
void setVisible(bool visible);
bool visible();

// Hooks into the window once it is open (input and lifecycle), and each frame.
void attachWindow();
void frame();
bool quitRequested();  // the system is closing the app

// Physical screen size class: how much to enlarge the 2D interface so text
// and buttons are big enough for fingers (1 = desktop).
float uiScale();

// Where the app may keep its settings (empty: nowhere).
std::string settingsFile();

// A file shipped inside the app package (the APK's assets).
bool readBundledFile(const std::string &name, std::string &data);

} // namespace arena::platform
