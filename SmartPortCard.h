// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortCard — Apple II expansion card that gives a //e (or II+) access
// to Sony 3.5" 800 K disks the same way the //c+ on-board SmartPort does.
// Modelled after the real "Apple II 3.5 Disk Controller Card" (a.k.a. the
// "Liron" card, Apple part 670-0186), slot 5 by convention.
//
// Architecture choice — block-level vs IWM-level:
//
// The real card carries a full IWM plus a tiny 6502 ROM with the SmartPort
// dispatcher; ProDOS calls the dispatcher and the dispatcher talks GCR to
// whichever Sony drive the user clicked. POM2's //c+ profile already
// emulates that full stack (`IWMDevice`, `SmartPortHub`, `Sony35Drive`).
// For a slot-plugged card on //e we don't need the bit-level emulation —
// no game does flux-level tricks through the Liron — so this class skips
// the IWM and exposes the underlying `Disk35Image` blocks directly through
// a streaming protocol identical to `ProDOSHardDiskCard`'s. Same shape:
//
//   $C0n0 write = drive select (0 = drive 1, 1 = drive 2)
//   $C0n1 write = block LO byte (selected drive)
//   $C0n2 write = block HI byte (selected drive)
//   $C0n3 read  = next byte of selected block (auto-incrementing)
//   $C0n3 write = next byte INTO selected block (write-back gated)
//   $C0n4 read  = status: bit 7 = no disk, bit 6 = write-protected
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
// reads block 0 of drive 1 to $0800 and JMPs $0801 — identical pattern
// to the HDV card so existing ProDOS bootstraps work unchanged.
//
// Image ownership: borrowed pointers. The host (MainWindow / EmulationController)
// owns the two `Disk35Image` slots; the card just exposes them on the bus.
// That way the //c+ on-board SmartPort and a //e-plugged Liron card share
// the same image storage when a user happens to run both configs in the
// same session — and the Disk 3.5" panel doesn't need to know which path
// is active.

#ifndef POM2_SMARTPORT_CARD_H
#define POM2_SMARTPORT_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

class FloppySoundSink;

namespace pom2 {

class Disk35Image;

class SmartPortCard : public SlotPeripheral
{
public:
    static constexpr int    kDefaultSlot   = 5;
    static constexpr size_t kBlockBytes    = 512;
    static constexpr size_t kBlocksPer800k = 1600;     // 819 200 / 512

    /// `slot` is baked into the slot ROM (signature byte, driver address,
    /// soft-switch trampolines). `image0` / `image1` are non-owning —
    /// MainWindow / EmulationController owns the storage; the card just
    /// dispatches block I/O onto whichever image is currently loaded.
    /// Pass `nullptr` for an unused slot (drive 2 is optional).
    SmartPortCard(int slot, Disk35Image* image0, Disk35Image* image1);

    int getSlot() const { return slot_; }

    /// Direct image access for the Disk 3.5" panel + tests. Non-owning.
    Disk35Image* image(int drive) const {
        return (drive == 0) ? image0_ : image1_;
    }

    /// Mechanical sound sink (head step / motor whirr / click). Optional;
    /// when null the card stays silent. The sound layer is necessarily
    /// synthetic on this card — the Liron does block-level transfers
    /// only, so we emit `step()` once per block and gate `motor()` with
    /// a wall-clock-ish spin-down derived from the running CPU cycle
    /// count (advanceCycles). MainWindow::plugSmartPort35 wires this to
    /// EmulationController::floppySound35().
    void setFloppySound(FloppySoundSink* fs) { sound_ = fs; }

    // ── SlotPeripheral interface ────────────────────────────────────────
    std::string_view name() const override { return "SmartPort 3.5\""; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead      (uint8_t low8) override;
    void    onReset() override;
    void    advanceCycles(int cycles) override;

private:
    int           slot_;
    Disk35Image*  image0_;
    Disk35Image*  image1_;
    std::array<uint8_t, 256> rom_{};

    // Per-drive transfer state. The byte-stream protocol auto-increments
    // `streamOffset_[drv]` per access, wrapping every 512 B. Drive select
    // ($C0n0) latches the active drive for $C0n3 (data) / $C0n4 (status)
    // — block setup ($C0n1/2) writes to the active drive's own register
    // pair.
    int      activeDrive_       = 0;          // 0 or 1
    uint16_t selectedBlock_[2]  = {0, 0};
    size_t   streamOffset_[2]   = {0, 0};

    // ── Sound state ─────────────────────────────────────────────────────
    // We emit one `step()` per actual block read or write, stamped with
    // `cpuCycleTotal_` (driven by advanceCycles, same source DiskIICard
    // uses). `motor(true)` fires on the first access after idle; the
    // spin-down `motor(false)` comes from advanceCycles when no fresh
    // access has happened in `kSpinDownCycles`. ~0.5 s of emulated CPU
    // time at 1 MHz feels right for a Liron — long enough not to clatter
    // on/off between blocks of one ProDOS read, short enough to hush
    // when the user navigates away.
    FloppySoundSink* sound_           = nullptr;
    uint64_t         cpuCycleTotal_   = 0;
    uint64_t         lastAccessCycle_ = 0;
    bool             audibleMotorOn_  = false;
    static constexpr uint64_t kSpinDownCycles = 500'000;  // ~0.5 s @ 1 MHz

    void    buildRom();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
    uint8_t statusByte() const;
    void    noteAccess();   // emits step + motor-on (idempotent), updates timer
    Disk35Image* active() const { return (activeDrive_ == 0) ? image0_ : image1_; }
};

} // namespace pom2

#endif // POM2_SMARTPORT_CARD_H
