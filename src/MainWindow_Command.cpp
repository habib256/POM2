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
#include "DevicePanelCoordinator.h"
#include "MouseCoordinator.h"
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
#include "SoftCardZ80.h"
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

// Physical MainWindow split: command palette, docking shell, menus and status bar.

void MainWindow::openCommandPalette()
{
    if (cmdPalette) cmdPalette->open();
}

void MainWindow::renderCommandPalette()
{
    if (!cmdPalette || !cmdPalette->isOpen()) return;
    const auto deviceInventory = devicePanelCoordinator_->captureInventory();
    const auto audioInventory = audioCoordinator_->captureInventory();
    const auto mouseInventory = mouseCoordinator_->capture();

    using Cmd = pom2::CommandPalette_ImGui::Command;
    std::vector<Cmd> cmds;
    cmds.reserve(80);

    auto add = [&cmds](const char* id, const char* cat, std::string label,
                       const char* shortcut = "", bool enabled = true,
                       bool checked = false) {
        Cmd c;
        c.id       = id;
        c.category = cat;
        c.label    = std::move(label);
        c.shortcut = shortcut;
        c.enabled  = enabled;
        c.checked  = checked;
        cmds.push_back(std::move(c));
    };
    // Panel toggles all follow the same shape, so keep them one-liners.
    auto panel = [&add](const char* id, const char* label, bool* flag,
                        bool enabled = true) {
        add(id, "Panel", label, "", enabled, flag && *flag);
    };

    const auto mode = controller->getMode();

    // ── Machine ──────────────────────────────────────────────────────────
    add("machine.run",      "Machine", "Run", "",
        mode != EmulationController::Mode::Running);
    add("machine.pause",    "Machine", "Pause", "",
        mode == EmulationController::Mode::Running);
    add("machine.step",     "Machine", "Step one instruction", "",
        mode != EmulationController::Mode::Running);
    add("machine.reset",    "Machine", "Reset (Ctrl-Reset)", "F11");
    add("machine.hardreset","Machine", "Hard reset", "F12");
    add("machine.coldboot", "Machine", "Cold boot (wipe RAM)");
    add("machine.screenshot","Machine","Save screenshot", "F9");
    add("printer.dumpscreen","Machine","Print screen (dump to printer)");
    add("view.kiosk", "View",
        uiState_->kiosk ? "Leave full screen (kiosk)" : "Full screen (kiosk)",
        "Ctrl+Alt+F / F10", true, uiState_->kiosk);
    add("view.mousegrab", "View",
        uiState_->mouseGrabbed ? "Release mouse capture" : "Capture mouse",
        "Ctrl+Alt+G", mouseInventory.plugged(),
        uiState_->mouseGrabbed);

    // Speed buckets, labelled with the real clock so "2x" isn't abstract.
    {
        const VideoTiming& vt = pom2VideoTiming(controller->getVideoStandard());
        const int cur = controller->getCyclesPerFrame();
        const double mhz = static_cast<double>(vt.cyclesPerFrame) * vt.refreshHz / 1e6;
        char buf[64];
        std::snprintf(buf, sizeof buf, "Speed 1x (%.2f MHz)", mhz);
        add("speed.1x", "Machine", buf, "", true, cur == vt.cyclesPerFrame);
        std::snprintf(buf, sizeof buf, "Speed 2x (%.2f MHz)", mhz * 2);
        add("speed.2x", "Machine", buf, "", true, cur == vt.cyclesPerFrame * 2);
        std::snprintf(buf, sizeof buf, "Speed 4x (%.2f MHz)", mhz * 4);
        add("speed.4x", "Machine", buf, "", true, cur == vt.cyclesPerFrame * 4);
        add("speed.max", "Machine", "Speed MAX (uncapped)", "", true,
            cur == 1'000'000);
    }

    // ── Profiles ─────────────────────────────────────────────────────────
    for (pom2::SystemProfile p : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(p);
        add(("profile." + std::to_string(static_cast<int>(p))).c_str(),
            "Profile", std::string(cfg.displayName), "", true,
            p == activeProfile);
    }

    // ── Display ──────────────────────────────────────────────────────────
    {
        const auto cur = display->getHiResMode();
        auto pipe = [&](const char* id, const char* label,
                        Apple2Display::HiResMode m, bool enabled = true) {
            add(id, "Display", label, "", enabled, cur == m);
        };
        pipe("disp.ntsc",     "NTSC (MAME)",  Apple2Display::HiResMode::ColorNTSC);
        pipe("disp.ntscmed",  "NTSC (MAME) medium", Apple2Display::HiResMode::ColorCompMedium);
        pipe("disp.ntsc4bit", "NTSC (MAME) 4-bit square", Apple2Display::HiResMode::ColorComp4Bit);
        pipe("disp.oegpu",    "Composite (OpenEmulator, GPU)", Apple2Display::HiResMode::ColorCompositeOE);
        pipe("disp.oecpu",    "Composite (OpenEmulator, CPU)", Apple2Display::HiResMode::ColorCompositeOECpu);
        pipe("disp.applewin", "AppleWin NTSC (TV line blur)", Apple2Display::HiResMode::ColorAppleWin);
        pipe("disp.rgb",      "RGB card - Le Chat Mauve", Apple2Display::HiResMode::ChatMauveRGB,
             deviceInventory.chatMauvePlugged());
        pipe("disp.mono",     "Monochrome white", Apple2Display::HiResMode::MonoWhite);
        pipe("disp.green",    "Monochrome green (P31)", Apple2Display::HiResMode::MonoGreen);
        pipe("disp.amber",    "Monochrome amber", Apple2Display::HiResMode::MonoAmber);
        add("disp.aspect.square",  "Display", "Aspect: square pixels", "", true,
            aspectMode == AspectMode::Square);
        add("disp.aspect.crt43",   "Display", "Aspect: 4:3 CRT shape", "", true,
            aspectMode == AspectMode::Crt43);
        add("disp.aspect.integer", "Display", "Aspect: integer scale", "", true,
            aspectMode == AspectMode::Integer);
        add("disp.crttoggle", "Display", "Toggle CRT effects", "", true,
            crtEffectsEnabled);
    }

    // ── Layout + interface ───────────────────────────────────────────────
    add("layout.reset",     "Layout", "Reset to default layout");
    add("layout.emulation", "Layout", "Emulation layout");
    add("layout.debug",     "Layout", "Debug layout");
    add("layout.audio",     "Layout", "Audio layout");
    {
        std::size_t n = 0;
        const pom2::UiAccent* accents = pom2::allAccents(n);
        for (std::size_t i = 0; i < n; ++i) {
            add((std::string("accent.") + pom2::accentKey(accents[i])).c_str(),
                "Interface", std::string("Accent: ") +
                pom2::accentLabel(accents[i]), "", true,
                uiAccent_ == accents[i]);
        }
        add("ui.zoomin",  "Interface", "Zoom in");
        add("ui.zoomout", "Interface", "Zoom out");
        add("ui.zoom100", "Interface", "Zoom reset to 100%");
    }

    // ── Panels ───────────────────────────────────────────────────────────
    panel("panel.disklibrary", "Disk Library",            &uiState_->showDiskLibrary);
    panel("panel.diskii",      "Disk II drive",           &uiState_->showDiskPanel);
    panel("panel.disk35",      "Disk 3.5\" drive",        &uiState_->showDisk35Panel);
    panel("panel.hdv",         "HDV / ProDOS volume",     &uiState_->showHdvPanel);
    panel("panel.smartport",   "SmartPort configuration", &uiState_->showSmartPortPanel,
          deviceInventory.smartPortPlugged());
    panel("panel.fujinet",     "FujiNet (SP over SLIP)",  &uiState_->showFujiNetPanel,
          deviceInventory.fujiNetPlugged());
    panel("panel.floppyemu",   "Floppy Emu (BMOW)",       &uiState_->showFloppyEmu);
    panel("panel.cassette",    "Cassette deck",           &uiState_->showCassetteDeck);
    panel("panel.slotconfig",  "Slot configuration",      &uiState_->showSlotConfigPanel);
    panel("panel.media",       "Internal disks & media",  &uiState_->showMediaPanel);
    panel("panel.romstatus",   "ROM status",              &uiState_->showRomStatusPanel);
    panel("panel.abstraction", "Abstraction levels (LLE/HLE)",
          &uiState_->showAbstractionPanel);
    panel("panel.keyboard",    "Apple //e keyboard",      &uiState_->showKeyboardPanel);
    panel("panel.mockingboard","Mockingboard",            &uiState_->showMockingboardPanel,
          audioInventory.hasMockingboard());
    panel("panel.phasor",      "Phasor",                  &uiState_->showPhasorPanel,
          audioInventory.hasPhasor());
    panel("panel.echoplus",    "Echo+ speech",            &uiState_->showEchoPlusPanel,
          audioInventory.hasEchoPlus());
    panel("panel.mixer",       "Audio mixer",             &uiState_->showAudioMixer);
    panel("panel.ssc",         "Super Serial",            &uiState_->showSscPanel,
          deviceInventory.serialPlugged());
    panel("panel.ethernet",    "Ethernet (Uthernet)",     &uiState_->showEthernetPanel,
          deviceInventory.ethernetPlugged());
    panel("panel.printer",     "Printer",                 &uiState_->showPrinterPanel,
          deviceInventory.printerPlugged());
    panel("panel.imagewriter", "ImageWriter II printout",  &uiState_->showImageWriterPanel);
    panel("panel.chatmauve",   "Le Chat Mauve",           &uiState_->showChatMauvePanel);
    panel("panel.joystick",    "Joystick / paddles",      &uiState_->showJoystickPanel);
    panel("panel.mouse",       "Mouse inspector",         &uiState_->showMouseInspector);
    panel("panel.nsclock",     "No-Slot Clock",           &uiState_->showNoSlotClockPanel);
    panel("panel.memviewer",   "Memory viewer",           &uiState_->showMemViewer);
    panel("panel.membar",      "Memory map bar",          &uiState_->showMemoryBar);
    panel("panel.membarh",     "Memory map bar (horizontal)", &uiState_->showMemoryBarH);
    panel("panel.memgrid",     "Memory map grid",         &uiState_->showMemoryGrid);
    panel("panel.crt",         "CRT settings",            &uiState_->showNtscSettings);
    panel("panel.voxel",       "3D voxel view",           &uiState_->show3dVoxel);
    panel("panel.voxelset",    "3D voxel settings",       &uiState_->showVoxelSettings);
    panel("panel.hgrpaint",    "HGR Paint editor",        &uiState_->showHgrPaintEditor);
    panel("panel.hgrsprite",   "HGR Sprite editor",       &uiState_->showHgrSpriteEditor);
    panel("panel.rewind",      "Rewind (time-travel)",    &uiState_->showRewindBar);
    panel("panel.welcome",     "Welcome / quick start",   &uiState_->showWelcomePanel);
#ifndef __EMSCRIPTEN__
    panel("panel.aicontrol",   "AI Control (HTTP)",       &uiState_->showAiControlPanel);
#endif

    // ── Media ────────────────────────────────────────────────────────────
    add("media.ejectall", "Media", "Eject all disks");

    cmdPalette->setCommands(std::move(cmds));
    const auto r = cmdPalette->render();
    if (r.executed) runCommand(r.commandId);
}

