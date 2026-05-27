// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortCard — Apple II expansion card that gives a //e (or II+) access
// to ProDOS block-level disks the same way the //c+ on-board SmartPort
// does. Modelled after the real "Apple II 3.5 Disk Controller Card" (a.k.a.
// the "Liron" card, Apple part 670-0186), slot 5 by convention.
//
// Architecture choice — block-level vs IWM-level:
//
// The real card carries a full IWM plus a tiny 6502 ROM with the SmartPort
// dispatcher; ProDOS calls the dispatcher and the dispatcher talks GCR to
// whichever drive the user clicked. POM2's //c+ profile already emulates
// that full stack (`IWMDevice`, `SmartPortHub`, `Sony35Drive`). For a slot-
// plugged card on //e we don't need the bit-level emulation — no game does
// flux-level tricks through the Liron — so this class skips the IWM and
// exposes the underlying volume blocks directly through a streaming
// protocol identical to `ProDOSHardDiskCard`'s.
//
// Per-unit polymorphism:
//
// The original v0.5 incarnation hardcoded two `Disk35Image*` slots. This
// design replaces them with a `std::array<unique_ptr<SmartPortUnit>, kMaxUnits>`
// where each slot can hold any concrete `SmartPortUnit` (3.5", HDV, future
// types). ProDOS sees the first two units as drive 1 / drive 2 of slot N;
// units 3+ are reserved for a future extended-SmartPort hookup and are
// inert today (kMaxUnits = 2 for v1).
//
//   $C0n0 write = drive select (bits 0-1 = unit index, 0..kMaxUnits-1)
//   $C0n1 write = block LO byte (selected unit)
//   $C0n2 write = block HI byte (selected unit)
//   $C0n3 read  = next byte of selected block (auto-incrementing)
//   $C0n3 write = next byte INTO selected block (write-back gated)
//   $C0n4 read  = status: bit 7 = no media, bit 6 = write-protected,
//                 bit 0 = last block transfer hit an I/O error (out-of-range
//                 / rejected block) — the ROM driver tests this after a
//                 read/write stream and returns carry-set (ProDOS $27) so a
//                 failed transfer is never reported as success.
//
// Slot ROM ($Cn00-$CnFF) holds a ProDOS block driver that ProDOS scans
// for at boot. Signature:
//
//   $Cn01 = $20     (ProDOS signature)
//   $Cn03 = $00
//   $Cn05 = $03
//   $Cn07 = $3C     (SmartPort signature — non-zero, identifies as block dev)
//   $CnFE = $13     (read+write+status, 2 logical units)
//   $CnFF = $50     (driver entry at $Cn50)
//
// The driver examines ProDOS's $43 unit byte (bit 7 = drive 2) and routes
// to the corresponding pair of soft switches. Boot routine at $Cn20
// reads block 0 of unit 0 to $0800 and JMPs $0801 — identical pattern to
// the HDV card so existing ProDOS bootstraps work unchanged.

#ifndef POM2_SMARTPORT_CARD_H
#define POM2_SMARTPORT_CARD_H

#include "MountableMediaCard.h"
#include "SlotPeripheral.h"
#include "SmartPortUnit.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class FloppySoundSink;

namespace pom2 {

class SmartPortCard : public SlotPeripheral, public MountableMediaCard
{
public:
    static constexpr int    kDefaultSlot   = 5;
    static constexpr size_t kBlockBytes    = SmartPortUnit::kBlockBytes;
    /// Maximum units chained on this card. Capped at 2 in v1 to match
    /// ProDOS-8's direct slot driver (drive 1 + drive 2). Real Liron =
    /// 2 as well; raising this to 4 needs the SmartPort extended-call
    /// protocol in the ROM driver (not wired today).
    static constexpr size_t kMaxUnits      = 2;

    /// `slot` is baked into the slot ROM (signature byte, driver
    /// address, soft-switch trampolines). All units start empty;
    /// the host plugs / replaces units via `setUnit`.
    explicit SmartPortCard(int slot);

    int getSlot() const { return slot_; }

