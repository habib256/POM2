// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskIICard — Apple Disk II Interface card (slot 6 by convention).
// Two-drive controller, 16-sector DOS 3.3 (.dsk / .do) and ProDOS-order
// (.po) images, plus raw .nib. Read-back via the bit-level LSS (default)
// or the legacy 32-cycle gate (fallback when no P6 PROM is loaded).
//
// Soft-switch map (slot N at $C080+N*16; for slot 6 → $C0E0-$C0EF):
//
//   $C0n0/n1   Phase 0 off / on    head stepper coil 0
//   $C0n2/n3   Phase 1 off / on
//   $C0n4/n5   Phase 2 off / on
//   $C0n6/n7   Phase 3 off / on
//   $C0n8      Drive (motor) off
//   $C0n9      Drive (motor) on
//   $C0nA      Select drive 1   (activeDrive ← 0 → uses images[0])
//   $C0nB      Select drive 2   (activeDrive ← 1 → uses images[1])
//   $C0nC      Q6L  — shift / read data register
//   $C0nD      Q6H  — load / write-protect probe
//   $C0nE      Q7L  — read mode
//   $C0nF      Q7H  — write mode (we acknowledge but never alter media)
//
// Each drive owns its own DiskImage, head position (in quarter-tracks),
// and nibble-buffer cursor. Phase magnet energization is controller
// state and shared between the two drives — same as real hardware,
// where only the selected drive's head responds to phase pulses.
// Motor on/off is also a single controller signal; in real hardware it
// reaches the selected drive only, but POM2 models it as global since
// a stopped drive sees no LSS activity anyway (active==MODE_IDLE skips
// sync).
//
// Slot ROM ($Cs00-$CsFF, s=6 → $C600-$C6FF) is the Apple-Disk-II "P5A"
// 256-byte boot PROM. The PROM autodetects its slot via the standard
// JSR-$FF58 / TSX / LDA $0100,X trick, then steps the head to track 0,
// reads the first sector via the address-field/data-field state machine,
// and JMPs to the loaded boot1 at $0801.
//
// Head stepping: real Disk II hardware pulls the head magnet coils in a
// 4-phase rotational pattern. The head's mechanical position is at any
// quarter-track offset; each phase magnet has a "well" at a quarter-track
// position whose index (mod 4) matches the phase number. With one magnet
// energized the head settles at that magnet's well; with two adjacent
// magnets energized the head settles between them; opposing magnets
// (0+2 or 1+3) cancel and the head holds. State is held in
// *quarter-tracks* (0..139 = 35 tracks × 4) so disks with quarter-track
// copy protection step accurately. The same algorithm is what MAME's
// `apple2_floppy_image_device` uses (see its phase-to-target lookup).
//
// Timing: at 1.0227 MHz with 4 µs bit cells, the LSS shift register
// outputs one nibble every ~32 CPU cycles. We accumulate cycles via
// advanceCycles() and step the track-buffer cursor accordingly. Reads of
// $C0nC return the current nibble; bit-7 is implicitly always set
// because every valid GCR nibble has it set.

#ifndef POM2_DISK_II_CARD_H
#define POM2_DISK_II_CARD_H

#include "DiskImage.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class M6502;
class FloppySoundSink;

