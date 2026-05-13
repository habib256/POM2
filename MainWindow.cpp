// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"
#include "CassetteDevice.h"
#include "Logger.h"
#include "ProDOSVolume.h"
#include "SpeakerDevice.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
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
    : memViewer(&controller.memory())
{
    // Memory viewer writes go through Memory::memWrite under stateMutex,
    // so a byte poked from the UI passes through ROM-write protection and
    // any future I/O hooks just like a CPU store would.
    memViewer.setWriteCallback([this](uint16_t a, uint8_t v) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.memory().memWrite(a, v);
    });

    // Load any persisted runtime config. Missing/malformed file → use
    // defaults; the fields below honour the saved values when present.
    settings.load();

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

    bool iiePresent = false;
    if (!forceIIPlus) {
        for (const char* p : iieRomCandidates) {
            if (fs::exists(p)) { romPath = p; iiePresent = true; break; }
        }
    }
    if (!iiePresent) {
        for (const char* p : romCandidates) {
            if (fs::exists(p)) { romPath = p; break; }
        }
    }
    charRomPath.clear();
    if (iiePresent) {
        for (const char* p : charRomIIeCandidates) {
            if (fs::exists(p)) { charRomPath = p; break; }
        }
    }
    if (charRomPath.empty()) {
        for (const char* p : charRomCandidates) {
            if (fs::exists(p)) { charRomPath = p; break; }
        }
    }

    if (iiePresent) {
        controller.memory().setIIEMode(true);
        display.setAuxMemory(controller.memory().auxData());
    }

    if (controller.memory().loadAppleIIRom(romPath.c_str())) {
        romStatus = std::string(iiePresent ? "IIe (128K): " : "loaded: ") + romPath;
    } else {
        romStatus = std::string("NO ROM (") + romPath +
                    ") — only $D000-$FFFF stub is active";
    }
    controller.memory().loadCharRom(charRomPath.c_str());

    // Plug all expansion cards in their user-configured slots. The
    // mapping is read from `slot_1_card`..`slot_7_card` settings; absent
    // keys fall back to the legacy defaults (DiskII=6, HDV=5, SSC=2,
    // Clock=4, ChatMauve=7) so first-run users see no regression.
    plugSlotsFromSettings();

    // ── Restore display + UI prefs from previous session ─────────────
    {
        const std::string mode = settings.getString("hi_res_mode", "");
        if      (mode == "ColorNTSC")    display.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        else if (mode == "ChatMauveRGB") display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        else if (mode == "MonoWhite")    display.setHiResMode(Apple2Display::HiResMode::MonoWhite);
        else if (mode == "MonoGreen")    display.setHiResMode(Apple2Display::HiResMode::MonoGreen);
        else if (mode == "MonoAmber")    display.setHiResMode(Apple2Display::HiResMode::MonoAmber);

        pixelScale         = settings.getFloat("pixel_scale",     pixelScale);
        showDiskPanel      = settings.getBool ("show_disk_panel", showDiskPanel);
        showHdvPanel       = settings.getBool ("show_hdv_panel",  showHdvPanel);
        showCassetteDeck   = settings.getBool ("show_cassette",   showCassetteDeck);
        showJoystickPanel  = settings.getBool ("show_joystick",   showJoystickPanel);
        showChatMauvePanel = settings.getBool ("show_chatmauve",  showChatMauvePanel);
        showSscPanel       = settings.getBool ("show_ssc",        showSscPanel);
        sscPortInput       = settings.getInt  ("ssc_port",        sscPortInput);
        diskTurboWhileMotor = settings.getBool("disk_turbo",      diskTurboWhileMotor);
    }

    // ── Restore Disk II state ─────────────────────────────────────────
    if (diskCard) {
        const bool wb = settings.getBool("disk_writeback", false);
        diskCard->setWriteBackEnabled(wb);

        const std::string diskPath = settings.getString("disk_path", "");
        std::error_code ec;
        if (!diskPath.empty() && fs::is_regular_file(diskPath, ec)) {
            if (diskCard->insertDisk(diskPath)) {
                pom2::log().info("Disk II", "Re-inserted from settings: " + diskPath);
            }
        }
    }

    // ── Restore audio levels ─────────────────────────────────────────
    {
        const float spkVol = settings.getFloat("speaker_volume", 1.0f);
        controller.speaker().setVolume(spkVol);
        controller.speaker().setMuted(settings.getBool("speaker_muted", false));
        controller.setCassetteVolume(settings.getFloat("cassette_volume", 0.6f));
    }

    // Always wake up at the Applesoft prompt. A default HDV / disk may be
    // mounted (above), but we never auto-boot — the user picks via the
    // Disk II / HDV panel libraries. Use coldBoot (not just a CPU reset)
    // so the Apple II Monitor runs its full cold-start sequence: HOME
    // clears the freshly-zeroed text page so the user briefly sees the
    // "Apple //e" banner instead of the `@`-tile garbage that the text
    // page renders when full of $00, then it tries slot 6, fails (no
    // disk in drive at first launch), and falls through to AppleSoft.
    controller.coldBoot();
    controller.setMode(EmulationController::Mode::Running);
    controller.start();

    // Determine the active profile from what the legacy boot path
    // resolved. If a `system_profile` setting was persisted from a
    // previous launch AND it disagrees with the auto-detected one, the
    // user explicitly picked that profile last time — honour it via a
    // full cold reset via applyProfile() (which the menu also calls).
    activeProfile = iiePresent ? pom2::SystemProfile::AppleIIe
                               : pom2::SystemProfile::AppleIIPlus;
    const std::string savedProfile = settings.getString("system_profile", "");
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
            if (resolved != controller.cpu().getCpuMode()) {
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                controller.cpu().setCpuMode(resolved);
            }
        }
    }
}

