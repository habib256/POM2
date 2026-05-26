// POM2 Apple II Emulator
// Copyright (C) 2026

#include "CliDispatcher.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "MainWindow.h"
// MainWindow.h now forward-declares EmulationController and Apple2Display
// to keep its include cone lean. main.cpp dereferences both via
// MainWindow::emul() / displayRef() so it needs the full types.
#include "Apple2Display.h"
#include "EmulationController.h"
#include "CassetteDevice.h"
#include "Disk35Image.h"
#include "ResourcePaths.h"
#include "SystemProfile.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void glfw_char_callback(GLFWwindow* w, unsigned int codepoint)
{
    ImGui_ImplGlfw_CharCallback(w, codepoint);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Skip the codepoint when ImGui is capturing keyboard (a text
        // input field, etc.). Otherwise the Apple II eats every keystroke
        // even when the user is editing a control widget.
        if (!ImGui::GetIO().WantCaptureKeyboard) mw->onChar(codepoint);
    }
}

static void glfw_key_callback(GLFWwindow* w, int key, int sc, int action, int mods)
{
    ImGui_ImplGlfw_KeyCallback(w, key, sc, action, mods);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // F11 (soft reset) and F12 (hard reset) are routed unconditionally
        // so the user can recover even when an ImGui widget has captured
        // the keyboard focus. F9 (screenshot) is routed the same way so
        // a screenshot can be triggered from a focused control widget.
        // Left/Right Alt = Open-Apple/Solid-Apple — routed unconditionally
        // so the IIe/IIc/IIc+ firmware can observe consistent press/release
        // edges via $C061/$C062 regardless of ImGui focus state.
        const bool isGlobalKey = (key == GLFW_KEY_F11 || key == GLFW_KEY_F12 ||
                                  key == GLFW_KEY_F9 ||
                                  key == GLFW_KEY_LEFT_ALT ||
                                  key == GLFW_KEY_RIGHT_ALT);
        if (!ImGui::GetIO().WantCaptureKeyboard || isGlobalKey) {
            mw->onKey(key, sc, action, mods);
        }
    }
}

static void glfw_cursor_pos_callback(GLFWwindow* w, double x, double y)
{
    // Forward to ImGui first so it tracks the cursor for hover detection.
    ImGui_ImplGlfw_CursorPosCallback(w, x, y);
    // Then route to the Apple II Mouse Card. MainWindow gates on
    // whether the cursor is inside the Apple II Screen widget rect —
    // outside, this is a no-op and ImGui handles the cursor normally.
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        mw->onMouseMove(x, y);
    }
}

static void glfw_mouse_button_callback(GLFWwindow* w, int button, int action, int mods)
{
    ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
    if (auto* mw = static_cast<MainWindow*>(glfwGetWindowUserPointer(w))) {
        // Always forward to MainWindow — it does its own
        // cursor-in-screen-rect check (same gate as the cursor-position
        // callback). The `WantCaptureMouse` gate is too coarse: the
        // Apple II Screen is itself an ImGui window, so clicks inside
        // the screen widget would otherwise be swallowed by ImGui.
        mw->onMouseButton(button, action);
    }
}

int main(int argc, char* argv[])
{
    pom2::log().info("POM2", "v0.6 - Apple II Emulator (Dear ImGui)");

    bool helpRequested = false;
    auto plan = pom2::parseCli(argc, argv, helpRequested);
    if (helpRequested) return 0;
    if (!plan)         return 1;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return -1;

#ifdef __EMSCRIPTEN__
    // WebGL2 ≈ OpenGL ES 3.0. ImGui's OpenGL3 backend selects shader
    // source variant from the GLSL version string — desktop "#version
    // 150" produces shaders WebGL2 can't compile, so ImGui silently
    // draws nothing (CPU + audio keep running → black canvas symptom).
    // CMake forces -sUSE_WEBGL2 / MIN=MAX=2 so the actual context is
    // always WebGL2 regardless of these hints; we still set them so
    // GLFW's Emscripten port doesn't fight us. GLFW_ALPHA_BITS=0 → the
    // canvas is opaque, otherwise the page background bleeds through
    // wherever we don't draw.
    const char* glsl_version = "#version 300 es";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_ALPHA_BITS, 0);