void MainWindow::runCommand(const std::string& id)
{
    auto toggle = [](bool& f) { f = !f; };

    // Machine
    if (id == "machine.run")        { controller->setMode(EmulationController::Mode::Running); return; }
    if (id == "machine.pause")      { controller->setMode(EmulationController::Mode::Stopped); return; }
    if (id == "machine.step")       { controller->requestStep(); return; }
    if (id == "machine.reset")      { controller->softReset();  return; }
    if (id == "machine.hardreset")  { controller->hardReset();  return; }
    if (id == "machine.coldboot")   { controller->coldBoot();   return; }
    if (id == "machine.screenshot") { saveScreenshot();         return; }
    if (id == "printer.dumpscreen") { dumpScreenToPrinter();    return; }
    if (id == "view.kiosk")        { toggleKioskMode();        return; }
    if (id == "view.mousegrab")    { toggleMouseGrab();        return; }

    if (id.rfind("speed.", 0) == 0) {
        const VideoTiming& vt = pom2VideoTiming(controller->getVideoStandard());
        if      (id == "speed.1x")  controller->setCyclesPerFrame(vt.cyclesPerFrame);
        else if (id == "speed.2x")  controller->setCyclesPerFrame(vt.cyclesPerFrame * 2);
        else if (id == "speed.4x")  controller->setCyclesPerFrame(vt.cyclesPerFrame * 4);
        else if (id == "speed.max") controller->setCyclesPerFrame(1'000'000);
        return;
    }

    if (id.rfind("profile.", 0) == 0) {
        const int idx = std::atoi(id.c_str() + 8);
        for (pom2::SystemProfile p : pom2::allProfiles())
            if (static_cast<int>(p) == idx) { applyProfile(p); return; }
        return;
    }

    // Display
    if (id == "disp.ntsc")     { display->setHiResMode(Apple2Display::HiResMode::ColorNTSC); return; }
    if (id == "disp.ntscmed")  { display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium); return; }
    if (id == "disp.ntsc4bit") { display->setHiResMode(Apple2Display::HiResMode::ColorComp4Bit); return; }
    if (id == "disp.oegpu")    { display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOE); return; }
    if (id == "disp.oecpu")    { display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu); return; }
    if (id == "disp.applewin") {
        display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);
        display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        return;
    }
    if (id == "disp.rgb")   { display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB); return; }
    if (id == "disp.mono")  { display->setHiResMode(Apple2Display::HiResMode::MonoWhite); return; }
    if (id == "disp.green") { display->setHiResMode(Apple2Display::HiResMode::MonoGreen); return; }
    if (id == "disp.amber") { display->setHiResMode(Apple2Display::HiResMode::MonoAmber); return; }
    if (id == "disp.aspect.square")  { aspectMode = AspectMode::Square;  return; }
    if (id == "disp.aspect.crt43")   { aspectMode = AspectMode::Crt43;   return; }
    if (id == "disp.aspect.integer") { aspectMode = AspectMode::Integer; return; }
    if (id == "disp.crttoggle")      { toggle(crtEffectsEnabled);        return; }

    // Layout + interface
    if (id == "layout.reset")     { pendingDockLayout_ = DockLayout::Reset;     dockLayoutRequested_ = true; return; }
    if (id == "layout.emulation") { pendingDockLayout_ = DockLayout::Emulation; dockLayoutRequested_ = true; return; }
    if (id == "layout.debug")     { pendingDockLayout_ = DockLayout::Debug;     dockLayoutRequested_ = true; return; }
    if (id == "layout.audio")     { pendingDockLayout_ = DockLayout::Audio;     dockLayoutRequested_ = true; return; }
    if (id.rfind("accent.", 0) == 0) {
        uiAccent_ = pom2::accentFromKey(id.c_str() + 7);
        applyUiTheme();
        return;
    }
    if (id == "ui.zoomin")  { uiScale_ = std::clamp(uiScale_ + pom2::kUiScaleStep * 2.0f, pom2::kUiScaleMin, pom2::kUiScaleMax); applyUiTheme(); return; }
    if (id == "ui.zoomout") { uiScale_ = std::clamp(uiScale_ - pom2::kUiScaleStep * 2.0f, pom2::kUiScaleMin, pom2::kUiScaleMax); applyUiTheme(); return; }
    if (id == "ui.zoom100") { uiScale_ = 1.0f; applyUiTheme(); return; }

    // Panels — id → flag, one table so there's no second place to update.
    struct PanelBinding { const char* id; bool* flag; };
    const PanelBinding panels[] = {
        { "panel.disklibrary",  &uiState_->showDiskLibrary       },
        { "panel.diskii",       &uiState_->showDiskPanel         },
        { "panel.disk35",       &uiState_->showDisk35Panel       },
        { "panel.hdv",          &uiState_->showHdvPanel          },
        { "panel.smartport",    &uiState_->showSmartPortPanel    },
        { "panel.fujinet",      &uiState_->showFujiNetPanel      },
        { "panel.floppyemu",    &uiState_->showFloppyEmu         },
        { "panel.cassette",     &uiState_->showCassetteDeck      },
        { "panel.slotconfig",   &uiState_->showSlotConfigPanel   },
        { "panel.media",        &uiState_->showMediaPanel        },
        { "panel.romstatus",    &uiState_->showRomStatusPanel    },
        { "panel.abstraction",  &uiState_->showAbstractionPanel  },
        { "panel.keyboard",     &uiState_->showKeyboardPanel     },
        { "panel.mockingboard", &uiState_->showMockingboardPanel },
        { "panel.phasor",       &uiState_->showPhasorPanel       },
        { "panel.echoplus",     &uiState_->showEchoPlusPanel     },
        { "panel.mixer",        &uiState_->showAudioMixer        },
        { "panel.ssc",          &uiState_->showSscPanel          },
        { "panel.ethernet",     &uiState_->showEthernetPanel     },
        { "panel.printer",      &uiState_->showPrinterPanel      },
        { "panel.imagewriter",  &uiState_->showImageWriterPanel  },
        { "panel.chatmauve",    &uiState_->showChatMauvePanel    },
        { "panel.joystick",     &uiState_->showJoystickPanel     },
        { "panel.mouse",        &uiState_->showMouseInspector    },
        { "panel.nsclock",      &uiState_->showNoSlotClockPanel  },
        { "panel.memviewer",    &uiState_->showMemViewer         },
        { "panel.membar",       &uiState_->showMemoryBar         },
        { "panel.membarh",      &uiState_->showMemoryBarH        },
        { "panel.memgrid",      &uiState_->showMemoryGrid        },
        { "panel.crt",          &uiState_->showNtscSettings      },
        { "panel.voxel",        &uiState_->show3dVoxel          },
        { "panel.voxelset",     &uiState_->showVoxelSettings    },
        { "panel.hgrpaint",     &uiState_->showHgrPaintEditor    },
        { "panel.hgrsprite",    &uiState_->showHgrSpriteEditor   },
        { "panel.rewind",       &uiState_->showRewindBar         },
        { "panel.welcome",      &uiState_->showWelcomePanel      },
        { "panel.aicontrol",    &uiState_->showAiControlPanel    },
    };
    for (const auto& b : panels)
        if (id == b.id) { toggle(*b.flag); return; }

    if (id == "media.ejectall") { ejectAllDisks(); return; }
}

