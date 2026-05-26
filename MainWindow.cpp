// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "Apple2Display.h"
#include "CassetteDeck_ImGui.h"
#include "CassetteDevice.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
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
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "CffaCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "SmartPort_ImGui.h"
#include "SpeakerDevice.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"
#include "Toolbar_ImGui.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
// Sentinel prefix used in HdvController_ImGui::LibraryEntry::fullPath to
// flag the synthetic prodos_disk/ host-folder mount. The dispatcher in
// renderHdvPanelWindow detects this prefix and routes to the synthesiser
// instead of treating the path as a real .hdv file.
constexpr const char* kProDOSHostSentinel = "@PRODOS_HOST_FOLDER@:";
} // namespace

MainWindow::MainWindow(bool forceIIPlus)
    // Member init order matches declaration order in MainWindow.h: the
    // controller is constructed first so memViewer can safely call
    // controller->memory() in its initialiser. Settings + AiControlServer
    // are heap-allocated for the same reason as the rest — keep their
    // headers out of MainWindow.h.
    : controller     (std::make_unique<EmulationController>()),
      display        (std::make_unique<Apple2Display>()),
      memViewer      (std::make_unique<MemoryViewer_ImGui>(&controller->memory())),
      settings       (std::make_unique<pom2::Settings>()),
      cassetteDeck   (std::make_unique<pom2::CassetteDeck_ImGui>()),
      disk35Panel    (std::make_unique<pom2::Disk35Controller_ImGui>()),
      diskLibrary    (std::make_unique<pom2::DiskLibrary_ImGui>()),
      hdvPanel       (std::make_unique<pom2::HdvController_ImGui>()),
      smartPortPanel (std::make_unique<pom2::SmartPort_ImGui>()),
      floppyEmu      (std::make_unique<pom2::FloppyEmuDevice>()),
      floppyEmuPanel (std::make_unique<pom2::FloppyEmu_ImGui>()),
      joystickPanel  (std::make_unique<pom2::JoystickPanel_ImGui>()),
      chatMauvePanel (std::make_unique<pom2::LeChatMauve_ImGui>()),
      toolbar        (std::make_unique<pom2::Toolbar_ImGui>()),
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
    memViewer->setWriteCallback([this](uint16_t a, uint8_t v) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        controller->memory().memWrite(a, v);
    });

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

    if (iiePresent) {
        controller->memory().setIIEMode(true);
        const int banks = settings->getInt("ramworks_banks", 1);
        controller->memory().setRamWorksBanks(
            static_cast<uint32_t>(banks > 0 ? banks : 1));
        display->setAuxMemory(controller->memory().auxData());
    }

    if (controller->memory().loadAppleIIRom(romPath.c_str())) {
        romStatus = std::string(iiePresent ? "IIe (128K): " : "loaded: ") + romPath;
    } else {
        romStatus = std::string("NO ROM (") + romPath +
                    ") — only $D000-$FFFF stub is active";
    }
    controller->memory().loadCharRom(charRomPath.c_str());

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
        controller->floppySound525().setVolume(vol525);
        controller->floppySound525().setMuted (mute525);
        controller->floppySound35().setVolume(
            settings->getFloat("floppy_sound_volume_35", vol525));
        controller->floppySound35().setMuted(
            settings->getBool ("floppy_sound_muted_35",  mute525));
        // Audio master (mixer panel). Default 1.0 / unmuted to preserve
        // pre-mixer behaviour.
        controller->audio().setMasterVolume(
            settings->getFloat("master_volume", 1.0f));
        controller->audio().setMasterMuted(
            settings->getBool ("master_muted",  false));
    }

    // Plug all expansion cards in their user-configured slots. The
    // mapping is read from `slot_1_card`..`slot_7_card` settings; absent
    // keys fall back to the legacy defaults (DiskII=6, HDV=5, SSC=2,
    // Clock=4, ChatMauve=7) so first-run users see no regression.
    plugSlotsFromSettings();

    // ── Restore display + UI prefs from previous session ─────────────
    {
        const std::string mode = settings->getString("hi_res_mode", "");
        if      (mode == "ColorNTSC")       display->setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        else if (mode == "ColorCompMedium") display->setHiResMode(Apple2Display::HiResMode::ColorCompMedium);
        else if (mode == "ColorComp4Bit")   display->setHiResMode(Apple2Display::HiResMode::ColorComp4Bit);
        else if (mode == "ChatMauveRGB")    display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        else if (mode == "MonoWhite")       display->setHiResMode(Apple2Display::HiResMode::MonoWhite);
        else if (mode == "MonoGreen")       display->setHiResMode(Apple2Display::HiResMode::MonoGreen);
        else if (mode == "MonoAmber")       display->setHiResMode(Apple2Display::HiResMode::MonoAmber);

        pixelScale         = settings->getFloat("pixel_scale",     pixelScale);
        showDiskPanel      = settings->getBool ("show_disk_panel", showDiskPanel);
        showDisk35Panel    = settings->getBool ("show_disk35_panel", showDisk35Panel);
        showDiskLibrary    = settings->getBool ("show_disk_library", showDiskLibrary);
        showHdvPanel       = settings->getBool ("show_hdv_panel",  showHdvPanel);
        showSmartPortPanel = settings->getBool ("show_smartport_panel", showSmartPortPanel);
        showSlotConfigPanel = settings->getBool ("show_slot_config", showSlotConfigPanel);
        showFloppyEmu      = settings->getBool ("show_floppy_emu", showFloppyEmu);
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
                // disks/ ・ disks35/ ・ hdv/. Probe the usual cwd anchors and,
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
        showCassetteDeck   = settings->getBool ("show_cassette",   showCassetteDeck);
        showToolbar        = settings->getBool ("show_toolbar",    showToolbar);
        showJoystickPanel  = settings->getBool ("show_joystick",   showJoystickPanel);
        showChatMauvePanel = settings->getBool ("show_chatmauve",  showChatMauvePanel);
        showMockingboardPanel = settings->getBool ("show_mockingboard",
                                                  showMockingboardPanel);
        showAudioMixer     = settings->getBool ("show_mixer",      showAudioMixer);
        showSscPanel       = settings->getBool ("show_ssc",        showSscPanel);
        sscPortInput       = settings->getInt  ("ssc_port",        sscPortInput);
        diskTurboWhileMotor = settings->getBool("disk_turbo",      diskTurboWhileMotor);
    }

    // ── Restore Disk II state per-slot ────────────────────────────────
    // Each plugged DiskII gets its own `disk_path_slotN` /
    // `disk_writeback_slotN` keys. The primary (lowest-slot) card also
    // honours the legacy unsuffixed `disk_path` / `disk_writeback` keys
    // so settings written before multi-instance support keep loading.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        const bool isPrimary = (c == diskCard);
        const bool wb = settings->getBool(
            "disk_writeback" + slotKey,
            isPrimary ? settings->getBool("disk_writeback", false) : false);
        c->setWriteBackEnabled(wb);
        const std::string diskPath = settings->getString(
            "disk_path" + slotKey,
            isPrimary ? settings->getString("disk_path", "") : std::string());
        std::error_code ec;
        if (!diskPath.empty() && fs::is_regular_file(diskPath, ec)) {
            if (c->insertDisk(diskPath)) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(c->getSlot()) +
                    " re-inserted from settings: " + diskPath);
            }
        }
    }

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
    aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
    aiServer->setAuthToken(aiTokenInput);
    aiServer->setProfileLabel(std::string(pom2::profileConfig(activeProfile).displayName));
    showAiControlPanel = settings->getBool("show_ai_control", showAiControlPanel);
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
    const std::string savedProfile =
        forceIIPlus ? std::string() : settings->getString("system_profile", "");
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
            if (resolved != controller->cpu().getCpuMode()) {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(resolved);
            }
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

