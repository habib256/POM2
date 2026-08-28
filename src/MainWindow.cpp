// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "MainWindow.h"

#include "MediaMount.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
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
#include "W5100NameResolver.h"
#include "FujiNetCard.h"
#include "FujiNetCardFactory.h"
#include "SerialPort.h"
#include "SpTransport.h"
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
#include "Debugger_ImGui.h"
#include "MemoryViewer_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCoordinator.h"
#include "NetworkCoordinator.h"
#include "AudioCoordinator.h"
#include "DevicePanelCoordinator.h"
#include "SlotCardFactory.h"
#include "DebugCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotProvisioningCoordinator.h"
#include "SlotRebuildCoordinator.h"
#include "StorageCoordinator.h"
#include "PrinterCoordinator.h"
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
#include "SuperSerialTransport.h"
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

// GL types/symbols are needed by uploadScreenTexture (further down) AND by
// the About-dialog photo loader called from this same translation unit.
// Pulling the platform-correct header once at the top keeps both sites
// working without duplicating the #if/#else block.
// stb_image is bundled (single-header public-domain JPEG/PNG decoder)
// solely for the About-dialog Apple ][+ photo. The implementation macro
// is defined *here* so symbols land in MainWindow.cpp.o and nowhere else.
// STB_IMAGE_STATIC keeps the unused entry points internal; we suppress
// the resulting -Wunused-function noise locally rather than tagging the
// third-party header.
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