MainWindow::~MainWindow()
{
    controller.stop();

    // Persist the current state so the next launch restores the same
    // mounted disks, video mode, panels, and audio levels.
    if (hdvCard && hdvCard->isImageLoaded()) {
        // Don't persist the synthesised host-folder volume — the path is
        // a sentinel, not a real file. Re-synthesis happens on click.
        const std::string& p = hdvCard->getImagePath();
        if (p.rfind("[host folder] ", 0) == std::string::npos) {
            settings.setString("hdv_path", p);
        } else {
            settings.setString("hdv_path", "");
        }
    } else {
        settings.setString("hdv_path", "");
    }

    if (diskCard) {
        settings.setString("disk_path",
            diskCard->isDiskLoaded() ? diskCard->getDiskPath() : std::string());
        settings.setBool("disk_writeback", diskCard->isWriteBackEnabled());
    }

    if (hdvCard) {
        settings.setBool("hdv_writeback", hdvCard->isWriteBackEnabled());
    }

    if (sscCard) {
        settings.setBool("ssc_listening", sscCard->isListening());
        settings.setInt ("ssc_port",      sscCard->getPort());
    }

    // Persist the per-slot card mapping so changes via the Slot
    // Configuration panel survive a restart.
    for (int s = 1; s <= 7; ++s) {
        settings.setString("slot_" + std::to_string(s) + "_card", slotCards[s]);
    }

    auto modeName = [](Apple2Display::HiResMode m) -> const char* {
        switch (m) {
            case Apple2Display::HiResMode::ColorNTSC:    return "ColorNTSC";
            case Apple2Display::HiResMode::ChatMauveRGB: return "ChatMauveRGB";
            case Apple2Display::HiResMode::MonoWhite:    return "MonoWhite";
            case Apple2Display::HiResMode::MonoGreen:    return "MonoGreen";
            case Apple2Display::HiResMode::MonoAmber:    return "MonoAmber";
        }
        return "ColorNTSC";
    };
    settings.setString("hi_res_mode", modeName(display.getHiResMode()));
    settings.setFloat ("pixel_scale", pixelScale);
    settings.setBool  ("show_disk_panel", showDiskPanel);
    settings.setBool  ("show_hdv_panel",  showHdvPanel);
    settings.setBool  ("show_cassette",   showCassetteDeck);
    settings.setBool  ("show_joystick",   showJoystickPanel);
    settings.setBool  ("show_chatmauve",  showChatMauvePanel);
    settings.setBool  ("show_ssc",        showSscPanel);
    settings.setBool  ("disk_turbo",      diskTurboWhileMotor);
    settings.setFloat ("speaker_volume",  controller.speaker().getVolume());
    settings.setBool  ("speaker_muted",   controller.speaker().isMuted());
    settings.setFloat ("cassette_volume", controller.cassette().getVolume());
    if (mockingboardCard) {
        settings.setFloat("mockingboard_volume", mockingboardCard->getVolume());
        settings.setBool ("mockingboard_muted",  mockingboardCard->isMuted());
    }

    settings.save();
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
        slotCards[s] = settings.getString(key, kDefaults[s]);
    }
    if (!settings.getBool("clock_card_enable", true)) {
        // Legacy opt-out: only respect when no explicit slot_N_card key
        // was set (settings.getString returned the default for slot 4).
        if (settings.getString("slot_4_card", "__missing__") == "__missing__"
            && slotCards[4] == "clock") {
            slotCards[4] = "";
        }
    }

    // Validate uniqueness — each card type plugs into at most one slot.
    auto firstOccurrence = [&](const std::string& type) -> int {
        for (int s = 1; s <= 7; ++s) if (slotCards[s] == type) return s;
        return -1;
    };
    for (int s = 1; s <= 7; ++s) {
        if (slotCards[s].empty()) continue;
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
            if (fs::exists(p)) { diskRomPath = p; break; }
        }
        auto card = std::make_unique<DiskIICard>(s);
        if (!card->loadBootRom(diskRomPath)) {
            diskRomStatus = std::string("NO Disk II PROM (") + diskRomPath + ")";
            return;     // no boot PROM → card stays unplugged
        }
        diskRomStatus = std::string("loaded: ") + diskRomPath;
        // Optional: load the P6 LSS sequencer PROM (Apple part 341-0028-A)
        // for the cycle-accurate bit-level controller path. Bundled at
        // `roms/diskii_p6.rom`; falls back to the embedded default.
        static const char* lssRomCandidates[] = {
            "roms/diskii_p6.rom", "../roms/diskii_p6.rom",
            "../../roms/diskii_p6.rom"
        };
        for (const char* p : lssRomCandidates) {
            if (fs::exists(p)) { (void)card->loadLssRom(p); break; }
        }
        // Wire the CPU pointer for sub-instruction cycle accuracy on
        // MMIO reads/writes (cycle-precise copy protections rely on the
        // LSS state at the exact sub-cycle of the data fetch, not at
        // instruction-start). See DiskIICard::setCpu doc for context.
        card->setCpu(&controller.cpu());
        diskCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
    };

    auto plugHdv = [&](int s) {
        // Pick an image to mount: the path saved in the previous session
        // first; otherwise the first .hdv/.2mg under hdv/ alphabetically.
        const std::string saved = settings.getString("hdv_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) {
            hdvPath = saved;
        } else {
            static const char* hdvDirs[] = { "hdv", "../hdv", "../../hdv" };
            for (const char* dir : hdvDirs) {
                if (!fs::is_directory(dir, ec)) continue;
                std::vector<std::string> found;
                for (const auto& entry : fs::directory_iterator(dir, ec)) {
                    if (!entry.is_regular_file()) continue;
                    const std::string ext = entry.path().extension().string();
                    if (ext != ".hdv" && ext != ".2mg") continue;
                    found.push_back(entry.path().string());
                }
                std::sort(found.begin(), found.end());
                if (!found.empty()) { hdvPath = found.front(); break; }
            }
        }

        auto card = std::make_unique<ProDOSHardDiskCard>(s);
        if (!hdvPath.empty() && card->loadImage(hdvPath)) {
            hdvStatus = std::string("loaded: ") + hdvPath;
        } else {
            hdvStatus = "no image mounted";
        }
        card->setWriteBackEnabled(settings.getBool("hdv_writeback", false));
        hdvCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
    };

    auto plugChatMauve = [&](int s) {
        auto card = std::make_unique<LeChatMauveCard>(s);
        chatMauveCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
        display.setChatMauveCard(chatMauveCard);
    };

    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        sscCard = card.get();
        // Use pasteText (not queueKey) — pasteText respects the paste
        // queue, so a stream of bytes from telnet doesn't clobber earlier
        // characters that BASIC hasn't picked up yet.
        sscCard->setKeyboardSink(
            [&mem = controller.memory()](uint8_t b) {
                const char buf[1] = { static_cast<char>(b) };
                mem.pasteText(buf, 1);
            });
        // Wire the slot IRQ line so any RDRF transition with RX-IRQ-enable
        // on raises the CPU's IRQ. Same pattern as Mockingboard above.
        sscCard->setCpuIrqLine(&controller.cpu());
        controller.memory().slotBus().plug(s, std::move(card));
        if (settings.getBool("ssc_listening", false)) {
            const int p = settings.getInt("ssc_port",
                                          SuperSerialCard::kDefaultPort);
            sscCard->startListening(static_cast<uint16_t>(p));
        }
    };

    auto plugClock = [&](int s) {
        auto card = std::make_unique<ClockCard>(s);
        clockCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
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
        card->setSampleRate(controller.audio().getActualSampleRate());
        card->setCpuIrqLine(&controller.cpu());
        // Default volume is conservative — the card's three-channel mix
        // can dwarf the speaker at peak; the user can crank via the
        // Mockingboard panel (TODO).
        card->setVolume(settings.getFloat("mockingboard_volume", 0.5f));
        card->setMuted(settings.getBool ("mockingboard_muted",  false));
        if (controller.audio().isAvailable()) {
            controller.audio().addSource(card->audioSource());
        }
        mockingboardCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
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
            if (fs::exists(p)) { slotRomPath = p; break; }
        }
        for (const char* p : mcuRomCandidates) {
            if (fs::exists(p)) { mcuRomPath = p; break; }
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
        // Wire the slot IRQ line. The card asserts via
        // `M6502::setIrqLine(slot, asserted)` — a wire-OR aggregator
        // keyed by slot, so other cards' IRQs stay live even if this
        // card releases. See M6502::setIrqLine() doc.
        card->setCpuIrqLine(&controller.cpu());
        mouseRomStatus = "loaded: " + slotRomPath + " + " + mcuRomPath;
        mouseCard = card.get();
        controller.memory().slotBus().plug(s, std::move(card));
    };

    // Dispatch: walk slots 1..7 and plug whichever card the settings ask
    // for. Anything we don't recognise is logged and skipped.
    for (int s = 1; s <= 7; ++s) {
        const std::string& kind = slotCards[s];
        if      (kind.empty())          continue;
        else if (kind == "diskii")      plugDiskII(s);
        else if (kind == "hdv")         plugHdv(s);
        else if (kind == "ssc")         plugSsc(s);
        else if (kind == "clock")       plugClock(s);
        else if (kind == "chatmauve")   plugChatMauve(s);
        else if (kind == "mouse")       plugMouse(s);
        else if (kind == "mockingboard") plugMockingboard(s);
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
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        w = display.width();
        h = display.height();
        const uint32_t* src = display.pixels();
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
    controller.memory().queueKey(apple2Code);
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
        case GLFW_KEY_ENTER:        injectAscii(0x0D); break;
        case GLFW_KEY_BACKSPACE:    injectAscii(0x08); break;
        case GLFW_KEY_LEFT:         injectAscii(0x08); break;
        case GLFW_KEY_RIGHT:        injectAscii(0x15); break;
        case GLFW_KEY_UP:           injectAscii(0x0B); break;
        case GLFW_KEY_DOWN:         injectAscii(0x0A); break;
        case GLFW_KEY_ESCAPE:       injectAscii(0x1B); break;
        case GLFW_KEY_TAB:          injectAscii(0x09); break;
        case GLFW_KEY_F9:           saveScreenshot(); break;
        case GLFW_KEY_F11:          controller.softReset(); break;
        case GLFW_KEY_F12:          controller.hardReset(); break;
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
    const size_t queued = controller.memory().pasteText(text);
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
    const size_t queued = controller.memory().pasteText(text);
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
        // dimensions are set after the first display.render() below.
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
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        display.render(controller.memory());
    }

    const int w = display.width();
    const int h = display.height();
    glBindTexture(GL_TEXTURE_2D, screenTexture);
    if (w != screenTextureWidth || h != screenTextureHeight) {
        // 80-col toggled — reallocate. glTexImage2D releases the previous
        // storage, so we don't leak GL memory across mode switches.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, display.pixels());
        screenTextureWidth  = w;
        screenTextureHeight = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, display.pixels());
    }
}

