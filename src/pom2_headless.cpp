// POM2 Apple II Emulator — headless console.
// Copyright (C) 2026
//
// A console-only build that brings up the same core (Memory + M6502 +
// SlotBus + DiskIICard + SuperSerialCard) without GLFW or ImGui. The
// SSC's TCP listener is the user-facing terminal: telnet to the port,
// type Apple II keystrokes, see PRINT output.
//
// Boot/post-boot keyboard injection: a real disk boots into Applesoft
// without knowing the SSC exists, so the emulator pastes a small setup
// sequence (`IN#2 / PR#2`) once the disk has had time to spin up. After
// that, every byte the telnet client sends is fed via the SSC into the
// Apple II's input vector, and every PRINTed byte comes back out on the
// SSC's TX path.
//
// Designed for headless self-test: launch in the background, telnet in,
// type DOS 3.3 commands, observe behaviour. Exits cleanly on SIGINT.

#include "ClockCard.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Memory.h"
#include "SuperSerialCard.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int /*sig*/) { g_quit = true; }

bool fileExists(const std::string& p)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string findFirst(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) if (fileExists(c)) return c;
    return {};
}

void usage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "  --rom <path>          apple2.rom (12K or 16K).      Default: probe roms/\n"
        "  --prom <path>         disk2.rom Disk II PROM.      Default: probe roms/\n"
        "  --disk <path>         .dsk/.do/.po/.nib floppy.    Default: dos33_master.dsk\n"
        "  --port <N>            SSC TCP port. Default 6502\n"
        "  --paste-after <sec>   Emulated seconds before autopasting\n"
        "                        the SSC setup sequence. Default 6.\n"
        "  --setup <s>           Override the setup paste. Default \"IN#2\\rPR#2\\r\".\n"
        "                        Use \\r for RETURN, \\n is left as-is.\n"
        "  --no-setup            Don't autopaste anything.\n"
        "\n"
        "Once running, telnet to 127.0.0.1:<port> to interact.\n",
        prog);
}

