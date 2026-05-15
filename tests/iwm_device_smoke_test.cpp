// Smoke test for IWMDevice — the MAME-faithful port at IWMDevice.{h,cpp}.
// Verifies the surface that's directly exercised by the Apple //c+ alt
// firmware cold-reset path:
//   1. Reset state matches MAME `iwm.cpp:48-69`.
//   2. Mode register handshake via $C0nF write + $C0nE read returns
//      the mode low-5-bits in the status register (MAME `iwm.cpp:259`).
//   3. Write-handshake register reads $0xBF (bit 7 ready, bit 6 clear)
//      from a fresh cold reset (MAME `iwm.cpp:57`).
//   4. control_ tracks Q6/Q7/motor/drive-select bits via even/odd
//      offset decode (MAME `iwm.cpp:148-158`).
//   5. Sync state machine is callable as a no-op when inactive (MAME
//      `iwm.cpp:337-338 sync` returns immediately when !m_active).
//
// The bit-shift transitions (SR_/SW_ states) are not exercised here —
// they need a real flux source from DiskImage and would belong in a
// dedicated flux-replay smoke test.

#include "IWMDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using pom2::IWMDevice;

void testResetState()
{
    IWMDevice iwm;
    assert(iwm.mode()    == 0x00);
    assert(iwm.status()  == 0x00);
    assert(iwm.whd()     == 0xBF);
    assert(iwm.control() == 0x00);
    assert(iwm.data()    == 0x00);
    std::printf("  ok: cold reset state matches MAME iwm.cpp:48-69\n");
}

void testControlBits()
{
    IWMDevice iwm;
    // Offsets 8..15 toggle control bits 0..7 (even=clear, odd=set).
    iwm.write(0x9, 0);              // set bit 4 (motor enable)
    assert((iwm.control() & 0x10) != 0);
    iwm.write(0xB, 0);              // set bit 5 (drive 2 select)
    assert((iwm.control() & 0x20) != 0);
    iwm.write(0xD, 0);              // set bit 6 (Q6)
    assert((iwm.control() & 0x40) != 0);
    iwm.write(0xF, 0);              // set bit 7 (Q7)
    assert((iwm.control() & 0x80) != 0);
    iwm.write(0x8, 0);              // clear bit 4
    assert((iwm.control() & 0x10) == 0);
    std::printf("  ok: control register bit decode (MAME iwm.cpp:148-158)\n");
}

void testModeStatusEcho()
{
    IWMDevice iwm;
    // Step 1: motor on so write_mode_w fires when offset & 1.
    // Without active state the write goes through mode_w (inactive
    // path) — that's exactly what the //c+ alt firmware does at
    // $C0EF when probing the IWM (before any drive is engaged).
    iwm.write(0xD, 0);              // Q6 set
    iwm.write(0xF, 0x1F);           // mode_w(0x1F) — control 0xC0 + odd
    // Low 5 of status reflects low 5 of mode (MAME line 259).
    assert((iwm.status() & 0x1F) == 0x1F);
    // Read $C0nE with Q6 high → control 0x40 → returns status.
    // The IWM read also pre-runs control() which clears Q7 (offset 0xE
    // is even, low 4 bit = bit 7 of control → 0). Status low 5 must
    // still be the mode echo.
    const uint8_t v = iwm.read(0xE);
    assert((v & 0x1F) == 0x1F);
    // WPT bit 7 high (no disk loaded → write-protected by default in
    // POM2 IWM read).
    assert((v & 0x80) != 0);
    std::printf("  ok: mode_w → status echo via $C0nF / $C0nE (MAME iwm.cpp:248-269)\n");
}

void testWhdReadIdle()
{
    IWMDevice iwm;
    // Read $C0nC with Q7 high + Q6 low → control 0x80 → returns whd.
    // Cold value is 0xBF; the //c+ alt firmware leans on bit 7 = 1
    // ("ready") to make its boot-time IWM init proceed.
    iwm.write(0xF, 0);              // Q7 set (write mode)
    iwm.write(0xC, 0);              // Q6 clear (this read clears Q6)
    const uint8_t v = iwm.read(0xC);
    assert(v == 0xBF);
    std::printf("  ok: whd cold read = 0xBF (MAME iwm.cpp:57 / read case 0x80)\n");
}

void testSyncNoCrashWhenInactive()
{
    IWMDevice iwm;
    // sync() must early-return when m_active is IDLE (MAME line 337-338).
    // No disk attached → still safe.
    iwm.tick(1'000'000);
    iwm.sync(1'000'000);
    iwm.tick(2'000'000);
    iwm.sync(2'000'000);
    // No assertion needed — we're checking that the call doesn't
    // segfault, divide by zero, or otherwise mis-step.
    std::printf("  ok: sync() inactive path is safe\n");
}

#include "Memory.h"

void testMemoryMirror()
{
    // Memory's $C0E0-$C0EF mirror only fires on `iicHasAltBank` (//c
    // 32K or //c+ profile). Build a synthetic 32K ROM with a valid
    // reset vector so `loadAppleIIRom(pickLower=true)` flips
    // iicHasAltBank = true, then verify that writes to $C0EF reach
    // the IWM mode register.
    Memory mem;
    IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIIEMode(true);

    // Minimal 32K //c-style image: reset vector at $FFFC = $FA62.
    namespace fs = std::filesystem;
    std::vector<uint8_t> rom(32 * 1024, 0);
    rom[0x3FFC] = 0x62; rom[0x3FFD] = 0xFA;
    rom[0x7FFC] = 0x88; rom[0x7FFD] = 0xC7;
    const fs::path tmp = fs::temp_directory_path() /
                         "pom2_iwm_mirror_smoke.rom";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(rom.data()),
                static_cast<std::streamsize>(rom.size()));
    }
    assert(mem.loadAppleIIRom(tmp.string().c_str(), /*pickLower=*/true));
    fs::remove(tmp);

    // Simulate the //c+ alt firmware's IWM mode probe:
    //   1. set Q6 (read $C0ED)
    //   2. write mode register: STA $C0EF, value = $1F
    //   3. read status via $C0EE — low 5 bits should echo $1F
    (void)mem.memRead(0xC0ED);              // Q6 on, loadMode tracking in IWM
    mem.memWrite(0xC0EF, 0x1F);             // mode_w(0x1F)
    const uint8_t status = mem.memRead(0xC0EE);
    // The Memory mirror only runs the IWM in shadow — the value
    // returned to the CPU is still from the slot bus, which has no
    // DiskII in this minimal harness, so `status` here is the slot
    // bus's "no card" floating-bus value (0x00). But the IWM should
    // have seen the mode write — inspect via the getter.
    (void)status;
    assert((iwm.mode() & 0x1F) == 0x1F);
    assert((iwm.status() & 0x1F) == 0x1F);
    std::printf("  ok: Memory $C0E0-$C0EF mirror reaches IWMDevice on //c+ profile\n");
}

int main()
{
    testResetState();
    testControlBits();
    testModeStatusEcho();
    testWhdReadIdle();
    testSyncNoCrashWhenInactive();
    testMemoryMirror();
    std::printf("OK iwm_device_smoke\n");
    return 0;
}
