// System profile smoke test — pure programmatic verification of the
// SystemProfile / ProfileConfig plumbing. Doesn't exercise the full
// MainWindow::applyProfile (that needs an ImGui + GLFW context) — but
// it locks the profile→CPU/iieMode/ROM-probe mapping so a future
// refactor can't silently lose a profile or swap CPU defaults.
//
// What this gates:
//   1. All 5 profiles (II / II+ / IIe / IIc / IIc+) are addressable
//      via `pom2::allProfiles()` and `pom2::profileConfig(...)`.
//   2. CPU defaults: II/II+ = NMOS, IIe/IIc/IIc+ = CMOS.
//   3. iieMode: II/II+ = false, IIe/IIc/IIc+ = true.
//   4. Profile key round-trip: `profileKey(p)` followed by
//      `profileFromKey(...)` returns the same profile.
//   5. Tolerant key aliases: `apple2` → II, `apple2plus` → II+,
//      `//e` → IIe, `//c` → IIc, `//c+` → IIc+.
//   6. Cold-reset cycle on Memory + M6502: simulate switching profiles
//      by toggling Memory::setIIEMode + M6502::setCpuMode without any
//      ROM loaded, verifying state stays consistent (no crash, no
//      lingering soft-switch flags from the previous profile).

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void testAllProfilesEnumerated()
{
    const auto& all = pom2::allProfiles();
    assert(all.size() == 5);
    assert(all[0] == pom2::SystemProfile::AppleII);
    assert(all[1] == pom2::SystemProfile::AppleIIPlus);
    assert(all[2] == pom2::SystemProfile::AppleIIe);
    assert(all[3] == pom2::SystemProfile::AppleIIc);
    assert(all[4] == pom2::SystemProfile::AppleIIcPlus);
}

void testProfileDefaults()
{
    // II / II+ are NMOS, no IIe paging.
    const auto& cII   = pom2::profileConfig(pom2::SystemProfile::AppleII);
    const auto& cIIp  = pom2::profileConfig(pom2::SystemProfile::AppleIIPlus);
    assert(cII.defaultCpu  == M6502::CpuMode::NMOS);
    assert(cIIp.defaultCpu == M6502::CpuMode::NMOS);
    assert(!cII.iieMode);
    assert(!cIIp.iieMode);

    // IIe / IIc / IIc+ are CMOS with IIe paging on.
    const auto& cIIe  = pom2::profileConfig(pom2::SystemProfile::AppleIIe);
    const auto& cIIc  = pom2::profileConfig(pom2::SystemProfile::AppleIIc);
    const auto& cIIcp = pom2::profileConfig(pom2::SystemProfile::AppleIIcPlus);
    assert(cIIe.defaultCpu  == M6502::CpuMode::CMOS);
    assert(cIIc.defaultCpu  == M6502::CpuMode::CMOS);
    assert(cIIcp.defaultCpu == M6502::CpuMode::CMOS);
    assert(cIIe.iieMode);
    assert(cIIc.iieMode);
    assert(cIIcp.iieMode);

    // Every profile has at least one ROM candidate and one charset
    // candidate, and a non-empty display name.
    for (auto p : pom2::allProfiles()) {
        const auto& c = pom2::profileConfig(p);
        assert(!c.romProbeOrder.empty());
        assert(!c.charRomProbeOrder.empty());
        assert(!c.displayName.empty());
        assert(c.defaultCyclesPerFrame > 0);
    }
}

void testKeyRoundTrip()
{
    for (auto p : pom2::allProfiles()) {
        const std::string_view k = pom2::profileKey(p);
        assert(!k.empty());
        const pom2::SystemProfile back = pom2::profileFromKey(k);
        assert(back == p);
    }
}

void testKeyAliases()
{
    assert(pom2::profileFromKey("apple2")       == pom2::SystemProfile::AppleII);
    assert(pom2::profileFromKey("appleii")      == pom2::SystemProfile::AppleII);
    assert(pom2::profileFromKey("ii")           == pom2::SystemProfile::AppleII);
    assert(pom2::profileFromKey("apple2plus")   == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("appleiiplus")  == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("ii+")          == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("iiplus")       == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("iie")          == pom2::SystemProfile::AppleIIe);
    assert(pom2::profileFromKey("apple2e")      == pom2::SystemProfile::AppleIIe);
    assert(pom2::profileFromKey("//e")          == pom2::SystemProfile::AppleIIe);
    assert(pom2::profileFromKey("iic")          == pom2::SystemProfile::AppleIIc);
    assert(pom2::profileFromKey("apple2c")      == pom2::SystemProfile::AppleIIc);
    assert(pom2::profileFromKey("//c")          == pom2::SystemProfile::AppleIIc);
    assert(pom2::profileFromKey("iic+")         == pom2::SystemProfile::AppleIIcPlus);
    assert(pom2::profileFromKey("apple2cplus")  == pom2::SystemProfile::AppleIIcPlus);
    assert(pom2::profileFromKey("appleiicplus") == pom2::SystemProfile::AppleIIcPlus);
    assert(pom2::profileFromKey("//c+")         == pom2::SystemProfile::AppleIIcPlus);

    // Empty / unknown falls back to II+.
    assert(pom2::profileFromKey("")          == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("foo")       == pom2::SystemProfile::AppleIIPlus);
    assert(pom2::profileFromKey("apple3")    == pom2::SystemProfile::AppleIIPlus);
}