MainWindow::~MainWindow()
{
    // Stop the AI control server BEFORE the CPU worker — pending requests
    // hold `controller->stateMutex()` and call into `controller->memory()` /
    // `controller->cpu()`; we want them quiesced before we tear anything
    // else down. The server's destructor would do the same on member
    // destruction order, but doing it here keeps the dependency obvious.
    aiServer->stop();
    controller->stop();

    // Persist the current state so the next launch restores the same
    // mounted disks, video mode, panels, and audio levels.
    // Skip persisting an HDV card that ensureHdvCardForBoot auto-plugged for
    // a one-shot `POM2 <image.hdv>` boot — it's session-local by contract.
    const bool hdvIsAutoProvisioned =
        hdvCard && hdvCard->getSlot() == autoProvisionedHdvSlot_;
    if (!hdvIsAutoProvisioned && hdvCard && hdvCard->isImageLoaded()) {
        // Don't persist the synthesised host-folder volume — the path is
        // a sentinel, not a real file. Re-synthesis happens on click.
        const std::string& p = hdvCard->getImagePath();
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
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        const std::string p = c->isDiskLoaded() ? c->getDiskPath() : std::string();
        settings->setString("disk_path"      + slotKey, p);
        settings->setBool  ("disk_writeback" + slotKey, c->isWriteBackEnabled());
        if (c == diskCard) {
            settings->setString("disk_path",      p);
            settings->setBool  ("disk_writeback", c->isWriteBackEnabled());
        }
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

    if (hdvCard) {
        settings->setBool("hdv_writeback", hdvCard->isWriteBackEnabled());
    }

    // CFFA per-slot image + write-back for EVERY plugged CFFA card. `cffa`
    // is multi-instance, so persist each (not just the primary `cffaCard`),
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

    if (sscCard) {
        settings->setBool("ssc_listening", sscCard->isListening());
        settings->setInt ("ssc_port",      sscCard->getPort());
        settings->setBool("ssc_raw_mode",  sscCard->rawMode());
    }

    // AI control listener — persist enable, port, token, and the panel
    // visibility flag. Re-armed on next launch by the constructor.
    settings->setBool  ("ai_control_enable", aiServer->isRunning());
    settings->setInt   ("ai_control_port",   aiServer->getPort());
    settings->setString("ai_control_token",  aiTokenInput);
    settings->setBool  ("show_ai_control",   showAiControlPanel);

    // Persist the per-slot card mapping so changes via the Slot
    // Configuration panel survive a restart.
    for (int s = 1; s <= 7; ++s) {
        if (s == autoProvisionedHdvSlot_) continue;   // session-local auto-plug; leave saved config intact
        settings->setString("slot_" + std::to_string(s) + "_card", slotCards[s]);
    }

    auto modeName = [](Apple2Display::HiResMode m) -> const char* {
        switch (m) {
            case Apple2Display::HiResMode::ColorNTSC:        return "ColorNTSC";
            case Apple2Display::HiResMode::ColorCompMedium:  return "ColorCompMedium";
            case Apple2Display::HiResMode::ColorComp4Bit:    return "ColorComp4Bit";
            case Apple2Display::HiResMode::ChatMauveRGB:     return "ChatMauveRGB";
            case Apple2Display::HiResMode::MonoWhite:        return "MonoWhite";
            case Apple2Display::HiResMode::MonoGreen:        return "MonoGreen";
            case Apple2Display::HiResMode::MonoAmber:        return "MonoAmber";
        }
        return "ColorNTSC";
    };
    settings->setString("hi_res_mode", modeName(display->getHiResMode()));
    settings->setFloat ("pixel_scale", pixelScale);
    settings->setBool  ("show_disk_panel", showDiskPanel);
    settings->setBool  ("show_disk35_panel", showDisk35Panel);
    settings->setBool  ("show_disk_library", showDiskLibrary);
    settings->setBool  ("show_hdv_panel",  showHdvPanel);
    settings->setBool  ("show_smartport_panel", showSmartPortPanel);
    settings->setBool  ("show_slot_config", showSlotConfigPanel);
    settings->setBool  ("show_floppy_emu", showFloppyEmu);
    settings->setString("floppyemu_mode",
                        pom2::FloppyEmuDevice::modeKey(floppyEmu->mode()));
    settings->setString("floppyemu_sd_root", floppyEmu->sdRoot());
    settings->setBool  ("show_cassette",   showCassetteDeck);
    settings->setBool  ("show_toolbar",    showToolbar);
    settings->setBool  ("show_joystick",   showJoystickPanel);
    settings->setBool  ("show_chatmauve",  showChatMauvePanel);
    settings->setBool  ("show_mockingboard", showMockingboardPanel);
    settings->setBool  ("show_mixer",      showAudioMixer);
    settings->setBool  ("show_ssc",        showSscPanel);
    settings->setBool  ("disk_turbo",      diskTurboWhileMotor);
    settings->setFloat ("speaker_volume",  controller->speaker().getVolume());
    settings->setBool  ("speaker_muted",   controller->speaker().isMuted());
    settings->setFloat ("cassette_volume", controller->cassette().getVolume());
    settings->setBool  ("cassette_auto_rewind",
                        controller->cassette().isAutoRewindEnabled());
    settings->setFloat ("floppy_sound_volume",    controller->floppySound525().getVolume());
    settings->setBool  ("floppy_sound_muted",     controller->floppySound525().isMuted());
    settings->setFloat ("floppy_sound_volume_35", controller->floppySound35().getVolume());
    settings->setBool  ("floppy_sound_muted_35",  controller->floppySound35().isMuted());
    settings->setFloat ("master_volume",          controller->audio().getMasterVolume());
    settings->setBool  ("master_muted",           controller->audio().isMasterMuted());
    settings->setString("char_rom_locale",        pom2::charRomLocaleKey(charRomLocale));
    if (mockingboardCard) {
        settings->setFloat("mockingboard_volume", mockingboardCard->getVolume());
        settings->setBool ("mockingboard_muted",  mockingboardCard->isMuted());
    }

    // Kiosk is a read-only launcher: don't write state.cfg, so the disk it
    // booted (and any HDV card auto-plugged for it by ensureHdvCardForBoot)
    // never leak into the user's saved GUI config. The setString calls
    // above are in-memory only and discarded with `settings` here.
    if (!kiosk_) settings->save();
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
void MainWindow::plugSlotsFromSettings()
{
    namespace fs = std::filesystem;

    // Default mapping when no `slot_N_card` keys are present. Matches the
    // historical hard-wired layout from before the slot-config refactor.
    static const char* kDefaults[8] = {
        "",          // slot 0 (Language Card — owned by Memory, not us)
        "",          // slot 1
        "ssc",       // slot 2
        "",          // slot 3
        "clock",     // slot 4
        "hdv",       // slot 5
        "diskii",    // slot 6
        "chatmauve"  // slot 7
    };

    // Resolve each slot from settings, with the legacy `clock_card_enable`
    // flag as a one-shot fallback: if the user has clock_card_enable=false
    // AND no slot_4_card key, slot 4 stays empty. Once any slot_N_card key
    // is set in the file, that key is the source of truth.
    for (int s = 1; s <= 7; ++s) {
        const std::string key = "slot_" + std::to_string(s) + "_card";
        slotCards[s] = settings->getString(key, kDefaults[s]);
    }
    if (!settings->getBool("clock_card_enable", true)) {
        // Legacy opt-out: only respect when no explicit slot_N_card key
        // was set (settings->getString returned the default for slot 4).
        if (settings->getString("slot_4_card", "__missing__") == "__missing__"
            && slotCards[4] == "clock") {
            slotCards[4] = "";
        }
    }

    // Profile-defined built-in slots override anything the user persisted.
    // Real //c / //c+ have no physical expansion bus; their on-board cards
    // (serial, mouse, Disk II, SmartPort) live at fixed virtual slot IDs
    // and CANNOT be unplugged (D-6-1, E-6-1). Keep the settings file's
    // value out of harm's way so toggling profiles back and forth doesn't
    // produce a wedged machine.
    {
        const auto& cfg = pom2::profileConfig(activeProfile);
        for (int s = 1; s <= 7; ++s) {
            if (cfg.builtInSlots[s].has_value()) {
                const std::string& forced = cfg.builtInSlots[s]->cardKey;
                if (slotCards[s] != forced) {
                    pom2::log().info("Slots",
                        "Slot " + std::to_string(s) + " forced to '" +
                        forced + "' (built-in on " +
                        std::string(cfg.displayName) +
                        "); user setting '" + slotCards[s] + "' ignored");
                    slotCards[s] = forced;
                }
            }
        }
    }

    // Validate uniqueness — each card type plugs into at most one slot.
    // Exceptions (multi-instance): "diskii" (option C 2026-05-15: //e configs
    // carried DiskII slot 6 + slot 4 for 4 drives), and — since the Slot
    // Manager can drive several of each — "cffa" and "smartport35", whose
    // persistence is already per-slot (cffa_slotN_*, smartport_slotN_*). The
    // rest stay single-instance because their driver / ROM signature / global
    // settings keys assume exclusivity ("hdv" uses the global hdv_path key).
    auto multiInstance = [](const std::string& k) -> bool {
        return k == "diskii" || k == "cffa" || k == "smartport35";
    };
    auto firstOccurrence = [&](const std::string& type) -> int {
        for (int s = 1; s <= 7; ++s) if (slotCards[s] == type) return s;
        return -1;
    };
    for (int s = 1; s <= 7; ++s) {
        if (slotCards[s].empty())  continue;
        if (multiInstance(slotCards[s])) continue;    // multi-instance OK
        const int first = firstOccurrence(slotCards[s]);
        if (first != s) {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " requested '" + slotCards[s] +
                "' but that card is already in slot " + std::to_string(first) +
                " — leaving slot " + std::to_string(s) + " empty");
            slotCards[s] = "";
        }
    }

    // ── Per-card construction helpers. Each one populates the matching
    //    raw `*Card` member pointer (non-owning) for the rest of MainWindow
    //    to find, and plugs the card into the SlotBus. ────────────────

    auto plugDiskII = [&](int s) {
        static const char* diskRomCandidates[] = {
            "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom"
        };
        for (const char* p : diskRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { diskRomPath = r; break; }
        }
        auto card = std::make_unique<DiskIICard>(s);
        // DiskIICard ctor pre-loads the embedded Apple 341-0027-A boot PROM,
        // so the card is bootable out of the box. A user-supplied dump at
        // `roms/disk2.rom` overrides only if it loads cleanly (256 bytes).
        if (fs::exists(diskRomPath) && card->loadBootRom(diskRomPath)) {
            diskRomStatus = std::string("loaded: ") + diskRomPath;
        } else {
            diskRomStatus = "embedded 341-0027-A PROM (no user disk2.rom)";
        }
        // Optional: load the P6 LSS sequencer PROM (Apple part 341-0028-A)
        // for the cycle-accurate bit-level controller path. Bundled at
        // `roms/diskii_p6.rom`; falls back to the embedded default.
        static const char* lssRomCandidates[] = {
            "roms/diskii_p6.rom", "../roms/diskii_p6.rom",
            "../../roms/diskii_p6.rom"
        };
        for (const char* p : lssRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadLssRom(r); break; }
        }
        // Optional 13-sector PROMs (Apple 341-0009 boot + 341-0010 LSS) for
        // pre-DOS-3.3 disks. The card serves them only while a 13-sector
        // disk is mounted (see DiskIICard serving13_). Bundled at
        // roms/disk2_13.rom + roms/diskii_p6_13.rom.
        static const char* boot13Candidates[] = {
            "roms/disk2_13.rom", "../roms/disk2_13.rom", "../../roms/disk2_13.rom"
        };
        for (const char* p : boot13Candidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadBootRom13(r); break; }
        }
        static const char* lss13Candidates[] = {
            "roms/diskii_p6_13.rom", "../roms/diskii_p6_13.rom",
            "../../roms/diskii_p6_13.rom"
        };
        for (const char* p : lss13Candidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { (void)card->loadLssRom13(r); break; }
        }
        // Wire the CPU pointer for sub-instruction cycle accuracy on
        // MMIO reads/writes (cycle-precise copy protections rely on the
        // LSS state at the exact sub-cycle of the data fetch, not at
        // instruction-start). See DiskIICard::setCpu doc for context.
        card->setCpu(&controller->cpu());
        card->setFloppySound(&controller->floppySound525());
        // //c+ on-board IWM — only the slot-6 card pushes its drive
        // pointer to the IWM, mirroring the //c+ wiring. Multi-instance
        // Disk II (option C) lets the user plug a second Disk II in
        // slot 4 etc.; that secondary card stays off the IWM path to
        // avoid clobbering the //c+ flux mirror.
        if (s == 6) card->setIWM(&controller->iwm());
        DiskIICard* raw = card.get();
        // First plugged DiskII is the "primary" — its panel & state
        // power the legacy menu paths (auto-turbo, AI control, top-
        // bar Eject). plugSlotsFromSettings iterates slots ascending,
        // so the lowest-numbered slot wins naturally.
        diskCards.push_back(raw);
        if (!diskCard) diskCard = raw;
        diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
        if (!diskPanel) diskPanel = diskPanels.front().get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugHdv = [&](int s) {
        // Restore ONLY the image explicitly mounted in the previous
        // session — mirrors the Disk II / 3.5" restore above (no folder
        // scan). This used to fall back to auto-mounting the first
        // .hdv/.2mg found under hdv/ when no path was saved, which
        // silently re-mounted (and auto-booted) a hard disk the user had
        // just ejected. An empty `hdv_path` now means "nothing mounted",
        // consistent with every other storage type.
        const std::string saved = settings->getString("hdv_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) {
            hdvPath = saved;
        }

        auto card = std::make_unique<ProDOSHardDiskCard>(s);
        if (!hdvPath.empty() && card->loadImage(hdvPath)) {
            hdvStatus = std::string("loaded: ") + hdvPath;
        } else {
            hdvStatus = "no image mounted";
        }
        card->setWriteBackEnabled(settings->getBool("hdv_writeback", false));
        if (!hdvCard) hdvCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugCffa = [&](int s) {
        // MAME-faithful CFFA 2.0: the real 4 KB firmware dump executed over an
        // emulated ATA chip (CffaCard → AtaBlockDevice → Block512Backing).
        // Pick the firmware variant matching the CPU (65C02 → eec02, else
        // ee02), falling back to whichever dump is present. No ROM → the slot
        // is left empty (the card type is also hidden in Slot Config then).
        // findResource adds the executable-relative / FHS roots on top of
        // the CWD + build/-relative ones, so the CFFA dumps resolve in a
        // portable bundle / AppImage / install too. See ResourcePaths.h.
        auto probe = [&](const char* a, const char* b) -> std::string {
            for (const char* nm : { a, b }) {
                std::string p = pom2::findResource(std::string("roms/") + nm);
                if (!p.empty()) return p;
            }
            return std::string();
        };
        const bool cmos =
            controller->cpu().getCpuMode() == M6502::CpuMode::CMOS;
        const std::string cffaRomPath = cmos
            ? probe("cffa20eec02.bin", "cffa20ee02.bin")
            : probe("cffa20ee02.bin", "cffa20eec02.bin");
        if (cffaRomPath.empty()) {
            pom2::log().warn("CFFA", "Slot " + std::to_string(s) +
                " requested CFFA but no firmware ROM (roms/cffa20ee02.bin) — "
                "leaving slot empty");
            slotCards[s] = "";
            return;
        }
        auto card = std::make_unique<pom2::CffaCard>(s);
        if (!card->loadRom(cffaRomPath)) { slotCards[s] = ""; return; }

        // Restore ONLY the explicitly-saved image (no folder scan — same
        // policy as plugHdv). Per-slot keys so a CFFA can live in any slot.
        const std::string key = "cffa_slot" + std::to_string(s);
        const std::string saved = settings->getString(key + "_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) {
            if (!card->loadImage(saved))
                pom2::log().warn("CFFA", "Re-mount failed: " + card->getLastError());
        }
        card->setWriteBackEnabled(settings->getBool(key + "_writeback", false));
        if (!cffaCard) cffaCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugChatMauve = [&](int s) {
        auto card = std::make_unique<LeChatMauveCard>(s);
        chatMauveCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
        display->setChatMauveCard(chatMauveCard);
    };

    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        sscCard = card.get();
        // Use pasteText (not queueKey) — pasteText respects the paste
        // queue, so a stream of bytes from telnet doesn't clobber earlier
        // characters that BASIC hasn't picked up yet.
        sscCard->setKeyboardSink(
            [&mem = controller->memory()](uint8_t b) {
                const char buf[1] = { static_cast<char>(b) };
                mem.pasteText(buf, 1);
            });
        // IRQ routing is auto-wired by SlotBus's installed router (see
        // Memory::setCpu) — no per-card setup needed.
        controller->memory().slotBus().plug(s, std::move(card));
        sscCard->setRawMode(settings->getBool("ssc_raw_mode", false));
        if (settings->getBool("ssc_listening", false)) {
            const int p = settings->getInt("ssc_port",
                                          SuperSerialCard::kDefaultPort);
            sscCard->startListening(static_cast<uint16_t>(p));
        }
    };

    auto plugClock = [&](int s) {
        auto card = std::make_unique<ClockCard>(s);
        clockCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugMockingboard = [&](int s) {
        // Mockingboard A/C — 6522×2 + AY-3-8910×2. No ROM dependency, no
        // image to mount: software detects it by writing to the VIA at
        // $C(s)00 and observing the read-back. We always-plug when
        // requested. The inner AudioSource is registered with the audio
        // device so synthesised samples mix with the speaker output, and
        // the CPU IRQ line is wired so VIA T1 can drive the music
        // driver's tick.
        auto card = std::make_unique<MockingboardCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        // CPU pointer feeds the lazy-sync timer back-channel
        // (getCycleCountNow); IRQ routing is auto-wired via SlotBus.
        card->setCpu(&controller->cpu());
        // Default volume is conservative — the card's three-channel mix
        // can dwarf the speaker at peak; the user can crank via the
        // Mockingboard panel (TODO).
        card->setVolume(settings->getFloat("mockingboard_volume", 0.5f));
        card->setMuted(settings->getBool ("mockingboard_muted",  false));
        if (controller->audio().isAvailable()) {
            controller->audio().addSource(card->audioSource());
        }
        mockingboardCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugSmartPort35 = [&](int s) {
        // Liron-class card. Each unit's type + image is restored from
        // settings (smartport_slotN_unitK_*) so per-card mixes (e.g.
        // unit 0 = 3.5", unit 1 = HDV) survive across launches. When
        // no setting exists, both units start empty — the user picks
        // a type via the SmartPort Configuration panel.
        auto card = std::make_unique<pom2::SmartPortCard>(s);
        // Mechanical sound: route to the dedicated 3.5" sound bank.
        // Block-level transfers only — the card synthesises step / motor
        // / click events from READBLOCK / WRITEBLOCK directly.
        card->setFloppySound(&controller->floppySound35());

        // Restore per-unit configuration from settings. Each unit row
        // remembers its kind ("35" / "hdv" / ""=empty), its mounted image
        // path, and the write-back toggle. Persistence keyspace:
        //   smartport_slotN_unitK_type     ("" / "35" / "hdv")
        //   smartport_slotN_unitK_path     (image path, optional)
        //   smartport_slotN_unitK_writeback (bool)
        const std::string slotKey = "smartport_slot" + std::to_string(s);
        for (size_t k = 0; k < pom2::SmartPortCard::kMaxUnits; ++k) {
            const std::string base = slotKey + "_unit" + std::to_string(k);
            const std::string kind = settings->getString(base + "_type", "");
            if (kind.empty()) continue;
            auto unit = pom2::makeSmartPortUnit(kind);
            if (!unit) {
                pom2::log().warn("SmartPort",
                    "Unknown unit kind '" + kind + "' for " + base +
                    " — leaving empty");
                continue;
            }
            unit->setWriteBackEnabled(
                settings->getBool(base + "_writeback", false));
            // Resolve the persisted path against multiple cwd anchors —
            // settings.ini may have been written when POM2 ran from
            // repo root (`hdv/X`) but a later launch from build/ would
            // fail `is_regular_file` on the bare relative path. Same
            // fallback the ROM probe uses.
            const std::string p = settings->getString(base + "_path", "");
            std::string resolved;
            if (!p.empty()) {
                std::error_code ec;
                for (const std::string& cand :
                     { p, std::string("../") + p, std::string("../../") + p }) {
                    if (std::filesystem::is_regular_file(cand, ec)) {
                        resolved = cand;
                        break;
                    }
                }
            }
            if (!resolved.empty()) {
                if (!unit->loadImage(resolved)) {
                    pom2::log().warn("SmartPort",
                        "Failed to remount " + resolved + " on " + base +
                        ": " + unit->lastError());
                }
            } else if (!p.empty()) {
                pom2::log().warn("SmartPort",
                    "Persisted path not found: " + p + " (on " + base + ")");
            }
            card->setUnit(k, std::move(unit));
        }
        if (!smartPortCard) smartPortCard = card.get();   // primary = lowest slot
        controller->memory().slotBus().plug(s, std::move(card));
    };

    auto plugMouse = [&](int s) {
        // Mouse Card (MAME-faithful 68705P3 + 6821 PIA + Apple ROMs).
        // Both Apple ROMs are required — without them the card has no
        // firmware and refuses to plug.
        std::string slotRomPath, mcuRomPath;
        static const char* slotRomCandidates[] = {
            "roms/mouse_341-0270-c.bin", "../roms/mouse_341-0270-c.bin",
            "../../roms/mouse_341-0270-c.bin"
        };
        static const char* mcuRomCandidates[] = {
            "roms/mouse_341-0269.bin", "../roms/mouse_341-0269.bin",
            "../../roms/mouse_341-0269.bin"
        };
        for (const char* p : slotRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { slotRomPath = r; break; }
        }
        for (const char* p : mcuRomCandidates) {
            std::string r = pom2::findResource(p);
            if (!r.empty()) { mcuRomPath = r; break; }
        }
        if (slotRomPath.empty() || mcuRomPath.empty()) {
            mouseRomStatus = "ROMs missing (need roms/mouse_341-0270-c.bin "
                             "and roms/mouse_341-0269.bin)";
            pom2::log().warn("Mouse",
                "Mouse Card requested in slot " + std::to_string(s) +
                " but " + mouseRomStatus + " — leaving slot empty");
            return;
        }
        auto card = std::make_unique<MouseCard>(s);
        if (!card->loadRoms(slotRomPath, mcuRomPath)) {
            mouseRomStatus = "ROM load failed (size mismatch?)";
            return;
        }
        // IRQ routing is auto-wired by SlotBus (see Memory::setCpu).
        // Mouse Card's MCU PB6 reaches the CPU via SlotPeripheral::
        // assertIrq, which fans out through M6502::setIrqLine(slot, …).
        mouseRomStatus = "loaded: " + slotRomPath + " + " + mcuRomPath;
        mouseCard = card.get();
        controller->memory().slotBus().plug(s, std::move(card));
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
        else if (kind == "clock")       plugClock(s);
        else if (kind == "chatmauve")   plugChatMauve(s);
        else if (kind == "mouse")       plugMouse(s);
        else if (kind == "mouseaw")     {
            // AppleWin HLE variant — only the slot EPROM is needed.
            std::string slotRomPath;
            static const char* slotRomCandidates[] = {
                "roms/mouse_341-0270-c.bin", "../roms/mouse_341-0270-c.bin",
                "../../roms/mouse_341-0270-c.bin"
            };
            for (const char* p : slotRomCandidates) {
                std::string r = pom2::findResource(p);
                if (!r.empty()) { slotRomPath = r; break; }
            }
            if (slotRomPath.empty()) {
                pom2::log().warn("MouseAW",
                    "Mouse (AppleWin HLE) requested in slot " +
                    std::to_string(s) +
                    " but roms/mouse_341-0270-c.bin not found — leaving slot empty");
                continue;
            }
            auto card = std::make_unique<MouseCardAppleWin>(s);
            if (!card->loadRom(slotRomPath)) {
                pom2::log().warn("MouseAW",
                    "ROM load failed for slot " + std::to_string(s));
                continue;
            }
            mouseAwCard = card.get();
            controller->memory().slotBus().plug(s, std::move(card));
        }
        else if (kind == "mockingboard") plugMockingboard(s);
        else if (kind == "smartport35") plugSmartPort35(s);
        else {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " has unknown card type '" +
                kind + "' — leaving empty");
            slotCards[s] = "";
        }
    }
}

// ─── Screenshot ───────────────────────────────────────────────────────────

void MainWindow::saveScreenshot()
{
    // Snapshot the framebuffer under stateMutex — the worker thread is
    // happily running but the renderer will never resize the buffer
    // mid-copy, so a brief lock is enough.
    int w = 0, h = 0;
    std::vector<uint32_t> pixels;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
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
    controller->memory().queueKey(apple2Code);
}

void MainWindow::onChar(unsigned int codepoint)
{
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
    std::string text((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    if (pasteAutoUppercase) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
    }
    const size_t queued = controller->memory().pasteText(text);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Paste: %zu chars from %s", queued, path.c_str());
    tapeStatusMessage = buf;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

// ─── Texture upload ──────────────────────────────────────────────────────

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

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

    {
        // Render under stateMutex so we get a consistent snapshot of RAM
        // (otherwise the CPU may be mid-frame with the text screen half
        // updated, producing tearing).
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        display->render(controller->memory());
    }

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

// ─── Render passes ───────────────────────────────────────────────────────

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        // Disk II (slot 6) — frequent action, lifted out of the old
        // Hardware kitchen-sink. Panel still exposes its own insert/eject
        // buttons; this is the keyboard-friendly path.
        ImGui::BeginDisabled(diskCard == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            diskCard && diskCard->isDiskLoaded())) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            diskCard->ejectDisk();
            tapeStatusMessage = "Disk ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(hdvCard == nullptr);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            hdvPanel->mountDialogOpen = true;
            if (hdvPanel->dialogPath.empty()) hdvPanel->dialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            hdvCard && hdvCard->isImageLoaded())) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            hdvCard->ejectImage();
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!hdvCard || !hdvCard->isImageLoaded());
        // Label reflects where the user actually has the card plugged.
        const std::string bootHdvLabel = "Boot HDV (slot " +
            std::to_string(hdvCard ? hdvCard->getSlot() : 5) + ")";
        if (ImGui::MenuItem(bootHdvLabel.c_str())) {
            bootHdvImage();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Reload ROM")) {
            bool ok = false;
            std::string err;
            {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = controller->memory().loadAppleIIRom(romPath.c_str());
                if (ok) {
                    controller->hardReset();
                } else {
                    err = controller->memory().getLastError();
                }
            }
            if (ok) romStatus = std::string("loaded: ") + romPath;
            else    romStatus = err;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            if (window) glfwSetWindowShouldClose(window, 1);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Paste from clipboard", "Ctrl+V"))
            pasteFromClipboard();
        if (ImGui::MenuItem("Paste from file..."))
            showPasteFileDialog = true;
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
        const auto curCpu = controller->cpu().getCpuMode();
        const std::string curOverride = settings->getString("cpu_mode_override", "auto");
        if (ImGui::BeginMenu("CPU")) {
            const auto& cfg = pom2::profileConfig(activeProfile);
            const char* profileLabel =
                (cfg.defaultCpu == M6502::CpuMode::CMOS) ? "65C02" : "NMOS 6502";
            char autoLabel[64];
            std::snprintf(autoLabel, sizeof(autoLabel),
                "Auto (profile default: %s)", profileLabel);
            if (ImGui::MenuItem(autoLabel, nullptr, curOverride == "auto")) {
                settings->setString("cpu_mode_override", "auto");
                settings->save();
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(cfg.defaultCpu);
            }
            if (ImGui::MenuItem("NMOS 6502", nullptr,
                                curOverride == "nmos" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::NMOS
                                 && curOverride != "65c02"))) {
                settings->setString("cpu_mode_override", "nmos");
                settings->save();
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(M6502::CpuMode::NMOS);
            }
            if (ImGui::MenuItem("65C02 (CMOS)", nullptr,
                                curOverride == "65c02" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::CMOS
                                 && curOverride != "nmos"))) {
                settings->setString("cpu_mode_override", "65c02");
                settings->save();
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                controller->cpu().setCpuMode(M6502::CpuMode::CMOS);
            }
            ImGui::Separator();
            ImGui::TextDisabled("NMOS = original 1975. Disables");
            ImGui::TextDisabled("STZ/BRA/PHX/etc. and SMB/RMB/");
            ImGui::TextDisabled("BBR/BBS extensions.");
            ImGui::TextDisabled("Override persists across profile");
            ImGui::TextDisabled("switches.");
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Slot Configuration...", nullptr, &showSlotConfigPanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Devices")) {
        ImGui::MenuItem("Disk Library (all formats)",    nullptr, &showDiskLibrary);
        ImGui::MenuItem("Floppy Emu (BMOW)",             nullptr, &showFloppyEmu);
        ImGui::Separator();
        ImGui::MenuItem("Cassette deck",                 nullptr, &showCassetteDeck);
        ImGui::MenuItem("Disk II (slot 6)",              nullptr, &showDiskPanel);
        {
            // Mirror the panel's dynamic title — slot N on //e with a
            // Liron-class card, "//c+ on-board" on //c+.
            std::string lbl;
            if (smartPortCard) {
                lbl = "Disk 3.5\" (slot " +
                    std::to_string(smartPortCard->getSlot()) + ")";
            } else {
                lbl = "Disk 3.5\" (//c+ on-board)";
            }
            ImGui::MenuItem(lbl.c_str(),                 nullptr, &showDisk35Panel);
        }
        {
            const std::string label = "HDV (slot " +
                std::to_string(hdvCard ? hdvCard->getSlot() : 5) + ")";
            ImGui::MenuItem(label.c_str(),               nullptr, &showHdvPanel);
        }
        if (smartPortCard) {
            const std::string label = "SmartPort Configuration (slot " +
                std::to_string(smartPortCard->getSlot()) + ")";
            ImGui::MenuItem(label.c_str(),               nullptr, &showSmartPortPanel);
        } else {
            ImGui::BeginDisabled();
            ImGui::MenuItem("SmartPort Configuration (no card plugged)",
                            nullptr, &showSmartPortPanel);
            ImGui::EndDisabled();
        }
        ImGui::MenuItem("Mockingboard (VIA + AY state)", nullptr, &showMockingboardPanel);
        ImGui::MenuItem("Super Serial (slot 2)",         nullptr, &showSscPanel);
        ImGui::MenuItem("Le Chat Mauve (slot 7)",        nullptr, &showChatMauvePanel);
        ImGui::MenuItem("Joystick",                      nullptr, &showJoystickPanel);
        ImGui::Separator();
        ImGui::MenuItem("Audio Mixer",                   nullptr, &showAudioMixer);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Display")) {
            ImGui::SliderFloat("Pixel scale", &pixelScale, 1.0f, 4.0f, "%.1fx");

            ImGui::Separator();
            ImGui::TextDisabled("Hi-res mode");
            const Apple2Display::HiResMode cur = display->getHiResMode();
            if (ImGui::MenuItem("Color NTSC", nullptr,
                                cur == Apple2Display::HiResMode::ColorNTSC))
                display->setHiResMode(Apple2Display::HiResMode::ColorNTSC);
            // Le Chat Mauve RGB — clean Péritel decode, two distinct grays,
            // no inter-byte fringing. Greyed out if the slot-7 card isn't
            // plugged (the Apple II would just see composite video).
            ImGui::BeginDisabled(chatMauveCard == nullptr);
            if (ImGui::MenuItem("Le Chat Mauve (RGB)", nullptr,
                                cur == Apple2Display::HiResMode::ChatMauveRGB))
                display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Mono White",  nullptr,
                                cur == Apple2Display::HiResMode::MonoWhite))
                display->setHiResMode(Apple2Display::HiResMode::MonoWhite);
            if (ImGui::MenuItem("Mono Green (P31)", nullptr,
                                cur == Apple2Display::HiResMode::MonoGreen))
                display->setHiResMode(Apple2Display::HiResMode::MonoGreen);
            if (ImGui::MenuItem("Mono Amber",  nullptr,
                                cur == Apple2Display::HiResMode::MonoAmber))
                display->setHiResMode(Apple2Display::HiResMode::MonoAmber);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem("Toolbar",                     nullptr, &showToolbar);
        ImGui::MenuItem("Emulation panel",             nullptr, &showEmulationPanel);
        ImGui::MenuItem("Memory viewer",               nullptr, &showMemViewer);
        ImGui::Separator();
        ImGui::MenuItem("Memory Map Bar",              nullptr, &showMemoryBar);
        ImGui::MenuItem("Memory Map Bar (Horizontal)", nullptr, &showMemoryBarH);
        ImGui::MenuItem("Memory Map Grid",             nullptr, &showMemoryGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Tools")) {
        ImGui::MenuItem("AI Control (HTTP)...", nullptr, &showAiControlPanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About POM2")) showAbout = true;
        ImGui::EndMenu();
    }

    // Status pill on the right.
    {
        const char* modeStr = "?";
        switch (controller->getMode()) {
            case EmulationController::Mode::Running: modeStr = "RUN";  break;
            case EmulationController::Mode::Stopped: modeStr = "STOP"; break;
            case EmulationController::Mode::Step:    modeStr = "STEP"; break;
        }
        const auto state = controller->memory().getDisplayState();
        const char* gfx = state.textMode ? "TEXT"
                        : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                         : (state.mixedMode ? "LGR+TXT" : "LGR");
        const auto& cfg = pom2::profileConfig(activeProfile);
        char buf[192];
        std::snprintf(buf, sizeof(buf), "  %.*s | %s | %s | %s",
                      static_cast<int>(cfg.displayName.size()), cfg.displayName.data(),
                      modeStr, gfx, romStatus.c_str());
        ImGui::TextDisabled("%s", buf);
    }
    ImGui::EndMainMenuBar();
}