std::string unescape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            switch (in[i + 1]) {
                case 'r': out.push_back('\r'); ++i; break;
                case 'n': out.push_back('\n'); ++i; break;
                case 't': out.push_back('\t'); ++i; break;
                case '\\': out.push_back('\\'); ++i; break;
                default:  out.push_back(in[i]);
            }
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    std::string romArg, promArg, diskArg, setupOverride;
    int  port        = SuperSerialCard::kDefaultPort;
    int  pasteAfter  = 6;     // emulated seconds before pasting setup
    bool doSetup     = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--rom"   && i+1 < argc) romArg   = argv[++i];
        else if (a == "--prom"  && i+1 < argc) promArg  = argv[++i];
        else if (a == "--disk"  && i+1 < argc) diskArg  = argv[++i];
        else if (a == "--port"  && i+1 < argc) port     = std::atoi(argv[++i]);
        else if (a == "--paste-after" && i+1 < argc) pasteAfter = std::atoi(argv[++i]);
        else if (a == "--setup" && i+1 < argc) setupOverride = argv[++i];
        else if (a == "--no-setup") doSetup = false;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return 1; }
    }

    const std::string romPath = !romArg.empty() ? romArg : findFirst({
        "roms/apple2.rom", "../roms/apple2.rom", "../../roms/apple2.rom" });
    const std::string promPath = !promArg.empty() ? promArg : findFirst({
        "roms/disk2.rom", "../roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string diskPath = !diskArg.empty() ? diskArg : findFirst({
        "disks/dos33_master.dsk", "../disks/dos33_master.dsk",
        "../../disks/dos33_master.dsk" });

    if (romPath.empty() || promPath.empty() || diskPath.empty()) {
        std::fprintf(stderr, "missing rom/prom/disk; try --help\n");
        return 1;
    }
    std::fprintf(stderr,
        "[POM2 headless] rom=%s prom=%s disk=%s port=%d\n",
        romPath.c_str(), promPath.c_str(), diskPath.c_str(), port);

    // II/II+ mode is enough for DOS 3.3. We deliberately avoid loading
    // apple2e.rom here even if present so the headless target stays
    // simple; switch via --rom if you need the IIe ROM.
    EmulationController controller;
    if (!controller.memory().loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n"); return 1;
    }

    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(promPath))         { std::fprintf(stderr, "PROM load failed\n");   return 1; }
    // Optional: load the bit-level LSS PROM if available. Falls back to
    // the legacy 32-cycle gate when missing.
    {
        namespace fs = std::filesystem;
        for (const char* p : { "roms/diskii_p6.rom",
                                "../roms/diskii_p6.rom",
                                "../../roms/diskii_p6.rom" }) {
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) { (void)disk->loadLssRom(p); break; }
        }
    }
    if (!disk->insertDisk(diskPath))          {
        std::fprintf(stderr, "insertDisk failed: %s\n", disk->getLastError().c_str()); return 1;
    }
    DiskIICard* diskRaw = disk.get();
    controller.memory().slotBus().plug(6, std::move(disk));

    auto ssc = std::make_unique<SuperSerialCard>(SuperSerialCard::kDefaultSlot);
    ssc->setKeyboardSink(
        [&mem = controller.memory()](uint8_t b) {
            const char buf[1] = { static_cast<char>(b) };
            mem.pasteText(buf, 1);
        });
    if (!ssc->startListening(static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "SSC listener failed (port busy?)\n"); return 1;
    }
    controller.memory().slotBus().plug(SuperSerialCard::kDefaultSlot, std::move(ssc));

    // ThunderClock+-compatible RTC in slot 4 — uPD1990AC bit-bang chip
    // emulation; ProDOS auto-detects and links its driver to it.
    controller.memory().slotBus().plug(ClockCard::kDefaultSlot,
        std::make_unique<ClockCard>(ClockCard::kDefaultSlot));

    // Mirror the live-emulator boot ritual: hard reset, propagate that
    // through the slot bus (so DiskIICard can re-arm its trace flags),
    // then start running. We don't go through the menu's PR#6 — the
    // disk PROM autoboots when PC starts at $C600, which is what
    // hardReset() leaves PC pointing at when the reset vector at $FFFC
    // is set up by the Apple II ROM to land in slot 6.
    controller.cpu().hardReset();
    controller.memory().slotBus().reset();
    diskRaw->seekTrack0();
    // The boot vector in stock apple2.rom auto-jumps to $C600 via the
    // Autostart Monitor's PR#6 path; just to be safe we also poke PC.
    controller.cpu().setProgramCounter(0xC600);
    controller.setMode(EmulationController::Mode::Running);
    controller.start();

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // Boot wait loop. Each iteration sleeps 100 ms, tracks emulated
    // seconds via cycleCounter, and once enough time has elapsed sends
    // the setup paste once (so the SSC slot is wired to BASIC's CSW/KSW).
    bool pasted = !doSetup;
    // Order matters: PR#2 patches the CHARACTER OUTPUT vector (CSWL/CSWH)
    // first, so the BASIC prompt and any echo go straight to the SSC. The
    // following IN#2 patches the INPUT vector (KSWL/KSWH); after that
    // moment, BASIC's GETLN reads from the SSC instead of the keyboard
    // latch — and the rest of our paste queue would be ignored, so the
    // CR after `IN#2` MUST be the last thing in the buffer.
    const std::string setup = unescape(setupOverride.empty()
        ? std::string("PR#2\rIN#2\r")
        : setupOverride);

    std::fprintf(stderr,
        "[POM2 headless] running. telnet 127.0.0.1 %d to interact.\n", port);
    if (doSetup) {
        std::fprintf(stderr,
            "[POM2 headless] setup paste %u chars at t≈%ds.\n",
            static_cast<unsigned>(setup.size()), pasteAfter);
    }

    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto cycles  = controller.memory().getCycleCounter();
        const int  seconds = static_cast<int>(cycles / 1'022'727);
        if (!pasted && seconds >= pasteAfter) {
            std::fprintf(stderr,
                "[POM2 headless] t=%ds — pasting setup.\n", seconds);
            controller.memory().pasteText(setup);
            pasted = true;
        }
    }

    std::fprintf(stderr, "[POM2 headless] shutting down.\n");
    controller.stop();
    return 0;
}
