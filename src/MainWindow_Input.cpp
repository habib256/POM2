// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"
#include "MainWindowUiState.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "AudioCoordinator.h"
#include "DebugCoordinator.h"
#include "NetworkCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "StorageCoordinator.h"
#include "AtomicFileReplace.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CassetteDeck_ImGui.h"
#include "Rewind_ImGui.h"
#include "CassetteDevice.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
#include "SoftCardZ80.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "GrapplerCard.h"
#include "NetworkBackend.h"
#include "SlirpNetworkBackend.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "W5100HostSockets.h"
#include "FujiNetCard.h"
#include "FujiNetHost.h"
#include "Uthernet_ImGui.h"
#include "ImageWriter.h"
#include "ImageWriter_ImGui.h"
#include "RomStatus_ImGui.h"
#include "AbstractionLevels_ImGui.h"
#include "Keyboard_ImGui.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "PrinterFeedCursor.h"
#include "Disk35Controller_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "Logger.h"
#include "MemoryViewer_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseGrab.h"
#include "NtscPostProcessor.h"
#include "CrtEffectStack.h"
#include "Voxel3DRenderer.h"
#include "CffaCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "IconsFontAwesome6.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "PrinterScreenDump.h"
#include "PrinterHistory.h"
#include "PrinterSoundDevice.h"
#include "SmartPort_ImGui.h"
#include "FujiNet_ImGui.h"
#include "SpeakerDevice.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SpOverSlipLink.h"
#include "SystemProfile.h"
#include "Toolbar_ImGui.h"
#include "CommandPalette_ImGui.h"

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar (status bar)
#include "Pom2GL.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

// Physical MainWindow split: screen capture, host input and texture upload.

void MainWindow::saveScreenshot()
{
    // Snapshot the framebuffer under BOTH locks. stateMutex keeps the
    // renderer from resizing the buffer mid-copy; demodMutex is what makes
    // `pixels()` safe — on ColorCompositeOE it lazily runs
    // finishPendingCpuDemod() → renderCompositeOeCpu(), a ~1-2 ms pass that
    // WRITES frame80, and the AI control server's /screen.ppm handler runs
    // the identical demod on its own thread holding only demodMutex. Lock
    // order stateMutex → demodMutex, never the other way (Apple2Display.h).
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        w = display->width();
        h = display->height();
        const uint32_t* src = display->pixels();
        pixels.assign(src, src + w * h);
    }

    // Pick the next unused screenshot_NNN.ppm in the current directory so
    // captures from successive F9 presses don't clobber each other.
    static int lastIdx = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string path;
    while (true) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "screenshot_%03d.ppm", lastIdx);
        path = buf;
        if (!fs::exists(path, ec)) break;
        ++lastIdx;
        if (lastIdx > 999) { lastIdx = 0; break; }
    }

    // PPM "P6" — binary RGB, 1 row per scanline. Apple2Display's pixels
    // are 0xAABBGGRR (RGBA little-endian); strip alpha and swizzle to RGB.
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        uiState_->tapeStatusMessage = "Screenshot: cannot write " + path;
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
        return;
    }
    f << "P6\n" << w << " " << h << "\n255\n";
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint32_t p = pixels[i];
        rgb[i * 3 + 0] = static_cast<uint8_t>( p        & 0xFF);
        rgb[i * 3 + 1] = static_cast<uint8_t>((p >>  8) & 0xFF);
        rgb[i * 3 + 2] = static_cast<uint8_t>((p >> 16) & 0xFF);
    }
    f.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));

    pom2::log().info("Screenshot", "wrote " + path +
                     " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    uiState_->tapeStatusMessage = "Screenshot: " + path;
    uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
    ++lastIdx;
}

// ─── Keyboard ─────────────────────────────────────────────────────────────

