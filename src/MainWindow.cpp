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
#include "PrinterCoordinator.h"
#include "SlotCardFactory.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotProvisioningCoordinator.h"
#include "SlotRebuildCoordinator.h"
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

namespace {

// state.cfg is a flat `key=value` file with one entry per line, so a list has
// to be packed into a single value. Disk paths can contain spaces, commas,
// semicolons and colons, so the separator must be a byte a path cannot hold:
// 0x1F (ASCII unit separator).
constexpr char kListSep = '\x1f';

std::string joinPaths(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += kListSep;
        out += v[i];
    }
    return out;
}

std::vector<std::string> splitPaths(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = s.find(kListSep, start);
        const std::string piece = (end == std::string::npos)
                                ? s.substr(start)
                                : s.substr(start, end - start);
        if (!piece.empty()) out.push_back(piece);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

} // anon namespace

MainWindow::MainWindow(bool forceIIPlus)
    // Member init order matches declaration order in MainWindow.h: the
    // controller is constructed first so memViewer can safely call
    // controller->memory() in its initialiser. Settings + AiControlServer
    // are heap-allocated for the same reason as the rest — keep their
    // headers out of MainWindow.h.
    : controller     (std::make_unique<EmulationController>()),
      display        (std::make_unique<Apple2Display>()),
      settings       (std::make_unique<pom2::Settings>()),
      devicePanelCoordinator_(
          std::make_unique<pom2::DevicePanelCoordinator>(*controller, *settings)),
      mouseCoordinator_(std::make_unique<pom2::MouseCoordinator>(*controller)),
      uiState_(std::make_unique<pom2::MainWindowUiState>()),
      audioCoordinator_(std::make_unique<pom2::AudioCoordinator>(
          controller->audio(), *controller)),
      debugCoordinator_(std::make_unique<pom2::DebugCoordinator>(*controller)),
      networkCoordinator_(std::make_unique<pom2::NetworkCoordinator>()),
      printerCoordinator_(std::make_unique<pom2::PrinterCoordinator>()),
      slotCardFactory_(std::make_unique<pom2::SlotCardFactory>()),
      slotCoordinator_(std::make_unique<pom2::SlotConfigurationCoordinator>()),
      storageCoordinator_(std::make_unique<pom2::StorageCoordinator>()),
      slotProvisioningCoordinator_(
          std::make_unique<pom2::SlotProvisioningCoordinator>(
              *slotCardFactory_, *storageCoordinator_)),
      cassetteDeck   (std::make_unique<pom2::CassetteDeck_ImGui>()),
      rewindPanel_   (std::make_unique<pom2::Rewind_ImGui>()),
      disk35Panel    (std::make_unique<pom2::Disk35Controller_ImGui>()),
      diskLibrary    (std::make_unique<pom2::DiskLibrary_ImGui>()),
      cmdPalette     (std::make_unique<pom2::CommandPalette_ImGui>()),
      hdvPanel       (std::make_unique<pom2::HdvController_ImGui>()),
      smartPortPanel (std::make_unique<pom2::SmartPort_ImGui>()),
      fujiNetPanel   (std::make_unique<pom2::FujiNet_ImGui>()),
      floppyEmu      (std::make_unique<pom2::FloppyEmuDevice>()),
      floppyEmuPanel (std::make_unique<pom2::FloppyEmu_ImGui>()),
      joystickPanel  (std::make_unique<pom2::JoystickPanel_ImGui>()),
      printerSound   (std::make_unique<pom2::PrinterSoundDevice>()),
      printerHistory (std::make_unique<pom2::PrinterHistory>()),
      imageWriter    (std::make_unique<pom2::ImageWriter>()),
      imageWriterPanel(std::make_unique<pom2::ImageWriter_ImGui>()),
      chatMauvePanel (std::make_unique<pom2::LeChatMauve_ImGui>()),
      toolbar        (std::make_unique<pom2::Toolbar_ImGui>()),
      hgrPaintHost   (std::make_unique<Pom2HgrPaintHost>(controller.get())),
      hgrPaintEditor (std::make_unique<hgrpaint::HgrPaintEditor>(hgrPaintHost.get())),
      hgrSpriteEditor(std::make_unique<hgrsprite::HgrSpriteEditor>(hgrPaintHost.get())),
      joystick       (std::make_unique<JoystickInput>()),
      sscPortInput   (SuperSerialCard::kDefaultPort),
      aiServer       (std::make_unique<pom2::AiControlServer>()),
      slotRebuildCoordinator_(
          std::make_unique<pom2::SlotRebuildCoordinator>(
              pom2::SlotRebuildCoordinator::Hooks{
                  [this] {
                      slotProvisioningCoordinator_->resetSessionTracking();
                      controller->rewind().clear();
                  },
                  [this] { aiServer->detach(); },
                  [this] { unregisterAllAudioSources(); },
                  [this] {
                      diskPanels.clear();
                      diskPanel = nullptr;
                  },
                  [this] { printerCoordinator_->resetFeedCursor(); },
                  [this] { networkCoordinator_->stopHelper(); },
                  [this] { display->setChatMauveCard(nullptr); },
                  [this] { aiServer->attach(controller.get(), display.get()); },
              })),
      aiPortInput    (pom2::AiControlServer::kDefaultPort),
      charRomLocale  (pom2::CharRomLocale::ProfileDefault),
      activeProfile  (pom2::SystemProfile::AppleIIPlus)
{
    // Load any persisted runtime config. Missing/malformed file → use
    // defaults; the fields below honour the saved values when present.
    settings->load();

    // Probe a few common locations so the binary works whether launched
    // from build/ or the repo root. Apple IIe (16 KB ROM at $C000-$FFFF
    // with internal I/O ROM in $C100-$CFFF) takes precedence: if
    // roms/apple2e.rom is present we run as a IIe (128 KB, 80-col, IIe
    // soft switches). Otherwise the legacy II+ path runs as before.
    namespace fs = std::filesystem;
    static const char* iieRomCandidates[]   = { "roms/apple2e.rom",
                                                "../roms/apple2e.rom",
                                                "../../roms/apple2e.rom" };
    static const char* romCandidates[]      = { "roms/apple2.rom",
                                                "../roms/apple2.rom",
                                                "../../roms/apple2.rom" };
    // Char ROM probing. Prefer the 4 KB IIe Enhanced variant (mousetext
    // + lowercase) when running in IIe mode; fall back to the 2 KB II/II+
    // ROM otherwise. Both formats are normalised to AppleWin-style
    // csbits in `Memory::loadCharRom`, so the renderer is uniform.
    static const char* charRomIIeCandidates[] = { "roms/apple2e_char.rom",
                                                  "../roms/apple2e_char.rom",
                                                  "../../roms/apple2e_char.rom" };
    static const char* charRomCandidates[]   = { "roms/apple2_char.rom",
                                                "../roms/apple2_char.rom",
                                                "../../roms/apple2_char.rom" };

    // findResource resolves each candidate against the CWD, the build/-
    // relative roots (dev), and the executable-relative / FHS roots
    // (portable bundle, AppImage, /usr/bin). See ResourcePaths.h.
    bool iiePresent = false;
    if (!forceIIPlus) {
        for (const char* p : iieRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { romPath = r; iiePresent = true; break; }
        }
    }
    if (!iiePresent) {
        for (const char* p : romCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { romPath = r; break; }
        }
    }
    charRomPath.clear();
    // Restore user-selected character ROM locale (toolbar dropdown).
    // ProfileDefault keeps the legacy auto-probe; anything else maps
    // to a specific file in roms/. The override is applied here BEFORE
    // the probe so the very first frame already shows the chosen font
    // — otherwise applyProfile() catches up a few hundred ms later
    // and the user briefly sees the wrong glyphs.
    charRomLocale = pom2::charRomLocaleFromKey(
        settings->getString("char_rom_locale", "default"));
    if (charRomLocale != pom2::CharRomLocale::ProfileDefault) {
        // resolveCharRomPath probes roms/X, ../roms/X, ../../roms/X —
        // same prefix sweep as the legacy IIe probe, so the override
        // works whether POM2 is launched from the repo root or from
        // build/.
        const std::string overridePath =
            pom2::resolveCharRomPath(charRomLocale);
        if (!overridePath.empty()) {
            charRomPath = overridePath;
        } else {
            // File missing — fall through to the legacy probe so we
            // don't end up with a blank screen, and reset the saved
            // locale so the dropdown reflects what actually loaded.
            charRomLocale = pom2::CharRomLocale::ProfileDefault;
        }
    }
    if (charRomPath.empty() && iiePresent) {
        for (const char* p : charRomIIeCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { charRomPath = r; break; }
        }
    }
    if (charRomPath.empty()) {
        for (const char* p : charRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { charRomPath = r; break; }
        }
    }

    // Constructor: the CPU worker is not running yet (controller->start()
    // comes later), so the raw accessor is correct here — there is no
    // second thread to be raced by. Everything past start() uses
    // lockState().
    if (iiePresent) {
        controller->memory().setIIEMode(true);
        const int banks = settings->getInt("ramworks_banks", 1);
        controller->memory().setRamWorksBanks(
            static_cast<uint32_t>(banks > 0 ? banks : 1));
        display->setAuxMemory(controller->memory().auxData());
    }

    if (controller->memory().loadAppleIIRom(romPath.c_str())) {
        romStatus = std::string(iiePresent ? "IIe (128K): " : "loaded: ") + romPath;
        romLoaded_ = true;
    } else {
        romStatus = std::string("NO ROM (") + romPath +
                    ") — only $D000-$FFFF stub is active";
        romLoaded_ = false;
        // First-launch newcomer with no firmware: greet them with the
        // Welcome panel (folders + expected filenames + Reload button)
        // instead of leaving them staring at a bare "NO ROM" screen.
        uiState_->showWelcomePanel = true;
    }
    controller->memory().loadCharRom(charRomPath.c_str(),
                                     pom2::charRomBank(charRomLocale));

    // Load the MAME floppy sound samples (head step, motor spin, insert
    // click) for both 5.25" and 3.5" form factors. Each FloppySoundDevice
    // instance stores a single sample bank; we have two — one per form
    // factor — wired to DiskIICard / Sony35Drive / SmartPortCard. Probe
    // paths mirror the ROM probe order; the first directory containing
    // either set wins, the other set degrades to silent if absent.
    static const char* floppySampleDirs[] = {
        "roms/floppy_samples",
        "../roms/floppy_samples",
        "../../roms/floppy_samples",
    };
    for (const char* d : floppySampleDirs) {
        const std::string dir = pom2::findResource(d);
        if (dir.empty() || !fs::is_directory(dir)) continue;
        const bool ok525 = controller->floppySound525().loadSamples(
            dir, FloppySoundDevice::FormFactor::FF525);
        const bool ok35  = controller->floppySound35().loadSamples(
            dir, FloppySoundDevice::FormFactor::FF35);
        if (ok525 || ok35) break;
    }
    audioCoordinator_->restore(*settings,
                               controller->speaker(),
                               controller->cassette(),
                               controller->floppySound525(),
                               controller->floppySound35(),
                               *printerSound);

    // Plug all expansion cards in their user-configured slots. The
    // mapping is read from `slot_1_card`..`slot_7_card` settings; absent
    // keys fall back to the legacy defaults (DiskII=6, HDV=5, SSC=2,
    // Clock=4, ChatMauve=7) so first-run users see no regression.
    // The lock is free here (the worker starts later in this ctor), but
    // plugSlotsFromSettings takes the handle rather than the mutex on
    // purpose — its other two callers are already holding it.
    {
        auto st = controller->lockState();
        plugSlotsFromSettings(st);
        restoreSlotMediaFromSettings(st);
    }

    // ── Restore display + UI prefs from previous session ─────────────
    {
        // Default when the key is absent (fresh install) is the
        // OpenEmulator GPU composite: it is POM2's best-looking colour
        // pipeline and the one the CRT Settings panel is built around. It
        // degrades safely — with no GL shader available `NtscPostProcessor`
        // falls back to the NTSC LUT, so this can't leave a user with a
        // black screen.
        const std::string mode =
            settings->getString("hi_res_mode", "ColorCompositeOE");
        if      (mode == "ColorNTSC")       display->setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        else if (mode == "ColorCompMedium") display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
        else if (mode == "ColorComp4Bit")   display->setHiResMode(Apple2Display::HiResMode::ColorComp4Bit);
        else if (mode == "ChatMauveRGB")    display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        else if (mode == "ColorCompositeOE") display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
        else if (mode == "ColorCompositeOECpu") display->setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        else if (mode == "MonoWhite")       display->setHiResMode(Apple2Display::HiResMode::MonoWhite);
        else if (mode == "MonoGreen")       display->setHiResMode(Apple2Display::HiResMode::MonoGreen);
        else if (mode == "MonoAmber")       display->setHiResMode(Apple2Display::HiResMode::MonoAmber);
        else if (mode == "ColorAppleWin")   display->setHiResMode(Apple2Display::HiResMode::ColorAppleWin);
        // AppleWin NTSC: only the TV (50% line-blur) sub-mode is exposed now
        // (Monitor / Idealized were dropped) — force it regardless of any
        // legacy applewin_submode value left in the settings file.
        display->setAppleWinSubMode(Apple2Display::AppleWinSubMode::Tv);

        // Seed the toolbar's colour-side memory from what was just restored,
        // so the first mono → colour round-trip returns to the mode the user
        // is actually looking at rather than to this member's ColorNTSC
        // initialiser. Mattered the moment the default stopped being
        // ColorNTSC.
        {
            const auto m = display->getHiResMode();
            if (m == Apple2Display::HiResMode::MonoWhite ||
                m == Apple2Display::HiResMode::MonoGreen ||
                m == Apple2Display::HiResMode::MonoAmber)
                lastMonoHiResMode_ = m;
            else
                lastColorHiResMode_ = m;
        }

        uiState_->showDiskPanel      = settings->getBool ("show_disk_panel", uiState_->showDiskPanel);
        uiState_->showDisk35Panel    = settings->getBool ("show_disk35_panel", uiState_->showDisk35Panel);
        uiState_->showDiskLibrary    = settings->getBool ("show_disk_library", uiState_->showDiskLibrary);
        uiState_->showHdvPanel       = settings->getBool ("show_hdv_panel",  uiState_->showHdvPanel);
        uiState_->showSmartPortPanel = settings->getBool ("show_smartport_panel", uiState_->showSmartPortPanel);
        uiState_->showFujiNetPanel   = settings->getBool ("show_fujinet_panel",   uiState_->showFujiNetPanel);
        uiState_->showSlotConfigPanel = settings->getBool ("show_slot_config", uiState_->showSlotConfigPanel);
        uiState_->showMediaPanel      = settings->getBool ("show_media_panel", uiState_->showMediaPanel);
        uiState_->showRomStatusPanel  = settings->getBool ("show_rom_status", uiState_->showRomStatusPanel);
        uiState_->showAbstractionPanel = settings->getBool ("show_abstraction", uiState_->showAbstractionPanel);
        uiState_->showKeyboardPanel   = settings->getBool ("show_keyboard", uiState_->showKeyboardPanel);
        uiState_->showFloppyEmu      = settings->getBool ("show_floppy_emu", uiState_->showFloppyEmu);
        // Floppy Emu: restore the emulation mode + SD-card root (its NVRAM).
        {
            pom2::FloppyEmuMode fm;
            if (pom2::FloppyEmuDevice::modeFromKey(
                    settings->getString("floppyemu_mode", "smartporthd"), fm))
                floppyEmu->setMode(fm);
            std::string sd = settings->getString("floppyemu_sd_root", "");
            if (sd.empty()) {
                // The Floppy Emu owns a DEDICATED 'floppyemu/' folder — its
                // virtual SD card, kept separate from the Disk Library's
                // disks_5.4/ ・ disks_3.5/ ・ hdv/. Probe the usual cwd anchors and,
                // if none exists, create it so there's always a clear place
                // to drop images.
                namespace fs = std::filesystem;
                std::error_code ec;
                for (const char* c : { "floppyemu", "../floppyemu",
                                       "../../floppyemu" }) {
                    if (fs::is_directory(c, ec)) { sd = c; break; }
                }
                if (sd.empty()) {
                    fs::create_directories("floppyemu", ec);
                    sd = "floppyemu";
                }
            }
            floppyEmu->setSdRoot(sd);
        }
        uiState_->showCassetteDeck   = settings->getBool ("show_cassette",   uiState_->showCassetteDeck);
        uiState_->showHgrPaintEditor = settings->getBool ("show_hgr_paint",  uiState_->showHgrPaintEditor);
        uiState_->showHgrSpriteEditor = settings->getBool("show_hgr_sprite", uiState_->showHgrSpriteEditor);
        {
            hgrpaint::HgrPaintEditor::Session hs;
            hs.mode       = settings->getInt   ("hgr_paint_mode",  0);
            hs.page2      = settings->getBool  ("hgr_paint_page2", false);
            hs.zoomIdx    = settings->getInt   ("hgr_paint_zoom",  2);
            hs.ntscColor  = settings->getBool  ("hgr_paint_ntsc",  true);
            hs.aspect43   = settings->getBool  ("hgr_paint_43",    false);
            hs.canvasPipeline = settings->getInt("hgr_paint_pipe", 0);
            hs.browserDir = settings->getString("hgr_paint_dir",   "");
            hgrPaintEditor->restoreSession(hs);
        }
        uiState_->showRewindBar      = settings->getBool ("show_rewind",     uiState_->showRewindBar);
        controller->rewind().setEnabled(settings->getBool("rewind_enabled", false));
        uiState_->showJoystickPanel  = settings->getBool ("show_joystick",   uiState_->showJoystickPanel);
        joystick->binding().squareGate =
            settings->getBool("joystick_square_gate",
                              joystick->binding().squareGate);
        uiState_->showMouseInspector = settings->getBool ("show_mouse_inspector",
                                                 uiState_->showMouseInspector);
        uiState_->showChatMauvePanel = settings->getBool ("show_chatmauve",  uiState_->showChatMauvePanel);
        uiState_->showMockingboardPanel = settings->getBool ("show_mockingboard",
                                                  uiState_->showMockingboardPanel);
        uiState_->showPhasorPanel    = settings->getBool ("show_phasor",     uiState_->showPhasorPanel);
        uiState_->showEchoPlusPanel  = settings->getBool ("show_echoplus",   uiState_->showEchoPlusPanel);
        uiState_->showAudioMixer     = settings->getBool ("show_mixer",      uiState_->showAudioMixer);
        uiState_->showSscPanel       = settings->getBool ("show_ssc",        uiState_->showSscPanel);
        uiState_->showEthernetPanel  = settings->getBool ("show_ethernet",   uiState_->showEthernetPanel);
        uiState_->showPrinterPanel   = settings->getBool ("show_printer",    uiState_->showPrinterPanel);
        uiState_->showImageWriterPanel =
            settings->getBool("show_imagewriter", uiState_->showImageWriterPanel);
        // Paper + raster density survive a restart; the printed sheets
        // themselves deliberately do not (they are output, like the spool).
        {
            const int paper = settings->getInt("imagewriter_paper", 0);
            if (paper > 0 && paper < static_cast<int>(
                                pom2::ImageWriter::PaperSize::Count))
                imageWriter->setPaperSize(
                    static_cast<pom2::ImageWriter::PaperSize>(paper));
            // Mechanical sound. Levels are restored here; the REGISTRATION
            // lives at the end of plugSlotsFromSettings() instead, because
            // both slot-rebuild paths (profile switch, Slot Config "Apply")
            // call unregisterAllAudioSources() and then only re-register
            // card-owned sources. Registering here meant the printer went
            // permanently silent after the first profile switch — including
            // the one the constructor itself performs when the saved profile
            // differs from the ROM auto-probe.
            imageWriter->setSoundSink(printerSound.get());

            // Durable printouts, alongside the spool and trace files POM2
            // already writes there.
            {
                std::string herr;
                const auto historyDir = pom2::userDataDir() / "printouts" / "history";
                if (!printerHistory->open(historyDir.string(), herr))
                    pom2::log().warn("PrinterHistory", herr);
            }

            imageWriter->setModel(static_cast<pom2::IwModel>(
                std::clamp(settings->getInt("imagewriter_model", 0), 0,
                           static_cast<int>(pom2::IwModel::Count) - 1)));
            imageWriter->setDpi(settings->getInt("imagewriter_dpi",
                                                 imageWriter->dpi()));
            // Line-feed-after-CR switch. Old configs stored a bool; the
            // mode (Auto/On/Off) supersedes it and Auto is the default —
            // it settles the question from the stream itself.
            imageWriter->setAutoFeedMode(
                static_cast<pom2::ImageWriter::AutoFeed>(
                    std::clamp(settings->getInt("imagewriter_autolf_mode",
                        static_cast<int>(pom2::ImageWriter::AutoFeed::Auto)),
                        0, static_cast<int>(
                               pom2::ImageWriter::AutoFeed::Count) - 1)));
            // POM2_TRACE_PRINTER=1 (or =<path>) opens the printer trace
            // before anything can print, so a printout that goes wrong
            // during boot is captured too.
            if (const char* t = std::getenv("POM2_TRACE_PRINTER")) {
                if (*t && std::strcmp(t, "0") != 0) {
                    const std::string path =
                        (std::strcmp(t, "1") == 0)
                            ? (pom2::userDataDir() / "printouts" /
                               "imagewriter_trace.log").string()
                            : std::string(t);
                    std::string err;
                    if (imageWriter->startTrace(path, err))
                        pom2::log().info("ImageWriter", "Tracing to " + path);
                    else
                        pom2::log().warn("ImageWriter", err);
                }
            }
            uiState_->printerBackPressure =
                settings->getBool("imagewriter_backpressure", false);
            imageWriter->setRibbon(
                static_cast<pom2::ImageWriter::Ribbon>(
                    std::clamp(settings->getInt("imagewriter_ribbon", 0), 0,
                               static_cast<int>(
                                   pom2::ImageWriter::Ribbon::Count) - 1)));
            const int spd = settings->getInt(
                "imagewriter_speed",
                static_cast<int>(pom2::ImageWriter::Speed::Draft));
            if (spd >= 0 && spd < static_cast<int>(
                                pom2::ImageWriter::Speed::Count))
                imageWriter->setSpeed(
                    static_cast<pom2::ImageWriter::Speed>(spd));
        }
        sscPortInput       = settings->getInt  ("ssc_port",        sscPortInput);
        diskTurboWhileMotor = settings->getBool("disk_turbo",      diskTurboWhileMotor);
        // Dallas DS1216E "No-Slot Clock" — sits under the Monitor ROM
        // and ProDOS 2.0.3+ / GS-OS auto-detect it via the magic-key
        // scan. Default ON (battery-backed RTC for all profiles incl.
        // //c, which never had a slot to host a ThunderClock card).
        controller->noSlotClock().setEnabled(
            settings->getBool("nsclock_enable", true));
        uiState_->showNoSlotClockPanel = settings->getBool("show_nsclock",
                                                 uiState_->showNoSlotClockPanel);
        uiState_->showNtscSettings   = settings->getBool("show_ntsc",
                                               uiState_->showNtscSettings);
        // Composite-NTSC shader params (saved under ntsc_*). We can't
        // call ntscFx->setParams() yet because the postprocessor is
        // lazy-constructed in drawScreenImage; stash them into a
        // pending-params instance that will be picked up on the first
        // construction.
        {
            pom2::NtscParams p;
            p.brightness  = settings->getFloat("ntsc_brightness",  p.brightness);
            p.contrast    = settings->getFloat("ntsc_contrast",    p.contrast);
            p.saturation  = settings->getFloat("ntsc_saturation",  p.saturation);
            p.hue         = settings->getFloat("ntsc_hue",         p.hue);
            p.sharpness   = settings->getFloat("ntsc_sharpness",   p.sharpness);
            p.persistence = settings->getFloat("ntsc_persistence", p.persistence);
            p.scanlines   = settings->getFloat("ntsc_scanlines",   p.scanlines);
            p.barrel      = settings->getFloat("ntsc_barrel",      p.barrel);
            p.shadowMaskStrength = settings->getFloat(
                "ntsc_shadow_strength", p.shadowMaskStrength);
            p.luminanceGain = settings->getFloat(
                "ntsc_luminance_gain", p.luminanceGain);
            p.centerLighting = settings->getFloat(
                "ntsc_center_lighting", p.centerLighting);
            p.phosphorGamma = settings->getFloat(
                "ntsc_phosphor_gamma", p.phosphorGamma);
            const int sm = settings->getInt("ntsc_shadow_mask",
                                            static_cast<int>(p.shadowMask));
            p.shadowMask = static_cast<pom2::NtscParams::ShadowMask>(
                std::clamp(sm, 0, 3));
            p.palMode    = settings->getBool("ntsc_pal",        p.palMode);
            p.textSharp  = settings->getBool("ntsc_text_sharp", p.textSharp);
            // Clamp every float to its slider range: only values created
            // in-app are slider-bounded — a hand-edited/corrupted state.cfg
            // with e.g. ntsc_center_lighting=0 hits 1.0/uCenterLighting in
            // the glass shader → exp(-inf) → fully black screen everywhere.
            p.brightness         = std::clamp(p.brightness,        -0.5f, 0.5f);
            p.contrast           = std::clamp(p.contrast,           0.5f, 1.5f);
            p.saturation         = std::clamp(p.saturation,         0.0f, 2.0f);
            p.hue                = std::clamp(p.hue,               -0.5f, 0.5f);
            p.sharpness          = std::clamp(p.sharpness,          0.0f, 1.0f);
            p.persistence        = std::clamp(p.persistence,        0.0f, 0.95f);
            p.scanlines          = std::clamp(p.scanlines,          0.0f, 1.0f);
            p.barrel             = std::clamp(p.barrel,             0.0f, 0.30f);
            p.shadowMaskStrength = std::clamp(p.shadowMaskStrength, 0.0f, 1.0f);
            p.luminanceGain      = std::clamp(p.luminanceGain,      1.0f, 2.0f);
            p.centerLighting     = std::clamp(p.centerLighting,     0.5f, 1.0f);
            p.phosphorGamma      = std::clamp(p.phosphorGamma,      0.6f, 2.6f);
#ifdef __EMSCRIPTEN__
            p.barrel = std::min(p.barrel, 0.03f);
#endif
            ntscFx = std::make_unique<pom2::NtscPostProcessor>();
            ntscFx->setParams(p);
        }
        crtEffectsEnabled = settings->getBool("crt_effects_enabled",
                                              crtEffectsEnabled);
        uiState_->show3dVoxel      = settings->getBool("show_3d_voxel", uiState_->show3dVoxel);
        uiState_->showVoxelSettings = settings->getBool("show_voxel_settings",
                                               uiState_->showVoxelSettings);
        // Own the renderer up-front (ctor is GL-free; initialize() stays lazy)
        // so the settings panel and persistence can bind to its tunables even
        // before the 3D view is first toggled on.
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // Same slider-range clamps as the NTSC params above: negative depth
        // extrudes the slab away, fill 0 makes every cube invisible, and
        // ambient >1 flips the diffuse term — all persisted, so a stray
        // Ctrl+click-typed value would survive restarts.
        voxel3d_->voxelDepth  = std::clamp(settings->getFloat("voxel_depth",      voxel3d_->voxelDepth),  0.0f, 12.0f);
        voxel3d_->colorShift  = std::clamp(settings->getFloat("voxel_colorshift", voxel3d_->colorShift),  0.0f, 24.0f);
        voxel3d_->cubeFill    = std::clamp(settings->getFloat("voxel_fill",       voxel3d_->cubeFill),    0.2f, 1.0f);
        voxel3d_->ambient     = std::clamp(settings->getFloat("voxel_ambient",    voxel3d_->ambient),     0.0f, 1.0f);
        voxel3d_->superSample = std::clamp(settings->getInt  ("voxel_supersample", voxel3d_->superSample), 1, 4);
        voxel3d_->mono          = settings->getBool("voxel_mono",          voxel3d_->mono);
        voxel3d_->perColorDepth = settings->getBool("voxel_percolor_depth", voxel3d_->perColorDepth);
        const std::string asp = settings->getString("aspect_mode", "");
        if      (asp == "crt43")   aspectMode = AspectMode::Crt43;
        else if (asp == "integer") aspectMode = AspectMode::Integer;
        else if (asp == "square")  aspectMode = AspectMode::Square;

        // Interface appearance. Only stored here — the theme is applied by
        // `setDpiScale()`, which main() calls right after construction with
        // the monitor's content scale (unknown at this point). Clamped so a
        // hand-edited state.cfg can't leave the UI unusably small or huge.
        uiAccent_ = pom2::accentFromKey(
            settings->getString("ui_accent",
                                pom2::accentKey(uiAccent_)).c_str());
        uiScale_  = std::clamp(settings->getFloat("ui_scale", uiScale_),
                               pom2::kUiScaleMin, pom2::kUiScaleMax);
        // Docking: has a layout already been seeded into imgui.ini? Without
        // this the default layout would be rebuilt on every launch, throwing
        // away whatever the user had docked.
        dockSeeded_ = settings->getBool("ui_dock_seeded", false);
        libraryFavourites_ = splitPaths(settings->getString("library_favourites", ""));
        libraryRecents_    = splitPaths(settings->getString("library_recents", ""));
        libraryHideSizeDate_ = settings->getBool("library_hide_sizedate", false);
        if (libraryRecents_.size() > kMaxLibraryRecents)
            libraryRecents_.resize(kMaxLibraryRecents);
#ifdef __EMSCRIPTEN__
        // Browser startup is intentionally chrome-light: keep only the menu,
        // toolbar, Apple II Screen window, and bottom status bar. Users can
        // still open panels from the menus after boot.
        display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
        lastColorHiResMode_ = Apple2Display::HiResMode::ColorCompMedium;
        uiState_->showDiskPanel = uiState_->showDisk35Panel = uiState_->showDiskLibrary = false;
        uiState_->showHdvPanel = uiState_->showSmartPortPanel = uiState_->showSlotConfigPanel = false;
        uiState_->showMediaPanel = uiState_->showRomStatusPanel = false;
        uiState_->showFloppyEmu = uiState_->showCassetteDeck = uiState_->showJoystickPanel = false;
        uiState_->showMouseInspector = uiState_->showChatMauvePanel = false;
        uiState_->showMockingboardPanel = uiState_->showPhasorPanel = uiState_->showEchoPlusPanel = false;
        uiState_->showAudioMixer = uiState_->showSscPanel = uiState_->showPrinterPanel = false;
        uiState_->showImageWriterPanel = false;
        uiState_->showNoSlotClockPanel = uiState_->showNtscSettings = uiState_->showAiControlPanel = false;
        uiState_->showVoxelSettings = false;
        uiState_->showMemViewer = uiState_->showMemoryBar = uiState_->showMemoryBarH = uiState_->showMemoryGrid = false;
#endif
    }

    // ── Restore previously-mounted 3.5" disks ─────────────────────────
    // Same pattern as the 5.25" / HDV restore above. Only honour the
    // paths when the file still exists; silently skip otherwise so a
    // moved / deleted image doesn't block startup.
    {
        std::error_code ec;
        const auto disk35 = storageCoordinator_->captureDisk35(*controller);
        // SmartPort units were already restored from their per-slot settings
        // and are authoritative when present. Legacy disk35_path_N belongs
        // only to the otherwise-active on-board pair.
        if (!disk35.usesSmartPort()) {
            const std::string p1 = settings->getString("disk35_path_1", "");
            if (!p1.empty() && fs::is_regular_file(p1, ec) &&
                storageCoordinator_->mountDisk35(
                    *controller, *settings, 0, p1).ok) {
                pom2::log().info(
                    "Sony35", "Internal re-mounted from settings: " + p1);
            }
            const std::string p2 = settings->getString("disk35_path_2", "");
            if (!p2.empty() && fs::is_regular_file(p2, ec) &&
                storageCoordinator_->mountDisk35(
                    *controller, *settings, 1, p2).ok) {
                pom2::log().info(
                    "Sony35", "External re-mounted from settings: " + p2);
            }
        }
    }

    // Always wake up at the Applesoft prompt. A default HDV / disk may be
    // mounted (above), but we never auto-boot — the user picks via the
    // Disk II / HDV panel libraries. Use coldBoot (not just a CPU reset)
    // so the Apple II Monitor runs its full cold-start sequence: HOME
    // clears the freshly-zeroed text page so the user briefly sees the
    // "Apple //e" banner instead of the `@`-tile garbage that the text
    // page renders when full of $00, then it tries slot 6, fails (no
    // disk in drive at first launch), and falls through to AppleSoft.
    controller->coldBoot();
    controller->setMode(EmulationController::Mode::Running);
    controller->start();

    // ── AI control server (loopback HTTP) ────────────────────────────────
    // Wire the bridge once the emulator core is alive so the server's
    // first request hits a fully-formed emulator. Auto-start only if
    // the last session left it on — fresh users opt in via the panel.
    aiPortInput   = settings->getInt   ("ai_control_port",   aiPortInput);
    aiTokenInput  = settings->getString("ai_control_token",  "");
    aiServer->attach(controller.get(), display.get());
    aiServer->setAuthToken(aiTokenInput);
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
    uiState_->showAiControlPanel = settings->getBool("show_ai_control", uiState_->showAiControlPanel);
    if (settings->getBool("ai_control_enable", false)) {
        aiServer->start(static_cast<uint16_t>(aiPortInput));
    }

    // Determine the active profile from what the legacy boot path
    // resolved. If a `system_profile` setting was persisted from a
    // previous launch AND it disagrees with the auto-detected one, the
    // user explicitly picked that profile last time — honour it via a
    // full cold reset via applyProfile() (which the menu also calls).
    activeProfile = iiePresent ? pom2::SystemProfile::AppleIIe
                               : pom2::SystemProfile::AppleIIPlus;
    // `--ii-plus` (forceIIPlus) must win over any persisted profile: it was
    // requested precisely to avoid the IIe path. Without this guard the
    // saved-profile catch-up below would re-apply a saved iie/iic/iic+ and
    // silently defeat the flag. (forceIIPlus already suppressed the IIe ROM
    // probe above, so activeProfile is AppleIIPlus here.)
    //
    // A fresh install (no `system_profile` key) defaults to **//e Enhanced
    // PAL**: 50 Hz European timing is what the French Touch / DIX demo corpus
    // POM2 benchmarks against is written for, and it is a superset machine —
    // 128 K, 65C02, 80 columns, all seven slots free. It is expressed as a
    // default for `getString` rather than as a new `activeProfile` initialiser
    // so the catch-up below runs `applyProfile` for it, which is what actually
    // installs the PAL video standard, the 20313-cycle frame budget and the
    // per-card clock updates. Falls back to the auto-probed profile when no
    // //e ROM was found (a PAL //e with no //e ROM would just fail to boot).
    const std::string defaultProfile =
        iiePresent ? std::string("iie-pal") : std::string();
    const std::string savedProfile =
        forceIIPlus ? std::string()
                    : settings->getString("system_profile", defaultProfile);
    if (!savedProfile.empty()) {
        const pom2::SystemProfile saved = pom2::profileFromKey(savedProfile);
        if (saved != activeProfile) {
            // Saved choice differs from auto-probe — re-run the full
            // profile machinery (slots will replug, ROMs reload, etc.).
            applyProfile(saved);
        } else {
            // Same profile but the user might have selected a non-default
            // CPU mode override. Apply it.
            const auto& cfg = pom2::profileConfig(activeProfile);
            const M6502::CpuMode resolved = resolveCpuMode(cfg.defaultCpu);
            auto st = controller->lockState();
            if (resolved != st.cpu().getCpuMode())
                st.cpu().setCpuMode(resolved);
        }
    }
    // Profile-specific floppy motor pitch — applies only to the 5.25"
    // bank. The 3.5" instance keeps motorPitch=1.0 because the 35_*.wav
    // samples are already recorded at the Sony 800K cadence, so a pitch
    // bump would over-shift them. applyProfile() already calls
    // setMotorPitch internally; do it here for the paths that don't go
    // through applyProfile (auto-probe matching the saved profile, or
    // no saved profile at all).
    controller->floppySound525().setMotorPitch(floppyMotorPitchForProfile(activeProfile));

    // activeProfile is now fully resolved (auto-probe + saved-profile
    // catch-up). Refresh the AI server's cached label: the wiring above set it
    // from the still-default activeProfile (AppleIIPlus) BEFORE resolution, and
    // when the saved profile matches the auto-probe the applyProfile() path
    // (which also refreshes the label) is skipped — so /status would otherwise
    // report the wrong machine (e.g. "Apple ][+" while running a //e).
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
}