void MainWindow::renderScreenWindow()
{
    // Default startup layout (2026-05-15 canonical): Apple II Screen
    // anchors the left column, unified Disk Library on the right. Y
    // starts at 90 to clear the menu bar (~28) + toolbar (~32) + a
    // small breathing margin. Window is ~1568×850 (see main.cpp),
    // leaving room for the 435 px Disk Library on the right.
    // `FirstUseEver` only applies on a fresh install — once the user
    // moves / resizes the window their imgui.ini takes over.
    ImGui::SetNextWindowPos (ImVec2(5,    90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1115, 745), ImGuiCond_FirstUseEver);

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
        {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            const float  th = ImGui::GetFrameHeight();
            const ImVec2 m  = ImGui::GetIO().MousePos;
            const bool overTitleBar =
                (m.x >= wp.x && m.x <= wp.x + ws.x &&
                 m.y >= wp.y && m.y <= wp.y + th);
            if (overTitleBar && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                             && !ImGui::IsAnyItemActive()) {
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

    // Scale to fill the content region while preserving the 4:3 aspect,
    // then centre — letterboxes on a wider/taller region (e.g. a full
    // kiosk viewport). Never shrink below 1× (tiny windows just clip).
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float scale = std::min(avail.x / Apple2Display::kWidth,
                           avail.y / Apple2Display::kHeight);
    scale = std::max(scale, 1.0f);
    ImVec2 size(Apple2Display::kWidth  * scale,
                Apple2Display::kHeight * scale);

    ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(
        cur.x + std::max(0.0f, (avail.x - size.x) * 0.5f),
        cur.y + std::max(0.0f, (avail.y - size.y) * 0.5f)));

    ImGui::Image(static_cast<ImTextureID>(screenTexture), size);
    // Capture the screen widget's screen-space rect so the GLFW
    // cursor-pos callback (Phase 5) can route motion over the
    // screen to the Mouse Card.
    screenRectMin = ImGui::GetItemRectMin();
    screenRectMax = ImGui::GetItemRectMax();
}

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

// ─── Mouse Card input routing ───────────────────────────────────────────

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

    // Either MAME-faithful MouseCard or AppleWin HLE MouseCardAppleWin
    // can be plugged (mutually exclusive). Both expose the same
    // `setHostMouse(rawX, rawY, button)` + `getSlot()` API; route through
    // tiny lambdas so the absolute / relative cursor logic below stays
    // variant-agnostic.
    if (!mouseCard && !mouseAwCard) return;
    auto pushMouse = [&](uint8_t rx, uint8_t ry, bool btn) {
        if (mouseCard)   mouseCard  ->setHostMouse(rx, ry, btn);
        if (mouseAwCard) mouseAwCard->setHostMouse(rx, ry, btn);
    };
    const int activeMouseSlot = mouseCard ? mouseCard->getSlot()
                                          : mouseAwCard->getSlot();

    // Gate on cursor inside the Apple II Screen widget. Outside, leave
    // the host mouse free for ImGui interaction.
    const float widgetW = screenRectMax.x - screenRectMin.x;
    const float widgetH = screenRectMax.y - screenRectMin.y;
    if (widgetW <= 0.0f || widgetH <= 0.0f) return;
    if (x < screenRectMin.x || x > screenRectMax.x ||
        y < screenRectMin.y || y > screenRectMax.y) {
        return;
    }

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
    // Only the primary button is wired to the Apple Mouse Card (PB7 of
    // the MCU). GLFW button 0 = left.
    if (button != 0) return;
    // Gate on cursor-in-screen-rect: clicks outside the Apple II Screen
    // widget belong to ImGui (menus, panels, etc.). Inside the screen
    // widget, we route to the Mouse Card. PRESS uses the current cursor
    // position; RELEASE always passes through so a button pressed inside
    // the screen but released outside still gets cleared on the card.
    const bool press = (action != 0);
    if (press) {
        const double x = lastMouseHostX;
        const double y = lastMouseHostY;
        if (x < screenRectMin.x || x > screenRectMax.x ||
            y < screenRectMin.y || y > screenRectMax.y) {
            return;
        }
    }
    mouseButtonHeld = press;             // GLFW_RELEASE = 0, others = press/repeat
    if (mouseCard)
        mouseCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
    if (mouseAwCard)
        mouseAwCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::renderControlsWindow()
{
    if (!showEmulationPanel) return;
    ImGui::SetNextWindowPos (ImVec2(1095, 780), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  210), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Emulation", &showEmulationPanel)) {
        // CPU speed control.
        int cycPerFrame = controller->getCyclesPerFrame();
        const int oneX  = 17045;
        const int twoX  = 34091;
        const int maxX  = 1'000'000;
        ImGui::Text("Speed: %d cycles/frame (~%.2f MHz)",
                    cycPerFrame, cycPerFrame * 60.0 / 1e6);
        if (ImGui::Button("1x"))  {
            controller->setCyclesPerFrame(oneX);
            if (diskTurboActive) diskSavedCyclesPerFrame = oneX;
        }
        ImGui::SameLine();
        if (ImGui::Button("2x"))  {
            controller->setCyclesPerFrame(twoX);
            if (diskTurboActive) diskSavedCyclesPerFrame = twoX;
        }
        ImGui::SameLine();
        if (ImGui::Button("MAX")) {
            controller->setCyclesPerFrame(maxX);
            if (diskTurboActive) diskSavedCyclesPerFrame = maxX;
        }

        ImGui::Separator();
        // Snapshot live CPU/Memory state under stateMutex — the worker thread
        // writes these (PC/regs every instruction, cycle counter via
        // advanceCycles) so reading them unlocked from the UI thread was a data
        // race. Copy under the lock, then format from the locals (don't hold the
        // lock across ImGui calls), matching the other UI snapshot sites.
        uint16_t pc; uint8_t a, x, y, sp; uint64_t cyc, spkToggles;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            pc  = controller->cpu().getProgramCounter();
            a   = controller->cpu().getAccumulator();
            x   = controller->cpu().getXRegister();
            y   = controller->cpu().getYRegister();
            sp  = controller->cpu().getStackPointer();
            cyc = controller->memory().getCycleCounter();
            spkToggles = controller->memory().getSpeakerToggleCount();
        }
        ImGui::Text("PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X", pc, a, x, y, sp);
        ImGui::Text("Cycles: %llu", (unsigned long long)cyc);
        ImGui::Text("Speaker toggles: %llu", (unsigned long long)spkToggles);

        // Audio mixing controls moved to the dedicated Audio Mixer panel
        // (View → Audio Mixer). Keep a one-liner so users notice the
        // shortcut.
        ImGui::TextDisabled("Audio: see View → Audio Mixer");

        ImGui::Separator();
        const size_t pendingPaste = controller->memory().pendingPasteSize();
        if (pendingPaste > 0) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.15f, 1.0f),
                               "Paste in flight: %zu chars", pendingPaste);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##paste")) {
                controller->memory().cancelPaste();
            }
        }
        ImGui::TextWrapped("ROM: %s", romStatus.c_str());
        if (!hdvStatus.empty()) ImGui::TextWrapped("HDV: %s", hdvStatus.c_str());
    }
    ImGui::End();
}

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    const std::string p = dev->getImagePath();
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

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. No need to
    // hold stateMutex — these setters write atomic-friendly scalars.
    Memory& mem = controller->memory();
    for (int i = 0; i < 4; ++i)  mem.setPaddle(i, joystick->paddleValue(i));
    for (int i = 0; i < 3; ++i)  mem.setPaddleButton(i, joystick->buttonDown(i));
}