    /// Replace the unit at `idx` (0..kMaxUnits-1). Pass nullptr to
    /// clear the slot. Returns the old unit so the caller can
    /// inspect / destroy it (typical use: just let unique_ptr drop).
    /// Idempotent on an already-empty slot.
    std::unique_ptr<SmartPortUnit> setUnit(size_t idx,
                                           std::unique_ptr<SmartPortUnit> u);

    /// Borrowed access for the panel UI. Nullable.
    SmartPortUnit*       unit(size_t idx)
    { return idx < kMaxUnits ? units_[idx].get() : nullptr; }
    const SmartPortUnit* unit(size_t idx) const
    { return idx < kMaxUnits ? units_[idx].get() : nullptr; }

    /// Currently selected unit (set by $C0n0 writes; resets to 0 on
    /// reset). The ProDOS driver re-latches it on every dispatch.
    size_t activeUnit() const { return activeUnit_; }

    /// Mechanical sound sink (head step / motor whirr / click). Optional;
    /// null = silent. Block-level transfers only, so we emit `step()`
    /// once per actual block read/write and gate `motor()` with a
    /// wall-clock-ish spin-down derived from `advanceCycles`.
    /// MainWindow wires this to EmulationController::floppySound35().
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    // ── SlotPeripheral interface ────────────────────────────────────────
    std::string_view name() const override { return "SmartPort"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    bool    exposesIicOnboardRom() const override;
    void    onReset() override;
    void    advanceCycles(int cycles) override;

    // ── MountableMediaCard: one bay per unit, with per-bay type select ──
    // (empty / 3.5" / HDV). Lets the Slot Manager drive each unit
    // generically. NOTE: persistence (smartport_slotN_unitK_*) is the
    // host's (MainWindow's) job — these only touch in-memory unit state.
    int  bayCount() const override { return static_cast<int>(kMaxUnits); }
    MediaBayInfo bayInfo(int bay) const override;
    bool mountBay(int bay, const std::string& path, std::string& errOut) override;
    void ejectBay(int bay) override;
    void setBayWriteBack(int bay, bool on) override;
    std::vector<std::pair<std::string, std::string>>
         bayTypeOptions(int bay) const override;
    void setBayType(int bay, const std::string& kindKey) override;

private:
    int  slot_;
    std::array<std::unique_ptr<SmartPortUnit>, kMaxUnits> units_{};
    std::array<uint8_t, 256> rom_{};

    // Per-unit transfer state. The byte-stream protocol auto-increments
    // `streamOffset_[u]` per access, wrapping every 512 B. Drive select
    // ($C0n0) latches `activeUnit_` for $C0n3 (data) / $C0n4 (status)
    // — block setup ($C0n1/2) writes to the active unit's register pair.
    size_t   activeUnit_                 = 0;
    std::array<uint16_t, kMaxUnits> selectedBlock_{};
    std::array<size_t,   kMaxUnits> streamOffset_{};
    std::array<std::array<uint8_t, kBlockBytes>, kMaxUnits> readCache_{};
    std::array<bool,     kMaxUnits> readCacheValid_{};
    std::array<uint16_t, kMaxUnits> readCacheBlock_{};
    std::array<std::array<uint8_t, kBlockBytes>, kMaxUnits> writeBuf_{};
    std::array<bool,     kMaxUnits> writeBufPrimed_{};
    // Set when the active unit's readBlock/writeBlock fails (out-of-range /
    // rejected). Surfaced as $C0n4 bit 0; cleared at the start of each
    // transfer (unit-select / block-register write). See buildRom().
    std::array<bool,     kMaxUnits> ioError_{};

    // ── Sound state ─────────────────────────────────────────────────────
    FloppySoundSink* sound_           = nullptr;
    uint64_t         cpuCycleTotal_   = 0;
    uint64_t         lastAccessCycle_ = 0;
    bool             audibleMotorOn_  = false;
    static constexpr uint64_t kSpinDownCycles = 500'000;  // ~0.5 s @ 1 MHz

    void    buildRom();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
    uint8_t statusByte() const;
    uint8_t blockCountByte(int which) const;   // 0 = low, 1 = high (STATUS)
    void    noteAccess();
};

} // namespace pom2

#endif // POM2_SMARTPORT_CARD_H
