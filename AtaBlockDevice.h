// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AtaBlockDevice — minimal ATA/IDE taskfile device over a flat 512-byte block
// backing. The subset the Apple II IDE-class slot cards actually drive: IDENTIFY
// DEVICE, READ SECTOR(S), WRITE SECTOR(S), LBA28. Reusable across CFFA / Vulcan
// / Zip / Focus (TODO.md § Cartes de stockage MAME-fidèles, P1).
//
// Register interface is isomorphic to MAME's ata_interface_device cs0 access
// (machine/ataintf): cs0_r/cs0_w take the taskfile register index 0..7 and the
// data register (0) is 16-bit. The 8-bit↔16-bit data latch split lives in the
// owning slot card (CffaCard), exactly as MAME's a2cffa.cpp does it.
//
//   reg 0  Data            (16-bit, DRQ-gated PIO)
//   reg 1  Error (R) / Features (W)
//   reg 2  Sector count    (0 ⇒ 256)
//   reg 3  LBA  0..7
//   reg 4  LBA  8..15
//   reg 5  LBA 16..23
//   reg 6  Drive/Head: bits0-3 LBA24..27, bit4 drive, bit6 LBA, bits5,7 obsolete
//   reg 7  Status (R) / Command (W)
//
// NOT modelled: DMA, interrupts, CHS addressing for I/O (IDENTIFY still reports
// a plausible CHS geometry), security/SMART. CHD backing is P1-phase-2.

#ifndef POM2_ATA_BLOCK_DEVICE_H
#define POM2_ATA_BLOCK_DEVICE_H

#include "Block512Backing.h"

#include <array>
#include <cstdint>

namespace pom2 {

class AtaBlockDevice
{
public:
    // ATA status register bits.
    static constexpr uint8_t kStBSY  = 0x80; // busy
    static constexpr uint8_t kStDRDY = 0x40; // device ready
    static constexpr uint8_t kStDF   = 0x20; // device fault
    static constexpr uint8_t kStDSC  = 0x10; // seek complete
    static constexpr uint8_t kStDRQ  = 0x08; // data request
    static constexpr uint8_t kStERR  = 0x01; // error

    // ATA commands we honour explicitly; everything else completes as a no-op.
    static constexpr uint8_t kCmdRead       = 0x20;
    static constexpr uint8_t kCmdReadMulti  = 0xC4;
    static constexpr uint8_t kCmdWrite      = 0x30;
    static constexpr uint8_t kCmdWriteMulti = 0xC5;
    static constexpr uint8_t kCmdIdentify   = 0xEC;

    /// The backing this device serves. CffaCard mounts images through it.
    Block512Backing&       backing()       { return backing_; }
    const Block512Backing& backing() const { return backing_; }

    /// Hard reset (power-on / SRST): clears the taskfile and any in-flight PIO.
    void reset();

    /// cs0 (command block) register access. reg 0 is the 16-bit data port; the
    /// other registers carry an 8-bit value in the low byte.
    uint16_t cs0_r(uint8_t reg);
    void     cs0_w(uint8_t reg, uint16_t val);

    /// cs1 (control block) register access — only the alternate status (read)
    /// and device control (write) at offset 6 are meaningful here.
    uint16_t cs1_r(uint8_t reg);
    void     cs1_w(uint8_t reg, uint16_t val);

private:
    enum class Phase { Idle, PioIn, PioOut };

    void startCommand(uint8_t cmd);
    void loadSectorToBuffer();   // backing block at lba_ → wordBuf_
    void flushBufferToSector();  // wordBuf_ → backing block at lba_
    void fillIdentify();
    uint32_t currentLba() const;

    Block512Backing backing_;

    // Taskfile registers (8-bit each except data).
    uint8_t error_       = 0x00;
    uint8_t features_    = 0x00;
    uint8_t sectorCount_ = 0x00;
    uint8_t lba0_        = 0x00;
    uint8_t lba1_        = 0x00;
    uint8_t lba2_        = 0x00;
    uint8_t devHead_     = 0xA0; // bits 5,7 obsolete-1, LBA bit clear initially
    uint8_t status_      = kStDRDY | kStDSC;
    uint8_t control_     = 0x00;

    Phase    phase_       = Phase::Idle;
    uint32_t lba_         = 0;   // LBA latched at command start
    uint16_t sectorsLeft_ = 0;   // sectors remaining in the current transfer
    size_t   wordIdx_     = 0;   // current word within wordBuf_ (0..256)
    std::array<uint16_t, 256> wordBuf_{}; // one 512-byte sector as 256 LE words
};

} // namespace pom2

#endif // POM2_ATA_BLOCK_DEVICE_H
