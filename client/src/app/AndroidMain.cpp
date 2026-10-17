// Android entry point. Android's NativeActivity loads libarena.so (no Java
// code of ours) and calls ANativeActivity_onCreate() below, which starts
// Panda's copy of the NDK app glue (android_native_app_glue.c); the glue
// calls android_main() on a thread of its own, which sets up what Panda's own
// android_main does for its Java activity, routes touches, typing, the back
// button and the app's lifecycle to the platform layer (Platform.h), and runs
// the game. Panda's Android window (androiddisplay, OpenGL ES) draws into the
// activity's window and reads its events while the game runs.
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
#include <android/native_activity.h>
#include <android/window.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

// Panda's Android window finds the activity through this (config_android.cxx
// defines it in Panda's own build; that file needs Panda's Java classes).
struct android_app *panda_android_app = nullptr;

// The glue's ANativeActivity_onCreate, renamed when it is compiled
// (cmake/Panda3DFromSource.cmake): it starts android_main().
extern "C" void arena_glue_onCreate(ANativeActivity *activity, void *savedState, size_t savedStateSize);

namespace {

android_app *g_app = nullptr;
JNIEnv *g_env = nullptr;  // android_main's thread, where input is handled
int32_t (*g_pandaInput)(android_app *, AInputEvent *) = nullptr;
void (*g_pandaCommand)(android_app *, int32_t) = nullptr;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_keyboard{false};

// ------------------------------------------------- Android's UI thread

void (*g_glueOnResume)(ANativeActivity *) = nullptr;
void (*g_glueOnFocus)(ANativeActivity *, int) = nullptr;

// Clears a pending Java exception (logging what failed); true if there was one.
bool failed(JNIEnv *env, const char *what) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_WARN, "PandaArena", "%s failed", what);
    return true;
}

// Local references made during one call from Android, deleted at its end.
struct LocalRefs {
    JNIEnv *env;
    std::vector<jobject> refs;
    template <typename T>
    T add(T ref) {
        if (ref != nullptr) refs.push_back(ref);
        return ref;
    }
    ~LocalRefs() {
        for (jobject ref : refs) env->DeleteLocalRef(ref);
    }
};

// activity.getWindow(), or null.
jobject activityWindow(ANativeActivity *activity, LocalRefs &refs) {
    JNIEnv *env = activity->env;
    jclass activityClass = refs.add(env->GetObjectClass(activity->clazz));
    jmethodID getWindow = env->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
    if (failed(env, "getWindow")) return nullptr;
    jobject window = refs.add(env->CallObjectMethod(activity->clazz, getWindow));
    return failed(env, "getWindow") ? nullptr : window;
}

// The game takes the whole screen: no navigation buttons or status bar (a
// swipe from the edge shows them for a moment, "sticky immersive"). View
// methods only work on the UI thread, where the activity callbacks run.
void hideSystemBars(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    LocalRefs refs{env, {}};
    jobject window = activityWindow(activity, refs);
    if (window == nullptr) return;
    jclass windowClass = refs.add(env->GetObjectClass(window));
    jmethodID getDecorView = env->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;");
    if (failed(env, "getDecorView")) return;
    jobject decor = refs.add(env->CallObjectMethod(window, getDecorView));
    if (failed(env, "getDecorView") || decor == nullptr) return;
    jclass viewClass = refs.add(env->GetObjectClass(decor));
    jmethodID setSystemUiVisibility = env->GetMethodID(viewClass, "setSystemUiVisibility", "(I)V");
    if (failed(env, "setSystemUiVisibility")) return;
    // View.SYSTEM_UI_FLAG_LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION |
    // LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY
    env->CallVoidMethod(decor, setSystemUiVisibility, 0x0100 | 0x0200 | 0x0400 | 0x0002 | 0x0004 | 0x1000);
    failed(env, "setSystemUiVisibility");
}

