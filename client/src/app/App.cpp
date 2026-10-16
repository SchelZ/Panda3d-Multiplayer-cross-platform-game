#include "app/App.h"

#include "app/Platform.h"
#include "core/Log.h"
#include "core/ResourcePack.h"
#include "core/Settings.h"
#include "game/ConnectScreen.h"
#include "game/GameClient.h"
#include "game/Hud.h"
#include "game/TouchControls.h"
#include "game/World.h"
#include "net/LanDiscovery.h"

#include "antialiasAttrib.h"
#include "clockObject.h"
#include "executionEnvironment.h"
#include "genericAsyncTask.h"
#include "graphicsStateGuardian.h"
#include "graphicsWindow.h"
#include "keyboardButton.h"
#include "load_prc_file.h"
#include "mouseButton.h"
#include "mouseWatcher.h"
#include "pandaFramework.h"
#include "perspectiveLens.h"
#include "thread.h"
#include "virtualFileSystem.h"
#include "windowFramework.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <sstream>
#include <string>

#ifdef ARENA_GLES
// Static builds: PandaFramework only knows desktop OpenGL; the OpenGL ES
// display is registered here.
#ifdef __ANDROID__
extern void init_libgles2gsg();
extern void init_libandroiddisplay();
#else
extern void init_libpandagles2();
#endif
#endif

namespace arena {

namespace {

void initGraphicsModules() {
#ifdef ARENA_GLES
#ifdef __ANDROID__
    init_libgles2gsg();
    init_libandroiddisplay();
#else
    init_libpandagles2();
#endif
#endif
}

// Give up on a server that never answered and go back to the connect screen.
constexpr double kJoinTimeout = 15.0;

struct AppState {
    Options options;
    PandaFramework framework;
    WindowFramework *window = nullptr;
    std::unique_ptr<game::World> world;
    std::unique_ptr<game::Hud> hud;
    std::unique_ptr<game::GameClient> game;
    MouseWatcher *mouse = nullptr;
    double startTime = 0.0;
    bool dashRequested = false;
    long chatClosedFrame = -1;
    bool screenshotTaken = false;
    bool shadersVerified = false;
    bool quit = false;