void MainWindow::renderSscPanelWindow()
{
    if (!showSscPanel || !sscCard) return;

    ImGui::SetNextWindowSize(ImVec2(440, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Super Serial (slot 2)", &showSscPanel)) {
        ImGui::End();
        return;
    }

    const bool listening = sscCard->isListening();
    const bool connected = sscCard->clientConnected();

    ImGui::Text("Status: %s%s",
        listening ? "listening" : "stopped",
        connected ? " — client connected" : "");
    ImGui::SameLine();
    ImGui::TextDisabled("(slot %d)", sscCard->getSlot());

    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("TCP port", &sscPortInput, 0, 0);
    if (sscPortInput < 1)     sscPortInput = 1;
    if (sscPortInput > 65535) sscPortInput = 65535;

    ImGui::SameLine();
    if (!listening) {
        if (ImGui::Button("Start listener")) {
            if (!sscCard->startListening(static_cast<uint16_t>(sscPortInput))) {
                tapeStatusMessage = "SSC: bind failed (port busy?)";
                tapeStatusUntil   = lastFrameTime + 4.0;
            }
        }
    } else {
        if (ImGui::Button("Stop listener")) sscCard->stopListening();
    }

    if (listening) {
        ImGui::TextWrapped("Connect from a host terminal:");
        ImGui::TextWrapped("  telnet 127.0.0.1 %d", sscCard->getPort());
        ImGui::TextWrapped("In the Apple II:  PR#%d  (or IN#%d for input)",
            sscCard->getSlot(), sscCard->getSlot());
    } else {
        ImGui::TextDisabled("Click Start, then telnet to the port to bridge "
                            "I/O between your host shell and the Apple II.");
    }

    ImGui::Separator();
    bool raw = sscCard->rawMode();
    if (ImGui::Checkbox("Raw mode (8-bit binary)", &raw)) {
        sscCard->setRawMode(raw);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off: stock telnet — IAC ($FF) negotiation\n"
                          "swallowed + CR/LF normalised to CR.\n"
                          "On: every byte forwarded verbatim. Use for\n"
                          "XMODEM / Kermit / ADTPro / any binary protocol.");
    }

    ImGui::Separator();
    ImGui::Text("RX (telnet → A2): %llu B",
        static_cast<unsigned long long>(sscCard->bytesRx()));
    ImGui::Text("TX (A2 → telnet): %llu B",
        static_cast<unsigned long long>(sscCard->bytesTx()));

    if (ImGui::CollapsingHeader("Recent traffic")) {
        ImGui::TextDisabled("Last bytes the Apple II printed via PR#%d:",
                            sscCard->getSlot());
        ImGui::TextWrapped("%s", sscCard->recentTxText().c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Last bytes the host typed:");
        ImGui::TextWrapped("%s", sscCard->recentRxText().c_str());
    }

    ImGui::End();
}

void MainWindow::renderAiControlPanelWindow()
{
    if (!showAiControlPanel) return;

    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Control (HTTP)", &showAiControlPanel)) {
        ImGui::End();
        return;
    }

    const bool running = aiServer->isRunning();
    ImGui::Text("Status: %s", running ? "listening" : "stopped");
    if (running) {
        ImGui::SameLine();
        ImGui::TextDisabled("on 127.0.0.1:%u", aiServer->getPort());
    }
    ImGui::Text("Requests served: %llu",
                static_cast<unsigned long long>(aiServer->requestsServed()));
    {
        const std::string addr = aiServer->lastClientAddr();
        if (!addr.empty()) ImGui::Text("Last client: %s", addr.c_str());
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("TCP port", &aiPortInput, 0, 0);
    if (aiPortInput < 1)     aiPortInput = 1;
    if (aiPortInput > 65535) aiPortInput = 65535;

    char tokenBuf[128];
    std::snprintf(tokenBuf, sizeof(tokenBuf), "%s", aiTokenInput.c_str());
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("Auth token (empty = open)", tokenBuf, sizeof(tokenBuf))) {
        aiTokenInput = tokenBuf;
        aiServer->setAuthToken(aiTokenInput);
    }

    ImGui::SameLine();
    if (!running) {
        if (ImGui::Button("Start")) {
            // Re-attach in case slot cards were rebuilt by the slot config
            // panel since the last start — pointers may have moved.
            aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
            aiServer->setAuthToken(aiTokenInput);
            if (!aiServer->start(static_cast<uint16_t>(aiPortInput))) {
                tapeStatusMessage = "AI Control: bind failed (port busy?)";
                tapeStatusUntil   = lastFrameTime + 4.0;
            }
        }
    } else {
        if (ImGui::Button("Stop")) aiServer->stop();
    }

    ImGui::Separator();
    ImGui::TextWrapped(
        "Drive POM2 from an AI agent (or curl/Postman) via HTTP/1.1.");
    ImGui::Spacing();
    ImGui::TextDisabled("Example:");
    ImGui::TextWrapped("  curl http://127.0.0.1:%d/status", aiPortInput);
    ImGui::TextWrapped(
        "  curl -d '{\"text\":\"PRINT 1+1\\r\"}' http://127.0.0.1:%d/keyboard",
        aiPortInput);
    ImGui::TextWrapped(
        "  curl http://127.0.0.1:%d/screen.ppm -o screen.ppm",
        aiPortInput);
    if (!aiTokenInput.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Send 'X-POM2-Token: <token>' header on each request.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Endpoints: /status /reset /cpu /mem /keyboard /disk "
                        "/eject /snapshot/save /snapshot/load /speed /screen.ppm");

    ImGui::End();
}