// ─── Render passes ───────────────────────────────────────────────────────

void MainWindow::renderMenuBar()
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Reload ROM")) {
            bool ok = false;
            std::string err;
            {
                // Must hold the emulation lock: loadAppleIIRom rewrites
                // $D000-$FFFF and can race with the CPU thread otherwise.
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                ok = controller.memory().loadAppleIIRom(romPath.c_str());
                if (ok) {
                    controller.hardReset();
                } else {
                    err = controller.memory().getLastError();
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
        const size_t pending = controller.memory().pendingPasteSize();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::MenuItem("Cancel pending paste")) {
            controller.memory().cancelPaste();
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
        const auto m = controller.getMode();
        if (ImGui::MenuItem("Run", nullptr, m == EmulationController::Mode::Running)) {
            controller.setMode(EmulationController::Mode::Running);
        }
        if (ImGui::MenuItem("Pause", nullptr, m == EmulationController::Mode::Stopped)) {
            controller.setMode(EmulationController::Mode::Stopped);
        }
        if (ImGui::MenuItem("Step (one instr)")) controller.requestStep();
        ImGui::Separator();
        if (ImGui::MenuItem("Reset (Ctrl-Reset)",     "F11")) controller.softReset();
        if (ImGui::MenuItem("Hard reset",             "F12")) controller.hardReset();
        if (ImGui::MenuItem("Cold boot (wipe RAM)"))          controller.coldBoot();
        ImGui::Separator();
        // CPU type selector. Three settings:
        //   * Auto (profile default) — NMOS for II/II+, CMOS for IIe/IIc/IIc+
        //   * NMOS 6502 — force NMOS regardless of profile (e.g. test
        //     IIe NMOS-unenhanced behaviour)
        //   * 65C02 — force CMOS (e.g. run NMOS-era software on 65C02)
        // Persisted to settings as `cpu_mode_override` so the choice
        // survives a relaunch. A profile switch re-applies the override.
        const auto curCpu = controller.cpu().getCpuMode();
        const std::string curOverride = settings.getString("cpu_mode_override", "auto");
        if (ImGui::BeginMenu("CPU")) {
            const auto& cfg = pom2::profileConfig(activeProfile);
            const char* profileLabel =
                (cfg.defaultCpu == M6502::CpuMode::CMOS) ? "65C02" : "NMOS 6502";
            char autoLabel[64];
            std::snprintf(autoLabel, sizeof(autoLabel),
                "Auto (profile default: %s)", profileLabel);
            if (ImGui::MenuItem(autoLabel, nullptr, curOverride == "auto")) {
                settings.setString("cpu_mode_override", "auto");
                settings.save();
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                controller.cpu().setCpuMode(cfg.defaultCpu);
            }
            if (ImGui::MenuItem("NMOS 6502", nullptr,
                                curOverride == "nmos" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::NMOS
                                 && curOverride != "65c02"))) {
                settings.setString("cpu_mode_override", "nmos");
                settings.save();
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                controller.cpu().setCpuMode(M6502::CpuMode::NMOS);
            }
            if (ImGui::MenuItem("65C02 (CMOS)", nullptr,
                                curOverride == "65c02" ||
                                (curOverride == "auto" && curCpu == M6502::CpuMode::CMOS
                                 && curOverride != "nmos"))) {
                settings.setString("cpu_mode_override", "65c02");
                settings.save();
                std::lock_guard<std::mutex> lk(controller.stateMutex());
                controller.cpu().setCpuMode(M6502::CpuMode::CMOS);
            }
            ImGui::Separator();
            ImGui::TextDisabled("NMOS = original 1975. Disables");
            ImGui::TextDisabled("STZ/BRA/PHX/etc. and SMB/RMB/");
            ImGui::TextDisabled("BBR/BBS extensions.");
            ImGui::TextDisabled("Override persists across profile");
            ImGui::TextDisabled("switches.");
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Presets")) {
        // 4 canonical Apple II profiles. Selecting one triggers a
        // full cold-reset via `applyProfile()`: new ROM, new char ROM,
        // RAM wiped, slot cards re-plugged, CPU type reset to the
        // profile default (overridable in Debug → CPU). Disk and HDV
        // mounts persist across the switch so the user can test the
        // same software stack under different models.
        for (pom2::SystemProfile p : pom2::allProfiles()) {
            const auto& cfg = pom2::profileConfig(p);
            const bool selected = (activeProfile == p);
            // Append " (current)" to the label when active so the
            // checkmark is reinforced by text — useful in colourblind
            // / monochrome ImGui themes.
            std::string label = std::string(cfg.displayName);
            if (selected) label += "  ✓";
            if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                applyProfile(p);
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Profile = full cold reset.");
        ImGui::TextDisabled("Mounted disks survive the switch.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Display")) {
        ImGui::SliderFloat("Pixel scale", &pixelScale, 1.0f, 4.0f, "%.1fx");

        ImGui::Separator();
        ImGui::TextDisabled("Hi-res mode");
        const Apple2Display::HiResMode cur = display.getHiResMode();
        if (ImGui::MenuItem("Color NTSC", nullptr,
                            cur == Apple2Display::HiResMode::ColorNTSC))
            display.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        // Le Chat Mauve RGB — clean Péritel decode, two distinct grays,
        // no inter-byte fringing. Greyed out if the slot-7 card isn't
        // plugged (the Apple II would just see composite video).
        ImGui::BeginDisabled(chatMauveCard == nullptr);
        if (ImGui::MenuItem("Le Chat Mauve (RGB)", nullptr,
                            cur == Apple2Display::HiResMode::ChatMauveRGB))
            display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        ImGui::EndDisabled();
        if (ImGui::MenuItem("Mono White",  nullptr,
                            cur == Apple2Display::HiResMode::MonoWhite))
            display.setHiResMode(Apple2Display::HiResMode::MonoWhite);
        if (ImGui::MenuItem("Mono Green (P31)", nullptr,
                            cur == Apple2Display::HiResMode::MonoGreen))
            display.setHiResMode(Apple2Display::HiResMode::MonoGreen);
        if (ImGui::MenuItem("Mono Amber",  nullptr,
                            cur == Apple2Display::HiResMode::MonoAmber))
            display.setHiResMode(Apple2Display::HiResMode::MonoAmber);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Hardware")) {
        ImGui::MenuItem("Cassette deck", nullptr, &showCassetteDeck);
        ImGui::MenuItem("Disk II (slot 6)", nullptr, &showDiskPanel);
        ImGui::MenuItem("HDV (slot 5)", nullptr, &showHdvPanel);
        ImGui::MenuItem("Le Chat Mauve (slot 7)", nullptr, &showChatMauvePanel);
        ImGui::MenuItem("Super Serial (slot 2)", nullptr, &showSscPanel);
        ImGui::MenuItem("Joystick", nullptr, &showJoystickPanel);
        ImGui::MenuItem("Slot Configuration...", nullptr, &showSlotConfigPanel);
        ImGui::Separator();
        ImGui::BeginDisabled(hdvCard == nullptr);
        if (ImGui::MenuItem("Mount HDV image (.hdv / .2mg)...")) {
            showHdvMountDialog = true;
            if (hdvDialogPath.empty()) hdvDialogPath = "hdv/";
        }
        if (ImGui::MenuItem("Eject HDV", nullptr, false,
                            hdvCard && hdvCard->isImageLoaded())) {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            hdvCard->ejectImage();
            hdvStatus = "no image mounted";
            tapeStatusMessage = "HDV ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!hdvCard || !hdvCard->isImageLoaded());
        if (ImGui::MenuItem("Boot HDV (slot 5)")) {
            bootHdvImage();
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(diskCard == nullptr);
        if (ImGui::MenuItem("Insert disk image (.dsk / .do / .po / .nib / .woz)...")) {
            showDiskInsertDialog = true;
            if (diskDialogPath.empty()) diskDialogPath = "disks/";
        }
        if (ImGui::MenuItem("Eject disk", nullptr, false,
                            diskCard && diskCard->isDiskLoaded())) {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            diskCard->ejectDisk();
            tapeStatusMessage = "Disk ejected";
            tapeStatusUntil   = lastFrameTime + 3.0;
        }
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Debug")) {
        ImGui::MenuItem("Emulation panel", nullptr, &showEmulationPanel);
        ImGui::MenuItem("Memory viewer",   nullptr, &showMemViewer);
        ImGui::Separator();
        ImGui::MenuItem("Memory Map Bar",              nullptr, &showMemoryBar);
        ImGui::MenuItem("Memory Map Bar (Horizontal)", nullptr, &showMemoryBarH);
        ImGui::MenuItem("Memory Map Grid",             nullptr, &showMemoryGrid);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About POM2")) showAbout = true;
        ImGui::EndMenu();
    }

    // Status pill on the right.
    {
        const char* modeStr = "?";
        switch (controller.getMode()) {
            case EmulationController::Mode::Running: modeStr = "RUN";  break;
            case EmulationController::Mode::Stopped: modeStr = "STOP"; break;
            case EmulationController::Mode::Step:    modeStr = "STEP"; break;
        }
        const auto state = controller.memory().getDisplayState();
        const char* gfx = state.textMode ? "TEXT"
                        : state.hiRes    ? (state.mixedMode ? "HGR+TXT" : "HGR")
                                         : (state.mixedMode ? "LGR+TXT" : "LGR");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  %s | %s | %s", modeStr, gfx, romStatus.c_str());
        ImGui::TextDisabled("%s", buf);
    }
    ImGui::EndMainMenuBar();
}

void MainWindow::renderScreenWindow()
{
    // Default startup layout (POM2 disables imgui.ini so every launch
    // re-applies these). Apple II Screen anchors the top-left ~60 %,
    // HDV stacks below it, Disk II owns the full-height right column.
    // FirstUseEver lets the user drag/resize freely after the first
    // frame — but the chosen positions don't persist across launches.
    ImGui::SetNextWindowPos (ImVec2(5,    30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 565), ImGuiCond_FirstUseEver);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    if (ImGui::Begin("Apple II Screen")) {
        uploadScreenTexture();

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
    ImGui::End();
    ImGui::PopStyleColor();
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
    const double dx = x - lastMouseHostX;
    const double dy = y - lastMouseHostY;
    lastMouseHostX = x;
    lastMouseHostY = y;

    if (!mouseCard) return;

    // Only route to the Apple II Mouse Card when the cursor is INSIDE
    // the screen widget rect. Outside, leave the host mouse free for
    // ImGui interaction.
    if (x < screenRectMin.x || x > screenRectMax.x ||
        y < screenRectMin.y || y > screenRectMax.y) {
        return;
    }

    // Accumulate host-pixel deltas into the 8-bit running counter the
    // MCU firmware reads as IPT_MOUSE_X/Y. Wraps freely — the MCU's
    // 8-bit subtraction handles deltas across the wrap boundary.
    mouseAppleX = static_cast<uint8_t>(mouseAppleX + static_cast<int>(dx));
    mouseAppleY = static_cast<uint8_t>(mouseAppleY + static_cast<int>(dy));
    mouseCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
}

void MainWindow::onMouseButton(int button, int action)
{
    // Only the primary button is wired to the Apple Mouse Card (PB7 of
    // the MCU). GLFW button 0 = left.
    if (button != 0) return;
    mouseButtonHeld = (action != 0);     // GLFW_RELEASE = 0, others = press/repeat
    if (mouseCard) {
        mouseCard->setHostMouse(mouseAppleX, mouseAppleY, mouseButtonHeld);
    }
}

void MainWindow::renderControlsWindow()
{
    if (!showEmulationPanel) return;
    ImGui::SetNextWindowPos (ImVec2(1095, 780), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  210), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Emulation", &showEmulationPanel)) {
        // CPU speed control.
        int cycPerFrame = controller.getCyclesPerFrame();
        const int oneX  = 17045;
        const int twoX  = 34091;
        const int maxX  = 1'000'000;
        ImGui::Text("Speed: %d cycles/frame (~%.2f MHz)",
                    cycPerFrame, cycPerFrame * 60.0 / 1e6);
        if (ImGui::Button("1x"))  {
            controller.setCyclesPerFrame(oneX);
            if (diskTurboActive) diskSavedCyclesPerFrame = oneX;
        }
        ImGui::SameLine();
        if (ImGui::Button("2x"))  {
            controller.setCyclesPerFrame(twoX);
            if (diskTurboActive) diskSavedCyclesPerFrame = twoX;
        }
        ImGui::SameLine();
        if (ImGui::Button("MAX")) {
            controller.setCyclesPerFrame(maxX);
            if (diskTurboActive) diskSavedCyclesPerFrame = maxX;
        }

        ImGui::Separator();
        ImGui::Text("PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X",
            controller.cpu().getProgramCounter(),
            controller.cpu().getAccumulator(),
            controller.cpu().getXRegister(),
            controller.cpu().getYRegister(),
            controller.cpu().getStackPointer());
        ImGui::Text("Cycles: %llu",
            (unsigned long long)controller.memory().getCycleCounter());
        ImGui::Text("Speaker toggles: %llu",
            (unsigned long long)controller.memory().getSpeakerToggleCount());

        // Speaker mix controls — directly bound to SpeakerDevice atomics.
        SpeakerDevice& spk = controller.speaker();
        float spkVol = spk.getVolume();
        if (ImGui::SliderFloat("Speaker vol", &spkVol, 0.0f, 2.0f, "%.2f"))
            spk.setVolume(spkVol);
        bool spkMute = spk.isMuted();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mute##spk", &spkMute)) spk.setMuted(spkMute);

        ImGui::Separator();
        const size_t pendingPaste = controller.memory().pendingPasteSize();
        if (pendingPaste > 0) {
            ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.15f, 1.0f),
                               "Paste in flight: %zu chars", pendingPaste);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##paste")) {
                controller.memory().cancelPaste();
            }
        }
        ImGui::TextWrapped("ROM: %s", romStatus.c_str());
        if (!hdvStatus.empty()) ImGui::TextWrapped("HDV: %s", hdvStatus.c_str());
    }
    ImGui::End();
}

