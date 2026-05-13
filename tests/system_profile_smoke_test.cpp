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
#include <cstring>
#include <string>

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

    std::printf("OK system_profile_smoke\n");
    return 0;
}