namespace {
// Sentinel prefix used in HdvController_ImGui::LibraryEntry::fullPath to
// flag the synthetic prodos_folder/ host-folder mount. The dispatcher in
// renderHdvPanelWindow detects this prefix and routes to the synthesiser
// instead of treating the path as a real .hdv file.
constexpr const char* kProDOSHostSentinel = "@PRODOS_HOST_FOLDER@:";
} // namespace

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
      debugCoordinator_(std::make_unique<pom2::DebugCoordinator>(*controller)),
      debuggerPanel  (std::make_unique<pom2::Debugger_ImGui>()),
      settings       (std::make_unique<pom2::Settings>()),
      cassetteDeck   (std::make_unique<pom2::CassetteDeck_ImGui>()),
      rewindPanel_   (std::make_unique<pom2::Rewind_ImGui>()),
      mouseCoordinator_(std::make_unique<pom2::MouseCoordinator>(*controller)),
      networkCoordinator_(std::make_unique<pom2::NetworkCoordinator>()),
      printerCoordinator_(std::make_unique<pom2::PrinterCoordinator>()),
      audioCoordinator_(std::make_unique<pom2::AudioCoordinator>(
          controller->audio(), *controller)),
      devicePanelCoordinator_(std::make_unique<pom2::DevicePanelCoordinator>(
          *controller, *settings)),
      storageCoordinator_(std::make_unique<pom2::StorageCoordinator>()),
      slotCardFactory_(std::make_unique<pom2::SlotCardFactory>()),
      slotConfigCoordinator_(
          std::make_unique<pom2::SlotConfigurationCoordinator>()),
      slotProvisioningCoordinator_(
          std::make_unique<pom2::SlotProvisioningCoordinator>(
              *slotCardFactory_, *storageCoordinator_)),
      // The rebuild transaction. Every hook is required — the coordinator
      // throws rather than let a half-wired teardown run — and the ORDER is
      // its contract, not this list's: gate AI requests, drop the non-owning
      // views (audio sources, panels, the printer feed identity), clear the
      // bus, then the host-side services that no longer have a card.
      slotRebuildCoordinator_(std::make_unique<pom2::SlotRebuildCoordinator>(
          pom2::SlotRebuildCoordinator::Hooks{
              // Only reached after a successful flush, which is the point:
              // history that indexes a topology must not outlive it, and
              // session-only provisioning must not be persisted from it.
              [this] {
                  controller->rewind().clear();
                  storageCoordinator_->clearAutoProvisioned();
              },
              [this] { aiServer->detach(); },
              // Drive teardown off the registration inventory, never the card
              // aliases: two coexisting Mockingboard variants left the first
              // card's AudioSource registered against freed memory.
              [this] { unregisterAllAudioSources(); },
              [this] {
                  diskPanels.clear();
                  diskPanel = nullptr;
                  // These are read by panels every frame. The FujiNet card
                  // also owns a listening socket and a worker thread that
                  // SlotBus::clear() joins, so the alias has to go before the
                  // clear, not after it.
              },
              [this] { printerCoordinator_->resetFeedCursor(); },
              // Nothing host-side outlives the cards today; the hook exists
              // so a helper process gets torn down here rather than somewhere
              // that runs before SlotBus::clear().
              [] {},
              [this] { display->setChatMauveCard(nullptr); },
              [this] {
                  aiServer->attach(controller.get(), display.get(),
                                   primaryDiskII(), primaryHdvCard());
              },
          })),
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
      aiPortInput    (pom2::AiControlServer::kDefaultPort),
      charRomLocale  (pom2::CharRomLocale::ProfileDefault),
      activeProfile  (pom2::SystemProfile::AppleIIPlus)
{
    // Memory viewer writes go through Memory::memWrite under stateMutex,
    // so a byte poked from the UI passes through ROM-write protection and
    // any future I/O hooks just like a CPU store would.
    // Bind every panel in the catalog to its flag BEFORE anything reads or
    // writes panel state: the menus, the palette, the settings round-trip and
    // the browser build's chrome-light startup are all derived from those
    // bindings, and an unbound panel is a menu row that toggles nothing.
    registerPanels();

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
        show(pom2::PanelId::Welcome) = true;
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
    {
        // Restore persisted volume/mute per channel. The 3.5" channel
        // inherits the 5.25" defaults on first run so users who had
        // already tuned floppy_sound_volume don't get a louder/quieter
        // 3.5" surprise.
        const float vol525 = settings->getFloat("floppy_sound_volume", 0.6f);
        const bool  mute525 = settings->getBool ("floppy_sound_muted",  false);
        const float vol35   = settings->getFloat("floppy_sound_volume_35", vol525);
        // WASM default boots from an HDV ("hard disk"), and the disk-drive
        // mechanical sounds carry loud over the browser's Web Audio path.
        // Quarter the disk channels in the browser build. (HDV access itself
        // is silent — the 5.25"/3.5" floppy channels are the only disk sounds,
        // so they are what the "HD noise" actually is.) The factor scales the
        // restored value, so a user who lowers the mixer slider further still
        // sticks (the mixer reads the live device volume, not settings).
#ifdef __EMSCRIPTEN__
        constexpr float kWasmDiskGain = 0.25f;
#else
        constexpr float kWasmDiskGain = 1.0f;
#endif
        controller->floppySound525().setVolume(vol525 * kWasmDiskGain);
        controller->floppySound525().setMuted (mute525);
        controller->floppySound35().setVolume(vol35 * kWasmDiskGain);
        controller->floppySound35().setMuted(
            settings->getBool ("floppy_sound_muted_35",  mute525));
        // Audio master (mixer panel). Default 1.0 / unmuted to preserve
        // pre-mixer behaviour.
        controller->audio().setMasterVolume(
            settings->getFloat("master_volume", 1.0f));
        controller->audio().setMasterMuted(
            settings->getBool ("master_muted",  false));
        // Stereo bus (2026-08-01). Off = true stereo, which is what the
        // hardware does; the switch exists for mono playback gear and
        // for anyone who would rather not have a single-AY tune arrive
        // from the left speaker only.
        controller->audio().setMonoDownmix(
            settings->getBool ("audio_mono_downmix", false));
        // Stereo placement of the mono sources. Centre by default — the
        // Apple's own speaker has no stereo position to be faithful to,
        // so this is a taste knob, not an emulation one.
        controller->speaker().pan.store(
            settings->getFloat("speaker_pan",  0.0f));
        controller->cassette().pan.store(
            settings->getFloat("cassette_pan", 0.0f));
        controller->floppySound525().pan.store(
            settings->getFloat("floppy_sound_pan",    0.0f));
        controller->floppySound35().pan.store(
            settings->getFloat("floppy_sound_pan_35", 0.0f));
    }

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

        // Panel visibility: one loop over the registry (MainWindow_Panels.cpp)
        // instead of the 32 hand-written lines that used to live here — and
        // the 32 that mirrored them in the save path, which is where they
        // drifted: seven panels the palette could open had no key at all, so
        // the user opened them and they were gone next launch.
        loadPanelVisibility();
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
        controller->rewind().setEnabled(settings->getBool("rewind_enabled", false));
        joystick->binding().squareGate =
            settings->getBool("joystick_square_gate",
                              joystick->binding().squareGate);
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
            printerSound->setVolume(
                settings->getFloat("printer_sound_volume", 0.35f));
            printerSound->setMuted(
                settings->getBool("printer_sound_muted", false));
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
            printerBackPressure =
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
        // Was 28 assignments naming 28 panels, which meant every panel added
        // after it was written stayed open on the browser build — the list
        // could only rot in one direction. The registry knows all of them.
        hideAllPanels();
        // …except the greeting a browser user with no ROM still needs: the
        // constructor opened it above, and chrome-light is about chrome.
        if (!romLoaded_) show(pom2::PanelId::Welcome) = true;
#endif
    }

    // Disk II / HDV / CFFA / SmartPort media are restored by
    // StorageCoordinator::restoreMediaFromSettings(), at the end of
    // plugSlotsFromSettings() above — one pass against the finished topology
    // rather than a second one here.
    //
    // This block used to do it, and it ran AFTER the plug pass, so every image
    // was opened twice at startup. It also called insertDisk() with the
    // default argument, so drive 2 was never restored at all: its path was
    // persisted on exit and silently ignored on the next launch.

    // ── Restore previously-mounted 3.5" disks ─────────────────────────
    // Same pattern as the 5.25" / HDV restore above. Only honour the
    // paths when the file still exists; silently skip otherwise so a
    // moved / deleted image doesn't block startup.
    {
        std::error_code ec;
        const std::string p1 = settings->getString("disk35_path_1", "");
        if (!p1.empty() && fs::is_regular_file(p1, ec) &&
            controller->mount35(0, p1)) {
            pom2::log().info("Sony35", "Internal re-mounted from settings: " + p1);
        }
        const std::string p2 = settings->getString("disk35_path_2", "");
        if (!p2.empty() && fs::is_regular_file(p2, ec) &&
            controller->mount35(1, p2)) {
            pom2::log().info("Sony35", "External re-mounted from settings: " + p2);
        }
    }

    // ── Restore audio levels ─────────────────────────────────────────
    {
        const float spkVol = settings->getFloat("speaker_volume", 1.0f);
        controller->speaker().setVolume(spkVol);
        controller->speaker().setMuted(settings->getBool("speaker_muted", false));
        controller->setCassetteVolume(settings->getFloat("cassette_volume", 0.6f));
        controller->cassette().setAutoRewind(
            settings->getBool("cassette_auto_rewind", false));
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
    aiServer->attach(controller.get(), display.get(), primaryDiskII(), primaryHdvCard());
    aiServer->setAuthToken(aiTokenInput);
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
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

bool MainWindow::setChatMauveInvertBit7(bool v)
{
    // Resolves the card under the lock, writes, unlocks, then persists.
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

    // Persist the current state so the next launch restores the same
    // mounted disks, video mode, panels, and audio levels.
    // Skip persisting an HDV card that ensureHdvCardForBoot auto-plugged for
    // a one-shot `POM2 <image.hdv>` boot — it's session-local by contract.
    const bool hdvIsAutoProvisioned =
        primaryHdvCard() && primaryHdvCard()->getSlot() == storageCoordinator_->autoProvisionedHdvSlot();
    if (!hdvIsAutoProvisioned && primaryHdvCard() && primaryHdvCard()->isImageLoaded()) {
        // Don't persist the synthesised host-folder volume — the path is
        // a sentinel, not a real file. Re-synthesis happens on click.
        const std::string& p = primaryHdvCard()->getImagePath();
        if (p.rfind("[host folder] ", 0) == std::string::npos) {
            settings->setString("hdv_path", p);
        } else {
            settings->setString("hdv_path", "");
        }
    } else {
        settings->setString("hdv_path", "");
    }

    // Persist per-slot DiskII state. The primary (lowest-slot) card ALSO
    // writes to the legacy unsuffixed `disk_path` / `disk_writeback` so
    // an older POM2 build reading this settings.ini still sees the disk.
    //
    // Flush the 5.25" media FIRST — the 3.5" block below has always done
    // this, and its comment claimed to "mirror the Disk II save-on-shutdown
    // hook", but no such hook existed: quitting with write-back on threw
    // away every sector DOS had written since the last eject. The card's
    // destructor now flushes too (covering profile switches, which rebuild
    // the slot cards without ejecting), but doing it here keeps it ordered
    // before the settings write and inside the same teardown the user can
    // see in the log.
    {
        std::string flushError;
        SlotBus& bus = controller->memory().slotBus();
        if (!storageCoordinator_->flushAll(bus, flushError))
            pom2::log().warn("Storage", "shutdown flush: " + flushError);

        // Both drives, and the legacy unsuffixed aliases from the lowest-slot
        // card so an older POM2 build reading this settings.ini still finds
        // the disk. The loop this replaces called isDiskLoaded()/getDiskPath()
        // with their default arguments, so drive 2's path was never written on
        // exit — the last of the five places that mistake was made.
        const auto snapshot = storageCoordinator_->captureRebuildSnapshot(bus);
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

    // Same auto-provision guard as `hdv_path` above: a card that
    // ensureHdvCardForBoot plugged for a one-shot drag-drop / CLI boot is
    // session-local, so its write-back flag must not overwrite the one the
    // user configured for their real HDV card. Without the guard, a single
    // dropped .hdv persisted `hdv_writeback = false` and silently disarmed
    // write-back for unrelated media on the next launch.
    if (primaryHdvCard() && !hdvIsAutoProvisioned) {
        settings->setBool("hdv_writeback", primaryHdvCard()->isWriteBackEnabled());
    }

    // CFFA per-slot image + write-back for EVERY plugged CFFA card. `cffa`
    // is multi-instance, so persist each (not just the primary `primaryCffaCard()`),
    // mirroring the DiskII loop above. (blockCards() also returns synthetic
    // HDV cards — those persist via hdv_path; skip them here.)
    for (auto* blk : blockCards()) {
        auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
        if (!cffa) continue;
        const std::string key = "cffa_slot" + std::to_string(cffa->getSlot());
        settings->setString(key + "_path",
                            cffa->isImageLoaded() ? cffa->getImagePath()
                                                  : std::string());
        settings->setBool(key + "_writeback", cffa->isWriteBackEnabled());
    }

    // Per-slot persistence so the //c's two SSC ports (printer sl1 +
    // modem sl2) each keep their own port / listener / raw-mode state.
    // Legacy global keys (`ssc_listening`, `ssc_port`, `ssc_raw_mode`)
    // are mirrored to the primary SSC for backwards-compat with older
    // settings files and the AI control path.
    for (auto* ssc : serialCards()) {
        if (!ssc) continue;
        const std::string sk = "_slot" + std::to_string(ssc->getSlot());
        settings->setBool("ssc_listening" + sk, ssc->isListening());
        settings->setInt ("ssc_port"      + sk, ssc->getPort());
        settings->setBool("ssc_raw_mode"  + sk, ssc->rawMode());
        settings->setBool("ssc_printer_tap" + sk, ssc->printerTap());
    }
    if (primarySerialCard()) {
        settings->setBool("ssc_listening", primarySerialCard()->isListening());
        settings->setInt ("ssc_port",      primarySerialCard()->getPort());
        settings->setBool("ssc_raw_mode",  primarySerialCard()->rawMode());
    }

    // FujiNet relay — transport choice and its parameters, per slot.
    // Resolved from the live bus rather than an alias: the destructor runs
    // after controller->stop(), so this is a UI-thread topology read.
    pom2::FujiNetCard* fujiNet = nullptr;
    for (int s = 1; s < SlotBus::kSlotCount && !fujiNet; ++s)
        fujiNet = dynamic_cast<pom2::FujiNetCard*>(
            controller->memory().slotBus().peripheral(s));
    if (fujiNet) {
        const std::string sk = "_slot" + std::to_string(fujiNet->getSlot());
        const auto& link = fujiNet->transportLink();
        settings->setBool("fujinet_enabled" + sk, link.isRunning());
        settings->setInt ("fujinet_timeout_ms" + sk, link.timeoutMs());
        settings->setString("fujinet_transport" + sk,
                            link.mode() == pom2::FujiNetTransport::Mode::Serial
                                ? "serial" : "tcp");
        settings->setInt   ("fujinet_port" + sk, link.tcpPort());
        settings->setString("fujinet_serial_path" + sk, link.serialPath());
        settings->setInt   ("fujinet_serial_baud" + sk, link.serialBaud());
        settings->setString("fujinet_helper_path" + sk,
                            networkCoordinator_->helperPath());
    }

    // AI control listener — persist enable, port, token, and the panel
    // visibility flag. Re-armed on next launch by the constructor.
    settings->setBool  ("ai_control_enable", aiServer->isRunning());
    settings->setInt   ("ai_control_port",   aiServer->getPort());
    settings->setString("ai_control_token",  aiTokenInput);
    // Persist the per-slot card mapping so changes via the Slot
    // Configuration panel survive a restart. Slots the ACTIVE profile forces
    // (//c/+ on-board SSC/Mouse/SmartPort/Disk II, and the empty virtual slots
    // on a no-physical-slots model) are NOT persisted — `slotCards` holds the
    // forced built-in there, and writing it would clobber the user's real
    // choice (e.g. quitting on //c would overwrite slot_4_card=mockingboard
    // with the //c's on-board "mouseaw", losing it when they go back to //e).
    // The Le Chat Mauve rear-connector adapter IS user-controllable on //c, so
    // it persists. (Mirrors the "saved key left untouched" contract in
    // plugSlotsFromSettings.)
    {
        const auto& cfg = pom2::profileConfig(activeProfile);
        for (int s = 1; s <= 7; ++s) {
            if (s == storageCoordinator_->autoProvisionedHdvSlot()) continue;   // session-local auto-plug
            if (s == storageCoordinator_->autoProvisionedSmartPortSlot()) continue;   // idem (Floppy Emu)
            // Profile-forced slots (built-ins / noPhysicalSlots) hold the
            // profile's value, not the user's — shared guard with the Slot
            // Config Apply button (pom2::slotKeyIsUserChoice).
            const std::string key = "slot_" + std::to_string(s) + "_card";
            if (!pom2::slotKeyIsUserChoice(cfg, s, slotCards[s],
                                           settings->getString(key, "")))
                continue;
            settings->setString(key, slotCards[s]);
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
    savePanelVisibility();
    settings->setString("floppyemu_mode",
                        pom2::FloppyEmuDevice::modeKey(floppyEmu->mode()));
    settings->setString("floppyemu_sd_root", floppyEmu->sdRoot());
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
    settings->setBool  ("rewind_enabled",  controller->rewind().enabled());
    settings->setInt   ("imagewriter_paper",
                        static_cast<int>(imageWriter->paperSize()));
    settings->setInt   ("imagewriter_dpi",    imageWriter->dpi());
    settings->setInt   ("imagewriter_model",
                        static_cast<int>(imageWriter->model()));
    settings->setBool  ("imagewriter_backpressure", printerBackPressure);
    settings->setInt   ("imagewriter_ribbon",
                        static_cast<int>(imageWriter->ribbon()));
    settings->setInt   ("imagewriter_autolf_mode",
                        static_cast<int>(imageWriter->autoFeedMode()));
    settings->setInt   ("imagewriter_speed",
                        static_cast<int>(imageWriter->speed()));
    // No-ops when no Grappler+ is plugged, so the keys keep their previous
    // values rather than being overwritten with a default.
    printerCoordinator_->persistGrappler(*settings, *controller);
    settings->setBool  ("nsclock_enable",  controller->noSlotClock().isEnabled());
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
    // One call for the whole audio block, host controls and slot cards alike.
    // The slot-card half is why it matters: the old code persisted a single
    // `mockingboard_volume` read through the last-plugged alias, so with two
    // Mockingboard variants on the bus one of them silently inherited the
    // other's level on the next launch. Each live card now gets its own
    // per-slot key, and the highest slot of each type still writes the legacy
    // type-wide key so existing state.cfg files keep working.
    audioCoordinator_->persist(*settings,
                               controller->speaker(),
                               controller->cassette(),
                               controller->floppySound525(),
                               controller->floppySound35(),
                               *printerSound);
    settings->setString("char_rom_locale",        pom2::charRomLocaleKey(charRomLocale));

    // Kiosk is a read-only launcher: don't write state.cfg, so the disk it
    // booted (and any HDV card auto-plugged for it by ensureHdvCardForBoot)
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

    if (aboutImageTex_) {
        GLuint t = aboutImageTex_;
        glDeleteTextures(1, &t);
        aboutImageTex_ = 0;
    }
    if (kbImageTex_) {
        GLuint t = kbImageTex_;
        glDeleteTextures(1, &t);
        kbImageTex_ = 0;
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
//   "mouse"      MouseCard (Phase 4 — falls through with a warning until then)
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
    // Idempotent, and it stays idempotent: the printer sound is re-registered
    // from every plugSlotsFromSettings() pass, and a double entry would mix
    // the source twice and then dangle after a single removeSource().
    audioCoordinator_->registerSource(src);
}

void MainWindow::unregisterAllAudioSources()
{
    audioCoordinator_->unregisterAll();
}

void MainWindow::plugSlotsFromSettings(const pom2::StateAccess& st)
{
    namespace fs = std::filesystem;

    // The fresh-install default map (grappler / mouseaw / — / mockingboard /
    // smartport35 / diskii / chatmauve, slot 3 deliberately empty because the
    // //e's 80 columns are internal $C300 ROM + the AUX connector, not a slot
    // card) now lives in SlotConfigurationCoordinator, next to the settings
    // lookup that falls back to it. CLAUDE.md documents the map itself.


    // The effective plan: settings defaults, the legacy `clock_card_enable`
    // opt-out, profile-forced built-in slots, the //c-class no-physical-slots
    // rule and the single-instance policy, resolved in one place.
    //
    // What it is NOT is a record of what ended up plugged. The plan holds what
    // the user asked for; a missing ROM or a session-only auto-provisioned
    // card does not rewrite it, so a CFFA whose firmware is absent today is
    // still a CFFA in the config tomorrow. The live SlotBus is the authority
    // on what is actually there, and the panel reads it separately.
    const auto& plan = slotConfigCoordinator_->resolve(*settings, activeProfile);
    for (int s = 1; s <= 7; ++s) slotCards[s] = plan[s];

    // ── Per-card construction helpers. Each one populates the matching
    //    raw `*Card` member pointer (non-owning) for the rest of MainWindow
    //    to find, and plugs the card into the SlotBus. ────────────────

    // Read once for every factory Request below: the CFFA firmware comes in
    // an NMOS and a 65C02 variant and the factory picks by this.
    const bool cpuIsCmosForSlots =
        st.cpu().getCpuMode() == M6502::CpuMode::CMOS;

    auto plugDiskII = [&](int s) {
        // Construction + every ROM lookup belongs to the factory: four
        // optional PROMs (boot, P6 LSS, and the 13-sector pair) each had their
        // own hand-rolled {roms/, ../roms/, ../../roms/} candidate loop here,
        // which is what `pom2::findResource` already does.
        auto made = slotCardFactory_->create(
            { "diskii", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        diskRomPath   = made.resourcePath;
        diskRomStatus = made.status;
        auto* card = static_cast<DiskIICard*>(made.card.get());

        // Runtime wiring stays here — it is composition, not construction.
        // Sub-instruction cycle accuracy on MMIO: cycle-precise copy
        // protections read the LSS state at the exact sub-cycle of the data
        // fetch, not at instruction start (DiskIICard::setCpu).
        card->setCpu(&st.cpu());
        card->setFloppySound(&controller->floppySound525());
        // //c+ on-board IWM — only the slot-6 card pushes its drive pointer to
        // the IWM. A second Disk II (slot 4, say) stays off that path so it
        // cannot clobber the //c+ flux mirror.
        if (s == 6) card->setIWM(&controller->iwm());

        // Media and write-back are NOT restored here. This builds empty
        // hardware; StorageCoordinator::restoreMediaFromSettings() runs once
        // at the end of this function against the finished topology, because
        // "is this the primary card" is a property of the whole bus.
        diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
        if (!diskPanel) diskPanel = diskPanels.front().get();
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugHdv = [&](int s) {
        // Empty hardware only; the image and write-back opt-in arrive in the
        // restore pass at the end of this function. `hdvPath` is seeded from
        // settings here purely so the panel has something to show before that
        // pass runs — an empty `hdv_path` means "nothing mounted", not "scan
        // hdv/ and pick one", which is what it used to mean and what silently
        // re-mounted a hard disk the user had just ejected.
        const std::string saved = settings->getString("hdv_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) hdvPath = saved;

        auto made = slotCardFactory_->create(
            { "hdv", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugCffa = [&](int s) {
        // The factory picks the firmware variant by CPU — the CFFA 2.0 ROM
        // ships in an NMOS and a 65C02 build — and falls back to the other if
        // only one is present. A missing or unloadable ROM clears the slot:
        // a CFFA with no firmware is not a card, it is a hole at $Cn00.
        auto made = slotCardFactory_->create(
            { "cffa", s, cpuIsCmosForSlots, activeProfile });
        if (!made) {
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            // The slot stays empty, but the PLAN keeps the user's request.
            // Clearing it here meant a CFFA whose ROM was missing today was
            // silently deleted from the config and gone tomorrow; the panel
            // now reports plan-vs-live divergence instead.
            return;
        }
        // Empty hardware only — the per-slot image and write-back arrive in
        // the restore pass at the end of this function.
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugChatMauve = [&](int s) {
        auto card = std::make_unique<LeChatMauveCard>(s);
        // Local, not a retained alias: the display genuinely needs the card
        // for its RGB decode path and is re-pointed on every rebuild.
        LeChatMauveCard* plugged = card.get();
        if (settings) {
            plugged->setInvertBit7(
                settings->getBool("chatmauve_invert_bit7", false));
            plugged->setColorTextEnabled(
                settings->getBool("chatmauve_color_text", true));
            plugged->setHgrDuochromeEnabled(
                settings->getBool("chatmauve_hgr_duochrome", false));
        }
        st.memory().slotBus().plug(s, std::move(card));
        display->setChatMauveCard(plugged);
    };

    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        SuperSerialCard* raw = card.get();
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
        // No alias list: serialCards() reads the bus, slot-ascending, so the
        // lowest-slot card is the primary exactly as before. The card is
        // already plugged above, so it is visible to that read.
        // Per-slot persistence; fall back to legacy global keys (the
        // primary SSC was the only one before //c dual-port support).
        const std::string sk = "_slot" + std::to_string(s);
        const bool legacyPrimary = (raw == primarySerialCard());
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

            // Give the card its host transport at plug time. The card cannot
            // build one itself — that would be a device reaching into runtime.
            raw->setTransport(pom2::makeSuperSerialTcpTransport(*raw, s));
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

    // Both Ethernet cards share one host-transport decision, so the
    // backend factory lives here rather than in either plug lambda.
    // Settings key `ethernet_backend`: "slirp" (default) | "loopback" |
    // "none". Loopback is a self-test mode — everything the guest
    // transmits comes straight back — and is also the honest fallback
    // when a user explicitly wants the cards inert.
    auto makeEthernetBackend = [&](const char* who)
        -> std::unique_ptr<pom2::NetworkBackend> {
        const std::string choice =
            settings->getString("ethernet_backend", "slirp");
        if (choice == "loopback")
            return std::make_unique<pom2::LoopbackNetworkBackend>();
        if (choice == "none")
            return std::make_unique<pom2::NullNetworkBackend>();

        if (!pom2::slirpAvailable()) {
            pom2::log().warn(who,
                "libslirp not compiled in — no host Ethernet transport. "
                "Uthernet II TCP/UDP still works; install libslirp-dev and "
                "rebuild for raw-frame modes and the Uthernet I.");
            return std::make_unique<pom2::NullNetworkBackend>();
        }
        auto slirp = pom2::makeSlirpBackend("pom2");
        if (!slirp) {
            pom2::log().warn(who, "libslirp failed to start — falling back "
                                  "to no host transport");
            return std::make_unique<pom2::NullNetworkBackend>();
        }
        return slirp;
    };

    auto plugUthernet = [&](int s) {
        // a2RetroSystems Uthernet I — CS8900A NIC, raw Ethernet only.
        // Without a working transport the card still plugs and probes
        // (drivers detect it via the PacketPage ProductID), it just never
        // sees a frame — which is a better failure mode than hiding it.
        auto card = std::make_unique<pom2::UthernetCard>(s);
        card->setBackend(makeEthernetBackend("Uthernet"));
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugUthernetII = [&](int s) {
        // a2RetroSystems Uthernet II — W5100 hardware TCP/IP. Its TCP and
        // UDP sockets are host sockets, so this card is fully functional
        // with no backend at all; the backend only serves MACRAW/IPRAW.
        auto card = std::make_unique<pom2::UthernetIICard>(s);
        // Inject the host socket factory: the W5100 cannot build one itself,
        // and without it its TCP/UDP modes stay CLOSED.
        card->chip().setSocketFactory(pom2::makeHostW5100SocketFactory());
        card->chip().setNameResolver(
            std::make_unique<pom2::W5100NameResolver>());
        card->setBackend(makeEthernetBackend("UthernetII"));
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
        auto card = pom2::makeFujiNetCard(s);
        card->setMemory(&st.memory());
        card->setCpu(&st.cpu());

        const std::string sk = "_slot" + std::to_string(s);
        auto& link = card->transportLink();
        link.setTimeoutMs(settings->getInt("fujinet_timeout_ms" + sk,
                                           pom2::FujiNetTransport::kDefaultTimeoutMs));

        // Built-in N:, on by default. The FujiNet desktop build's own network
        // device answers the guest's open with success and then never opens a
        // socket, so relaying faithfully to it means the guest can never
        // fetch anything; POM2 serving N: itself is the difference between a
        // machine that browses and one that does not. Everything else still
        // goes to the peer. Set `fujinet_builtin_network<slot> = false` for a
        // real FujiNet board over USB, whose N: works and does far more than
        // plain HTTP.
        card->setBuiltInNetwork(
            settings->getBool("fujinet_builtin_network" + sk, true));

        const std::string transport =
            settings->getString("fujinet_transport" + sk, "tcp");
        if (transport == "serial") {
            link.setSerialMode(
                settings->getString("fujinet_serial_path" + sk, ""),
                settings->getInt("fujinet_serial_baud" + sk,
                                 pom2::SerialPort::kDefaultBaud));
        } else {
            link.setTcpMode(static_cast<uint16_t>(
                settings->getInt("fujinet_port" + sk,
                                 pom2::SpTcpTransport::kDefaultPort)));
        }

        if (settings->getBool("fujinet_enabled" + sk, true)) {
            std::string err;
            if (!link.start(err))
                pom2::log().warn("FujiNet", "link not started: " + err);
        }

        st.memory().slotBus().plug(s, std::move(card));

        // setHelperPath resolves against PATH as well: a configured name the
        // host cannot find is what the panel must show as unresolved.
        networkCoordinator_->setHelperPath(
            settings->getString("fujinet_helper_path" + sk, ""));
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
        card->setVolume(settings->getFloat("phasor_volume", 0.5f));
        card->setMuted (settings->getBool ("phasor_muted",  false));
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
        card->setVolume(settings->getFloat("echoplus_volume", 0.7f));
        card->setMuted (settings->getBool ("echoplus_muted",  false));
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
        // Orange Micro Grappler+ — ROM-gated parallel printer. Loads
        // roms/grappler_plus.bin if present; falls back to a synthetic
        // stub ROM (PR#n trampoline only) so the card always plugs.
        auto made = slotCardFactory_->create(
            { "grappler", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        if (!made.warning.empty())
            pom2::log().warn(made.warningCategory.c_str(), made.warning);
        auto* card = static_cast<GrapplerCard*>(made.card.get());
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
        card->setVolume(settings->getFloat("mockingboard_volume", 0.5f));
        card->setMuted(settings->getBool ("mockingboard_muted",  false));
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugSmartPort35 = [&](int s) {
        // Liron-class card. Each unit's type + image is restored from
        // settings (smartport_slotN_unitK_*) so per-card mixes (e.g.
        // unit 0 = 3.5", unit 1 = HDV) survive across launches. When
        // no setting exists, both units start empty — the user picks
        // a type via the SmartPort Configuration panel.
        // The factory owns the Liron ROM rule: the real identity
        // (roms/liron.rom, BMOW dump) goes on slot-having machines only —
        // NEVER on //c-class, whose on-board $C500 stub must keep the
        // synthetic $Cn07=$01 ProDOS-block identity, because a
        // SmartPort-class byte there re-triggers the boot-scan confusion
        // (project_iic_smartport_boot). That is why Request carries the
        // profile.
        auto made = slotCardFactory_->create(
            { "smartport35", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        auto* card = static_cast<pom2::SmartPortCard*>(made.card.get());
        // Mechanical sound: route to the dedicated 3.5" sound bank. Block-level
        // transfers only — the card synthesises step / motor / click events
        // from READBLOCK / WRITEBLOCK directly. Wiring, so it stays here.
        card->setFloppySound(&controller->floppySound35());

        // Units are NOT built here. The card is plugged empty and
        // StorageCoordinator::restoreMediaFromSettings() creates each unit
        // from its persisted kind, applies the write-back opt-in and resolves
        // the image path against the same cwd anchors, once the whole topology
        // exists. Keyspace unchanged:
        //   smartport_slotN_unitK_type      ("" / "35" / "hdv")
        //   smartport_slotN_unitK_path      (image path, optional)
        //   smartport_slotN_unitK_writeback (bool)
        // No alias to seed: primarySmartPortCard() reads the bus.
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugMouse = [&](int s) {
        // MAME-faithful 68705P3 + 6821 PIA. Both Apple ROMs are required —
        // without them the card has no firmware and refuses to plug, which is
        // why the slot is left empty rather than half-built.
        auto made = slotCardFactory_->create(
            { "mouse", s, cpuIsCmosForSlots, activeProfile });
        mouseRomStatus = made.status;
        if (!made) {
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            return;
        }
        // IRQ routing is auto-wired by SlotBus (Memory::setCpu): the MCU's
        // PB6 reaches the CPU through SlotPeripheral::assertIrq, which fans
        // out via M6502::setIrqLine(slot, …).
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    // Dispatch: walk slots 1..7 and plug whichever card the settings ask
    // for. Anything we don't recognise is logged and skipped.
    for (int s = 1; s <= 7; ++s) {
        const std::string& kind = slotCards[s];
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
        else if (kind == "mouseaw") {
            // The factory owns the fallback: if the AppleWin HLE slot EPROM
            // is missing or will not load, it builds the MC68705 "mouse" card
            // instead and reports `fallback` with the reason, rather than
            // leaving the slot empty. MODE_INT_VBL pacing follows the
            // machine's VIDEO frame — scanlinesPerFrame × cyclesPerScanline
            // (17030 NTSC / 20280 PAL), not the worker's cyclesPerFrame
            // budget — which is why the Request carries the profile.
            auto made = slotCardFactory_->create(
                { "mouseaw", s, cpuIsCmosForSlots, activeProfile });
            mouseRomStatus = made.status;
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            if (!made) continue;
            // A fallback is a LIVE fact, not a configuration change: the
            // user still asked for "mouseaw", and the ROM may be there next
            // launch. The panel reads the live snapshot to show what is
            // actually plugged.
            st.memory().slotBus().plug(s, std::move(made.card));
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

    // ── Phase 2: media, once the whole topology exists ────────────────────
    // Everything above builds EMPTY hardware. Only now can "is this the
    // primary Disk II / HDV" be answered, which is what the legacy `disk_path`
    // and `disk_writeback` keys fall back on — asking it while the bus was
    // half-built is what made a moved primary HDV and the second Disk II drive
    // restore against the wrong card.
    //
    // Runs under the caller's lock (this function takes a StateAccess to prove
    // it), so it does its file reads there, exactly as the per-card restores
    // it replaces did. That is the documented profile-switch exception in
    // MainWindow_Slots.cpp: the CPU worker is stopped across a rebuild anyway.
    const auto restored =
        storageCoordinator_->restoreMediaFromSettings(st.memory().slotBus(),
                                                      *settings);
    for (const std::string& warning : restored.warnings)
        pom2::log().warn("Storage", warning);

    // The HDV panel's status line is derived, not remembered.
    if (auto* hdv = primaryHdvCard()) {
        hdvStatus = hdv->isImageLoaded()
                        ? std::string("loaded: ") + hdv->getImagePath()
                        : "no image mounted";
        hdvPath = hdv->isImageLoaded() ? hdv->getImagePath() : std::string();
    } else {
        hdvStatus = "no image mounted";
    }
}

// ─── Screenshot ───────────────────────────────────────────────────────────

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
        tapeStatusMessage = "Screenshot: cannot write " + path;
        tapeStatusUntil   = lastFrameTime + 3.0;
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
    tapeStatusMessage = "Screenshot: " + path;
    tapeStatusUntil   = lastFrameTime + 3.0;
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
    if (kioskMenuOpen_) return;
    // In kiosk, K is reserved (Select fallback): the OPEN direction leaks
    // otherwise — this callback fires while the menu is still closed, then
    // updateKioskMenu opens the non-pausing Keys band the same frame, so the
    // running game would receive a live 'k' on every open.
    if (kiosk_ && (codepoint == 'k' || codepoint == 'K')) return;
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
    // Through `pushAppleKeys()`, never straight to Memory: the on-screen
    // keyboard presses the same two wires and would otherwise clear this.
    if (key == GLFW_KEY_LEFT_ALT) {
        appleKeys_.hostOpen = (action != GLFW_RELEASE);
        pushAppleKeys();
        return;
    }
    if (key == GLFW_KEY_RIGHT_ALT) {
        appleKeys_.hostSolid = (action != GLFW_RELEASE);
        pushAppleKeys();
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
    if (kioskMenuOpen_) {
        // F10 still leaves kiosk with the in-kiosk menu open — the user
        // must always have a way back to the GUI. (Ctrl+Alt+F is handled
        // above, so it works here too.)
        if (key == GLFW_KEY_F10 && action == GLFW_PRESS) toggleKioskMode();
        return;
    }
    // K reserved in kiosk (see onChar) — also blocks Ctrl-K's $0B, since
    // eSelect fires on the K key regardless of modifiers.
    if (kiosk_ && key == GLFW_KEY_K) return;

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
        tapeStatusMessage = "Paste: clipboard is empty";
        tapeStatusUntil   = lastFrameTime + 3.0;
        return;
    }
    std::string text = clip;
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars queued from clipboard", queued);
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::pasteFromFile(const std::string& path)
{
    std::ifstream f(path);
    if (!f) {
        tapeStatusMessage = "Paste: cannot open " + path;
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    std::string text;
    text.resize(Memory::kPasteMaxChars);
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(f.gcount()));
    const bool truncated = f.peek() != std::char_traits<char>::eof();
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars%s from %s", queued,
                  truncated ? " (file truncated)" : "", path.c_str());
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
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

void MainWindow::openCommandPalette()
{
    if (cmdPalette) cmdPalette->open();
}

void MainWindow::renderCommandPalette()
{
    if (!cmdPalette || !cmdPalette->isOpen()) return;

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
        kiosk_ ? "Leave full screen (kiosk)" : "Full screen (kiosk)",
        "Ctrl+Alt+F / F10", true, kiosk_);
    add("view.mousegrab", "View",
        mouseGrabbed_ ? "Release mouse capture" : "Capture mouse",
        "Ctrl+Alt+G", mouseCoordinator_->capture().plugged(),
        mouseGrabbed_);

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
             devicePanelCoordinator_->captureInventory().chatMauvePlugged());
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
    // One line, and it is the point of the exercise: this list used to be 38
    // hand-written entries here and 38 more in runCommand's dispatch table,
    // with a third copy of the same names in the menus. All three are now
    // views of PanelCatalog.h + registerPanels().
    forEachPanelCommand([&add](const char* id, const std::string& label,
                               const char* shortcut, bool enabled, bool checked) {
        add(id, "Panel", label, shortcut, enabled, checked);
    });

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

    // Panels: one lookup in the registry. This used to be a 38-row table of
    // id → &showXxx, kept in step by hand with the 38 rows that BUILT those
    // commands 80 lines above and with the menu rows that toggle the same
    // flags. `runPanelCommand` returns false for a non-panel id, so the
    // handling below it is unchanged.
    if (runPanelCommand(id)) return;

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

    if (ImGui::BeginMenu("File")) {
        panelMenuItem(pom2::PanelId::DiskLibrary);
        ImGui::Separator();
        // Disk II (slot 6) — frequent action, lifted out of the old
        // Hardware kitchen-sink. Panel still exposes its own insert/eject
        // buttons; this is the keyboard-friendly path.
        ImGui::BeginDisabled(primaryDiskII() == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks_5.4/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            primaryDiskII() && primaryDiskII()->isDiskLoaded())) {
            // Through the coordinator so the persisted disk_path_slotN key is
            // cleared with the medium. Ejecting from this menu used to leave
            // the path behind, and the next launch re-mounted the disk the
            // user had just ejected.
            const int slot = primaryDiskII()->getSlot();
            const auto r = storageCoordinator_->ejectDiskII(
                *controller, *settings, slot, 0);
            tapeStatusMessage = r.ok ? "Disk ejected"
                                     : "Disk eject failed: " + r.error;
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(primaryHdvCard() == nullptr);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            hdvPanel->mountDialogOpen = true;
            if (hdvPanel->dialogPath.empty()) hdvPanel->dialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            primaryHdvCard() && primaryHdvCard()->isImageLoaded())) {
            const int slot = primaryHdvCard()->getSlot();
            const auto r = storageCoordinator_->ejectMediaBay(
                *controller, *settings, slot, 0);
            tapeStatusMessage = r.ok ? "HDV ejected"
                                     : "HDV eject failed: " + r.error;
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!primaryHdvCard() || !primaryHdvCard()->isImageLoaded());
        // Label reflects where the user actually has the card plugged.
        const std::string bootHdvLabel = "Boot HDV (slot " +
            std::to_string(primaryHdvCard() ? primaryHdvCard()->getSlot() : 5) + ")";
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
            showPasteFileDialog = true;
#endif
        ImGui::Separator();
        const size_t pending = controller->memory().pendingPasteSize();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::MenuItem("Cancel pending paste")) {
            controller->memory().cancelPaste();
            tapeStatusMessage = "Paste cancelled";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Auto-uppercase pasted text", nullptr, &pasteAutoUppercase);
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
        panelMenuItem(pom2::PanelId::SlotConfig);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        // One flat 17-item list became hard to scan (audit 2026-05-31), so it
        // is grouped under SeparatorText headers. What used to follow was 25
        // hand-written rows carrying each panel's label, tooltip, greyed-out
        // condition and slot-number formatting — a third copy of facts the
        // palette and the settings round-trip also held. They live in
        // PanelCatalog.h now; this menu says only which groups it shows and
        // in what order.
        ImGui::SeparatorText("Storage");
        panelMenuGroup(pom2::PanelGroup::DevStorage);
        ImGui::SeparatorText("Sound");
        panelMenuGroup(pom2::PanelGroup::DevSound);
        ImGui::SeparatorText("Ports & cards");
        panelMenuGroup(pom2::PanelGroup::DevPorts);
        ImGui::SeparatorText("Inspectors & tools");
        panelMenuGroup(pom2::PanelGroup::DevInspectors);
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
        panelMenuItem(pom2::PanelId::Crt);

        // 3D voxel view (MicroM8 "Voxel Cube"): rebuild the screen as an
        // upright 4:3 slab of equal-depth cubes; left-drag orbits, middle-drag
        // pans, wheel zooms. Works on any colour mode.
        panelMenuItem(pom2::PanelId::Voxel);
        panelMenuItem(pom2::PanelId::VoxelSettings);

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
        ImGui::BeginDisabled(
            !devicePanelCoordinator_->captureInventory().chatMauvePlugged());
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
                            "Ctrl+Alt+F", kiosk_)) {
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
            const bool haveCard = mouseCoordinator_->capture().plugged();
            if (ImGui::MenuItem(ICON_FA_ARROW_POINTER " Capture mouse",
                                "Ctrl+Alt+G", mouseGrabbed_, haveCard)) {
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

        panelMenuItem(pom2::PanelId::MemViewer);
        panelMenuItem(pom2::PanelId::Debugger);
        ImGui::Separator();
        panelMenuItem(pom2::PanelId::MemBar);
        panelMenuItem(pom2::PanelId::MemBarH);
        panelMenuItem(pom2::PanelId::MemGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Command palette...", "Ctrl+Shift+P"))
            openCommandPalette();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fuzzy-search every menu item, panel and machine\n"
                              "action. Type \"mock\", \"amber\", \"eject\"...");
        ImGui::Separator();
        // The AI Control row hides itself under WASM — AiControlServer cannot
        // open a listening socket in the browser sandbox. That used to be an
        // #ifndef here; it is now one line in registerPanels(), where the rest
        // of the panel's identity already lives.
        panelMenuGroup(pom2::PanelGroup::Tools);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        // Three rows, and one of them used to be wrong: ROM Status's tooltip
        // was attached to the Abstraction Levels item (two IsItemHovered
        // blocks after the same MenuItem), so one row showed the other's tip
        // and ROM Status showed none. Both now come from the catalog.
        panelMenuGroup(pom2::PanelGroup::Help);
        ImGui::Separator();
        if (ImGui::MenuItem("About POM2")) showAbout = true;
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
                                              info.hasUnsavedChanges });
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
                    //
                    // Reserved as a REAL item (an InvisibleButton the exact
                    // size of the text) and painted through the draw list,
                    // rather than drawn as text with a hand-rolled
                    // IsMouseHoveringRect. That call is not z-order aware, so
                    // the chip lit up through anything drawn over it — its own
                    // eject popup included — while the click, which goes
                    // through IsItemClicked, correctly did not. One item now
                    // answers hover, tooltip and click alike.
                    const std::string chip =
                        std::string(row.icon) + " " + row.label;
                    const ImVec2 chipSz  = ImGui::CalcTextSize(chip.c_str());
                    const ImVec2 chipPos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton(
                        "##chip", ImVec2(ImMax(chipSz.x, 1.0f), lineH));
                    const bool hot = ImGui::IsItemHovered();
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(chipPos.x,
                               chipPos.y + (lineH - chipSz.y) * 0.5f),
                        hot ? pal.accent
                            : (row.active ? pal.text : pal.textDim),
                        chip.c_str());
                    if (hot)
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
            if (mouseGrabbed_ && roomFor(9.0f)) {
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
                if (lastFrameTime < mouseGrabHintUntil_ && roomFor(30.0f)) {
                    ImGui::SameLine();
                    ImGui::TextColored(u32(pal.textDim),
                                       ICON_FA_ARROW_RIGHT
                                       " Ctrl+Alt+G or middle click to release");
                }
            } else if (!mouseGrabbed_ && screenHovered_ &&
                       mouseCoordinator_->capture().plugged() && roomFor(30.0f)) {
                // Not captured, but the pointer is over the emulated screen
                // with a Mouse Card on the bus — the exact moment the user is
                // about to wonder why the guest cursor won't follow theirs.
                // Say how to hand it over, here rather than on the screen:
                // the on-screen captions were removed for being noise over a
                // running game, and this is the same information in the one
                // place that is already a status surface.
                //
                // `screenHovered_` is ImGui's z-order-aware verdict from
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
            if (hostCapsLock_ && roomFor(8.0f)) {
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
                const bool waiting =
                    printerBackPressure &&
                    printerCoordinator_->captureHost(*controller).grapplerBusy;
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
            // right-aligned and auto-expiring (tapeStatusUntil). This is the
            // text that used to float in a separate overlay near the bottom.
            if (!tapeStatusMessage.empty() && lastFrameTime < tapeStatusUntil) {
                const float msgW = ImGui::CalcTextSize(
                    tapeStatusMessage.c_str()).x;
                const float avail = ImGui::GetContentRegionAvail().x;
                if (avail > msgW) {
                    ImGui::SameLine(0.0f, avail - msgW);
                } else {
                    ImGui::SameLine();
                }
                ImGui::TextColored(
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent),
                    "%s", tapeStatusMessage.c_str());
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void MainWindow::renderScreenWindow()
{
    // Default startup layout. Native keeps room for the Disk Library column;
    // WASM starts with only menu + toolbar + Apple II Screen + status bar.
    // `FirstUseEver` only applies on a fresh install — once the user
    // moves / resizes the window their imgui.ini takes over.
#ifdef __EMSCRIPTEN__
    ImGui::SetNextWindowPos (ImVec2(5,    56),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 760), ImGuiCond_FirstUseEver);
#else
    ImGui::SetNextWindowPos (ImVec2(5,    90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 745), ImGuiCond_FirstUseEver);
#endif

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    // `NoMove` so ImGui's default "drag-from-anywhere" doesn't eat
    // click-and-drag gestures inside the screen (Mouse Card games like
    // A2Desktop need them to reach the guest). `NoCollapse` so a
    // double-click on the title bar doesn't accidentally collapse the
    // window — a single concern at a time. We restore the title-bar
    // drag manually below.
    const ImGuiWindowFlags screenFlags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("Apple II Screen", nullptr, screenFlags)) {
        // ── Manual title-bar drag ─────────────────────────────────────
        // Compute the title bar rect from the public Begin geometry
        // (window pos + size + frame height). When the user clicks
        // inside that strip we latch `screenDraggingByTitleBar`; on
        // every subsequent frame we apply `io.MouseDelta` until the
        // button is released. `IsAnyItemActive()` guards against
        // claiming a click that another widget already consumed (e.g.
        // any future title-bar button).
        //
        // Skipped entirely while DOCKED: a docked window has no title bar of
        // its own (it's a tab in the host node) and its position is owned by
        // the dock node. Left enabled, the rect we compute lands on the dock
        // node's tab bar and `SetWindowPos` fights the node every frame —
        // the screen jitters and the tab won't drag out.
        if (!ImGui::IsWindowDocked()) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float  th = ImGui::GetFrameHeight();
            const ImVec2 m  = ImGui::GetIO().MousePos;
            const bool overTitleBar =
                (m.x >= wp.x && m.x <= wp.x + ws.x &&
                 m.y >= wp.y && m.y <= wp.y + th);
            // Do not start dragging when another foreground window covers
            // this title-bar rectangle. ImGui's normal move handling already
            // gives that front window priority; the Apple II Screen's manual
            // title drag must obey the same z-order rule.
            const bool screenWindowHovered = ImGui::IsWindowHovered();
            if (screenWindowHovered && overTitleBar &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemActive()) {
                screenDraggingByTitleBar = true;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                screenDraggingByTitleBar = false;
            }
            if (screenDraggingByTitleBar) {
                const ImVec2 d = ImGui::GetIO().MouseDelta;
                if (d.x != 0.0f || d.y != 0.0f) {
                    ImGui::SetWindowPos(ImVec2(wp.x + d.x, wp.y + d.y));
                }
            }
        }

        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void MainWindow::drawScreenImage()
{
    uploadScreenTexture();

    // OpenEmulator-style composite path: when the user selected
    // ColorCompositeOE AND Apple2Display produced a 14.318 MHz signal
    // this frame, lazily spin up the NTSC shader and run a single pass
    // that consumes the signal texture and writes an RGBA output we
    // hand to ImGui::Image. The first call compiles the shader; if
    // anything fails (driver lacking GL 3.x, shader compile error, …)
    // we fall back to the regular `screenTexture` for the rest of the
    // session — no crashes, no flicker, just the existing LUT view.
    unsigned int presentTex = screenTexture;
    // The 3D voxel view must sample the decoded COLOUR image, NEVER the CRT
    // glass — scanlines / shadow-mask / barrel warp would bake into the cube
    // grid. Track that tap point separately: it follows the colour pipeline
    // (NTSC demod, OE or otherwise) but stops *before* CrtEffectStack. For all
    // non-OE-GPU modes the colour image already lives in `screenTexture`; the
    // OE-GPU branch below redirects it to the demod output.
    unsigned int voxelSrcTex = screenTexture;
    // Sharp-text override: when the user wants legible text under the
    // composite mode, skip the shader for TEXT scanlines and let the
    // crisp RGB framebuffer go straight to ImGui. Full-screen text uses
    // textSharp; mixed HGR/lo-res/DHGR keeps the demod on the graphics
    // band only — the bottom 4 text rows are patched in frame80 as
    // white/black after demod (mixedCompositeUsesFramebuffer).
    // Use the soft-switch snapshot the render() above actually consumed —
    // re-polling Memory::getDisplayState() here raced the CPU worker (it may
    // have advanced past the rendered frame between the two), flashing one
    // LUT-fallback frame on a text↔graphics switch.
    const auto displayState = display->lastRenderState();
    const bool oeGpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOE;
    const bool oeCpuMode = display->getHiResMode()
                         == Apple2Display::HiResMode::ColorCompositeOECpu;
    const bool oeMode    = oeGpuMode;
    const bool oeFamily  = oeGpuMode || oeCpuMode;
    const bool wantSharpText = ntscFx && ntscFx->getParams().textSharp
                            && displayState.textMode;
    const bool mixedFbPresent = display->mixedCompositeUsesFramebuffer();

    // Compute the on-screen target size up-front so the CRT effect pass can
    // render at native output resolution. That is what lets the scanline /
    // shadow-mask patterns be sampled finely enough to analytically
    // anti-alias (no barrel-warp moiré) and lets ImGui blit the result 1:1
    // (no second resample beat). Same three aspect modes used for the final
    // blit below, which reuses `avail` / `size`.
    const float W = static_cast<float>(Apple2Display::kWidth);
    const float H = static_cast<float>(Apple2Display::kHeight);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 size;
    switch (aspectMode) {
        case AspectMode::Crt43: {
            float h = std::min(avail.y, avail.x * 3.0f / 4.0f);
            h = std::max(h, H);
            size = ImVec2(h * 4.0f / 3.0f, h);
            break;
        }
        case AspectMode::Integer: {
            float s = std::max(1.0f, std::floor(std::min(avail.x / W, avail.y / H)));
            size = ImVec2(W * s, H * s);
            break;
        }
        case AspectMode::Square:
        default: {
            float s = std::max(1.0f, std::min(avail.x / W, avail.y / H));
            size = ImVec2(W * s, H * s);
            break;
        }
    }
    const int dstW = std::max(1, static_cast<int>(size.x + 0.5f));
    const int dstH = std::max(1, static_cast<int>(size.y + 0.5f));

    if (oeMode && display->signalProduced() && !wantSharpText && !mixedFbPresent) {
        if (!ntscFx) ntscFx = std::make_unique<pom2::NtscPostProcessor>();
        if (!ntscFx->available() && !ntscFx->initialize()) {
            // initialize() already logged the failure. Stop trying so
            // we don't spam the log every frame.
        }
        if (ntscFx->available()) {
            // Phase 4 final: NtscPostProcessor is now demod-only (colour
            // recovery). The CRT glass (scanlines / mask / barrel /
            // persistence / BCS) lives in the shared CrtEffectStack, so OE
            // chains into it just like every other mode — one effects
            // implementation. Run the stack ALWAYS for OE (not gated by the
            // opt-in toggle) so the look the user configured is preserved.
            unsigned int demod = 0;
            {
                // demodMutex: `signal()` hands the shader a raw pointer into
                // signalBuf, which the AI control server's /screen handler
                // rewrites via display->render() on its own thread. Without
                // the lock the upload can read a half-rewritten field (one
                // torn frame). uploadScreenTexture() released the lock
                // before returning, so this is a fresh acquisition, not a
                // nesting; scoped tight so the CRT stack below runs unlocked.
                std::lock_guard<std::mutex> demodLk(display->demodMutex());
                demod = ntscFx->process(
                    display->signal(),
                    display->signalWidth(),
                    display->signalHeight(),
                    display->signalPhaseOffset());
            }
            if (demod != 0) {
                presentTex = demod;
                voxelSrcTex = demod;   // colour image, pre-CRT-glass (3D tap)
                // CRT glass only when the master toggle is on (top of the CRT
                // Settings window); otherwise present the raw demod output.
                if (crtEffectsEnabled) {
                    if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
                    if (!crtFx->available()) crtFx->initialize();
                    if (crtFx->available()) {
                        // The OE demod shader already applied hue + chroma-
                        // bandwidth sharpness; zero hue and neutralise sharpness
                        // here so the shared stack doesn't rotate the chroma or
                        // sharpen a second time (0.5 = the stack's neutral pass).
                        pom2::NtscParams crtP = ntscFx->getParams();
                        crtP.hue       = 0.0f;
                        crtP.sharpness = 0.5f;
                        crtFx->setParams(crtP);
                        const unsigned int out = crtFx->process(
                            demod, ntscFx->outputWidth(), ntscFx->outputHeight(),
                            dstW, dstH);
                        if (out != 0) presentTex = out;
                    }
                }
            }
        }
    }

    // OE CPU demod writes frame80 → screenTexture; the OE-GPU fallbacks
    // (mixed graphics+text frame, sharp text, shader unavailable, demod
    // failure) also still present screenTexture at this point. Give ALL of
    // them the same CRT glass — gating on oeCpuMode alone made scanlines /
    // mask / persistence vanish on exactly the mixed frames (score bands,
    // menus, BASIC) and pop back on full-screen graphics, with a stale-
    // persistence ghost on re-entry. `presentTex == screenTexture` is
    // precisely "no OE branch above produced a processed texture".
    if (oeFamily && crtEffectsEnabled && presentTex == screenTexture) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // Neutralise the demod-stage knobs, same as the GPU branch above:
            // the OE-CPU demod (and the mixed-frame CPU demod band) now
            // applies hue + chroma-bandwidth sharpness itself via
            // setOeDemodParams, so the stack must not rotate the chroma or
            // sharpen a second time. (The only frames reaching here without
            // a demod are crisp B/W text — hue is a no-op on grays — and the
            // shader-unavailable LUT fallback, a documented degraded path.)
            pom2::NtscParams crtP = ntscFx ? ntscFx->getParams() : pom2::NtscParams{};
            crtP.hue       = 0.0f;
            crtP.sharpness = 0.5f;
            crtFx->setParams(crtP);
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // Universal CRT effect stack (Phase 3): for every NON-OE colour mode,
    // run the framebuffer through the shared scanline / mask / barrel /
    // persistence / BCS pass so those effects work on Color NTSC, Mono,
    // Chat Mauve and AppleWin too. OE GPU and OE CPU share one CRT branch
    // (demod hue/sharpness applied by their demod stage — neutralised there).
    if (!oeFamily && crtEffectsEnabled) {
        if (!crtFx) crtFx = std::make_unique<pom2::CrtEffectStack>();
        if (!crtFx->available()) crtFx->initialize();
        if (crtFx->available()) {
            // One CRT Settings panel drives both processors: mirror the
            // NtscParams the user edits there (the demod-only knobs are
            // ignored by the effect stack).
            if (ntscFx) crtFx->setParams(ntscFx->getParams());
            const unsigned int out = crtFx->process(
                presentTex, display->width(), display->height(), dstW, dstH);
            if (out != 0) presentTex = out;
        }
    }

    // 3D voxel view: rebuild the decoded COLOUR image (`voxelSrcTex`, the tap
    // taken before CrtEffectStack — so the cubes never inherit scanlines / mask
    // / barrel) as an upright 4:3 slab of equal-depth cubes (MicroM8 "Voxel
    // Cube"), viewed by the orbit camera. Renders at the on-screen size so the
    // aspect is exact; replaces the flat blit (CRT glass computed above is
    // discarded when the 3D view wins). Falls back to the flat texture if the
    // GL renderer can't initialise.
    if (show(pom2::PanelId::Voxel)) {
        if (!voxel3d_) voxel3d_ = std::make_unique<pom2::Voxel3DRenderer>();
        // One voxel per live Apple II pixel (280 or 560 × 192) so the cube grid
        // captures the full image — half-res sampling visibly lost detail.
        voxel3d_->gridW = std::max(1, display->width());
        voxel3d_->gridH = std::max(1, display->height());
#if defined(__EMSCRIPTEN__)
        // Perf guard: halve the 560-wide DHGR/80-col geometry on the browser so
        // the cube count stays near the comfortable HGR 280×192 (~54k); the FBO
        // supersample is likewise capped in Voxel3DRenderer::process.
        if (voxel3d_->gridW > 280) voxel3d_->gridW = 280;
#endif
        const int vw = std::max(16, static_cast<int>(size.x));
        const int vh = std::max(16, static_cast<int>(size.y));
        const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
        const unsigned int out =
            voxel3d_->process(voxelSrcTex, vw, vh, voxelCam_.viewProj(aspect));
        if (out != 0) presentTex = out;
    }

    // Scale to the content region, then centre (letterbox on a wider/taller
    // region, e.g. a kiosk viewport). `avail` / `size` were computed up-front
    // (above) so the CRT effect pass could render at this exact resolution.
    // The 280×192 active area drove a 4:3 CRT, so its pixels are not square —
    // three presentation modes:
    //   Square  — 1:1 logical pixels (280:192 ≈ 1.46); crisp; never < 1×.
    //   Crt43   — stretch the active area to a true 4:3 frame (real-monitor
    //             shape); fills the region, letterboxed.
    //   Integer — Square snapped to an integer multiple (no fractional-scale
    //             shimmer); never < 1×.
    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
        cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

    ImGui::Image(static_cast<ImTextureID>(presentTex), size);
    // Capture the screen widget's screen-space rect so the GLFW
    // cursor-pos callback (Phase 5) can map a host position onto Apple
    // pixels. The rect answers "*where* in the screen", never "is the
    // screen the one being pointed at" — see screenHovered_ below.
    screenRectMin = ImGui::GetItemRectMin();
    screenRectMax = ImGui::GetItemRectMax();
    // ...and ImGui's own z-order aware verdict on whether the pointer is
    // actually on the screen widget, which is what decides *ownership* of a
    // click (mouseGrabContext). Unlike the rect, this is false while a
    // dropdown, popup or panel is drawn over the screen, so a click aimed at
    // an open menu no longer doubles as a click into the guest — nor as the
    // capturing press that would steal the pointer behind that menu.
    // Recomputed every frame; renderFrame clears it first so a collapsed or
    // hidden screen window cannot leave a stale `true` behind.
    const bool screenHovered = ImGui::IsItemHovered();
    screenHovered_ = screenHovered;

    // 3D voxel view camera: left-drag orbits, middle-drag strafes (pan),
    // wheel zooms (MicroM8-style). All reference the Image item above
    // (IsItemHovered), so this must stay right after it. Mutates the
    // persistent `voxelCam_` the renderer reads.
    if (show(pom2::PanelId::Voxel) && screenHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            voxelCam_.azimuth   += d.x * 0.008f;
            voxelCam_.elevation += d.y * 0.008f;
            const float lim = 1.5f;   // ~86°: stay off the lookAt up-vector poles
            voxelCam_.elevation = std::clamp(voxelCam_.elevation, -lim, lim);
        }
        // Middle-drag = pan/strafe. Scale to world-units-per-pixel at the
        // target plane so the grab tracks the cursor 1:1, and grab the scene
        // (drag right → scene follows right → camera slides left).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            const float k = 2.0f * voxelCam_.distance *
                            std::tan(voxelCam_.fovY * 0.5f) /
                            std::max(1.0f, size.y);
            voxelCam_.pan(-d.x * k, d.y * k);
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            voxelCam_.distance =
                std::clamp(voxelCam_.distance * std::pow(0.9f, wheel), 0.6f, 20.0f);
    }

}

// The Apple II screen carries NO capture caption. It used to carry two — a
// "Click to capture the mouse" hint and a how-to-get-out reminder — and both
// existed to paper over the click-to-grab contract that is now gone: a click
// that silently changed what the mouse did had to announce itself, and a user
// who got captured by accident had to be told the way out.
//
// Neither problem exists any more. Capture is entered only by Ctrl+Alt+G or a
// middle click, and each of those is also the way out, so anyone captured got
// there deliberately and already knows the gesture. The standing reminder is
// the status-bar GRAB chip (with a long-lived hint beside it); painting over
// the emulated screen to say the same thing is exactly the clutter the chip
// exists to avoid.

void MainWindow::renderKiosk()
{
    // Chrome-free full-viewport window: just the Apple II screen, centred
    // and letterboxed on a black background. No title bar, no resize, no
    // background decoration — the OS window is already full-screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##kiosk", nullptr, flags)) {
        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ─── Kiosk disk selector (gamepad-driven) ───────────────────────────────
//
// A keyboard-free way to flip disks in kiosk mode: Start opens a list of the
// 5.25" images sitting in the same folder as the booted disk (so a game's
// "Side B" is one press away), D-pad/stick move, A mounts the highlighted one
// into the boot Disk II drive (slot 6, drive 1) without rebooting — the
// flip-disk gesture Wings of Fury and friends expect — and B/Start dismiss.

DiskIICard* MainWindow::kioskBootDiskCard()
{
    // Prefer the conventional boot slot 6; fall back to the primary card.
    for (auto* c : diskIICards()) if (c && c->getSlot() == 6) return c;
    return primaryDiskII();
}

void MainWindow::openKioskStartMenu()
{
    kioskMenuOpen_ = true;
    kioskPage_     = KioskPage::List;
    kioskZone_     = KioskZone::Games;
    kioskActSel_   = 0;
    kioskRescanDisks();
}

// Rebuild the GAMES list from the booted disk's folder + the extra ROM
// folders. Split from openKioskStartMenu so the RomDirs page can refresh the
// list on its way back — otherwise a folder added/removed there is invisible
// until the menu is closed and reopened.
void MainWindow::kioskRescanDisks()
{
    kioskDiskPaths_.clear();
    kioskDiskLabels_.clear();
    kioskDiskSel_ = 0;
    kioskStatus_.clear();

    namespace fs = std::filesystem;
    std::error_code ec;

    DiskIICard* boot = kioskBootDiskCard();
    // Copied UNDER the lock. getDiskPath() hands back a reference into live
    // DiskImage state, and the AI control server's HTTP thread reassigns that
    // very string on /disk and /eject — copy-constructing from it while its
    // heap buffer is being freed is a garbage path at best and a segfault at
    // worst. renderStatusBar already snapshots for exactly this reason.
    std::string cur;
    if (boot) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        cur = boot->getDiskPath(0);
    }

    // Scan the booted disk's own folder PLUS every configured extra ROM
    // folder. Unlike the old build we do NOT filter out unrelated titles:
    // we keep every mountable 5.25" image and SORT by name-proximity so the
    // current title's other sides float to the top while
    // the rest of the collection stays reachable below.
    std::vector<fs::path> dirs;
    auto addDir = [&](const fs::path& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        const fs::path norm = fs::weakly_canonical(d, ec);
        const fs::path key   = ec ? d : norm;
        for (const auto& e : dirs) if (e == key) return;   // dedup
        dirs.push_back(key);
    };
    if (!cur.empty()) addDir(fs::path(cur).parent_path());
    for (const auto& d : kioskRomDirs_) addDir(fs::path(d));

    if (dirs.empty()) {
        kioskStatus_ = boot ? "No disk folder to browse — add one via ROM folders"
                            : "No Disk II card in this config";
        return;
    }

    auto toLower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    };
    auto commonPrefix = [](const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        size_t i = 0;
        while (i < n && a[i] == b[i]) ++i;
        return i;
    };
    // A candidate is a "sibling" of the mounted disk when its stem shares a
    // long common prefix (≥6 chars, ≥half the shorter stem) — the same title's
    // other sides ("… (Side A)" ↔ "… (Side B)"), not every disk in the folder.
    const std::string curName = cur.empty() ? std::string{}
                                            : fs::path(cur).filename().string();
    const std::string curKey  = cur.empty() ? std::string{}
                                            : toLower(fs::path(cur).stem().string());
    auto isSibling = [&](const fs::path& p) {
        if (curKey.empty()) return false;
        const std::string k = toLower(p.stem().string());
        if (k == curKey) return true;
        const size_t pref   = commonPrefix(curKey, k);
        const size_t minLen = std::min(curKey.size(), k.size());
        return pref >= 6 && pref * 2 >= minLen;
    };

    // Accept every image the launcher can route — 5.25", 800K 3.5" and HDV —
    // not just floppies. A 5.25" disk is hot-swapped in place (flip-disk); a
    // 3.5"/HDV is mounted + booted through insertAndBootImage on activation.
    std::vector<fs::path> found;
    for (const auto& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (classifyDiskForSlot(it->path().string()) == DiskSlotClass::Unknown)
                continue;
            found.push_back(it->path());
        }
    }
    // Sort: siblings of the mounted disk first, then alphabetical by filename.
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
        const bool sa = isSibling(a), sb = isSibling(b);
        if (sa != sb) return sa;
        return a.filename().string() < b.filename().string();
    });

    // Mark the disk currently in the boot drive so the list shows a ● next to
    // it and the cursor lands on it. Match canonically: the mounted path may
    // be relative (kiosk launched as `POM2 games/foo.dsk`) while scanned
    // entries come out of canonicalized dirs.
    std::error_code ecCur;
    const fs::path curCanon = cur.empty()
        ? fs::path{} : fs::weakly_canonical(fs::path(cur), ecCur);
    kioskMountedPath_ = cur;
    for (const auto& p : found) {
        const std::string name = p.filename().string();
        const bool isMounted = !cur.empty() &&
            (p.string() == cur ||
             (!ecCur && !curCanon.empty() && p == curCanon));
        if (isMounted) {
            kioskDiskSel_     = int(kioskDiskPaths_.size());
            // Adopt the scanned spelling so the render loop's exact string
            // compare against kioskDiskPaths_ draws the ● marker.
            kioskMountedPath_ = p.string();
        }
        kioskDiskPaths_.push_back(p.string());
        kioskDiskLabels_.push_back(name);
    }

    if (kioskDiskPaths_.empty())
        kioskStatus_ = "No disks found in the scanned folder(s)";

    pom2::log().info("Kiosk", "disk scan: " +
                     std::to_string(kioskDiskPaths_.size()) + " disk(s) across " +
                     std::to_string(dirs.size()) + " folder(s)");
}

void MainWindow::kioskMountSelected()
{
    if (kioskDiskSel_ < 0 || kioskDiskSel_ >= int(kioskDiskPaths_.size())) return;

    const std::string path  = kioskDiskPaths_[kioskDiskSel_];
    const std::string label = kioskDiskLabels_[kioskDiskSel_];

    // 3.5" and HDV volumes are boot media, not swap-in-place floppies: route
    // them into the right card and cold-boot straight away (like the CLI
    // launcher). 5.25" keeps the flip-disk gesture: hot-swap, no reboot.
    if (classifyDiskForSlot(path) != DiskSlotClass::Floppy525) {
        kioskSetPaused(false);          // let the worker run for the boot
        std::string err;
        if (insertAndBootImage(path, err)) {
            kioskMountedPath_ = path;
            kioskMenuOpen_ = false;     // booted → back to the game
        } else {
            kioskStatus_ = "Boot failed: " + err;
        }
        return;
    }

    DiskIICard* boot = kioskBootDiskCard();
    if (!boot) { kioskStatus_ = "No Disk II card in this config"; return; }

    // Two-phase: the file read happens here, unlocked; MediaMount takes the
    // lock only to swap the finished image in. In kiosk the window has no
    // other affordance, so a stall would look exactly like a hang.
    std::string mountErr;
    const bool ok = pom2::mountDiskII(*controller, *boot, 0, path, mountErr);
    if (ok) {
        // Keep the menu open so the user can chain a Restart (reboot on the
        // just-mounted disk) without reopening; B / Start dismisses it.
        kioskMountedPath_ = path;
        kioskStatus_ = "Mounted " + label + " — pick RESTART to reboot on it";
    } else {
        kioskStatus_ = "Mount failed: " + boot->getLastError(0);
    }
}

void MainWindow::kioskActivateFocused()
{
    // GAMES zone → mount the highlighted disk in place (no reboot).
    if (kioskZone_ == KioskZone::Games) {
        kioskMountSelected();
        return;
    }

    // ACTIONS zone → 0 Restart · 1 Keyboard · 2 ROM folders ·
    //                3 Exit kiosk · 4 Quit.
    switch (kioskActSel_) {
        case 0: {   // Restart — reboot on whatever disk is now in the drive
            DiskIICard* boot = kioskBootDiskCard();
            kioskSetPaused(false);          // let the worker run for the boot
            if (boot) controller->bootFromSlot(boot->getSlot());
            else      controller->coldBoot();
            controller->setMode(EmulationController::Mode::Running);
            kioskMenuOpen_ = false;
            break;
        }
        case 1:     // Keyboard band — live keys to the running game
            kioskPage_   = KioskPage::Keys;
            kioskKeySel_ = 0;
            kioskSetPaused(false);          // game keeps running under the band
            break;
        case 2:     // ROM folders manager
            if (kioskPruneRomDirs()) kioskSaveRomDirs();
            kioskRomDirSel_ = 0;
            kioskPage_      = KioskPage::RomDirs;
            break;
        case 3:     // Back to the windowed GUI — machine keeps running
                    // (setKioskModeRuntime closes the menu and un-pauses).
            setKioskModeRuntime(false);
            break;
        case 4:     // Quit — ask for confirmation first
            kioskPage_ = KioskPage::Quit;
            break;
    }
}

// Apple II key grid for the SELECT band. Each cell is an ASCII code the
// running program reads straight from the keyboard latch (queueKey), so no
// make/break bookkeeping is needed (unlike a scancode make/break scheme). Arrows use
// the II's control codes (←=$08 →=$15 ↑=$0B ↓=$0A).
namespace {
struct KioskKey { const char* label; uint8_t ascii; };
const KioskKey kKioskKeys[] = {
    {"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},
    {"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},{"0",'0'},          // row 0 (10)
    {"Q",'Q'},{"W",'W'},{"E",'E'},{"R",'R'},{"T",'T'},
    {"Y",'Y'},{"U",'U'},{"I",'I'},{"O",'O'},{"P",'P'},          // row 1 (10)
    {"A",'A'},{"S",'S'},{"D",'D'},{"F",'F'},{"G",'G'},
    {"H",'H'},{"J",'J'},{"K",'K'},{"L",'L'},                    // row 2 (9)
    {"Z",'Z'},{"X",'X'},{"C",'C'},{"V",'V'},{"B",'B'},
    {"N",'N'},{"M",'M'},                                        // row 3 (7)
    {"SPACE",' '},{"RET",0x0D},{"ESC",0x1B},
    {"\xe2\x86\x90",0x08},{"\xe2\x86\x91",0x0B},
    {"\xe2\x86\x93",0x0A},{"\xe2\x86\x92",0x15},                // row 4 (7)
};
constexpr int kKioskKeyCount = int(sizeof(kKioskKeys) / sizeof(kKioskKeys[0]));
// Row start/end (half-open) indices — mirrors the layout above.
const int kKioskKeyRows[][2] = { {0,10}, {10,20}, {20,29}, {29,36}, {36,43} };
constexpr int kKioskKeyRowN = int(sizeof(kKioskKeyRows) / sizeof(kKioskKeyRows[0]));
}  // namespace

void MainWindow::kioskInjectSelectedKey()
{
    if (kioskKeySel_ < 0 || kioskKeySel_ >= kKioskKeyCount) return;
    // The band leaves the machine running, so the latch is read live by the
    // program. queueKey masks to 7 bits and sets the strobe like the hardware.
    controller->memory().queueKey(kKioskKeys[kioskKeySel_].ascii);
    kioskStatus_ = std::string("Sent ") + kKioskKeys[kioskKeySel_].label;
}

void MainWindow::kioskSetPaused(bool want)
{
    if (want == kioskPausedByMenu_) return;
    if (want) {
        // Remember whether the machine was ALREADY stopped (user paused it
        // from the GUI toolbar before entering kiosk, or it never started).
        // Without this, closing the menu resumed a machine the user had
        // deliberately paused — the menu's pause is not ours to undo when
        // it was a no-op in the first place.
        kioskPauseWasAlreadyStopped_ =
            controller->getMode() != EmulationController::Mode::Running;
    } else if (kioskPauseWasAlreadyStopped_) {
        kioskPausedByMenu_ = false;      // give the pause back to the user
        return;
    }
    if (!want) {
        // Resuming: while Stopped, the audio thread kept advancing the
        // speaker's reconstruction cursor over silence, so it now sits far
        // ahead of the (frozen) production. Flush it — otherwise the catch-up
        // logic would swallow the game's first sounds for ~the pause duration.
        controller->speaker().reset();
    }
    controller->setMode(want ? EmulationController::Mode::Stopped
                             : EmulationController::Mode::Running);
    kioskPausedByMenu_ = want;
}

void MainWindow::updateKioskMenu()
{
    // Load the persisted extra ROM folders once (feeds the disk scan below).
    if (!kioskRomDirsLoaded_) { kioskLoadRomDirs(); kioskRomDirsLoaded_ = true; }

    // The pad was already polled this frame (pollJoystickAndPushToMemory).
    const JoystickInput::UiNav nav = joystick->uiNav();

    // Keyboard fallbacks work even when the controller isn't a recognized
    // GLFW gamepad (so the gamepad-mapped buttons never fire). They mirror
    // the pad: F1/Start opens the Start menu, K/Select the keyboard band,
    // arrows move, Enter validates, Esc goes back.
    //
    // F1, NOT F10: F10 is the global full-screen ⇄ windowed toggle, so
    // using it here meant entering kiosk ALSO opened this menu in the same
    // frame (onKey runs during glfwPollEvents, before render) — the user
    // asked for the game to go full-screen, not for a menu.
    const bool eStart   = nav.menu    || ImGui::IsKeyPressed(ImGuiKey_F1,     false);
    const bool eSelect  = nav.select  || ImGui::IsKeyPressed(ImGuiKey_K,      false);
    const bool eConfirm = nav.confirm || ImGui::IsKeyPressed(ImGuiKey_Enter,  false);
    const bool eCancel  = nav.cancel  || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    // Left/right zone-swap is a one-shot edge (never auto-repeats).
    const bool eLeft    = nav.left    || ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
    const bool eRight   = nav.right   || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);

    // ── SELECT: open/close the keyboard band directly, even mid-game ─────
    // (Back/Select toggles the live keyboard without pausing).
    if (eSelect) {
        if (kioskMenuOpen_ && kioskPage_ == KioskPage::Keys) {
            kioskMenuOpen_ = false;
        } else {
            kioskMenuOpen_ = true;
            kioskPage_     = KioskPage::Keys;
            kioskKeySel_   = 0;
            kioskStatus_.clear();
        }
    }

    // ── START: open/close the Start menu ────────────────────────────────
    if (eStart) {
        if (kioskMenuOpen_ && kioskPage_ != KioskPage::Keys) kioskMenuOpen_ = false;
        else openKioskStartMenu();
    }

    // Pause the machine on every Start-menu page, but NOT the keyboard band
    // (its keys must reach a running game). Closed menu → running.
    const bool wantPause = kioskMenuOpen_ && kioskPage_ != KioskPage::Keys;
    kioskSetPaused(wantPause);
    // Re-park if something else resumed the worker behind the open menu
    // (e.g. an F6 hold released across the menu-open frame ends in
    // rewindEndAndResume → Mode::Running); kioskSetPaused alone early-outs
    // because kioskPausedByMenu_ still says "paused".
    if (wantPause && controller->getMode() != EmulationController::Mode::Stopped)
        controller->setMode(EmulationController::Mode::Stopped);

    if (!kioskMenuOpen_) return;

    // ── Temporal auto-repeat for held directions (400ms delay, 150ms rate) ──
    // The paused loop runs unthrottled, so a per-frame step would be
    // unaimable; gate held-direction steps on the wall clock instead. Edge
    // presses from the keyboard already single-step via IsKeyPressed below,
    // so `step` covers the *held* pad/keys case only.
    const bool upHeld   = nav.upHeld   || ImGui::IsKeyDown(ImGuiKey_UpArrow);
    const bool downHeld = nav.downHeld || ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool leftHeld = nav.leftHeld || ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const bool rightHeld= nav.rightHeld|| ImGui::IsKeyDown(ImGuiKey_RightArrow);
    const bool pgUpHeld = nav.pageUpHeld  || ImGui::IsKeyDown(ImGuiKey_PageUp);
    const bool pgDnHeld = nav.pageDownHeld|| ImGui::IsKeyDown(ImGuiKey_PageDown);
    const bool navHeld  = upHeld || downHeld || leftHeld || rightHeld ||
                          pgUpHeld || pgDnHeld;
    const double tNow = ImGui::GetTime();
    bool step = false;
    if (navHeld) {
        if (!kioskNavHeld_) { step = true; kioskNavHeld_ = true; kioskNavNextT_ = tNow + 0.40; }
        else if (tNow >= kioskNavNextT_) { step = true; kioskNavNextT_ = tNow + 0.15; }
    } else {
        kioskNavHeld_ = false;
    }
    // Resolve directional intents: a fresh keyboard edge OR a repeat `step`.
    const bool up    = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   false) || (step && upHeld);
    const bool down  = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || (step && downHeld);
    const bool left  = step && leftHeld;   // (edge handled separately below)
    const bool right = step && rightHeld;
    const bool pgUp  = ImGui::IsKeyPressed(ImGuiKey_PageUp,   false) || (step && pgUpHeld);
    const bool pgDn  = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || (step && pgDnHeld);

    switch (kioskPage_) {

    case KioskPage::Quit:
        if (eConfirm) { if (window) glfwSetWindowShouldClose(window, 1); kioskMenuOpen_ = false; }
        if (eCancel)  kioskPage_ = KioskPage::List;
        break;

    case KioskPage::Keys: {
        // 2D grid: up/down change row (clamp column), left/right within row.
        if (up || down) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            int col = kioskKeySel_ - kKioskKeyRows[row][0];
            row = (row + (down ? 1 : -1) + kKioskKeyRowN) % kKioskKeyRowN;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            if (col >= len) col = len - 1;
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (left || right || eLeft || eRight) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            int col = kioskKeySel_ - kKioskKeyRows[row][0] + ((right || eRight) ? 1 : -1);
            col = std::max(0, std::min(len - 1, col));
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (eConfirm) kioskInjectSelectedKey();   // send key, band stays open
        if (eCancel)  kioskMenuOpen_ = false;      // (B) close → resume game
        break;
    }

    case KioskPage::RomDirs: {
        const int total = 1 + int(kioskRomDirs_.size());   // [0]=ADD, [1..]=folders
        if (up || down) {
            kioskRomDirSel_ += down ? 1 : -1;
            kioskRomDirSel_ = (kioskRomDirSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskRomDirSel_ == 0) {                     // + ADD → browser
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path start = (!kioskRomDirs_.empty() &&
                                  fs::is_directory(kioskRomDirs_.back(), ec))
                                     ? fs::path(kioskRomDirs_.back())
                                     : fs::current_path(ec);
                fs::path abs = fs::absolute(start, ec);
                kioskBrowseDir_ = (ec ? start : abs).lexically_normal().string();
                kioskComputeShortcuts();
                kioskScanBrowse(kioskBrowseDir_);
                kioskPage_ = KioskPage::Browse;
            } else {                                        // remove this folder
                const int idx = kioskRomDirSel_ - 1;
                if (idx >= 0 && idx < int(kioskRomDirs_.size()))
                    kioskRomDirs_.erase(kioskRomDirs_.begin() + idx);
                kioskSaveRomDirs();
                if (kioskRomDirSel_ > int(kioskRomDirs_.size()))
                    kioskRomDirSel_ = int(kioskRomDirs_.size());
            }
        }
        if (eCancel) {
            kioskPage_ = KioskPage::List;
            kioskZone_ = KioskZone::Actions;
            kioskRescanDisks();   // pick up folders added/removed just now
        }
        break;
    }

    case KioskPage::Browse: {
        namespace fs = std::filesystem;
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        if (up || down) {
            kioskBrowseSel_ += down ? 1 : -1;
            kioskBrowseSel_ = (kioskBrowseSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskBrowseSel_ == 0) {                     // USE THIS FOLDER
                if (std::find(kioskRomDirs_.begin(), kioskRomDirs_.end(), kioskBrowseDir_)
                        == kioskRomDirs_.end())
                    kioskRomDirs_.push_back(kioskBrowseDir_);
                kioskSaveRomDirs();
                kioskRomDirSel_ = 0;
                kioskPage_ = KioskPage::RomDirs;
            } else if (kioskBrowseSel_ == 1) {              // .. parent
                const fs::path p(kioskBrowseDir_);
                if (p.has_parent_path() && p.parent_path() != p)
                    kioskBrowseDir_ = p.parent_path().string();
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else if (kioskBrowseSel_ < 2 + nShort) {      // shortcut
                kioskBrowseDir_ = kioskBrowseShortcutPaths_[kioskBrowseSel_ - 2];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else {                                        // descend
                kioskBrowseDir_ = kioskBrowseSubdirs_[kioskBrowseSel_ - 2 - nShort];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            }
        }
        if (eCancel) kioskPage_ = KioskPage::RomDirs;
        break;
    }

    case KioskPage::List: default: {
        const int nd = int(kioskDiskPaths_.size());
        // LEFT/RIGHT (edge) swaps focus between the GAMES and ACTIONS zones.
        if (eLeft || eRight)
            kioskZone_ = (kioskZone_ == KioskZone::Games) ? KioskZone::Actions
                                                          : KioskZone::Games;
        if (kioskZone_ == KioskZone::Games) {
            if (nd > 0) {
                constexpr int kPage = 10;   // L1/R1 fast jump size
                int delta = 0;
                if      (down) delta =  1;
                else if (up)   delta = -1;
                else if (pgDn) delta =  kPage;
                else if (pgUp) delta = -kPage;
                if (delta != 0) {
                    kioskDiskSel_ += delta;
                    if (delta == 1 || delta == -1)          // step: wrap
                        kioskDiskSel_ = (kioskDiskSel_ % nd + nd) % nd;
                    else                                    // page jump: clamp
                        kioskDiskSel_ = std::max(0, std::min(nd - 1, kioskDiskSel_));
                }
            }
        } else if (up || down) {                            // ACTIONS column
            kioskActSel_ += down ? 1 : -1;
            kioskActSel_ = (kioskActSel_ % kKioskActionCount + kKioskActionCount)
                           % kKioskActionCount;
        }
        if (eConfirm) kioskActivateFocused();
        if (eCancel)  kioskMenuOpen_ = false;               // (B) resume game
        break;
    }
    }

    // A close on any page must let the machine run again.
    if (!kioskMenuOpen_) kioskSetPaused(false);
}

void MainWindow::renderKioskMenu()
{
    if (!kioskMenuOpen_) return;
    namespace fs = std::filesystem;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 disp = vp->Size;
    const bool keysPage = (kioskPage_ == KioskPage::Keys);

    const ImVec4 kYellow(1.0f, 0.85f, 0.30f, 1.0f);
    const ImVec4 kGreen (0.47f, 0.90f, 0.47f, 1.0f);
    const ImVec4 kDim   (0.50f, 0.50f, 0.50f, 1.0f);
    const ImVec4 kGrey  (0.60f, 0.60f, 0.60f, 1.0f);

    // Full-screen dim veil behind every page EXCEPT the keyboard band (there
    // the game must stay visible so you see keys land).
    if (!keysPage) {
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(disp);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##kioskVeil", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();
    }

    // Geometry: centered panel for full-screen pages, bottom band for keys.
    if (keysPage) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.98f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.66f, disp.y * 0.34f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.82f, disp.y * 0.80f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, 245));
    }
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(220, 200, 80, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    // The kiosk screen is a full-viewport OPAQUE window — force the menu to
    // the front every frame or it renders hidden behind the black background.
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavInputs;

    ImGui::Begin("##kioskMenu", nullptr, flags);

    auto rowText = [&](bool sel, const ImVec4& col, const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[512]; std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s %s", sel ? "\xe2\x96\xb6" : "  ", buf);
        if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
    };

    // ================= LIST — two zones (games / actions) ================
    if (kioskPage_ == KioskPage::List) {
        const int nd = int(kioskDiskLabels_.size());
        const bool zGames = (kioskZone_ == KioskZone::Games);

        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(kYellow, ICON_FA_GAMEPAD " MENU");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "\xe2\x97\x80\xe2\x96\xb6 switch zone   \xc2\xb7   "
                                  "up/down select   \xc2\xb7   L1/R1 fast   \xc2\xb7   "
                                  "A confirm   \xc2\xb7   B resume   \xc2\xb7   SELECT keyboard");
        ImGui::Separator();

        // Footer reserve = the 4 action rows @2.3 + a "disks found" line @1.3.
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float bf = ImGui::GetFontSize();
        // 5.0f = the five action rows @2.3 (Restart / Keyboard / ROM
        // folders / Exit kiosk / Quit). Reserving four clipped the last
        // one below the panel edge — the window is NoScrollbar and a
        // gamepad user cannot scroll to it.
        const float footer = 5.0f * (bf * 2.3f + sp) + (bf * 1.3f + sp) + (sp + 6.0f);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextColored(zGames ? kYellow : kDim,
                           zGames ? "\xe2\x96\xb6 " ICON_FA_COMPACT_DISC " GAMES"
                                  : "  " ICON_FA_COMPACT_DISC " GAMES");
        ImGui::BeginChild("##kdList", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), true);
        ImGui::SetWindowFontScale(2.6f);
        if (nd == 0)
            ImGui::TextColored(kDim, "(no disks — add a ROM folder)");
        for (int i = 0; i < nd; ++i) {
            const bool sel = (i == kioskDiskSel_);
            const ImVec4 col = zGames ? kGreen : kDim;
            // ● marks the disk currently in the boot drive (flip-disk anchor).
            const bool mounted = !kioskMountedPath_.empty() &&
                                 kioskDiskPaths_[i] == kioskMountedPath_;
            rowText(sel, col, "%s%s", mounted ? ICON_FA_COMPACT_DISC " " : "",
                    kioskDiskLabels_[i].c_str());
        }
        ImGui::EndChild();

        // Actions column.
        ImGui::SetWindowFontScale(2.3f);
        const bool zAct = (kioskZone_ == KioskZone::Actions);
        auto actionRow = [&](int idx, const ImVec4& col, const char* label) {
            const bool sel = (kioskActSel_ == idx);
            ImGui::PushStyleColor(ImGuiCol_Text, (sel && zAct) ? kGreen : (zAct ? col : kDim));
            ImGui::Text("%s %s", (sel && zAct) ? "\xe2\x96\xb6" : "  ", label);
            ImGui::PopStyleColor();
        };
        actionRow(0, ImVec4(1.0f, 0.60f, 0.15f, 1.0f), ICON_FA_ROTATE " RESTART MACHINE");
        actionRow(1, ImVec4(0.55f, 0.80f, 1.0f, 1.0f), ICON_FA_KEYBOARD " KEYBOARD");
        actionRow(2, ImVec4(0.60f, 0.95f, 0.60f, 1.0f), ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        // Exit to the windowed GUI. Discoverable here because a kiosk user
        // has no menu bar and may not know about Ctrl+Alt+F / F10.
        actionRow(3, ImVec4(0.80f, 0.80f, 1.0f, 1.0f),
                  ICON_FA_COMPRESS " EXIT KIOSK (WINDOWED)");
        actionRow(4, ImVec4(1.0f, 0.50f, 0.40f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT");

        ImGui::SetWindowFontScale(1.3f);
        ImGui::Separator();
        ImGui::TextColored(kYellow, ICON_FA_COMPACT_DISC " Disks found: %d", nd);
        if (!kioskStatus_.empty()) ImGui::TextColored(kGrey, "%s", kioskStatus_.c_str());
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= KEYBOARD band (game runs underneath) ==============
    else if (kioskPage_ == KioskPage::Keys) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(kYellow, ICON_FA_KEYBOARD " KEYBOARD");
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "  A send  \xc2\xb7  B close  \xc2\xb7  game keeps running");
        ImGui::Separator();
        for (int r = 0; r < kKioskKeyRowN; ++r) {
            ImGui::SetWindowFontScale(2.2f);
            for (int i = kKioskKeyRows[r][0]; i < kKioskKeyRows[r][1]; ++i) {
                const bool sel = (i == kioskKeySel_);
                if (i > kKioskKeyRows[r][0]) ImGui::SameLine();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                char cell[24];
                std::snprintf(cell, sizeof cell, sel ? "[%s]" : " %s ", kKioskKeys[i].label);
                ImGui::TextUnformatted(cell);
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= QUIT confirmation =================================
    else if (kioskPage_ == KioskPage::Quit) {
        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT?");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 30));
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(kGreen, ICON_FA_POWER_OFF " (A) Yes, quit");
        ImGui::SetWindowFontScale(1.8f);
        ImGui::TextColored(kGrey, "(B) No, back to menu");
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= ROM folders manager ==============================
    else if (kioskPage_ == KioskPage::RomDirs) {
        ImGui::SetWindowFontScale(2.6f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A add / remove   \xc2\xb7   B back");
        ImGui::Separator();
        ImGui::BeginChild("##kRomDirs", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.2f);
        const int total = 1 + int(kioskRomDirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskRomDirSel_);
            if (i == 0) {
                rowText(sel, kGreen, ICON_FA_PLUS " [ ADD A FOLDER ]");
            } else {
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%s " ICON_FA_XMARK " %s", sel ? "\xe2\x96\xb6" : "  ",
                            kioskRomDirs_[i - 1].c_str());
                if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
            }
        }
        if (kioskRomDirs_.empty()) {
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(kDim, "   (only the booted disk's folder is scanned)");
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= Directory browser ================================
    else if (kioskPage_ == KioskPage::Browse) {
        ImGui::SetWindowFontScale(2.4f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " SELECT ROM FOLDER");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A enter / select   \xc2\xb7   B cancel");
        ImGui::Separator();
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", kioskBrowseDir_.c_str());
        ImGui::Separator();
        ImGui::BeginChild("##kBrowse", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.1f);
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskBrowseSel_);
            if (i == 0)                 rowText(sel, kGreen, ICON_FA_STAR " [ USE THIS FOLDER ]");
            else if (i == 1)            rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " ..");
            else if (i < 2 + nShort)    rowText(sel, kGreen, "%s", kioskBrowseShortcutLabels_[i - 2].c_str());
            else                        rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " %s",
                                                fs::path(kioskBrowseSubdirs_[i - 2 - nShort]).filename().string().c_str());
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(keysPage ? 1 : 2);
}

// ─── Kiosk ROM-folders manager + directory browser ──────────────────────

void MainWindow::kioskScanBrowse(const std::string& dir)
{
    namespace fs = std::filesystem;
    kioskBrowseSubdirs_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) kioskBrowseSubdirs_.push_back(it->path().string());
    }
    std::sort(kioskBrowseSubdirs_.begin(), kioskBrowseSubdirs_.end(),
              [](const std::string& a, const std::string& b) {
                  return fs::path(a).filename().string() < fs::path(b).filename().string();
              });
    kioskBrowseSel_ = 0;
}

void MainWindow::kioskComputeShortcuts()
{
    namespace fs = std::filesystem;
    kioskBrowseShortcutPaths_.clear();
    kioskBrowseShortcutLabels_.clear();
    auto add = [&](const std::string& path, const std::string& label) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return;
        if (std::find(kioskBrowseShortcutPaths_.begin(), kioskBrowseShortcutPaths_.end(),
                      path) != kioskBrowseShortcutPaths_.end()) return;   // dedup
        kioskBrowseShortcutPaths_.push_back(path);
        kioskBrowseShortcutLabels_.push_back(label);
    };
    add("/", std::string(ICON_FA_SERVER) + " / (filesystem root)");
    if (const char* home = std::getenv("HOME"))
        add(home, std::string(ICON_FA_FOLDER_OPEN) + " Home");
    // Removable-media mount points (guarded so each OS only shows what exists).
    const char* user = std::getenv("USER");
    std::vector<std::string> roots{ "/Volumes" };
    if (user) { roots.push_back(std::string("/run/media/") + user);
                roots.push_back(std::string("/media/") + user); }
    roots.push_back("/mnt");
    for (const auto& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2))
                add(it->path().string(),
                    std::string(ICON_FA_HARD_DRIVE) + " " + it->path().filename().string());
        }
    }
}

// Extra ROM folders persist OUTSIDE state.cfg (kiosk keeps its main config
// read-only), in a sibling "kiosk_romdirs.txt" — one absolute path per line.
static std::filesystem::path kioskRomDirsFile(const pom2::Settings& s)
{
    namespace fs = std::filesystem;
    fs::path store = s.getStorePath();
    fs::path dir = store.has_parent_path() ? store.parent_path() : fs::path(".");
    return dir / "kiosk_romdirs.txt";
}

void MainWindow::kioskLoadRomDirs()
{
    namespace fs = std::filesystem;
    kioskRomDirs_.clear();
    std::ifstream f(kioskRomDirsFile(*settings));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::error_code ec;
        if (!line.empty() && fs::is_directory(line, ec)) kioskRomDirs_.push_back(line);
    }
}

void MainWindow::kioskSaveRomDirs()
{
    std::ofstream f(kioskRomDirsFile(*settings), std::ios::trunc);
    if (!f) return;
    for (const auto& d : kioskRomDirs_) f << d << '\n';
}

bool MainWindow::kioskPruneRomDirs()
{
    namespace fs = std::filesystem;
    bool changed = false;
    for (size_t i = 0; i < kioskRomDirs_.size(); ) {
        std::error_code ec;
        if (!fs::is_directory(kioskRomDirs_[i], ec)) {
            kioskRomDirs_.erase(kioskRomDirs_.begin() + long(i));
            changed = true;
        } else ++i;
    }
    return changed;
}

// ─── Mouse Card input routing ───────────────────────────────────────────

// `MouseGrab.h` stays GLFW-free so a headless test can link it. Prove its
// mirrored tokens still match the real ones here, where <GLFW/glfw3.h> is
// in scope — an upstream renumbering becomes a compile error, not a chord
// that silently stops working.
static_assert(pom2::mousegrab::kKeyG         == GLFW_KEY_G);
static_assert(pom2::mousegrab::kModControl   == GLFW_MOD_CONTROL);
static_assert(pom2::mousegrab::kModAlt       == GLFW_MOD_ALT);
static_assert(pom2::mousegrab::kButtonLeft   == GLFW_MOUSE_BUTTON_LEFT);
static_assert(pom2::mousegrab::kButtonMiddle == GLFW_MOUSE_BUTTON_MIDDLE);

pom2::mousegrab::Context MainWindow::mouseGrabContext() const
{
    pom2::mousegrab::Context c;
    c.cardPlugged = mouseCoordinator_->capture().plugged();
    c.grabbed     = mouseGrabbed_;
    c.voxelView   = show(pom2::PanelId::Voxel);
    // Hover, NOT rect containment. `screenHovered_` is ImGui's own z-order
    // aware verdict, captured next to the screen Image (renderScreenWindow).
    // A raw "is the cursor between screenRectMin and screenRectMax" test
    // cannot see what is drawn on top: an open dropdown, a popup or a panel
    // docked over the screen all sit *inside* that rect, so every click the
    // user aimed at the menu also reached the Mouse Card — and, worse, armed
    // `shouldGrabOnPress` into capturing the pointer behind the menu.
    // The rect itself is still the right tool for the *coordinate* mapping
    // in onMouseMove; it is only wrong as an ownership test.
    c.screenHovered = screenHovered_;
    return c;
}

void MainWindow::setMouseGrab(bool on)
{
    if (on == mouseGrabbed_) return;
    if (on && !mouseCoordinator_->capture().plugged()) {
        // Capturing the pointer with nothing to feed would strand the user
        // in a hidden-cursor mode for no gain.
        tapeStatusMessage = "Mouse capture: no Mouse Card plugged "
                            "(Slot Configuration → mouse / mouseaw)";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    mouseGrabbed_ = on;

    if (window) {
        // GLFW_CURSOR_DISABLED hides the OS cursor AND unbounds it: the
        // reported position keeps accumulating past the window edges, which
        // is exactly the infinite-delta source a relative quadrature mouse
        // wants. GLFW restores the pre-grab cursor position on the way out.
        glfwSetInputMode(window, GLFW_CURSOR,
                         on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
#ifndef __EMSCRIPTEN__
        // Raw (unaccelerated, unscaled) motion while captured — the desktop's
        // pointer-acceleration curve is tuned for a screen-sized target area,
        // and it makes the guest cursor's speed depend on how fast the user
        // flicks. Only meaningful under GLFW_CURSOR_DISABLED. The browser has
        // no equivalent knob (pointer lock already delivers raw movementX/Y).
        if (glfwRawMouseMotionSupported()) {
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                             on ? GLFW_TRUE : GLFW_FALSE);
        }
#endif
    }

    // Take the mouse away from ImGui for the duration: under a captured
    // pointer io.MousePos tracks the virtual cursor, which would hover and
    // click panels the user cannot see. The GLFW backend already skips its
    // own cursor-shape updates while GLFW_CURSOR_DISABLED is set
    // (imgui_impl_glfw.cpp `ImGui_ImplGlfw_UpdateMouseCursor`), so the two
    // do not fight over the input mode.
    ImGuiIO& io = ImGui::GetIO();
    if (on) io.ConfigFlags |=  ImGuiConfigFlags_NoMouse;
    else    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    // Both edges warp the reported cursor position (into the virtual space on
    // entry, back to the real one on exit). Re-seed the delta baseline so the
    // first event after the transition doesn't inject that jump as motion,
    // and drop sub-pixel residue accumulated under the other regime.
    mouseInited    = false;
    mouseSubAppleX = 0.0;
    mouseSubAppleY = 0.0;

    // Never leave the guest holding a button it can no longer release.
    if (!on && mouseButtonHeld) {
        mouseButtonHeld = false;
        (void)mouseCoordinator_->routeHost(mouseAppleX, mouseAppleY, false);
    }

    tapeStatusMessage = on
        ? "Mouse captured — Ctrl+Alt+G or middle click to release"
        : "Mouse released";
    tapeStatusUntil     = lastFrameTime + (on ? 4.0 : 2.0);
    // The bar-side "how to get out" hint. 4 s was tuned for a caption
    // painted over the emulated screen, where it had to get out of the way
    // fast; in the status bar it costs only bar width, and the person who
    // needs it is the one still working out where their pointer went. Long
    // enough to notice, read and act on without it becoming furniture.
    mouseGrabHintUntil_ = on ? lastFrameTime + 30.0 : 0.0;
}

void MainWindow::toggleMouseGrab() { setMouseGrab(!mouseGrabbed_); }

void MainWindow::onWindowFocus(bool focused)
{
    if (!focused) setMouseGrab(false);
}

void MainWindow::onMouseMove(double x, double y)
{
    // First call after startup just seeds last-position; no delta yet.
    if (!mouseInited) {
        lastMouseHostX = x;
        lastMouseHostY = y;
        mouseInited = true;
        return;
    }
    const double rawDx = x - lastMouseHostX;
    const double rawDy = y - lastMouseHostY;
    lastMouseHostX = x;
    lastMouseHostY = y;

    // Either MAME-faithful MouseCard or AppleWin HLE MouseCardAppleWin can be
    // plugged (mutually exclusive). MouseCoordinator re-resolves both from the
    // live SlotBus under the machine lock, so the absolute / relative cursor
    // logic below stays variant-agnostic AND cannot write through a card that
    // a slot replacement has already destroyed.
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();
    // Card plugged + (pointer captured, or hovering the screen widget).
    // Uncaptured motion outside the widget belongs to ImGui — see MouseGrab.h.
    if (!pom2::mousegrab::shouldRouteMotion(grabCtx)) return;
    auto pushMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        (void)mouseCoordinator_->routeHost(rx, ry, btn);
    };
    // Need a valid Apple II Screen widget rect to map host pixels into
    // Apple-cursor coordinates. Bail until renderScreen has populated it.
    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    if (widgetW <= 0.0f || widgetH <= 0.0f) return;

    // ── Absolute closed-loop cursor sync (AppleWin HLE only) ───────────
    // When the `mouseaw` card is plugged AND the AppleMouse firmware has
    // been turned on (MODE_MOUSE_ON, bit 0 of the latched MODE byte), the
    // card's HLE'd MCU keeps the cursor position in `iX/iY` clamped to
    // the firmware-installed window `[iMinX..iMaxX] × [iMinY..iMaxY]`.
    // We read that authoritative state via the debug snapshot, project
    // the host cursor's position onto the widget rect (saturating clamp
    // outside, so wandering out of the widget pins the Apple cursor at
    // the matching edge instead of letting it drift), and inject the
    // delta needed to drive `iX/iY` toward the projected target. The
    // earlier closed-loop attempt (reverted in commit ccd9a95) failed
    // because it assumed the clamp range equalled the display resolution
    // — that's true for the //e desktop but wrong for e.g. MousePaint's
    // 0..559 horizontal clamp. Using the card-reported clamp window
    // sidesteps that guess entirely.
    // Each push is bounded to ±127 (the MCU's 8-bit signed wrap range);
    // large gaps (first event after re-entry, big window resize) converge
    // over several events.
    bool absoluteHandled = false;
    const auto mouseInventory = mouseCoordinator_->capture();
    if (mouseInventory.appleWinPlugged &&
        pom2::mousegrab::allowAbsoluteSync(grabCtx)) {
        const auto& s = mouseInventory.appleWin;
        const bool mouseOn = s.mouseOn();
        const int rangeX = s.iMaxX - s.iMinX;
        const int rangeY = s.iMaxY - s.iMinY;
        if (mouseOn && rangeX > 0 && rangeY > 0) {
            const double fracX = std::clamp(
                (x - double(screenRectMin.x)) / double(widgetW), 0.0, 1.0);
            const double fracY = std::clamp(
                (y - double(screenRectMin.y)) / double(widgetH), 0.0, 1.0);
            const int targetX = s.iMinX + int(fracX * rangeX + 0.5);
            const int targetY = s.iMinY + int(fracY * rangeY + 0.5);
            int dx = targetX - s.iX;
            int dy = targetY - s.iY;
            if (dx >  127) dx =  127;
            if (dx < -127) dx = -127;
            if (dy >  127) dy =  127;
            if (dy < -127) dy = -127;
            mouseAppleX = static_cast<uint8_t>(mouseAppleX + dx);
            mouseAppleY = static_cast<uint8_t>(mouseAppleY + dy);
            pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
            // Drop relative sub-pixel residue so a later fallback (mouse
            // turned off mid-session) doesn't replay stale fractional
            // motion accumulated before sync was active.
            mouseSubAppleX = 0.0;
            mouseSubAppleY = 0.0;
            absoluteHandled = true;
        }
    }
    if (absoluteHandled) return;

    // ── Relative drive (fallback, and the only path while captured) ──
    // Used by the MAME-faithful MouseCard (no iX/iY exposed — firmware
    // lives inside the 68705P3 MCU's internal RAM), by the AppleWin HLE
    // card before the firmware enables MOUSE_ON, and by BOTH whenever the
    // pointer is captured (a grabbed cursor has no meaningful position in
    // the widget, only deltas — see MouseGrab.h). The cursor-inside-widget
    // gate was already applied by `shouldRouteMotion` above.

    // ── Speed mapping (relative drive — the only path) ──────────────
    // The closed-loop absolute sync was an experiment that didn't survive
    // contact with real apps: the cursor's real clamp range lives behind the
    // firmware (the MCU on the //e card, the internal ROM on the //c) and the
    // app's ClampMouse parameters don't reliably land in 6502-readable holes
    // for MGTK-based apps (A2Desktop/MousePaint), so any absolute target was
    // guesswork. The proven proportional drive below — what AppleWin/MAME do
    // — gives no centre-jump and lets the app's own firmware clamp at its
    // own edges naturally.
    // Used when the AppleMouse firmware is off or its clamp window is
    // non-standard (holes out of display range). Convert host-pixel
    // deltas to Apple-cursor units so 1 host pixel of motion = 1 host
    // pixel of cursor motion visually in the widget.
    //   apple_per_host_px = logical_screen_dim / widget_host_dim
    // The widget is ALWAYS drawn at kWidth(280)×kHeight(192) aspect
    // (drawScreenImage), so the X mapping must use kWidth, NOT
    // display->width() — the latter returns 560 in DHGR/80-col, which made
    // X track 2× faster than Y in 80-column mode (where A2Desktop runs).
    // Both axes now share the same logical→widget scale. Sub-pixel motion
    // accumulates across events.
    const double sxRatio = double(Apple2Display::kWidth)  / double(widgetW);
    const double syRatio = double(Apple2Display::kHeight) / double(widgetH);
    mouseSubAppleX += rawDx * sxRatio;
    mouseSubAppleY += rawDy * syRatio;
    int dxApple = static_cast<int>(mouseSubAppleX);
    int dyApple = static_cast<int>(mouseSubAppleY);
    // Clamp BEFORE consuming the sub-pixel accumulator so big jumps
    // (>127 ticks in one event, e.g. cursor teleported across widget)
    // carry the residual forward to the next event instead of being
    // silently dropped. ±127 = MCU's 8-bit signed wrap-correction range.
    if (dxApple >  127) dxApple =  127;
    if (dxApple < -127) dxApple = -127;
    if (dyApple >  127) dyApple =  127;
    if (dyApple < -127) dyApple = -127;
    mouseSubAppleX -= dxApple;
    mouseSubAppleY -= dyApple;

    mouseAppleX = static_cast<uint8_t>(mouseAppleX + dxApple);
    mouseAppleY = static_cast<uint8_t>(mouseAppleY + dyApple);
    pushMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::onMouseButton(int button, int action)
{
    const bool press = (action != 0);   // GLFW_RELEASE = 0, others = press/repeat
    const pom2::mousegrab::Context grabCtx = mouseGrabContext();

    // Middle click TOGGLES the capture, matching what every VM viewer trains
    // into the user's fingers, and it is one of exactly two gestures that
    // can — Ctrl+Alt+G is the other. Checked first and on PRESS only:
    // releasing the wheel button must not toggle back, and while captured
    // ImGui has no mouse, so nothing else wants this event.
    //
    // A left press does NOT capture. That contract is gone on purpose: it
    // made an ordinary click silently change the meaning of every later
    // click, and the capturing press had to be swallowed (the guest cursor
    // is wherever its firmware left it, not under the host pointer), so the
    // user's first click simply vanished. Left presses now always route by
    // shouldRouteButton below and mean what they look like.
    if (pom2::mousegrab::isToggleButton(button)) {
        if (press && pom2::mousegrab::shouldToggleGrab(grabCtx))
            setMouseGrab(!mouseGrabbed_);
        return;
    }

    // Only the primary button is wired to the Apple Mouse Card (PB7 of the
    // MCU). Captured, every press is the guest's; uncaptured, only presses
    // over the Apple II Screen widget are (the rest belong to ImGui —
    // menus, panels). A RELEASE always passes through, so a button pressed
    // inside the screen but released outside still gets cleared on the card.
    if (!pom2::mousegrab::shouldRouteButton(grabCtx, button, press)) return;

    mouseButtonHeld = press;
    (void)mouseCoordinator_->routeHost(mouseAppleX, mouseAppleY,
                                       mouseButtonHeld);
}

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    // Under the lock, same reason as kioskRescanDisks: this is a reference
    // into live card state that another thread can reassign.
    std::string p;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        p = dev->getImagePath();
    }
    // Boot from the slot the HDV/CFFA card is actually plugged in — the user
    // can move it to slot 2 / 7 / etc. via Slot Configuration and the
    // boot path follows. The card's slot ROM bakes its slot number into
    // the ProDOS dispatcher trampolines, so `bootFromSlot(N)` lands on
    // the right $C(N)00 entry point automatically.
    controller->bootFromSlot(dev->getSlot());
    tapeStatusMessage = "Booting HDV (slot " +
        std::to_string(dev->getSlot()) + "): " + p;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

void MainWindow::renderPasteFileDialog()
{
    if (showPasteFileDialog) {
        ImGui::OpenPopup("Paste from file");
        showPasteFileDialog = false;
    }
    if (ImGui::BeginPopupModal("Paste from file", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Path to a text file (Applesoft listing, etc.)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", pasteDialogPath.c_str());
        if (ImGui::InputText("##PastePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            pasteDialogPath = buf;
        else
            pasteDialogPath = buf;
        ImGui::Checkbox("Auto-uppercase", &pasteAutoUppercase);
        ImGui::Separator();
        if (ImGui::Button("Paste", ImVec2(120, 0))) {
            if (!pasteDialogPath.empty()) pasteFromFile(pasteDialogPath);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ─── Joystick / paddles ──────────────────────────────────────────────────

void MainWindow::pollJoystickAndPushToMemory()
{
    joystick->poll();
    joystick->autoBindIfUnconfigured();

    // One-shot diagnostic when the bound pad (or its gamepad-mapping status)
    // changes: the kiosk Start-menu only works when gamepad-mapped=yes. If a
    // pad is present but reports "no", GLFW has no standard mapping for it,
    // so use the F1 keyboard fallback (or add an SDL mapping).
    {
        const int  hi = joystick->binding().hostIdx;
        const bool gp = joystick->activeIsGamepad();
        if (hi != loggedJoyHost_ || gp != loggedJoyGamepad_) {
            loggedJoyHost_    = hi;
            loggedJoyGamepad_ = gp;
            if (hi >= 0) {
                pom2::log().info(
                    "Joystick", "bound #" + std::to_string(hi + 1) + " '" +
                    joystick->activeName() + "' gamepad-mapped=" +
                    (gp ? "yes (Start opens kiosk disk menu)"
                        : "no (use F1 for the kiosk disk menu)"));
            } else {
                pom2::log().info("Joystick", "no pad bound");
            }
        }
    }

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. Hold stateMutex
    // while writing: the CPU worker reads paddleValue/paddleButton inside
    // softSwitchAccess under the same lock (during processor.run()), so an
    // unlocked write here is a data race on those non-atomic arrays.
    // While the kiosk disk selector is open the pad drives the menu, not the
    // game: feed the Apple II centered paddles + released buttons so the A/B
    // navigation presses (which share physical buttons with PB0/PB1) don't
    // leak into the running title.
    const bool menuActive = kioskMenuOpen_;
    const JoystickInput::GamepadPlay play = joystick->play();

    // Menu → game isolation across the close. Circle/Cross double as the
    // menu's B/A and the Apple PB0/PB1, and this poll runs BEFORE
    // updateKioskMenu, so `menuActive` lags the close by a frame: the press
    // that dismissed the menu would land in the game as a fire-button hit.
    // Latch a swallow on the open→closed edge and hold it until every shared
    // button (faces + D-pad) is released. Analog paddles stay live — the
    // stick isn't a menu control and carries no edge.
    // Only gamepad-mapped pads need the latch: raw pads can't drive the menu
    // (nav requires a mapping), so a fire button held across a keyboard-
    // driven close is a legitimate game input, not a menu leftover.
    if (kioskMenuWasOpen_ && !menuActive && play.valid) kioskSwallowPad_ = true;
    kioskMenuWasOpen_ = menuActive;
    if (kioskSwallowPad_) {
        const bool anyHeld = play.valid
            ? (play.button0 || play.button1 || play.dpadUp || play.dpadDown ||
               play.dpadLeft || play.dpadRight)
            : (joystick->buttonDown(0) || joystick->buttonDown(1) ||
               joystick->buttonDown(2));
        if (!anyHeld) kioskSwallowPad_ = false;
    }
    const bool suppressGame = menuActive || kioskSwallowPad_;

    {
        auto st = controller->lockState();
        Memory& mem = st.memory();
        for (int i = 0; i < 4; ++i)
            mem.setPaddle(i, menuActive ? 128 : joystick->paddleValue(i));

        if (suppressGame) {
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, false);
        } else if (play.valid) {
            // Gamepad-mapped: only Cross/Circle are Apple game-port buttons;
            // the other face buttons are keyboard keys (below), so PB2 is up.
            mem.setPaddleButton(0, play.button0);   // Circle → PB0
            mem.setPaddleButton(1, play.button1);   // Cross  → PB1
            mem.setPaddleButton(2, false);
        } else {
            // Raw pad (unknown layout): legacy buttons 0/1/2 → PB0/1/2.
            for (int i = 0; i < 3; ++i) mem.setPaddleButton(i, joystick->buttonDown(i));
        }
    }

    // Keyboard routing for the digital controls — outside stateMutex, since
    // queueKey has its own keyboard lock. Only in-game (menu closed, swallow
    // drained) and only for a gamepad-mapped pad whose layout we can trust.
    //
    // Its own reference, deliberately: the paddle block above reaches Memory
    // through the state lock, this one must NOT hold that lock (queueKey
    // takes `Memory::kbMutex`, the finer-grained one). Sharing a single
    // reference across the two, as this function used to, is what made the
    // split invisible.
    Memory& mem = controller->memory();
    if (suppressGame || !play.valid) {
        // Drop the auto-repeat history so a direction still held from menu
        // navigation re-arms cleanly (press-then-delay) once released.
        for (bool& h : padArrowHeld_) h = false;
    } else {
        if (play.spaceEdge) mem.queueKey(0x20);   // Square   → SPACE
        if (play.enterEdge) mem.queueKey(0x0D);   // Triangle → RETURN
        // D-pad → Apple II arrow codes (←$08 →$15 ↑$0B ↓$0A) with auto-repeat
        // so a held direction keeps moving, like the //e keyboard.
        const bool    held[4] = { play.dpadUp, play.dpadDown, play.dpadLeft, play.dpadRight };
        const uint8_t code[4] = { 0x0B, 0x0A, 0x08, 0x15 };
        const double  t = ImGui::GetTime();
        for (int i = 0; i < 4; ++i) {
            if (!held[i]) { padArrowHeld_[i] = false; continue; }
            if (!padArrowHeld_[i]) {                       // press: fire once
                mem.queueKey(code[i]);
                padArrowHeld_[i]  = true;
                padArrowNextT_[i] = t + 0.35;              // delay before repeat
            } else if (t >= padArrowNextT_[i]) {           // held: repeat
                mem.queueKey(code[i]);
                padArrowNextT_[i] = t + 0.06;              // ~16/s
            }
        }
    }
}


// ─── Abstraction Levels (LLE / HLE) ──────────────────────────────────────
//
// The window is `AbstractionLevels_ImGui` and the catalog of subsystems is
// static data beside it; what lives here is the part only MainWindow can
// answer — which cards are on the bus, and which of them are running their
// real ROM versus a fallback. That distinction is the panel's reason to
// exist: `docs/lle_vs_hle.md` § "Keeping a level once you have it" names
// silent degradation as a structural hole, because every ROM-driven low
// level in POM2 falls back to a working higher one when its dump is absent
// and nothing anywhere says so.

bool MainWindow::swapSlotCardVariant(const char* fromKey, const char* toKey)
{
    // In place, in the slot the card already occupies: the two keys are the
    // same peripheral at two abstraction levels, so moving it would be a
    // second, unasked-for change (and would break any software that has the
    // slot number baked in, which for a mouse or a printer is most of it).
    int slot = -1;
    for (int s = 1; s <= 7; ++s)
        if (slotCards[s] == fromKey) { slot = s; break; }
    if (slot < 0) return false;

    const std::string key      = "slot_" + std::to_string(slot) + "_card";
    const std::string previous = settings->getString(key, "");
    settings->setString(key, toKey);
    if (!settings->save()) {
        settings->setString(key, previous);
        tapeStatusMessage = "Could not save the slot change.";
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    if (!restartEmulationFromSettings()) {
        // Rebuild refused — the live machine was deliberately left intact, so
        // the persisted key has to go back too or the refused change would
        // apply silently on the next launch. Same contract as Slot Config's
        // Apply. Say so: the panel's radio snaps back to the old side next
        // frame, and an unexplained snap-back reads as a dead control.
        settings->setString(key, previous);
        settings->save();
        tapeStatusMessage = std::string("Could not rebuild the machine with ") +
                            toKey + " — kept " + fromKey + ".";
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    pom2::log().info("Abstraction",
                     "slot " + std::to_string(slot) + ": " + fromKey +
                     " -> " + toKey);
    return true;
}

void MainWindow::renderAbstractionPanel()
{
    if (!show(pom2::PanelId::Abstraction)) return;
    if (!abstractionPanel)
        abstractionPanel = std::make_unique<pom2::AbstractionLevels_ImGui>();

    using Panel = pom2::AbstractionLevels_ImGui;
    using Live  = Panel::Live;
    Panel::Snapshot snap;

    // Plug state comes from the live slot map rather than from the dozen
    // `*Card` pointers: one uniform test that covers every catalogued card,
    // including the ones MainWindow keeps no pointer to.
    auto plugged = [&](const char* key) {
        for (int s = 1; s <= 7; ++s)
            if (slotCards[s] == key) return true;
        return false;
    };
    auto row = [&](const char* id, Live live, const char* detail = "") {
        Panel::Row r;
        r.id     = id;
        r.live   = live;
        r.detail = detail;
        snap.rows.push_back(std::move(r));
    };
    // A card that is plugged but running its fallback ROM: still working,
    // still wrong about its level. `actual` is where it really sits.
    auto degradable = [&](const char* id, bool isPlugged, bool atFullLevel,
                          pom2::AbsLevel fallback, const char* why) {
        Panel::Row r;
        r.id = id;
        if      (!isPlugged)   r.live = Live::NotPlugged;
        else if (atFullLevel)  r.live = Live::Active;
        else                 { r.live = Live::Degraded; r.actual = fallback;
                               r.detail = why; }
        snap.rows.push_back(std::move(r));
    };

    // ── Storage ─────────────────────────────────────────────────────────
    // Disk II is the sharpest case in the whole table: no P6 dump and no WOZ
    // mounted means the legacy 32-cycle nibble gate, which reads stock DOS
    // 3.3 fine and loses every bitstream-reading protection. `usingBitLss()`
    // is the honest test — a mounted WOZ forces the L0 path even with no
    // roms/diskii_p6.rom, using the embedded default P6.
    degradable("diskii", primaryDiskII() != nullptr,
               primaryDiskII() && primaryDiskII()->usingBitLss(), pom2::AbsLevel::H1,
               "roms/diskii_p6.rom absent and no WOZ mounted — running the "
               "legacy 32-cycle nibble gate, which cannot decode "
               "bitstream-level copy protection.");
    row("diskimage", primaryDiskII() ? Live::Active : Live::NotPlugged);
    row("cffa", plugged("cffa")        ? Live::Active : Live::NotPlugged);
    row("hdv",  plugged("hdv")         ? Live::Active : Live::NotPlugged);
    degradable("smartportcard", plugged("smartport35"),
               primarySmartPortCard() && primarySmartPortCard()->isLironRomLoaded(),
               pom2::AbsLevel::H1,
               "roms/liron.rom absent — the slot page and the $C800 bank are "
               "POM2's synthetic firmware instead of the real Liron ROM.");
    {
        const bool iicClass =
            activeProfile == pom2::SystemProfile::AppleIIc ||
            activeProfile == pom2::SystemProfile::AppleIIcPlus ||
            activeProfile == pom2::SystemProfile::AppleIIcPAL;
        row("iicsp", iicClass ? Live::Active : Live::NotPlugged,
            iicClass ? "Armed only by an explicit boot from slot 5; every "
                       "reset disarms it." : "");
        row("iwm", activeProfile == pom2::SystemProfile::AppleIIcPlus
                       ? Live::Active : Live::NotPlugged);
        row("sony35", (iicClass || plugged("smartport35"))
                       ? Live::Active : Live::NotPlugged);
    }
    row("prodosvol", Live::NotApplicable);

    // ── Input, clocks, printing ─────────────────────────────────────────
    const auto mouseInventory = mouseCoordinator_->capture();
    row("mouse",   mouseInventory.mamePlugged     ? Live::Active : Live::NotPlugged);
    row("mouseaw", mouseInventory.appleWinPlugged ? Live::Active : Live::NotPlugged);
    degradable("clock", plugged("clock"),
               devicePanelCoordinator_->captureInventory().clockRomFromDump,
               pom2::AbsLevel::H1,
               "roms/thunderclock_u9_v1.3.bin absent — running the synthetic "
               "ProDOS-signature stub, so tools that pull the driver off the "
               "card find nothing.");
    degradable("grappler", plugged("grappler"),
               printerCoordinator_->captureHost(*controller).grapplerRomLoaded,
               pom2::AbsLevel::H1,
               "roms/grappler_plus.bin absent — running buildStubRom(), so "
               "the real Orange Micro firmware is not executing.");
    row("printercard", plugged("printer") ? Live::Active : Live::NotPlugged);
    row("nsclock", controller->noSlotClock().isEnabled()
                       ? Live::Active : Live::NotPlugged);

    // ── Video ───────────────────────────────────────────────────────────
    // Exactly one of the two colour paths is running, which makes this pair
    // the clearest live illustration of the axis in the whole panel.
    {
        const auto hi = display->getHiResMode();
        const bool oe = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                         hi == Apple2Display::HiResMode::ColorCompositeOECpu);
        row("oe",  oe ? Live::Active : Live::NotPlugged,
            oe ? "Signal-level demodulation of the 14.318 MHz 1-bit stream."
               : "Pick a Composite (OpenEmulator) mode in Display to run it.");
        row("lut", oe ? Live::NotPlugged : Live::Active,
            oe ? "" : "Artifact colours are read from a table; no signal is "
                      "synthesised.");
    }
    row("chatmauve", devicePanelCoordinator_->captureInventory().chatMauvePlugged()
                         ? Live::Active : Live::NotPlugged);

    // ── Audio, network, CPU ─────────────────────────────────────────────
    row("mockingboard", (plugged("mockingboard") || plugged("mockingboard_c") ||
                         plugged("phasor")) ? Live::Active : Live::NotPlugged);
    row("ssi263", (plugged("echoplus") || plugged("mockingboard_c"))
                      ? Live::Active : Live::NotPlugged);
    row("tms5220", plugged("echoplus_tms") ? Live::Active : Live::NotPlugged);
    row("ssc",       plugged("ssc")       ? Live::Active : Live::NotPlugged);
    row("uthernet",  plugged("uthernet")  ? Live::Active : Live::NotPlugged);
    row("uthernet2", plugged("uthernet2") ? Live::Active : Live::NotPlugged);
    // The one entry in this group that used to report nothing, so it
    // defaulted to NotApplicable ("always present") — which is exactly the
    // silent-degradation blind spot the panel exists to close. libslirp is
    // an OPTIONAL build dependency: without it `SlirpNetworkBackend`
    // compiles to a stub that always fails, so Uthernet I has no transport
    // at all and Uthernet II is confined to its own hardware stack.
#ifdef POM2_HAVE_SLIRP
    row("netbackend", Live::Active,
        "libslirp linked — user-mode NAT available to both Uthernet cards.");
#else
    row("netbackend", Live::NotPlugged,
        "Built without libslirp: no user-mode NAT. Uthernet I (raw frames) "
        "has no transport; Uthernet II still does TCP/UDP through its own "
        "W5100 hardware stack.");
#endif
    row("fujinet",   plugged("fujinet")   ? Live::Active : Live::NotPlugged);
    row("softcard",  plugged("softcard")  ? Live::Active : Live::NotPlugged);
    row("z80",       plugged("softcard")  ? Live::Active : Live::NotPlugged);

    // ── The switchable boundaries ───────────────────────────────────────
    // Availability is gated on the dump each low side needs, because the
    // whole point of the panel is that a missing dump silently costs you a
    // level — offering a switch that would land on the fallback would repeat
    // the exact mistake it exists to expose.
    auto have = [](const char* rel) {
        return !pom2::findResource(rel).empty();
    };
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::MouseCard;
        t.title        = "Mouse Card";
        t.needsRestart = true;
        t.low.label    = "MAME — the M68705 MCU executes its mask ROM";
        t.low.level    = pom2::AbsLevel::L0;
        t.low.available = have("roms/mouse_341-0270-c.bin") &&
                          have("roms/mouse_341-0269.bin");
        t.low.blockedBy = "both roms/mouse_341-0270-c.bin (slot EPROM) and "
                          "roms/mouse_341-0269.bin (MCU mask ROM) are needed";
        t.low.why      = "Decodes real quadrature edges: at most one edge per\n"
                         "axis per MCU PortB read, so fast host motion is\n"
                         "rate-limited exactly as the hardware limits it.";
        t.high.label   = "AppleWin HLE — the MCU is a C++ state machine";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.available = have("roms/mouse_341-0270-c.bin");
        t.high.blockedBy = "roms/mouse_341-0270-c.bin (slot EPROM) is needed";
        t.high.why     = "Copies the host delta straight into the HLE'd MCU,\n"
                         "so it never drops motion — smoother, and less\n"
                         "correct. Needs a compensating absolute cursor sync\n"
                         "the L0 card does not.";
        const auto mouseInventory = mouseCoordinator_->capture();
        t.selected     = mouseInventory.mamePlugged ? 0
                       : (mouseInventory.appleWinPlugged ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add a mouse in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::BlockStorage;
        t.title        = "ProDOS block storage";
        t.needsRestart = true;
        t.low.label    = "CFFA 2.0 — the real 4 KB firmware executes over ATA";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/cffa20ee02.bin") ||
                          have("roms/cffa20eec02.bin");
        t.low.blockedBy = "roms/cffa20ee02.bin (or the 65C02 variant) is needed";
        t.low.why      = "An ATA taskfile model isomorphic to MAME's\n"
                         "cs0_r/cs0_w, driven by the card's own firmware.\n"
                         "Skips DMA / IRQ / SMART.";
        t.high.label   = "HDV card — synthetic ROM, 4-register port, memcpy";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "H1 IS the feature here: it mounts .hdv/.2mg\n"
                         "directly with no card ROM dump at all. The ProDOS\n"
                         "block corpus has no protection to lose.";
        t.selected     = plugged("cffa") ? 0 : (plugged("hdv") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id           = pom2::AbsToggle::PrinterIface;
        t.title        = "Printer interface";
        t.needsRestart = true;
        t.low.label    = "Grappler+ — the real Orange Micro EPROM executes";
        t.low.level    = pom2::AbsLevel::L2;
        t.low.available = have("roms/grappler_plus.bin");
        t.low.blockedBy = "roms/grappler_plus.bin is needed";
        t.low.why      = "Status byte, register decode, $C800 banking and the\n"
                         "S1 DIPs, line-cited against MAME grappler.cpp.\n"
                         "What AppleWorks and the graphics dumps expect.";
        t.high.label   = "Printer card — synthetic ROM, PR#n hook only";
        t.high.level   = pom2::AbsLevel::H1;
        t.high.why     = "A CSWL/CSWH hook and a 4-byte trampoline. No PROM\n"
                         "dump exists to run, and the Pascal entry block is\n"
                         "deliberately absent, so BASIC PR#n only.";
        t.selected     = plugged("grappler") ? 0 : (plugged("printer") ? 1 : -1);
        if (t.selected < 0)
            t.note = "Neither is plugged — add one in Slot Configuration "
                     "first, then switch levels here.";
        snap.toggles.push_back(std::move(t));
    }
    {
        Panel::ToggleState t;
        t.id         = pom2::AbsToggle::CompositeVideo;
        t.title      = "Colour pipeline";
        t.low.label  = "Composite (OpenEmulator) — demodulate a real signal";
        t.low.level  = pom2::AbsLevel::L1;
        t.low.why    = "The display emits a 14.318 MHz 1-bit luminance\n"
                       "stream; the shader demodulates Y/I/Q off the\n"
                       "subcarrier. Artifact colour is EMERGENT.";
        t.high.label = "Artifact LUT — look the colour up per dot pattern";
        t.high.level = pom2::AbsLevel::H1;
        t.high.why   = "MAME's composite colour tables: the RESULT of NTSC\n"
                       "artifacting, tabulated. Cheap, sharp, and unable to\n"
                       "show anything the table has no entry for.";
        const auto hi = display->getHiResMode();
        t.selected   = (hi == Apple2Display::HiResMode::ColorCompositeOE ||
                        hi == Apple2Display::HiResMode::ColorCompositeOECpu)
                           ? 0 : 1;
        t.note       = "Mono modes are neither — they bypass colour entirely.";
        snap.toggles.push_back(std::move(t));
    }

    const Panel::Request req = abstractionPanel->render(&show(pom2::PanelId::Abstraction),
                                                       snap);
    switch (req.toggle) {
        case pom2::AbsToggle::None:
            break;
        case pom2::AbsToggle::MouseCard:
            swapSlotCardVariant(req.option == 0 ? "mouseaw" : "mouse",
                                req.option == 0 ? "mouse" : "mouseaw");
            break;
        case pom2::AbsToggle::BlockStorage:
            swapSlotCardVariant(req.option == 0 ? "hdv" : "cffa",
                                req.option == 0 ? "cffa" : "hdv");
            break;
        case pom2::AbsToggle::PrinterIface:
            swapSlotCardVariant(req.option == 0 ? "printer" : "grappler",
                                req.option == 0 ? "grappler" : "printer");
            break;
        case pom2::AbsToggle::CompositeVideo:
            // No restart: the colour pipeline is a render-path choice, and
            // the machine never sees it. Persisted by the dtor's hi_res_mode
            // write, exactly like a View-menu pick.
            display->setHiResMode(
                req.option == 0 ? Apple2Display::HiResMode::ColorCompositeOE
                                : Apple2Display::HiResMode::ColorNTSC);
            lastColorHiResMode_ = display->getHiResMode();
            break;
    }
}



void MainWindow::renderDiskPanelWindow()
{
    if (!show(pom2::PanelId::DiskII)) return;

    // Disk library is the same for every plugged DiskII (it's the
    // contents of disks_5.4/ on disk). Build it once and share via copy.
    std::vector<pom2::DiskController_ImGui::LibraryEntry> sharedLibrary;
    // Disk library — scan disks_5.4/ recursively for .dsk/.do/.po/.nib/.woz.
    // Sub-folders are honoured so users can shelve their library by
    // format (`disks_5.4/dsk/`, `disks_5.4/woz/`, …) or by collection
    // (`disks_5.4/games/`, `disks_5.4/dev/`, …) without losing the one-click
    // boot. Cheap (a few dirent reads per frame); sorted alphabetically
    // so the list doesn't reshuffle as the OS hands us a different
    // dirent order. `displayName` carries the path relative to the
    // scanned root so two files of the same name in different sub-
    // folders don't collide visually.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks_5.4", "../disks_5.4", "../../disks_5.4" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                // Skip dotfiles AND dotdirs (.git, .DS_Store, …). On a
                // dotdir we disable_recursion_pending so we don't walk
                // into it on the next ++.
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                    ext != ".nib" && ext != ".woz") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                sharedLibrary.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(sharedLibrary.begin(), sharedLibrary.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // (Auto-turbo lives in updateAutoTurbo(), called every frame from
    // render() — it must run even when this panel window is hidden, which is
    // the default. See MainWindow::updateAutoTurbo.)

    // ── Render one window per plugged DiskII ──────────────────────────
    // Title carries the slot number so ImGui assigns each card its own
    // window state (position, size, dock). The primary (lowest-slot) card
    // gets the curated default position; subsequent cards cascade slightly
    // down/right so they don't perfectly overlap on first show.
    // Hoisted: each call walks the bus, so re-reading it per iteration would
    // make this quadratic in slot count for no gain — the topology cannot
    // change inside one UI-thread loop.
    const auto diskCardList = diskIICards();
    for (size_t idx = 0; idx < diskCardList.size() && idx < diskPanels.size(); ++idx) {
        DiskIICard*                       card  = diskCardList[idx];
        pom2::DiskController_ImGui*       panel = diskPanels[idx].get();
        if (!card || !panel) continue;

        pom2::DiskController_ImGui::DriveSnapshot snap;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            snap.bootRomLoaded     = card->hasBootRom();
            snap.diskLoaded        = card->isDiskLoaded();
            snap.motorOn           = card->isMotorOn();
            snap.track             = card->getCurrentTrack();
            snap.halfTrack         = card->getHalfTrack();
            snap.trackPos          = card->getTrackPosition();
            snap.diskPath          = card->getDiskPath();
            snap.lastError         = card->getLastError();
            snap.writeBackEnabled  = card->isWriteBackEnabled();
            snap.hasUnsavedChanges = card->hasUnsavedChanges();
            snap.fileWriteProtected = card->isFileWriteProtected();
        }
        snap.turboWhileMotor = diskTurboWhileMotor;
        snap.turboActive     = diskTurboActive;
        snap.library         = sharedLibrary;     // shared copy

        const float baseX = 1055.0f, baseY = 30.0f;
        ImGui::SetNextWindowPos (
            ImVec2(baseX - static_cast<float>(idx) * 30.0f,
                   baseY + static_cast<float>(idx) * 30.0f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(705, 960), ImGuiCond_FirstUseEver);

        char title[64];
        std::snprintf(title, sizeof(title),
                      "Disk II (slot %d)", card->getSlot());
        // Only the primary card honours `show(pom2::PanelId::DiskII)` (the menu toggle).
        // Secondary cards share the same toggle for simplicity — the user
        // sees them appear/disappear together. We feed the same flag to
        // each render() call.
        auto result = panel->render(title, show(pom2::PanelId::DiskII), snap);

        if (result.turboToggleChanged) {
            diskTurboWhileMotor = result.turboNewValue;
        }
        if (result.writeBackToggleChanged) {
            // Persists disk_writeback_slotN with the change — the bare setter
            // did not, so the toggle did not survive a restart, and since
            // isWriteProtected() is `fileWriteProtected || !writeBack` the
            // guest then saw a write-protected disk again.
            (void)storageCoordinator_->setDiskIIWriteBack(
                *controller, *settings, card->getSlot(),
                result.writeBackNewValue);
            tapeStatusMessage = "slot " + std::to_string(card->getSlot()) +
                (result.writeBackNewValue
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (result.requestEject) {
            const auto r = storageCoordinator_->ejectDiskII(
                *controller, *settings, card->getSlot(), 0);
            tapeStatusMessage = r.ok
                ? ("Disk ejected (slot " + std::to_string(card->getSlot()) + ")")
                : ("Eject failed: " + r.error);
            tapeStatusUntil   = lastFrameTime + 4.0;
        }
        if (result.requestBoot) {
            auto st = controller->lockState();
            card->seekTrack0();
            const uint16_t pc = static_cast<uint16_t>(
                0xC000 + (card->getSlot() << 8));
            st.cpu().setProgramCounter(pc);
            controller->setMode(EmulationController::Mode::Running);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Boot: PC → $%04X", pc);
            tapeStatusMessage = msg;
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (!result.requestInsertAndBoot.empty()) {
            const std::string path = result.requestInsertAndBoot;
            std::string err;
            const bool ok = pom2::mountDiskII(*controller, *card, 0, path, err,
                                              /*seekTrack0=*/true);
            if (ok) {
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library click → insert + boot: " + path);
                tapeStatusMessage = "Booting: " + path;
            } else {
                tapeStatusMessage = "Boot failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (!result.requestInsertOnly.empty()) {
            const std::string path = result.requestInsertOnly;
            std::string err;
            const bool ok = pom2::mountDiskII(*controller, *card, 0, path, err);
            if (ok) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library right-click → insert only: " + path);
                tapeStatusMessage = "Inserted (no boot): " + path;
            } else {
                tapeStatusMessage = "Insert failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
}

// ─── Disk Library (unified browser: 5.25 / 3.5 / HDV) ───────────────────

bool MainWindow::ejectMediaBay(int slot, int index, bool diskII)
{
    // Addressed by SLOT + bay rather than by a card pointer: the status bar
    // builds its rows from a value snapshot taken under the lock and released
    // before drawing, so by the time the user clicks, any pointer captured
    // back then could belong to a card a Slot-Config Apply has since deleted.
    // Re-resolving through the bus here is what makes the click safe.
    std::string label;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        SlotPeripheral* per = controller->memory().slotBus().peripheral(slot);
        if (!per) return false;

        if (diskII) {
            auto* d2 = dynamic_cast<DiskIICard*>(per);
            if (!d2) return false;
            // ejectDisk() saves dirty tracks first when write-back is on and
            // returns false if that save failed — in which case the medium
            // deliberately stays mounted rather than losing the writes.
            ok = d2->ejectDisk(index);
            label = "slot " + std::to_string(slot) + " drive " +
                    std::to_string(index + 1);
            if (ok) {
                // Match the shutdown-persist keys so the disk does not come
                // back on the next launch (the same pair `restartEmulation
                // FromSettings` snapshots).
                const std::string k = "_slot" + std::to_string(slot);
                settings->setString("disk_path" + k, "");
                if (index == 0 && d2 == primaryDiskII()) settings->setString("disk_path", "");
            }
        } else {
            auto* media = dynamic_cast<pom2::MountableMediaCard*>(per);
            if (!media) return false;
            ok = media->ejectBay(index);
            label = "slot " + std::to_string(slot);
            if (media->bayCount() > 1)
                label += " bay " + std::to_string(index + 1);
            if (ok) {
                // SmartPort units persist their own per-unit key, and it is
                // written eagerly on mount (not at shutdown), so an eject that
                // did not clear it would remount the image on the next launch.
                if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(per)) {
                    settings->setString(
                        "smartport_slot" + std::to_string(sp->getSlot()) +
                        "_unit" + std::to_string(index) + "_path", "");
                }
                if (dynamic_cast<ProDOSHardDiskCard*>(per))
                    settings->setString("hdv_path", "");
                if (auto* cffa = dynamic_cast<pom2::CffaCard*>(per)) {
                    settings->setString(
                        "cffa_slot" + std::to_string(cffa->getSlot()) + "_path",
                        "");
                }
            }
        }
    }
    if (ok) settings->save();

    tapeStatusMessage = ok ? ("Ejected " + label)
                           : ("Eject refused for " + label +
                              " — the image could not be saved, so it stays "
                              "mounted");
    tapeStatusUntil = lastFrameTime + (ok ? 3.0 : 8.0);
    if (!ok) pom2::log().warn("Media", tapeStatusMessage);
    return ok;
}

void MainWindow::ejectAllDisks()
{
    // One locked pass over the whole topology — Disk II (BOTH drives), every
    // block device, every SmartPort unit and the on-board 3.5" pair — with the
    // settings writes applied after the lock is released.
    //
    // Two things the hand-rolled version got wrong. It called `ejectDisk()`
    // with the default argument, so it only ever ejected drive 1 and left
    // drive 2's medium mounted with its path still persisted. And it ignored
    // every eject's return value, so a medium whose write-back failed was
    // reported as ejected: the failure is exactly when the user needs to know,
    // because the card keeps that disk mounted so the write can be retried.
    const auto result = storageCoordinator_->ejectAllMedia(*controller, *settings);

    if (!result.ok()) {
        std::string msg = "Eject failed (media left mounted): ";
        for (size_t i = 0; i < result.failures.size(); ++i)
            msg += (i ? "; " : "") + result.failures[i];
        tapeStatusMessage = std::move(msg);
    } else {
        tapeStatusMessage = result.changed ? "Eject completed"
                                           : "Nothing was mounted";
    }
    tapeStatusUntil = lastFrameTime + 3.0;
}

bool MainWindow::routeMount35(int driveIdx, const std::string& path,
                              std::string& errOut)
{
    // One routing rule for 3.5" media, in the coordinator: a plugged SmartPort
    // card's units own it (auto-creating a SmartPort35Unit on the target bay,
    // flushing whatever was there first so a failed write-back aborts the
    // mount rather than losing the writes), and only without one does it fall
    // through to the //c+ on-board hub. Callers keep this signature; the CLI
    // insert+boot path and the Library share it.
    const auto r = storageCoordinator_->mountDisk35(*controller, *settings,
                                                    driveIdx, path);
    if (!r.ok) errOut = r.error;
    return r.ok;
}

bool MainWindow::routeMountHdv(const std::string& path, int& bootSlotOut,
                               std::string& errOut)
{
    // On //c-class the cffa/hdv slot cards are ROM-masked by the forced
    // INTCXROM and can't boot ($Cn00 reads internal ROM, not the card) —
    // the only bootable block device is the on-board SmartPort (slot 5),
    // so skip the dedicated-HDV-card branch there and route HDV to the
    // SmartPort unit. See project_iic_smartport_boot.
    const bool iicClass =
        (activeProfile == pom2::SystemProfile::AppleIIc ||
         activeProfile == pom2::SystemProfile::AppleIIcPlus ||
         activeProfile == pom2::SystemProfile::AppleIIcPAL);
    // Prefer a dedicated HDV-class card — the MAME-faithful CffaCard if
    // plugged, else the synthetic ProDOSHardDiskCard; else route to a
    // SmartPort card's unit 0 (auto-creating a SmartPortHdvUnit). Promoted
    // from a lambda in renderDiskLibraryWindow.
    if (!iicClass) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            // Two-phase: up to 32 MiB read with no lock held, then the lock
            // only for the 2IMG parse and the swap. This was the largest
            // single stall in the tree — 12.8 ms warm-cache, most of a PAL
            // frame with the machine and the window both stopped.
            // Same two-phase read, and the coordinator writes hdv_path with
            // the mount instead of leaving it for the next shutdown.
            const int slot = dev->getSlot();
            const auto r = storageCoordinator_->mountMediaBay(
                *controller, *settings, slot, 0, path);
            if (!r.ok) {
                errOut = r.error;
                hdvStatus = "no image mounted";
                return false;
            }
            hdvPath = path;
            hdvStatus = "loaded: " + path;
            bootSlotOut = slot;
            return true;
        }
    }
    if (primarySmartPortCard()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        const std::string base =
            "smartport_slot" + std::to_string(primarySmartPortCard()->getSlot()) +
            "_unit0";
        pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
        bool replaced = false;
        if (!u || u->kindKey() != pom2::SmartPortHdvUnit::kKindKey) {
            // Flush before setUnit destroys it — see routeMount35 above for
            // why the destructor's best-effort save is not good enough.
            if (u && !u->saveDirty()) {
                errOut = "unsaved changes on SmartPort unit 1 could not be "
                         "written: " + u->lastError();
                return false;
            }
            primarySmartPortCard()->setUnit(
                0, std::make_unique<pom2::SmartPortHdvUnit>());
            u = primarySmartPortCard()->unit(0);
            replaced = true;
        }
        // Same two-phase mount as the dedicated HDV card above: the unit
        // swap needs the lock (it mutates the card's unit table), the 32 MiB
        // read does not. The 3.5" branch above deliberately stays inline —
        // its unit has no block backing, so it would pay for a phase-1 read
        // it then discards.
        if (!pom2::mountSmartPortUnit(*controller, *u, path, errOut))
            return false;
        settings->setString(base + "_type",
            std::string(pom2::SmartPortHdvUnit::kKindKey));
        settings->setString(base + "_path", path);
        if (replaced) settings->setBool(base + "_writeback", false);
        if (!settingsReadOnly()) settings->save();   // kiosk: never touch state.cfg
        bootSlotOut = primarySmartPortCard()->getSlot();
        return true;
    }
    errOut = "no HDV or SmartPort card plugged";
    return false;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    // The CFFA-outranks-HDV preference is the coordinator's rule now, so the
    // menus, the AI server and the boot path cannot drift apart on it.
    return storageCoordinator_->topology(controller->memory().slotBus())
        .preferredBlock();
}

// Bus *topology* reads (which slot holds which card) are UI-thread-confined:
// every writer — plugSlotsFromSettings, applyProfile, the slot-config
// rebuild — runs on this thread, and the worker only ever reads the table.
// A lock here would protect nothing while reading as though it did; per-card
// *state* is a different matter and does go through lockState().
std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    // Walk the bus (slots 1..7) and cross-cast each plugged peripheral to
    // the ProDOSBlockCard mix-in. Both implementers (ProDOSHardDiskCard,
    // CffaCard) inherit SlotPeripheral *and* ProDOSBlockCard, so the
    // dynamic_cast side-cast succeeds for exactly those and yields nullptr
    // for everything else. Slot order is ascending, matching the "lowest
    // slot is primary" convention used for primaryDiskII()/primaryHdvCard().
    return storageCoordinator_->topology(controller->memory().slotBus())
        .blockCards;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .smartPortCards;
}

std::vector<DiskIICard*> MainWindow::diskIICards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .diskIICards;
}

std::vector<SuperSerialCard*> MainWindow::serialCards() const
{
    // Slot-ascending, which is what makes "the primary is the first" true.
    std::vector<SuperSerialCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot)))
            out.push_back(ssc);
    return out;
}

SuperSerialCard* MainWindow::primarySerialCard() const
{
    const auto cards = serialCards();
    return cards.empty() ? nullptr : cards.front();
}

DiskIICard* MainWindow::primaryDiskII() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryDiskII;
}

ProDOSHardDiskCard* MainWindow::primaryHdvCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryHdv;
}

pom2::CffaCard* MainWindow::primaryCffaCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryCffa;
}

pom2::SmartPortCard* MainWindow::primarySmartPortCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primarySmartPort;
}

bool MainWindow::flushSlotMedia(std::string& err)
{
    // The coordinator walks the same three families (Disk II pending writes,
    // block devices, SmartPort units) and aggregates the same failure text.
    // It is the flush half of the rebuild transaction — only a successful one
    // may prepare a teardown — so it has to be the same code as the one the
    // rebuild path uses, not a second copy that can drift.
    std::lock_guard<std::mutex> lk(controller->stateMutex());
    return storageCoordinator_->flushAll(controller->memory().slotBus(), err);
}

int MainWindow::ensureHdvCardForBoot()
{
    // Preference order and the session-only marking both live in the
    // coordinator, so the CLI, drag-and-drop and Floppy Emu paths cannot
    // disagree about which target a boot should use.
    //
    // Behaviour change, deliberate: on a //c-class profile this now REFUSES
    // when no SmartPort target exists, instead of falling through to plug an
    // HDV card. Slot cards there are ROM-masked by the forced INTCXROM and
    // cannot boot ($Cn00 reads internal ROM, not the card), so the old
    // fallback conjured a card that could never be booted from and then
    // reported success.
    const auto r = slotProvisioningCoordinator_->ensureHdvBootTarget(
        *controller, *settings, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

int MainWindow::ensureSmartPortCardForBoot()
{
    const auto r = slotProvisioningCoordinator_->ensureSmartPortBootTarget(
        *controller, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
    // Classify by extension + size, route into the matching slot under the
    // active profile/slot config, then cold-boot. Shared by the CLI
    // positional-disk / --kiosk launcher and (potentially) any future
    // single-call boot entry point. Mirrors the Disk Library "insert +
    // boot" buttons but with no UI surface.
    //
    // No ROM means no Monitor and no Applesoft: the boot PROM would still
    // pull sector 0 and jump into a loader that calls firmware entry points
    // backed by nothing, hanging on a garbage screen. Say so instead of
    // mounting and then reporting a boot that cannot happen.
    if (!romLoaded_) {
        errOut = "no Apple II ROM loaded — see Help > Welcome / Quick Start";
        return false;
    }
    switch (classifyDiskForSlot(path)) {
        case DiskSlotClass::Floppy525: {
            // Prefer the Disk II in the conventional boot slot 6; fall back
            // to the primary (lowest-slot) card. Booting a single floppy
            // from a non-6 slot is unconventional and breaks software that
            // hardcodes slot 6 for its loader — matters when the config has
            // Disk II in several slots (primary = lowest = e.g. slot 5).
            DiskIICard* target = nullptr;
            for (auto* c : diskIICards()) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = primaryDiskII();
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            const bool ok = pom2::mountDiskII(*controller, *target, 0, path,
                                              errOut, /*seekTrack0=*/true);
            if (!ok) return false;
            if (!controller->bootFromSlot(target->getSlot())) {
                errOut = "slot " + std::to_string(target->getSlot()) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            // Without a SmartPort card, routeMount35 falls through to the
            // //c+ on-board Sony hub — a device that only exists on the
            // //c+. On any other machine the image would "mount" into
            // hardware the guest can't see and the cold boot below would
            // land at the BASIC prompt with no error at all.
            //
            // When neither exists, auto-plug a Liron-class SmartPort the
            // same way the HDV branch below auto-plugs a block card: a
            // dropped 800K .po/.2mg is explicit "boot this" intent, and
            // failing it on the stock II+/IIe config (which ships no
            // SmartPort) made drag-and-drop refuse the single most common
            // 3.5" distribution format. Session-local, never persisted.
            if (!primarySmartPortCard() &&
                activeProfile != pom2::SystemProfile::AppleIIcPlus &&
                ensureSmartPortCardForBoot() < 0) {
                errOut = "no 3.5\" device in this config, and no free slot "
                         "to plug a SmartPort 3.5\" card into";
                return false;
            }
            if (!routeMount35(0, path, errOut)) return false;
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (primarySmartPortCard()) {
                if (!controller->bootFromSlot(primarySmartPortCard()->getSlot())) {
                    errOut = "slot " + std::to_string(primarySmartPortCard()->getSlot()) +
                             " did not boot the image (cold-booted instead)";
                    return false;
                }
            } else {
                // //c+ on-board Sony hub. The IWM bit-shift state machine is
                // deliberately unmodelled (CLAUDE.md), so this cold boot does
                // NOT reach the mounted 3.5" disk — the image is mounted and
                // the machine restarted, nothing more. Don't call it a boot.
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                errOut = "mounted on the //c+ on-board 3.5\" drive, which "
                         "POM2 cannot boot from (unmodelled IWM) — use a "
                         "SmartPort 3.5\" card to boot this image";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Hdv: {
            // Make sure a card exists to host the HDV (auto-plug one if the
            // saved config has none), then route + boot.
            if (ensureHdvCardForBoot() < 0) {
                errOut = "no free slot to plug an HDV card into";
                return false;
            }
            int bootSlot = 0;
            if (!routeMountHdv(path, bootSlot, errOut)) return false;
            if (!controller->bootFromSlot(bootSlot)) {
                errOut = "slot " + std::to_string(bootSlot) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Unknown:
        default:
            errOut = "unrecognised disk image (extension/size): " + path;
            return false;
    }
}

void MainWindow::renderDiskLibraryWindow()
{
    if (!show(pom2::PanelId::DiskLibrary)) return;

    // Default position: right column of the curated 1568×850 layout,
    // flush against the screen window. 435 px wide × 745 px tall =
    // enough for the 3-tab table + the search/sort header without
    // scroll overflow on a typical 800+ disk library.
    ImGui::SetNextWindowPos (ImVec2(1125, 90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(435,  745), ImGuiCond_FirstUseEver);

    pom2::DiskLibrary_ImGui::CurrentlyMounted mounted;
    // Build the mount snapshot under stateMutex, copying every path BY
    // VALUE — same rule as renderDiskPanelWindow / renderSmartPortPanelWindow
    // / renderFloppyEmuWindow. getDiskPath() & friends return a reference
    // into the card's live DiskImage, and the AI control server's HTTP
    // thread reassigns it from /disk insert + eject. The lock is released
    // before diskLibrary->render() below: that path re-locks.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        // Currently-inserted Disk II images (per plugged card). The library
        // tags rows with `* ` when their path matches any of these — gives
        // the user a visual cue across all plugged cards at once.
        for (auto* c : diskIICards()) {
            if (!c) continue;
            pom2::DiskLibrary_ImGui::CurrentlyMounted::DiskIICardInfo info;
            info.slot = c->getSlot();
            if (c->isDiskLoaded(0)) {
                info.drive1 = c->getDiskPath(0);
                mounted.diskII.push_back(info.drive1);
            }
            if (c->isDiskLoaded(1)) {
                info.drive2 = c->getDiskPath(1);
                mounted.diskII.push_back(info.drive2);
            }
            mounted.diskIICards.push_back(info);
        }
        // 3.5" mount sources: the //c+ on-board hub OR a slot-plugged
        // SmartPort card's unit 0/1 (one or the other, never both on the
        // same profile). The library marks rows mounted on either, so the
        // user sees the `* ` cue regardless of which path is active.
        mounted.disk35Internal = controller->disk35Internal().isLoaded()
            ? controller->disk35Internal().path() : std::string();
        mounted.disk35External = controller->disk35External().isLoaded()
            ? controller->disk35External().path() : std::string();
        if (primarySmartPortCard()) {
            const pom2::SmartPortUnit* u0 = primarySmartPortCard()->unit(0);
            const pom2::SmartPortUnit* u1 = primarySmartPortCard()->unit(1);
            if (u0 && u0->isLoaded() &&
                u0->kindKey() == pom2::SmartPort35Unit::kKindKey &&
                mounted.disk35Internal.empty()) {
                mounted.disk35Internal = u0->path();
            }
            if (u1 && u1->isLoaded() &&
                u1->kindKey() == pom2::SmartPort35Unit::kKindKey &&
                mounted.disk35External.empty()) {
                mounted.disk35External = u1->path();
            }
        }
        if (pom2::ProDOSBlockCard* dev = hdvDevice(); dev && dev->isImageLoaded()) {
            mounted.hdv = dev->getImagePath();
        } else if (primarySmartPortCard()) {
            // SmartPort-routed HDV — show as mounted in the Library so the
            // `* ` marker matches reality regardless of which path holds it.
            const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
            if (u && u->isLoaded() &&
                u->kindKey() == pom2::SmartPortHdvUnit::kKindKey) {
                mounted.hdv = u->path();
            }
        }
    }

    // Favourites + recents are host state (persisted to state.cfg); the panel
    // only renders them and reports a toggle.
    pom2::DiskLibrary_ImGui::Lists lists;
    lists.favourites   = libraryFavourites_;
    lists.recents      = libraryRecents_;
    lists.hideSizeDate = libraryHideSizeDate_;

    const auto r = diskLibrary->render("Disk Library", show(pom2::PanelId::DiskLibrary),
                                       mounted, lists);

    if (r.toggleHideSizeDate) libraryHideSizeDate_ = !libraryHideSizeDate_;

    if (!r.toggleFavourite.empty()) {
        auto it = std::find(libraryFavourites_.begin(),
                            libraryFavourites_.end(), r.toggleFavourite);
        if (it != libraryFavourites_.end()) libraryFavourites_.erase(it);
        else libraryFavourites_.push_back(r.toggleFavourite);
    }

    // Anything the user actually mounted this frame becomes the newest recent.
    // Driven off the panel's requests rather than off the cards, so a mount
    // that came from the CLI or a drag-and-drop doesn't silently reorder the
    // list behind the user's back.
    for (const std::string* p : { &r.request525InsertAndBoot,
                                  &r.request525InsertOnly,
                                  &r.request35MountAndBoot,
                                  &r.request35MountOnly,
                                  &r.requestHdvMountAndBoot,
                                  &r.requestHdvMountOnly,
                                  &r.requestFloppyEmuMountAndBoot,
                                  &r.requestFloppyEmuMountOnly }) {
        if (p->empty()) continue;
        noteLibraryRecent(*p);
    }

    // ── Eject-all (header-row button, moved here from the toolbar) ─────
    if (r.requestEjectAllDisks) ejectAllDisks();

    // ── 5.25" actions → the DiskII card/drive the right-click menu picked ──
    // request525Slot = -1 means "primary card" (left-click default); a real
    // slot routes to that specific DiskII card. drive 0 = drive 1, 1 = drive 2.
    auto resolve525 = [&](int slot) -> DiskIICard* {
        if (slot < 0) return primaryDiskII();
        for (auto* c : diskIICards()) if (c && c->getSlot() == slot) return c;
        return primaryDiskII();
    };
    if (!r.request525InsertAndBoot.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const std::string path = r.request525InsertAndBoot;
        std::string err;
        const bool ok = target && pom2::mountDiskII(*controller, *target, drive,
                                                    path, err,
                                                    /*seekTrack0=*/true);
        if (ok && target) {
            // Boot the target card's slot (its boot PROM boots drive 1).
            const bool booted = controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            tapeStatusMessage = std::string("Library: inserted") +
                (booted ? " + booted" : " (slot did not boot — cold-booted)") +
                " (slot " + std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + "): " + path;
        } else {
            tapeStatusMessage = "Library: boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request525InsertOnly.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        std::string err = "no DiskII card";
        if (target && pom2::mountDiskII(*controller, *target, drive,
                                        r.request525InsertOnly, err)) {
            tapeStatusMessage = "Library: inserted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + ", no boot): " +
                r.request525InsertOnly;
        } else {
            tapeStatusMessage = "Library: insert failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── 3.5" actions ─────────────────────────────────────────────────
    // Routing: on //c+ profile the on-board hub owns 3.5" media; on any
    // other profile with a SmartPort card plugged, the card's units do.
    // The Library click is explicit user intent to mount 3.5" here, so
    // we auto-create a SmartPort35Unit on the target index if the slot
    // is empty or holds a different kind (HDV) — the user can re-pick
    // the type later from the SmartPort Configuration panel.
    // routeMount35 / routeMountHdv are now member methods (shared with the
    // CLI insert+boot path) — see their definitions above.

    if (!r.request35MountAndBoot.empty()) {
        std::string err;
        if (routeMount35(r.request35BootDrive,
                         r.request35MountAndBoot, err)) {
            // Slot-aware boot: explicit `bootFromSlot(N)` whenever a
            // SmartPort card is plugged — now including the //c-class
            // built-in SmartPort (slot 5). No SmartPort card at all means
            // the //c+ on-board Sony hub, whose IWM boot path POM2
            // deliberately does not model: the cold boot below restarts the
            // machine but never reaches the disk, so don't call it a boot.
            bool booted = false;
            if (primarySmartPortCard()) {
                booted = controller->bootFromSlot(primarySmartPortCard()->getSlot());
            } else {
                controller->coldBoot();
            }
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35BootDrive == 0 ? "1" : "2")
                + (booted ? " booted: " : " mounted (did not boot): ")
                + r.request35MountAndBoot;
        } else {
            tapeStatusMessage = "Library: 3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request35MountOnly.empty()) {
        std::string err;
        if (routeMount35(r.request35MountDrive,
                         r.request35MountOnly, err)) {
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35MountDrive == 0 ? "1" : "2")
                + " mounted: " + r.request35MountOnly;
        } else {
            tapeStatusMessage = "Library: 3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── HDV actions ──────────────────────────────────────────────────
    // Two routing targets: the legacy `ProDOSHardDiskCard` slot (if
    // plugged) OR a SmartPort card's unit 0 (if a SmartPort is plugged
    // but no HDV card is). The Library click is explicit intent to
    // mount HDV; on a SmartPort-only config, auto-create / replace
    // unit 0 with a SmartPortHdvUnit so users don't have to detour
    // through the SmartPort Configuration panel.
    if (!r.requestHdvMountAndBoot.empty()) {
        const std::string path = r.requestHdvMountAndBoot;
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(path, bootSlot, err)) {
            const bool booted = controller->bootFromSlot(bootSlot);
            tapeStatusMessage = "Library: HDV (slot " +
                std::to_string(bootSlot) +
                (booted ? ") booted: " : ") mounted, did not boot: ") + path;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.requestHdvMountOnly.empty()) {
        int bootSlot = 0;
        std::string err;
        if (routeMountHdv(r.requestHdvMountOnly, bootSlot, err)) {
            tapeStatusMessage = "Library: HDV mounted: " + r.requestHdvMountOnly;
        } else {
            tapeStatusMessage = "Library: HDV mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── Floppy Emu SD card (floppyemu/) ───────────────────────────────
    // The SD card is not a bay: it holds 5.25", 3.5" and Smartport images
    // side by side, because the device emulates all of them. So a click
    // here is FILE-driven and goes through the same helper the CLI
    // positional disk uses — which also auto-plugs an HDV card when the
    // saved config has none. (The OLED panel stays MODE-driven: that is
    // what a Floppy Emu *is*. See DiskLibrary_ImGui.h.)
    if (!r.requestFloppyEmuMountAndBoot.empty()) {
        const std::string path = r.requestFloppyEmuMountAndBoot;
        std::string err;
        tapeStatusMessage = insertAndBootImage(path, err)
            ? ("Library: Floppy Emu booted: " + path)
            : ("Library: Floppy Emu boot failed: " + err);
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.requestFloppyEmuMountOnly.empty()) {
        const std::string path = r.requestFloppyEmuMountOnly;
        std::string err;
        bool ok = false;
        switch (classifyDiskForSlot(path)) {
            case DiskSlotClass::Floppy525:
                if (primaryDiskII()) {
                    ok = pom2::mountDiskII(*controller, *primaryDiskII(), 0, path, err);
                } else {
                    err = "no Disk II card in the current config";
                }
                break;
            case DiskSlotClass::Sony35:
                ok = routeMount35(0, path, err);
                break;
            case DiskSlotClass::Hdv: {
                int bootSlot = 0;
                ok = routeMountHdv(path, bootSlot, err);
                break;
            }
            case DiskSlotClass::Unknown:
            default:
                err = "unrecognised disk image (extension/size)";
                break;
        }
        tapeStatusMessage = ok
            ? ("Library: Floppy Emu mounted: " + path)
            : ("Library: Floppy Emu mount failed: " + err);
        tapeStatusUntil = lastFrameTime + 4.0;
    }

    // ── Eject actions ─────────────────────────────────────────────────
    // 5.25": eject from whichever plugged DiskII holds the clicked
    // image. Match by path so multi-instance DiskII setups (the same
    // image plugged into two slots) all clear together.
    if (!r.request525EjectPath.empty()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        bool ok = true;
        std::string err;
        for (auto* c : diskIICards()) {
            if (!c) continue;
            for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                if (c->isDiskLoaded(d) &&
                    c->getDiskPath(d) == r.request525EjectPath) {
                    if (!c->ejectDisk(d)) {
                        ok = false;
                        err = c->getLastError(d);
                    }
                }
            }
        }
        tapeStatusMessage = ok ? "Library: 5.25\" disk ejected"
                               : "Library: 5.25\" eject failed: " + err;
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (r.request35EjectDrive >= 0) {
        // Through the coordinator so the eject follows the SAME routing the
        // mount did. This called controller->eject35() unconditionally, which
        // only ever touches the on-board pair — so with a SmartPort card
        // owning the media the button silently did nothing while the panel
        // went on showing the disk.
        const auto e = storageCoordinator_->ejectDisk35(
            *controller, *settings, r.request35EjectDrive);
        tapeStatusMessage = e.ok
            ? ("Library: 3.5\" drive " +
               std::string(r.request35EjectDrive == 0 ? "1" : "2") + " ejected")
            : ("Library: 3.5\" eject failed: " + e.error);
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.requestHdvEject) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            const bool ok = dev->ejectImage();
            if (ok) {
                hdvPath.clear();
                hdvStatus = "no image mounted";
            }
            tapeStatusMessage = ok ? "Library: HDV ejected"
                                   : "Library: HDV eject failed: " + dev->getLastError();
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
    }
}

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

void MainWindow::renderSmartPortPanelWindow()
{
    if (!show(pom2::PanelId::SmartPort)) return;

    // One acquisition for every unit row, with the card resolved from the live
    // bus inside it. The old code fetched a SmartPortUnit* and then reused it
    // across several INDEPENDENT lock acquisitions — snapshot, then type swap,
    // then write-back, then mount, then eject — so a slot rebuild between any
    // two of them left the rest writing through a freed unit.
    const auto snap = storageCoordinator_->captureSmartPortPanel(*controller);

    char title[64];
    if (snap.plugged) {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration (slot %d)", snap.slot);
    } else {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration");
    }

    const auto r = smartPortPanel->render(title, show(pom2::PanelId::SmartPort), snap);

    if (!snap.plugged) return;

    // Re-resolves the card under the lock, applies the whole frame's request
    // in one critical section, and saves settings after unlocking. Unit-type
    // replacement flushes the outgoing unit first, so a failed write-back
    // aborts the swap instead of destroying the dirty medium with it.
    const auto status =
        storageCoordinator_->applySmartPortPanel(*controller, *settings,
                                                 snap.slot, r);
    if (!status.message.empty()) {
        tapeStatusMessage = status.message;
        tapeStatusUntil   = lastFrameTime + status.visibleSeconds;
    }
}

bool MainWindow::plugFujiNetFromCli(int& slot, bool slotExplicit, bool serial,
                                    const std::string& serialDevice,
                                    int tcpPort, std::string& errOut)
{
    if (slot < 1 || slot > 7) { errOut = "slot must be 1-7"; return false; }
    if (pom2::profileConfig(activeProfile).noPhysicalSlots) {
        errOut = "the active //c-class profile has no physical expansion slots";
        return false;
    }

    auto  st  = controller->lockState();
    auto& bus = st.memory().slotBus();

    if (bus.peripheral(slot) != nullptr && !slotExplicit) {
        // The user never named a slot — 7 is only POM2's preference, and its
        // own first-run default puts a Le Chat Mauve there, so refusing here
        // would make the documented bare `--fujinet` fail on a stock install.
        // Fall back the way docs/fujinet_plan.md specifies. Downwards from 7:
        // the autostart scan walks slots high to low, so the highest free slot
        // is the one most likely to be reached before the Disk II in 6.
        int free = 0;
        for (int s = 7; s >= 1 && free == 0; --s)
            if (bus.peripheral(s) == nullptr) free = s;
        if (free == 0) {
            errOut = "every slot is occupied — free one, or name it with "
                     "--fujinet-slot";
            return false;
        }
        pom2::log().info("CLI", "--fujinet: slot " + std::to_string(slot) +
                                    " holds " +
                                    std::string(bus.peripheral(slot)->name()) +
                                    ", using free slot " + std::to_string(free));
        slot = free;
    }

    if (bus.peripheral(slot) != nullptr) {
        errOut = "slot " + std::to_string(slot) + " already holds " +
                 std::string(bus.peripheral(slot)->name()) +
                 " — pick a free slot with --fujinet-slot";
        return false;
    }

    if (!plugFujiNetUnlocked(st, slot, serial, serialDevice, tcpPort, errOut))
        return false;

    // Remember it so every later slot rebuild reproduces it — see the header.
    cliFujiNetSlot_       = slot;
    cliFujiNetSerial_     = serial;
    cliFujiNetSerialPath_ = serialDevice;
    cliFujiNetPort_       = tcpPort;
    return true;
}

bool MainWindow::plugFujiNetUnlocked(const pom2::StateAccess& st,
                                     int slot, bool serial,
                                     const std::string& serialDevice,
                                     int tcpPort, std::string& errOut)
{
    auto& bus = st.memory().slotBus();
    auto card = pom2::makeFujiNetCard(slot);
    card->setMemory(&st.memory());
    card->setCpu(&st.cpu());
    auto& link = card->transportLink();
    if (serial)
        link.setSerialMode(serialDevice, pom2::SerialPort::kDefaultBaud);
    else
        link.setTcpMode(static_cast<uint16_t>(tcpPort));

    std::string err;
    if (!link.start(err)) { errOut = err; return false; }

    bus.plug(slot, std::move(card));
    // Session-only (CLI --fujinet / drag-and-drop): the live bus shows it,
    // the plan does not claim it.
    return true;
}

void MainWindow::archiveNewPrinterPages()
{
    if (!imageWriter || !printerHistory || !printerHistory->isOpen()) return;

    const uint64_t ejected = static_cast<uint64_t>(imageWriter->sheetsEjected());
    if (ejected <= printerArchivedSheets_) return;

    // How many of those sheets are still reachable. The stack is capped, so a
    // burst of form feeds between two frames can push pages off it before we
    // ever see them — archive what is there and resynchronise rather than
    // pretending we captured everything.
    const uint64_t missed = ejected - printerArchivedSheets_;
    const size_t   have   = imageWriter->completedPageCount();
    const size_t   take   = static_cast<size_t>(std::min<uint64_t>(missed, have));
    const uint64_t irrecoverablyDropped = missed - take;
    size_t accepted = 0;

    for (size_t i = have - take; i < have; ++i) {
        std::string err;
        if (!printerHistory->addPage(imageWriter->completedPage(i),
                                     static_cast<int>(imageWriter->model()),
                                     static_cast<int>(imageWriter->ribbon()),
                                     imageWriter->paperWidthIn(),
                                     imageWriter->paperLengthIn(), err)) {
            pom2::log().warn("PrinterHistory", err);
            break;
        }
        ++accepted;
    }
    if (take < missed) {
        pom2::log().warn("PrinterHistory",
            "archived " + std::to_string(take) + " of " +
            std::to_string(missed) + " ejected sheets — the rest had already "
            "fallen off the printer's page stack");
    }
    // Advance only past sheets actually archived (plus sheets already fallen
    // off the bounded live stack). A transient index failure is retried next
    // frame instead of silently discarding the failed page and the rest of
    // the batch.
    printerArchivedSheets_ += irrecoverablyDropped + accepted;
}

void MainWindow::dumpScreenToPrinter()
{
    if (!imageWriter) return;

    // Snapshot the framebuffer under BOTH locks, exactly as saveScreenshot
    // does — `pixels()` can lazily run the composite demod, which writes
    // frame80 and races the AI control server's /screen handler otherwise.
    // Lock order stateMutex → demodMutex, never the other way.
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        w = display->width();
        h = display->height();
        if (w > 0 && h > 0) {
            const uint32_t* src = display->pixels();
            px.assign(src, src + static_cast<size_t>(w) * h);
        }
    }
    if (px.empty()) return;

    // The dump goes in as BYTES, through the printer's own ESC G parser —
    // never painted onto the page. So it obeys the ribbon, the pacing and the
    // paper, lands in the tray and the PDF export, and cannot drift from what
    // a period driver would have produced. See PrinterScreenDump.h.
    // Each head has its own graphics grammar, so the dump has to be built
    // for the one actually fitted — the Epson wants ESC * with a binary count
    // and bit 7 as the top dot, the C. Itoh family ESC G with four ASCII
    // digits and bit 0.
    std::vector<uint8_t> stream;
    if (imageWriter->model() == pom2::IwModel::EpsonFX80)
        pom2::buildScreenDumpEpson(px.data(), w, h, w,
                                   printerDumpOptions_, stream);
    else
        pom2::buildScreenDumpImageWriter(px.data(), w, h, w,
                                         printerDumpOptions_, stream);
    if (stream.empty()) return;

    imageWriter->queueBytes(stream.data(), stream.size());
    show(pom2::PanelId::ImageWriter) = true;  // the user asked to print; show the paper
}

void MainWindow::renderFujiNetPanelWindow()
{
    if (!show(pom2::PanelId::FujiNet)) return;

    // One acquisition for the snapshot, one for whatever the frame asked for.
    // What this replaces bound `auto& link = card->transportLink()` OUTSIDE
    // any lock and then wrote through that reference inside SIX separate
    // critical sections — a slot rebuild between any two of them left the rest
    // writing to a freed link — and applied the timeout change with no lock at
    // all.
    auto snap = networkCoordinator_->captureFujiNetPanel(*controller);

    // Both of these take the machine lock themselves, so they must stay
    // outside any guard: stateMutex is non-recursive.
    //
    // Outranked iff the arbitration picked something OTHER than this tap.
    snap.printerOutranked =
        snap.printerTap &&
        printerCoordinator_->captureHost(*controller).source !=
            pom2::PrinterCoordinator::SourceKind::FujiNet;
    snap.hostClockCard =
        devicePanelCoordinator_->captureInventory().clockPlugged();

    const auto r = fujiNetPanel->render("FujiNet", show(pom2::PanelId::FujiNet), snap);
    if (!snap.plugged) return;

    // The web-UI button has no portable "open a URL" helper in POM2, so the
    // coordinator surfaces the address on the status line instead.
    networkCoordinator_->applyFujiNetPanel(*controller, r);
}

void MainWindow::renderFloppyEmuWindow()
{
    if (!show(pom2::PanelId::FloppyEmu)) return;
    namespace fs = std::filesystem;
    using Mode = pom2::FloppyEmuMode;
    const Mode mode = floppyEmu->mode();

    auto baseName = [](const std::string& p) {
        return fs::path(p).filename().string();
    };
    auto human = [](uint64_t b) -> std::string {
        if (b == 0)              return std::string();
        if (b >= 1024 * 1024)    return std::to_string(b / (1024 * 1024)) + "M";
        if (b >= 1024)           return std::to_string(b / 1024) + "K";
        return std::to_string(b) + "B";
    };
    auto controllerReady = [&](Mode m) -> bool {
        switch (m) {
            case Mode::Disk525:   return primaryDiskII() != nullptr;
            case Mode::Disk35:
            case Mode::Unidisk35: return primarySmartPortCard() != nullptr ||
                                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
            case Mode::SmartportHD: return hdvDevice() != nullptr ||
                                           primarySmartPortCard() != nullptr;
        }
        return false;
    };
    auto controllerHint = [&](Mode m) -> std::string {
        switch (m) {
            case Mode::Disk525:
                return "No Disk II controller — add 'Disk II' in the Slot Manager.";
            case Mode::Disk35:
            case Mode::Unidisk35:
                return "No SmartPort/Liron controller for 3.5\" media.";
            case Mode::SmartportHD:
                return "No SmartPort or HDV controller for hard-disk media.";
        }
        return std::string();
    };
    auto insertedLabel = [&](Mode m) -> std::string {
        // Snapshot-under-lock: load state / paths are worker-mutable
        // (inserts can come from soft-switch-triggered write-back paths
        // and other panels). Lock released before any rendering.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        switch (m) {
            case Mode::Disk525:
                return (primaryDiskII() && primaryDiskII()->isDiskLoaded())
                           ? baseName(primaryDiskII()->getDiskPath()) : std::string();
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (primarySmartPortCard()) {
                    const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return controller->disk35Internal().isLoaded()
                           ? baseName(controller->disk35Internal().path())
                           : std::string();
            case Mode::SmartportHD:
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    return dev->isImageLoaded() ? baseName(dev->getImagePath())
                                                : std::string();
                if (primarySmartPortCard()) {
                    const pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return std::string();
        }
        return std::string();
    };
    auto mountImage = [&](const std::string& path, Mode m) {
        std::string err;
        int bootSlot = 0;
        // Selecting an image BOOTS it, like a left-click in the Disk Library
        // ("left-click = insert + boot"). Mounting alone left the user
        // staring at whatever was already on screen with a status line
        // telling them to go and reboot the machine themselves — the image
        // was in the drive and nothing had happened, which reads as "the
        // click did nothing". Routing stays MODE-driven, not
        // extension-driven: the Floppy Emu emulates the device its mode
        // says, so a .2mg selected in Smartport mode must boot from the
        // SmartPort slot even though insertAndBootImage would classify the
        // same file by its extension. -1 = mount succeeded but no explicit
        // boot slot (cold-boot instead); -2 = nothing mounted.
        constexpr int kNoMount   = -2;
        constexpr int kColdBoot  = -1;
        int bootTarget = kNoMount;
        switch (m) {
            case Mode::Disk525: {
                if (!primaryDiskII()) { floppyEmuStatus = controllerHint(m); break; }
                // Two-phase (MediaMount): the decode runs unlocked, and the
                // lock is taken only to swap the track buffers the worker's
                // LSS streams from. seekTrack0 parks the head before the boot
                // PROM reads — the same step insertAndBootImage does.
                std::string mountErr;
                const bool ok = pom2::mountDiskII(*controller, *primaryDiskII(), 0,
                                                  path, mountErr,
                                                  /*seekTrack0=*/true);
                if (ok) bootTarget = primaryDiskII()->getSlot();
                floppyEmuStatus = ok
                    ? ("Booting " + baseName(path))
                    : ("5.25 mount failed: " + baseName(path));
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (!controllerReady(m)) ensureSmartPortCardForBoot();
                if (routeMount35(0, path, err)) {
                    // With a SmartPort card, boot its slot explicitly;
                    // without one the mount landed on the //c+ on-board hub,
                    // which has no slot to jump to — cold boot instead.
                    bootTarget = primarySmartPortCard() ? primarySmartPortCard()->getSlot()
                                               : kColdBoot;
                    floppyEmuStatus = "Booting " + baseName(path);
                } else {
                    floppyEmuStatus = "3.5\" mount failed: " + err;
                }
                break;
            case Mode::SmartportHD:
                if (!controllerReady(m)) ensureSmartPortCardForBoot();
                if (routeMountHdv(path, bootSlot, err)) {
                    // bootSlot is what routeMountHdv resolved. It was being
                    // filled and then dropped on the floor here.
                    bootTarget = bootSlot;
                    floppyEmuStatus = "Booting " + baseName(path);
                } else {
                    floppyEmuStatus = "Smartport mount failed: " + err;
                }
                break;
        }
        if (bootTarget != kNoMount) {
            if (bootTarget == kColdBoot) controller->coldBoot();
            else                        controller->bootFromSlot(bootTarget);
            controller->setMode(EmulationController::Mode::Running);
        }
    };
    auto ejectCurrent = [&](Mode m) {
        bool ok = false;
        std::string err;
        switch (m) {
            case Mode::Disk525: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (primaryDiskII()) {
                    ok = primaryDiskII()->ejectDisk();
                    if (!ok) err = primaryDiskII()->getLastError();
                }
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (primarySmartPortCard()) {
                    std::lock_guard<std::mutex> lk(controller->stateMutex());
                    if (pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0)) {
                        ok = u->eject();
                        if (!ok) err = u->lastError();
                    }
                } else {
                    ok = controller->eject35(0);  // re-locks the state mutex itself
                    if (!ok) err = controller->disk35Internal().lastError();
                }
                break;
            case Mode::SmartportHD: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
                    ok = dev->ejectImage();
                    if (!ok) err = dev->getLastError();
                }
                else if (primarySmartPortCard()) {
                    if (pom2::SmartPortUnit* u = primarySmartPortCard()->unit(0)) {
                        ok = u->eject();
                        if (!ok) err = u->lastError();
                    }
                }
                break;
            }
        }
        floppyEmuStatus = ok ? "Ejected" : "Eject failed: " + err;
    };

    // ── Build the snapshot. ──────────────────────────────────────────────
    pom2::FloppyEmu_ImGui::Snapshot snap;
    snap.modeLabel = pom2::FloppyEmuDevice::modeLabel(mode);
    snap.sdPresent = floppyEmu->sdPresent();
    snap.sdRootDisplay = floppyEmu->sdRoot();
    {
        const std::string cur = floppyEmu->currentDir();
        const std::string root = floppyEmu->sdRoot();
        snap.dirLabel = (cur.size() >= root.size() &&
                         cur.compare(0, root.size(), root) == 0)
                            ? cur.substr(root.size()) : cur;
    }
    const auto fav = floppyEmu->favorites();
    snap.favoritesAvailable = fav.present;
    snap.favoritesActive    = floppyEmuFavActive_ && fav.present;
    if (snap.favoritesActive) {
        for (const auto& e : fav.entries) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.sublabel = human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    } else {
        for (const auto& e : floppyEmu->listing()) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.isDir = e.isDir; it.isUp = e.isUp;
            it.sublabel = e.isDir ? "DIR" : human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    }
    snap.controllerReady = controllerReady(mode);
    snap.controllerHint  = controllerHint(mode);
    snap.insertedLabel   = insertedLabel(mode);
    snap.statusLine      = floppyEmuStatus;
    for (Mode m : pom2::FloppyEmuDevice::allModes()) {
        snap.modeOptions.push_back(pom2::FloppyEmuDevice::modeLabel(m));
        if (m == mode) snap.currentModeIndex =
            static_cast<int>(snap.modeOptions.size()) - 1;
    }

    const auto r = floppyEmuPanel->render("Floppy Emu (BMOW)", show(pom2::PanelId::FloppyEmu), snap);

    // ── Apply actions. ───────────────────────────────────────────────────
    if (r.setModeIndex >= 0) {
        const auto modes = pom2::FloppyEmuDevice::allModes();
        if (r.setModeIndex < static_cast<int>(modes.size())) {
            floppyEmu->setMode(modes[r.setModeIndex]);
            floppyEmuFavActive_ = false;
            floppyEmuStatus = std::string("Mode: ") +
                pom2::FloppyEmuDevice::modeLabel(modes[r.setModeIndex]);
        }
    }
    if (r.toggleFavorites) floppyEmuFavActive_ = !floppyEmuFavActive_;
    if (r.requestConfigureController) {
        if (mode == Mode::Disk525)
            floppyEmuStatus = "Add a Disk II card via the Slot Manager (Apply restarts).";
        else {
            const int s = ensureSmartPortCardForBoot();
            floppyEmuStatus = (s >= 0)
                ? ("Added SmartPort card in slot " + std::to_string(s))
                : "No free slot for a SmartPort card.";
        }
    }
    if (r.requestEject) ejectCurrent(mode);
    if (r.activateIndex >= 0) {
        if (snap.favoritesActive) {
            if (r.activateIndex < static_cast<int>(fav.entries.size()))
                mountImage(fav.entries[r.activateIndex].fullPath, mode);
        } else {
            const auto items = floppyEmu->listing();
            if (r.activateIndex < static_cast<int>(items.size())) {
                const auto& e = items[r.activateIndex];
                if (e.isDir || e.isUp) floppyEmu->enterDir(e);
                else                   mountImage(e.fullPath, mode);
            }
        }
    }
}

void MainWindow::renderHdvPanelWindow()
{
    if (!show(pom2::PanelId::Hdv)) return;

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (primaryHdvCard()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.imageLoaded       = primaryHdvCard()->isImageLoaded();
        snap.imagePath         = primaryHdvCard()->getImagePath();
        snap.blockCount        = primaryHdvCard()->getBlockCount();
        snap.writeBackEnabled  = primaryHdvCard()->isWriteBackEnabled();
        snap.hasUnsavedChanges = primaryHdvCard()->hasUnsavedChanges();
        snap.supportsWriteBack = primaryHdvCard()->canWriteBack();
        snap.isSynthVolume     = primaryHdvCard()->isSynthVolumeMounted();
    }

    // Library scan — hdv/ for .hdv and .2mg, sorted alphabetically so the
    // list stays stable across frames regardless of dirent order. Plus a
    // synthetic entry for prodos_folder/ if that folder exists (host-folder
    // mount: contents are synthesised into a read-only ProDOS volume on
    // click, see kProDOSHostSentinel below).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "hdv", "../hdv", "../../hdv" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            // Recursive walk so users can shelve images by collection /
            // OS / target machine (`hdv/prodos/`, `hdv/iigs/`, …) and
            // still get one-click mount. See the Disk II library
            // comment above for the rationale and ignore rules.
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".hdv" && ext != ".2mg") continue;
                pom2::HdvController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });

        // Synthetic entry for the host-folder mount.
        const char* prodosCandidates[] = {
            "prodos_folder", "../prodos_folder", "../../prodos_folder"
        };
        std::string prodosDir;
        for (const char* d : prodosCandidates) {
            if (fs::is_directory(d, ec)) { prodosDir = d; break; }
        }
        if (!prodosDir.empty()) {
            std::size_t fileCount = 0;
            for (const auto& e : fs::directory_iterator(prodosDir, ec)) {
                if (e.is_regular_file()) ++fileCount;
            }
            pom2::HdvController_ImGui::LibraryEntry e;
            e.displayName = "[host folder] " + prodosDir + "/  ("
                          + std::to_string(fileCount) + " files)";
            e.fullPath    = std::string(kProDOSHostSentinel) + prodosDir;
            snap.library.push_back(std::move(e));
        }
    }

    // HDV = bottom-left panel in the curated layout (under the Screen).
    ImGui::SetNextWindowPos (ImVec2(5,    600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 390), ImGuiCond_FirstUseEver);
    // Title reflects the actual slot the HDV card is plugged in.
    char hdvTitle[48];
    std::snprintf(hdvTitle, sizeof(hdvTitle), "HDV (slot %d)",
                  primaryHdvCard() ? primaryHdvCard()->getSlot() : 5);
    auto result = hdvPanel->render(hdvTitle, show(pom2::PanelId::Hdv), snap);

    if (result.writeBackToggleChanged && primaryHdvCard()) {
        // Persists the hdv_writeback key with the change; the bare setter did
        // not, so the toggle was forgotten at the next launch.
        (void)storageCoordinator_->setMediaBayWriteBack(
            *controller, *settings, primaryHdvCard()->getSlot(), 0,
            result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : "HDV: write-back disabled";
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestEject && primaryHdvCard()) {
        const auto r = storageCoordinator_->ejectMediaBay(
            *controller, *settings, primaryHdvCard()->getSlot(), 0);
        if (r.ok) hdvStatus = "no image mounted";
        tapeStatusMessage = r.ok ? "HDV ejected"
                                 : "HDV eject failed: " + r.error;
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestBoot && primaryHdvCard()) {
        bootHdvImage();
    }
    if (!result.requestMountAndBoot.empty() && primaryHdvCard()) {
        const std::string path = result.requestMountAndBoot;
        const std::string sentinel(kProDOSHostSentinel);

        if (path.rfind(sentinel, 0) == 0) {
            // Host-folder mount: synthesise a read-only ProDOS volume from
            // the folder contents and load it into the slot 5 card. We do
            // NOT auto-boot — block 0 is zero, so the volume isn't
            // bootable. The user boots ProDOS from elsewhere (Disk II or
            // an HDV) and ProDOS then sees /HOST/ as a second drive.
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = primaryHdvCard()->loadImageFromBytes(std::move(bytes),
                                                 std::string("[host folder] ") + hostDir,
                                                 hostDir);
                if (ok) {
                    hdvPath   = path;
                    hdvStatus = std::string("synth from ") + hostDir +
                                " (" + std::to_string(br.filesIncluded) + " files)";
                } else {
                    hdvStatus = "synth load failed";
                }
            }
            if (ok) {
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                    "/HOST/ mounted from %s (%zu files, %zu skipped, %zu blocks). Boot ProDOS from another drive.",
                    hostDir.c_str(), br.filesIncluded, br.filesSkipped, br.totalBlocks);
                tapeStatusMessage = msg;
                pom2::log().info("HDV",
                    std::string("Synthesised volume from ") + hostDir +
                    " (" + std::to_string(br.filesIncluded) + " files, " +
                    std::to_string(br.totalBlocks) + " blocks)");
            } else {
                tapeStatusMessage = "Synth load failed";
            }
            tapeStatusUntil = lastFrameTime + 8.0;
            return;
        }

        // Real .hdv / .2mg / .po file: load under the lock so the card
        // has the right blocks before bootFromSlot wipes RAM and jumps
        // PC = $C(N)00 (where N is the slot the card actually lives in).
        // Two-step lock is safe — the CPU worker only resumes when
        // bootFromSlot flips mode to Running.
        bool ok = false;
        std::string err;
        {
            // Through the coordinator: same two-phase read (32 MiB off the
            // lock), plus the hdv_path key written with the mount. The bare
            // helper left the key stale, so the panel and settings disagreed
            // until the next shutdown.
            {
                const auto r = storageCoordinator_->mountMediaBay(
                    *controller, *settings, primaryHdvCard()->getSlot(), 0,
                    path);
                ok  = r.ok;
                err = r.error;
            }
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            controller->bootFromSlot(primaryHdvCard()->getSlot());
            pom2::log().info("HDV",
                "slot " + std::to_string(primaryHdvCard()->getSlot()) +
                " library click → mount + boot: " + path);
            tapeStatusMessage = "Mounting + booting HDV (slot " +
                std::to_string(primaryHdvCard()->getSlot()) + "): " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!result.requestMountOnly.empty() && primaryHdvCard()) {
        // Right-click "mount only": swap the image without booting.
        // Host-folder sentinel is handled the same as mount-and-boot
        // above (it never auto-boots anyway), so funnel both branches
        // here when no Apple II reset is wanted.
        const std::string path = result.requestMountOnly;
        const std::string sentinel(kProDOSHostSentinel);

        bool ok = false;
        std::string err;
        if (path.rfind(sentinel, 0) == 0) {
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                tapeStatusMessage = "ProDOS synth failed: " + br.error;
                tapeStatusUntil   = lastFrameTime + 5.0;
                return;
            }
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = primaryHdvCard()->loadImageFromBytes(std::move(bytes),
                                             std::string("[host folder] ") + hostDir,
                                             hostDir);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("synth from ") + hostDir;
            } else {
                err = "synth load failed";
                hdvStatus = err;
            }
        } else {
            // Through the coordinator: same two-phase read (32 MiB off the
            // lock), plus the hdv_path key written with the mount. The bare
            // helper left the key stale, so the panel and settings disagreed
            // until the next shutdown.
            {
                const auto r = storageCoordinator_->mountMediaBay(
                    *controller, *settings, primaryHdvCard()->getSlot(), 0,
                    path);
                ok  = r.ok;
                err = r.error;
            }
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            pom2::log().info("HDV",
                std::string("Library right-click → mount only: ") + path);
            tapeStatusMessage = "Mounted (no boot): " + path;
        } else {
            tapeStatusMessage = "Mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    // Find the panel that currently has its insertDialogOpen flag set.
    // With option C (multi-instance DiskII), any of the per-card panels
    // could have triggered the popup via its "Insert .dsk..." button —
    // we route the eventual insertDisk() to the corresponding card.
    pom2::DiskController_ImGui* triggeredPanel = nullptr;
    DiskIICard*                 triggeredCard  = nullptr;
    const auto diskCardList = diskIICards();
    for (size_t i = 0; i < diskPanels.size() && i < diskCardList.size(); ++i) {
        if (diskPanels[i] && diskPanels[i]->insertDialogOpen) {
            triggeredPanel = diskPanels[i].get();
            triggeredCard  = diskCardList[i];
            break;
        }
    }
    // Top-level "Insert disk image..." menu (no per-panel context) routes
    // to the primary card by convention.
    if (!triggeredPanel && diskPanel && diskPanel->insertDialogOpen) {
        triggeredPanel = diskPanel;
        triggeredCard  = primaryDiskII();
    }

    if (triggeredPanel) {
        ImGui::OpenPopup("Insert disk image");
        triggeredPanel->insertDialogOpen = false;
        // Remember which card the popup routes to until the user clicks
        // Insert / Cancel. ImGui modal state survives between frames so
        // the pointer needs to survive too.
        diskDialogTargetSlot = triggeredCard ? triggeredCard->getSlot() : -1;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    // Resolve the target card via the saved slot — the panel pointer may
    // have moved (rare profile-switch races), but the slot number is
    // stable until plugSlotsFromSettings rebuilds.
    DiskIICard*                 popupCard  = nullptr;
    pom2::DiskController_ImGui* popupPanel = nullptr;
    const auto popupCardList = diskIICards();
    for (size_t i = 0; i < popupCardList.size(); ++i) {
        if (popupCardList[i] && popupCardList[i]->getSlot() == diskDialogTargetSlot) {
            popupCard  = popupCardList[i];
            popupPanel = (i < diskPanels.size()) ? diskPanels[i].get() : nullptr;
            break;
        }
    }
    if (!popupPanel) popupPanel = diskPanel;
    if (!popupCard)  popupCard  = primaryDiskII();

    if (popupCard) {
        ImGui::Text("Target: Disk II slot %d", popupCard->getSlot());
    }
    ImGui::TextUnformatted("Path to a 5.25\" image —"
                           " .dsk / .do (DOS 3.3, 143 360 B) or"
                           " .po (ProDOS, 143 360 B) or .nib (raw"
                           " nibble stream, 232 960 B) or .woz"
                           " (bit-cell, copy-protected disks; read-only)."
                           " Write-back is opt-in via the panel checkbox.");
    if (popupPanel) {
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", popupPanel->dialogPath.c_str());
        if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            popupPanel->dialogPath = buf;
        else
            popupPanel->dialogPath = buf;
    }

    // Quick list of disk images in disks_5.4/ (mirrors the cassette dialog).
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_5.4", "../disks_5.4", "../../disks_5.4" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                ext != ".nib" && ext != ".woz") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()) && popupPanel)
                popupPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (popupCard && popupPanel && !popupPanel->dialogPath.empty()) {
            std::string err;
            if (pom2::mountDiskII(*controller, *popupCard, 0,
                                  popupPanel->dialogPath, err)) {
                tapeStatusMessage = "Disk inserted (slot " +
                    std::to_string(popupCard->getSlot()) + "): " +
                    popupPanel->dialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::renderHdvFileDialog()
{
    if (hdvPanel->mountDialogOpen) {
        ImGui::OpenPopup("Mount HDV image");
        hdvPanel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount HDV image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("ProDOS block-device image — .hdv (raw blocks)"
                           " or .2mg (with 2IMG header, ProDOS order)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", hdvPanel->dialogPath.c_str());
    if (ImGui::InputText("##HdvPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        hdvPanel->dialogPath = buf;
    else
        hdvPanel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "hdv", "../hdv", "../../hdv" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".hdv" && ext != ".2mg") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                hdvPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    const bool canMount = primaryHdvCard() && !hdvPanel->dialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        std::string mountErr;
        const auto r = storageCoordinator_->mountMediaBay(
            *controller, *settings, primaryHdvCard()->getSlot(), 0,
            hdvPanel->dialogPath);
        mountErr = r.error;
        if (r.ok) {
            hdvPath   = hdvPanel->dialogPath;
            hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            tapeStatusMessage = "HDV mounted: " + hdvPanel->dialogPath;
        } else {
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV mount failed: " + mountErr;
        }
        tapeStatusUntil = lastFrameTime + 5.0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mount and Boot", ImVec2(160, 0))) {
        bool ok = false;
        {
            std::string mountErr;
            const auto r = storageCoordinator_->mountMediaBay(
                *controller, *settings, primaryHdvCard()->getSlot(), 0,
                hdvPanel->dialogPath);
            ok = r.ok;
            mountErr = r.error;
            if (ok) {
                hdvPath   = hdvPanel->dialogPath;
                hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            } else {
                hdvStatus = "no image mounted";
                tapeStatusMessage = "HDV mount failed: " + mountErr;
                tapeStatusUntil   = lastFrameTime + 5.0;
            }
        }
        if (ok) bootHdvImage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── //c+ SmartPort 3.5" ─────────────────────────────────────────────────

namespace {
// Where a 3.5" WOZ's writable twin should go: same directory, same stem,
// `.po`. Never overwrites — appends " (2)", " (3)" … until the name is free,
// and gives up rather than looping if a hundred already exist. Returns ""
// when no free name could be found, which greys the button out.
std::string freePoNameFor(const std::string& wozPath)
{
    if (wozPath.empty()) return {};
    namespace fs = std::filesystem;
    const fs::path src(wozPath);
    const fs::path dir  = src.parent_path();
    const std::string stem = src.stem().string();
    std::error_code ec;
    for (int n = 1; n <= 99; ++n) {
        const std::string name =
            (n == 1) ? stem + ".po"
                     : stem + " (" + std::to_string(n) + ").po";
        const fs::path cand = dir / name;
        if (!fs::exists(cand, ec)) return cand.string();
    }
    return {};
}
}  // namespace

bool MainWindow::convertWoz35ToPo(int drive, bool /*useSmartPort35*/)
{
    // The way out of a read-only 3.5" WOZ, and the reason it exists: POM2
    // decodes Sony GCR but cannot encode it, so a `.woz` mounted at 800K can
    // never take a guest write — which breaks any program that keeps its
    // configuration on its own program disk (The New Print Shop's printer
    // setup is the canonical case). The decode already produced the exact
    // 1600 blocks a `.po` holds, so the conversion is a file write and
    // nothing more. The WOZ is never touched: it stays the archival master.
    //
    // The coordinator picks the source image by the same routing rule as
    // mount and eject, writes the `.po`, re-mounts it in place of the WOZ and
    // persists the new path — so the drive the panel is showing is the drive
    // that gets converted. The routing argument is ignored and kept only so
    // the call sites need not change.
    const auto r = storageCoordinator_->convertDisk35WozToPo(*controller,
                                                             *settings, drive);
    if (!r.ok) {
        tapeStatusMessage = "3.5\" convert failed: " + r.error;
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    // The memoised convert-target name is stale the moment the medium
    // changes: the drive now holds the .po, not the WOZ it was computed from.
    convertSrc_[drive].clear();
    convertDst_[drive].clear();
    tapeStatusMessage = "3.5\" drive " + std::string(drive == 0 ? "1" : "2") +
                        " converted to " + r.outputPath + " (now writable)";
    tapeStatusUntil   = lastFrameTime + 6.0;
    return true;
}

void MainWindow::renderDisk35PanelWindow()
{
    if (!show(pom2::PanelId::Disk35)) return;

    pom2::Disk35Controller_ImGui::PanelSnapshot snap;
    // 3.5" is "supported" by the //c+ profile (on-board SmartPort + MIG) OR by
    // ANY profile where the user plugged a SmartPort 3.5" card (//e +
    // Liron-class). Both paths share the same Disk35Image objects, so the
    // panel does not have to care which mux is talking.
    snap.supportedByProfile =
        (activeProfile == pom2::SystemProfile::AppleIIcPlus) ||
        (primarySmartPortCard() != nullptr);

    // One acquisition, and — this is the behaviour change — the SAME source
    // rule the mount path uses: a plugged SmartPort card owns the 3.5" media,
    // whatever the profile. The panel used to exclude //c+ from that branch
    // and read the on-board hub instead, while routeMount35 sent the media to
    // the card's units regardless. So on //c+ the panel showed two empty
    // on-board drives over media that was really in the card, and eject and
    // write-back hit the wrong object.
    const auto d35 = storageCoordinator_->captureDisk35(*controller);
    for (int i = 0; i < 2; ++i) {
        const auto& src = d35.drives[i];
        auto& dst = snap.drives[i];
        dst.diskLoaded        = src.loaded;
        dst.motorOn           = src.motorOn;
        dst.track             = src.track;
        dst.side1             = src.side1;
        dst.writeProtected    = src.writeProtected;
        dst.diskPath          = src.path;
        dst.lastError         = src.lastError;
        dst.hasUnsavedChanges = src.hasUnsavedChanges;
        dst.writeBackEnabled  = src.writeBackEnabled;
        dst.isWoz             = src.isWoz;
    }

    // Convert-target names stay memoised HERE rather than taken from the
    // snapshot. The coordinator computes them outside its lock (so they never
    // block the CPU worker), but it recomputes on every capture, and
    // `freePoNameFor` stats the filesystem up to 99 times when earlier
    // candidates are taken — this panel re-snapshots every frame. The answer
    // only changes when the medium changes, so the path is the whole key.
    for (int i = 0; i < 2; ++i) {
        auto& s = snap.drives[i];
        if (!s.isWoz) { convertSrc_[i].clear(); convertDst_[i].clear(); continue; }
        if (convertSrc_[i] != s.diskPath) {
            convertSrc_[i] = s.diskPath;
            convertDst_[i] = freePoNameFor(s.diskPath);
        }
        s.convertTargetPath = convertDst_[i];
    }

    // Library scan — mirrors the Disk II library scan but only picks
    // up files large enough to be 800K (size sniff via filesystem).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5",
                                 "disks_5.4",   "../disks_5.4",   "../../disks_5.4" }) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".po" && ext != ".2mg" && ext != ".woz") continue;
                const auto sz = entry.file_size(ec);
                if (ec) continue;
                // 800K raw or 2IMG-wrapped (header + 819 200). A `.woz` is
                // FLUX, so its size says nothing about the payload — the
                // 3.5" loader decodes it and refuses a 5.25" one by name.
                if (ext != ".woz" &&
                    sz != 819200 && sz != 819200 + 64 &&
                    !(sz > 819200 && sz < 819200 + 4096)) continue;
                pom2::Disk35Controller_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            if (!snap.library.empty()) break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    ImGui::SetNextWindowPos (ImVec2(1055, 30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(705,  600), ImGuiCond_FirstUseEver);
    // Title reflects where the SmartPort path lives: on-board on //c+,
    // or the explicit slot of the plugged Liron-class card on other
    // profiles. Stable ImGui window-id per slot so the user's position/
    // size choices are remembered per-configuration.
    char disk35Title[64];
    if (primarySmartPortCard()) {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (slot %d)", primarySmartPortCard()->getSlot());
    } else {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (//c+ on-board)");
    }
    auto result = disk35Panel->render(
        disk35Title, show(pom2::PanelId::Disk35), snap);

    if (result.requestConvertDrive >= 0)
        convertWoz35ToPo(result.requestConvertDrive, d35.usesSmartPort());

    for (int d = 0; d < 2; ++d) {
        if (result.requestEject[d]) {
            // Routed like the mount: the coordinator decides whether the
            // medium lives in a SmartPort unit or the on-board pair, ejects
            // there and clears the matching settings key. The two branches
            // here duplicated that decision and only the SmartPort one
            // persisted, so an on-board eject came back on the next launch.
            const auto e = storageCoordinator_->ejectDisk35(*controller,
                                                            *settings, d);
            tapeStatusMessage = e.ok
                ? (std::string("3.5\" drive ") +
                   (d == 0 ? "1 (internal)" : "2 (external)") + " ejected")
                : ("3.5\" eject failed: " + e.error);
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        // Per-drive write-back toggle. The coordinator applies it under the
        // machine lock — a save-on-eject race against the worker must not
        // half-flip the flag — and persists after unlocking. The on-board
        // branch here never wrote a settings key at all, so that toggle was
        // forgotten every launch.
        if (result.requestWriteBackToggle[d]) {
            (void)storageCoordinator_->setDisk35WriteBack(
                *controller, *settings, d, result.newWriteBack[d]);
            tapeStatusMessage = std::string("3.5\" drive ")
                + (d == 0 ? "1" : "2")
                + (result.newWriteBack[d]
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
    }
    if (result.openMountDialog) {
        disk35Panel->mountDialogOpen     = true;
        disk35Panel->mountDialogForDrive = result.openMountDialogForDrive;
        if (disk35Panel->dialogPath.empty()) disk35Panel->dialogPath = "disks_3.5/";
    }
    if (!result.requestMountPath.empty()) {
        // routeMount35 sends the image to the SmartPort card's unit on
        // non-//c+ profiles, or to the on-board hub on //c+ — the same
        // routing the Disk Library + CLI use. Keeps the standalone panel
        // and the library in lock-step.
        std::string err;
        if (routeMount35(result.requestMountDrive, result.requestMountPath, err)) {
            tapeStatusMessage = "3.5\" mounted: " + result.requestMountPath;
        } else {
            tapeStatusMessage = "3.5\" mount failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    // Library left-click default = mount + cold boot. The //c+ ROM's
    // power-on probe scans SmartPort devices in order and boots the
    // first ready volume, so `coldBoot()` is enough — no need to
    // pre-set PC. On non-//c+ profiles `mount35` succeeds (the image
    // sits idle in Sony35Drive) but no device walker exists to read
    // it, so we still cold-boot but the user sees the Applesoft
    // prompt instead of the new image's loader.
    if (!result.requestInsertAndBoot.empty()) {
        const int d = result.insertAndBootDrive;
        std::string err;
        if (routeMount35(d, result.requestInsertAndBoot, err)) {
            // Prefer an explicit `bootFromSlot(N)` when the SmartPort
            // path is provided by a slot card on a non-//c+ profile —
            // the user picked the slot in Slot Configuration and the
            // PR#N landing should follow that. On //c+ on-board, fall
            // back to `coldBoot()` so the ROM autostart picks up the
            // built-in SmartPort firmware.
            if (d35.usesSmartPort()) {
                controller->bootFromSlot(d35.smartPortSlot);
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted (slot " + std::to_string(d35.smartPortSlot)
                    + "): " + result.requestInsertAndBoot;
            } else {
                controller->coldBoot();
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted: " + result.requestInsertAndBoot;
            }
        } else {
            tapeStatusMessage = "3.5\" boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDisk35FileDialog()
{
    if (disk35Panel->mountDialogOpen) {
        ImGui::OpenPopup("Mount 3.5\" image");
        disk35Panel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount 3.5\" image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Target drive: %s",
                disk35Panel->mountDialogForDrive == 0
                    ? "1 (internal, //c+ on-board)"
                    : "2 (external, SmartPort daisy-chain)");
    ImGui::TextUnformatted("800K Sony 3.5\" image — .po (raw ProDOS blocks,"
                           " 819 200 B) or .2mg (with 2IMG header).");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", disk35Panel->dialogPath.c_str());
    if (ImGui::InputText("##Disk35Path", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        disk35Panel->dialogPath = buf;
    else
        disk35Panel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".po" && ext != ".2mg" && ext != ".woz") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                disk35Panel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        if (!disk35Panel->dialogPath.empty()) {
            // routeMount35 dispatches to the SmartPort card unit (non-//c+)
            // or the on-board hub (//c+), matching the panel's read source.
            std::string err;
            if (routeMount35(disk35Panel->mountDialogForDrive,
                             disk35Panel->dialogPath, err)) {
                tapeStatusMessage = "3.5\" mounted: " + disk35Panel->dialogPath;
            } else {
                tapeStatusMessage = "3.5\" mount failed: " + err;
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── Apple //e keyboard (clickable photo) ────────────────────────────────

void MainWindow::ensureKeyboardImageLoaded()
{
    if (kbImageTried_) return;
    kbImageTried_ = true;

    const std::string path = pom2::findResource("pic/Keyboard_AppleIIe.jpeg");
    if (path.empty()) {
        kbImageError_ = "not found in any resource search dir";
        return;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        kbImageError_ = stbi_failure_reason() ? stbi_failure_reason()
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

    kbImageTex_ = tex;
    kbImageW_   = w;
    kbImageH_   = h;
}

void MainWindow::pushAppleKeys()
{
    // $C061/$C062 bit 7 is one wire with two things pressing it: the host's
    // Left/Right Alt and the on-screen keyboard's latches. Either alone is
    // enough, so the sources are OR'd rather than assigned — an assignment
    // from one source silently releases the other, which is how the panel
    // used to disable Alt for the whole session (it runs every frame).
    // `openAppleKey`/`solidAppleKey` are atomics, so no lock is needed
    // (CLAUDE.md's unlocked-Memory carve-out).
    controller->memory().setOpenAppleKey (appleKeys_.openApple());
    controller->memory().setSolidAppleKey(appleKeys_.solidApple());
}

void MainWindow::renderKeyboardPanel()
{
    if (!show(pom2::PanelId::Keyboard)) {
        // A latched Open-Apple must not outlive the window that shows it as
        // down: with the panel closed there is nothing to un-latch it with,
        // and the guest would see a key held forever.
        //
        // EDGE-TRIGGERED on the close, not run every frame the window is
        // shut. `keyboardPanel` is never destroyed once built, so the
        // unconditional form kept firing for the rest of the session — and
        // since it wrote $C061/$C062 directly, it also stamped the host's
        // Left/Right Alt back to false 60x/s. Dropping only THIS source and
        // letting `pushAppleKeys()` re-OR is what keeps Alt working.
        if (keyboardPanel && kbPanelWasOpen_) {
            keyboardPanel->releaseAll();
            appleKeys_.releasePanel();
            pushAppleKeys();
        }
        kbPanelWasOpen_ = false;
        return;
    }
    kbPanelWasOpen_ = true;
    ensureKeyboardImageLoaded();
    if (!keyboardPanel)
        keyboardPanel = std::make_unique<pom2::Keyboard_ImGui>();

    const auto ev = keyboardPanel->render(&show(pom2::PanelId::Keyboard), kbImageTex_,
                                          kbImageW_, kbImageH_, kbImageError_);

    // The Apple keys are LEVELS, not events: $C061/$C062 bit 7 reads the
    // switch, so the latch has to be pushed every frame for as long as it is
    // down, exactly like the host's Left/Right Alt in onKey.
    const auto& lat = keyboardPanel->latches();
    appleKeys_.setPanel(lat.openApple, lat.solidApple);
    pushAppleKeys();

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
                    tapeStatusMessage =
                        "Reset needs Control — latch CONTROL, then click Reset "
                        "(Open-Apple too for a cold boot).";
                    tapeStatusUntil = lastFrameTime + 6.0;
                    break;
                }
                // Open-Apple+Ctrl+Reset is the //e's cold boot; Ctrl+Reset
                // alone is the warm one. Same two verbs as F12 / F11.
                if (ev.latches.openApple) {
                    controller->hardReset();
                    tapeStatusMessage = "Open-Apple + Ctrl + Reset — cold boot";
                } else {
                    controller->softReset();
                    tapeStatusMessage = "Ctrl + Reset";
                }
                tapeStatusUntil  = lastFrameTime + 3.0;
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
    if (aboutImageTried_) return;
    aboutImageTried_ = true;

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

    aboutImageTex_ = tex;
    aboutImageW_   = w;
    aboutImageH_   = h;
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
            tapeStatusMessage = "Dropped + booted: " +
                std::filesystem::path(path).filename().string();
            pom2::log().info("Drop", "booted dropped image: " + path);
        } else {
            tapeStatusMessage = "Drop failed: " + err;
            pom2::log().warn("Drop", "dropped image rejected: " + err);
        }
        tapeStatusUntil = lastFrameTime + 4.0;
        return;
    }
    // Nothing usable in the drop — tell the user rather than silently
    // ignoring it (the most common case is dropping a ROM or a .zip).
    tapeStatusMessage =
        "Dropped file not a disk image "
        "(.dsk/.do/.d13/.po/.nib/.woz/.hdv/.2mg)";
    tapeStatusUntil = lastFrameTime + 4.0;
}

void MainWindow::renderWelcomePanelWindow()
{
    if (!show(pom2::PanelId::Welcome)) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Welcome to POM2###welcomePanel", &show(pom2::PanelId::Welcome),
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
    if (ImGui::Button("Close")) show(pom2::PanelId::Welcome) = false;
    ImGui::SameLine();
    if (ImGui::Button("About POM2...")) { showAbout = true; }
    ImGui::End();
}

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ensureAboutImageLoaded();
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About POM2", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Photo on the left, all text flowed into a column on the right.
        if (aboutImageTex_ && aboutImageW_ > 0 && aboutImageH_ > 0) {
            // Scale to a sensible width in the dialog while preserving the
            // 800×792 aspect of the original photo (≈ 1:1).
            const float displayW = 220.0f;
            const float displayH = displayW *
                static_cast<float>(aboutImageH_) /
                static_cast<float>(aboutImageW_);
            ImGui::BeginGroup();
            ImGui::Image(static_cast<ImTextureID>(
                             static_cast<intptr_t>(aboutImageTex_)),
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
        if (ImGui::Button("Close")) showAbout = false;
    }
    ImGui::End();
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
                tapeStatusMessage = "Tape loaded: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Load failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
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
                tapeStatusMessage = "Tape saved: " + cassetteDeck->dialogPath;
            } else {
                tapeStatusMessage = "Save failed: " + controller->cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The transient tapeStatusMessage (disk load / boot / eject / screenshot
    // / paste …) is now surfaced in the bottom status bar (renderStatusBar),
    // right-aligned and auto-expiring via tapeStatusUntil — no separate
    // floating overlay.
}

void MainWindow::render()
{
    // Track wallclock between frames so the deck counter / armed pulse /
    // status overlay can age correctly.
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    const double now = std::chrono::duration<double>(clock::now() - t0).count();
    const float deltaSeconds = static_cast<float>(std::max(0.0, now - lastFrameTime));
    lastFrameTime = now;

    // Screen-widget hover is re-established by renderScreenWindow() further
    // down, if it draws at all. Clearing it here is what makes "the screen
    // window is collapsed / hidden / not reached this frame" mean "the
    // pointer is not the guest's" — a latched `true` would keep feeding the
    // Mouse Card from a widget that is no longer on screen.
    screenHovered_ = false;

    pollJoystickAndPushToMemory();

    // A pointer capture only makes sense while a Mouse Card is on the bus.
    // Every path that can take one away — Slot Configuration, a profile
    // switch, a snapshot restore — unplugs both mouse cards, so
    // releasing here covers all of them at one point instead of chasing
    // each call site. Kiosk deliberately keeps the grab (it is the mode
    // most likely to want it), which is why this sits above the kiosk
    // early-out below.
    if (mouseGrabbed_ && !mouseCoordinator_->capture().plugged()) setMouseGrab(false);

    // Decide CPU turbo from disk activity every frame, independent of whether
    // any disk panel window is open (the disk panel defaults to hidden).
    updateAutoTurbo();

    // Kiosk: only the screen, no chrome. Joystick + auto-turbo above still
    // run so the machine behaves identically; everything else is skipped.
    // F6 hold-to-rewind still works (no toolbar button in kiosk).
    if (kiosk_) {
        // F6 is inert while the menu has the machine parked: releaseHold →
        // rewindEndAndResume would setMode(Running) behind the overlay.
        driveRewindHold(!kioskMenuOpen_ && ImGui::IsKeyDown(ImGuiKey_F6));
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
        tb.memoryGridVisible  = show(pom2::PanelId::MemGrid);
        tb.activeProfile      = activeProfile;
        tb.hasPrimaryDiskCard = (primaryDiskII() != nullptr);
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
                tapeStatusMessage = "Boot: " + browserResetBootImage_;
                tapeStatusUntil = lastFrameTime + 3.0;
            } else {
                tapeStatusMessage = "Boot failed: " + err;
                tapeStatusUntil = lastFrameTime + 5.0;
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
        if (tr.requestMemoryGridToggle) show(pom2::PanelId::MemGrid) = !show(pom2::PanelId::MemGrid);
        // Same entry point as F10 and the View menu item, so the three
        // routes into kiosk cannot drift apart.
        if (tr.requestKioskToggle)      toggleKioskMode();
        if (tr.requestAbout)            showAbout = true;
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
            // picks it up next frame and routes to `primaryDiskII()`.
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

    // Every panel, in catalog order, each drawn only while it is visible.
    // This was 43 hand-ordered calls — some gated here, most gating
    // themselves, in an order that was the order somebody happened to add
    // them in. What is left around it is the code that is NOT a panel: the
    // modal file dialogs, the printer pump (a side effect that must run
    // whether or not its window is open), the About box, the status bar and
    // the palette overlay, which must stay last so it draws above everything.
    renderPanels(deltaSeconds);

    // The printer keeps consuming its card's spool with every window shut —
    // without this a //c printing with the panel closed parked every byte in
    // the spool forever, nothing on paper.
    pumpImageWriter();

    renderTapeFileDialogs();
    renderPasteFileDialog();
    renderHdvFileDialog();
    renderDiskFileDialog();
    renderDisk35FileDialog();
    renderAboutDialog();
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
    const auto mouseInventory = mouseCoordinator_->capture();
    if (mouseInventory.appleWinPlugged) {
        const bool mouseOn = mouseInventory.appleWin.mouseOn();
        const float w = screenRectMax.x - screenRectMin.x;
        const float h = screenRectMax.y - screenRectMin.y;
        const bool insideWidget =
            w > 0.0f && h > 0.0f &&
            lastMouseHostX >= double(screenRectMin.x) &&
            lastMouseHostX <= double(screenRectMax.x) &&
            lastMouseHostY >= double(screenRectMin.y) &&
            lastMouseHostY <= double(screenRectMax.y);
        if (mouseOn && insideWidget) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
    }
}