void MainWindow::renderJoystickPanelWindow()
{
    if (!showJoystickPanel) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick->deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick->binding();
    snap.hostIdx  = cf.hostIdx;
    snap.deadzone = cf.deadzone;
    snap.invert   = cf.invert;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick->paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick->buttonDown(i);

    auto result = joystickPanel->render("Joystick", showJoystickPanel, snap);
    if (result.changed) {
        auto& bind = joystick->binding();
        bind.hostIdx  = result.hostIdx;
        bind.deadzone = result.deadzone;
        bind.invert   = result.invert;
    }
}

// ─── Audio Mixer ─────────────────────────────────────────────────────────
//
// Consolidated mixer panel: one row per source (Master, Speaker, Cassette,
// Mockingboard if plugged, Disk 5.25", Disk 3.5" if its sample bank
// loaded). Sliders + mute checkboxes write directly into the underlying
// atomics on each source / on AudioDevice — no UI-side cache, so cross-
// thread read-back stays consistent. Replaces the two sliders that used
// to live in the Status panel.

void MainWindow::renderAudioMixerWindow()
{
    if (!showAudioMixer) return;

    ImGui::SetNextWindowPos (ImVec2(80, 80),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 260), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Mixer", &showAudioMixer)) {
        ImGui::End();
        return;
    }

    auto channelRow = [](const char* label, float& vol, bool& mute,
                         float peak, const char* idSuffix, bool dim) {
        if (dim) ImGui::BeginDisabled();
        ImGui::PushID(idSuffix);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(110.0f);
        ImGui::SetNextItemWidth(160.0f);
        const std::string slid = std::string("##v") + idSuffix;
        ImGui::SliderFloat(slid.c_str(), &vol, 0.0f, 2.0f, "%.2f");
        // Tiny activity meter: lets the user confirm at a glance that
        // the channel is actually producing samples (addresses the
        // "doesn't seem connected to the mixer" feedback). Green for
        // safe levels, yellow approaching clip, red at clip.
        ImGui::SameLine();
        const float clamped = std::min(1.0f, std::max(0.0f, peak));
        ImVec4 col(0.20f, 0.80f, 0.20f, 1.0f);
        if      (clamped >= 0.95f) col = ImVec4(0.90f, 0.20f, 0.20f, 1.0f);
        else if (clamped >= 0.70f) col = ImVec4(0.90f, 0.75f, 0.20f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(clamped, ImVec2(40.0f, 0.0f), "");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::string muteId = std::string("Mute##") + idSuffix;
        ImGui::Checkbox(muteId.c_str(), &mute);
        ImGui::PopID();
        if (dim) ImGui::EndDisabled();
    };

    // ── Master ─────────────────────────────────────────────────────────
    AudioDevice& dev = controller->audio();
    float masterVol = dev.getMasterVolume();
    bool  masterMute = dev.isMasterMuted();
    channelRow("Master", masterVol, masterMute, dev.getMasterPeak(),
               "master", false);
    if (masterVol != dev.getMasterVolume()) dev.setMasterVolume(masterVol);
    if (masterMute != dev.isMasterMuted()) dev.setMasterMuted(masterMute);
    ImGui::Separator();

    // ── Speaker ────────────────────────────────────────────────────────
    SpeakerDevice& spk = controller->speaker();
    float spkVol = spk.getVolume();
    bool  spkMute = spk.isMuted();
    channelRow("Speaker", spkVol, spkMute,
               spk.lastBufferPeak.load(std::memory_order_relaxed),
               "spk", false);
    if (spkVol != spk.getVolume()) spk.setVolume(spkVol);
    if (spkMute != spk.isMuted()) spk.setMuted(spkMute);

    // ── Cassette ───────────────────────────────────────────────────────
    CassetteDevice& tape = controller->cassette();
    float tapeVol = tape.getVolume();
    bool  tapeMute = tape.isMuted();
    channelRow("Cassette", tapeVol, tapeMute,
               tape.lastBufferPeak.load(std::memory_order_relaxed),
               "tape", false);
    if (tapeVol != tape.getVolume()) tape.setVolume(tapeVol);
    if (tapeMute != tape.isMuted()) tape.setMuted(tapeMute);

    // ── Mockingboard ───────────────────────────────────────────────────
    if (mockingboardCard) {
        float mbVol = mockingboardCard->getVolume();
        bool  mbMute = mockingboardCard->isMuted();
        const std::string lbl = "Mockingbd (S" +
            std::to_string(mockingboardCard->getSlot()) + ")";
        AudioSource* mbSrc = mockingboardCard->audioSource();
        const float mbPeak = mbSrc
            ? mbSrc->lastBufferPeak.load(std::memory_order_relaxed)
            : 0.0f;
        channelRow(lbl.c_str(), mbVol, mbMute, mbPeak, "mb", false);
        if (mbVol != mockingboardCard->getVolume()) mockingboardCard->setVolume(mbVol);
        if (mbMute != mockingboardCard->isMuted()) mockingboardCard->setMuted(mbMute);
    } else {
        float dummyVol = 0; bool dummyMute = false;
        channelRow("Mockingbd (no card)", dummyVol, dummyMute, 0.0f, "mb", true);
    }

    // ── Disk 5.25" ─────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs525 = controller->floppySound525();
        const bool dim = !fs525.isLoaded();
        float vol = fs525.getVolume();
        bool  mute = fs525.isMuted();
        channelRow(dim ? "Disk 5.25\" (samples missing)" : "Disk 5.25\"",
                   vol, mute,
                   fs525.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs525", dim);
        if (!dim) {
            if (vol != fs525.getVolume()) fs525.setVolume(vol);
            if (mute != fs525.isMuted()) fs525.setMuted(mute);
        }
    }

    // ── Disk 3.5" ──────────────────────────────────────────────────────
    {
        FloppySoundDevice& fs35 = controller->floppySound35();
        const bool dim = !fs35.isLoaded();
        float vol = fs35.getVolume();
        bool  mute = fs35.isMuted();
        channelRow(dim ? "Disk 3.5\" (samples missing)" : "Disk 3.5\"",
                   vol, mute,
                   fs35.lastBufferPeak.load(std::memory_order_relaxed),
                   "fs35", dim);
        if (!dim) {
            if (vol != fs35.getVolume()) fs35.setVolume(vol);
            if (mute != fs35.isMuted()) fs35.setMuted(mute);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Master is post-mix; per-channel knobs are pre-mix.");
    ImGui::TextDisabled("Bars show last-buffer peak with ~100 ms release.");
    ImGui::End();
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!showChatMauvePanel) return;

    pom2::LeChatMauve_ImGui::Snapshot snap;
    if (chatMauveCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.plugged   = true;
        snap.mode      = chatMauveCard->currentMode();
        snap.fifoBits  = chatMauveCard->fifoBits();
        snap.eightyCol = chatMauveCard->eightyCol();
        snap.an3High   = chatMauveCard->an3High();
    }

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel->render("Le Chat Mauve (slot 7)",
                                        showChatMauvePanel, snap);

    if (chatMauveCard && result.requestOverride) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        chatMauveCard->overrideMode(result.overrideTo);
    }
    if (chatMauveCard && result.requestReset) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        chatMauveCard->onReset();
    }
}