void MainWindow::bootHdvImage()
{
    if (!hdvCard || !hdvCard->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    const std::string p = hdvCard->getImagePath();
    controller.bootFromSlot(5);
    tapeStatusMessage = "Booting HDV: " + p;
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
    joystick.poll();
    joystick.autoBindIfUnconfigured();

    // Apple II paddles (4) and push buttons (3). The Memory side already
    // handles the $C064-$C067 RC discharge model and $C061-$C063 push
    // buttons; we just hand it fresh values once per frame. No need to
    // hold stateMutex — these setters write atomic-friendly scalars.
    Memory& mem = controller.memory();
    for (int i = 0; i < 4; ++i)  mem.setPaddle(i, joystick.paddleValue(i));
    for (int i = 0; i < 3; ++i)  mem.setPaddleButton(i, joystick.buttonDown(i));
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

void MainWindow::renderJoystickPanelWindow()
{
    if (!showJoystickPanel) return;

    pom2::JoystickPanel_ImGui::Snapshot snap;
    for (int h = 0; h < JoystickInput::kHostCount; ++h) {
        const auto& d = joystick.deviceState(h);
        if (!d.present) continue;
        pom2::JoystickPanel_ImGui::HostDevice hd;
        hd.index   = h;
        hd.name    = d.name;
        hd.axis    = d.axis;
        hd.buttons = d.buttons;
        snap.hosts.push_back(std::move(hd));
    }
    const auto& cf = joystick.binding();
    snap.hostIdx  = cf.hostIdx;
    snap.deadzone = cf.deadzone;
    snap.invert   = cf.invert;
    for (int i = 0; i < 4; ++i) snap.appleIIPaddle[i] = joystick.paddleValue(i);
    for (int i = 0; i < 3; ++i) snap.appleIIButton[i] = joystick.buttonDown(i);

    auto result = joystickPanel.render("Joystick", showJoystickPanel, snap);
    if (result.changed) {
        auto& bind = joystick.binding();
        bind.hostIdx  = result.hostIdx;
        bind.deadzone = result.deadzone;
        bind.invert   = result.invert;
    }
}

// ─── Le Chat Mauve (slot 7) ──────────────────────────────────────────────

void MainWindow::renderChatMauvePanelWindow()
{
    if (!showChatMauvePanel) return;

    pom2::LeChatMauve_ImGui::Snapshot snap;
    if (chatMauveCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        snap.plugged   = true;
        snap.mode      = chatMauveCard->currentMode();
        snap.fifoBits  = chatMauveCard->fifoBits();
        snap.eightyCol = chatMauveCard->eightyCol();
        snap.an3High   = chatMauveCard->an3High();
    }

    ImGui::SetNextWindowPos (ImVec2(1095, 45),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330,  500), ImGuiCond_FirstUseEver);

    auto result = chatMauvePanel.render("Le Chat Mauve (slot 7)",
                                        showChatMauvePanel, snap);

    if (chatMauveCard && result.requestOverride) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        chatMauveCard->overrideMode(result.overrideTo);
    }
    if (chatMauveCard && result.requestReset) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        chatMauveCard->onReset();
    }
}

