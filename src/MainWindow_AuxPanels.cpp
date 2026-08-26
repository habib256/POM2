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

// Physical MainWindow split: keyboard, about, editor, cassette and rewind panels.

// stb_image is bundled solely for the keyboard/About photos. Keep its static
// implementation in the one translation unit which owns those panels.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

void MainWindow::ensureKeyboardImageLoaded()
{
    if (uiState_->keyboardImageTried) return;
    uiState_->keyboardImageTried = true;

    const std::string path = pom2::findResource("pic/Keyboard_AppleIIe.jpeg");
    if (path.empty()) {
        uiState_->keyboardImageError = "not found in any resource search dir";
        return;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        uiState_->keyboardImageError = stbi_failure_reason() ? stbi_failure_reason()
                                              : "decode failed";
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // LINEAR both ways: the photo is 2578 px wide and the window is usually
    // narrower, so it is nearly always minified — GL_NEAREST turned the key
    // legends into aliased mush at every size but 1:1.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    uiState_->keyboardImageTexture = tex;
    uiState_->keyboardImageWidth   = w;
    uiState_->keyboardImageHeight   = h;
}

void MainWindow::renderKeyboardPanel()
{
    if (!uiState_->showKeyboardPanel) {
        // A latched Open-Apple must not outlive the window that shows it as
        // down: with the panel closed there is nothing to un-latch it with,
        // and the guest would see a key held forever.
        if (keyboardPanel) {
            keyboardPanel->releaseAll();
            controller->memory().setOpenAppleKey(false);
            controller->memory().setSolidAppleKey(false);
        }
        return;
    }
    ensureKeyboardImageLoaded();
    if (!keyboardPanel)
        keyboardPanel = std::make_unique<pom2::Keyboard_ImGui>();

    const auto ev = keyboardPanel->render(&uiState_->showKeyboardPanel, uiState_->keyboardImageTexture,
                                          uiState_->keyboardImageWidth, uiState_->keyboardImageHeight, uiState_->keyboardImageError);

    // The Apple keys are LEVELS, not events: $C061/$C062 bit 7 reads the
    // switch, so the latch has to be pushed every frame for as long as it is
    // down, exactly like the host's Left/Right Alt in onKey.
    const auto& lat = keyboardPanel->latches();
    controller->memory().setOpenAppleKey(lat.openApple);
    controller->memory().setSolidAppleKey(lat.solidApple);

    if (!ev.key) return;

    const pom2::KeyHotspot& k = *ev.key;
    bool consumedOneShots = false;

    if (k.kind == pom2::KeyKind::Char) {
        char c = ev.latches.shift ? k.shift : k.base;
        // Caps Lock is a LETTER latch on the //e, not a shift: it uppercases
        // A-Z and leaves the digit row alone (which is why the number keys
        // still need Shift for their symbols on a real machine).
        if (ev.latches.caps && c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        uint8_t code = static_cast<uint8_t>(c);
        if (ev.latches.control) {
            // Ctrl-A..Ctrl-Z = $01..$1A, on either case of the letter.
            const char up = (c >= 'a' && c <= 'z')
                                ? static_cast<char>(c - 'a' + 'A') : c;
            if (up >= 'A' && up <= 'Z')
                code = static_cast<uint8_t>(up - 'A' + 1);
        }
        injectAscii(code);
        consumedOneShots = true;
    } else {
        switch (k.action) {
            case pom2::KeyAction::Esc:    injectAscii(0x1B); break;
            case pom2::KeyAction::Tab:    injectAscii(0x09); break;
            case pom2::KeyAction::Return: injectAscii(0x0D); break;
            // $7F is what the //e's DELETE cap actually generates. It is NOT
            // the $08 the host Backspace injects — that one is the left
            // arrow's code, which is what a II/II+ had instead of a DELETE
            // key. The cap in the photo says Del, so it sends Del.
            case pom2::KeyAction::Delete: injectAscii(0x7F); break;
            case pom2::KeyAction::Left:   injectAscii(0x08); break;
            case pom2::KeyAction::Right:  injectAscii(0x15); break;
            case pom2::KeyAction::Down:   injectAscii(0x0A); break;
            case pom2::KeyAction::Up:     injectAscii(0x0B); break;
            case pom2::KeyAction::Reset:
                // Faithful: RESET alone does nothing on any Apple II — the
                // key is wired through the keyboard encoder's Ctrl line
                // precisely so a stray knock cannot reboot the machine. So
                // the panel refuses too, and says why, rather than quietly
                // being more dangerous than the hardware.
                if (!ev.latches.control) {
                    uiState_->tapeStatusMessage =
                        "Reset needs Control — latch CONTROL, then click Reset "
                        "(Open-Apple too for a cold boot).";
                    uiState_->tapeStatusUntil = uiState_->lastFrameTime + 6.0;
                    break;
                }
                // Open-Apple+Ctrl+Reset is the //e's cold boot; Ctrl+Reset
                // alone is the warm one. Same two verbs as F12 / F11.
                if (ev.latches.openApple) {
                    controller->hardReset();
                    uiState_->tapeStatusMessage = "Open-Apple + Ctrl + Reset — cold boot";
                } else {
                    controller->softReset();
                    uiState_->tapeStatusMessage = "Ctrl + Reset";
                }
                uiState_->tapeStatusUntil  = uiState_->lastFrameTime + 3.0;
                consumedOneShots = true;
                break;
            default: break;
        }
        if (k.action != pom2::KeyAction::Reset) consumedOneShots = true;
    }

    if (consumedOneShots) keyboardPanel->clearOneShots();
}

void MainWindow::ensureAboutImageLoaded()
{
    if (uiState_->aboutImageTried) return;
    uiState_->aboutImageTried = true;

    const std::string path = pom2::findResource("pic/Apple_II_plus.jpg");
    if (path.empty()) return;

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) return;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    uiState_->aboutImageTexture = tex;
    uiState_->aboutImageWidth   = w;
    uiState_->aboutImageHeight   = h;
}

void MainWindow::onFileDrop(int count, const char** paths)
{
    if (count <= 0 || !paths) return;
    // Boot the first image whose extension/size we recognise; skip the
    // rest (a single Apple II has one boot path). insertAndBootImage takes
    // the state lock itself and must run on the UI thread — the GLFW drop
    // callback fires inside glfwPollEvents(), which the render loop drives,
    // so we are on that thread here.
    for (int i = 0; i < count; ++i) {
        if (!paths[i]) continue;
        const std::string path = paths[i];
        if (classifyDiskForSlot(path) == DiskSlotClass::Unknown) continue;
        std::string err;
        if (insertAndBootImage(path, err)) {
            uiState_->tapeStatusMessage = "Dropped + booted: " +
                std::filesystem::path(path).filename().string();
            pom2::log().info("Drop", "booted dropped image: " + path);
        } else {
            uiState_->tapeStatusMessage = "Drop failed: " + err;
            pom2::log().warn("Drop", "dropped image rejected: " + err);
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        return;
    }
    // Nothing usable in the drop — tell the user rather than silently
    // ignoring it (the most common case is dropping a ROM or a .zip).
    uiState_->tapeStatusMessage =
        "Dropped file not a disk image "
        "(.dsk/.do/.d13/.po/.nib/.woz/.hdv/.2mg)";
    uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
}

void MainWindow::renderWelcomePanelWindow()
{
    if (!uiState_->showWelcomePanel) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Welcome to POM2###welcomePanel", &uiState_->showWelcomePanel,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 580.0f);

    // ── No-ROM banner ────────────────────────────────────────────────
    // The single biggest newcomer trip-up: ROMs are user-provided and the
    // machine shows a bare "NO ROM" screen without them. Surface the fix
    // first, in red, only while no ROM is loaded.
    if (!romLoaded_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("No Apple II ROM is loaded yet.");
        ImGui::PopStyleColor();
        ImGui::TextWrapped(
            "Apple II firmware is copyrighted, so POM2 does not ship it. "
            "Drop your firmware dump into a \"roms/\" folder next to POM2, "
            "then use File → Reload ROM (or relaunch).");
        ImGui::Spacing();

        // Which file the *active* profile wants, and where POM2 looks.
        const auto& cfg = pom2::profileConfig(activeProfile);
        if (!cfg.romProbeOrder.empty()) {
            ImGui::Text("Expected ROM for %.*s:",
                        static_cast<int>(cfg.displayName.size()),
                        cfg.displayName.data());
            for (const auto& cand : cfg.romProbeOrder)
                ImGui::BulletText("%s", cand.c_str());
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("POM2 searches these folders (first hit wins):");
        for (const auto& dir : pom2::resourceSearchDirs())
            ImGui::BulletText("%s", dir.string().c_str());

        ImGui::Spacing();
#ifndef __EMSCRIPTEN__
        if (ImGui::Button("Reload ROM (re-probe folders)")) {
            bool ok = false;
            {
                auto st = controller->lockState();
                // Re-resolve from the active profile so dropping the
                // profile-specific dump in is picked up without a relaunch.
                std::string newRom;
                for (const auto& cand : pom2::profileConfig(activeProfile).romProbeOrder) {
                    std::string r = pom2::findResource(cand);
                    if (!r.empty()) { newRom = r; break; }
                }
                if (newRom.empty()) newRom = romPath;  // last-known path
                ok = st.memory().loadAppleIIRom(newRom.c_str());
                if (ok) romPath = newRom;
            }
            if (ok) {
                controller->hardReset();
                romStatus  = std::string("loaded: ") + romPath;
                romLoaded_ = true;
            }
        }
        ImGui::SameLine();
#endif
        ImGui::TextDisabled("(%s)", romStatus.c_str());
        ImGui::Separator();
        ImGui::Spacing();
    }

    // ── Loading software ─────────────────────────────────────────────
    ImGui::SeparatorText("Loading a disk");
    ImGui::BulletText("Drag a .woz / .dsk / .po / .nib / .hdv / .2mg onto this window.");
    ImGui::BulletText("Or File → Disk Library to browse bundled images.");
    ImGui::BulletText("Or launch from a terminal: POM2 path/to/game.woz");
    ImGui::TextDisabled("POM2 auto-routes each image to Disk II, SmartPort 3.5\" or ProDOS HDV.");

    ImGui::Spacing();
    ImGui::SeparatorText("Suggested media folders");
    ImGui::BulletText("roms/        Apple II firmware dumps");
    ImGui::BulletText("disks_5.4/   5.25\" disk images (.dsk/.woz/.nib)");
    ImGui::BulletText("disks_3.5/   3.5\" disk images (800K)");
    ImGui::BulletText("hdv/         ProDOS hard-disk images (.hdv/.2mg)");

    // ── Keys & signature features ────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Keys");
    ImGui::BulletText("F11  Reset (Ctrl-Reset)        F12  Hard reset");
    ImGui::BulletText("F9   Screenshot                F6   Hold to rewind");
    ImGui::BulletText("Left Alt = Open-Apple          Right Alt = Solid-Apple");
    ImGui::BulletText("Ctrl+Alt+F  Full screen (kiosk) \xe2\x87\x84 windowed  (F10 too)");
    ImGui::BulletText("Ctrl+V pastes clipboard text as keystrokes");

    ImGui::Spacing();
    ImGui::SeparatorText("Try these");
    ImGui::BulletText("Display → 3D voxel view  — MicroM8-style cube renderer.");
    ImGui::BulletText("Devices → Rewind (F6)    — scrub back through machine state.");
    ImGui::BulletText("Display → CRT Settings   — scanlines, phosphor, NTSC look.");
    ImGui::BulletText("Machine → Profile        — switch between ][ / ][+ / //e / //c / //c+.");

    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close")) uiState_->showWelcomePanel = false;
    ImGui::SameLine();
    if (ImGui::Button("About POM2...")) { uiState_->showAbout = true; }
    ImGui::End();
}

void MainWindow::renderAboutDialog()
{
    if (!uiState_->showAbout) return;
    ensureAboutImageLoaded();
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About POM2", &uiState_->showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Photo on the left, all text flowed into a column on the right.
        if (uiState_->aboutImageTexture && uiState_->aboutImageWidth > 0 && uiState_->aboutImageHeight > 0) {
            // Scale to a sensible width in the dialog while preserving the
            // 800×792 aspect of the original photo (≈ 1:1).
            const float displayW = 220.0f;
            const float displayH = displayW *
                static_cast<float>(uiState_->aboutImageHeight) /
                static_cast<float>(uiState_->aboutImageWidth);
            ImGui::BeginGroup();
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<intptr_t>(uiState_->aboutImageTexture)),
                         ImVec2(displayW, displayH));
            ImGui::EndGroup();
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        // Constrain wrapped text to a fixed column so it stays beside the photo.
        const float textColumnW = 380.0f;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textColumnW);

        ImGui::Text("POM2 " POM2_VERSION_STRING);
        ImGui::Text("Apple II / II+ / //e / //c / //c+ emulator");
        ImGui::Text("MOS 6502 / 65C02 / Rockwell / WDC, Dear ImGui frontend");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Hardware accuracy comes from verbatim ports of MAME's "
            "device models. Wherever POM2 emulates a chip or a "
            "peripheral, the implementation cites the MAME source "
            "file and line range it follows.");
        ImGui::Spacing();
        ImGui::TextWrapped("Subsystems ported from MAME include:");
        ImGui::BulletText("M6502 / 65C02 dispatch table and timing");
        ImGui::BulletText("IWM (Apple Integrated Woz Machine) for //c+ and 3.5\" SmartPort");
        ImGui::BulletText("AY-3-8910 PSG + 6522 VIA (Mockingboard)");
        ImGui::BulletText("uPD1990AC RTC (ThunderClock+)");
        ImGui::BulletText("M68705P3 + MC6821 PIA (Mouse Card)");
        ImGui::BulletText("WozFDC / Disk II LSS + flux event model");
        ImGui::BulletText("Sony 3.5\" zoned GCR encoder / decoder");
        ImGui::BulletText("RamWorks III aux-slot expander");
        ImGui::BulletText("Floppy mechanical sound samples + cadence");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Thanks to the MAME team for the meticulous reverse "
            "engineering work that makes POM2's parity possible. "
            "MAME is GPL-2.0 / BSD-3-Clause; POM2 is GPL-3.0.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("F11 = Reset (Ctrl-Reset)   F12 = Hard reset");
        ImGui::Text("ESC, arrows, Ctrl-A..Z map straight to the keyboard");

        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        ImGui::Spacing();
        if (ImGui::Button("Close")) uiState_->showAbout = false;
    }
    ImGui::End();
}