// Simulate the core CPU+Memory state changes that `applyProfile` does,
// minus the GLFW/ImGui surface area. Confirms there's no state bleed
// when cycling profiles back-to-back.
void testProfileSwitchCycle()
{
    Memory  mem;
    M6502   cpu(&mem);

    // Walk the canonical cycle the user would do in the GUI menu.
    const pom2::SystemProfile cycle[] = {
        pom2::SystemProfile::AppleIIPlus,
        pom2::SystemProfile::AppleIIe,
        pom2::SystemProfile::AppleIIc,
        pom2::SystemProfile::AppleIIcPlus,
        pom2::SystemProfile::AppleII,
        pom2::SystemProfile::AppleIIPlus,
    };
    for (auto p : cycle) {
        const auto& cfg = pom2::profileConfig(p);
        // Equivalent of applyProfile core: clearRam → resetSoftSwitches
        // → setIIEMode → cpu.setCpuMode → cpu.hardReset.
        mem.clearRam();
        mem.resetSoftSwitches();
        mem.setIIEMode(cfg.iieMode);
        cpu.setCpuMode(cfg.defaultCpu);
        cpu.hardReset();
        // Verify the resulting state matches the profile config.
        assert(mem.isIIE() == cfg.iieMode);
        assert(cpu.getCpuMode() == cfg.defaultCpu);
    }
}

// 32 KB ROM bank-picking: //e dumps carry firmware in the UPPER 16 KB
// (char data lower), //c / //c+ dumps carry bank 0 in the LOWER 16 KB
// and bank 1 in the upper. Same file size, opposite slicing. Lock the
// loader behaviour so a future "simplification" can't silently swap them.
void test32kBankPicking()
{
    namespace fs = std::filesystem;
    // Build a 32 KB synthetic ROM with distinct reset vectors per half.
    //   lower 16K reset @0x3FFC = $FA62 (looks like //c bank 0 cold-start)
    //   upper 16K reset @0x7FFC = $C788 (looks like //c bank 1 alt)
    std::vector<uint8_t> rom(32 * 1024, 0);
    rom[0x3FFC] = 0x62; rom[0x3FFD] = 0xFA;
    rom[0x7FFC] = 0x88; rom[0x7FFD] = 0xC7;

    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_system_profile_smoke_32k.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }

    // //e layout (pickLower=false) — firmware = upper 16K → $FFFC reads
    // bank-1's reset vector.
    {
        Memory mem;
        mem.setIIEMode(true);
        const int rc = mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/false);
        assert(rc == 1);
        const uint16_t reset =
            mem.data()[0xFFFC] | (mem.data()[0xFFFD] << 8);
        assert(reset == 0xC788);
    }

    // //c layout (pickLower=true) — firmware = lower 16K → $FFFC reads
    // bank-0's reset vector. This is the path that fixes the //c no-boot.
    {
        Memory mem;
        mem.setIIEMode(true);
        const int rc = mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/true);
        assert(rc == 1);
        const uint16_t reset =
            mem.data()[0xFFFC] | (mem.data()[0xFFFD] << 8);
        assert(reset == 0xFA62);
    }

    fs::remove(tmp);
}