void MainWindow::injectAscii(uint8_t apple2Code)
{
    // Not under lockState(), on purpose: queueKey takes `Memory::kbMutex`,
    // the finer-grained lock that lets the UI and the AI server inject keys
    // without contending with the worker on every keystroke. Same for
    // pasteText / pendingPasteSize / cancelPaste and the kiosk key path;
    // the Open/Solid-Apple setters are plain atomics. Those are the only
    // Memory entry points in this file that may be reached unlocked.
    controller->memory().queueKey(apple2Code);
}

void MainWindow::onChar(unsigned int codepoint)
{
    // While the kiosk menu is up, its keyboard fallbacks (K toggles the key
    // band, etc.) are read via ImGui::IsKeyPressed — the same keystroke must
    // not also land in the $C000 latch.
    if (uiState_->kioskMenuOpen) return;
    // In kiosk, K is reserved (Select fallback): the OPEN direction leaks
    // otherwise — this callback fires while the menu is still closed, then
    // updateKioskMenu opens the non-pausing Keys band the same frame, so the
    // running game would receive a live 'k' on every open.
    if (uiState_->kiosk && (codepoint == 'k' || codepoint == 'K')) return;
    // Apple II accepts the full ASCII range (uppercase and lowercase). We
    // forward the codepoint as-is — Applesoft and the Monitor pick whichever
    // case the user typed.
    if (codepoint >= 0x20 && codepoint < 0x80) {
        injectAscii(static_cast<uint8_t>(codepoint));
    }
}