class DiskIICard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 6;

    /// Construct with the slot number this card will be plugged into.
    /// The Disk II boot PROM is slot-agnostic at runtime — the P5A PROM
    /// auto-detects its own slot via the standard `JSR $FF58 / TSX /
    /// LDA $0100,X` trick — so the slot number is held only for
    /// diagnostics / UI display.
    explicit DiskIICard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// Inject the host CPU pointer so the LSS can resolve the precise
    /// sub-instruction cycle of every $C0EX MMIO access. Without this,
    /// the LSS treats every MMIO access as happening at the START of the
    /// instruction, when in reality (per MAME's per-cycle state machine)
    /// the data fetch of `LDA $C0EC` happens on the 4th cycle (sub-cycle
    /// 3) and the LSS state at that exact moment is what software sees.
    /// Cycle-precise copy-protection schemes (Spiradisc, Locksmith, some
    /// Sirius / Sierra titles) rely on this. Stock DOS / ProDOS RWTS
    /// doesn't notice the difference. Safe to leave unset (nullptr) —
    /// behaviour falls back to instruction-aligned access timing.
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Inject the mechanical-sound source (head step, motor spin-up/down,
    /// disk insert/eject click). Optional — when nullptr the card is
    /// silent on the floppy-sound side (the data path is unchanged).
    /// Sound calls are sparse (mutex-guarded command queue inside the
    /// source) so this stays cheap even on read-heavy workloads.
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    /// Force every drive's head back to track 0 and reset the LSS state.
    /// Used by the "Boot disk" UI shortcut so the boot PROM finds
    /// D5 AA 96 quickly even if a head wandered while waiting for a disk
    /// insert.
    void seekTrack0() {
        for (int d = 0; d < kDriveCount; ++d) {
            headQuarterTrack[d] = 0;
            trackPos[d]         = 0;
        }
        cycleAccum = 0;
    }

    /// Load the 256-byte Disk II boot PROM from disk. Must succeed before
    /// the card is useful — without the PROM, $C600-$C6FF reads back
    /// $FF and `PR#6` jumps into nothing.
    bool loadBootRom(const std::string& path);
    bool hasBootRom() const { return bootRomLoaded; }

    /// Load the 256-byte Disk II Logic State Sequencer PROM (Apple part
    /// 341-0028-A, "P6"). When loaded, the card switches to a cycle-
    /// accurate bit-level LSS that drives Q6/Q7 and the data register
    /// per the PROM's state-table — a faithful port of MAME's
    /// `wozfdc.cpp lss_sync()`. Without this PROM, reads fall back to
    /// the simplified 32-cycle byteReady gate (which is good enough for
    /// stock DOS 3.3 / ProDOS RWTS but not for software like Copy II
    /// Plus that scans for sync-gap-shifted byte boundaries).
    bool loadLssRom(const std::string& path);
    bool hasLssRom() const { return p6RomLoaded; }

    /// Number of drives the controller models. The Disk II Interface
    /// has two slots in the daisy chain (drive 1 = images[0], drive 2 =
    /// images[1]).
    static constexpr int kDriveCount = 2;

    /// Insert / eject a disk image. The single-arg variants target
    /// drive 1 (= index 0) for backwards compatibility with existing UI
    /// and test call sites; pass an explicit drive index (0 or 1) for
    /// the two-arg form.
    bool insertDisk(int drive, const std::string& path);
    bool insertDisk(const std::string& path) { return insertDisk(0, path); }
    void ejectDisk(int drive);
    void ejectDisk() { ejectDisk(0); }

    bool isDiskLoaded(int drive = 0) const { return images[drive].isLoaded(); }
    const std::string& getDiskPath (int drive = 0) const { return images[drive].getPath(); }
    const std::string& getLastError(int drive = 0) const { return images[drive].getLastError(); }

    int  getCurrentTrack(int drive = 0) const { return headQuarterTrack[drive] / 4; }
    int  getHalfTrack   (int drive = 0) const { return headQuarterTrack[drive] / 2; }
    int  getQuarterTrack(int drive = 0) const { return headQuarterTrack[drive]; }
    bool isMotorOn() const { return motorOn; }
    /// Index of the drive currently selected by the most recent
    /// $C0nA / $C0nB access (0 = drive 1, 1 = drive 2).
    int  getActiveDrive() const { return activeDrive; }
    int  getTrackPosition(int drive = 0) const { return trackPos[drive]; }
    bool hasUnsavedChanges(int drive = 0) const { return images[drive].hasUnsavedChanges(); }
    /// Total nibble write flushes (across both drives) since last reset.
    /// Used by the dos33_save smoke test to confirm SAVE actually
    /// exercised the write pipeline (vs. erroring out before any write).
    uint64_t getWriteFlushCount() const { return writeFlushCount; }

    /// User opt-in for write-back. When true, eject (and explicit save)
    /// rewrites the source file with any modified sectors. Default off
    /// to avoid silently mutating the user's image file. The toggle is
    /// card-wide and applies to both drives.
    void setWriteBackEnabled(bool on) {
        for (int d = 0; d < kDriveCount; ++d) images[d].setWriteBackEnabled(on);
        writeBackEnabled = on;
    }
    bool isWriteBackEnabled() const { return writeBackEnabled; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Disk II"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;

private:
    int slot_;
    /// Optional CPU pointer for sub-instruction cycle resolution at MMIO
    /// access points. See `setCpu()` doc above.
    M6502* cpu_ = nullptr;
    FloppySoundSink* sound_ = nullptr;
    std::array<DiskImage, kDriveCount> images{};
    /// Drive currently routed to the LSS / legacy gate. Set by control()
    /// in response to $C0nA ($activeDrive=0) or $C0nB ($activeDrive=1).
    /// Persists across onReset() — matches the 74LS259 latch on real
    /// hardware which is not cleared by 6502 RES.
    int activeDrive = 0;
    std::array<uint8_t, 256> bootRom{};
    bool bootRomLoaded = false;

    bool motorOn   = false;
    // MAME `wozfdc_device::active` — MODE_IDLE / MODE_ACTIVE / MODE_DELAY.
    // MODE_DELAY: a motor-off ($C0E8) command does NOT stop the drive
    // immediately; `delay_timer` holds the LSS active for ~1 second of
    // CPU cycles before the head transitions to MODE_IDLE. A fresh
    // motor-on ($C0E9) during the delay cancels it (back to MODE_ACTIVE).
    enum ActiveMode { MODE_IDLE = 0, MODE_ACTIVE, MODE_DELAY };
    ActiveMode active = MODE_IDLE;
    int  motorOffDelay = 0;
    bool writeMode = false;     // Q7 latch: false=read, true=write
    bool loadMode  = false;     // Q6 latch: false=shift, true=load
    bool writeBackEnabled = false;     // forwarded to DiskImage on toggle
    uint8_t writeLatch = 0xFF;         // latched data nibble for next bit-cell flush

    // Head stepper. phaseOn[i] = magnet i currently energized. The phase
    // signals are controller state and shared between the two drives —
    // only the selected drive's head physically moves in response.
    std::array<bool, 4> phaseOn{};
    // Head position in quarter-tracks, per drive. 35 tracks × 4 qt = 140;
    // the head can sit at any qt from 0 (track 0) to 4*(kTracks-1) = 136
    // (track 34). Quarter-tracks are needed for some copy protections;
    // the standard DOS 3.3 / ProDOS skew uses whole tracks (qt mod 4 == 0).
    int headQuarterTrack[kDriveCount] = {0, 0};

    // Position into the active drive's track nibble buffer (0..6655).
    // Wraps continuously while the motor is on. Used by the legacy
    // 32-cycle gate and the LSS write path (where a complete write
    // nibble lands at trackPos[activeDrive]).
    int trackPos[kDriveCount] = {0, 0};
    int cycleAccum = 0;       // CPU cycles since the last nibble advance

    // LSS shift-register model (legacy 32-cycle gate). While a new
    // nibble is assembling, the data register's bit 7 reads as 0
    // (intermediate shift state), so the host CPU's `LDA $C0EC ; BPL
    // loop` busy-wait holds until the full byte is in.
    uint8_t dataLatch = 0;
    bool    byteReady = false;

    // ── Bit-level LSS state (when useBitLss == true) ─────────────────
    //
    // Port of the Apple Disk II Logic State Sequencer, faithful to MAME's
    // `wozfdc.cpp lss_sync()`. The 256-byte P6 PROM (Apple part 341-0028-A)
    // is indexed by a persistent 8-bit address register whose bits are:
    //
    //   bit 7  state[3]  (= prev opcode bit 7)
    //   bit 6  state[2]  (= prev opcode bit 6)
    //   bit 5  state[0]  (= prev opcode bit 4)  ← scrambled with state[1]
    //   bit 4  !PULSE    (no flux transition this sub-cell)
    //   bit 3  Q7        (mode_write — 0=read, 1=write)
    //   bit 2  Q6        (mode_load — 0=shift, 1=load)
    //   bit 1  QA        (data register MSB, sampled AFTER each opcode)
    //   bit 0  state[1]  (= prev opcode bit 5)  ← scrambled with state[0]
    //
    // The PROM byte read at that address is the opcode for the current
    // LSS tick. Its high nibble becomes the *new* state (re-scrambled into
    // address bits 7,6,5,0 via MAME's exact pack: `(opcode & 0xC0) |
    // ((opcode & 0x20) >> 5) | ((opcode & 0x10) << 1)`). Its low nibble is
    // the ALU op on the data register, dispatched per MAME's full range:
    //
    //     0x0..0x7      CLR  data ← 0
    //     0x8, 0xC      NOP
    //     0x9           SL0  data ← (data << 1)
    //     0xA, 0xE      SR   data ← (data >> 1) | (write-protect ? 0x80 : 0)
    //     0xB, 0xF      LD   data ← writeLatch (CPU bus)
    //     0xD           SL1  data ← (data << 1) | 1
    //
    // In write mode, bit 7 of the new address (= opcode bit 7 = new
    // state[3]) is the WRITE_DATA bit driven onto the disk surface for
    // the next cell.
    //
    // The PROM bytes embedded as kP6RomDefault[] are pre-permuted into
    // MAME's address layout by scripts/permute_p6_rom.py from the
    // apple2js `SEQUENCER_ROM_16` table; same 256 logical bytes, indexed
    // differently. roms/diskii_p6.rom on disk is also MAME-layout.
    std::array<uint8_t, 256> p6Rom{};
    bool    p6RomLoaded = false;
    bool    useBitLss   = false;        // false → legacy 32-cycle gate
    uint8_t address     = 0x10;         // MAME's persistent LSS address reg
    uint8_t lssData     = 0;            // 8-bit shift / data register

    // ── MAME wozfdc cycle bookkeeping ───────────────────────────────────
    //
    // MAME's `wozfdc_device::cycles` is the absolute LSS-cycle counter
    // (clock = 2× CPU clock; one PROM lookup per LSS cycle). The CPU side
    // calls `lss_sync()` whose loop runs from `cycles` up to a `cycles_limit`
    // computed from `time_to_cycles(machine().time())`. POM2 doesn't have
    // a global emu time; we recover the equivalent via `cpuCycleTotal` —
    // a running CPU cycle counter that `advanceCycles(n)` bumps before
    // calling `lss_sync(0)`. The LSS limit is `cpuCycleTotal*2 + extra`.
    //
    // This is a verbatim port of MAME's algorithm; only the time-base
    // substrate changes (LSS cycles directly, instead of attotime
    // converted via clock()*2).
    uint64_t lssCycle       = 0;
    uint64_t cpuCycleTotal  = 0;

    // MAME `floppy_image_device::m_revolution_start_time`, one per drive.
    // Set to the current `lssCycle` on a motor-on transition (MAME's
    // `mon_w(false)` — fires from `control() case 0x9 MODE_IDLE→ACTIVE`
    // and on the *new* drive in `selectDrive()` when the controller is
    // already spinning). Kept `kNeverRev` while the drive's motor is off
    // (MAME stores `attotime::never`). The disk angular position is
    // `(lssCycle - revolutionStartLssCycle[d]) mod track_period_lsscycles`,
    // computed inside `DiskImage::getNextTransition`.
    //
    // Why per-drive: MAME's `revolution_start_time` lives on the
    // `floppy_image_device`, not on the `wozfdc_device`. When the user
    // does `$C0EA / $C0EB` to switch drives mid-spin, each drive's disk
    // remembers its own angular position; the previously-selected drive's
    // disk does NOT freeze when the controller looks away. POM2 used to
    // collapse this into the single global `lssCycle`, which fell apart
    // when a head step on the active drive made the new track's period
    // differ from the old one (the modulo wrap suddenly snapped angular
    // position to a stale slot). See `DiskImage::getNextTransition` for
    // the angular-position computation.
    static constexpr int64_t kNeverRev = -1;
    int64_t revolutionStartLssCycle[kDriveCount] = { kNeverRev, kNeverRev };

    // MAME `write_buffer[32]` — flux event timestamps (LSS cycles)
    // captured from WRITE_DATA edge transitions during the active write
    // session. Flushed via `image.writeFlux(...)` either on Q7 falling
    // edge (control() $C0nE) or pre-emptively when write_position ≥ 30
    // (avoids ever hitting the assert at 32, mirroring MAME's flush
    // condition exactly).
    static constexpr int  kWriteBufferSize = 32;
    int64_t  writeBuffer[kWriteBufferSize] = {};
    int      writePosition  = 0;
    int64_t  writeStartTime = 0;       // LSS cycle of last splice start
    bool     writeLineActive = false;  // tracks WRITE_DATA edge state

    uint64_t writeFlushCount = 0;

    void handleSwitchAccess(uint8_t low4);
    /// MAME `floppy_image_device::seek_phase_w`: settle the head into the
    /// well of the current 4-bit magnet pattern, capped at ±4 quarter-
    /// tracks per call. Called from handleSwitchAccess after every phase
    /// soft-switch hit (rising AND falling).
    void seekPhaseW(int phases);
    // Legacy 32-cycle gate body, retained as a fallback when no P6 PROM
    // is loaded.
    void legacyAdvance(int cycles);

    // ── MAME wozfdc API ─────────────────────────────────────────────────
    //
    // Verbatim ports of `wozfdc_device::lss_start()`, `lss_sync(extra)`,
    // and `control(offset)`. Together these implement the full Disk II
    // controller behaviour as MAME models it: per-LSS-cycle PROM lookup,
    // per-cycle WRITE_DATA toggle detection, and flux event splicing on
    // mode transitions.
    void lssStart();
    void lssSync(uint64_t extraCycles = 0);
    void control(int offset);

public:
    /// Debug-only: dump the last N $C0EC reads with their cycle stamp +
    /// byte value to stderr. Intended for `hero_probe` / boot-dump
    /// diagnostics — set `POM2_DEBUG_DISK_READS=1` to enable capture.
    void dumpRecentReads(size_t maxEntries = 256) const;
private:
    // Ring of (cpuCycleTotal, lssData, qt) at each $C0EC read.
    // Populated only when `POM2_DEBUG_DISK_READS=1`.
    struct ReadLog {
        uint64_t cycle;
        uint8_t  data;
        uint8_t  qt;
    };
    mutable std::vector<ReadLog> readLog_;
    mutable size_t               readLogCursor_ = 0;
    static constexpr size_t      kReadLogCap    = 4096;

    /// MAME `wozfdc_device::control()` cases 0xa/0xb — drive_select
    /// switch. When the motor is on (`active != MODE_IDLE`), MAME calls
    /// `floppy->mon_w(true)` on the old drive (commits in-flight writes
    /// and freezes the flux cursor) and `floppy->mon_w(false)` on the
    /// new drive (sets `revolution_start_time = now`, so the new drive
    /// resumes reading from position 0 of its current track). POM2's
    /// equivalent: flush the write buffer to the old drive, then reset
    /// the LSS-cycle base so the new drive's flux stream starts fresh.
    /// No-op if `newDrive == activeDrive`.
    void selectDrive(int newDrive);

    // Boot-trace one-shot flags (POM2_DEBUG_DISK=1). Reset at onReset().
    struct {
        bool sawSlotRom     = false;
        bool sawDevSelect   = false;
        bool sawMotorOn     = false;
        bool sawFirstNibble = false;
    } trace;
};

#endif // POM2_DISK_II_CARD_H