// Once, before the window is shown: the keyboard covers the screen's lower
// part instead of squeezing the game into the upper part (the connect
// screen's address field is at the top), and the game also draws beside a
// camera cutout (Android 9+).
void setUpWindow(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    LocalRefs refs{env, {}};
    jobject window = activityWindow(activity, refs);
    if (window == nullptr) return;
    jclass windowClass = refs.add(env->GetObjectClass(window));
    jmethodID setSoftInputMode = env->GetMethodID(windowClass, "setSoftInputMode", "(I)V");
    if (failed(env, "setSoftInputMode")) return;
    env->CallVoidMethod(window, setSoftInputMode, 0x30);  // SOFT_INPUT_ADJUST_NOTHING
    if (failed(env, "setSoftInputMode") || activity->sdkVersion < 28) return;
    jmethodID getAttributes =
        env->GetMethodID(windowClass, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
    if (failed(env, "getAttributes")) return;
    jmethodID setAttributes =
        env->GetMethodID(windowClass, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
    if (failed(env, "setAttributes")) return;
    jobject attrs = refs.add(env->CallObjectMethod(window, getAttributes));
    if (failed(env, "getAttributes") || attrs == nullptr) return;
    jclass paramsClass = refs.add(env->GetObjectClass(attrs));
    jfieldID mode = env->GetFieldID(paramsClass, "layoutInDisplayCutoutMode", "I");
    if (failed(env, "layoutInDisplayCutoutMode")) return;
    env->SetIntField(attrs, mode, 1);  // LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
    env->CallVoidMethod(window, setAttributes, attrs);
    failed(env, "setAttributes");
}

// The phone's keyboard only comes up for the view that has the focus, and
// NativeActivity's view (the one ANativeActivity_showSoftInput asks it for)
// cannot take it: since Android 8 a plain view is not focusable, and touching
// the screen takes the focus from views that are not focusable in touch mode.
// So the keyboard never showed on a touch screen. Make that view focusable
// (also in touch mode) and give it the focus.
void focusContentView(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    LocalRefs refs{env, {}};
    jclass activityClass = refs.add(env->GetObjectClass(activity->clazz));
    jmethodID findViewById = env->GetMethodID(activityClass, "findViewById", "(I)Landroid/view/View;");
    if (failed(env, "findViewById")) return;
    // android.R.id.content: the frame NativeActivity put its view into.
    jobject content = refs.add(env->CallObjectMethod(activity->clazz, findViewById, 0x01020002));
    if (failed(env, "findViewById") || content == nullptr) return;
    jclass groupClass = refs.add(env->FindClass("android/view/ViewGroup"));
    if (failed(env, "ViewGroup")) return;
    jmethodID getChildAt = env->GetMethodID(groupClass, "getChildAt", "(I)Landroid/view/View;");
    if (failed(env, "getChildAt")) return;
    jobject view = refs.add(env->CallObjectMethod(content, getChildAt, 0));
    if (failed(env, "getChildAt") || view == nullptr) return;
    jclass viewClass = refs.add(env->FindClass("android/view/View"));
    if (failed(env, "View")) return;
    jmethodID setFocusable = env->GetMethodID(viewClass, "setFocusable", "(Z)V");
    if (failed(env, "setFocusable")) return;
    jmethodID setFocusableInTouchMode = env->GetMethodID(viewClass, "setFocusableInTouchMode", "(Z)V");
    if (failed(env, "setFocusableInTouchMode")) return;
    jmethodID requestFocus = env->GetMethodID(viewClass, "requestFocus", "()Z");
    if (failed(env, "requestFocus")) return;
    env->CallVoidMethod(view, setFocusable, JNI_TRUE);
    if (failed(env, "setFocusable")) return;
    env->CallVoidMethod(view, setFocusableInTouchMode, JNI_TRUE);
    if (failed(env, "setFocusableInTouchMode")) return;
    env->CallBooleanMethod(view, requestFocus);
    failed(env, "requestFocus");
}

// The system shows the bars again after the app was in the background or a
// dialog (volume, notification shade) took the focus: hide them again.
void onResumeUi(ANativeActivity *activity) {
    g_glueOnResume(activity);
    hideSystemBars(activity);
}

void onFocusUi(ANativeActivity *activity, int focused) {
    g_glueOnFocus(activity, focused);
    if (focused) {
        hideSystemBars(activity);
        focusContentView(activity);
    }
}

// ---------------------------------------------- android_main's thread

void pushTouch(arena::platform::Touch::Kind kind, const AInputEvent *event, size_t index) {
    // Window pixels, like Panda's own pointer (androidGraphicsWindow.cxx).
    arena::platform::pushTouch({kind, static_cast<int>(AMotionEvent_getPointerId(event, index)),
                                AMotionEvent_getX(event, index) - g_app->contentRect.left,
                                AMotionEvent_getY(event, index) - g_app->contentRect.top});
}

// The character a key makes, as Java's KeyEvent.getUnicodeChar() tells (the
// NDK has no such call): the keyboard's layout, with shift and the like.
int unicodeChar(int32_t keyCode, int32_t metaState) {
    JNIEnv *env = g_env;
    if (env == nullptr) return 0;
    static jclass keyEventClass = nullptr;
    static jmethodID construct = nullptr, getUnicodeChar = nullptr;
    if (keyEventClass == nullptr) {
        jclass local = env->FindClass("android/view/KeyEvent");
        if (failed(env, "KeyEvent") || local == nullptr) return 0;
        construct = env->GetMethodID(local, "<init>", "(II)V");
        if (!failed(env, "KeyEvent()")) getUnicodeChar = env->GetMethodID(local, "getUnicodeChar", "(I)I");
        if (failed(env, "getUnicodeChar") || construct == nullptr || getUnicodeChar == nullptr) {
            env->DeleteLocalRef(local);
            return 0;
        }
        keyEventClass = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
    }
    jobject keyEvent = env->NewObject(keyEventClass, construct, AKEY_EVENT_ACTION_DOWN, keyCode);
    jint c = 0;
    if (!failed(env, "KeyEvent()") && keyEvent != nullptr) c = env->CallIntMethod(keyEvent, getUnicodeChar, metaState);
    if (failed(env, "getUnicodeChar")) c = 0;
    if (keyEvent != nullptr) env->DeleteLocalRef(keyEvent);
    return c;
}

// While the phone's keyboard is up, keys are typing (held keys repeat as
// more key-downs).
void typeKey(const AInputEvent *event) {
    if (AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_DOWN) return;
    const int32_t code = AKeyEvent_getKeyCode(event);
    if (code == AKEYCODE_DEL) {
        arena::platform::pushTyped('\b');
    } else if (code == AKEYCODE_ENTER || code == AKEYCODE_NUMPAD_ENTER) {
        arena::platform::pushTyped('\n');
    } else {
        const int c = unicodeChar(code, AKeyEvent_getMetaState(event));
        if (c >= 0x20 && c < 0x7f) arena::platform::pushTyped(static_cast<char>(c));
    }
}

// All fingers (Panda's window only follows the first one, as a mouse),
// typing, and the back button; other keys (hardware keys) go on to Panda.
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
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        if (code == AKEYCODE_BACK) {
            // (With the keyboard up, it takes the back button itself first.)
            if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP) arena::platform::pushBack();
            return 1;  // ours: Android must not close the activity by itself
        }
        if (code == AKEYCODE_UNKNOWN) {
            return 1;  // emulators (BlueStacks) send these with mouse clicks; Panda would warn about each
        }
        if (g_keyboard) {
            typeKey(event);
            return 1;
        }
    }
    return g_pandaInput != nullptr ? g_pandaInput(app, event) : 0;
}

