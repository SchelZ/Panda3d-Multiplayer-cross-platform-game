// The queues between Android's input (AndroidMain.cpp, called from Panda's
// event processing) and the game's frame (App.cpp).
#include "app/Platform.h"

#include <atomic>
#include <mutex>

namespace arena::platform {

namespace {
std::mutex g_mutex;
std::vector<Touch> g_touches;
std::string g_typed;
std::atomic<bool> g_back{false};
std::atomic<bool> g_visible{true};
} // namespace

void pushTouch(const Touch &touch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_touches.size() < 1024) g_touches.push_back(touch);
}

std::vector<Touch> takeTouches() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Touch> out;
    out.swap(g_touches);
    return out;
}

void pushTyped(char c) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_typed.size() < 256) g_typed += c;
}

std::string takeTyped() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    out.swap(g_typed);
    return out;
}

void pushBack() { g_back = true; }
bool takeBack() { return g_back.exchange(false); }

void setVisible(bool visible) { g_visible = visible; }
bool visible() { return g_visible; }

} // namespace arena::platform