void MainWindow::renderMemoryViewerWindow()
{
    debugCoordinator_->renderMemoryViewer(uiState_->showMemViewer);
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!uiState_->showCassetteDeck) return;

    // Build the deck snapshot under stateMutex — cheap enough that holding
    // the emul lock for the time it takes to copy a dozen scalars is fine.
    pom2::CassetteDeck_ImGui::DeckSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const CassetteDevice& d = controller->cassette();
        snap.loadedTape          = d.hasLoadedTape();
        snap.recordedTape        = d.hasRecordedTape();
        snap.playbackActive      = d.isPlaybackActive();
        snap.playbackArmed       = d.isPlaybackArmed();
        snap.rewinding           = d.isRewinding();
        snap.audioAvailable      = d.isAudioAvailable();
        snap.playbackPaused      = d.isPlaybackPaused();
        snap.audioStreamMode     = d.isAudioStreamMode();
        snap.queuedAudioSeconds  = d.getQueuedAudioSeconds();
        snap.playbackPositionSec = d.getPlaybackPositionSeconds();
        snap.playbackTotalSec    = d.getPlaybackTotalSeconds();
        snap.loadedTransitions   = d.getLoadedTransitionCount();
        snap.recordedTransitions = d.getRecordedTransitionCount();
        snap.volume              = d.getVolume();
        snap.loadedTapePath      = d.getLoadedTapePath();
        snap.loadInfo            = d.getLoadInfo();
    }

    ImGui::SetNextWindowSize(ImVec2(440, 720), ImGuiCond_FirstUseEver);
    auto result = cassetteDeck->render("Cassette Deck",
                                      uiState_->showCassetteDeck,
                                      controller.get(),
                                      snap,
                                      deltaSeconds);

    if (!result.statusMessage.empty()) {
        uiState_->tapeStatusMessage = std::move(result.statusMessage);
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;  // show for 4 seconds
    }
}