#else
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
#endif

    // 1568×850 matches the curated default layout (2026-05-15):
    // Apple II Screen on the left ~1115 px, unified Disk Library
    // (5.25 / 3.5 / HDV tabs) on the right ~435 px. Per-card panels
    // (Disk II / HDV / Disk 3.5") stay hidden by default — the user
    // opens them on demand via Devices menu / Slot Configuration.
    // The `FirstUseEver` pos/size calls in MainWindow rely on roughly
    // this canvas.
    const char* kWindowTitle = "POM2 v0.6 - Apple II Emulator";
    GLFWwindow* window = nullptr;
    if (plan->kiosk) {
        // Exclusive full-screen on the primary monitor at its current
        // video mode (no resolution change beyond what the mode dictates).
        // Copying the mode's bit depths + refresh into the hints is the
        // GLFW-recommended way to request a "windowed full screen" that
        // doesn't force a mode switch.
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
        if (mon && vm) {
            glfwWindowHint(GLFW_RED_BITS,     vm->redBits);
            glfwWindowHint(GLFW_GREEN_BITS,   vm->greenBits);
            glfwWindowHint(GLFW_BLUE_BITS,    vm->blueBits);
            glfwWindowHint(GLFW_REFRESH_RATE, vm->refreshRate);
            window = glfwCreateWindow(vm->width, vm->height, kWindowTitle,
                                      mon, nullptr);
            pom2::log().info("CLI", "--kiosk: full-screen " +
                std::to_string(vm->width) + "x" + std::to_string(vm->height));
        }
        if (!window) {
            pom2::log().warn("CLI",
                "--kiosk: no primary monitor / video mode — falling back "
                "to a windowed canvas");
        }
    }
    if (!window) {
        window = glfwCreateWindow(1568, 850, kWindowTitle, nullptr, nullptr);
    }
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    // Note: ImGui's global `ConfigWindowsMoveFromTitleBarOnly` would be
    // the obvious knob to make the Apple II Screen content-area click-
    // through, but the user wants only THAT window restricted (others
    // keep the comfort of drag-from-anywhere). We instead apply
    // `ImGuiWindowFlags_NoMove` to the Apple II Screen and roll our
    // own title-bar drag inside `MainWindow::renderScreenWindow`.
    // Persist ImGui window positions / sizes / docking state to
    // ~/.config/POM2/imgui.ini (mirrors Settings::resolveStorePath).
    // First launch sees no file → `FirstUseEver` defaults from
    // `renderScreenWindow`/`renderDiskPanelWindow`/`renderHdvPanelWindow`
    // pick the curated layout (Screen top-left, HDV bottom-left, Disk II
    // right column). Subsequent launches restore whatever the user
    // dragged into place. Falls back to the cwd `imgui.ini` if $HOME
    // isn't set (matches Settings's fallback path).
    if (plan->kiosk) {
        // Kiosk has no movable windows, and we don't want it to overwrite
        // the GUI layout the user curated — disable .ini persistence.
        io.IniFilename = nullptr;
    } else {
        static std::string iniPath;
        namespace fs = std::filesystem;
        if (const char* home = std::getenv("HOME"); home && *home) {
            fs::path xdg = fs::path(home) / ".config" / "POM2";
            std::error_code ec;
            fs::create_directories(xdg, ec);
            if (!ec && fs::is_directory(xdg, ec)) {
                iniPath = (xdg / "imgui.ini").string();
            } else {
                iniPath = (fs::path(home) / ".pom2_imgui.ini").string();
            }
        } else {
            iniPath = "imgui.ini";
        }
        io.IniFilename = iniPath.c_str();
    }
    ImGui::StyleColorsDark();

    // Base UI font — Proggy Clean (ImGui's default) is a bitmap that
    // only covers ASCII, so anything past U+007F (em-dash "—", en-dash
    // "–", curly quotes, accented letters used in POM2's localised
    // strings, ellipsis "…") renders as '?'. Try to load a real TTF
    // with Latin-1 Supplement + a handful of General Punctuation
    // glyphs so the menus / status text / tooltips read properly.
    //
    // Probe order: bundled font (none today), then a few common
    // distribution paths for DejaVu Sans (Debian/Ubuntu, Fedora,
    // Arch). Last resort = AddFontDefault → users see '?' for the
    // missing glyphs but the rest of the UI still works.
    //
    // The explicit SizePixels matters: since ImGui added the
    // ImFontFlags_ImplicitRefSize check, merging FontAwesome (which
    // requests 14.0f below) into a destination that uses implicit
    // ref size hard-asserts at AddFont. Both branches here pick an
    // explicit size for that reason.
    {
        namespace fs = std::filesystem;
        const char* baseCandidates[] = {
            // POM2-bundled (drop a TTF into fonts/ to override).
            "fonts/DejaVuSans.ttf",
            "../fonts/DejaVuSans.ttf",
            "../../fonts/DejaVuSans.ttf",
            // System locations seen in the wild.
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
            "C:\\Windows\\Fonts\\segoeui.ttf",
        };
        // findResource resolves the bundled "fonts/…" entries against the
        // executable-relative / FHS roots too (so an installed binary finds
        // its TTF), and returns the absolute system paths verbatim when
        // they exist. See ResourcePaths.h.
        std::string basePath;
        for (const char* c : baseCandidates) {
            std::string r = pom2::findResource(c);
            if (!r.empty()) { basePath = r; break; }
        }
        // Latin-1 Supplement + selected General Punctuation. Listed
        // pair-by-pair (ImGui ranges are inclusive [from, to] pairs
        // terminated with 0). Covers: ASCII printables + accented
        // letters (é, à, ç, ü, ö, …), en/em dash, curly single + double
        // quotes, ellipsis, non-breaking space.
        static const ImWchar baseRanges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
            0x2013, 0x2014,   // – (en dash) — (em dash)
            0x2018, 0x201D,   // ' ' " "  (curly quotes)
            0x2022, 0x2022,   // • (bullet)
            0x2026, 0x2026,   // … (ellipsis)
            0x20AC, 0x20AC,   // € (euro sign — used in localised strings)
            0,
        };
        if (!basePath.empty()) {
            ImFontConfig baseCfg;
            baseCfg.SizePixels    = 14.0f;
            baseCfg.OversampleH   = 2;
            baseCfg.OversampleV   = 2;
            baseCfg.PixelSnapH    = false;
            if (!io.Fonts->AddFontFromFileTTF(basePath.c_str(), 14.0f,
                                              &baseCfg, baseRanges)) {
                // TTF load failure → fall back to default so the UI
                // still comes up.
                ImFontConfig defCfg;
                defCfg.SizePixels = 13.0f;
                io.Fonts->AddFontDefault(&defCfg);
            }
        } else {
            std::fprintf(stderr,
                "POM2: no Unicode TTF found (tried DejaVu Sans + system "
                "paths) — em-dashes, curly quotes and accented chars "
                "will render as '?'. Drop DejaVuSans.ttf into fonts/ to "
                "fix.\n");
            ImFontConfig defCfg;
            defCfg.SizePixels = 13.0f;
            io.Fonts->AddFontDefault(&defCfg);
        }
    }
    {
        const char* candidates[] = {
            "fonts/fa-solid-900.ttf",
            "../fonts/fa-solid-900.ttf",
            "../../fonts/fa-solid-900.ttf",
        };
        std::string fontPath;
        for (const char* c : candidates) {
            std::string r = pom2::findResource(c);
            if (!r.empty()) { fontPath = r; break; }
        }
        if (!fontPath.empty()) {
            ImFontConfig iconsConfig;
            iconsConfig.MergeMode        = true;
            iconsConfig.PixelSnapH       = true;
            iconsConfig.GlyphMinAdvanceX = 15.0f;
            static const ImWchar iconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
            if (!io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 14.0f,
                                              &iconsConfig, iconsRanges)) {
                std::fprintf(stderr,
                    "POM2: failed to load %s — deck icons will render as '?'\n",
                    fontPath.c_str());
            }
        } else {
            std::fprintf(stderr,
                "POM2: fa-solid-900.ttf not found in fonts/ — deck icons will render as '?'\n");
        }
    }

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    MainWindow mainWindow(plan->forceIIPlus);
    if (plan->forceIIPlus) {
        pom2::log().info("CLI", "--ii-plus: ignoring apple2e.rom, booting as II+");
    }
    // Hand the GLFW window to MainWindow BEFORE any applyProfile() call so
    // the profile-driven title update (step 13 in applyProfile) sees a
    // valid handle even when --preset triggers the switch.
    mainWindow.setGlfwWindow(window);
    mainWindow.setKioskMode(plan->kiosk);
    // CLI --preset selection (must come AFTER MainWindow's legacy boot so
    // it overrides via the full cold-reset path applyProfile uses).
    if (plan->preset != pom2::CliPreset::Default) {
        pom2::SystemProfile sp = pom2::SystemProfile::AppleIIPlus;
        switch (plan->preset) {
            case pom2::CliPreset::AppleII:      sp = pom2::SystemProfile::AppleII;      break;
            case pom2::CliPreset::AppleIIPlus:  sp = pom2::SystemProfile::AppleIIPlus;  break;
            case pom2::CliPreset::AppleIIeUnenhanced:
                                                sp = pom2::SystemProfile::AppleIIeUnenhanced; break;
            case pom2::CliPreset::AppleIIe:     sp = pom2::SystemProfile::AppleIIe;     break;
            case pom2::CliPreset::AppleIIc:     sp = pom2::SystemProfile::AppleIIc;     break;
            case pom2::CliPreset::AppleIIcPlus: sp = pom2::SystemProfile::AppleIIcPlus; break;
            case pom2::CliPreset::Default: break;
        }
        mainWindow.applyProfile(sp);
    }
    glfwSetWindowUserPointer(window, &mainWindow);
    glfwSetCharCallback(window, glfw_char_callback);
    glfwSetKeyCallback (window, glfw_key_callback);
    glfwSetCursorPosCallback  (window, glfw_cursor_pos_callback);
    glfwSetMouseButtonCallback(window, glfw_mouse_button_callback);

    // ─── Phase B: apply boot-time overrides on the live emulator ─────────
    if (plan->cpuMax) {
        mainWindow.emul().setCyclesPerFrame(1'000'000);
        pom2::log().info("CLI", "--cpu-max: emulator running flat-out");
    }
    if (plan->executionSpeed) {
        mainWindow.emul().setCyclesPerFrame(*plan->executionSpeed);
    }
    if (plan->displayMode != pom2::CliDisplayMode::NoHint) {
        Apple2Display::HiResMode m = Apple2Display::HiResMode::ColorNTSC;
        const char* label = "ntsc";
        switch (plan->displayMode) {
            case pom2::CliDisplayMode::ColorNTSC:
                m = Apple2Display::HiResMode::ColorNTSC;    label = "ntsc";        break;
            case pom2::CliDisplayMode::ChatMauveRGB:
                m = Apple2Display::HiResMode::ChatMauveRGB; label = "chatmauve";   break;
            case pom2::CliDisplayMode::MonoWhite:
                m = Apple2Display::HiResMode::MonoWhite;    label = "mono-white";  break;
            case pom2::CliDisplayMode::MonoGreen:
                m = Apple2Display::HiResMode::MonoGreen;    label = "mono-green";  break;
            case pom2::CliDisplayMode::MonoAmber:
                m = Apple2Display::HiResMode::MonoAmber;    label = "mono-amber";  break;
            case pom2::CliDisplayMode::NoHint: break;
        }
        mainWindow.displayRef().setHiResMode(m);
        pom2::log().info("CLI", std::string("--display ") + label);
    }
    if (!plan->initialTapePath.empty()) {
        if (mainWindow.emul().loadTape(plan->initialTapePath)) {
            pom2::log().info("CLI", "--tape loaded: " + plan->initialTapePath);
            if (plan->initialTapeAutoPlay) mainWindow.emul().playTape();
        } else {
            pom2::log().warn("CLI", "--tape failed: " +
                mainWindow.emul().cassette().getLastError());
        }
    }
    auto mount35Cli = [&](int idx, const std::string& path, const char* flag) {
        if (path.empty()) return;
        // Documented contract: --35-disk1/2 drive the //c+ on-board Sony 3.5"
        // hub, which only exists on the //c+ profile. On any other profile
        // mount35() would silently write into a hub the machine can't read,
        // so warn and ignore (CliDispatcher.h). Slot SmartPort 3.5" cards on
        // //e/II+ are mounted through the GUI/Library, not this flag.
        if (mainWindow.currentProfile() != pom2::SystemProfile::AppleIIcPlus) {
            pom2::log().warn("CLI", std::string(flag) +
                " ignored: 3.5\" disks require the //c+ profile (--preset iic+)");
            return;
        }
        if (mainWindow.emul().mount35(idx, path)) {
            pom2::log().info("CLI", std::string(flag) + " mounted: " + path);
        } else {
            const auto& img = (idx == 0)
                ? mainWindow.emul().disk35Internal()
                : mainWindow.emul().disk35External();
            pom2::log().warn("CLI",
                std::string(flag) + " failed: " + img.lastError());
        }
    };
    mount35Cli(0, plan->disk35Internal, "--35-disk1");
    mount35Cli(1, plan->disk35External, "--35-disk2");

    // ─── Phase C deferred actions: kick off in a background thread that
    // sleeps briefly (let the worker thread + first render frame land)
    // then runs every action in order. The thread is TRACKED (not
    // detached) so we can join before MainWindow goes out of scope —
    // otherwise a fast-quit user could close the window in <250 ms and
    // the lambda would dereference a destroyed EmulationController. The
    // `deferredCancelled` flag lets fast-quit skip the actions entirely
    // (sleep loop polls it every 10 ms) so shutdown stays snappy.
    std::atomic<bool>  deferredCancelled{false};