// ─── Mockingboard live state panel ───────────────────────────────────────
//
// Diagnostic window for the Mockingboard A/C. Shows both 6522 VIAs' T1
// counter / IFR / IER + slot IRQ state, and both AY-3-8910 register
// banks (R0..R15). Primary use case: figuring out why an IRQ-driven
// music driver is silent. Three observable cases:
//
//   1. AY registers all stay 0 — the music handler isn't running at
//      all. Check IFR/IER + irqAsserted on the VIA. If T1 ticks but
//      IRQ never asserts, the IER is wrong or the driver hasn't
//      enabled T1 yet.
//   2. AY registers move every few frames — the driver is running and
//      the AY synth is producing samples; if you hear nothing, look at
//      AudioDevice (volume/mute) or the channel mixer R7.
//   3. AY registers load once and freeze — the install ran but only
//      one IRQ landed. Likely the handler isn't re-arming T1 or the
//      ack path is broken.
//
// The panel takes the controller state mutex for each snapshot and
// reads via the card's existing test/debug accessors
// (`peekViaRegister`, `getAyRegister`, `isIrqAsserted`).
void MainWindow::renderMockingboardPanelWindow()
{
    if (!showMockingboardPanel) return;

    ImGui::SetNextWindowPos (ImVec2(720, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mockingboard (VIA + AY state)",
                      &showMockingboardPanel,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!mockingboardCard) {
        ImGui::TextDisabled("No Mockingboard plugged. Use Hardware → Slot "
                            "Configuration to assign it to a slot.");
        ImGui::End();
        return;
    }

    // Snapshot all the per-chip state under one lock so the two VIAs and
    // two AYs are coherent against each other. The card's accessors are
    // const-ish reads — no side effects on the running emulator.
    struct ChipSnap {
        uint8_t t1cl, t1ch, t1ll, t1lh, sr, acr, pcr, ifr, ier;
        uint8_t ay[16];
        uint32_t viaWrites, ayWrites, ayResets;
        uint32_t cmdInactive, cmdRead, cmdWrite, cmdLatch;
    } via[2]{};
    bool slotIrq = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        slotIrq = mockingboardCard->isIrqAsserted();
        for (int c = 0; c < 2; ++c) {
            via[c].t1cl = mockingboardCard->peekViaRegister(c, 0x04);
            via[c].t1ch = mockingboardCard->peekViaRegister(c, 0x05);
            via[c].t1ll = mockingboardCard->peekViaRegister(c, 0x06);
            via[c].t1lh = mockingboardCard->peekViaRegister(c, 0x07);
            via[c].sr   = mockingboardCard->peekViaRegister(c, 0x0A);
            via[c].acr  = mockingboardCard->peekViaRegister(c, 0x0B);
            via[c].pcr  = mockingboardCard->peekViaRegister(c, 0x0C);
            via[c].ifr  = mockingboardCard->peekViaRegister(c, 0x0D);
            via[c].ier  = mockingboardCard->peekViaRegister(c, 0x0E);
            for (int r = 0; r < 16; ++r) {
                via[c].ay[r] = mockingboardCard->getAyRegister(c, r);
            }
            via[c].viaWrites = mockingboardCard->getViaWriteCount(c);
            via[c].ayWrites  = mockingboardCard->getAyWriteCount(c);
            via[c].ayResets  = mockingboardCard->getAyResetCount(c);
            via[c].cmdInactive = mockingboardCard->getAyCommandCount(c, 0);
            via[c].cmdRead     = mockingboardCard->getAyCommandCount(c, 1);
            via[c].cmdWrite    = mockingboardCard->getAyCommandCount(c, 2);
            via[c].cmdLatch    = mockingboardCard->getAyCommandCount(c, 3);
        }
    }

    // Slot IRQ indicator and volume readout.
    ImGui::Text("Slot IRQ line: %s", slotIrq ? "ASSERTED (low)" : "released");
    ImGui::SameLine();
    ImGui::TextDisabled(" | Volume: %.2f %s",
                        mockingboardCard->getVolume(),
                        mockingboardCard->isMuted() ? "(MUTED)" : "");
    ImGui::Separator();

    // Two-column display, one per 6522+AY pair.
    if (ImGui::BeginTable("##mb_chips", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("VIA #1 + AY #0 ($Cn00-$Cn0F)");
        ImGui::TableSetupColumn("VIA #2 + AY #1 ($Cn80-$Cn8F)");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        for (int c = 0; c < 2; ++c) {
            ImGui::TableSetColumnIndex(c);
            const auto& v = via[c];
            // Telemetry first — these counters tell you instantly
            // whether the music driver is even talking to this VIA.
            // viaWrites grows on every guest STA $CnXX (Mockingboard
            // slot-ROM window); ayWrites grows only when LATCH→WRITE
            // strobes successfully complete and a register lands in
            // the AY. Both staying near zero mid-game = the driver
            // isn't running or its IRQ handler is short-circuiting
            // before the AY phase.
            ImGui::Text("VIA writes : %u", v.viaWrites);
            ImGui::Text("AY writes  : %u  (register-store WRITE strobes)",
                        v.ayWrites);
            ImGui::Text("AY resets  : %u  (!RESET pulses, wipes all regs)",
                        v.ayResets);
            ImGui::TextDisabled("AY cmd:  LATCH=%u WRITE=%u INACT=%u READ=%u",
                                v.cmdLatch, v.cmdWrite, v.cmdInactive,
                                v.cmdRead);
            ImGui::Separator();
            ImGui::Text("T1 ctr  $%02X%02X   latch $%02X%02X",
                        v.t1ch, v.t1cl, v.t1lh, v.t1ll);
            ImGui::Text("ACR=$%02X  PCR=$%02X  SR=$%02X",
                        v.acr, v.pcr, v.sr);
            ImGui::Text("IFR=$%02X  IER=$%02X  T1en=%s",
                        v.ifr, v.ier, (v.ier & 0x40) ? "yes" : "no");
            const bool t1Fired = (v.ifr & 0x40) != 0;
            ImGui::TextColored(t1Fired
                                 ? ImVec4(1.0f, 0.6f, 0.2f, 1.0f)
                                 : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "IFR.T1: %s", t1Fired ? "PENDING" : "clear");
            ImGui::Separator();
            ImGui::TextDisabled("AY-3-8910 registers:");
            // Two columns inside the cell: 8 regs each.
            for (int row = 0; row < 8; ++row) {
                const int r0 = row;
                const int r1 = row + 8;
                ImGui::Text("R%-2d %3u $%02X    R%-2d %3u $%02X",
                            r0, v.ay[r0], v.ay[r0],
                            r1, v.ay[r1], v.ay[r1]);
            }
            // Friendly labels for the regs that decide audibility.
            ImGui::Separator();
            const uint16_t periodA = v.ay[0] | ((v.ay[1] & 0x0F) << 8);
            const uint16_t periodB = v.ay[2] | ((v.ay[3] & 0x0F) << 8);
            const uint16_t periodC = v.ay[4] | ((v.ay[5] & 0x0F) << 8);
            ImGui::Text("Ch A period $%03X  vol $%X", periodA, v.ay[8] & 0x1F);
            ImGui::Text("Ch B period $%03X  vol $%X", periodB, v.ay[9] & 0x1F);
            ImGui::Text("Ch C period $%03X  vol $%X", periodC, v.ay[10] & 0x1F);
            ImGui::Text("Mixer $%02X (tone %c%c%c noise %c%c%c)",
                        v.ay[7],
                        (v.ay[7] & 0x01) ? '.' : 'A',
                        (v.ay[7] & 0x02) ? '.' : 'B',
                        (v.ay[7] & 0x04) ? '.' : 'C',
                        (v.ay[7] & 0x08) ? '.' : 'A',
                        (v.ay[7] & 0x10) ? '.' : 'B',
                        (v.ay[7] & 0x20) ? '.' : 'C');
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ─── Disk II ─────────────────────────────────────────────────────────────

void MainWindow::updateAutoTurbo()
{
    // Auto-turbo: while a disk is actively streaming, crank the CPU to ~60x
    // emulated so loads don't crawl at 1 MHz. Two activity sources:
    //
    //   • DiskII (5.25"): the motor line. Multi-instance friendly — one card
    //     spinning up is enough to enter turbo; all must be idle to leave it.
    //   • ProDOS hard disk (ProDOSHardDiskCard): no motor line, so the byte-
    //     loop firmware streams blocks at the plain CPU rate and HD games
    //     (e.g. Nox Archaist) load far slower than a 5.25" game that gets the
    //     motor-on turbo. Treat recent HDV data-port activity as the same
    //     "busy" signal; the card decays it over a few frames so a multi-block
    //     load stays in turbo end-to-end, then drops back for gameplay.
    //
    // Called every frame from render() (NOT from renderDiskPanelWindow, which
    // early-returns when its window is hidden — the default).
    bool anyMotorOn = false;
    for (auto* c : diskCards) {
        if (c && c->isMotorOn()) { anyMotorOn = true; break; }
    }
    // Decay + poll EVERY block card (HDV + CFFA can coexist) so a load on
    // either keeps turbo engaged. tickActivityDecay() must run on each card
    // (not short-circuit) so their independent decay counters all advance.
    bool hdvBusy = false;
    const auto blocks = blockCards();
    for (auto* dev : blocks) {
        dev->tickActivityDecay();
        if (dev->isBusy()) hdvBusy = true;
    }
    const bool anyBusy       = anyMotorOn || hdvBusy;
    const bool turboEligible =
        diskTurboWhileMotor && (!diskCards.empty() || !blocks.empty());
    if (turboEligible) {
        if (anyBusy && !diskTurboActive) {
            diskSavedCyclesPerFrame = controller->getCyclesPerFrame();
            controller->setCyclesPerFrame(1'000'000);
            diskTurboActive = true;
        } else if (!anyBusy && diskTurboActive) {
            controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
        diskTurboActive = false;
    }
}

void MainWindow::renderDiskPanelWindow()
{
    if (!showDiskPanel) return;

    // Disk library is the same for every plugged DiskII (it's the
    // contents of disks/ on disk). Build it once and share via copy.
    std::vector<pom2::DiskController_ImGui::LibraryEntry> sharedLibrary;
    // Disk library — scan disks/ recursively for .dsk/.do/.po/.nib/.woz.
    // Sub-folders are honoured so users can shelve their library by
    // format (`disks/dsk/`, `disks/woz/`, …) or by collection
    // (`disks/games/`, `disks/dev/`, …) without losing the one-click
    // boot. Cheap (a few dirent reads per frame); sorted alphabetically
    // so the list doesn't reshuffle as the OS hands us a different
    // dirent order. `displayName` carries the path relative to the
    // scanned root so two files of the same name in different sub-
    // folders don't collide visually.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks", "../disks", "../../disks" };
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
    for (size_t idx = 0; idx < diskCards.size() && idx < diskPanels.size(); ++idx) {
        DiskIICard*                       card  = diskCards[idx];
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
        // Only the primary card honours `showDiskPanel` (the menu toggle).
        // Secondary cards share the same toggle for simplicity — the user
        // sees them appear/disappear together. We feed the same flag to
        // each render() call.
        auto result = panel->render(title, showDiskPanel, snap);

        if (result.turboToggleChanged) {
            diskTurboWhileMotor = result.turboNewValue;
        }
        if (result.writeBackToggleChanged) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->setWriteBackEnabled(result.writeBackNewValue);
            tapeStatusMessage = "slot " + std::to_string(card->getSlot()) +
                (result.writeBackNewValue
                    ? ": write-back ENABLED (saves on eject)"
                    : ": write-back disabled");
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        if (result.requestEject) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->ejectDisk();
            tapeStatusMessage = "Disk ejected (slot " +
                std::to_string(card->getSlot()) + ")";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (result.requestBoot) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            card->seekTrack0();
            const uint16_t pc = static_cast<uint16_t>(
                0xC000 + (card->getSlot() << 8));
            controller->cpu().setProgramCounter(pc);
            controller->setMode(EmulationController::Mode::Running);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Boot: PC → $%04X", pc);
            tapeStatusMessage = msg;
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        if (!result.requestInsertAndBoot.empty()) {
            const std::string path = result.requestInsertAndBoot;
            bool ok = false;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = card->insertDisk(path);
                if (ok) card->seekTrack0();
                else    err = card->getLastError();
            }
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
            bool ok = false;
            std::string err;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = card->insertDisk(path);
                if (!ok) err = card->getLastError();
            }
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

void MainWindow::ejectAllDisks()
{
    // Eject under stateMutex for the things we own directly (DiskII, HDV,
    // SmartPort units). `eject35` re-locks stateMutex itself — call it
    // OUTSIDE the lock to avoid recursive locking on the non-recursive
    // std::mutex (was crashing POM2 when Eject-All was clicked).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) if (c) c->ejectDisk();
        // Every block-device card (HDV + CFFA may both be plugged).
        for (auto* dev : blockCards()) dev->ejectImage();
        // Every SmartPort card, and every unit inside each.
        bool dirty = false;
        for (auto* sp : smartPortCards()) {
            for (size_t k = 0; k < pom2::SmartPortCard::kMaxUnits; ++k) {
                pom2::SmartPortUnit* u = sp->unit(k);
                if (u && u->isLoaded()) {
                    u->eject();
                    const std::string base =
                        "smartport_slot" + std::to_string(sp->getSlot()) +
                        "_unit" + std::to_string(k);
                    settings->setString(base + "_path", "");
                    dirty = true;
                }
            }
        }
        if (dirty) settings->save();
    }
    controller->eject35(0);
    controller->eject35(1);
    tapeStatusMessage = "Ejected all disks";
    tapeStatusUntil   = lastFrameTime + 3.0;
}

bool MainWindow::routeMount35(int driveIdx, const std::string& path,
                              std::string& errOut)
{
    // Whenever a SmartPort card is plugged its units own 3.5" media;
    // auto-create a SmartPort35Unit on the target index if it's empty or
    // holds a different kind (HDV). This now includes //c-class: the //c /
    // //c+ built-in SmartPort (slot 5) is the boot path POM2 exposes
    // (block-level — the on-board IWM/Sony GCR boot is unmodelled, see
    // project_iic_smartport_boot). (Promoted from a lambda in
    // renderDiskLibraryWindow so the CLI insert+boot path shares it.)
    if (smartPortCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::SmartPortUnit* u =
            smartPortCard->unit(static_cast<size_t>(driveIdx));
        if (!u || u->kindKey() != pom2::SmartPort35Unit::kKindKey) {
            smartPortCard->setUnit(
                static_cast<size_t>(driveIdx),
                std::make_unique<pom2::SmartPort35Unit>());
            u = smartPortCard->unit(static_cast<size_t>(driveIdx));
        }
        if (!u->loadImage(path)) {
            errOut = u->lastError();
            return false;
        }
        const std::string base =
            "smartport_slot" + std::to_string(smartPortCard->getSlot()) +
            "_unit" + std::to_string(driveIdx);
        settings->setString(base + "_type",
            std::string(pom2::SmartPort35Unit::kKindKey));
        settings->setString(base + "_path", path);
        if (!kiosk_) settings->save();   // kiosk is read-only: never touch state.cfg
        return true;
    }
    // //c+ on-board path.
    if (!controller->mount35(driveIdx, path)) {
        const auto& img = (driveIdx == 0)
            ? controller->disk35Internal()
            : controller->disk35External();
        errOut = img.lastError();
        return false;
    }
    return true;
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
         activeProfile == pom2::SystemProfile::AppleIIcPlus);
    // Prefer a dedicated HDV-class card — the MAME-faithful CffaCard if
    // plugged, else the synthetic ProDOSHardDiskCard; else route to a
    // SmartPort card's unit 0 (auto-creating a SmartPortHdvUnit). Promoted
    // from a lambda in renderDiskLibraryWindow.
    if (!iicClass) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (!dev->loadImage(path)) {
                errOut = dev->getLastError();
                hdvStatus = "no image mounted";
                return false;
            }
            hdvPath = path;
            hdvStatus = "loaded: " + path;
            bootSlotOut = dev->getSlot();
            return true;
        }
    }
    if (smartPortCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        pom2::SmartPortUnit* u = smartPortCard->unit(0);
        if (!u || u->kindKey() != pom2::SmartPortHdvUnit::kKindKey) {
            smartPortCard->setUnit(
                0, std::make_unique<pom2::SmartPortHdvUnit>());
            u = smartPortCard->unit(0);
        }
        if (!u->loadImage(path)) {
            errOut = u->lastError();
            return false;
        }
        const std::string base =
            "smartport_slot" + std::to_string(smartPortCard->getSlot()) +
            "_unit0";
        settings->setString(base + "_type",
            std::string(pom2::SmartPortHdvUnit::kKindKey));
        settings->setString(base + "_path", path);
        if (!kiosk_) settings->save();   // kiosk is read-only: never touch state.cfg
        bootSlotOut = smartPortCard->getSlot();
        return true;
    }
    errOut = "no HDV or SmartPort card plugged";
    return false;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    if (cffaCard) return static_cast<pom2::ProDOSBlockCard*>(cffaCard);
    if (hdvCard)  return static_cast<pom2::ProDOSBlockCard*>(hdvCard);
    return nullptr;
}

std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    // Walk the bus (slots 1..7) and cross-cast each plugged peripheral to
    // the ProDOSBlockCard mix-in. Both implementers (ProDOSHardDiskCard,
    // CffaCard) inherit SlotPeripheral *and* ProDOSBlockCard, so the
    // dynamic_cast side-cast succeeds for exactly those and yields nullptr
    // for everything else. Slot order is ascending, matching the "lowest
    // slot is primary" convention used for diskCard/hdvCard.
    std::vector<pom2::ProDOSBlockCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int s = 1; s < SlotBus::kSlotCount; ++s) {
        if (auto* blk = dynamic_cast<pom2::ProDOSBlockCard*>(bus.peripheral(s)))
            out.push_back(blk);
    }
    return out;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    std::vector<pom2::SmartPortCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int s = 1; s < SlotBus::kSlotCount; ++s) {
        if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(bus.peripheral(s)))
            out.push_back(sp);
    }
    return out;
}

