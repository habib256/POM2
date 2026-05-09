// Copy II Plus 8.3 boot dump — one-off diagnostic. Reuses MainWindow's
// slot wiring (Memory + M6502 + Disk II + optional HDV) to boot the disk
// and log:
//   - the screen text every N seconds
//   - the highest-loaded address inside UTIL.SYSTEM ($2000-$8FFF)
//   - Disk II soft-switch traffic (when POM2_DEBUG_DISK=1)
//
// Compiled but not registered as a CTest target — it's a debug aid.

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
    bool plugHdv = true;
    bool forceIIPlus = false;
    int budgetSec = 30;
    std::string diskOverride;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-hdv") plugHdv = false;
        else if (a == "--ii+") forceIIPlus = true;
        else if (a == "--seconds" && i + 1 < argc) budgetSec = std::atoi(argv[++i]);
        else if (a == "--disk" && i + 1 < argc) diskOverride = argv[++i];
    }

    const std::string iieRom = forceIIPlus ? std::string{} : findFirst({
        "../roms/apple2e.rom", "roms/apple2e.rom", "../../roms/apple2e.rom" });
    const std::string romPath = iieRom.empty() ? findFirst({
        "../roms/apple2.rom", "roms/apple2.rom", "../../roms/apple2.rom"})
        : iieRom;
    const std::string promPath = findFirst({
        "../roms/disk2.rom", "roms/disk2.rom", "../../roms/disk2.rom" });
    const std::string lssPromPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom" });
    const std::string dskPath  = !diskOverride.empty() ? diskOverride :
        findFirst({
            "../disks/Copy II Plus v8.3.do",
            "disks/Copy II Plus v8.3.do",
            "../../disks/Copy II Plus v8.3.do" });
    const std::string hdvImg = findFirst({
        "../hdv/A2DeskTop-GIST.hdv",
        "hdv/A2DeskTop-GIST.hdv",
        "../../hdv/A2DeskTop-GIST.hdv" });

    if (romPath.empty() || promPath.empty() || dskPath.empty()) {
        std::fprintf(stderr,
            "missing apple2[e].rom, disk2.rom, or disk image\n");
        return 1;
    }
    std::printf("rom=%s\nprom=%s\ndsk=%s\n",
                romPath.c_str(), promPath.c_str(), dskPath.c_str());

    Memory mem;
    if (!iieRom.empty()) mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(romPath.c_str())) {
        std::fprintf(stderr, "loadAppleIIRom failed\n");
        return 1;
    }

    auto disk = std::make_unique<DiskIICard>();
    if (!disk->loadBootRom(promPath)) {
        std::fprintf(stderr, "Disk II PROM load failed\n");
        return 1;
    }
    if (!lssPromPath.empty()) {
        (void)disk->loadLssRom(lssPromPath);
    }
    if (!disk->insertDisk(dskPath)) {
        std::fprintf(stderr, "insertDisk failed: %s\n",
                     disk->getLastError().c_str());
        return 1;
    }
    DiskIICard* diskRaw = disk.get();
    mem.slotBus().plug(6, std::move(disk));

    if (plugHdv) {
        auto hdv = std::make_unique<ProDOSHardDiskCard>();
        if (!hdvImg.empty()) (void)hdv->loadImage(hdvImg);
        mem.slotBus().plug(5, std::move(hdv));
    }

    M6502 cpu(&mem);
    cpu.hardReset();
    mem.slotBus().reset();
    cpu.setProgramCounter(0xC600);

    constexpr int kSliceCycles = 100'000;
    const int kBudget = budgetSec * 1'022'727;
    int total = 0;
    int last = -1;
    while (total < kBudget) {
        const int slice = cpu.run(kSliceCycles);
        total += slice;
        const int sec = total / 1'022'727;
        if (sec != last && (sec % 10 == 0)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                "t=%ds PC=$%04X", sec, cpu.getProgramCounter());
            dumpScreen(mem, buf);
            // Find the highest non-zero page in $2000-$8FFF (UTIL.SYSTEM
            // load region). Coarse scan, 256-byte granularity.
            int hi = 0x2000;
            for (int a = 0x8FFF; a >= 0x2000; --a) {
                if (mem.data()[a] != 0) { hi = a; break; }
            }
            std::printf("UTIL.SYS top: $%04X  $D370=$%02X  $D36F=$%02X\n",
                hi, mem.memRead(0xD370), mem.memRead(0xD36F));
            last = sec;
        }
    }

    std::printf("final PC=$%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X\n",
                cpu.getProgramCounter(), cpu.getAccumulator(),
                cpu.getXRegister(), cpu.getYRegister(),
                cpu.getStackPointer(), cpu.getStatusRegister());

    // Stack walk — dump the chain of return addresses so we can see what
    // outer caller is in the watchdog/dispatcher loop.
    std::printf("Stack (%d entries from $%04X):\n",
                0xFF - cpu.getStackPointer(), 0x100 + cpu.getStackPointer() + 1);
    for (int i = 1; i + cpu.getStackPointer() <= 0xFF && i < 32; ++i) {
        const uint8_t lo = mem.memRead(0x100 + cpu.getStackPointer() + i);
        ++i;
        if (i + cpu.getStackPointer() > 0xFF) break;
        const uint8_t hi = mem.memRead(0x100 + cpu.getStackPointer() + i);
        const uint16_t ret = (lo | (hi << 8)) + 1;
        std::printf("  ret $%04X\n", ret);
    }

    // What's at $2000 (UTIL.SYSTEM entry).
    std::printf("$2000 entry: ");
    for (int i = 0; i < 24; ++i) std::printf("%02X ", mem.memRead(0x2000 + i));
    std::printf("\n");

    // ProDOS MLI parameter block ($42-$47):
    //   $42 = command (0=status, 1=read, 2=write, 3=format)
    //   $43 = unit number (slot << 4 | drive << 7)
    //   $44/$45 = buffer address
    //   $46/$47 = block number (0..0xFFFF)
    std::printf("MLI param block:\n");
    std::printf("  $42 cmd  = $%02X (%s)\n", mem.memRead(0x42),
        mem.memRead(0x42) == 0 ? "status" :
        mem.memRead(0x42) == 1 ? "READ" :
        mem.memRead(0x42) == 2 ? "WRITE" :
        mem.memRead(0x42) == 3 ? "format" : "?");
    std::printf("  $43 unit = $%02X (slot %d drive %d)\n",
        mem.memRead(0x43),
        (mem.memRead(0x43) >> 4) & 7,
        (mem.memRead(0x43) >> 7) + 1);
    std::printf("  $44/45 buf = $%02X%02X\n",
        mem.memRead(0x45), mem.memRead(0x44));
    std::printf("  $46/47 blk = $%02X%02X\n",
        mem.memRead(0x47), mem.memRead(0x46));
    std::printf("  $D356 = $%02X (block-high mirror)\n", mem.memRead(0xD356));
    std::printf("  $D357 = $%02X (driver private)\n", mem.memRead(0xD357));
    std::printf("  $D358 = $%02X (last error)\n", mem.memRead(0xD358));

    // UTIL.SYSTEM call sites — show 8 bytes BEFORE each presumed JSR.
    auto showCallSite = [&](uint16_t ret, const char* label) {
        std::printf("%s callsite (ret=$%04X):", label, ret);
        for (int i = -8; i < 4; ++i) {
            std::printf(" %02X", mem.memRead(static_cast<uint16_t>(ret + i)));
        }
        std::printf("\n");
    };
    // Re-walk the stack interpreting every uint16 as a possible return.
    for (int i = 1; i + cpu.getStackPointer() <= 0xFF && i < 32; i += 2) {
        const uint8_t lo = mem.memRead(0x100 + cpu.getStackPointer() + i);
        if (i + 1 + cpu.getStackPointer() > 0xFF) break;
        const uint8_t hi = mem.memRead(0x100 + cpu.getStackPointer() + i + 1);
        const uint16_t ret = (lo | (hi << 8)) + 1;
        // Only show entries that look code-ish (1000-EFFF in RAM).
        if (ret >= 0x1000 && ret <= 0xFFFF) {
            const uint8_t op = mem.memRead(static_cast<uint16_t>(ret - 3));
            if (op == 0x20) {           // 'JSR abs'
                showCallSite(ret, "JSR");
            } else if (op == 0x6C || op == 0x4C) {
                showCallSite(ret, "??");
            }
        }
    }
    // Buffer wherever ProDOS most recently asked it to be loaded.
    const uint16_t bufAddr = mem.memRead(0x44) | (mem.memRead(0x45) << 8);
    std::printf("buf $%04X: ", bufAddr);
    for (int i = 0; i < 32; ++i) std::printf("%02X ", mem.memRead(bufAddr + i));
    std::printf("\n");
    std::printf("buf+256: ");
    for (int i = 0; i < 16; ++i) std::printf("%02X ", mem.memRead(bufAddr + 256 + i));
    std::printf("\n");
    // Last error register at $D358.
    std::printf("retry counter $D36B = $%02X\n", mem.memRead(0xD36B));
    std::printf("addr-found $D5A = $%02X (0=match, !=0=keep searching)\n",
                mem.memRead(0xD5A));

    // ── Crash-site dump: bytes + disassembly around final PC ─────────
    {
        const uint16_t pc = cpu.getProgramCounter();
        std::printf("\n=== CRASH-SITE @ PC=$%04X ===\n", pc);
        // 64-byte raw window (32 before, 32 after PC).
        const uint16_t winLo = static_cast<uint16_t>(pc - 32);
        std::printf("bytes $%04X..$%04X:\n", winLo,
                    static_cast<uint16_t>(winLo + 63));
        for (int row = 0; row < 4; ++row) {
            const uint16_t base = static_cast<uint16_t>(winLo + row * 16);
            std::printf("  $%04X:", base);
            for (int c = 0; c < 16; ++c) {
                std::printf(" %02X", mem.memRead(static_cast<uint16_t>(base + c)));
            }
            std::printf("\n");
        }
        // Disassembly: snapshot the window via memRead (so LC/aux
        // routing is honoured). Skip $C000-$C0FF because reading those
        // toggles soft switches — we'd corrupt paging state mid-dump.
        std::vector<uint8_t> snap(0x10000, 0);
        for (int i = 0; i < 0x10000; ++i) {
            if (i >= 0xC000 && i <= 0xC0FF) continue;
            snap[i] = mem.memRead(static_cast<uint16_t>(i));
        }
        std::printf("disasm $%04X..$%04X (* = current PC):\n",
                    winLo, static_cast<uint16_t>(winLo + 63));
        uint16_t cur = winLo;
        const uint16_t end = static_cast<uint16_t>(winLo + 64);
        while (cur < end) {
            int len = 0;
            const std::string mn = pom2::disassemble6502(snap.data(), cur, len);
            std::printf("  %s$%04X:", cur == pc ? "*" : " ", cur);
            for (int j = 0; j < 3; ++j) {
                if (j < len) std::printf(" %02X", snap[cur + j]);
                else         std::printf("   ");
            }
            std::printf("  %s\n", mn.c_str());
            cur = static_cast<uint16_t>(cur + (len > 0 ? len : 1));
        }
        // Zero page snapshot for context (Copy II+ stores all its working
        // state in $00..$FF and $D000-area in LC).
        std::printf("zero page $00..$3F:\n");
        for (int row = 0; row < 4; ++row) {
            std::printf("  $%02X:", row * 16);
            for (int c = 0; c < 16; ++c) {
                std::printf(" %02X", mem.memRead(static_cast<uint16_t>(row * 16 + c)));
            }
            std::printf("\n");
        }
        // IIe paging + Disk II state. Read status flags non-intrusively
        // through the status soft switches at $C013-$C018, $C01E/$C01F.
        std::printf("IIe paging:");
        std::printf(" RAMRD=%d",  (mem.memRead(0xC013) & 0x80) != 0);
        std::printf(" RAMWRT=%d", (mem.memRead(0xC014) & 0x80) != 0);
        std::printf(" CXROM=%d",  (mem.memRead(0xC015) & 0x80) != 0);
        std::printf(" ALTZP=%d",  (mem.memRead(0xC016) & 0x80) != 0);
        std::printf(" SLOTC3=%d", (mem.memRead(0xC017) & 0x80) != 0);
        std::printf(" 80STORE=%d",(mem.memRead(0xC018) & 0x80) != 0);
        std::printf(" 80COL=%d",  (mem.memRead(0xC01F) & 0x80) != 0);
        std::printf(" ALTCHAR=%d",(mem.memRead(0xC01E) & 0x80) != 0);
        std::printf("\n");
        std::printf("Disk II: track=%d (qt=%d) motor=%d wf=%llu\n",
            diskRaw->getCurrentTrack(),
            diskRaw->getQuarterTrack(),
            diskRaw->isMotorOn() ? 1 : 0,
            static_cast<unsigned long long>(diskRaw->getWriteFlushCount()));
        // Sample 32 successive nibbles WITHOUT advancing the CPU. Toggling
        // the soft switches that read $C0EC has side effects on the LSS so
        // we use the controller's read API once and then walk the nibble
        // buffer directly via getQuarterTrack/getCurrentTrack — which only
        // expose head position, not raw bytes. So just dump the latched
        // dataLatch via a sequence of $C0EC reads through memRead. This
        // changes byteReady but doesn't move the head.
        std::printf("nibble samples @ $C0EC (32 reads):");
        for (int i = 0; i < 32; ++i) {
            std::printf(" %02X", mem.memRead(0xC0EC));
        }
        std::printf("\n");
    }

    cpu.dumpPcTrace("trace");
    (void)diskRaw;
    return 0;
}