// ─── Docking ─────────────────────────────────────────────────────────────

void MainWindow::renderDockSpace()
{
    // The dockspace covers the viewport WORK area, which the main menu bar,
    // the toolbar and the status bar have each already reserved a slice of
    // (all three are `BeginViewportSideBar` windows). So the chrome is never
    // overlapped and never needs hardcoded offsets.
    //
    // PassthruCentralNode: when nothing is docked in the middle, the central
    // node draws no background. Without it an empty centre is a grey slab
    // covering the whole work area.
    dockspaceId_ = ImGui::DockSpaceOverViewport(
        ImGui::GetID("POM2_DockSpace"), ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);

    // Seed the default layout the first time POM2 runs with docking (or when
    // the user picks a preset). Gated on a persisted flag rather than on "is
    // the node empty": DockSpaceOverViewport has already created the node by
    // this point, so emptiness can't distinguish "fresh install" from "user
    // undocked everything on purpose".
    if (!dockSeeded_) {
        dockSeeded_          = true;
        dockLayoutRequested_ = true;
        pendingDockLayout_   = DockLayout::Reset;
    }
    if (dockLayoutRequested_) {
        dockLayoutRequested_ = false;
        applyDockLayout(pendingDockLayout_);
    }
}

void MainWindow::applyDockLayout(DockLayout preset)
{
    if (dockspaceId_ == 0) return;

    // Rebuild from scratch. RemoveNode undocks everything first, so windows
    // the preset doesn't mention end up floating rather than stuck in a
    // stale node.
    ImGui::DockBuilderRemoveNode(dockspaceId_);
    ImGui::DockBuilderAddNode(dockspaceId_, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId_,
                                  ImGui::GetMainViewport()->WorkSize);

    // `centre` is rebound by each split to the *remaining* opposite side, so
    // successive splits carve off the outside and leave the screen in the
    // middle. SetNodeSize above matters: split ratios are computed against
    // the node's size, and without it the first split's sizes are unreliable.
    ImGuiID centre = dockspaceId_;
    ImGuiID right = 0, rightLower = 0, bottom = 0;

    switch (preset) {
        case DockLayout::Reset:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.34f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.45f, nullptr, &right);
            break;
        case DockLayout::Emulation:
            // No inspectors: one right column, all storage.
            right = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                0.32f, nullptr, &centre);
            rightLower = right;
            break;
        case DockLayout::Debug:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.38f, nullptr, &centre);
            bottom     = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down,
                                                     0.30f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.50f, nullptr, &right);
            break;
        case DockLayout::Audio:
            right      = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right,
                                                     0.40f, nullptr, &centre);
            rightLower = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down,
                                                     0.35f, nullptr, &right);
            break;
    }
    if (rightLower == 0) rightLower = right;
    if (bottom     == 0) bottom     = rightLower;

    // The screen is always the centre. Its window carries NoMove + a manual
    // title-bar drag; `renderScreenWindow` disables that drag while docked so
    // the two don't fight (a docked window is moved by its tab, not its body).
    ImGui::DockBuilderDockWindow("Apple II Screen", centre);

    // Everything below docks by literal title. Panels that are hidden right
    // now still get assigned — the assignment is what makes them open as a
    // tab in the right group later instead of floating over the screen, which
    // is most of the value of doing this at all.
    auto dock = [](const char* title, ImGuiID node) {
        ImGui::DockBuilderDockWindow(title, node);
    };

    switch (preset) {
        case DockLayout::Reset:
            // The default startup trio, tabbed to the right of the screen:
            // what you mount (Disk Library), what the machine is made of
            // (Slot Configuration), what it prints (ImageWriter II). All
            // three default to visible, so a fresh install opens on exactly
            // this arrangement. Disk Library is docked first, so it is the
            // selected tab.
            dock("Disk Library", right);
            dock("Slot Configuration", right);
            dock(ICON_FA_PRINT " ImageWriter II###imageWriterPanel", right);
            // Inspector tab group, bottom right.
            dock("Cassette Deck", rightLower);
            dock("Floppy Emu (BMOW)", rightLower);
            dock("Memory viewer", rightLower);
            dock("Mockingboard (VIA + AY state)", rightLower);
            dock("Mouse Inspector", rightLower);
            dock("CRT Settings (Composite NTSC)", rightLower);
            dock("Audio Mixer", rightLower);
            dock("Joystick", rightLower);
            dock("Rewind", rightLower);
            break;

        case DockLayout::Emulation:
            dock("Disk Library", right);
            dock("Cassette Deck", right);
            dock("Floppy Emu (BMOW)", right);
            dock("Internal Disks & Media", right);
            dock("Slot Configuration", right);
            dock("Rewind", right);
            break;

        case DockLayout::Debug:
            dock("Memory viewer", right);
            dock("Memory Map Grid", right);
            dock("Memory Map Bar", right);
            dock("Mouse Inspector", rightLower);
            dock("No-Slot Clock (Dallas DS1216E)###nsclockPanel", rightLower);
            dock("AI Control (HTTP)", rightLower);
            dock("Memory Map Bar (Horizontal)", bottom);
            break;

        case DockLayout::Audio:
            dock("Mockingboard (VIA + AY state)", right);
            dock("Phasor (mode + 2×VIA + 4×AY)", right);
            dock("Echo+ (SSI263 speech)", right);
            dock("Audio Mixer", rightLower);
            dock("Cassette Deck", rightLower);
            break;
    }

    ImGui::DockBuilderFinish(dockspaceId_);
}

// ─── Interface appearance ────────────────────────────────────────────────

void MainWindow::applyUiTheme()
{
    pom2::applyTheme(uiAccent_, uiScale_, dpiScale_);
}

void MainWindow::setDpiScale(float s)
{
    // Guard against a windowing system reporting 0 (or something absurd) —
    // a zero scale would collapse every padding to 0 and hide the font.
    dpiScale_ = (s > 0.1f && s < 8.0f) ? s : 1.0f;
    applyUiTheme();
}

