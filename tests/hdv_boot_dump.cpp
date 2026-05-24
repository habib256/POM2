// HDV boot dump — diagnostic for the "ProDOS loads then user binary BRKs"
// pattern on .2mg HDV images. Boots //e from a user-supplied HDV in slot 5,
// runs for N seconds, traps BRK by watching the IRQ/B flag, and logs the
// PC + register state + screen at the moment of trap.
//
// Usage:
//   hdv_boot_dump --image ../hdv/ScoSwamp-0.5alpha.2mg
//
// Not registered as a CTest target — debug aid only.

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "Disassembler6502.h"
#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

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

void writePpm(Apple2Display& disp, Memory& mem, const std::string& path)
{
    disp.render(mem);
    const uint32_t* px = disp.pixels();
    const int w = disp.width(), h = disp.height();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        const uint32_t p = px[i];
        const unsigned char rgb[3] = {
            static_cast<unsigned char>( p        & 0xFF),
            static_cast<unsigned char>((p >>  8) & 0xFF),
            static_cast<unsigned char>((p >> 16) & 0xFF),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

void dumpScreen(Memory& mem, const char* label)
{
    std::printf("=== SCREEN %s ===\n", label);
    for (int row = 0; row < 24; ++row) {
        const int base = 0x400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            uint8_t b = mem.data()[base + col];
            b &= 0x7F;
            if (b < 0x20) b = ' ';
            std::putchar(static_cast<char>(b));
        }
        std::putchar('\n');
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::string hdvImage;
    int budgetSec = 20;
    std::string keysRaw;        // fed one key per keyboard-wait
    int keysAtSec = -1;         // unused now; kept for compat
    std::string shotDir;        // if set, write a PPM screenshot each second
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--image" && i + 1 < argc) hdvImage = argv[++i];
        else if (a == "--seconds" && i + 1 < argc) budgetSec = std::atoi(argv[++i]);
        else if (a == "--keys" && i + 1 < argc) keysRaw = argv[++i];
        else if (a == "--keys-at" && i + 1 < argc) keysAtSec = std::atoi(argv[++i]);
        else if (a == "--shotdir" && i + 1 < argc) shotDir = argv[++i];
    }
    // Decode escapes in the key script: \r=Return(0x0D), \n=0x0D, \e=Esc(0x1B),
    // \t=Tab, \\=backslash. Everything else is taken verbatim (ASCII).
    std::string keys;
    for (size_t i = 0; i < keysRaw.size(); ++i) {
        if (keysRaw[i] == '\\' && i + 1 < keysRaw.size()) {
            char c = keysRaw[++i];
            switch (c) {
                case 'r': case 'n': keys.push_back('\r'); break;
                case 'e': keys.push_back('\x1b'); break;
                case 't': keys.push_back('\t'); break;
                // Apple //e arrow keys.
                case 'u': keys.push_back('\x0b'); break;   // up
                case 'd': keys.push_back('\x0a'); break;   // down
                case 'l': keys.push_back('\x08'); break;   // left
                case 'g': keys.push_back('\x15'); break;   // right
                default:  keys.push_back(c); break;
            }
        } else {
            keys.push_back(keysRaw[i]);
        }
    }
    if (hdvImage.empty()) {
        std::fprintf(stderr, "Usage: %s --image <path.2mg|.hdv> [--seconds N]\n",
                     argv[0]);
        return 1;
    }

    const std::string romPath = findFirst({
        "../roms/apple2e.rom", "roms/apple2e.rom", "../../roms/apple2e.rom" });
    if (romPath.empty()) {
        std::fprintf(stderr, "missing apple2e.rom under roms/\n");
        return 1;
    }
    std::printf("rom=%s\nhdv=%s\n", romPath.c_str(), hdvImage.c_str());

    Memory mem;
    mem.setIIEMode(true);
    mem.setIoReadTrace(true);   // record $C0xx reads for the wait-loop tally
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto hdv = std::make_unique<ProDOSHardDiskCard>();
    if (!hdv->loadImage(hdvImage)) {
        std::fprintf(stderr, "HDV loadImage failed: %s\n",
                     hdv->getLastError().c_str());
        return 1;
    }
    mem.slotBus().plug(5, std::move(hdv));

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();

    // Watch for the BRK trap by sampling PC every short slice. When the CPU
    // jumps into the IIe ROM BRK handler ($FA40-ish), we capture the
    // pre-jump state from the stack.
    constexpr int kStepCycles = 1;       // single-step
    const int kBudget = budgetSec * 1'022'727;
    int total = 0;
    int lastSec = -1;
    uint16_t lastPC = 0;
    bool keysFed = false;
    size_t keyIdx = 0;
    int lastFeedSec = -100;
    bool brkSeen = false;
    uint16_t brkPC = 0;
    uint8_t brkA = 0, brkX = 0, brkY = 0, brkP = 0, brkS = 0;

    // Ring buffer of the last N PCs traversed, for trace-on-BRK.
    constexpr int kRing = 512;
    uint16_t pcRing[kRing] = {0};
    uint8_t  cycRing[kRing] = {0};
    int ringIdx = 0;

    while (total < kBudget && !brkSeen) {
        const uint16_t pcBefore = cpu.getProgramCounter();
        const int slice = cpu.run(kStepCycles);
        total += slice;
        const uint16_t pc = cpu.getProgramCounter();
        pcRing[ringIdx] = pcBefore;
        cycRing[ringIdx] = static_cast<uint8_t>(slice);
        ringIdx = (ringIdx + 1) % kRing;
        // Apple //e ROM BRK handler entry is around $FA40-$FA60. The
        // hardware vector is at $FFFE/$FFFF; we want to catch the moment
        // we first land in the handler region. Reset vector $FA62 also
        // sits in this range, so guard against the very first execution
        // by requiring the cycle budget to have made progress.
        if (total > 200'000 && pc >= 0xFA40 && pc <= 0xFA70 && lastPC < 0xFA40) {
            brkSeen = true;
            // BRK pushed PCH, PCL, P. SP is now (pre-BRK SP) - 3.
            const uint8_t sp = cpu.getStackPointer();
            const uint8_t pLo = mem.memRead(0x100 + ((sp + 1) & 0xFF));
            const uint8_t pPCLo = mem.memRead(0x100 + ((sp + 2) & 0xFF));
            const uint8_t pPCHi = mem.memRead(0x100 + ((sp + 3) & 0xFF));
            // For BRK the saved PC points to the byte AFTER the BRK
            // (BRK is 2-byte: opcode + signature byte). The instruction
            // that caused BRK is at saved-PC - 2.
            brkPC = ((static_cast<uint16_t>(pPCHi) << 8) | pPCLo) - 2;
            brkA = cpu.getAccumulator();
            brkX = cpu.getXRegister();
            brkY = cpu.getYRegister();
            brkP = pLo;
            brkS = (sp + 3) & 0xFF;     // pre-BRK SP
            break;
        }
        lastPC = pc;

        const int sec = total / 1'022'727;
        if (sec != lastSec) {
            const std::string tally = mem.recentIoReadSummary();
            std::printf("[t=%2ds] PC=$%04X  io: %s\n", sec, pc, tally.c_str());
            lastSec = sec;
            if (!shotDir.empty())
                writePpm(disp, mem, shotDir + "/f" + std::to_string(sec) + ".ppm");
            // Feed the key script ONE key per input-responsive moment: when
            // the recent reads include $C000 (the game is polling the
            // keyboard) and a minimum gap has elapsed since the last key, so
            // each key lands on its own prompt (title → slot → moves → enter).
            (void)keysFed; (void)keysAtSec;
            // Key feeding. The first two keys (Return on the title, '1' at the
            // slot prompt) are STATIC menu screens: feed only when $C000 is the
            // top read AND there is NO $C019/VBL (i.e. NOT the animated title —
            // feeding during the title animation gets the key dropped). The
            // remaining keys (↑ ↑ E) act on the OVERWORLD, which animates (VBL
            // present), so just require $C000 to be the top read. 3 s gap so
            // each key lands on its own prompt. pasteRawKeys uses the proven
            // FIFO+strobe path.
            const bool c000top  = (tally.rfind("$C000", 0) == 0);
            const bool noVbl    = (tally.find("$C019") == std::string::npos);
            const bool menuKey  = (keyIdx < 2);
            const bool ready    = menuKey ? (c000top && noVbl) : c000top;
            if (keyIdx < keys.size() && (sec - lastFeedSec) >= 3 && ready) {
                const char k = keys[keyIdx];
                mem.pasteRawKeys(&k, 1);
                std::printf("    >>> fed key[%zu] = 0x%02X ('%c')\n",
                            keyIdx, (unsigned char)k,
                            (k >= 32 && k < 127) ? k : '.');
                ++keyIdx;
                lastFeedSec = sec;
            }
        }
    }

    if (brkSeen) {
        std::printf("\n*** BRK trapped ***\n");
        std::printf("  pre-BRK PC=$%04X  A=$%02X X=$%02X Y=$%02X P=$%02X S=$%02X\n",
                    brkPC, brkA, brkX, brkY, brkP, brkS);
        // Dump the ring of last PCs in chronological order.
        std::printf("  PC trace (last %d, oldest first) — PC BEFORE step + cycles:\n", kRing);
        for (int i = 0; i < kRing; ++i) {
            const int k = (ringIdx + i) % kRing;
            std::printf("    [%3d] $%04X  cyc=%d\n", i, pcRing[k], cycRing[k]);
        }
        std::printf("  bytes around PC: ");
        for (int o = -4; o <= 6; ++o) {
            const uint16_t a = static_cast<uint16_t>(brkPC + o);
            std::printf("%s%02X ", o == 0 ? "[" : "",
                        mem.memRead(a));
            if (o == 0) std::printf("\b]");
            if (o == 0) std::printf(" ");
        }
        std::printf("\n");
        // Disassemble the 16 bytes starting at the instruction we BRK'd on.
        std::printf("  disasm from $%04X:\n", brkPC);
        std::vector<uint8_t> buf(64);
        for (int i = 0; i < 64; ++i) buf[i] = mem.memRead((brkPC + i) & 0xFFFF);
        uint16_t pc = brkPC;
        int consumed = 0;
        while (consumed < 24) {
            int len = 0;
            std::string mnem = pom2::disassemble6502(buf.data() + consumed, pc, len);
            std::printf("    $%04X: ", pc);
            for (int k = 0; k < len; ++k) std::printf("%02X ", buf[consumed + k]);
            for (int k = len; k < 3; ++k) std::printf("   ");
            std::printf(" %s\n", mnem.c_str());
            pc = static_cast<uint16_t>(pc + len);
            consumed += len;
        }

        // Stack walk.
        std::printf("  stack walk (32 entries from $%04X):\n",
                    0x100 + brkS + 1);
        for (int i = 1; i + brkS <= 0xFF && i < 32; ++i) {
            const uint8_t lo = mem.memRead(0x100 + ((brkS + i) & 0xFF));
            ++i;
            if (i + brkS > 0xFF) break;
            const uint8_t hi = mem.memRead(0x100 + ((brkS + i) & 0xFF));
            const uint16_t ret = ((static_cast<uint16_t>(hi) << 8) | lo) + 1;
            std::printf("    [$%04X] ret $%04X\n",
                        0x100 + ((brkS + i - 1) & 0xFF), ret);
        }

        // Dump a few key memory regions to help reverse-engineer the
        // dispatch that led PC into zero RAM.
        std::printf("\n  $D030..$D04F (as the CPU sees it at BRK time):\n");
        for (int i = 0; i < 32; i += 8) {
            std::printf("    $%04X:", 0xD030 + i);
            for (int k = 0; k < 8; ++k)
                std::printf(" %02X", mem.memRead(0xD030 + i + k));
            std::printf("\n");
        }
        // Probe each LC bank: bank 2, bank 1, and high RAM.
        auto probe = [&](const char* label, int switchAddr){
            (void)mem.memRead(switchAddr);
            (void)mem.memRead(switchAddr);
            std::printf("\n  %s: forced via $C%03X ×2\n", label, switchAddr & 0xFFF);
            for (uint16_t base : {0xD030, 0xD400, 0xE000, 0xF400}) {
                std::printf("    $%04X:", base);
                for (int k = 0; k < 8; ++k)
                    std::printf(" %02X", mem.memRead(base + k));
                std::printf("\n");
            }
        };
        probe("LC bank 2 read", 0xC080);
        probe("LC bank 1 read", 0xC088);

        // What does the CPU *actually* read at $CAE2, where the trace
        // shows uniform +3 advancement? Force INTCXROM via $C007 hit, then
        // memRead.
        // First check internalIORom directly via the public accessor.
        const uint8_t* iorom = mem.internalIORomData();
        std::printf("\n  internalIORom[$0AE2..$0AF0]:");
        for (int k = 0; k < 14; ++k) std::printf(" %02X", iorom[0x0AE2 + k]);
        std::printf("\n  memRead($CAE2..$CAF0) at current state:");
        for (int k = 0; k < 14; ++k) std::printf(" %02X", mem.memRead(0xCAE2 + k));
        std::printf("\n");
        std::printf("\n  $BF00..$BF20 (ProDOS MLI entry):\n");
        for (int i = 0; i < 32; i += 8) {
            std::printf("    $%04X:", 0xBF00 + i);
            for (int k = 0; k < 8; ++k)
                std::printf(" %02X", mem.memRead(0xBF00 + i + k));
            std::printf("\n");
        }
        std::printf("\n  $03F0..$03FF (BRK + soft vectors):\n");
        std::printf("    $03F0:");
        for (int k = 0; k < 16; ++k) std::printf(" %02X", mem.memRead(0x03F0 + k));
        std::printf("\n");
        dumpScreen(mem, "at BRK");
    } else {
        std::printf("\n=== no BRK in %d seconds ===\n", budgetSec);
        std::printf("final PC=$%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X\n",
                    cpu.getProgramCounter(), cpu.getAccumulator(),
                    cpu.getXRegister(), cpu.getYRegister(),
                    cpu.getStackPointer(), cpu.getStatusRegister());
        std::printf("recent $C0xx reads (most-polled first):\n  %s\n",
                    mem.recentIoReadSummary().c_str());
        // Zero-page trampoline diagnosis: the freeze jumps to $0081 (empty →
        // BRK). Show ZP $0040-$00BF in BOTH banks + paging, to see whether the
        // trampoline routine is in main or aux (or neither).
        std::printf("//e paging flags=$%04X (ALTZP=%d RAMRD=%d RAMWRT=%d 80STORE=%d)\n",
                    (unsigned)mem.iieModeFlags(),
                    (mem.iieModeFlags() & 0x10) ? 1 : 0,
                    (mem.iieModeFlags() & 0x02) ? 1 : 0,
                    (mem.iieModeFlags() & 0x04) ? 1 : 0,
                    (mem.iieModeFlags() & 0x01) ? 1 : 0);
        const uint8_t* mn = mem.data();
        const uint8_t* ax = mem.auxData();
        for (uint16_t base = 0x0000; base < 0x00C0; base += 16) {
            std::printf("  ZP $%04X main:", base);
            for (int k = 0; k < 16; ++k) std::printf(" %02X", mn[base + k]);
            std::printf("\n          aux :");
            for (int k = 0; k < 16; ++k) std::printf(" %02X", ax[base + k]);
            std::printf("\n");
        }
        dumpScreen(mem, "final");
    }
    return 0;
}