// Apple //c $C028 ROMBANK soft switch: with a //c-style 32 KB dump
// loaded (pickLower=true), reads or writes to $C028 toggle which 16 KB
// firmware bank is mapped at $D000-$FFFF (and $C100-$CFFF under INTCXROM).
// On //e-style dumps (pickLower=false), $C028 stays in the cassette
// fall-through path — no banking, no crash.
void testIicRomBankSwitch()
{
    namespace fs = std::filesystem;
    // Tag each half so we can identify which bank is currently visible
    // at $D000 just by reading a single byte. Bank 0 byte at $D000 →
    // file offset 0x1000 = 0xB0; bank 1 → file offset 0x5000 = 0xB1.
    // Reset vectors stay distinct so the loader still distinguishes
    // the layouts.
    std::vector<uint8_t> rom(32 * 1024, 0);
    rom[0x1000] = 0xB0;   // bank 0 marker at $D000
    rom[0x5000] = 0xB1;   // bank 1 marker at $D000
    rom[0x3FFC] = 0x62; rom[0x3FFD] = 0xFA;
    rom[0x7FFC] = 0x88; rom[0x7FFD] = 0xC7;

    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_system_profile_smoke_iic_bank.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }

    // //c layout — bank 0 visible at reset.
    {
        Memory mem;
        mem.setIIEMode(true);
        assert(mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/true));
        // Initial bank = 0: $D000 reads bank-0 marker.
        assert(mem.memRead(0xD000) == 0xB0);

        // $C028 toggles via read.
        (void)mem.memRead(0xC028);
        assert(mem.memRead(0xD000) == 0xB1);

        // $C028 toggles via write too.
        mem.memWrite(0xC028, 0x00);
        assert(mem.memRead(0xD000) == 0xB0);

        // resetSoftSwitches() restores bank 0 (cold-start invariant).
        (void)mem.memRead(0xC028);                 // flip to 1
        assert(mem.memRead(0xD000) == 0xB1);
        mem.resetSoftSwitches();
        assert(mem.memRead(0xD000) == 0xB0);
    }

    // //e layout — same file but loaded the other way. $C028 must NOT
    // bank-switch (no alt bank stashed); reads of $D000 always return
    // the same upper-half byte regardless of $C028 access count.
    {
        Memory mem;
        mem.setIIEMode(true);
        assert(mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/false));
        const uint8_t before = mem.memRead(0xD000);
        (void)mem.memRead(0xC028);
        mem.memWrite(0xC028, 0xFF);
        (void)mem.memRead(0xC028);
        const uint8_t after  = mem.memRead(0xD000);
        assert(before == after);
    }

    fs::remove(tmp);
}

// //c hardware doesn't have physical slots — the internal motherboard
// ROM is mapped at $C100-$CFFF regardless of the INTCXROM softswitch.
// MAME `apple2e.cpp:1617-1635` (`update_slotrom_banks`) ORs `m_isiic`
// into every internal-ROM gate; the //c reset routine at $FA62 calls
// `JSR $CE4D` etc. on the very first instructions, so without this
// override the //c boots into garbage from an empty slot bus.
//
// This test plants distinctive sentinels in the //c lower-bank's
// $C100-$CFFF region (via the loader's internalIORom slice) and
// verifies that reads of those addresses come back from the internal
// ROM at cold-boot time with INTCXROM still cleared in iieMemMode.
void testIicInternalRomAlwaysMapped()
{
    namespace fs = std::filesystem;
    std::vector<uint8_t> rom(32 * 1024, 0);
    // Lower bank $C100-$CFFF area → file offsets 0x100-0xFFF; load a
    // recognisable pattern. internalIORom[0xE4D] (the //c JSR $CE4D
    // landing site) gets 0x4D; we'll read $CE4D and expect 0x4D.
    rom[0x0E4D] = 0x4D;
    rom[0x0C04] = 0xC4;   // $CC04 — second //c reset JSR target
    rom[0x0740] = 0x40;   // $C740 — //c+ reset JSR target
    // $C800-$CFFF on //c also lives in internal ROM (MAME c800_int_r
    // returns from rom+0x800). Plant a marker at $C8AB.
    rom[0x08AB] = 0x88;
    // Reset vector — required for the loader's IIe split path.
    rom[0x3FFC] = 0x62; rom[0x3FFD] = 0xFA;
    rom[0x7FFC] = 0x88; rom[0x7FFD] = 0xC7;
    // Upper bank: distinct sentinels so we can tell ROMBANK works too.
    rom[0x4E4D] = 0xAA;
    rom[0x48AB] = 0xBB;

    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_system_profile_smoke_iic_intcxrom.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }

    Memory mem;
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/true));

    // The //c boot routine at $FA62 immediately calls $CE4D. Reads of
    // $C100-$CFFF MUST come from internalIORom even though iieMemMode
    // is freshly zero at the constructor (loadAppleIIRom sets
    // MF_INTCXROM on //c — separate assertion below).
    assert(mem.memRead(0xCE4D) == 0x4D);  // //c JSR target
    assert(mem.memRead(0xCC04) == 0xC4);  // //c JSR target
    assert(mem.memRead(0xC740) == 0x40);  // //c+ JSR target
    assert(mem.memRead(0xC8AB) == 0x88);  // $C800-$CFFF on //c too

    // INTCXROM softswitch is forced on at //c load (MAME apple2e.cpp:1273
    // does the same in machine_reset). $C015 (RDCXROM) returns bit 7
    // when INTCXROM is set.
    assert((mem.memRead(0xC015) & 0x80) != 0);

    // Sanity: after explicitly clearing INTCXROM via $C006 (which
    // doesn't actually exist on the IIe but matches the softswitch
    // wire), the //c override must STILL keep internal ROM mapped.
    // We simulate this by stomping iieMemMode through resetSoftSwitches
    // and re-checking — the resetSoftSwitches hook re-asserts INTCXROM
    // for //c.
    mem.resetSoftSwitches();
    assert((mem.memRead(0xC015) & 0x80) != 0);
    assert(mem.memRead(0xCE4D) == 0x4D);

    // ROMBANK toggle still works: flip $C028, $CE4D now reads from
    // the upper bank's marker (0xAA).
    (void)mem.memRead(0xC028);
    assert(mem.memRead(0xCE4D) == 0xAA);
    assert(mem.memRead(0xC8AB) == 0xBB);

    // For comparison: //e layout (pickLower=false). iicHasAltBank
    // stays false; INTCXROM remains 0 at reset; reads of $CE4D fall
    // through to the slot bus (returns 0 from an unplugged slot bus).
    {
        Memory iie;
        iie.setIIEMode(true);
        assert(iie.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/false));
        assert((iie.memRead(0xC015) & 0x80) == 0);  // INTCXROM off
        assert(iie.memRead(0xCE4D) != 0x4D);        // not from internal
    }

    fs::remove(tmp);
}