void MainWindow::renderHgrPaintWindow()
{
    if (!uiState_->showHgrPaintEditor) return;

    // 64 KB main-RAM (+ IIe aux) snapshot under stateMutex — the editor's
    // per-frame canvas/shadow read source (same idiom as the deck snapshot).
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        uiState_->hgrPaintMemory.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            uiState_->hgrPaintAuxMemory.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            uiState_->hgrPaintAuxMemory.clear();
    }

    const float w = hgrpaint::kHiresWidth  * 3.0f + 40.0f;
    const float h = hgrpaint::kHiresHeight * 3.0f + 180.0f;
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Paint Editor", &uiState_->showHgrPaintEditor))
        hgrPaintEditor->render(uiState_->hgrPaintMemory, uiState_->hgrPaintAuxMemory.empty() ? nullptr
                                                                  : &uiState_->hgrPaintAuxMemory);
    ImGui::End();
}

void MainWindow::renderHgrSpriteWindow()
{
    if (!uiState_->showHgrSpriteEditor) return;
    {
        auto st = controller->lockState();
        const Memory& mem = st.memory();
        uiState_->hgrPaintMemory.assign(mem.data(), mem.data() + 0x10000);
        if (mem.isIIE())
            uiState_->hgrPaintAuxMemory.assign(mem.auxData(), mem.auxData() + 0x10000);
        else
            uiState_->hgrPaintAuxMemory.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("HGR Sprite Editor", &uiState_->showHgrSpriteEditor))
        hgrSpriteEditor->render(uiState_->hgrPaintMemory, uiState_->hgrPaintAuxMemory.empty() ? nullptr
                                                                   : &uiState_->hgrPaintAuxMemory);
    ImGui::End();
}

