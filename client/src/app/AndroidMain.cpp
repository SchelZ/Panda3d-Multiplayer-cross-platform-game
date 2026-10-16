// Android entry point. Android's NativeActivity loads libarena.so (no Java
// code of ours); Panda's copy of the NDK app glue (android_native_app_glue.c)
// then calls android_main() on a thread of its own, which sets up what
// Panda's own android_main does for its Java activity, routes touches, the
// back button and the app's lifecycle to the platform layer (Platform.h), and
// runs the game. Panda's Android window (androiddisplay, OpenGL ES) draws
// into the activity's window and reads its events while the game runs.
#include "app/App.h"
#include "app/Options.h"
#include "app/Platform.h"

#include "filename.h"
#include "thread.h"

#include "android_native_app_glue.h"

#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/window.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <string>

// Panda's Android window finds the activity through this (config_android.cxx
// defines it in Panda's own build; that file needs Panda's Java classes).
struct android_app *panda_android_app = nullptr;

namespace {

android_app *g_app = nullptr;
int32_t (*g_pandaInput)(android_app *, AInputEvent *) = nullptr;
void (*g_pandaCommand)(android_app *, int32_t) = nullptr;
std::atomic<bool> g_quit{false};

void pushTouch(arena::platform::Touch::Kind kind, const AInputEvent *event, size_t index) {
    // Window pixels, like Panda's own pointer (androidGraphicsWindow.cxx).
    arena::platform::pushTouch({kind, static_cast<int>(AMotionEvent_getPointerId(event, index)),
                                AMotionEvent_getX(event, index) - g_app->contentRect.left,
                                AMotionEvent_getY(event, index) - g_app->contentRect.top});
}

// All fingers (Panda's window only follows the first one, as a mouse), and
// the back button; everything else (hardware keys) goes on to Panda.
int32_t onInput(android_app *app, AInputEvent *event) {
    using Kind = arena::platform::Touch::Kind;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        const int32_t action = AMotionEvent_getAction(event);
        const size_t index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                                 AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        const size_t count = AMotionEvent_getPointerCount(event);
        switch (action & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            pushTouch(Kind::Down, event, index);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            pushTouch(Kind::Up, event, index);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < count; ++i) pushTouch(Kind::Move, event, i);
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (size_t i = 0; i < count; ++i) pushTouch(Kind::Cancel, event, i);
            break;
        default:
            break;
        }
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event) == AKEYCODE_BACK) {
        if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) arena::platform::pushBack();
        return 1;  // ours: Android must not close the activity by itself
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event) == AKEYCODE_UNKNOWN) {
        return 1;  // emulators (BlueStacks) send these with mouse clicks; Panda would warn about each
    }
    return g_pandaInput != nullptr ? g_pandaInput(app, event) : 0;
}

// The game draws only while the activity is resumed and has its window.
// Pausing comes first when the app leaves the screen (Home, the app switcher),
// before Android takes the window's surface away: drawing into it after that
// fails at every frame.
void updateVisible(const android_app *app, int32_t command) {
    // The glue updates activityState before a command is handled, and clears
    // the window only after APP_CMD_TERM_WINDOW was handled.
    const bool hasWindow = app->window != nullptr && command != APP_CMD_TERM_WINDOW;
    arena::platform::setVisible(hasWindow && app->activityState == APP_CMD_RESUME);
}

void onCommand(android_app *app, int32_t command) {
    if (g_pandaCommand != nullptr) g_pandaCommand(app, command);
    updateVisible(app, command);
    switch (command) {
    case APP_CMD_LOST_FOCUS:
        // A finger that was down may never be lifted (notification shade).
        arena::platform::pushTouch({arena::platform::Touch::Kind::Cancel, -1, 0.0f, 0.0f});
        break;
    case APP_CMD_DESTROY:
        g_quit = true;
        break;
    default:
        break;
    }
}

void installHooks() {
    if (g_app == nullptr || g_app->onInputEvent == onInput) return;
    // Panda's window registered its handlers when it opened: wrap them.
    g_pandaInput = g_app->onInputEvent;
    g_pandaCommand = g_app->onAppCmd;
    g_app->onInputEvent = onInput;
    g_app->onAppCmd = onCommand;
    updateVisible(g_app, -1);  // Panda's window handled the commands until now
}

} // namespace

namespace arena::platform {

void attachWindow() { installHooks(); }

void frame() {
    if (g_app == nullptr) return;
    if (g_app->onInputEvent != nullptr && g_app->onInputEvent != onInput) installHooks();
}

bool quitRequested() { return g_quit || (g_app != nullptr && g_app->destroyRequested); }

float uiScale() {
    // Phones are small and held at arm's length: enlarge the interface by
    // how much shorter the screen is than a small laptop's (~600 dp).
    if (g_app == nullptr || g_app->config == nullptr) return 1.35f;
    const int32_t w = AConfiguration_getScreenWidthDp(g_app->config);
    const int32_t h = AConfiguration_getScreenHeightDp(g_app->config);
    const int32_t shortSide = std::min(w, h);
    if (shortSide <= 0) return 1.35f;
    return std::clamp(560.0f / static_cast<float>(shortSide), 1.0f, 1.5f);
}

std::string settingsFile() {
    if (g_app == nullptr || g_app->activity->internalDataPath == nullptr) return {};
    return std::string(g_app->activity->internalDataPath) + "/settings.txt";
}

bool readBundledFile(const std::string &name, std::string &data) {
    if (g_app == nullptr) return false;
    AAsset *asset = AAssetManager_open(g_app->activity->assetManager, name.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr) return false;
    const off64_t size = AAsset_getLength64(asset);
    const void *bytes = AAsset_getBuffer(asset);
    const bool ok = bytes != nullptr && size > 0;
    if (ok) data.assign(static_cast<const char *>(bytes), static_cast<size_t>(size));
    AAsset_close(asset);
    return ok;
}

} // namespace arena::platform

void android_main(struct android_app *app) {
    g_app = app;
    panda_android_app = app;

    // Panda's threads attach themselves to the Java VM (threadPosixImpl.cxx),
    // which it learns about in JNI_OnLoad; NativeActivity does not call that
    // for us. This thread is not a Panda thread yet either.
    JNIEnv *env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);
    PT(Thread) thread = Thread::bind_thread("android_main", "android_app");
    JNI_OnLoad(app->activity->vm, nullptr);
    if (app->activity->internalDataPath != nullptr) {
        Filename::_internal_data_dir = app->activity->internalDataPath;
    }
    __android_log_print(ANDROID_LOG_INFO, "PandaArena", "starting (data in %s)",
                        app->activity->internalDataPath ? app->activity->internalDataPath : "?");

    // Nobody touches the screen while watching a round: keep it on.
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

    arena::Options options;
    options.touch = true;      // and the connect screen (no --host)
    // No MSAA: phone screens have tiny pixels, and multisampled EGL configs are
    // where emulators (BlueStacks) and some drivers misbehave.
    options.msaa = false;
    options.fullscreen = true;
    char arg0[] = "arena";
    char *argv[] = {arg0, nullptr};
    const int code = arena::runApp(options, 1, argv);
    __android_log_print(ANDROID_LOG_INFO, "PandaArena", "game ended (%d)", code);

    // Close the activity, and keep the glue's events flowing until Android
    // has taken it down.
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source *source = nullptr;
        const int id = ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void **>(&source));
        if (id >= 0 && source != nullptr) source->process(app, source);
    }
    app->activity->vm->DetachCurrentThread();
    g_app = nullptr;
    panda_android_app = nullptr;
}