// Out-of-line accessor bodies — these need EmulationController and
// Apple2Display to be complete types, which is true here but not in
// MainWindow.h (where both are forward-declared so consumers don't drag
// in the whole subsystem cone). Public API behaviour unchanged.
EmulationController& MainWindow::emul()       { return *controller; }
Apple2Display&       MainWindow::displayRef() { return *display; }

void MainWindow::setHostCapsLock(bool on)
{
    uiState_->hostCapsLock = on;
}

bool MainWindow::kioskMode() const
{
    return uiState_->kiosk;
}

bool MainWindow::settingsReadOnly() const
{
    return uiState_->kiosk || uiState_->launchedInKiosk;
}

bool MainWindow::setChatMauveInvertBit7(bool v)
{
    return devicePanelCoordinator_->setChatMauveInvertBit7(v);
}

MainWindow::~MainWindow()
{
    // Stop the AI control server BEFORE the CPU worker — pending requests
    // hold `controller->stateMutex()` and call into `controller->memory()` /
    // `controller->cpu()`; we want them quiesced before we tear anything
    // else down. The server's destructor would do the same on member
    // destruction order, but doing it here keeps the dependency obvious.
    aiServer->stop();
    controller->stop();

    // Flush every mounted medium while all cards are still alive.  The old
    // teardown only covered Disk II and 3.5-inch drives, so dirty HDV/CFFA
    // blocks vanished on quit without even attempting a host write.
    std::string shutdownFlushError;
    if (!flushSlotMedia(shutdownFlushError)) {
        pom2::log().error("Disk", "save-on-shutdown failed: " + shutdownFlushError);
    }

    // Detach every audio source BEFORE any member is destroyed.
    //
    // AudioDevice keeps raw pointers and dereferences them from the miniaudio
    // callback thread every ~5 ms, and `controller->stop()` only parks the CPU
    // worker — it never touches audio. Member destruction then runs in reverse
    // declaration order, and `controller` (which owns the AudioDevice that
    // finally drains the callback) is the FIRST member, hence the last to go:
    // everything else, `printerSound` included, dies while the callback is
    // still live. Card-owned sources were safe by accident, because
    // ~EmulationController tears down Memory (and the SlotBus) after
    // audioDev.reset(); the first MainWindow-owned source inverted that.
    unregisterAllAudioSources();

    // Free the paint/sprite editors' GPU textures while the GL context is
    // still current (same window teardown order as the About-photo texture).
    if (hgrPaintEditor)  hgrPaintEditor->releaseGL();
    if (hgrSpriteEditor) hgrSpriteEditor->releaseGL();
    if (imageWriterPanel) imageWriterPanel->shutdown();

    // Persist the complete value-only storage snapshot after the successful
    // flush. StorageCoordinator owns the per-slot/two-drive Disk II keys,
    // legacy primary aliases, CFFA state, and the session-only/synthetic HDV
    // exclusions; MainWindow no longer interprets concrete storage cards.
    {
        auto st = controller->lockState();
        const auto snapshot = storageCoordinator_->captureRebuildSnapshot(
            st.memory().slotBus());
        storageCoordinator_->persistSessionSettings(*settings, snapshot);
    }

    // Persist mounted 3.5" disks across restarts AND flush any firmware-
    // driven write-backs (format / save / etc.) that arrived after the
    // user opted in to write-back. Mirrors the Disk II save-on-shutdown
    // hook so changes survive a hard quit.
    for (pom2::Disk35Image* img :
            { &controller->disk35Internal(), &controller->disk35External() }) {
        if (img->isLoaded() && img->hasUnsavedChanges() &&
            !img->isWriteProtected()) {
            img->saveDirty();
        }
    }
    settings->setString("disk35_path_1",
        controller->disk35Internal().isLoaded()
            ? controller->disk35Internal().path() : std::string());
    settings->setString("disk35_path_2",
        controller->disk35External().isLoaded()
            ? controller->disk35External().path() : std::string());

    // Per-slot values plus the legacy primary aliases are copied from the
    // live SlotBus without retaining SSC pointers in MainWindow.
    devicePanelCoordinator_->persistSerial();

    // FujiNet relay — transport choice and its parameters, per slot.
    networkCoordinator_->persistFujiNet(*settings, *controller);

    // AI control listener — persist enable, port, token, and the panel
    // visibility flag. Re-armed on next launch by the constructor.
    settings->setBool  ("ai_control_enable", aiServer->isRunning());
    settings->setInt   ("ai_control_port",   aiServer->getPort());
    settings->setString("ai_control_token",  aiTokenInput);
    settings->setBool  ("show_ai_control",   uiState_->showAiControlPanel);

    // Persist the per-slot card mapping so changes via the Slot
    // Configuration panel survive a restart. Slots the ACTIVE profile forces
    // (//c/+ on-board SSC/Mouse/SmartPort/Disk II, and the empty virtual slots
    // on a no-physical-slots model) are NOT persisted — the effective plan
    // holds the forced built-in there; writing it would clobber the user's
    // real choice (e.g. quitting on //c would overwrite slot_4_card=mockingboard
    // with the //c's on-board "mouseaw", losing it when they go back to //e).
    // The Le Chat Mauve rear-connector adapter IS user-controllable on //c, so
    // it persists. (Mirrors the "saved key left untouched" contract in
    // plugSlotsFromSettings.)
    {
        const auto& cfg = pom2::profileConfig(activeProfile);
        for (int s = 1; s <= 7; ++s) {
            if (slotProvisioningCoordinator_->isSessionOnlySlot(s)) continue;
            // Profile-forced slots (built-ins / noPhysicalSlots) hold the
            // profile's value, not the user's — shared guard with the Slot
            // Config Apply button (pom2::slotKeyIsUserChoice).
            const std::string key = "slot_" + std::to_string(s) + "_card";
            if (!pom2::slotKeyIsUserChoice(
                    cfg, s, slotCoordinator_->effectivePlan()[s],
                    settings->getString(key, "")))
                continue;
            settings->setString(key, slotCoordinator_->effectivePlan()[s]);
        }
    }

    auto modeName = [](Apple2Display::HiResMode m) -> const char* {
        switch (m) {
            case Apple2Display::HiResMode::ColorNTSC:        return "ColorNTSC";
            case Apple2Display::HiResMode::ColorCompMedium:  return "ColorCompMedium";
            case Apple2Display::HiResMode::ColorComp4Bit:    return "ColorComp4Bit";
            case Apple2Display::HiResMode::ChatMauveRGB:     return "ChatMauveRGB";
            case Apple2Display::HiResMode::ColorCompositeOE: return "ColorCompositeOE";
            case Apple2Display::HiResMode::ColorCompositeOECpu: return "ColorCompositeOECpu";
            case Apple2Display::HiResMode::MonoWhite:        return "MonoWhite";
            case Apple2Display::HiResMode::MonoGreen:        return "MonoGreen";
            case Apple2Display::HiResMode::MonoAmber:        return "MonoAmber";
            case Apple2Display::HiResMode::ColorAppleWin:    return "ColorAppleWin";
        }
        return "ColorNTSC";
    };
    settings->setString("hi_res_mode", modeName(display->getHiResMode()));
    {
        const char* sub = "monitor";
        switch (display->getAppleWinSubMode()) {
            case Apple2Display::AppleWinSubMode::Monitor:   sub = "monitor";   break;
            case Apple2Display::AppleWinSubMode::Tv:        sub = "tv";        break;
            case Apple2Display::AppleWinSubMode::Idealized: sub = "idealized"; break;
        }
        settings->setString("applewin_submode", sub);
    }
    settings->setBool  ("show_disk_panel", uiState_->showDiskPanel);
    settings->setBool  ("show_disk35_panel", uiState_->showDisk35Panel);
    settings->setBool  ("show_disk_library", uiState_->showDiskLibrary);
    settings->setBool  ("show_hdv_panel",  uiState_->showHdvPanel);
    settings->setBool  ("show_smartport_panel", uiState_->showSmartPortPanel);
    settings->setBool  ("show_fujinet_panel",   uiState_->showFujiNetPanel);
    settings->setBool  ("show_slot_config", uiState_->showSlotConfigPanel);
    settings->setBool  ("show_media_panel", uiState_->showMediaPanel);
    settings->setBool  ("show_rom_status", uiState_->showRomStatusPanel);
    settings->setBool  ("show_abstraction", uiState_->showAbstractionPanel);
    settings->setBool  ("show_keyboard", uiState_->showKeyboardPanel);
    settings->setBool  ("show_floppy_emu", uiState_->showFloppyEmu);
    settings->setString("floppyemu_mode",
                        pom2::FloppyEmuDevice::modeKey(floppyEmu->mode()));
    settings->setString("floppyemu_sd_root", floppyEmu->sdRoot());
    settings->setBool  ("show_cassette",   uiState_->showCassetteDeck);
    settings->setBool  ("show_hgr_paint",  uiState_->showHgrPaintEditor);
    settings->setBool  ("show_hgr_sprite", uiState_->showHgrSpriteEditor);
    {
        const auto hs = hgrPaintEditor->session();
        settings->setInt   ("hgr_paint_mode",  hs.mode);
        settings->setBool  ("hgr_paint_page2", hs.page2);
        settings->setInt   ("hgr_paint_zoom",  hs.zoomIdx);
        settings->setBool  ("hgr_paint_ntsc",  hs.ntscColor);
        settings->setBool  ("hgr_paint_43",    hs.aspect43);
        settings->setInt   ("hgr_paint_pipe",  hs.canvasPipeline);
        settings->setString("hgr_paint_dir",   hs.browserDir);
    }
    settings->setBool  ("show_rewind",     uiState_->showRewindBar);
    settings->setBool  ("rewind_enabled",  controller->rewind().enabled());
    settings->setBool  ("show_joystick",   uiState_->showJoystickPanel);
    settings->setBool  ("show_mouse_inspector", uiState_->showMouseInspector);
    settings->setBool  ("show_chatmauve",  uiState_->showChatMauvePanel);
    settings->setBool  ("show_mockingboard", uiState_->showMockingboardPanel);
    settings->setBool  ("show_phasor",       uiState_->showPhasorPanel);
    settings->setBool  ("show_echoplus",     uiState_->showEchoPlusPanel);
    settings->setBool  ("show_mixer",      uiState_->showAudioMixer);
    settings->setBool  ("show_ssc",        uiState_->showSscPanel);
    settings->setBool  ("show_ethernet",  uiState_->showEthernetPanel);
    settings->setBool  ("show_printer",    uiState_->showPrinterPanel);
    settings->setBool  ("show_imagewriter", uiState_->showImageWriterPanel);
    settings->setInt   ("imagewriter_paper",
                        static_cast<int>(imageWriter->paperSize()));
    settings->setInt   ("imagewriter_dpi",    imageWriter->dpi());
    settings->setInt   ("imagewriter_model",
                        static_cast<int>(imageWriter->model()));
    settings->setBool  ("imagewriter_backpressure", uiState_->printerBackPressure);
    settings->setInt   ("imagewriter_ribbon",
                        static_cast<int>(imageWriter->ribbon()));
    settings->setInt   ("imagewriter_autolf_mode",
                        static_cast<int>(imageWriter->autoFeedMode()));
    settings->setInt   ("imagewriter_speed",
                        static_cast<int>(imageWriter->speed()));
    printerCoordinator_->persistGrappler(*settings, *controller);
    settings->setBool  ("show_nsclock",    uiState_->showNoSlotClockPanel);
    settings->setBool  ("nsclock_enable",  controller->noSlotClock().isEnabled());
    settings->setBool  ("show_ntsc",       uiState_->showNtscSettings);
    if (ntscFx) {
        const auto& p = ntscFx->getParams();
        settings->setFloat("ntsc_brightness",  p.brightness);
        settings->setFloat("ntsc_contrast",    p.contrast);
        settings->setFloat("ntsc_saturation",  p.saturation);
        settings->setFloat("ntsc_hue",         p.hue);
        settings->setFloat("ntsc_sharpness",   p.sharpness);
        settings->setFloat("ntsc_persistence", p.persistence);
        settings->setFloat("ntsc_scanlines",   p.scanlines);
        settings->setFloat("ntsc_barrel",      p.barrel);
        settings->setFloat("ntsc_shadow_strength", p.shadowMaskStrength);
        settings->setFloat("ntsc_luminance_gain", p.luminanceGain);
        settings->setFloat("ntsc_center_lighting", p.centerLighting);
        settings->setFloat("ntsc_phosphor_gamma", p.phosphorGamma);
        settings->setInt  ("ntsc_shadow_mask", static_cast<int>(p.shadowMask));
        settings->setBool ("ntsc_pal",         p.palMode);
        settings->setBool ("ntsc_text_sharp",  p.textSharp);
    }
    settings->setBool  ("crt_effects_enabled", crtEffectsEnabled);
    settings->setBool  ("show_3d_voxel", uiState_->show3dVoxel);
    settings->setBool  ("show_voxel_settings", uiState_->showVoxelSettings);
    if (voxel3d_) {
        settings->setFloat("voxel_depth",       voxel3d_->voxelDepth);
        settings->setFloat("voxel_colorshift",  voxel3d_->colorShift);
        settings->setFloat("voxel_fill",        voxel3d_->cubeFill);
        settings->setFloat("voxel_ambient",     voxel3d_->ambient);
        settings->setInt  ("voxel_supersample", voxel3d_->superSample);
        settings->setBool ("voxel_mono",           voxel3d_->mono);
        settings->setBool ("voxel_percolor_depth", voxel3d_->perColorDepth);
    }
    settings->setString("aspect_mode",
        aspectMode == AspectMode::Crt43   ? "crt43" :
        aspectMode == AspectMode::Integer ? "integer" : "square");
    settings->setString("ui_accent", pom2::accentKey(uiAccent_));
    settings->setFloat ("ui_scale",  uiScale_);
    settings->setBool  ("ui_dock_seeded", dockSeeded_);
    settings->setString("library_favourites", joinPaths(libraryFavourites_));
    settings->setString("library_recents",    joinPaths(libraryRecents_));
    settings->setBool  ("library_hide_sizedate", libraryHideSizeDate_);
    settings->setBool  ("disk_turbo",      diskTurboWhileMotor);
    audioCoordinator_->persist(*settings,
                               controller->speaker(),
                               controller->cassette(),
                               controller->floppySound525(),
                               controller->floppySound35(),
                               *printerSound);
    settings->setString("char_rom_locale",        pom2::charRomLocaleKey(charRomLocale));
    // Kiosk is a read-only launcher: don't write state.cfg, so the disk it
    // booted (and any storage card provisioned for that session)
    // never leak into the user's saved GUI config. The setString calls
    // above are in-memory only and discarded with `settings` here.
    // Record where the window ended up so the next launch (and any later
    // kiosk round-trip) reopens at the same size and place. Skipped while
    // in kiosk — the live geometry is full-screen, not what we want to
    // restore; the value captured on the way INTO kiosk still stands.
    // NB the geometry itself was captured by main() via
    // captureWindowGeometryNow() while GLFW was still up — measuring here
    // would be too late (see that function).
    if (!settingsReadOnly()) {
        saveWindowGeometryToSettings();
        settings->save();
    }

    // Runtime ownership stays above FujiNetCard. Stop the injected link first
    // so its peer closes cleanly, then reap the helper process; persistence
    // above intentionally sampled the pre-shutdown running state.
    networkCoordinator_->shutdownFujiNet(*controller);

    if (uiState_->aboutImageTexture) {
        GLuint t = uiState_->aboutImageTexture;
        glDeleteTextures(1, &t);
        uiState_->aboutImageTexture = 0;
    }
    if (uiState_->keyboardImageTexture) {
        GLuint t = uiState_->keyboardImageTexture;
        glDeleteTextures(1, &t);
        uiState_->keyboardImageTexture = 0;
    }
}