int MainWindow::ensureHdvCardForBoot()
{
    // On //c-class only the on-board SmartPort (slot 5) is ROM-visible /
    // bootable — cffa/hdv slot cards are masked by the forced INTCXROM, so
    // prefer the SmartPort and never plug a (dead) ProDOSHardDiskCard here.
    // See project_iic_smartport_boot.
    if (activeProfile == pom2::SystemProfile::AppleIIc ||
        activeProfile == pom2::SystemProfile::AppleIIcPlus) {
        if (smartPortCard) return smartPortCard->getSlot();
    }
    if (cffaCard)      return cffaCard->getSlot();
    if (hdvCard)       return hdvCard->getSlot();
    if (smartPortCard) return smartPortCard->getSlot();

    // Neither an HDV nor a SmartPort card is plugged (e.g. a saved config
    // that only has Disk II cards). Plug a ProDOSHardDiskCard into a free
    // slot for THIS session so the CLI/kiosk launcher can still boot the
    // named HDV. Prefer slot 7 (conventional HDV/SmartPort slot), else the
    // highest free slot. Not persisted — the saved config is untouched.
    int slot = -1;
    for (int s = 7; s >= 1; --s) {
        if (!controller->memory().slotBus().isPlugged(s)) { slot = s; break; }
    }
    if (slot < 0) return -1;

    // Plug under the state lock (the slot is empty, so no card destructor
    // races the worker — a held lock is enough; no stop/start needed).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        auto card = std::make_unique<ProDOSHardDiskCard>(slot);
        hdvCard = card.get();
        controller->memory().slotBus().plug(slot, std::move(card));
        slotCards[slot] = "hdv";
        autoProvisionedHdvSlot_ = slot;   // session-local; ~MainWindow won't persist it
    }
    pom2::log().info("CLI",
        "auto-plugged ProDOS HDV card in slot " + std::to_string(slot) +
        " (saved slot config unchanged)");
    return slot;
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
    // Classify by extension + size, route into the matching slot under the
    // active profile/slot config, then cold-boot. Shared by the CLI
    // positional-disk / --kiosk launcher and (potentially) any future
    // single-call boot entry point. Mirrors the Disk Library "insert +
    // boot" buttons but with no UI surface.
    switch (classifyDiskForSlot(path)) {
        case DiskSlotClass::Floppy525: {
            // Prefer the Disk II in the conventional boot slot 6; fall back
            // to the primary (lowest-slot) card. Booting a single floppy
            // from a non-6 slot is unconventional and breaks software that
            // hardcodes slot 6 for its loader — matters when the config has
            // Disk II in several slots (primary = lowest = e.g. slot 5).
            DiskIICard* target = nullptr;
            for (auto* c : diskCards) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = diskCard;
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                ok = target->insertDisk(0, path);
                if (ok) target->seekTrack0();
                else    errOut = target->getLastError(0);
            }
            if (!ok) return false;
            controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            if (!routeMount35(0, path, errOut)) return false;
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (smartPortCard) {
                controller->bootFromSlot(smartPortCard->getSlot());
            } else {
                controller->coldBoot();
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
            controller->bootFromSlot(bootSlot);
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
    if (!showDiskLibrary) return;

    // Default position: right column of the curated 1568×850 layout,
    // flush against the screen window. 435 px wide × 745 px tall =
    // enough for the 3-tab table + the search/sort header without
    // scroll overflow on a typical 800+ disk library.
    ImGui::SetNextWindowPos (ImVec2(1125, 90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(435,  745), ImGuiCond_FirstUseEver);

    pom2::DiskLibrary_ImGui::CurrentlyMounted mounted;
    // Currently-inserted Disk II images (per plugged card). The library
    // tags rows with `* ` when their path matches any of these — gives
    // the user a visual cue across all plugged cards at once.
    for (auto* c : diskCards) {
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
    if (smartPortCard) {
        const pom2::SmartPortUnit* u0 = smartPortCard->unit(0);
        const pom2::SmartPortUnit* u1 = smartPortCard->unit(1);
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
    } else if (smartPortCard) {
        // SmartPort-routed HDV — show as mounted in the Library so the
        // `* ` marker matches reality regardless of which path holds it.
        const pom2::SmartPortUnit* u = smartPortCard->unit(0);
        if (u && u->isLoaded() &&
            u->kindKey() == pom2::SmartPortHdvUnit::kKindKey) {
            mounted.hdv = u->path();
        }
    }

    const auto r = diskLibrary->render("Disk Library", showDiskLibrary, mounted);

    // ── Eject-all (header-row button, moved here from the toolbar) ─────
    if (r.requestEjectAllDisks) ejectAllDisks();

    // ── 5.25" actions → the DiskII card/drive the right-click menu picked ──
    // request525Slot = -1 means "primary card" (left-click default); a real
    // slot routes to that specific DiskII card. drive 0 = drive 1, 1 = drive 2.
    auto resolve525 = [&](int slot) -> DiskIICard* {
        if (slot < 0) return diskCard;
        for (auto* c : diskCards) if (c && c->getSlot() == slot) return c;
        return diskCard;
    };
    if (!r.request525InsertAndBoot.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const std::string path = r.request525InsertAndBoot;
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (target) {
                ok = target->insertDisk(drive, path);
                if (ok) target->seekTrack0();
                else    err = target->getLastError(drive);
            }
        }
        if (ok && target) {
            // Boot the target card's slot (its boot PROM boots drive 1).
            controller->bootFromSlot(target->getSlot());
            controller->setMode(EmulationController::Mode::Running);
            tapeStatusMessage = "Library: inserted + booted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + "): " + path;
        } else {
            tapeStatusMessage = "Library: boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!r.request525InsertOnly.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (target && target->insertDisk(drive, r.request525InsertOnly)) {
            tapeStatusMessage = "Library: inserted (slot " +
                std::to_string(target->getSlot()) + " drive " +
                std::to_string(drive + 1) + ", no boot): " +
                r.request525InsertOnly;
        } else {
            tapeStatusMessage = "Library: insert failed: " +
                (target ? target->getLastError(drive) : std::string("no DiskII card"));
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
            // built-in SmartPort (slot 5). Falls back to `coldBoot()`
            // only when there is no SmartPort card at all.
            if (smartPortCard) {
                controller->bootFromSlot(smartPortCard->getSlot());
            } else {
                controller->coldBoot();
            }
            tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35BootDrive == 0 ? "1" : "2")
                + " booted: " + r.request35MountAndBoot;
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
            controller->bootFromSlot(bootSlot);
            tapeStatusMessage = "Library: HDV (slot " +
                std::to_string(bootSlot) + ") booted: " + path;
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

    // ── Eject actions ─────────────────────────────────────────────────
    // 5.25": eject from whichever plugged DiskII holds the clicked
    // image. Match by path so multi-instance DiskII setups (the same
    // image plugged into two slots) all clear together.
    if (!r.request525EjectPath.empty()) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) {
            if (!c) continue;
            for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                if (c->isDiskLoaded(d) &&
                    c->getDiskPath(d) == r.request525EjectPath) {
                    c->ejectDisk(d);
                }
            }
        }
        tapeStatusMessage = "Library: 5.25\" disk ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.request35EjectDrive >= 0) {
        controller->eject35(r.request35EjectDrive);
        tapeStatusMessage = "Library: 3.5\" drive "
            + std::string(r.request35EjectDrive == 0 ? "1" : "2") + " ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (r.requestHdvEject) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            dev->ejectImage();
            hdvPath.clear();
            hdvStatus = "no image mounted";
            tapeStatusMessage = "Library: HDV ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
    }
}

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

void MainWindow::renderSmartPortPanelWindow()
{
    if (!showSmartPortPanel) return;

    // Build snapshot from the currently-plugged SmartPort card. When no
    // card is plugged the panel renders a "no card" hint.
    pom2::SmartPort_ImGui::CardSnapshot snap;
    snap.plugged = (smartPortCard != nullptr);
    if (snap.plugged) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.slot = smartPortCard->getSlot();
        for (size_t k = 0; k < snap.units.size(); ++k) {
            const pom2::SmartPortUnit* u = smartPortCard->unit(k);
            auto& us = snap.units[k];
            if (!u) {
                us.kind.clear();
                us.kindLabel.clear();
                continue;
            }
            us.kind             = std::string(u->kindKey());
            us.kindLabel        = std::string(u->kindLabel());
            us.path             = u->path();
            us.lastError        = u->lastError();
            us.blockCount       = u->blockCount();
            us.loaded           = u->isLoaded();
            us.writeProtected   = u->isWriteProtected();
            us.writeBackEnabled = u->isWriteBackEnabled();
        }
    }

    char title[64];
    if (snap.plugged) {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration (slot %d)", snap.slot);
    } else {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration");
    }

    const auto r = smartPortPanel->render(title, showSmartPortPanel, snap);

    if (!snap.plugged) return;

    // Apply per-unit actions under the state mutex. Settings updates
    // (kind / path / writeback) happen here so a restart restores the
    // same layout. Eject + writeback writes pass through the unit's
    // own API; setUnit replaces the entire unit (e.g. type swap).
    const std::string slotKey =
        "smartport_slot" + std::to_string(snap.slot);
    bool dirtySettings = false;
    for (size_t k = 0; k < snap.units.size(); ++k) {
        const auto& a    = r.units[k];
        const std::string base = slotKey + "_unit" + std::to_string(k);

        if (a.clearType || !a.setType.empty()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (a.clearType) {
                smartPortCard->setUnit(k, nullptr);
                settings->setString(base + "_type", "");
                settings->setString(base + "_path", "");
                settings->setBool  (base + "_writeback", false);
                tapeStatusMessage = "SmartPort unit " +
                    std::to_string(k) + ": cleared";
            } else {
                auto unit = pom2::makeSmartPortUnit(a.setType);
                if (unit) {
                    settings->setString(base + "_type", a.setType);
                    settings->setString(base + "_path", "");
                    settings->setBool  (base + "_writeback", false);
                    smartPortCard->setUnit(k, std::move(unit));
                    tapeStatusMessage = "SmartPort unit " +
                        std::to_string(k) + ": type = " + a.setType;
                } else {
                    tapeStatusMessage = "SmartPort unit " +
                        std::to_string(k) + ": unknown type '" +
                        a.setType + "'";
                }
            }
            dirtySettings = true;
            tapeStatusUntil = lastFrameTime + 3.0;
            // Type change drops any previously-loaded media in this
            // unit — skip the rest of the actions for this row.
            continue;
        }

        pom2::SmartPortUnit* u = smartPortCard->unit(k);
        if (!u) continue;

        if (a.writeBackChanged) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            u->setWriteBackEnabled(a.writeBackOn);
            settings->setBool(base + "_writeback", a.writeBackOn);
            dirtySettings = true;
            tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                ": write-back " + (a.writeBackOn ? "ON" : "OFF");
            tapeStatusUntil = lastFrameTime + 3.0;
        }

        if (!a.mountPath.empty()) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (u->loadImage(a.mountPath)) {
                settings->setString(base + "_path", a.mountPath);
                dirtySettings = true;
                tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                    ": mounted " + a.mountPath;
            } else {
                tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                    ": mount failed: " + u->lastError();
            }
            tapeStatusUntil = lastFrameTime + 4.0;
        }

        if (a.eject) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            u->eject();
            settings->setString(base + "_path", "");
            dirtySettings = true;
            tapeStatusMessage = "SmartPort unit " + std::to_string(k) +
                ": ejected";
            tapeStatusUntil = lastFrameTime + 3.0;
        }
    }
    if (dirtySettings) settings->save();
}

