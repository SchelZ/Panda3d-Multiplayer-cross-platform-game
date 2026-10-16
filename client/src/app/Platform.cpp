#include "app/Platform.h"

#include <atomic>
#include <mutex>

namespace arena::platform {

namespace {
std::mutex g_mutex;
std::vector<Touch> g_touches;
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

void pushBack() { g_back = true; }
bool takeBack() { return g_back.exchange(false); }

void setVisible(bool visible) { g_visible = visible; }
bool visible() { return g_visible; }

#ifndef __ANDROID__
void attachWindow() {}
void frame() {}
bool quitRequested() { return false; }
float uiScale() { return 1.0f; }
std::string settingsFile() { return {}; }
bool readBundledFile(const std::string &, std::string &) { return false; }
#endif

} // namespace arena::platform