// ─── Slot configuration ─────────────────────────────────────────────────
//
// `plugSlotsFromSettings()` is the single source of truth for which card
// is in which slot. It reads `slot_1_card`..`slot_7_card` from the runtime
// settings store, falling back to the historical defaults below when a
// slot key is absent (so first-run users see no regression). Each card is
// constructed with its slot number passed to the constructor — the slot
// is baked into card slot ROMs (PR#n entry points, ProDOS unit numbers,
// etc.) so we can't just plug a "slot-2-style" SSC into slot 5 and expect
// PR#5 to find it.
//
// Validation: each card-type identifier appears in at most one slot. A
// duplicate request logs a warning and skips the second instance. Empty
// slots are simply not plugged.
//
// Identifiers (canonical strings stored in settings):
//   ""           empty slot
//   "diskii"     DiskIICard
//   "hdv"        ProDOSHardDiskCard
//   "ssc"        SuperSerialCard
//   "clock"      ClockCard
//   "chatmauve"  LeChatMauveCard
//   "mouse"      MouseCard (firmware-backed implementation)
//   "mockingboard"  MockingboardCard (Sweet Microsystems A/C — 6522×2 + AY×2)

// ─── Audio-source inventory ──────────────────────────────────────────────
//
// See MainWindow.h: AudioDevice holds raw pointers that the miniaudio
// callback thread dereferences, so every source registered here must be
// unregistered before the SlotBus destroys the card that owns it. Going
// through this pair (rather than the named `*Card` aliases) is what makes
// that hold for multi-instance configurations — two Mockingboard variants,
// a future second Phasor — without each new card type having to remember
// to add a line to two teardown blocks.