// ─── Render passes ───────────────────────────────────────────────────────

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;
    const auto deviceInventory = devicePanelCoordinator_->captureInventory();
    const auto audioInventory = audioCoordinator_->captureInventory();
    const auto mouseInventory = mouseCoordinator_->capture();
    const auto storageInventory =
        storageCoordinator_->captureInventory(*controller);

    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("Disk Library (all formats)", nullptr, &uiState_->showDiskLibrary);
        ImGui::Separator();
        // Disk II (slot 6) — frequent action, lifted out of the old
        // Hardware kitchen-sink. Panel still exposes its own insert/eject
        // buttons; this is the keyboard-friendly path.
        ImGui::BeginDisabled(!storageInventory.hasDiskII());
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            storageInventory.primaryDiskLoaded)) {
            const auto command = storageCoordinator_->ejectDiskII(
                *controller, *settings,
                storageInventory.primaryDiskIISlot, 0);
            uiState_->tapeStatusMessage = command.ok
                ? "Disk ejected" : "Disk eject failed: " + command.error;
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(storageInventory.primaryHdvSlot < 0);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            hdvPanel->mountDialogOpen = true;
            if (hdvPanel->dialogPath.empty()) hdvPanel->dialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            storageInventory.primaryHdvLoaded)) {
            const auto command = storageCoordinator_->ejectMediaBay(
                *controller, *settings, storageInventory.primaryHdvSlot, 0);
            uiState_->tapeStatusMessage = command.ok
                ? "HDV ejected" : "HDV eject failed: " + command.error;
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!storageInventory.primaryHdvLoaded);
        // Label reflects where the user actually has the card plugged.
        const std::string bootHdvLabel = "Boot HDV (slot " +
            std::to_string(storageInventory.primaryHdvSlot >= 0
                               ? storageInventory.primaryHdvSlot : 5) + ")";
        if (ImGui::MenuItem(bootHdvLabel.c_str())) {
            bootHdvImage();
        }
        ImGui::EndDisabled();
#ifndef __EMSCRIPTEN__
        // Reload ROM re-reads the ROM file from disk. Useful on native (swap
        // a roms/ file, reload without restarting); pointless under WASM,
        // where the ROM is baked into POM2.data and cannot be replaced — so
        // it is hidden in the browser build.
        ImGui::Separator();
        if (ImGui::MenuItem("Reload ROM")) {
            bool ok = false;
            std::string err;
            {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                auto st = controller->lockState();
                ok = st.memory().loadAppleIIRom(romPath.c_str());
                if (!ok) err = st.memory().getLastError();
            }
            // hardReset() re-acquires stateMutex internally, so it MUST run
            // outside the lock_guard scope above — calling it while the lock
            // is held self-deadlocks the non-recursive mutex (mirrors the
            // coldBoot/bootFromSlot call sites elsewhere in this file).
            if (ok) {
                controller->hardReset();
                romStatus = std::string("loaded: ") + romPath;
                romLoaded_ = true;
            } else {
                romStatus = err;
                romLoaded_ = false;
            }
        }
        // Quit is a no-op in the browser: the frame loop is driven by
        // emscripten_set_main_loop_arg (main.cpp), which ignores
        // glfwWindowShouldClose, and a canvas cannot close its own tab.
        // Hide the entry under WASM so the menu stays honest.
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (window) glfwSetWindowShouldClose(window, 1);
        }
#endif
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Paste from clipboard", "Ctrl+V"))
            pasteFromClipboard();
#ifndef __EMSCRIPTEN__
        // "Paste from file" reads a host text file; the browser build has no
        // host filesystem (only the read-only bundled MEMFS), and nothing
        // pasteable ships in it. Clipboard paste above still works in WASM.
        if (ImGui::MenuItem("Paste from file..."))
            uiState_->showPasteFileDialog = true;