    // Touch screens and the connect screen (Options::touch).
    std::unique_ptr<game::TouchControls> touch;
    std::unique_ptr<game::ConnectScreen> menu;  // only where there is no --host
    net::LanDiscovery discovery;
    std::unique_ptr<core::Settings> settings;
    float uiScale = 1.0f;
    bool mouseFingerDown = false;  // --touch on a PC: the mouse is finger 0
    double gameStartedAt = 0.0;
    // What the last game left behind, for the summary line and exit code.
    game::GameClient::Counters counters;
    game::GameClient::Phase phase = game::GameClient::Phase::Offline;
    std::string rejectReason;
};

long frameCount() { return ClockObject::get_global_clock()->get_frame_count(); }

// Panda3D's configuration (PRC), built in. Only what changes something for
// this game is set: Panda's defaults already suit most variables (the state
// caches, vertex buffers, pre-munged vertex data, deferred state garbage
// collection), and the model, texture-loading and audio settings do not apply
// (the world is generated at start-up; there are no model or texture files).
void configure(const Options &o) {
    std::string prc;
    auto set = [&prc](const std::string &line) { prc += line + "\n"; };

    // Logging: warnings and errors only; --verbose adds the GL driver's own
    // messages and shader validation.
    set("notify-level warning");
    set("default-directnotify-level warning");
    set("notify-level-device fatal");
    set(std::string("gl-debug ") + (o.verbose ? "#t" : "#f"));
    // glValidateProgram on a shader's first use only prints diagnostics, and
    // stalls that first frame.
    set(std::string("gl-validate-shaders ") + (o.verbose ? "#t" : "#f"));

    // The renderer. OpenGL ES (Android, linux-gles): pandagles2, for our
    // shaders, as Panda's Android guide recommends; pandagles (fixed-function
    // OpenGL ES) and then the software renderer as fallbacks. In this build
    // the game always draws with pandagles2: Panda is compiled into the
    // client, the OpenGL ES 2/3 renderer (androiddisplay + gles2gsg, egl on
    // Linux) is the only one compiled in, and it registers itself while the
    // program loads, before these lines are read, so Panda never looks for
    // plugin files. The lines name the choice, and take effect in a build that
    // loads renderers as plugins (Panda's own Android builds).
    // Desktop OpenGL names no load-display: it would make a Panda3D SDK build
    // load the OpenGL ES plugin, which our desktop GLSL does not run on. The
    // SDK build (linux-gcc-sdk) can still fall back to the software renderer,
    // which is a plugin there.
#ifdef ARENA_GLES
    set("load-display pandagles2");
    set("aux-display pandagles");
#endif
    set("aux-display p3tinydisplay");

    // The window's framebuffer, stated rather than left to the driver: 8 bits
    // per colour (a 16-bit 565 screen would band the sky), a 24-bit depth
    // buffer (16 bits make distant hills and trees flicker) and no stencil
    // (not used). The alpha channel stays requested (Panda's default): EGL
    // configs without one rendered black on Mesa. MSAA is requested per
    // attempt in runApp().
    set("color-bits 8 8 8");
    set("depth-bits 24");
    set("framebuffer-stencil #f");

    set("window-title Panda Arena");
    set("win-origin -2 -2");  // centred on the screen
    set("text-encoding utf8");
    set("textures-power-2 none");
    // HUD text: mipmapped glyphs stay crisp when small (no shimmering
    // strokes), and larger font pages hold every glyph the HUD uses on one or
    // two textures, so the text draws in fewer batches.
    set("text-minfilter linear_mipmap_linear");
    set("text-page-size 512 512");
#ifdef ARENA_GLES
    // A sized depth format for the sun's shadow map (OpenGL ES 3 renders to
    // DEPTH_COMPONENT24; desktop OpenGL picks its own).
    set("shadow-depth-bits 24");
#endif

    // Networking: all messages of a frame in one TCP write; we flush every
    // frame. The timeout bounds the blocking connect on the connect thread.
    set("collect-tcp 1");
    set("collect-tcp-interval 0.05");
    set("game-server-timeout-ms 5000");

    // Frame pacing. Sleeping, not busy-waiting, between frames (Panda's
    // default sleep-precision spins the last 10 ms of every frame).
    if (o.headless) {
        set("window-type none");
        set("audio-library-name null");
        set("clock-mode limited");
        set("clock-frame-rate 60");
        set("sleep-precision 0");
    } else {
        set(std::string("sync-video ") + (o.vsync ? "#t" : "#f"));
#ifdef __ANDROID__
        // Phones: at most 60 frames a second, also on 90 and 120 Hz screens,
        // so the phone stays cool and keeps its full clock speed (and its
        // battery) through a round.
        set("clock-mode limited");
        set("clock-frame-rate 60");
        set("sleep-precision 0");
#endif
    }
    load_prc_file_data("arena", prc);
}

// Resources (DC files, shaders): the encrypted arena.pak inside the app
// package (Android), next to the client (or --pack), mounted at /arena;
// without one, plain files (development).
bool mountResources(Options &o) {
    auto usePack = [&o]() {
        if (o.dcDir.empty()) o.resourceDcDir = std::string(core::kPackMount) + "/dc";
        o.resourceShaderDir = std::string(core::kPackMount) + "/shaders";
    };
    std::string bundled;
    if (o.pack.empty() && platform::readBundledFile("arena.pak", bundled)) {
        std::string error;
        if (!core::mountPackData(std::move(bundled), "arena.pak", Filename(core::kPackMount), error)) {
            LOG_ERROR("%s", error.c_str());
            return false;
        }
        LOG_INFO("resources: arena.pak in the app package");
        usePack();
        return true;
    }

    VirtualFileSystem *vfs = VirtualFileSystem::get_global_ptr();
    const Filename exeDir = Filename::from_os_specific(ExecutionEnvironment::get_binary_name()).get_dirname();
    const Filename pak = o.pack.empty() ? Filename(exeDir, "arena.pak") : Filename::from_os_specific(o.pack);
    if (!o.pack.empty() || vfs->exists(pak)) {
        std::string error;
        if (!core::mountPack(pak, Filename(core::kPackMount), error)) {
            LOG_ERROR("%s", error.c_str());
            return false;
        }
        LOG_INFO("resources: %s", pak.to_os_specific().c_str());
        usePack();
        return true;
    }
    LOG_WARN("no arena.pak next to the client: reading plain files (development build)");
    for (const char *rel : {"shaders", "../client/shaders", "../../client/shaders", "../../../client/shaders"}) {
        const Filename dir(exeDir, rel);
        if (vfs->exists(Filename(dir, "world.vert"))) {
            o.resourceShaderDir = dir.get_fullpath();
            break;
        }
    }
    return true;
}

// ------------------------------------------------------------ game / menu

void keepCounters(AppState &app) {
    if (!app.game) return;
    const auto &c = app.game->counters();
    // Several games in one run (the connect screen): add them up.
    auto &k = app.counters;
    k.everPlayed = k.everPlayed || c.everPlayed;
    k.maxOtherPlayers = std::max(k.maxOtherPlayers, c.maxOtherPlayers);
    k.orbsCollected += c.orbsCollected;
    k.pongs += c.pongs;
    k.chatsReceived += c.chatsReceived;
    k.hitsGiven += c.hitsGiven;
    k.hitsTaken += c.hitsTaken;
    k.corrections += c.corrections;
    k.reconnects += c.reconnects;
    k.arenaRebuilds += c.arenaRebuilds;
    k.pingSumMs += c.pingSumMs;
    k.pingMaxMs = std::max(k.pingMaxMs, c.pingMaxMs);
    k.dashes += c.dashes;
    k.dashContacts += c.dashContacts;
    k.contactsConfirmed += c.contactsConfirmed;
    k.contactsMissed += c.contactsMissed;
    k.phantomHits += c.phantomHits;
    k.positionSends += c.positionSends;
    k.playSeconds += c.playSeconds;
    k.frames += c.frames;
    k.bytesIn += c.bytesIn;
    k.bytesOut += c.bytesOut;
    app.phase = app.game->phase();
    app.rejectReason = app.game->rejectReason();
}

bool startGame(AppState &app, std::string &error) {
    app.game = std::make_unique<game::GameClient>(app.options, app.world.get(), app.hud.get());
    if (!app.game->init(error)) {
        app.game.reset();
        return false;
    }
    app.game->start();
    app.gameStartedAt = ClockObject::get_global_clock()->get_frame_time();
    if (app.hud) app.hud->setVisible(true);
    if (app.touch) app.touch->setVisible(true);
    return true;
}

void stopGame(AppState &app) {
    if (!app.game) return;
    app.game->shutdown();
    keepCounters(app);
    app.game.reset();
    if (app.world) app.world->clear();
}

// A meadow to look at behind the connect screen, until a server sends its own.
void showMenuScenery(AppState &app) {
    if (!app.world) return;
    game::ArenaLayout layout;
    layout.pillars = {{9.0f, 6.0f, 1.6f}, {-11.0f, -4.0f, 2.0f}, {3.0f, -13.0f, 1.3f}};
    app.world->setArena(layout, game::Rules());
}

void openMenu(AppState &app, const std::string &message) {
    stopGame(app);
    if (!app.menu) return;
    if (app.hud) {
        app.hud->reset();  // no scores or chat from the last game
        app.hud->setVisible(false);
    }
    if (app.touch) app.touch->setVisible(false);
    app.menu->setVisible(true);
    app.menu->setMessage(message.empty() ? "Looking for servers on this network..." : message);
    app.discovery.start();
}

void joinFromMenu(AppState &app, const std::string &address) {
    std::string host;
    uint16_t port = 0;
    if (!game::ConnectScreen::parseAddress(address, host, port)) return;
    app.options.host = host;
    app.options.port = port;
    if (app.settings) {
        app.settings->set("server", address);
        app.settings->save();
    }
    app.discovery.stop();
    app.menu->setVisible(false);
    std::string error;
    if (!startGame(app, error)) {
        LOG_ERROR("%s", error.c_str());
        openMenu(app, error);
    }
}

// ------------------------------------------------------------------ input

void onEscape(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    if (app->hud && app->hud->chatOpen()) {
        app->hud->closeChat();
        app->chatClosedFrame = frameCount();
    } else if (app->menu) {
        platform::pushBack();  // like Android's back button: game -> menu -> quit
    } else {
        app->quit = true;
    }
}

void onEnter(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    // The PGEntry accept event and this one fire for the same key press.
    if (app->game && app->hud && !app->hud->chatOpen() && app->chatClosedFrame != frameCount()) {
        app->hud->openChat();
    }
}

void onChatAccept(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    const std::string text = app->hud->takeChatText();
    app->hud->closeChat();
    app->chatClosedFrame = frameCount();
    if (app->game) app->game->sendChat(text);
}

void onSpace(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    if (!app->hud || !app->hud->chatOpen()) app->dashRequested = true;
}

void onHelp(const Event *, void *data) { static_cast<AppState *>(data)->hud->toggleHelp(); }
void onStats(const Event *, void *data) { static_cast<AppState *>(data)->hud->toggleStats(); }
void onWheelUp(const Event *, void *data) { static_cast<AppState *>(data)->world->zoom(1.0f); }
void onWheelDown(const Event *, void *data) { static_cast<AppState *>(data)->world->zoom(-1.0f); }

void onFullscreen(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    GraphicsWindow *win = app->window->get_graphics_window();
    if (win == nullptr) return;
    WindowProperties props;
    const bool full = !win->get_properties().get_fullscreen();
    props.set_fullscreen(full);
    if (full) {
        props.set_size(win->get_pipe()->get_display_width(), win->get_pipe()->get_display_height());
    } else {
        props.set_size(app->options.width, app->options.height);
    }
    win->request_properties(props);
}

game::PlayerInput readInput(AppState &app) {
    game::PlayerInput in;
    in.dash = app.dashRequested;
    app.dashRequested = false;
    if (app.mouse == nullptr || (app.hud && app.hud->chatOpen())) {
        return in;
    }
    auto down = [&](ButtonHandle b) { return app.mouse->is_button_down(b); };
    if (down(KeyboardButton::ascii_key('w')) || down(KeyboardButton::up())) in.y += 1.0f;
    if (down(KeyboardButton::ascii_key('s')) || down(KeyboardButton::down())) in.y -= 1.0f;
    if (down(KeyboardButton::ascii_key('d')) || down(KeyboardButton::right())) in.x += 1.0f;
    if (down(KeyboardButton::ascii_key('a')) || down(KeyboardButton::left())) in.x -= 1.0f;
    const float len = std::sqrt(in.x * in.x + in.y * in.y);
    if (len > 1.0f) {
        in.x /= len;
        in.y /= len;
    }
    return in;
}

#ifndef __ANDROID__
// --touch on a PC: the left mouse button is a finger. Press and release come
// from Panda's events (queued, so a quick click is not lost between frames);
// while it is down, the finger follows the mouse.
bool mousePixels(AppState &app, float &x, float &y) {
    GraphicsWindow *win = app.window ? app.window->get_graphics_window() : nullptr;
    if (app.mouse == nullptr || win == nullptr || !app.mouse->has_mouse() || !win->has_size()) return false;
    const LPoint2 m = app.mouse->get_mouse();
    x = (static_cast<float>(m[0]) + 1.0f) * 0.5f * win->get_x_size();
    y = (1.0f - static_cast<float>(m[1])) * 0.5f * win->get_y_size();
    return true;
}

void onMouseDown(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    float x, y;
    if (!app->options.touch || app->mouseFingerDown || !mousePixels(*app, x, y)) return;
    app->mouseFingerDown = true;
    platform::pushTouch({platform::Touch::Kind::Down, 0, x, y});
}

void onMouseUp(const Event *, void *data) {
    auto *app = static_cast<AppState *>(data);
    if (!app->mouseFingerDown) return;
    app->mouseFingerDown = false;
    float x = 0, y = 0;
    mousePixels(*app, x, y);
    platform::pushTouch({platform::Touch::Kind::Up, 0, x, y});
}

void mouseAsFinger(AppState &app) {
    float x, y;
    if (app.mouseFingerDown && mousePixels(app, x, y)) {
        platform::pushTouch({platform::Touch::Kind::Move, 0, x, y});
    }
}
#endif

// ------------------------------------------------------------------ frame

void menuFrame(AppState &app, double now, float dt, const std::vector<platform::Touch> &touches) {
    for (const platform::Touch &t : touches) app.menu->handle(t);
    if (app.discovery.update(now)) app.menu->setServers(app.discovery.servers());
    app.menu->update(now);
    if (app.world) {
        // The camera drifts slowly around the meadow.
        const float a = static_cast<float>(now) * 0.05f;
        app.world->update(now, dt, true, std::cos(a) * 8.0f, std::sin(a) * 8.0f);
    }
    std::string address;
    if (app.menu->takeJoin(address)) joinFromMenu(app, address);
    if (platform::takeBack()) app.quit = true;
}

void gameFrame(AppState &app, double now, double dt, const std::vector<platform::Touch> &touches) {
    game::PlayerInput input = readInput(app);
    if (app.touch) {
        for (const platform::Touch &t : touches) app.touch->handle(t);
        const game::TouchControls::Output out = app.touch->take();
        if (out.x != 0.0f || out.y != 0.0f) {
            input.x = out.x;
            input.y = out.y;
        }
        input.dash = input.dash || out.dash;
        if (out.zoomSteps != 0.0f && app.world) app.world->zoom(out.zoomSteps);
        if (out.toggleStats && app.hud) app.hud->toggleStats();
    }
    if (!platform::visible()) input = game::PlayerInput();  // in the background
    app.game->update(now, dt, input);
    if (app.touch) app.touch->update(now, app.game->dashReady());

    if (app.menu) {
        // Back, or a server that sent us away / never answered: the menu.
        const auto phase = app.game->phase();
        if (platform::takeBack()) {
            openMenu(app, "");
        } else if (phase == game::GameClient::Phase::Rejected) {
            openMenu(app, "The server said: " + app.game->rejectReason());
        } else if (!app.game->counters().everPlayed && now - app.gameStartedAt > kJoinTimeout) {
            openMenu(app, "No answer from " + app.options.host + ":" + std::to_string(app.options.port) +
                              ". Is the server running, and on this network?");
        }
    } else if (platform::takeBack()) {
        app.quit = true;
    }
}

AsyncTask::DoneStatus frameTask(GenericAsyncTask *, void *data) {
    auto &app = *static_cast<AppState *>(data);
    platform::frame();
    if (platform::quitRequested()) app.quit = true;
    ClockObject *clock = ClockObject::get_global_clock();
    if (app.window == nullptr) {
        // Normally GraphicsEngine::render_frame() ticks the clock; headless
        // there is no engine, so we do it (this also applies clock-frame-rate).
        clock->tick();
    }
    const double now = clock->get_frame_time();
    const double dt = clock->get_dt();

    int width = 0, height = 0;
    if (app.window != nullptr) {
        GraphicsWindow *win = app.window->get_graphics_window();
        if (win != nullptr && win->has_size() && win->get_y_size() > 0) {
            width = win->get_x_size();
            height = win->get_y_size();
            app.hud->setAspect(static_cast<float>(width) / height);
            if (app.touch) app.touch->setWindow(width, height, app.uiScale);
            if (app.menu) app.menu->setWindow(width, height, app.uiScale);
        }
    }
#ifndef __ANDROID__
    if (app.options.touch) mouseAsFinger(app);
#endif
    std::vector<platform::Touch> touches = platform::takeTouches();
    if (app.window != nullptr) {
        // In the background (Android: paused, or without a surface) the window
        // is not drawn; Panda still reads its events, so it comes back.
        if (GraphicsWindow *win = app.window->get_graphics_window()) win->set_active(platform::visible());
    }
    if (!platform::visible()) {
        touches.clear();
        if (app.touch) app.touch->releaseAll();
        // Nothing is drawn in the background: do not spin.
        Thread::sleep(0.05);
    }

    if (app.game) {
        gameFrame(app, now, dt, touches);
    } else if (app.menu) {
        menuFrame(app, now, static_cast<float>(dt), touches);
    }

    if (app.world && !app.shadersVerified && clock->get_frame_count() > 2) {
        app.shadersVerified = true;  // shaders compile on first use: check once they did
        app.world->verifyShaders();
    }

    const Options &o = app.options;
    if (o.duration > 0.0 && now - app.startTime >= o.duration) {
        app.quit = true;
    }
    if (!o.screenshot.empty() && !app.screenshotTaken && app.window != nullptr &&
        ((app.game && app.game->playingSince() >= 0.0 && now - app.game->playingSince() >= o.screenshotAfter) ||
         (!app.game && app.menu && now - app.startTime >= o.screenshotAfter))) {
        app.screenshotTaken = true;
        GraphicsWindow *win = app.window->get_graphics_window();
        const Filename file = Filename::from_os_specific(o.screenshot);
        if (win != nullptr && win->save_screenshot(file)) {
            LOG_INFO("saved screenshot %s", o.screenshot.c_str());
        } else {
            LOG_ERROR("could not save screenshot %s", o.screenshot.c_str());
        }
        if (o.duration <= 0.0) app.quit = true;
    }
    if (app.game && app.game->phase() == game::GameClient::Phase::Rejected && o.headless) {
        app.quit = true;
    }
    if (app.quit) {
        app.framework.set_exit_flag();
    }
    return AsyncTask::DS_cont;
}

std::string randomName() {
    std::random_device rd;
    return "Player" + std::to_string(std::uniform_int_distribution<int>(100, 999)(rd));
}

} // namespace

int runApp(Options options, int argc, char **argv) {
    setLogLevel(options.verbose ? LogLevel::Debug : LogLevel::Info);
    configure(options);
    if (!mountResources(options)) {
        return kExitInitFailed;
    }

    auto app = std::make_unique<AppState>();
    // Remembered between runs where there is no command line (Android).
    const std::string settingsFile = platform::settingsFile();
    if (!settingsFile.empty()) {
        app->settings = std::make_unique<core::Settings>(settingsFile);
        app->settings->load();
        if (options.name.empty()) {
            options.name = app->settings->get("name");
            if (options.name.empty()) {
                options.name = randomName();
                app->settings->set("name", options.name);
                app->settings->save();
            }
        }
    }
    if (options.touch && options.name.empty()) {
        options.name = randomName();  // shown on the connect screen
    }
    app->options = options;
    initGraphicsModules();
    app->framework.open_framework(argc, argv);
    app->framework.set_window_title("Panda Arena");

    if (!options.headless) {
        WindowProperties props = WindowProperties::get_default();
        props.set_size(options.width, options.height);
        props.set_title("Panda Arena");
        props.set_fullscreen(options.fullscreen);

        // PandaFramework always opens windows with FrameBufferProperties::
        // get_default(), which is computed once from the PRC files and cached.
        // It is a reference to a non-const static, so we adjust it between
        // attempts: MSAA first, then without, then accepting a software
        // OpenGL framebuffer (VMs and remote desktops have no GPU driver; the
        // SDK's Config.prc demands hardware with framebuffer-hardware #t).
        auto &fb = const_cast<FrameBufferProperties &>(FrameBufferProperties::get_default());
        // BF_fb_props_optional: take the closest framebuffer when the exact
        // request (8-bit colour, 24-bit depth, see configure()) is not on
        // offer, instead of abandoning the window.
        const int closestFb = GraphicsPipe::BF_require_window | GraphicsPipe::BF_fb_props_optional;
#ifdef __ANDROID__
        // Android: one attempt, and it has to work. The system hands its
        // window to the first one; Panda's Android window cannot open again
        // after a failed attempt (no APP_CMD_INIT_WINDOW arrives a second
        // time, so it never gets a surface). So no MSAA, the closest config,
        // and no insisting on "hardware" (Android decides that from the GPU's
        // name afterwards; only the old PixelFlinger counts as software).
        fb.set_multisamples(0);
        fb.set_force_hardware(false);
        app->window = app->framework.open_window(props, closestFb);
        if (app->window == nullptr) {
            LOG_ERROR("could not open the window (OpenGL ES)");
            app->framework.close_framework();
            return kExitInitFailed;
        }
        options.msaa = false;
        LOG_INFO("window opened (OpenGL ES, no MSAA)");
#else
        const bool configuredHardware = fb.get_force_hardware();
        struct Attempt {
            bool msaa, software;
            const char *what;
        };
        const Attempt attempts[] = {{true, false, "with 4x MSAA"},
                                    {false, false, "without MSAA"},
                                    {false, true, "with a software OpenGL framebuffer"}};
        for (const Attempt &a : attempts) {
            if (a.msaa && !options.msaa) continue;
            if (a.software && !configuredHardware) continue;  // already allowed
            fb.set_multisamples(a.msaa ? 4 : 0);
            fb.set_force_hardware(!a.software && configuredHardware);
            // The MSAA attempt insists (else the next one is the fallback);
            // the others take the closest framebuffer.
            app->window = app->framework.open_window(props, a.msaa ? 0 : closestFb);
            if (app->window != nullptr) {
                LOG_INFO("window opened %s", a.what);
                if (a.software) {
                    LOG_WARN("no hardware OpenGL: rendering in software; use --low if it is slow");
                }
                options.msaa = a.msaa;
                break;
            }
            LOG_WARN("could not open a window %s", a.what);
        }
        if (app->window == nullptr) {
            LOG_ERROR("could not open a window (no OpenGL driver?); try --low or --headless");
            app->framework.close_framework();
            return kExitInitFailed;
        }
#endif
        platform::attachWindow();
        GraphicsWindow *win = app->window->get_graphics_window();
        if (win != nullptr) {
            LOG_INFO("renderer: %s", win->get_gsg()->get_driver_renderer().c_str());
            // What the driver gave (the request is only the closest match on
            // phones): 16-bit colour or depth would show as banding and
            // flicker.
            std::ostringstream fbText;
            win->get_fb_properties().output(fbText);
            LOG_INFO("framebuffer: %s", fbText.str().c_str());
        }
        app->window->enable_keyboard();
        app->window->set_background_type(WindowFramework::BT_other);
        if (DisplayRegion *dr = app->window->get_display_region_3d()) {
            dr->set_clear_color_active(true);
            dr->set_clear_color(LColor(0.74f, 0.86f, 0.95f, 1.0f));  // the hazy horizon
        }
        if (Camera *cam = app->window->get_camera(0)) {
            if (Lens *lens = cam->get_lens()) {
                lens->set_fov(62.0f);
                // The camera stays 15 m or more from what it looks at, and the
                // haze hides everything beyond ~200 m: a near plane at 1 m
                // (not 0.5) doubles the depth precision, against flicker.
                lens->set_near_far(1.0f, 600.0f);
            }
        }
        NodePath render = app->window->get_render();
        if (options.msaa) {
            render.set_antialias(AntialiasAttrib::M_multisample);
        }
        game::WorldSettings ws;
        ws.shaders = !options.lowQuality;
        ws.shadows = options.shadows && !options.lowQuality;
        ws.shaderDir = options.resourceShaderDir;
#ifdef __ANDROID__
        ws.shadowSize = 1024;  // phones: a quarter of the fill rate, still crisp
#endif
        if (GraphicsStateGuardian *gsg = win != nullptr ? win->get_gsg() : nullptr) {
            // (If GLSL does not compile, World::verifyShaders() falls back too.)
            if (ws.shaders && gsg->is_valid() && !gsg->get_supports_glsl()) {
                LOG_WARN("the graphics driver has no GLSL: fixed-function lighting, no shadow maps");
                ws.shaders = false;
            }
        }
        if (!ws.shaders) {
            ws.shadows = false;  // World falls back to blob shadows
        }
        app->world = std::make_unique<game::World>(render, app->window->get_camera_group(), ws);
        app->world->setZoom(options.zoom);
        app->uiScale = options.uiScale > 0.0f ? options.uiScale : platform::uiScale();
        app->hud = std::make_unique<game::Hud>(app->window->get_aspect_2d());
        app->hud->setUiScale(app->uiScale);
        app->mouse = DCAST(MouseWatcher, app->window->get_mouse().node());
        if (options.touch) {
            app->hud->setTouchMode(true);
            app->touch = std::make_unique<game::TouchControls>(app->window->get_aspect_2d());
            if (!options.hostGiven) {
                std::string address = app->settings ? app->settings->get("server") : std::string();
                app->menu = std::make_unique<game::ConnectScreen>(app->window->get_aspect_2d(), address,
                                                                  options.name);
            }
        }

        PandaFramework &fw = app->framework;
        void *data = app.get();
        fw.define_key("escape", "quit / close chat", onEscape, data);
        fw.define_key("enter", "chat", onEnter, data);
        fw.define_key(app->hud->chatAcceptEvent(), "send chat", onChatAccept, data);
        fw.define_key("space", "dash", onSpace, data);
        fw.define_key("f1", "help", onHelp, data);
        fw.define_key("f3", "network stats", onStats, data);
        fw.define_key("f11", "fullscreen", onFullscreen, data);
        fw.define_key("wheel_up", "zoom in", onWheelUp, data);
        fw.define_key("wheel_down", "zoom out", onWheelDown, data);
#ifndef __ANDROID__
        if (options.touch) {
            fw.define_key("mouse1", "finger down", onMouseDown, data);
            fw.define_key("mouse1-up", "finger up", onMouseUp, data);
        }
#endif
    }

    if (app->menu) {
        app->startTime = ClockObject::get_global_clock()->get_frame_time();
        showMenuScenery(*app);  // later the last server's arena stays behind the menu
        openMenu(*app, "");
    } else {
        std::string error;
        if (!startGame(*app, error)) {
            LOG_ERROR("%s", error.c_str());
            app->touch.reset();
            app->hud.reset();
            app->world.reset();
            app->framework.close_framework();
            return kExitInitFailed;
        }
        app->startTime = ClockObject::get_global_clock()->get_frame_time();
    }

    // After input (data_loop, sort -50) and events, before rendering (igLoop).
    PT(GenericAsyncTask) task = new GenericAsyncTask("arena-frame", frameTask, app.get());
    task->set_sort(10);
    app->framework.get_task_mgr().add(task);

    Thread *thread = Thread::get_current_thread();
    while (app->framework.do_frame(thread)) {
    }

    stopGame(*app);
    app->discovery.stop();
    const auto counters = app->counters;
    const auto phase = app->phase;
    const std::string rejectReason = app->rejectReason;
    const bool menuOnly = app->menu != nullptr;
    app->framework.get_task_mgr().remove(task);
    app->menu.reset();
    app->touch.reset();
    app->hud.reset();
    app->world.reset();
    app->framework.close_framework();

    const double secs = counters.playSeconds > 0.0 ? counters.playSeconds : 1.0;
    std::printf("summary: played=%d others=%d orbs=%d pongs=%d chats=%d hits=%d/%d corrections=%d reconnects=%d"
                " arena_rebuilds=%d ping_avg_ms=%.1f ping_max_ms=%.1f dashes=%d contacts=%d"
                " confirmed=%d missed=%d phantom=%d pos_sends_s=%.1f up_Bps=%.0f down_Bps=%.0f fps=%.1f\n",
                counters.everPlayed ? 1 : 0, counters.maxOtherPlayers, counters.orbsCollected, counters.pongs,
                counters.chatsReceived, counters.hitsGiven, counters.hitsTaken, counters.corrections,
                counters.reconnects, counters.arenaRebuilds,
                counters.pongs ? counters.pingSumMs / counters.pongs : 0.0,
                counters.pingMaxMs, counters.dashes, counters.dashContacts, counters.contactsConfirmed,
                counters.contactsMissed, counters.phantomHits, counters.positionSends / secs,
                counters.bytesOut / secs, counters.bytesIn / secs, counters.frames / secs);
    std::fflush(stdout);

    if (menuOnly) {
        return kExitOk;  // the player left from the connect screen
    }
    if (phase == game::GameClient::Phase::Rejected) {
        LOG_ERROR("rejected by the server: %s", rejectReason.c_str());
        return kExitRejected;
    }
    if (!counters.everPlayed) {
        return kExitNeverJoined;
    }
    if (options.expectPlayers >= 0 && counters.maxOtherPlayers < options.expectPlayers) {
        LOG_ERROR("saw %d other players, expected %d", counters.maxOtherPlayers, options.expectPlayers);
        return kExitExpectationFailed;
    }
    return kExitOk;
}

} // namespace arena