#ifndef __EMSCRIPTEN__
    std::thread        deferredThread;
    if (!plan->deferredActions.empty()) {
        deferredThread = std::thread(
            [actions = plan->deferredActions,
             emu     = &mainWindow.emul(),
             cancel  = &deferredCancelled] {
                for (int i = 0; i < 25; ++i) {
                    if (cancel->load(std::memory_order_acquire)) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (cancel->load(std::memory_order_acquire)) return;
                pom2::runDeferredActions(actions, *emu);
            });
    }
#endif

    // Optional auto-boot path for capturing traces / repro headless-ish.
    // POM2_AUTO_BOOT_HDV=<N>  → after N seconds (default 1), call
    //                          bootHdvImage() on the main UI thread.
    // POM2_AUTO_QUIT=<N>      → after N seconds, glfwSetWindowShouldClose
    //                          so the binary exits cleanly without state.cfg
    //                          stomping (state IS saved on clean exit).
    std::atomic<bool> autoBootRequested{false};
    std::atomic<bool> autoQuitRequested{false};
#ifndef __EMSCRIPTEN__
    std::thread       autoBootThread;
    {
        const char* abEnv = std::getenv("POM2_AUTO_BOOT_HDV");
        const char* aqEnv = std::getenv("POM2_AUTO_QUIT");
        if (abEnv || aqEnv) {
            const int abDelay = abEnv ? std::max(0, std::atoi(abEnv)) : -1;
            const int aqDelay = aqEnv ? std::max(0, std::atoi(aqEnv)) : -1;
            autoBootThread = std::thread([&, abDelay, aqDelay]() {
                if (abDelay >= 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(abDelay > 0 ? abDelay : 1));
                    autoBootRequested.store(true);
                }
                if (aqDelay >= 0) {
                    std::this_thread::sleep_for(std::chrono::seconds(aqDelay));
                    autoQuitRequested.store(true);
                }
            });
        }
    }