void MainWindow::registerAudioSource(AudioSource* src)
{
    audioCoordinator_->registerSource(src);
}

void MainWindow::unregisterAllAudioSources()
{
    audioCoordinator_->unregisterAll();
}

void MainWindow::restoreSlotMediaFromSettings(const pom2::StateAccess& st)
{
    const auto result = storageCoordinator_->restoreMediaFromSettings(
        st.memory().slotBus(), *settings);
    for (const auto& warning : result.warnings)
        pom2::log().warn("Storage", warning);
}

void MainWindow::plugSlotsFromSettings(const pom2::StateAccess& st)
{
    const auto& slotPlan = slotCoordinator_->resolve(*settings, activeProfile);

    // ── Per-card construction helpers. SlotBus remains the sole owner and
    //    source of truth; MainWindow does not cache aliases to these cards. ──

    auto createResourceCard = [&](std::string key, int slot) {
        pom2::SlotCardFactory::Request request;
        request.key = std::move(key);
        request.slot = slot;
        request.cpuIsCmos =
            st.cpu().getCpuMode() == M6502::CpuMode::CMOS;
        request.profile = activeProfile;
        auto result = slotCardFactory_->create(request);
        if (!result.warning.empty()) {
            const char* tag = result.warningCategory.empty()
                ? "Slots" : result.warningCategory.c_str();
            pom2::log().warn(tag, result.warning);
        }
        return result;
    };

    auto plugDiskII = [&](int s) {
        auto made = createResourceCard("diskii", s);
        auto* card = dynamic_cast<DiskIICard*>(made.card.get());
        if (!card) return;
        if (!made.resourcePath.empty()) diskRomPath = made.resourcePath;
        diskRomStatus = made.status;
        // Wire the CPU pointer for sub-instruction cycle accuracy on
        // MMIO reads/writes (cycle-precise copy protections rely on the
        // LSS state at the exact sub-cycle of the data fetch, not at
        // instruction-start). See DiskIICard::setCpu doc for context.
        card->setCpu(&st.cpu());
        card->setFloppySound(&controller->floppySound525());
        // //c+ on-board IWM — only the slot-6 card pushes its drive
        // pointer to the IWM, mirroring the //c+ wiring. Multi-instance
        // Disk II (option C) lets the user plug a second Disk II in
        // slot 4 etc.; that secondary card stays off the IWM path to
        // avoid clobbering the //c+ flux mirror.
        if (s == 6) card->setIWM(&controller->iwm());
        diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
        if (!diskPanel) diskPanel = diskPanels.front().get();
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugHdv = [&](int s) {
        auto made = createResourceCard("hdv", s);
        if (!dynamic_cast<ProDOSHardDiskCard*>(made.card.get())) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugCffa = [&](int s) {
        auto made = createResourceCard("cffa", s);
        if (!dynamic_cast<pom2::CffaCard*>(made.card.get())) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugChatMauve = [&](int s) {
        auto card = std::make_unique<LeChatMauveCard>(s);
        auto* chatMauve = card.get();
        if (settings) {
            chatMauve->setInvertBit7(
                settings->getBool("chatmauve_invert_bit7", false));
            chatMauve->setColorTextEnabled(
                settings->getBool("chatmauve_color_text", true));
            chatMauve->setHgrDuochromeEnabled(
                settings->getBool("chatmauve_hgr_duochrome", false));
        }
        st.memory().slotBus().plug(s, std::move(card));
        display->setChatMauveCard(chatMauve);
    };

    bool haveConfiguredSsc = false;
    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        SuperSerialCard* raw = card.get();
        raw->setTransport(pom2::makeSuperSerialTcpTransport(*raw));
        // Use pasteText (not queueKey) — pasteText respects the paste
        // queue, so a stream of bytes from telnet doesn't clobber earlier
        // characters that BASIC hasn't picked up yet.
        raw->setKeyboardSink(
            [&mem = st.memory()](uint8_t b) {
                const char buf[1] = { static_cast<char>(b) };
                mem.pasteText(buf, 1);
            });
        // IRQ routing is auto-wired by SlotBus's installed router (see
        // Memory::setCpu) — no per-card setup needed.
        st.memory().slotBus().plug(s, std::move(card));
        // Per-slot persistence; fall back to legacy global keys (the
        // primary SSC was the only one before //c dual-port support).
        const std::string sk = "_slot" + std::to_string(s);
        const bool legacyPrimary = !haveConfiguredSsc;
        haveConfiguredSsc = true;
        raw->setRawMode(settings->getBool(
            "ssc_raw_mode" + sk,
            legacyPrimary ? settings->getBool("ssc_raw_mode", false) : false));
        // Printer tap: slot 1 is the printer-port convention (the //c
        // hard-wires it), so the tap defaults ON there — a //c user gets
        // PR#1 landing on the ImageWriter with zero configuration.
        raw->setPrinterTap(settings->getBool("ssc_printer_tap" + sk, s == 1));
        const bool listenDefault = legacyPrimary
            ? settings->getBool("ssc_listening", false) : false;
        if (settings->getBool("ssc_listening" + sk, listenDefault)) {
            const int portDefault = legacyPrimary
                ? settings->getInt("ssc_port", SuperSerialCard::kDefaultPort)
                : SuperSerialCard::kDefaultPort;
            const int p = settings->getInt("ssc_port" + sk, portDefault);
            raw->startListening(static_cast<uint16_t>(p));
        }
    };

    auto plugClock = [&](int s) {
        auto card = std::make_unique<ClockCard>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugSoftCard = [&](int s) {
        // Microsoft SoftCard (Z80 DMA bus master). No ROM to probe — the
        // hardware has none. The card needs the real bus (soft-switch
        // side effects, LC paging) and the 6502 so its $CnXX toggle can
        // halt the in-flight run() chunk at an instruction boundary.
        auto card = std::make_unique<SoftCardZ80>();
        card->setMemory(&st.memory());
        card->setCpu(&st.cpu());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugPrinter = [&](int s) {
        auto card = std::make_unique<PrinterCard>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugUthernet = [&](int s) {
        // a2RetroSystems Uthernet I — CS8900A NIC, raw Ethernet only.
        // Without a working transport the card still plugs and probes
        // (drivers detect it via the PacketPage ProductID), it just never
        // sees a frame — which is a better failure mode than hiding it.
        auto card = std::make_unique<pom2::UthernetCard>(s);
        card->setBackend(
            networkCoordinator_->makeEthernetBackend(*settings, "Uthernet"));
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugUthernetII = [&](int s) {
        // a2RetroSystems Uthernet II — W5100 hardware TCP/IP. Its TCP and
        // UDP sockets are supplied by the runtime adapter; the optional
        // Ethernet backend only serves MACRAW/IPRAW.
        auto card = std::make_unique<pom2::UthernetIICard>(s);
        card->setSocketFactory(networkCoordinator_->makeW5100SocketFactory());
        card->setBackend(
            networkCoordinator_->makeEthernetBackend(*settings, "UthernetII"));
        // Virtual DNS is an emulator extension (not on real silicon) that
        // lets a guest connect by hostname without carrying a resolver.
        // On by default, matching AppleWin, and detectable by software as
        // PTIMER == 0.
        card->chip().setVirtualDnsEnabled(
            settings->getBool("uthernet2_virtual_dns", true));
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugFujiNet = [&](int s) {
        // FujiNet relay. The card itself is inert until the link finds a
        // peer, and finding one is asynchronous, so plugging always succeeds
        // — a machine with this card and no FujiNet running behaves like a
        // machine with an empty drive, not a broken one.
        auto card = std::make_unique<pom2::FujiNetCard>(s);
        auto hostLink = std::make_unique<pom2::SpOverSlipLink>();
        card->setMemory(&st.memory());
        card->setCpu(&st.cpu());

        auto& link = *hostLink;
        networkCoordinator_->restoreFujiNet(link, *settings, s);

        card->setLink(std::move(hostLink));
        st.memory().slotBus().plug(s, std::move(card));

    };

    auto plugPhasor = [&](int s) {
        // Applied Engineering Phasor. Same MMIO surface as a Mockingboard
        // plus a mode soft-switch at $C0(8+s)X that flips between MB-
        // compat (1 AY per VIA) and Phasor-native (2 AYs per VIA × 2 VIAs
        // = 4 chips, 12 voices). Audio synth is a v1 placeholder — the
        // card detects + responds to MMIO correctly but emits silence
        // until the 4-AY mix lands (TODO 🟡 [Phasor] audio synth).
        auto card = std::make_unique<PhasorCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&st.cpu());
        const auto mix = audioCoordinator_->restoreCardSettings(
            *settings, pom2::AudioCoordinator::CardKind::Phasor, s, 0.5f);
        card->setVolume(mix.volume);
        card->setMuted(mix.muted);
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlus = [&](int s) {
        // Street Electronics Echo+ — standalone SSI263 speech synth.
        // No PROM, no ROM dependency, audio is silent in v1 (chip
        // model complete but phoneme PCM blob deferred to a separate
        // commit pending license review of AppleWin's data).
        auto card = std::make_unique<EchoPlusCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&st.cpu());
        const auto mix = audioCoordinator_->restoreCardSettings(
            *settings, pom2::AudioCoordinator::CardKind::EchoPlus, s, 0.7f);
        card->setVolume(mix.volume);
        card->setMuted(mix.muted);
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlusTms = [&](int s) {
        // Street Electronics Echo+ AS ACTUALLY SHIPPED — 2×AY-3-8913 +
        // TMS5220. Scaffold only: register decode is present so software
        // detects the card, but the LPC core + AY synth are deferred.
        // Audio is silent. See EchoPlusTMS5220Card.h for the chipset
        // sourcing notes.
        auto card = std::make_unique<EchoPlusTMS5220Card>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugGrappler = [&](int s) {
        auto made = createResourceCard("grappler", s);
        auto* card = dynamic_cast<GrapplerCard*>(made.card.get());
        if (!card) return;
        // S1 printer-type DIP. Default = Apple Dot Matrix / ImageWriter:
        // POM2's printer IS an ImageWriter II, and MAME's Epson default
        // makes the firmware emit Epson escape codes that this printer
        // renders as garbage (same as flipping the switches wrong on a
        // real desk).
        card->setPrinterType(static_cast<GrapplerCard::PrinterType>(
            settings->getInt("grappler_printer_type",
                static_cast<int>(GrapplerCard::PrinterType::AppleDotMatrix))));
        card->setMsbSoftwareControl(
            settings->getBool("grappler_msb_software", true));
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugMockingboard = [&](int s, MockingboardCard::Variant variant) {
        // Mockingboard A/C — 6522×2 + AY-3-8910×2. No ROM dependency, no
        // image to mount: software detects it by writing to the VIA at
        // $C(s)00 and observing the read-back. We always-plug when
        // requested. The inner AudioSource is registered with the audio
        // device so synthesised samples mix with the speaker output, and
        // the CPU IRQ line is wired so VIA T1 can drive the music
        // driver's tick.
        //
        // Variant::SoundII additionally adds an SSI263 speech synth at
        // $C(s)40-$C(s)44 with A/!R wired to VIA1.CA1 → IFR.CA1 →
        // (gated by IER.CA1) slot IRQ. Drivers configure PCR.0=0 for
        // negative-edge detection on the inverted A/!R wiring.
        auto card = std::make_unique<MockingboardCard>(s, variant);
        card->setSampleRate(controller->audio().getActualSampleRate());
        // Emulated CPU clock for the audio thread's emuCycles replay
        // cursor. Set HERE too, not only from setVideoStandard: a Slot
        // Config "Apply" re-plugs cards without re-running the profile's
        // video-standard step, so a Mockingboard added on a PAL profile
        // would otherwise keep the NTSC default and collapse every queued
        // AY write to the buffer edge.
        card->setCpuClock(static_cast<double>(
            pom2VideoTiming(controller->getVideoStandard()).cpuClockHz));
        // CPU pointer feeds the lazy-sync timer back-channel
        // (getCycleCountNow); IRQ routing is auto-wired via SlotBus.
        card->setCpu(&st.cpu());
        // Default volume is conservative — the card's three-channel mix
        // can dwarf the speaker at peak; the user can crank via the
        // Mockingboard panel (TODO).
        const auto mix = audioCoordinator_->restoreCardSettings(
            *settings, pom2::AudioCoordinator::CardKind::Mockingboard, s, 0.5f);
        card->setVolume(mix.volume);
        card->setMuted(mix.muted);
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugSmartPort35 = [&](int s) {
        // Construct empty hardware. StorageCoordinator restores unit types,
        // images and write-back only after the complete topology exists.
        auto made = createResourceCard("smartport35", s);
        auto* card = dynamic_cast<pom2::SmartPortCard*>(made.card.get());
        if (!card) return;
        // Mechanical sound: route to the dedicated 3.5" sound bank.
        // Block-level transfers only — the card synthesises step / motor
        // / click events from READBLOCK / WRITEBLOCK directly.
        card->setFloppySound(&controller->floppySound35());
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugMouse = [&](int s) {
        auto made = createResourceCard("mouse", s);
        mouseRomStatus = made.status;
        if (!made) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    // Dispatch: walk slots 1..7 and plug whichever card the settings ask
    // for. Anything we don't recognise is logged and skipped.
    for (int s = 1; s <= 7; ++s) {
        const std::string& kind = slotPlan[s];
        if      (kind.empty())          continue;
        else if (kind == "diskii")      plugDiskII(s);
        else if (kind == "hdv")         plugHdv(s);
        else if (kind == "cffa")        plugCffa(s);
        else if (kind == "ssc")         plugSsc(s);
        else if (kind == "printer")     plugPrinter(s);
        else if (kind == "clock")       plugClock(s);
        else if (kind == "softcard")    plugSoftCard(s);
        else if (kind == "chatmauve")   plugChatMauve(s);
        else if (kind == "mouse")       plugMouse(s);
        else if (kind == "mouseaw")     {
            auto made = createResourceCard("mouseaw", s);
            mouseRomStatus = made.status;
            if (made) st.memory().slotBus().plug(s, std::move(made.card));
        }
        else if (kind == "mockingboard")   plugMockingboard(s, MockingboardCard::Variant::AC);
        else if (kind == "mockingboard_c") plugMockingboard(s, MockingboardCard::Variant::SoundII);
        else if (kind == "phasor")      plugPhasor(s);
        else if (kind == "echoplus")    plugEchoPlus(s);
        else if (kind == "echoplus_tms") plugEchoPlusTms(s);
        else if (kind == "grappler")    plugGrappler(s);
        else if (kind == "uthernet")    plugUthernet(s);
        else if (kind == "uthernet2")   plugUthernetII(s);
        else if (kind == "smartport35") plugSmartPort35(s);
        else if (kind == "fujinet")     plugFujiNet(s);
        else {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " has unknown card type '" +
                kind + "' — leaving empty");
        }
    }

    // The printer's mechanical sound is not owned by any card, but it IS
    // swept away with the card-owned ones: both rebuild paths call
    // unregisterAllAudioSources() before getting here. Re-registering it on
    // every rebuild is what keeps it alive across a profile switch and a Slot
    // Config "Apply" — registerAudioSource() is idempotent, so the
    // constructor's first pass through here simply arms it.
    registerAudioSource(printerSound.get());

    // Re-apply a `--fujinet` card requested on the command line.
    //
    // It is deliberately not in the settings file (a one-shot CLI card must
    // not leak into the user's saved slot config), so the re-seed at the top
    // of this function has just erased it. Doing it here means applyProfile's
    // step 7 reproduces the card, which matters twice: `--preset` no longer
    // destroys it moments after the CLI logged success, and because step 7
    // runs BEFORE step 11's cold boot, the autostart scan still finds a
    // FujiNet on its first pass.
    if (cliFujiNetSlot_ > 0 &&
        !pom2::profileConfig(activeProfile).noPhysicalSlots) {
        const int s = cliFujiNetSlot_;
        if (st.memory().slotBus().peripheral(s) != nullptr) {
            pom2::log().warn("CLI", "--fujinet: slot " + std::to_string(s) +
                                        " is taken after the rebuild — card "
                                        "not restored");
        } else {
            std::string err;
            if (!plugFujiNetUnlocked(st, s, cliFujiNetSerial_,
                                     cliFujiNetSerialPath_, cliFujiNetPort_,
                                     err))
                pom2::log().warn("CLI", "--fujinet: " + err);
        }
    }
}

// UI surfaces live in the bounded MainWindow_*.cpp translation units listed in
// docs/ARCHITECTURE.md. This file remains the composition/lifecycle root and
// deliberately keeps only the frame-level orchestration below.

void MainWindow::render()
{
    // Track wallclock between frames so the deck counter / armed pulse /
    // status overlay can age correctly.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const float deltaSeconds = static_cast<float>(std::max(0.0, now - uiState_->lastFrameTime));
    uiState_->lastFrameTime = now;

    // Screen-widget hover is re-established by renderScreenWindow() further
    // down, if it draws at all. Clearing it here is what makes "the screen
    // window is collapsed / hidden / not reached this frame" mean "the
    // pointer is not the guest's" — a latched `true` would keep feeding the
    // Mouse Card from a widget that is no longer on screen.
    uiState_->screenHovered = false;

    pollJoystickAndPushToMemory();

    // A pointer capture only makes sense while a Mouse Card is on the bus.
    // Every path that can take one away is observed through the live SlotBus,
    // so releasing here covers all of them at one point instead of chasing
    // each call site. Kiosk deliberately keeps the grab (it is the mode
    // most likely to want it), which is why this sits above the kiosk
    // early-out below.
    if (uiState_->mouseGrabbed && !mouseCoordinator_->capture().plugged())
        setMouseGrab(false);

    // Decide CPU turbo from disk activity every frame, independent of whether
    // any disk panel window is open (the disk panel defaults to hidden).
    updateAutoTurbo();

    // Kiosk: only the screen, no chrome. Joystick + auto-turbo above still
    // run so the machine behaves identically; everything else is skipped.
    // F6 hold-to-rewind still works (no toolbar button in kiosk).
    if (uiState_->kiosk) {
        // F6 is inert while the menu has the machine parked: releaseHold →
        // rewindEndAndResume would setMode(Running) behind the overlay.
        driveRewindHold(!uiState_->kioskMenuOpen && ImGui::IsKeyDown(ImGuiKey_F6));
        updateKioskMenu();         // Start/Select drive the in-game menu
        renderKiosk();
        renderKioskMenu();         // overlay drawn on top of the screen
        // The printer still runs behind the chrome-free screen — without
        // this a //c printing in kiosk mode parked every byte in the card
        // spool forever (unbounded growth, nothing on paper).
        pumpImageWriter();
        return;
    }

    renderMenuBar();
    // Toolbar must render after the menu bar so we know its height
    // (`ImGui::GetFrameHeight()` reflects the menu bar font size +
    // padding). It's positioned just below — pinned, can't be moved
    // or resized.
    {
        pom2::Toolbar_ImGui::Snapshot tb;
        const auto mode = controller->getMode();
        tb.isRunning          = (mode == EmulationController::Mode::Running);
        tb.isStopped          = (mode == EmulationController::Mode::Stopped);
        tb.cyclesPerFrame     = controller->getCyclesPerFrame();
        tb.videoStandard      = controller->getVideoStandard();
        tb.memoryGridVisible  = uiState_->showMemoryGrid;
        tb.activeProfile      = activeProfile;
        tb.hasPrimaryDiskCard =
            storageCoordinator_->captureInventory(*controller).hasDiskII();
        tb.charRomLocale      = charRomLocale;
        auto isMonoHiRes = [](Apple2Display::HiResMode m) {
            return m == Apple2Display::HiResMode::MonoWhite ||
                   m == Apple2Display::HiResMode::MonoGreen ||
                   m == Apple2Display::HiResMode::MonoAmber;
        };
        tb.displayIsMono      = isMonoHiRes(display->getHiResMode());
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            tb.rewindEnabled   = controller->rewind().enabled();
            tb.rewindHasFrames = !controller->rewind().empty();
        }

        const auto tr = toolbar->render(ImGui::GetFrameHeight(), tb);
#ifdef __EMSCRIPTEN__
        auto browserResetBoot = [&]() -> bool {
            if (browserResetBootImage_.empty()) return false;
            std::string err;
            if (insertAndBootImage(browserResetBootImage_, err)) {
                uiState_->tapeStatusMessage = "Boot: " + browserResetBootImage_;
                uiState_->tapeStatusUntil = uiState_->lastFrameTime + 3.0;
            } else {
                uiState_->tapeStatusMessage = "Boot failed: " + err;
                uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
            }
            return true;
        };
#endif
        if (tr.requestColdBoot) {
#ifdef __EMSCRIPTEN__
            if (!browserResetBoot())
#endif
            controller->coldBoot();
        }
        if (tr.requestSoftReset) {
#ifdef __EMSCRIPTEN__
            if (!browserResetBoot())
#endif
            controller->softReset();
        }
        if (tr.requestHardReset)       controller->hardReset();
        if (tr.requestPauseToggle) {
            controller->setMode(tb.isRunning
                ? EmulationController::Mode::Stopped
                : EmulationController::Mode::Running);
        }
        if (tr.requestStep)            controller->requestStep();
        if (tr.requestScreenshot)      saveScreenshot();
        if (tr.setCyclesPerFrame > 0)
            controller->setCyclesPerFrame(tr.setCyclesPerFrame);
        if (tr.setProfileRequested)    applyProfile(tr.setProfile);
        if (tr.requestMemoryGridToggle) uiState_->showMemoryGrid = !uiState_->showMemoryGrid;
        // Same entry point as F10 and the View menu item, so the three
        // routes into kiosk cannot drift apart.
        if (tr.requestKioskToggle)      toggleKioskMode();
        if (tr.requestAbout)            uiState_->showAbout = true;
        if (tr.requestMonoColorToggle) {
            // Flip color ↔ monochrome, remembering each side's submode so a
            // round-trip restores the user's exact choice. Persisted via the
            // dtor's hi_res_mode write, like the View menu picks.
            const auto curHi = display->getHiResMode();
            if (isMonoHiRes(curHi)) {
                lastMonoHiResMode_ = curHi;
                display->setHiResMode(lastColorHiResMode_);
            } else {
                lastColorHiResMode_ = curHi;
                display->setHiResMode(lastMonoHiResMode_);
            }
        }
        if (tr.requestInsertDisk && diskPanel) {
            // Reuse the existing per-panel popup machinery: setting the
            // primary panel's `insertDialogOpen` flag is exactly what
            // its own "Insert .dsk…" button does. `renderDiskFileDialog`
            // picks it up next frame and resolves the primary Disk II slot.
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (tr.setCharRomRequested) {
            // Hot swap: Memory::loadCharRom rewrites the csbits table
            // in place; Apple2Display re-reads `mem.charRom()` on every
            // frame so the new glyphs show up at the next render. No
            // cold reset needed.
            charRomLocale = tr.setCharRomLocale;
            std::string newPath;
            if (charRomLocale == pom2::CharRomLocale::ProfileDefault) {
                // Replay the active profile's probe order (which
                // already lists path candidates resolvable from both
                // repo root and build/, via the SystemProfile config).
                const auto& cfg = pom2::profileConfig(activeProfile);
                for (const auto& p : cfg.charRomProbeOrder) {
                    const std::string r = pom2::resolveCharRomPath(p);
                    if (!r.empty()) { newPath = r; break; }
                }
            } else {
                newPath = pom2::resolveCharRomPath(charRomLocale);
            }
            namespace fs = std::filesystem;
            if (!newPath.empty() && fs::exists(newPath)) {
                auto st = controller->lockState();
                if (st.memory().loadCharRom(
                        newPath.c_str(), pom2::charRomBank(charRomLocale))) {
                    charRomPath = newPath;
                    settings->setString("char_rom_locale",
                        pom2::charRomLocaleKey(charRomLocale));
                    settings->save();
                    pom2::log().info("CharRom",
                        std::string("Switched to ") + newPath);
                } else {
                    pom2::log().warn("CharRom",
                        std::string("loadCharRom failed for ") + newPath);
                }
            } else {
                pom2::log().warn("CharRom",
                    std::string("Selected ROM missing: ") +
                    (newPath.empty() ? "(no path)" : newPath));
            }
        }

        // Hold-to-rewind from either input source: F6 (works everywhere) or
        // the toolbar's rewind button (held this frame). One edge-tracker.
        driveRewindHold(ImGui::IsKeyDown(ImGuiKey_F6) || tr.requestRewindHeld);
    }
    // After the menu bar + toolbar (both reserve viewport work area), before
    // any dockable window: the DockSpace has to exist when the panels below
    // call Begin(), or their first frame renders undocked.
    renderDockSpace();
    renderScreenWindow();
    renderMemoryViewerWindow();
    if (uiState_->showMemoryBar)  renderMemoryBarWindow();
    if (uiState_->showMemoryBarH) renderMemoryBarHorizontalWindow();
    if (uiState_->showMemoryGrid) renderMemoryGridWindow();
    renderCassetteDeckWindow(deltaSeconds);
    renderHgrPaintWindow();
    renderHgrSpriteWindow();
    if (uiState_->showRewindBar) renderRewindWindow(deltaSeconds);
    renderTapeFileDialogs();
    renderPasteFileDialog();
    renderHdvFileDialog();
    renderDiskPanelWindow();
    renderDiskFileDialog();
    renderDiskLibraryWindow();
    renderDisk35PanelWindow();
    renderDisk35FileDialog();
    renderHdvPanelWindow();
    renderSmartPortPanelWindow();
    renderFujiNetPanelWindow();
    renderChatMauvePanelWindow();
    renderMockingboardPanelWindow();
    renderPhasorPanelWindow();
    renderEchoPlusPanelWindow();
    renderSscPanelWindow();
    renderEthernetPanelWindow();
    renderPrinterPanelWindow();
    pumpImageWriter();
    renderImageWriterWindow();
    renderNoSlotClockPanelWindow();
    renderJoystickPanelWindow();
    renderMouseInspectorWindow();
    renderAudioMixerWindow();
    renderNtscSettingsWindow();
    renderVoxelSettingsWindow();
    renderAiControlPanelWindow();
    renderSlotConfigPanel();
    renderMediaPanel();
    if (uiState_->showRomStatusPanel) {
        if (!romStatusPanel)
            romStatusPanel = std::make_unique<pom2::RomStatus_ImGui>();
        romStatusPanel->render(
            &uiState_->showRomStatusPanel,
            std::string(pom2::profileConfig(activeProfile).displayName));
    }
    renderAbstractionPanel();
    renderKeyboardPanel();
    renderFloppyEmuWindow();
    renderAboutDialog();
    renderWelcomePanelWindow();
    renderStatusBar();
    // Last: the palette is an overlay and must draw above every panel.
    renderCommandPalette();

    // Hide the host OS cursor whenever the AppleWin HLE firmware is
    // driving a visible emulated cursor AND the host pointer is over the
    // Apple II Screen widget — the two cursors are otherwise stacked and
    // distracting. The ImGui-Glfw backend honours
    // ImGuiMouseCursor_None at EndFrame by calling
    // glfwSetInputMode(GLFW_CURSOR_HIDDEN); on the next frame, leaving
    // it Normal (default ImGuiMouseCursor_Arrow) brings the OS cursor
    // back. The screen rect is fresh because `renderScreenWindow()` ran
    // earlier this frame.
    {
        const auto mouse = mouseCoordinator_->capture();
        const bool mouseOn = mouse.appleWinActive() && mouse.appleWin.mouseOn();
        const float w = uiState_->screenRectMax.x - uiState_->screenRectMin.x;
        const float h = uiState_->screenRectMax.y - uiState_->screenRectMin.y;
        const bool insideWidget =
            w > 0.0f && h > 0.0f &&
            uiState_->lastMouseHostX >= double(uiState_->screenRectMin.x) &&
            uiState_->lastMouseHostX <= double(uiState_->screenRectMax.x) &&
            uiState_->lastMouseHostY >= double(uiState_->screenRectMin.y) &&
            uiState_->lastMouseHostY <= double(uiState_->screenRectMax.y);
        if (mouseOn && insideWidget) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
    }
}