// ─── Disk II ─────────────────────────────────────────────────────────────

void MainWindow::renderDiskPanelWindow()
{
    if (!showDiskPanel) return;

    pom2::DiskController_ImGui::DriveSnapshot snap;
    if (diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        snap.bootRomLoaded     = diskCard->hasBootRom();
        snap.diskLoaded        = diskCard->isDiskLoaded();
        snap.motorOn           = diskCard->isMotorOn();
        snap.track             = diskCard->getCurrentTrack();
        snap.halfTrack         = diskCard->getHalfTrack();
        snap.trackPos          = diskCard->getTrackPosition();
        snap.diskPath          = diskCard->getDiskPath();
        snap.writeBackEnabled  = diskCard->isWriteBackEnabled();
        snap.hasUnsavedChanges = diskCard->hasUnsavedChanges();
    }
    snap.turboWhileMotor = diskTurboWhileMotor;
    snap.turboActive     = diskTurboActive;

    // Disk library — scan disks/ for .dsk, .do and .po files. Cheap (a
    // few dirent reads per frame), but sorted alphabetically so the list
    // doesn't reshuffle each time the OS hands us a different order.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks", "../disks", "../../disks" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                    ext != ".nib" && ext != ".woz") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = entry.path().filename().string();
                e.fullPath    = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // Auto-turbo: while the motor is spinning, replace the user's chosen
    // cyclesPerFrame with a "MAX" value (~60 MHz emulated) so the LSS's
    // nibble pacing is multiplied by the same factor — the disk reads
    // proceed at ~1 second of wall time for a full DOS 3.3 boot. When the
    // motor stops (DOS finished loading) we restore the prior setting so
    // typed-in BASIC programs run at the natural Apple II clock.
    if (diskCard && diskTurboWhileMotor) {
        const bool wantTurbo = snap.motorOn;
        if (wantTurbo && !diskTurboActive) {
            diskSavedCyclesPerFrame = controller.getCyclesPerFrame();
            controller.setCyclesPerFrame(1'000'000);
            diskTurboActive = true;
        } else if (!wantTurbo && diskTurboActive) {
            controller.setCyclesPerFrame(diskSavedCyclesPerFrame);
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        // Toggle was switched off mid-spin — drop back to saved speed.
        controller.setCyclesPerFrame(diskSavedCyclesPerFrame);
        diskTurboActive = false;
    }

    // Disk II = full-height right column in the curated layout. Tall
    // enough to show the controls AND the library list without scrolling.
    ImGui::SetNextWindowPos (ImVec2(1055, 30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(705,  960), ImGuiCond_FirstUseEver);
    auto result = diskPanel.render("Disk II (slot 6)", showDiskPanel, snap);
    if (result.turboToggleChanged) {
        diskTurboWhileMotor = result.turboNewValue;
    }
    if (result.writeBackToggleChanged && diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard->setWriteBackEnabled(result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "Disk II: write-back ENABLED (saves on eject)"
            : "Disk II: write-back disabled";
        tapeStatusUntil = lastFrameTime + 4.0;
    }
    if (result.requestInsertDialog) {
        showDiskInsertDialog = true;
        if (diskDialogPath.empty()) diskDialogPath = "disks/";
    }
    if (result.requestEject && diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard->ejectDisk();
        tapeStatusMessage = "Disk ejected";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (result.requestBoot && diskCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard->seekTrack0();
        controller.cpu().setProgramCounter(0xC600);
        controller.setMode(EmulationController::Mode::Running);
        tapeStatusMessage = "Boot: PC → $C600";
        tapeStatusUntil   = lastFrameTime + 3.0;
    }
    if (!result.requestInsertAndBoot.empty() && diskCard) {
        // Library-click "insert + boot": insert under the lock so the
        // image is in the drive, then cold-boot from the reset vector
        // so the Apple II Monitor ROM runs its normal cold-start
        // sequence (HOME clears the text page, banner is displayed,
        // autostart scans slots and JSRs $C600). Going straight to
        // PC=$C600 (the old bootFromSlot path) skipped the ROM banner
        // and left the freshly-wiped text page showing as an `@`-tile
        // garbage frame until cc65/ProDOS switched to HGR — visible
        // for several real-time seconds on slow-loading binaries.
        const std::string path = result.requestInsertAndBoot;
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            ok = diskCard->insertDisk(path);
            if (ok) diskCard->seekTrack0();
            else    err = diskCard->getLastError();
        }
        if (ok) {
            controller.coldBoot();
            controller.setMode(EmulationController::Mode::Running);
            pom2::log().info("Disk II",
                std::string("Library click → insert + boot: ") + path);
            tapeStatusMessage = "Booting: " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

void MainWindow::renderHdvPanelWindow()
{
    if (!showHdvPanel) return;

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (hdvCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
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
            for (const auto& entry : fs::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".hdv" && ext != ".2mg") continue;
                pom2::HdvController_ImGui::LibraryEntry e;
                e.displayName = entry.path().filename().string();
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
    auto result = hdvPanel.render("HDV (slot 5)", showHdvPanel, snap);

    if (result.requestMountDialog) {
        showHdvMountDialog = true;
        if (hdvDialogPath.empty()) hdvDialogPath = "hdv/";
    }
    if (result.writeBackToggleChanged && hdvCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        hdvCard->setWriteBackEnabled(result.writeBackNewValue);
        tapeStatusMessage = result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : "HDV: write-back disabled";
        tapeStatusUntil   = lastFrameTime + 4.0;
    }
    if (result.requestEject && hdvCard) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
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
                std::lock_guard<std::mutex> lk(controller.stateMutex());
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
        // has the right blocks before bootFromSlot(5) wipes RAM and
        // jumps PC = $C500. Two-step lock is safe — the CPU worker only
        // resumes when bootFromSlot flips mode to Running.
        bool ok = false;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
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
            controller.bootFromSlot(5);
            pom2::log().info("HDV",
                std::string("Library click → mount + boot: ") + path);
            tapeStatusMessage = "Booting HDV: " + path;
        } else {
            tapeStatusMessage = "Boot failed: " + err;
        }
        tapeStatusUntil = lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    if (showDiskInsertDialog) {
        ImGui::OpenPopup("Insert disk image");
        showDiskInsertDialog = false;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("Path to a 5.25\" image —"
                           " .dsk / .do (DOS 3.3, 143 360 B) or"
                           " .po (ProDOS, 143 360 B) or .nib (raw"
                           " nibble stream, 232 960 B) or .woz"
                           " (bit-cell, copy-protected disks; read-only)."
                           " Write-back is opt-in via the panel checkbox.");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", diskDialogPath.c_str());
    if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        diskDialogPath = buf;
    else
        diskDialogPath = buf;

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
            if (ImGui::Selectable(name.c_str()))
                diskDialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (diskCard && !diskDialogPath.empty()) {
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            if (diskCard->insertDisk(diskDialogPath)) {
                tapeStatusMessage = "Disk inserted: " + diskDialogPath;
            } else {
                tapeStatusMessage = "Insert failed: " + diskCard->getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainWindow::renderHdvFileDialog()
{
    if (showHdvMountDialog) {
        ImGui::OpenPopup("Mount HDV image");
        showHdvMountDialog = false;
    }
    if (!ImGui::BeginPopupModal("Mount HDV image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("ProDOS block-device image — .hdv (raw blocks)"
                           " or .2mg (with 2IMG header, ProDOS order)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", hdvDialogPath.c_str());
    if (ImGui::InputText("##HdvPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        hdvDialogPath = buf;
    else
        hdvDialogPath = buf;

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
                hdvDialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    const bool canMount = hdvCard && !hdvDialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        if (hdvCard->loadImage(hdvDialogPath)) {
            hdvPath   = hdvDialogPath;
            hdvStatus = std::string("loaded: ") + hdvDialogPath;
            tapeStatusMessage = "HDV mounted: " + hdvDialogPath;
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
            std::lock_guard<std::mutex> lk(controller.stateMutex());
            ok = hdvCard->loadImage(hdvDialogPath);
            if (ok) {
                hdvPath   = hdvDialogPath;
                hdvStatus = std::string("loaded: ") + hdvDialogPath;
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

void MainWindow::renderAboutDialog()
{
    if (!showAbout) return;
    ImGui::OpenPopup("About POM2");
    if (ImGui::BeginPopupModal("About POM2", &showAbout,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("POM2 v0.1");
        ImGui::Text("Apple II / II+ emulator (Dear ImGui, MOS 6502)");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Apple II / II+ emulator. MOS 6502, 48 KB RAM,"
            " text / lo-res / hi-res display, soft-switch I/O.");
        ImGui::Spacing();
        ImGui::Text("F11 = Reset (Ctrl-Reset)   F12 = Hard reset");
        ImGui::Text("ESC, arrows, Ctrl-A..Z map straight to the keyboard");
        ImGui::Spacing();
        if (ImGui::Button("Close")) showAbout = false;
        ImGui::EndPopup();
    }
}

void MainWindow::renderMemoryViewerWindow()
{
    if (!showMemViewer) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &showMemViewer)) {
        // Hold the state mutex briefly so the snapshot the viewer reads
        // (Memory::data()) is consistent — no torn writes mid-row.
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        memViewer.render();
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
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        const CassetteDevice& d = controller.cassette();
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
    auto result = cassetteDeck.render("Cassette Deck",
                                      showCassetteDeck,
                                      &controller,
                                      snap,
                                      deltaSeconds);

    if (result.requestLoadDialog) {
        showTapeLoadDialog = true;
        if (tapeDialogPath.empty()) tapeDialogPath = "cassettes/";
    }
    if (result.requestSaveDialog) {
        showTapeSaveDialog = true;
        if (tapeDialogPath.empty()) tapeDialogPath = "cassettes/recording.aci";
    }
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

    if (showTapeLoadDialog) {
        ImGui::OpenPopup("Load Tape");
        showTapeLoadDialog = false;
    }
    if (ImGui::BeginPopupModal("Load Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Tape file path (.aci / .wav / .mp3 / .ogg / .flac)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", tapeDialogPath.c_str());
        if (ImGui::InputText("##LoadPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            tapeDialogPath = buf;
        else
            tapeDialogPath = buf;

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
                    tapeDialogPath = entry.path().string();
            }
            break;  // first existing candidate dir wins
        }

        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (controller.loadTape(tapeDialogPath)) {
                tapeStatusMessage = "Tape loaded: " + tapeDialogPath;
            } else {
                tapeStatusMessage = "Load failed: " + controller.cassette().getLastError();
            }
            tapeStatusUntil = lastFrameTime + 5.0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (showTapeSaveDialog) {
        ImGui::OpenPopup("Save Tape");
        showTapeSaveDialog = false;
    }
    if (ImGui::BeginPopupModal("Save Tape", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        pathInput("Output file path (.aci or .wav)");
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", tapeDialogPath.c_str());
        if (ImGui::InputText("##SavePath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            tapeDialogPath = buf;
        else
            tapeDialogPath = buf;

        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (controller.saveTape(tapeDialogPath)) {
                tapeStatusMessage = "Tape saved: " + tapeDialogPath;
            } else {
                tapeStatusMessage = "Save failed: " + controller.cassette().getLastError();
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

    renderMenuBar();
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
    renderHdvPanelWindow();
    renderChatMauvePanelWindow();
    renderSscPanelWindow();
    renderJoystickPanelWindow();
    renderSlotConfigPanel();
    renderAboutDialog();
}