void hideKeyboard() {
    if (g_app != nullptr && g_keyboard.exchange(false)) ANativeActivity_hideSoftInput(g_app->activity, 0);
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
    case APP_CMD_PAUSE:
        hideKeyboard();  // a forced keyboard would stay over the next app
        break;
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

// NativeActivity calls this on its UI thread when the activity is created.
extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity, void *savedState,
                                                   size_t savedStateSize) {
    arena_glue_onCreate(activity, savedState, savedStateSize);
    // The glue set its callbacks; they run on this thread, so wrapping two of
    // them here is safe (android_main's thread never touches them).
    g_glueOnResume = activity->callbacks->onResume;
    g_glueOnFocus = activity->callbacks->onWindowFocusChanged;
    activity->callbacks->onResume = onResumeUi;
    activity->callbacks->onWindowFocusChanged = onFocusUi;
    setUpWindow(activity);
    focusContentView(activity);
    hideSystemBars(activity);  // now already: the window opens full-screen
}

namespace arena::platform {

void attachWindow() { installHooks(); }

void frame() {
    if (g_app == nullptr) return;
    if (g_app->onInputEvent != nullptr && g_app->onInputEvent != onInput) installHooks();
}

bool quitRequested() { return g_quit || (g_app != nullptr && g_app->destroyRequested); }

void showKeyboard(bool show) {
    if (!show) {
        hideKeyboard();
    } else if (g_app != nullptr && visible() && !g_keyboard.exchange(true)) {
        // FORCED: NativeActivity's view is no text editor, and the keyboard
        // would not come up for an implicit request.
        ANativeActivity_showSoftInput(g_app->activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    }
}

bool keyboardShown() { return g_keyboard; }

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
    g_env = env;
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
    options.fullscreen = true;  // (and 4x MSAA where the phone has it: App.cpp)
    char arg0[] = "arena";
    char *argv[] = {arg0, nullptr};
    const int code = arena::runApp(options, 1, argv);
    __android_log_print(ANDROID_LOG_INFO, "PandaArena", "game ended (%d)", code);
    hideKeyboard();

    // Close the activity, and keep the glue's events flowing until Android
    // has taken it down.
    ANativeActivity_finish(app->activity);
    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source *source = nullptr;
        const int id = ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void **>(&source));
        if (id >= 0 && source != nullptr) source->process(app, source);
    }
    g_env = nullptr;
    app->activity->vm->DetachCurrentThread();
    g_app = nullptr;
    panda_android_app = nullptr;
}