void MainWindow::onKey(int key, int /*scancode*/, int action, int mods)
{
    // Open-Apple / Solid-Apple are read by the IIe/IIc/IIc+ firmware via
    // $C061/$C062 bit 7 (MAME `apple2e.cpp:2157-2169`) — the firmware itself
    // decides cold-reboot vs self-test on Ctrl+Reset. We just source the
    // bits; observe both press and release so the firmware sees the key
    // released after Reset like on real hardware.
    if (key == GLFW_KEY_LEFT_ALT) {
        controller->memory().setOpenAppleKey(action != GLFW_RELEASE);
        return;
    }
    if (key == GLFW_KEY_RIGHT_ALT) {
        controller->memory().setSolidAppleKey(action != GLFW_RELEASE);
        return;
    }

    // Ctrl+Alt+G toggles the Mouse Card pointer capture. Placed above every
    // other branch — including the kiosk-menu gate below — for the same
    // reason F10/F11/F12 are routed unconditionally: a captured pointer with
    // no reachable way out is a trap. PRESS only, so holding the chord can't
    // flip capture ~30×/s on auto-repeat. Note the Left-Alt half also sets
    // Open-Apple (handled above, and cleared when the user lifts it) — the
    // guest sees a modifier press it would have seen anyway.
    // Tested before the Ctrl-letter path further down, which would otherwise
    // also inject Ctrl-G ($07) into the keyboard latch.
    if (pom2::mousegrab::isToggleChord(key, mods)) {
        if (action == GLFW_PRESS) toggleMouseGrab();
        return;
    }

    // Ctrl+Alt+F — the second GUI ⇄ kiosk toggle, alongside F10. Sits with
    // Ctrl+Alt+G above every other branch for the same two reasons: leaving
    // kiosk must ALWAYS work, and the chord has to be tested before the
    // Ctrl-letter path further down or it would also inject Ctrl-F ($06)
    // into the keyboard latch. Matched on either Alt and regardless of
    // Shift/Super (GLFW folds both Alts into GLFW_MOD_ALT), so a stray
    // modifier can never strand a full-screen session. PRESS only: on
    // GLFW_REPEAT a held chord would flip full-screen ⇄ windowed ~30×/s,
    // each flip doing a window-monitor change AND a synchronous
    // settings->save() to disk.
    //
    // Why a second binding at all: F10 is claimed by the window manager on
    // several desktops (GNOME/KDE open the focused window's menu with it),
    // where it never reaches GLFW. A chord in the same family as Ctrl+Alt+G
    // is reachable everywhere.
    if (key == GLFW_KEY_F && (mods & GLFW_MOD_CONTROL) && (mods & GLFW_MOD_ALT)) {
        if (action == GLFW_PRESS) toggleKioskMode();
        return;
    }

    // Kiosk menu open: its arrows/Enter/Esc fallbacks are polled with
    // ImGui::IsKeyPressed and the menu window never captures the keyboard,
    // so everything below would double-deliver — Enter on the key band
    // would send the cell AND inject $0D, Esc would close the menu AND
    // type $1B into the game on resume.
    if (uiState_->kioskMenuOpen) {
        // F10 still leaves kiosk with the in-kiosk menu open — the user
        // must always have a way back to the GUI. (Ctrl+Alt+F is handled
        // above, so it works here too.)
        if (key == GLFW_KEY_F10 && action == GLFW_PRESS) toggleKioskMode();
        return;
    }
    // K reserved in kiosk (see onChar) — also blocks Ctrl-K's $0B, since
    // eSelect fires on the K key regardless of modifiers.
    if (uiState_->kiosk && key == GLFW_KEY_K) return;

    if (action != GLFW_PRESS && action != GLFW_REPEAT) return;
    const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Ctrl-V intercepts the host shortcut: paste system clipboard into
    // the Apple II keyboard buffer rather than injecting raw $16. The
    // Apple II's own Ctrl-V (rarely used) can still be reached via the
    // Edit menu or via Ctrl-Shift-V if a future version chooses to map it.
    if (ctrl && key == GLFW_KEY_V) {
        pasteFromClipboard();
        return;
    }

    // Ctrl+Shift+P opens the command palette. Shift is what keeps it off the
    // Apple II's Ctrl-P ($10) — which CP/M under the SoftCard uses for printer
    // echo, so plain Ctrl-P must keep reaching the guest.
    if (ctrl && (mods & GLFW_MOD_SHIFT) && key == GLFW_KEY_P) {
        openCommandPalette();
        return;
    }

    switch (key) {
        case GLFW_KEY_ENTER:        // fallthrough — main + numpad Enter both
        case GLFW_KEY_KP_ENTER:     injectAscii(0x0D); break;
        case GLFW_KEY_BACKSPACE:    injectAscii(0x08); break;
        case GLFW_KEY_LEFT:         injectAscii(0x08); break;
        case GLFW_KEY_RIGHT:        injectAscii(0x15); break;
        case GLFW_KEY_UP:           injectAscii(0x0B); break;
        case GLFW_KEY_DOWN:         injectAscii(0x0A); break;
        case GLFW_KEY_ESCAPE:       injectAscii(0x1B); break;
        case GLFW_KEY_TAB:          injectAscii(0x09); break;
        case GLFW_KEY_F9:           saveScreenshot(); break;
        // F10 = GUI <-> kiosk (Ctrl+Alt+F does the same, handled above).
        // "Full screen" in the GUI IS kiosk mode:
        // exclusive full-screen with the chrome-free render path. The
        // machine keeps running across the switch (no snapshot needed —
        // kiosk touches only windowing / rendering / settings-writes).
        // PRESS only: this switch also runs for GLFW_REPEAT, and holding
        // F10 would otherwise flip full-screen ⇄ windowed ~30×/s, each
        // entry doing a window-monitor change AND a synchronous
        // settings->save() to disk.
        case GLFW_KEY_F10:
            if (action == GLFW_PRESS) toggleKioskMode();
            break;
        case GLFW_KEY_F11:          controller->softReset(); break;
        case GLFW_KEY_F12:          controller->hardReset(); break;
        default:
            // Ctrl-A..Ctrl-Z generate $01..$1A — these matter for Applesoft
            // (Ctrl-C breaks out of a running program, Ctrl-G beeps, etc.)
            if (ctrl && key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
                injectAscii(static_cast<uint8_t>(key - GLFW_KEY_A + 1));
            }
            break;
    }
}

// ─── Paste ───────────────────────────────────────────────────────────────