void MainWindow::driveRewindHold(bool held)
{
    // Edge-detect: hold → step the machine backwards one frame; release →
    // resume live from the rewound point. No-op when recording is off
    // (holdRewind/beginScrub bail out), so it never surprises a non-user.
    if (held)                  rewindPanel_->holdRewind(*controller, 1);
    else if (uiState_->rewindHeldPrevious)  rewindPanel_->releaseHold(*controller);
    uiState_->rewindHeldPrevious = held;
}

void MainWindow::renderRewindWindow(float deltaSeconds)
{
    if (!uiState_->showRewindBar) return;
    auto result = rewindPanel_->render("Rewind", uiState_->showRewindBar, *controller, deltaSeconds);
    if (!result.statusMessage.empty()) {
        uiState_->tapeStatusMessage = std::move(result.statusMessage);
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
    }
}

void MainWindow::renderTapeFileDialogs()
{
    auto pathInput = [](const char* label) {
        // Minimal text-only path widget — POM2 doesn't pull in nativefiledialog.
        // The user types a path; convenience dirs/files can be appended later.
        ImGui::TextUnformatted(label);
    };

    if (cassetteDeck->loadDialogOpen) {
        ImGui::OpenPopup("Load Tape");
        cassetteDeck->loadDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Load Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Tape file path (.aci / .wav / .mp3 / .ogg / .flac)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##LoadPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        // Quick list of cassettes/ directory contents (one click → fill).
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "cassettes", "../cassettes", "../../cassettes" }) {
            if (!fs::is_directory(dir, ec)) continue;
            ImGui::Separator();
            ImGui::TextDisabled("%s/", dir);
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".aci" && ext != ".wav" && ext != ".mp3" &&
                    ext != ".ogg" && ext != ".flac") continue;
                const std::string name = entry.path().filename().string();
                if (ImGui::Selectable(name.c_str()))
                    cassetteDeck->dialogPath = entry.path().string();
            }
            break;  // first existing candidate dir wins
        }

        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (controller->loadTape(cassetteDeck->dialogPath)) {
                uiState_->tapeStatusMessage = "Tape loaded: " + cassetteDeck->dialogPath;
            } else {
                uiState_->tapeStatusMessage = "Load failed: " + controller->cassette().getLastError();
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (cassetteDeck->saveDialogOpen) {
        ImGui::OpenPopup("Save Tape");
        cassetteDeck->saveDialogOpen = false;
    }
    if (ImGui::BeginPopupModal("Save Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Output file path (.aci or .wav)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", cassetteDeck->dialogPath.c_str());
        if (ImGui::InputText("##SavePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            cassetteDeck->dialogPath = buf;
        else
            cassetteDeck->dialogPath = buf;

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (controller->saveTape(cassetteDeck->dialogPath)) {
                uiState_->tapeStatusMessage = "Tape saved: " + cassetteDeck->dialogPath;
            } else {
                uiState_->tapeStatusMessage = "Save failed: " + controller->cassette().getLastError();
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The transient uiState_->tapeStatusMessage (disk load / boot / eject / screenshot
    // / paste …) is now surfaced in the bottom status bar (renderStatusBar),
    // right-aligned and auto-expiring via uiState_->tapeStatusUntil — no separate
    // floating overlay.
}