// 20 KB Apple II+ ROM dumps (common MAME pack format) have 4 KB of
// leading filler followed by the real 16 KB $C000-$FFFF firmware. The
// old "best effort" loader landed loadAddr at $B000, clobbering user
// RAM with filler bytes. Verify the new path skips the filler and
// lands the reset vector and main ROM bytes where they belong.
void test20kIIPlusRomLoad()
{
    namespace fs = std::filesystem;
    std::vector<uint8_t> rom(20 * 1024, 0);
    // 4 KB of filler — distinctive non-zero values so we can detect a
    // regression that loaded them into user RAM at $B000-$BFFF.
    for (size_t i = 0; i < 0x1000; ++i) rom[i] = 0xEE;
    // High 16 KB at file offset 0x1000 onwards is the "real" $C000-$FFFF
    // firmware. We'll plant a $F000 Applesoft marker (file 0x1000 +
    // $F000-$C000 = 0x4000) and the reset vector at $FFFC (file 0x1000 +
    // $FFFC-$C000 = 0x4FFC).
    rom[0x4000] = 0x6F;                 // $F000 marker
    rom[0x4FFC] = 0x62; rom[0x4FFD] = 0xFA;  // reset = $FA62

    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_system_profile_smoke_iiplus_20k.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }

    Memory mem;
    assert(mem.loadAppleIIRom(tmp.string().c_str()));

    // Reset vector landed at $FFFC = $FA62 (Autostart entry).
    assert(mem.memRead(0xFFFC) == 0x62);
    assert(mem.memRead(0xFFFD) == 0xFA);
    // Applesoft marker landed at $F000.
    assert(mem.memRead(0xF000) == 0x6F);
    // User RAM at $B000-$BFFF stays clean — was NOT clobbered by the
    // leading-filler bytes. (The previous "best effort" branch would
    // have written 0xEE here.)
    assert(mem.memRead(0xB000) == 0x00);
    assert(mem.memRead(0xB800) == 0x00);
    assert(mem.memRead(0xBFFF) == 0x00);

    fs::remove(tmp);
}

}  // namespace

int main()
{
    testAllProfilesEnumerated();
    std::printf("  ok: all 5 profiles enumerated\n");

    testProfileDefaults();
    std::printf("  ok: profile CPU/iieMode defaults match expectations\n");

    testKeyRoundTrip();
    std::printf("  ok: profileKey ⇄ profileFromKey round-trip\n");

    testKeyAliases();
    std::printf("  ok: tolerant key aliases (apple2 / //e / iic+ / etc.)\n");

    testProfileSwitchCycle();
    std::printf("  ok: 6-step profile cycle (II+ → IIe → IIc → IIc+ → II → II+)\n");

    test32kBankPicking();
    std::printf("  ok: 32 KB ROM bank-picking (//e upper / //c lower)\n");

    testIicRomBankSwitch();
    std::printf("  ok: //c $C028 ROMBANK toggles firmware bank\n");

    testIicInternalRomAlwaysMapped();
    std::printf("  ok: //c internal ROM mapped at $C100-$CFFF regardless of INTCXROM\n");

    test20kIIPlusRomLoad();
    std::printf("  ok: 20 KB II+ ROM dump loads at $C000 (skips leading filler)\n");

    std::printf("OK system_profile_smoke\n");
    return 0;
}