#endif
        ImGui::Separator();
        const size_t pending = controller->memory().pendingPasteSize();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::MenuItem("Cancel pending paste")) {
            controller->memory().cancelPaste();
            uiState_->tapeStatusMessage = "Paste cancelled";
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Auto-uppercase pasted text", nullptr, &uiState_->pasteAutoUppercase);
        if (pending > 0) {
            ImGui::Separator();
            ImGui::TextDisabled("(%zu chars pending)", pending);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Machine")) {
        const auto m = controller->getMode();
        if (ImGui::MenuItem("Run", nullptr, m == EmulationController::Mode::Running)) {
            controller->setMode(EmulationController::Mode::Running);
        }
        if (ImGui::MenuItem("Pause", nullptr, m == EmulationController::Mode::Stopped)) {
            controller->setMode(EmulationController::Mode::Stopped);
        }
        if (ImGui::MenuItem("Step (one instr)")) controller->requestStep();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset (Ctrl-Reset)",     "F11")) controller->softReset();
        if (ImGui::MenuItem("Hard reset",             "F12")) controller->hardReset();
        if (ImGui::MenuItem("Cold boot (wipe RAM)"))          controller->coldBoot();
        ImGui::Separator();
        if (ImGui::BeginMenu("Profile")) {
            // 5 canonical Apple II profiles. Selecting one triggers a
            // full cold-reset via `applyProfile()`: new ROM, new char ROM,
            // RAM wiped, slot cards re-plugged, CPU type reset to the
            // profile default (overridable in Machine → CPU). Disk and HDV
            // mounts persist across the switch so the user can test the
            // same software stack under different models.
            for (pom2::SystemProfile p : pom2::allProfiles()) {
                const auto& cfg = pom2::profileConfig(p);
                const bool selected = (activeProfile == p);
                // ImGui's MenuItem 3rd-arg `selected` draws the native
                // checkmark on the right of the row — that's enough; no
                // need to append an extra "✓" to the label (the double
                // mark looked wrong, 2026-05-15). string_view → string
                // for guaranteed null-termination.
                const std::string label(cfg.displayName);
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    applyProfile(p);
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Profile = full cold reset.");
            ImGui::TextDisabled("Mounted disks survive the switch.");
            ImGui::EndMenu();
        }
        // CPU type selector. Three settings:
        //   * Auto (profile default) — NMOS for II/II+, CMOS for IIe/IIc/IIc+
        //   * NMOS 6502 — force NMOS regardless of profile (e.g. test
        //     IIe NMOS-unenhanced behaviour)
        //   * 65C02 — force CMOS (e.g. run NMOS-era software on 65C02)
        // Persisted to settings as `cpu_mode_override` so the choice
        // survives a relaunch. A profile switch re-applies the override.
        M6502::CpuMode curCpu;
        { auto st = controller->lockState(); curCpu = st.cpu().getCpuMode(); }
        const std::string curOverride = settings->getString("cpu_mode_override", "auto");
        if (ImGui::BeginMenu("CPU")) {
            const auto& cfg = pom2::profileConfig(activeProfile);
            // CMOS-only machines (//c, //c+, enhanced //e, PAL variants) have
            // a 65C02 soldered in — an NMOS override is physically impossible
            // AND freezes their 65C02 ROMs (KIL opcodes). resolveCpuMode()
            // clamps it; mirror that here so the menu can't re-arm the freeze.
            const bool cmosOnly = (cfg.defaultCpu == M6502::CpuMode::CMOS);
            const char* profileLabel = cmosOnly ? "65C02" : "NMOS 6502";
            char autoLabel[64];
            std::snprintf(autoLabel, sizeof(autoLabel),
                "Auto (profile default: %s)", profileLabel);
            if (ImGui::MenuItem(autoLabel, nullptr, curOverride == "auto")) {
                settings->setString("cpu_mode_override", "auto");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(cfg.defaultCpu);
            }
            ImGui::BeginDisabled(cmosOnly);
            // On a CMOS-only profile the NMOS override is inert (clamped), so
            // never show it checked there — the running CPU is 65C02.
            if (ImGui::MenuItem("NMOS 6502", nullptr,
                                !cmosOnly &&
                                (curOverride == "nmos" ||
                                 (curOverride == "auto" && curCpu == M6502::CpuMode::NMOS
                                  && curOverride != "65c02")))) {
                settings->setString("cpu_mode_override", "nmos");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(M6502::CpuMode::NMOS);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("65C02 (CMOS)", nullptr,
                                curOverride == "65c02" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::CMOS
                                 && curOverride != "nmos"))) {
                settings->setString("cpu_mode_override", "65c02");
                settings->save();
                auto st = controller->lockState();
                st.cpu().setCpuMode(M6502::CpuMode::CMOS);
            }
            ImGui::Separator();
            ImGui::TextDisabled("NMOS = original 1975. Disables");
            ImGui::TextDisabled("STZ/BRA/PHX/etc. and SMB/RMB/");
            ImGui::TextDisabled("BBR/BBS extensions.");
            ImGui::TextDisabled("Override persists across profile");
            ImGui::TextDisabled("switches (NMOS ignored on 65C02-");
            ImGui::TextDisabled("only models: //c, //c+, enh. //e).");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Slot Configuration...", nullptr, &uiState_->showSlotConfigPanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        // One flat 17-item list became hard to scan (audit 2026-05-31), so
        // it is grouped under SeparatorText headers and every row carries a
        // hover tooltip explaining what the panel does. `devItem` keeps the
        // boilerplate (optional grey-out + tooltip) in one place; disabled
        // rows still show their tip via AllowWhenDisabled so the user learns
        // what a card *would* do before plugging it in Slot Config.
        auto devItem = [](const char* label, bool* flag, const char* tip,
                          bool enabled = true) {
            if (!enabled) ImGui::BeginDisabled();
            ImGui::MenuItem(label, nullptr, flag);
            if (!enabled) ImGui::EndDisabled();
            if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tip);
        };

        ImGui::SeparatorText("Storage");
        devItem("Internal Disks & Media...", &uiState_->showMediaPanel,
                "Every internal drive and mountable bay in one place. "
                "Mount / Insert / Eject act immediately — the card-per-slot "
                "list is Machine \xe2\x86\x92 Slot Configuration.");
        devItem("Floppy Emu (BMOW)", &uiState_->showFloppyEmu,
                "BMOW Floppy Emu: SD-card image browser + OLED, emulated.");
        devItem("Cassette deck", &uiState_->showCassetteDeck,
                "Load/save tape images (.wav) on II/II+/IIe.");
        devItem("Disk II (slot 6)", &uiState_->showDiskPanel,
                "5.25\" drive panel: insert / eject / write-protect, drive LEDs.");
        {
            // Mirror the panel's dynamic title — slot N on //e with a
            // Liron-class card, "//c+ on-board" on //c+.
            std::string lbl = deviceInventory.smartPortPlugged()
                ? "Disk 3.5\" (slot " +
                  std::to_string(deviceInventory.smartPortSlot) + ")"
                : std::string("Disk 3.5\" (//c+ on-board)");
            devItem(lbl.c_str(), &uiState_->showDisk35Panel,
                    "800K 3.5\" drive (SmartPort / //c+ on-board IWM).");
        }
        {
            const std::string lbl = "HDV (slot " +
                std::to_string(storageInventory.primaryHdvSlot >= 0
                                   ? storageInventory.primaryHdvSlot : 5) + ")";
            devItem(lbl.c_str(), &uiState_->showHdvPanel,
                    "ProDOS hard-disk image (.hdv/.2mg): mount / eject / boot.");
        }
        {
            const std::string lbl = deviceInventory.smartPortPlugged()
                ? "SmartPort Configuration (slot " +
                      std::to_string(deviceInventory.smartPortSlot) + ")"
                : std::string("SmartPort Configuration (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showSmartPortPanel,
                    "SmartPort units behind a Liron-class card (3.5\" + HDV volumes).",
                    deviceInventory.smartPortPlugged());
        }
        {
            const std::string lbl = deviceInventory.fujiNetPlugged()
                ? "FujiNet (slot " +
                  std::to_string(deviceInventory.fujiNetSlot) + ")"
                : std::string("FujiNet (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showFujiNetPanel,
                    "FujiNet relay: transport, attached devices and call counters.",
                    deviceInventory.fujiNetPlugged());
        }

        ImGui::SeparatorText("Sound");
        {
            const int slot = audioInventory.primaryMockingboardSlot();
            const std::string lbl = slot >= 0
                ? "Mockingboard (slot " + std::to_string(slot) + ")"
                : std::string("Mockingboard (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showMockingboardPanel,
                    "Mockingboard A/C: live 6522 VIA + AY-3-8910 PSG register view.",
                    audioInventory.hasMockingboard());
        }
        {
            const int slot = audioInventory.primaryPhasorSlot();
            const std::string lbl = slot >= 0
                ? "Phasor (slot " + std::to_string(slot) + ")"
                : std::string("Phasor (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showPhasorPanel,
                    "Applied Engineering Phasor: 2× VIA, 4× AY, mode soft-switch.",
                    audioInventory.hasPhasor());
        }
        {
            const int slot = audioInventory.primaryEchoPlusSlot();
            const std::string lbl = slot >= 0
                ? "Echo+ (slot " + std::to_string(slot) + ")"
                : std::string("Echo+ (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showEchoPlusPanel,
                    "Echo/Cricket SSI263 speech chip state.",
                    audioInventory.hasEchoPlus());
        }
        devItem("Audio Mixer", &uiState_->showAudioMixer,
                "Per-source volume: speaker, Mockingboard/Phasor, speech, floppy.");

        ImGui::SeparatorText("Ports & cards");
        // Super Serial — //c ships TWO (printer + modem), other profiles
        // have at most one. Label shows actual slot(s) so the user knows
        // which entry opens which port.
        {
            std::string lbl;
            if (deviceInventory.serialSlots.empty()) {
                lbl = "Super Serial (no card plugged)";
            } else if (deviceInventory.serialSlots.size() == 1) {
                lbl = "Super Serial (slot " +
                      std::to_string(deviceInventory.serialSlots[0]) + ")";
            } else {
                lbl = "Super Serial (slots";
                for (size_t i = 0; i < deviceInventory.serialSlots.size(); ++i) {
                    lbl += (i == 0) ? " " : ", ";
                    lbl += std::to_string(deviceInventory.serialSlots[i]);
                }
                lbl += ")";
            }
            devItem(lbl.c_str(), &uiState_->showSscPanel,
                    "6551 ACIA serial port + telnet bridge (modem / printer).",
                    deviceInventory.serialPlugged());
        }
        // Ethernet — one entry covers both cards; the panel tabs between
        // whichever are plugged.
        {
            std::string lbl = "Ethernet";
            if (deviceInventory.uthernetIISlot >= 0 &&
                deviceInventory.uthernetSlot >= 0) {
                lbl += " (Uthernet I slot " +
                       std::to_string(deviceInventory.uthernetSlot) +
                       ", II slot " +
                       std::to_string(deviceInventory.uthernetIISlot) + ")";
            } else if (deviceInventory.uthernetIISlot >= 0) {
                lbl += " (Uthernet II, slot " +
                       std::to_string(deviceInventory.uthernetIISlot) + ")";
            } else if (deviceInventory.uthernetSlot >= 0) {
                lbl += " (Uthernet I, slot " +
                       std::to_string(deviceInventory.uthernetSlot) + ")";
            } else {
                lbl += " (no card plugged)";
            }
            devItem(lbl.c_str(), &uiState_->showEthernetPanel,
                    "Uthernet I / II state: host transport, MAC, W5100 sockets.",
                    deviceInventory.ethernetPlugged());
        }
        {
            const std::string lbl = deviceInventory.printerPlugged()
                ? "Printer (slot " +
                      std::to_string(deviceInventory.printerSlot) + ")"
                : std::string("Printer (no card plugged)");
            devItem(lbl.c_str(), &uiState_->showPrinterPanel,
                    "Parallel printer card → text spool (.txt).",
                    deviceInventory.printerPlugged());
        }
        // The ImageWriter is the *printer* hanging off whichever printer
        // interface card is plugged, so it is always openable — an empty
        // paper tray is a legitimate thing to look at.
        devItem("ImageWriter II (printout)", &uiState_->showImageWriterPanel,
                "Rendered ImageWriter II output: pages, colour ribbon, PNG export.");
        devItem("Le Chat Mauve (slot 7)", &uiState_->showChatMauvePanel,
                "Le Chat Mauve RGB / Eve video card controls.");
        devItem("Joystick", &uiState_->showJoystickPanel,
                "Analog paddles / joystick mapping + push-buttons.");
        devItem("Apple //e Keyboard", &uiState_->showKeyboardPanel,
                "A photo of the real //e keyboard, clickable. The keys a host\n"
                "keyboard has nowhere to put — Open-Apple, Solid-Apple, the\n"
                "//e's own Reset — are here, with the real legends.");

        ImGui::SeparatorText("Inspectors & tools");
        ImGui::MenuItem("Rewind (time-travel)", "F6", &uiState_->showRewindBar);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scrub back through machine state. Hold F6 to rewind live.");
        devItem("Mouse Inspector", &uiState_->showMouseInspector,
                "Apple II Mouse Card state + host-cursor sync diagnostics.");
        devItem("No-Slot Clock (DS1216E)", &uiState_->showNoSlotClockPanel,
                "Dallas DS1216E real-time clock hidden under the Monitor ROM.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Display")) {
        // Presentation aspect (Phase 6). The Apple II pixel is not square;
        // these pick how the 280×192 active area fills the window.
        if (ImGui::BeginMenu("Aspect ratio")) {
            if (ImGui::MenuItem("Square pixels (1:1)", nullptr,
                                aspectMode == AspectMode::Square))
                aspectMode = AspectMode::Square;
            if (ImGui::MenuItem("4:3 (CRT shape)", nullptr,
                                aspectMode == AspectMode::Crt43))
                aspectMode = AspectMode::Crt43;
            if (ImGui::MenuItem("Integer scale (crisp)", nullptr,
                                aspectMode == AspectMode::Integer))
                aspectMode = AspectMode::Integer;
            ImGui::EndMenu();
        }

        // CRT glass sliders (scanlines / mask / barrel / persistence /
        // sharpness / BCS). The shared effect stack runs on every pipeline,
        // so this one panel governs the CRT look across all modes.
        ImGui::MenuItem("CRT Settings (sliders)...", nullptr, &uiState_->showNtscSettings);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Scanlines, shadow mask, barrel, phosphor curve,\n"
                              "persistence, brightness/contrast/saturation.");

        // 3D voxel view (MicroM8 "Voxel Cube"): rebuild the screen as an
        // upright 4:3 slab of equal-depth cubes; left-drag orbits, middle-drag
        // pans, wheel zooms. Works on any colour mode.
        ImGui::MenuItem("3D voxel view", nullptr, &uiState_->show3dVoxel);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("MicroM8-style cube renderer.\n"
                              "Left-drag orbits, middle-drag pans, wheel zooms.");
        ImGui::MenuItem("3D voxel settings...", nullptr, &uiState_->showVoxelSettings);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Depth, colour pop, fill, anti-alias, mono, per-colour depth.");

        // ── Color pipeline ──────────────────────────────────────────────
        // How the Apple II bit stream becomes colour. One pick; the CRT
        // glass below is an independent, composable layer (Phase 3/4 — one
        // shared effect stack downstream of every pipeline).
        ImGui::Separator();
        ImGui::TextDisabled("Color pipeline");
        const Apple2Display::HiResMode cur = display->getHiResMode();
        auto pipeItem = [&](const char* label, const char* tip,
                            Apple2Display::HiResMode m) {
            if (ImGui::MenuItem(label, nullptr, cur == m))
                display->setHiResMode(m);
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        pipeItem("NTSC (MAME)", "7-bit artifact LUT — the canonical composite look.",
                 Apple2Display::HiResMode::ColorNTSC);
        pipeItem("NTSC (MAME) — medium",
                 "composite_color_mode 1: biases 4-dot colour runs (uglier 40-col text).",
                 Apple2Display::HiResMode::ColorCompMedium);
        pipeItem("NTSC (MAME) — 4-bit square",
                 "composite_color_mode 2: each 4-dot nibble → palette index, sharp edges.",
                 Apple2Display::HiResMode::ColorComp4Bit);
        pipeItem("Composite (OpenEmulator, GPU)",
                 "True subcarrier demodulation in a GLSL shader (presets under Effect layers).",
                 Apple2Display::HiResMode::ColorCompositeOE);
        pipeItem("Composite (OpenEmulator, CPU)",
                 "Same OpenEmulator demodulation computed on the CPU into the\n"
                 "framebuffer — no GLSL shader. Works without a GL shader path\n"
                 "and lets you A/B the two. CRT effect layers still apply.",
                 Apple2Display::HiResMode::ColorCompositeOECpu);

        // AppleWin NTSC — only the TV (50% line-blur) sub-mode is exposed
        // (the Monitor / Idealized variants were dropped). Flat entry that
        // forces the Tv sub-mode and selects the pipeline.
        if (ImGui::MenuItem("AppleWin NTSC (TV 50% line blur)", nullptr,
                            cur == Apple2Display::HiResMode::ColorAppleWin)) {
            display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);
            display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        }

        // RGB card — clean Péritel decode, two distinct grays. Greyed out
        // when no Le Chat Mauve card is plugged in slot 7.
        ImGui::BeginDisabled(!deviceInventory.chatMauvePlugged());
        pipeItem("RGB card — Le Chat Mauve", nullptr,
                 Apple2Display::HiResMode::ChatMauveRGB);
        ImGui::EndDisabled();

        pipeItem("Monochrome — White",      nullptr, Apple2Display::HiResMode::MonoWhite);
        pipeItem("Monochrome — Green (P31)", nullptr, Apple2Display::HiResMode::MonoGreen);
        pipeItem("Monochrome — Amber",      nullptr, Apple2Display::HiResMode::MonoAmber);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        // ── Full screen = kiosk ─────────────────────────────────────────
        // There is no separate "full screen": going full screen IS kiosk
        // mode (exclusive full-screen, chrome-free, settings read-only).
        // The machine keeps running across the switch — no state is lost.
        if (ImGui::MenuItem(ICON_FA_EXPAND " Full screen (kiosk)",
                            "Ctrl+Alt+F", uiState_->kiosk)) {
            toggleKioskMode();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Exclusive full screen with no UI chrome — the kiosk view.\n"
                "Ctrl+Alt+F (or F10) toggles back; the emulated machine\n"
                "keeps running across the switch, so nothing is lost.\n"
                "Settings are not written while in kiosk.");

        // ── Mouse capture ───────────────────────────────────────────────
        // Greyed with no Mouse Card on the bus: capturing the pointer with
        // nothing to feed it is the one state this feature must not reach.
        {
            const bool haveCard = mouseInventory.plugged();
            if (ImGui::MenuItem(ICON_FA_ARROW_POINTER " Capture mouse",
                                "Ctrl+Alt+G", uiState_->mouseGrabbed, haveCard)) {
                toggleMouseGrab();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    haveCard
                    ? "Give the host pointer to the Apple II Mouse Card.\n"
                      "Captured, motion is unbounded (the guest cursor can\n"
                      "always reach its own clamp edges) and the OS cursor\n"
                      "is hidden. Ctrl+Alt+G or a middle click releases it."
                    : "No Mouse Card plugged — add 'mouse' (MAME, needs both\n"
                      "ROMs) or 'mouseaw' (AppleWin HLE) in Slot Configuration.");
        }
        ImGui::Separator();

        // ── Docking layout ──────────────────────────────────────────────
        // Task-oriented presets. No checkmarks on purpose: the entries are
        // actions, and the moment the user drags a tab the "active" preset
        // stops describing what's on screen.
        if (ImGui::BeginMenu(ICON_FA_TABLE_COLUMNS " Layout")) {
            auto layoutItem = [&](const char* label, DockLayout p,
                                  const char* tip) {
                if (ImGui::MenuItem(label)) {
                    pendingDockLayout_   = p;
                    dockLayoutRequested_ = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            layoutItem("Reset to default", DockLayout::Reset,
                       "Screen centre; Disk Library, Slot Configuration and\n"
                       "ImageWriter II tabbed right; inspectors as a tab\n"
                       "group bottom-right.");
            ImGui::Separator();
            layoutItem("Emulation", DockLayout::Emulation,
                       "Widest screen. Disk Library / Cassette / Floppy Emu\n"
                       "and Slot Config in one right column. No debug tools.");
            layoutItem("Debug", DockLayout::Debug,
                       "Memory viewer + maps right, horizontal map along the\n"
                       "bottom, inspectors bottom-right.");
            layoutItem("Audio", DockLayout::Audio,
                       "Mockingboard / Phasor / Echo+ right, mixer and tape\n"
                       "bottom-right.");
            ImGui::Separator();
            ImGui::TextDisabled("Drag any tab to re-dock; layout is saved.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Docks persist in ~/.config/POM2/imgui.ini.\n"
                    "Slot-numbered panels (Disk II, 3.5\", HDV, SmartPort,\n"
                    "Printer) build their title at runtime, so presets can't\n"
                    "place them — dock them once and they stay put.");
            ImGui::EndMenu();
        }
        ImGui::Separator();

        // ── Interface appearance ────────────────────────────────────────
        // Accent + zoom. Both re-theme immediately (applyUiTheme rebuilds
        // the style from scratch, so repeated calls don't compound the
        // scale) and both persist to state.cfg on exit.
        if (ImGui::BeginMenu(ICON_FA_PALETTE " Interface")) {
            ImGui::SeparatorText("Accent");
            std::size_t nAccents = 0;
            const pom2::UiAccent* accents = pom2::allAccents(nAccents);
            for (std::size_t i = 0; i < nAccents; ++i) {
                const pom2::UiAccent a = accents[i];
                if (ImGui::MenuItem(pom2::accentLabel(a), nullptr,
                                    uiAccent_ == a)) {
                    uiAccent_ = a;
                    applyUiTheme();
                }
            }

            ImGui::SeparatorText("Zoom");
            // Percent rather than a raw multiplier — "125 %" is the unit
            // every other desktop app uses for this control.
            int pct = static_cast<int>(uiScale_ * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderInt("##uiscale", &pct,
                                 static_cast<int>(pom2::kUiScaleMin * 100.0f),
                                 static_cast<int>(pom2::kUiScaleMax * 100.0f),
                                 "%d %%")) {
                uiScale_ = std::clamp(static_cast<float>(pct) / 100.0f,
                                      pom2::kUiScaleMin, pom2::kUiScaleMax);
                applyUiTheme();
            }
            auto zoomStep = [&](const char* label, float delta) {
                if (ImGui::MenuItem(label)) {
                    uiScale_ = std::clamp(uiScale_ + delta,
                                          pom2::kUiScaleMin, pom2::kUiScaleMax);
                    applyUiTheme();
                }
            };
            zoomStep("Zoom in",  +pom2::kUiScaleStep * 2.0f);
            zoomStep("Zoom out", -pom2::kUiScaleStep * 2.0f);
            if (ImGui::MenuItem("Reset to 100 %")) {
                uiScale_ = 1.0f;
                applyUiTheme();
            }
            if (dpiScale_ != 1.0f) {
                ImGui::Separator();
                ImGui::TextDisabled("Display scale: %.0f %% (from the OS)",
                                    dpiScale_ * 100.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Monitor content scale reported by the windowing\n"
                        "system. The zoom above multiplies on top of it —\n"
                        "effective UI scale is %.0f %%.",
                        uiScale_ * dpiScale_ * 100.0f);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();

        ImGui::MenuItem("Memory viewer",               nullptr, &uiState_->showMemViewer);
        ImGui::Separator();
        ImGui::MenuItem("Memory Map Bar",              nullptr, &uiState_->showMemoryBar);
        ImGui::MenuItem("Memory Map Bar (Horizontal)", nullptr, &uiState_->showMemoryBarH);
        ImGui::MenuItem("Memory Map Grid",             nullptr, &uiState_->showMemoryGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Command palette...", "Ctrl+Shift+P"))
            openCommandPalette();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fuzzy-search every menu item, panel and machine\n"
                              "action. Type \"mock\", \"amber\", \"eject\"...");
        ImGui::Separator();
        ImGui::MenuItem("HGR Paint Editor", nullptr, &uiState_->showHgrPaintEditor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Paint directly into HGR/GR/DHGR video RAM through "
                              "the real NTSC pipeline (image import included).");
        ImGui::MenuItem("HGR Sprite Editor", nullptr, &uiState_->showHgrSpriteEditor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw HGR sprites on a scratch page, grab from / "
                              "stamp to the live screen, export ca65 .byte tables.");
#ifndef __EMSCRIPTEN__
        // The AI Control HTTP server is compiled out under WASM
        // (AiControlServer::start() returns false — no listening socket in
        // the browser sandbox), so its entry is hidden there.
        ImGui::MenuItem("AI Control (HTTP)...", nullptr, &uiState_->showAiControlPanel);
#endif
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        ImGui::MenuItem("Welcome / Quick Start", nullptr, &uiState_->showWelcomePanel);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where to put ROMs/disks, keys, and signature features.");
        ImGui::MenuItem("ROM Status...", nullptr, &uiState_->showRomStatusPanel);
        ImGui::MenuItem("Abstraction Levels (LLE / HLE)...", nullptr,
                        &uiState_->showAbstractionPanel);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "What POM2 emulates as silicon and what it emulates as a\n"
                "service, subsystem by subsystem — plus which level is\n"
                "actually running (a missing ROM dump quietly demotes\n"
                "several of them) and the four boundaries you can move.");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Every ROM POM2 probes: present or missing, which\n"
                              "dump resolved, and what breaks without it.");
        ImGui::Separator();
        if (ImGui::MenuItem("About POM2")) uiState_->showAbout = true;
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// Bottom-of-viewport status bar. Carries the machine/mode/graphics summary,
// plus the three things that used to require opening a panel to answer:
// is a drive spinning and on what image, is the machine actually keeping up
// with the requested clock, and is host caps-lock on.
//
// Everything past the machine/mode/graphics group is optional and dropped
// when the window is too narrow (widths are measured in em so the pruning
// behaves the same at any UI scale).
void MainWindow::renderStatusBar()
{
    const bool mousePlugged = mouseCoordinator_->capture().plugged();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    // NoDocking: the status bar is chrome. Without it a dragged panel can be
    // dropped into the one-line strip at the bottom of the screen.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down,
                                    height, flags)) {
        if (ImGui::BeginMenuBar()) {
            const pom2::Palette& pal = pom2::palette();
            const auto  u32   = ImGui::ColorConvertU32ToFloat4;
            // Width unit: everything below budgets in ems so the drop-when-
            // narrow logic survives the UI zoom.
            const float em    = ImGui::GetFontSize();
            auto roomFor = [&](float ems) {
                return ImGui::GetContentRegionAvail().x > ems * em;
            };

            const auto mode = controller->getMode();
            const char* modeStr = "?";
            ImU32       modeCol = pal.textDim;
            switch (mode) {
                case EmulationController::Mode::Running:
                    modeStr = "RUN";  modeCol = pal.ok;   break;
                case EmulationController::Mode::Stopped:
                    modeStr = "STOP"; modeCol = pal.warn; break;
                case EmulationController::Mode::Step:
                    modeStr = "STEP"; modeCol = pal.info; break;
            }
            // Under the lock: this is live soft-switch state the worker
            // rewrites as the guest flips $C050-$C057, and it is copied out
            // as a struct, so an unlocked read can straddle a change.
            Memory::DisplayState state;
            { auto st = controller->lockState(); state = st.memory().getDisplayState(); }
            const char* gfx = state.textMode ? "TEXT"
                            : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                             : (state.mixedMode ? "LGR+TXT" : "LGR");
            const auto& cfg = pom2::profileConfig(activeProfile);

            // ── Machine · mode · graphics (always shown) ─────────────────
            ImGui::TextColored(u32(pal.textDim), "%.*s",
                               static_cast<int>(cfg.displayName.size()),
                               cfg.displayName.data());
            pom2::verticalRule();
            ImGui::TextColored(u32(modeCol), "%s", modeStr);
            pom2::verticalRule();
            ImGui::TextColored(u32(pal.textDim), "%s", gfx);

            // ── Mounted media, every bay, each with its own access LED ───
            // Walked off the SlotBus rather than the named card aliases so a
            // second Disk II, a second CFFA or a SmartPort's two units all
            // appear — the aliases only ever remember one card per kind.
            //
            // The bar is one line and the machine can carry a lot of media,
            // so each entry is added only while `roomFor` still says yes and
            // the rest are dropped silently. Entries go in bus order (slot,
            // then drive/bay) so a given machine's layout stays put instead
            // of reshuffling as drives spin up.
            //
            // LED semantics differ per bay and that is deliberate: a Disk II
            // lights on real spindle motion, while a block device has no
            // mechanics and instead bleeds off an activity counter
            // (`Block512Backing::isBusy`). SmartPort units expose no activity
            // signal at all, so theirs stays dark — better an honest dark LED
            // than one that never means anything.
            //
            // Built as a VALUE snapshot under `stateMutex`, then rendered
            // with the lock released. Both halves matter: `getDiskPath()`
            // hands back a reference into live `DiskImage` state that the AI
            // server's HTTP thread rewrites on /disk and /eject, and holding
            // `stateMutex` across ImGui calls is what deadlocked the memory
            // viewer (a non-recursive mutex, and ImGui callbacks can re-enter
            // host code). Snapshot, unlock, draw.
            struct MediaRow {
                bool        active = false;
                const char* icon   = nullptr;
                std::string label;
                std::string tip;
                // Identity, so a click on the chip can act on the exact bay
                // it names. `index` is the Disk II drive or the media bay.
                int         slot   = 0;
                int         index  = 0;
                bool        diskII = false;
                bool        dirty  = false;   // unsaved changes pending
            };
            std::vector<MediaRow> mediaRows;
            {
                auto baseName = [](const std::string& p) {
                    return std::filesystem::path(p).filename().string();
                };
                auto st = controller->lockState();
                for (int slot = 1; slot <= 7; ++slot) {
                    SlotPeripheral* per =
                        st.memory().slotBus().peripheral(slot);
                    if (!per) continue;

                    if (auto* d2 = dynamic_cast<DiskIICard*>(per)) {
                        for (int drv = 0; drv < 2; ++drv) {
                            if (!d2->isDiskLoaded(drv)) continue;
                            // Only the SELECTED drive's motor turns: a Disk II
                            // controller drives one spindle at a time.
                            const bool spinning =
                                d2->isMotorOn() && d2->getActiveDrive() == drv;
                            const std::string path = d2->getDiskPath(drv);
                            mediaRows.push_back(
                                { spinning, ICON_FA_FLOPPY_DISK,
                                  baseName(path),
                                  "Slot " + std::to_string(d2->getSlot()) +
                                      ", drive " + std::to_string(drv + 1) +
                                      " — track " +
                                      std::to_string(d2->getCurrentTrack(drv)) +
                                      "\n" + path,
                                  d2->getSlot(), drv, /*diskII=*/true,
                                  d2->hasUnsavedChanges(drv) });
                        }
                        continue;
                    }

                    // Everything else that can hold an image advertises bays,
                    // and each bay reports its own activity — a SmartPort's
                    // two units light independently, which a card-wide flag
                    // could not express.
                    auto* media = dynamic_cast<pom2::MountableMediaCard*>(per);
                    if (!media) continue;

                    for (int bay = 0; bay < media->bayCount(); ++bay) {
                        const pom2::MediaBayInfo info = media->bayInfo(bay);
                        if (!info.loaded || info.path.empty()) continue;
                        std::string tip = "Slot " + std::to_string(slot);
                        if (media->bayCount() > 1)
                            tip += ", bay " + std::to_string(bay + 1);
                        if (!info.kindLabel.empty())
                            tip += " — " + info.kindLabel;
                        tip += "\n" + info.path;
                        mediaRows.push_back({ info.busy, ICON_FA_HARD_DRIVE,
                                              baseName(info.path),
                                              std::move(tip),
                                              slot, bay, /*diskII=*/false,
                                              /*dirty=*/false });
                    }
                }
            }
            {
                const float lineH = ImGui::GetFrameHeight();
                int rowIdx = 0;
                for (const MediaRow& row : mediaRows) {
                    // 6 ems of chrome (rule + dot + icon + padding) plus the
                    // label itself, measured rather than guessed so a long
                    // filename cannot push the row off the end of the bar.
                    const float need =
                        6.0f * em + ImGui::CalcTextSize(row.label.c_str()).x;
                    if (ImGui::GetContentRegionAvail().x <= need) break;
                    ImGui::PushID(rowIdx++);
                    pom2::verticalRule();
                    pom2::indicatorDot(row.active, pal.warn, 4.0f, lineH);
                    // Each chip is a control, not a label: clicking it opens
                    // an eject menu for THAT bay. Brightening on hover is what
                    // says so — a status bar is read as read-only furniture
                    // until something under the pointer reacts.
                    const bool hot = ImGui::IsMouseHoveringRect(
                        ImGui::GetCursorScreenPos(),
                        ImVec2(ImGui::GetCursorScreenPos().x +
                                   ImGui::CalcTextSize(row.label.c_str()).x +
                                   2.0f * em,
                               ImGui::GetCursorScreenPos().y + lineH));
                    ImGui::TextColored(
                        u32(hot ? pal.accent : (row.active ? pal.text
                                                           : pal.textDim)),
                        "%s %s", row.icon, row.label.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n\nClick to eject.",
                                          row.tip.c_str());
                    // A menu rather than eject-on-click: the bar is a dense
                    // strip of small targets right under the screen, and an
                    // accidental click would pull a disk out from under a
                    // running program. One extra click also buys room to name
                    // the bay and to warn about unsaved changes.
                    if (ImGui::IsItemClicked()) ImGui::OpenPopup("##ejectmenu");
                    if (ImGui::BeginPopup("##ejectmenu")) {
                        ImGui::TextDisabled("%s", row.tip.c_str());
                        ImGui::Separator();
                        if (row.dirty)
                            ImGui::TextColored(
                                u32(pal.warn),
                                "Unsaved changes — ejecting writes them back\n"
                                "if write-back is on for this drive, and drops\n"
                                "them if it is not.");
                        if (ImGui::MenuItem(ICON_FA_EJECT " Eject"))
                            ejectMediaBay(row.slot, row.index, row.diskII);
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }

            // ── Mouse capture ────────────────────────────────────────────
            // The ONLY capture indicator now that the on-screen caption is
            // gone — this is the badge a user looks for when the pointer
            // "disappeared". Only ever shown while captured.
            if (uiState_->mouseGrabbed && roomFor(9.0f)) {
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.accent),
                                   ICON_FA_ARROW_POINTER " GRAB");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The host pointer is captured and feeding the Mouse\n"
                        "Card: motion no longer stops at the edge of the\n"
                        "screen widget, and the OS cursor is hidden.\n"
                        "Ctrl+Alt+G or a middle click gives it back.");
                // Spell the way out in full, in the bar, for a good while
                // after the capture — long enough to read without hunting
                // for a tooltip, and it costs nothing but bar width. It is
                // written out rather than left to the tooltip above because
                // a user who cannot find their pointer is not in a mood to
                // go hovering things to find out why.
                if (uiState_->lastFrameTime < uiState_->mouseGrabHintUntil && roomFor(30.0f)) {
                    ImGui::SameLine();
                    ImGui::TextColored(u32(pal.textDim),
                                       ICON_FA_ARROW_RIGHT
                                       " Ctrl+Alt+G or middle click to release");
                }
            } else if (!uiState_->mouseGrabbed && uiState_->screenHovered &&
                       mousePlugged && roomFor(30.0f)) {
                // Not captured, but the pointer is over the emulated screen
                // with a Mouse Card on the bus — the exact moment the user is
                // about to wonder why the guest cursor won't follow theirs.
                // Say how to hand it over, here rather than on the screen:
                // the on-screen captions were removed for being noise over a
                // running game, and this is the same information in the one
                // place that is already a status surface.
                //
                // `uiState_->screenHovered` is ImGui's z-order-aware verdict from
                // renderScreenWindow(), which runs earlier this frame — so a
                // menu or a docked panel drawn over the screen correctly
                // suppresses the hint. Gated on a card being plugged because
                // `shouldToggleGrab` would refuse to capture without one, and
                // advertising a shortcut that then does nothing is worse than
                // silence.
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.textDim),
                                   ICON_FA_ARROW_POINTER
                                   " Ctrl+Alt+G or middle click to capture");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The Mouse Card is a relative device: uncaptured, "
                        "your pointer\nstops at the edge of the screen "
                        "widget while the guest cursor\nstill has clamp "
                        "window left, and the two drift apart.\n"
                        "Capturing hides the OS cursor and feeds every delta "
                        "to the guest.");
            }

            // ── Host caps-lock ───────────────────────────────────────────
            // Only ever shown when ON: a permanent "CAPS off" badge would be
            // noise. Explains the classic "the game ignores my keys" report.
            if (uiState_->hostCapsLock && roomFor(8.0f)) {
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.warn), ICON_FA_KEYBOARD " CAPS");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Host caps-lock is on — every letter reaches the\n"
                        "Apple II as uppercase. Harmless on II/II+ (which\n"
                        "are uppercase-only) but breaks lowercase input on\n"
                        "//e and //c software.");
            }

            // A print in progress has to be visible without the paper tray
            // being open: the printer runs at 250 cps, so a page takes
            // minutes of host time, and with the real handshake enabled
            // the guest is deliberately frozen for that whole stretch.
            // Unexplained, that reads as a hung emulator.
            if (imageWriter && imageWriter->busy()) {
                const auto printerInventory =
                    devicePanelCoordinator_->captureInventory();
                const bool waiting =
                    uiState_->printerBackPressure &&
                    printerInventory.grapplerPlugged() &&
                    printerInventory.grapplerBusy;
                pom2::verticalRule();
                ImGui::TextColored(u32(pal.warn),
                                   ICON_FA_PRINT " printing %zu B%s",
                                   imageWriter->pendingBytes(),
                                   waiting ? " (Apple II waiting)" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The ImageWriter is still laying this job down at "
                        "its real speed.\nDevices → ImageWriter II to watch "
                        "the sheet, or \"Print now\" to skip the wait.");
            }

            // Transient disk load / boot (and other) status messages, shown
            // right-aligned and auto-expiring (uiState_->tapeStatusUntil). This is the
            // text that used to float in a separate overlay near the bottom.
            if (!uiState_->tapeStatusMessage.empty() && uiState_->lastFrameTime < uiState_->tapeStatusUntil) {
                const float msgW = ImGui::CalcTextSize(
                    uiState_->tapeStatusMessage.c_str()).x;
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > msgW) {
                    ImGui::SameLine(0.0f, avail - msgW);
                } else {
                    ImGui::SameLine();
                }
                ImGui::TextColored(
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent),
                    "%s", uiState_->tapeStatusMessage.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}