void MainWindow::pasteFromClipboard()
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) {
        uiState_->tapeStatusMessage = "Paste: clipboard is empty";
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
        return;
    }
    std::string text = clip;
    if (uiState_->pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars queued from clipboard", queued);
    uiState_->tapeStatusMessage = buf;
    uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
}

void MainWindow::pasteFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        uiState_->tapeStatusMessage = "Paste: cannot open " + path;
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        return;
    }
    std::string text;
    text.resize(Memory::kPasteMaxChars);
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(f.gcount()));
    const bool truncated = f.peek() != std::char_traits<char>::eof();
    if (uiState_->pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars%s from %s", queued,
                  truncated ? " (file truncated)" : "", path.c_str());
    uiState_->tapeStatusMessage = buf;
    uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
}

// ─── Texture upload ──────────────────────────────────────────────────────

void MainWindow::uploadScreenTexture()
{
    if (screenTexture == 0) {
        glGenTextures(1, &screenTexture);
        glBindTexture(GL_TEXTURE_2D, screenTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Initial allocation matches the 280-wide buffer; the real
        // dimensions are set after the first display->render() below.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     Apple2Display::kWidth, Apple2Display::kHeight,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        screenTextureWidth  = Apple2Display::kWidth;
        screenTextureHeight = Apple2Display::kHeight;
    }

    // Mirror the live demod knobs into the display BEFORE render() so the
    // OE-CPU demod (and the OE-GPU mixed-frame CPU demod band) track the
    // CRT-Settings sliders exactly like the GPU shader — hue / sharpness /
    // PAL / textSharp used to be GPU-only and silently dead on the CPU path.
    {
        Apple2Display::OeDemodParams dp;
        if (ntscFx) {
            const pom2::NtscParams& np = ntscFx->getParams();
            dp.hue       = np.hue;
            dp.sharpness = np.sharpness;
            dp.palMode   = np.palMode;
            dp.textSharp = np.textSharp;
        }
        display->setOeDemodParams(dp);
    }

    // demodMutex covers render + demod + upload: the AI control server's
    // /screen handler runs the same render/demod/pixels phases on its own
    // thread, and the two used to race over frame/frame80/signalBuf with
    // no shared lock at all. stateMutex covers only render() (the
    // guest-RAM snapshot) so the CPU worker still isn't stalled by the
    // ~1-2 ms demod. Lock order: stateMutex → demodMutex, never nested
    // the other way.
    std::unique_lock<std::mutex> demodLk(display->demodMutex(),
                                         std::defer_lock);
    {
        // Render under stateMutex so we get a consistent snapshot of RAM
        // (otherwise the CPU may be mid-frame with the text screen half
        // updated, producing tearing).
        auto st = controller->lockState();
        demodLk.lock();
        display->render(st.memory());
    }
    display->finishPendingCpuDemod();

    const int w = display->width();
    const int h = display->height();
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    if (w != screenTextureWidth || h != screenTextureHeight) {
        // 80-col toggled — reallocate. glTexImage2D releases the previous
        // storage, so we don't leak GL memory across mode switches.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
        screenTextureWidth  = w;
        screenTextureHeight = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, display->pixels());
    }
}

// ─── Disk Library favourites / recents ───────────────────────────────────


void MainWindow::noteLibraryRecent(const std::string& path)
{
    if (path.empty()) return;
    auto it = std::find(libraryRecents_.begin(), libraryRecents_.end(), path);
    if (it != libraryRecents_.end()) libraryRecents_.erase(it);
    libraryRecents_.insert(libraryRecents_.begin(), path);
    if (libraryRecents_.size() > kMaxLibraryRecents)
        libraryRecents_.resize(kMaxLibraryRecents);
}

// ─── Command palette ─────────────────────────────────────────────────────
//
// One list, one dispatch switch. Adding a command means one line in
// buildCommands() plus one `if` in runCommand() — deliberately not a
// registration mechanism with callbacks, because the whole value of the
// palette is that every command is visible in one place when you read it.