#endif

    // Positional disk image → mount + boot once the worker thread and the
    // first few frames have settled (slot cards plugged, CPU running). A
    // small frame countdown keeps this on the UI thread between frames, so
    // the SlotBus mutation in insertAndBootImage() doesn't race the worker.
    // Works in both GUI and --kiosk mode (bare `POM2 disk` boots in GUI).
    int cliBootCountdown = plan->bootDiskPath.empty() ? -1 : 30;

    // Loop iteration packaged as a function so the native path can stay a
    // plain `while`, and the WASM path can hand it to
    // `emscripten_set_main_loop_arg` (which forbids a blocking loop on
    // the main thread — the browser owns the frame schedule there).
    struct FrameCtx {
        GLFWwindow*         window;
        MainWindow*         mainWindow;
        std::string         bootDiskPath;
        int                 cliBootCountdown;
        std::atomic<bool>*  autoBootRequested;
        std::atomic<bool>*  autoQuitRequested;
    } frameCtx{
        window, &mainWindow, plan->bootDiskPath, cliBootCountdown,
        &autoBootRequested, &autoQuitRequested
    };
    auto iterate = [](void* userdata) {
        auto& c = *static_cast<FrameCtx*>(userdata);
        glfwPollEvents();
#ifdef __EMSCRIPTEN__
        // Single-threaded build: drive the CPU from the render loop
        // since there's no worker thread on this host. Run one frame's
        // worth of cycles before drawing so input → CPU → display
        // pipeline still updates every frame.
        c.mainWindow->emul().tickFrame();
#endif

        if (c.cliBootCountdown > 0 && --c.cliBootCountdown == 0) {
            std::string err;
            if (c.mainWindow->insertAndBootImage(c.bootDiskPath, err)) {
                pom2::log().info("CLI", "booted disk: " + c.bootDiskPath);
            } else {
                pom2::log().warn("CLI", "disk boot failed: " + err);
            }
        }

        if (c.autoBootRequested->exchange(false)) {
            c.mainWindow->bootHdvImage();
        }
        if (c.autoQuitRequested->load()) {
            glfwSetWindowShouldClose(c.window, GLFW_TRUE);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        c.mainWindow->render();

        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(c.window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(c.window);
    };

#ifdef __EMSCRIPTEN__
    // `1` = simulate_infinite_loop → the runtime throws to terminate
    // main() so the captured FrameCtx and surrounding locals stay alive
    // for the lifetime of the browser tab. fps=0 → browser-driven RAF
    // cadence; WebGL swap takes care of vsync.
    // Captureless lambda decays to `void(*)(void*)` (em_arg_callback_func).
    emscripten_set_main_loop_arg(iterate, &frameCtx, 0, 1);
#else
    while (!glfwWindowShouldClose(window)) {
        iterate(&frameCtx);
    }
#endif

    // Stop the deferred-actions worker before any destructors run.
    // Signal cancellation so a thread still in its 250 ms wakeup window
    // exits promptly instead of running actions on the about-to-be-
    // destroyed emulator.
    deferredCancelled.store(true, std::memory_order_release);
#ifndef __EMSCRIPTEN__
    if (deferredThread.joinable()) deferredThread.join();
    if (autoBootThread.joinable()) autoBootThread.join();
#endif

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
