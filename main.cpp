// POM2 Apple II Emulator
// Copyright (C) 2026

#include "CliDispatcher.h"
#include "IconsFontAwesome6.h"
#include "Logger.h"
#include "MainWindow.h"

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
        const bool isGlobalKey = (key == GLFW_KEY_F11 || key == GLFW_KEY_F12 ||
                                  key == GLFW_KEY_F9);
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
    pom2::log().info("POM2", "v0.5 - Apple II Emulator (Dear ImGui)");

    bool helpRequested = false;
    auto plan = pom2::parseCli(argc, argv, helpRequested);
    if (helpRequested) return 0;
    if (!plan)         return 1;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return -1;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // 1770×1000 matches the curated default layout (Apple II Screen on
    // the left ~1080 px, Disk II at x=1095, HDV at x=1430, Emulation
    // stacked below Disk II). The FirstUseEver pos/size calls in
    // MainWindow rely on roughly this canvas — height is sized for the
    // 2×-tall Disk II / HDV library lists.
    GLFWwindow* window = glfwCreateWindow(1770, 1000, "POM2 v0.5 - Apple II Emulator", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    // Persist ImGui window positions / sizes / docking state to
    // ~/.config/POM2/imgui.ini (mirrors Settings::resolveStorePath).
    // First launch sees no file → `FirstUseEver` defaults from
    // `renderScreenWindow`/`renderDiskPanelWindow`/`renderHdvPanelWindow`
    // pick the curated layout (Screen top-left, HDV bottom-left, Disk II
    // right column). Subsequent launches restore whatever the user
    // dragged into place. Falls back to the cwd `imgui.ini` if $HOME
    // isn't set (matches Settings's fallback path).
    {
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

    // Default ImGui font — we still want it as the base, then merge Font
    // Awesome 6 Solid icons on top so the cassette deck's toolbar buttons
    // (folder, save, eraser, volume, mute) render as glyphs instead of '?'.
    io.Fonts->AddFontDefault();
    {
        namespace fs = std::filesystem;
        const char* candidates[] = {
            "fonts/fa-solid-900.ttf",
            "../fonts/fa-solid-900.ttf",
            "../../fonts/fa-solid-900.ttf",
        };
        std::string fontPath;
        for (const char* c : candidates) {
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) { fontPath = c; break; }
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
    // CLI --preset selection (must come AFTER MainWindow's legacy boot so
    // it overrides via the full cold-reset path applyProfile uses).
    if (plan->preset != pom2::CliPreset::Default) {
        pom2::SystemProfile sp = pom2::SystemProfile::AppleIIPlus;
        switch (plan->preset) {
            case pom2::CliPreset::AppleII:      sp = pom2::SystemProfile::AppleII;      break;
            case pom2::CliPreset::AppleIIPlus:  sp = pom2::SystemProfile::AppleIIPlus;  break;
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

    // ─── Phase C deferred actions: kick off in a background thread that
    // sleeps briefly (let the worker thread + first render frame land)
    // then runs every action in order. The thread is TRACKED (not
    // detached) so we can join before MainWindow goes out of scope —
    // otherwise a fast-quit user could close the window in <250 ms and
    // the lambda would dereference a destroyed EmulationController. The
    // `deferredCancelled` flag lets fast-quit skip the actions entirely
    // (sleep loop polls it every 10 ms) so shutdown stays snappy.
    std::thread        deferredThread;
    std::atomic<bool>  deferredCancelled{false};
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

    // Optional auto-boot path for capturing traces / repro headless-ish.
    // POM2_AUTO_BOOT_HDV=<N>  → after N seconds (default 1), call
    //                          bootHdvImage() on the main UI thread.
    // POM2_AUTO_QUIT=<N>      → after N seconds, glfwSetWindowShouldClose
    //                          so the binary exits cleanly without state.cfg
    //                          stomping (state IS saved on clean exit).
    std::thread       autoBootThread;
    std::atomic<bool> autoBootRequested{false};
    std::atomic<bool> autoQuitRequested{false};
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

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (autoBootRequested.exchange(false)) {
            mainWindow.bootHdvImage();
        }
        if (autoQuitRequested.load()) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        mainWindow.render();

        ImGui::Render();
        int dw, dh;
        glfwGetFramebufferSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Stop the deferred-actions worker before any destructors run.
    // Signal cancellation so a thread still in its 250 ms wakeup window
    // exits promptly instead of running actions on the about-to-be-
    // destroyed emulator.
    deferredCancelled.store(true, std::memory_order_release);
    if (deferredThread.joinable()) deferredThread.join();
    if (autoBootThread.joinable()) autoBootThread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