void MainWindow::renderFloppyEmuWindow()
{
    if (!showFloppyEmu) return;
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
    // Live-plug a SmartPort (Liron-class) card into a free slot, mirroring
    // ensureHdvCardForBoot — the empty slot means no card destructor races
    // the worker, so a held lock suffices (no stop/start). Returns slot/-1.
    auto ensureSmartPort = [&]() -> int {
        if (smartPortCard) return smartPortCard->getSlot();
        SlotBus& bus = controller->memory().slotBus();
        int slot = (!bus.isPlugged(5)) ? 5 : -1;
        if (slot < 0)
            for (int s = 7; s >= 1; --s)
                if (!bus.isPlugged(s)) { slot = s; break; }
        if (slot < 0) return -1;
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        auto card = std::make_unique<pom2::SmartPortCard>(slot);
        card->setFloppySound(&controller->floppySound35());
        smartPortCard = card.get();
        controller->memory().slotBus().plug(slot, std::move(card));
        slotCards[slot] = "smartport35";
        pom2::log().info("FloppyEmu",
            "auto-plugged SmartPort card in slot " + std::to_string(slot));
        return slot;
    };
    auto controllerReady = [&](Mode m) -> bool {
        switch (m) {
            case Mode::Disk525:   return diskCard != nullptr;
            case Mode::Disk35:
            case Mode::Unidisk35: return smartPortCard != nullptr ||
                                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
            case Mode::SmartportHD: return hdvDevice() != nullptr ||
                                           smartPortCard != nullptr;
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
        switch (m) {
            case Mode::Disk525:
                return (diskCard && diskCard->isDiskLoaded())
                           ? baseName(diskCard->getDiskPath()) : std::string();
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return controller->disk35Internal().isLoaded()
                           ? baseName(controller->disk35Internal().path())
                           : std::string();
            case Mode::SmartportHD:
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    return dev->isImageLoaded() ? baseName(dev->getImagePath())
                                                : std::string();
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return std::string();
        }
        return std::string();
    };
    auto mountImage = [&](const std::string& path, Mode m) {
        std::string err;
        int bootSlot = 0;
        switch (m) {
            case Mode::Disk525:
                if (!diskCard) { floppyEmuStatus = controllerHint(m); break; }
                if (diskCard->insertDisk(path))
                    floppyEmuStatus = "Inserted " + baseName(path) +
                        " — reboot the Apple II to boot it.";
                else
                    floppyEmuStatus = "5.25 mount failed: " + baseName(path);
                break;
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (!controllerReady(m)) ensureSmartPort();
                floppyEmuStatus = routeMount35(0, path, err)
                    ? ("3.5\" mounted: " + baseName(path))
                    : ("3.5\" mount failed: " + err);
                break;
            case Mode::SmartportHD:
                if (!controllerReady(m)) ensureSmartPort();
                floppyEmuStatus = routeMountHdv(path, bootSlot, err)
                    ? ("Smartport mounted: " + baseName(path))
                    : ("Smartport mount failed: " + err);
                break;
        }
    };
    auto ejectCurrent = [&](Mode m) {
        switch (m) {
            case Mode::Disk525: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (diskCard) diskCard->ejectDisk();
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (smartPortCard) {
                    std::lock_guard<std::mutex> lk(controller->stateMutex());
                    if (pom2::SmartPortUnit* u = smartPortCard->unit(0)) u->eject();
                } else {
                    controller->eject35(0);  // re-locks the state mutex itself
                }
                break;
            case Mode::SmartportHD: {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (pom2::ProDOSBlockCard* dev = hdvDevice()) dev->ejectImage();
                else if (smartPortCard) {
                    if (pom2::SmartPortUnit* u = smartPortCard->unit(0)) u->eject();
                }
                break;
            }
        }
        floppyEmuStatus = "Ejected";
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

    const auto r = floppyEmuPanel->render("Floppy Emu (BMOW)", showFloppyEmu, snap);

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
            const int s = ensureSmartPort();
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
    if (!showHdvPanel) return;

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.imageLoaded       = hdvCard->isImageLoaded();
        snap.imagePath         = hdvCard->getImagePath();
        snap.blockCount        = hdvCard->getBlockCount();
        snap.writeBackEnabled  = hdvCard->isWriteBackEnabled();
        snap.hasUnsavedChanges = hdvCard->hasUnsavedChanges();
        snap.supportsWriteBack = hdvCard->canWriteBack();
        snap.isSynthVolume     = hdvCard->isSynthVolumeMounted();
    }

    // Library scan — hdv/ for .hdv and .2mg, sorted alphabetically so the
    // list stays stable across frames regardless of dirent order. Plus a
    // synthetic entry for prodos_disk/ if that folder exists (host-folder
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
            "prodos_disk", "../prodos_disk", "../../prodos_disk"
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
                  hdvCard ? hdvCard->getSlot() : 5);
    auto result = hdvPanel->render(hdvTitle, showHdvPanel, snap);

    if (result.writeBackToggleChanged && hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        hdvCard->setWriteBackEnabled(result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : "HDV: write-back disabled";
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestEject && hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        hdvCard->ejectImage();
        hdvStatus = "no image mounted";
        tapeStatusMessage = "HDV ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (result.requestBoot && hdvCard) {
        bootHdvImage();
    }
    if (!result.requestMountAndBoot.empty() && hdvCard) {
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
                ok = hdvCard->loadImageFromBytes(std::move(bytes),
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
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(path);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                err = hdvCard->getLastError();
                hdvStatus = "no image mounted";
            }
        }
        if (ok) {
            controller->bootFromSlot(hdvCard->getSlot());
            pom2::log().info("HDV",
                "slot " + std::to_string(hdvCard->getSlot()) +
                " library click → mount + boot: " + path);
            tapeStatusMessage = "Mounting + booting HDV (slot " +
                std::to_string(hdvCard->getSlot()) + "): " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (!result.requestMountOnly.empty() && hdvCard) {
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
            ok = hdvCard->loadImageFromBytes(std::move(bytes),
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
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(path);
            if (ok) {
                hdvPath   = path;
                hdvStatus = std::string("loaded: ") + path;
            } else {
                err = hdvCard->getLastError();
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
    for (size_t i = 0; i < diskPanels.size() && i < diskCards.size(); ++i) {
        if (diskPanels[i] && diskPanels[i]->insertDialogOpen) {
            triggeredPanel = diskPanels[i].get();
            triggeredCard  = diskCards[i];
            break;
        }
    }
    // Top-level "Insert disk image..." menu (no per-panel context) routes
    // to the primary card by convention.
    if (!triggeredPanel && diskPanel && diskPanel->insertDialogOpen) {
        triggeredPanel = diskPanel;
        triggeredCard  = diskCard;
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
    for (size_t i = 0; i < diskCards.size(); ++i) {
        if (diskCards[i] && diskCards[i]->getSlot() == diskDialogTargetSlot) {
            popupCard  = diskCards[i];
            popupPanel = (i < diskPanels.size()) ? diskPanels[i].get() : nullptr;
            break;
        }
    }
    if (!popupPanel) popupPanel = diskPanel;
    if (!popupCard)  popupCard  = diskCard;

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

    // Quick list of disk images in disks/ (mirrors the cassette dialog).
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks", "../disks", "../../disks" }) {
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
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (popupCard->insertDisk(popupPanel->dialogPath)) {
                tapeStatusMessage = "Disk inserted (slot " +
                    std::to_string(popupCard->getSlot()) + "): " +
                    popupPanel->dialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + popupCard->getLastError();
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
    const bool canMount = hdvCard && !hdvPanel->dialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (hdvCard->loadImage(hdvPanel->dialogPath)) {
            hdvPath   = hdvPanel->dialogPath;
            hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            tapeStatusMessage = "HDV mounted: " + hdvPanel->dialogPath;
        } else {
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV mount failed: " + hdvCard->getLastError();
        }
        tapeStatusUntil = lastFrameTime + 5.0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mount and Boot", ImVec2(160, 0))) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            ok = hdvCard->loadImage(hdvPanel->dialogPath);
            if (ok) {
                hdvPath   = hdvPanel->dialogPath;
                hdvStatus = std::string("loaded: ") + hdvPanel->dialogPath;
            } else {
                hdvStatus = "no image mounted";
                tapeStatusMessage = "HDV mount failed: " + hdvCard->getLastError();
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

void MainWindow::renderDisk35PanelWindow()
{
    if (!showDisk35Panel) return;

    pom2::Disk35Controller_ImGui::PanelSnapshot snap;
    // 3.5" is "supported" by the //c+ profile (on-board SmartPort + MIG)
    // OR by ANY profile where the user plugged a SmartPort 3.5" card
    // (option C 2026-05-15: //e + Liron-class card). Both paths share
    // the same Disk35Image objects so the panel doesn't have to care
    // which mux is talking.
    snap.supportedByProfile =
        (activeProfile == pom2::SystemProfile::AppleIIcPlus) ||
        (smartPortCard != nullptr);

    // Source selection: a SmartPort slot card on any NON-//c+ profile owns
    // its 3.5" media in its own per-unit `Disk35Image` (SmartPort35Unit) —
    // NOT the //c+ on-board hub's pair. The panel used to always read the
    // hub here, so on a //e + Liron-class card it showed two empty on-board
    // drives while the actual media sat in the card's units (and mount /
    // eject / write-back all hit the wrong object). Read + route from the
    // card's units when that's the source; keep the hub path for //c+.
    const bool useSmartPort35 =
        (smartPortCard != nullptr &&
         activeProfile != pom2::SystemProfile::AppleIIcPlus);
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        if (useSmartPort35) {
            for (int i = 0; i < 2; ++i) {
                auto& s = snap.drives[i];
                auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                    smartPortCard->unit(static_cast<size_t>(i)));
                if (!u) { s = {}; continue; }  // empty or non-3.5 (HDV) unit
                const pom2::Disk35Image& img = u->image();
                s.diskLoaded        = u->isLoaded();
                s.motorOn           = false;  // block-level card: no flux/motor line
                s.track             = 0;
                s.side1             = false;
                s.writeProtected    = u->isWriteProtected();
                s.diskPath          = u->path();
                s.lastError         = u->lastError();
                s.hasUnsavedChanges = img.hasUnsavedChanges();
                s.writeBackEnabled  = u->isWriteBackEnabled();
            }
        } else {
            const pom2::Sony35Drive* drives[2] = {
                &controller->sony35Internal(), &controller->sony35External(),
            };
            const pom2::Disk35Image* images[2] = {
                &controller->disk35Internal(),  &controller->disk35External(),
            };
            for (int i = 0; i < 2; ++i) {
                auto& s = snap.drives[i];
                s.diskLoaded        = drives[i]->isInserted();
                s.motorOn           = drives[i]->isMotorOn();
                s.track             = drives[i]->track();
                s.side1             = drives[i]->side1();
                s.writeProtected    = drives[i]->isWriteProtected();
                s.diskPath          = images[i]->path();
                s.lastError         = images[i]->lastError();
                s.hasUnsavedChanges = images[i]->hasUnsavedChanges();
                s.writeBackEnabled  = images[i]->isWriteBackEnabled();
            }
        }
    }

    // Library scan — mirrors the Disk II library scan but only picks
    // up files large enough to be 800K (size sniff via filesystem).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "disks35", "../disks35", "../../disks35",
                                 "disks",   "../disks",   "../../disks" }) {
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
                if (ext != ".po" && ext != ".2mg") continue;
                const auto sz = entry.file_size(ec);
                if (ec) continue;
                // 800K raw or 2IMG-wrapped (header + 819 200).
                if (sz != 819200 && sz != 819200 + 64 &&
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
    if (smartPortCard) {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (slot %d)", smartPortCard->getSlot());
    } else {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (//c+ on-board)");
    }
    auto result = disk35Panel->render(
        disk35Title, showDisk35Panel, snap);

    for (int d = 0; d < 2; ++d) {
        if (result.requestEject[d]) {
            if (useSmartPort35) {
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                        smartPortCard->unit(static_cast<size_t>(d)))) {
                    u->eject();
                    const std::string base =
                        "smartport_slot" +
                        std::to_string(smartPortCard->getSlot()) +
                        "_unit" + std::to_string(d);
                    settings->setString(base + "_path", "");
                    settings->save();
                }
            } else {
                controller->eject35(d);
            }
            tapeStatusMessage = std::string("3.5\" drive ") +
                (d == 0 ? "1 (internal)" : "2 (external)") + " ejected";
            tapeStatusUntil = lastFrameTime + 4.0;
        }
        // Per-drive write-back toggle. Apply under stateMutex so a save-
        // on-eject race against the worker can't half-flip the flag.
        if (result.requestWriteBackToggle[d]) {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            if (useSmartPort35) {
                if (auto* u = dynamic_cast<pom2::SmartPort35Unit*>(
                        smartPortCard->unit(static_cast<size_t>(d)))) {
                    u->setWriteBackEnabled(result.newWriteBack[d]);
                    const std::string base =
                        "smartport_slot" +
                        std::to_string(smartPortCard->getSlot()) +
                        "_unit" + std::to_string(d);
                    settings->setBool(base + "_writeback", result.newWriteBack[d]);
                    settings->save();
                }
            } else {
                pom2::Disk35Image& img = (d == 0)
                    ? controller->disk35Internal()
                    : controller->disk35External();
                img.setWriteBackEnabled(result.newWriteBack[d]);
            }
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
        if (disk35Panel->dialogPath.empty()) disk35Panel->dialogPath = "disks35/";
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
            if (useSmartPort35) {
                controller->bootFromSlot(smartPortCard->getSlot());
                tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted (slot " + std::to_string(smartPortCard->getSlot())
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
    for (const char* dir : { "disks35", "../disks35", "../../disks35" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".po" && ext != ".2mg") continue;
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

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("About POM2", &showAbout, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("POM2 v0.6");
        ImGui::Text("Apple II / II+ / //e / //c / //c+ emulator");
        ImGui::Text("MOS 6502 / 65C02 / Rockwell / WDC, Dear ImGui frontend");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped(
            "Hardware accuracy comes from verbatim ports of MAME's "
            "device models. Wherever POM2 emulates a chip or a "
            "peripheral, the implementation cites the MAME source "
            "file and line range it follows, and each behaviour is "
            "pinned by a smoke test under tests/.");
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
        ImGui::Spacing();
        if (ImGui::Button("Close")) showAbout = false;
    }
    ImGui::End();
}

void MainWindow::renderMemoryViewerWindow()
{
    if (!showMemViewer) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &showMemViewer)) {
        // Hold the state mutex briefly so the snapshot the viewer reads
        // (Memory::data()) is consistent — no torn writes mid-row.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        memViewer->setCmosMode(
            controller->cpu().getCpuMode() == M6502::CpuMode::CMOS);
        memViewer->render();
    }
    ImGui::End();
}

// ─── Cassette deck ───────────────────────────────────────────────────────

void MainWindow::renderCassetteDeckWindow(float deltaSeconds)
{
    if (!showCassetteDeck) return;

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
                                      showCassetteDeck,
                                      controller.get(),
                                      snap,
                                      deltaSeconds);

    if (!result.statusMessage.empty()) {
        tapeStatusMessage = std::move(result.statusMessage);
        tapeStatusUntil   = lastFrameTime + 4.0;  // show for 4 seconds
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

    // Status line near the bottom.
    if (!tapeStatusMessage.empty() && lastFrameTime < tapeStatusUntil) {
        ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 36),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("##TapeStatus", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
            ImGui::TextUnformatted(tapeStatusMessage.c_str());
        }
        ImGui::End();
    }
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

    pollJoystickAndPushToMemory();

    // Decide CPU turbo from disk activity every frame, independent of whether
    // any disk panel window is open (the disk panel defaults to hidden).
    updateAutoTurbo();

    // Kiosk: only the screen, no chrome. Joystick + auto-turbo above still
    // run so the machine behaves identically; everything else is skipped.
    if (kiosk_) {
        renderKiosk();
        return;
    }

    renderMenuBar();
    // Toolbar must render after the menu bar so we know its height
    // (`ImGui::GetFrameHeight()` reflects the menu bar font size +
    // padding). It's positioned just below — pinned, can't be moved
    // or resized.
    if (showToolbar) {
        pom2::Toolbar_ImGui::Snapshot tb;
        const auto mode = controller->getMode();
        tb.isRunning          = (mode == EmulationController::Mode::Running);
        tb.isStopped          = (mode == EmulationController::Mode::Stopped);
        tb.cyclesPerFrame     = controller->getCyclesPerFrame();
        tb.memViewerVisible   = showMemViewer;
        tb.activeProfile      = activeProfile;
        tb.hasPrimaryDiskCard = (diskCard != nullptr);
        tb.charRomLocale      = charRomLocale;
        auto isMonoHiRes = [](Apple2Display::HiResMode m) {
            return m == Apple2Display::HiResMode::MonoWhite ||
                   m == Apple2Display::HiResMode::MonoGreen ||
                   m == Apple2Display::HiResMode::MonoAmber;
        };
        tb.displayIsMono      = isMonoHiRes(display->getHiResMode());

        const auto tr = toolbar->render(showToolbar,
                                        ImGui::GetFrameHeight(), tb);
        if (tr.requestColdBoot)        controller->coldBoot();
        if (tr.requestSoftReset)       controller->softReset();
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
        if (tr.requestMemViewerToggle) showMemViewer = !showMemViewer;
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
            // picks it up next frame and routes to `diskCard`.
            diskPanel->insertDialogOpen = true;
            if (diskPanel->dialogPath.empty()) diskPanel->dialogPath = "disks/";
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
                std::lock_guard<std::mutex> lk(controller->stateMutex());
                if (controller->memory().loadCharRom(newPath.c_str())) {
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
    }
    renderScreenWindow();
    renderControlsWindow();
    renderMemoryViewerWindow();
    if (showMemoryBar)  renderMemoryBarWindow();
    if (showMemoryBarH) renderMemoryBarHorizontalWindow();
    if (showMemoryGrid) renderMemoryGridWindow();
    renderCassetteDeckWindow(deltaSeconds);
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
    renderChatMauvePanelWindow();
    renderMockingboardPanelWindow();
    renderSscPanelWindow();
    renderJoystickPanelWindow();
    renderAudioMixerWindow();
    renderAiControlPanelWindow();
    renderSlotConfigPanel();
    renderFloppyEmuWindow();
    renderAboutDialog();
}
