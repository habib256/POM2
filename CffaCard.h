// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CffaCard — MAME-faithful CFFA 2.0 (CompactFlash / IDE) slot card: the real
// 4 KB dumped firmware (cffa20ee02.bin / cffa20eec02.bin) executed over an
// emulated ATA bus chip (AtaBlockDevice), image stored as raw LBA behind it.
// Contrast with the synthetic ProDOSHardDiskCard (AppleWin lineage). See
// DEV.md § CffaCard and TODO.md § Cartes de stockage MAME-fidèles (P1).
//
// Ported from MAME src/devices/bus/a2bus/a2cffa.cpp (master). Cited line
// ranges below are approximate — re-pin against the exact revision on touch
// (TODO « MAME path drift refresher »).
//
//   $C0nX device-select  → read_c0nx / write_c0nx  : 8↔16-bit data latch + ATA
//   $CnXX slot ROM       → read_cnxx               : eeprom[off + slot*0x100]
//   $C800 expansion ROM  → read_c800 / write_c800  : eeprom[off + 0x800], WP-gated

#ifndef POM2_CFFA_CARD_H
#define POM2_CFFA_CARD_H

#include "AtaBlockDevice.h"
#include "ProDOSBlockCard.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

class CffaCard : public SlotPeripheral, public ProDOSBlockCard
{
public:
    static constexpr int    kDefaultSlot = 7; // conventional IDE/hard-disk slot
    static constexpr size_t kRomBytes    = 0x1000; // 4 KB EEPROM (MAME ROM_REGION)

    explicit CffaCard(int slot = kDefaultSlot) : slot_(slot) {}

    int getSlot() const { return slot_; }

    /// Load the 4 KB CFFA firmware EEPROM dump. Must be exactly 4096 bytes.
    bool loadRom(const std::string& path);
    bool isRomLoaded() const { return romLoaded_; }

    /// Image management — forwarded to the ATA device's block backing so the
    /// HDV Library can mount .hdv/.2mg into the CFFA exactly like the HDV card.
    bool loadImage(const std::string& path);
    bool loadImageFromBytes(std::vector<uint8_t> bytes, const std::string& label,
                            const std::string& hostFolder = std::string{});
    void ejectImage();
    bool saveDirty() { return ata_.backing().saveDirty(); }

    bool isImageLoaded()      const { return ata_.backing().isLoaded(); }
    const std::string& getImagePath() const { return ata_.backing().path(); }
    const std::string& getLastError() const { return lastError_; }
    size_t getBlockCount()    const { return ata_.backing().blockCount(); }
    bool isWriteProtected()   const { return ata_.backing().isWriteProtected(); }
    bool isWriteBackEnabled() const { return ata_.backing().isWriteBackEnabled(); }
    void setWriteBackEnabled(bool on) { ata_.backing().setWriteBackEnabled(on); }
    bool canWriteBack()       const { return ata_.backing().canWriteBack(); }
    bool hasUnsavedChanges()  const { return ata_.backing().hasUnsavedChanges(); }
    bool isBusy() const { return ata_.backing().isBusy(); }
    void tickActivityDecay() { ata_.backing().tickActivityDecay(); }

    std::string_view name() const override { return "CFFA 2.0"; }
    uint8_t deviceSelectRead (uint8_t low4) override;       // read_c0nx
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override; // write_c0nx
    uint8_t slotRomRead (uint8_t low8) override;            // read_cnxx
    uint8_t expansionRomRead (uint16_t offset) override;    // read_c800
    void    expansionRomWrite(uint16_t offset, uint8_t v) override; // write_c800
    void    onReset() override;

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    bool romLoaded_ = false;
    std::string lastError_;

    AtaBlockDevice ata_;

    // 8↔16-bit data-port latch (a2cffa.cpp m_lastreaddata / m_lastdata).
    uint16_t lastReadData_  = 0;
    uint16_t lastWriteData_ = 0;
    // EEPROM write-enable (a2cffa.cpp m_writeprotect): default protected.
    bool writeProtect_ = true;
};

} // namespace pom2

#endif // POM2_CFFA_CARD_H
