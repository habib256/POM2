// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ProDOSHardDiskCard — minimal ProDOS block-device card for raw .hdv images.
// Slot 5 by convention: ProDOS gives SmartPort / hard-disk devices there
// priority over the Disk II in slot 6 while leaving video cards in slot 7.
//
// The card exposes Apple's ProDOS disk ID bytes in its slot ROM:
//
//   $Cn01 = $20, $Cn03 = $00, $Cn05 = $03
//   $CnFE = $03       read/write/status advertised; writes return WP error
//   $CnFF = $50       ProDOS block driver entry at $Cn50
//
// The 6502 ROM translates the standard ProDOS device parameter block
// ($42 command, $43 unit, $44/$45 buffer, $46/$47 block) into a byte-stream
// read from three soft switches in the slot's device-select window.

#ifndef POM2_PRODOS_HARD_DISK_CARD_H
#define POM2_PRODOS_HARD_DISK_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ProDOSHardDiskCard : public SlotPeripheral
{
public:
    static constexpr int kSlot = 5;
    static constexpr size_t kBlockBytes = 512;

    ProDOSHardDiskCard();

    bool loadImage(const std::string& path);
    /// Replace the in-memory image with synthesised bytes (e.g. produced by
    /// pom2::buildVolumeFromFolder). `label` is what the UI shows; it does
    /// not have to be a real filesystem path. `hostFolder`, when non-empty,
    /// flags the volume as a synth from a host folder so save-on-eject can
    /// decode the modified volume back into that folder. Returns false if
    /// `bytes` is empty or not a multiple of 512.
    bool loadImageFromBytes(std::vector<uint8_t> bytes,
                            const std::string& label,
                            const std::string& hostFolder = std::string{});
    void ejectImage();

    bool isImageLoaded() const { return imageLoaded; }
    const std::string& getImagePath() const { return imagePath; }
    const std::string& getLastError() const { return lastError; }
    size_t getBlockCount() const { return image.size() / kBlockBytes; }

    /// User opt-in for write-back. Default off — disk-image files and host
    /// folders are not modified until the user explicitly enables it.
    bool isWriteProtected()  const { return !writeBackEnabled || writeProtectedHeader; }
    bool isWriteBackEnabled() const { return writeBackEnabled; }
    void setWriteBackEnabled(bool on) { writeBackEnabled = on; }
    bool canWriteBack()       const { return supportsWriteBack && !writeProtectedHeader; }
    bool hasUnsavedChanges() const { return anyDirty; }
    bool isSynthVolumeMounted() const { return isSynthVolume; }
    /// Persist all dirty 512-byte blocks back to the source file (.hdv/.2mg)
    /// preserving the 2MG container header verbatim, OR for synth volumes,
    /// decode the modified volume back to the host folder. No-op when
    /// write-back is disabled, the image is read-only, or nothing is dirty.
    bool saveDirty();

    std::string_view name() const override { return "ProDOS HDV"; }
    uint8_t deviceSelectRead(uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    onReset() override;

private:
    std::array<uint8_t, 256> rom{};
    std::vector<uint8_t> image;
    std::vector<uint8_t> headerBytes;     // 2MG container bytes [0..dataOffset)
    size_t  dataOffset = 0;
    size_t  dataLength = 0;
    std::vector<bool> dirtyBlocks;
    bool    anyDirty             = false;
    bool    writeBackEnabled     = false;
    bool    writeProtectedHeader = false;
    bool    supportsWriteBack    = false;
    bool    isSynthVolume        = false;
    std::string hostFolderPath;
    std::string imagePath;
    std::string lastError;
    bool imageLoaded = false;

    uint16_t selectedBlock = 0;
    size_t streamOffset = 0;       // byte offset within the selected 512-byte block

    void buildRom();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
};

#endif // POM2_PRODOS_HARD_DISK_CARD_H
