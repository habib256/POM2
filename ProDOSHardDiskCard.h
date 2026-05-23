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
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class ProDOSHardDiskCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 5;
    static constexpr size_t kBlockBytes = 512;

    /// Construct with the slot number this card will be plugged into.
    /// The slot is baked into the slot ROM (signature byte $Cn02, ProDOS
    /// driver entry $Cn50, soft-switch addresses inside the read/write
    /// trampolines) so changing it after construction would require
    /// rebuilding the ROM.
    explicit ProDOSHardDiskCard(int slot = kDefaultSlot);

    int getSlot() const { return slot; }

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

    /// Hardware write-protect, as seen by the emulated ProDOS driver. This
    /// reflects ONLY the real medium's WP state (the 2MG header flag) — NOT
    /// the host-file write-back preference. A real hard disk presents a
    /// read/write volume to the OS; "don't modify my .hdv file" is a separate
    /// host-side concern handled by `writeBackEnabled` (see writeDataByte /
    /// saveDirty). Conflating the two used to make ProDOS see a write-
    /// protected boot volume by default, so games that write state on the fly
    /// (e.g. Nox Archaist entering a city) got an unexpected $2B error and
    /// crashed. Cf. AppleWin Harddisk.cpp: image writes always land in RAM;
    /// a separate read-only flag gates the medium.
    bool isWriteProtected()  const { return writeProtectedHeader; }
    /// User opt-in for persisting RAM writes back to the host .hdv/.2mg file.
    /// Default off — the in-session volume is fully writable either way, but
    /// the file on disk is not modified until the user explicitly enables it.
    bool isWriteBackEnabled() const { return writeBackEnabled; }
    void setWriteBackEnabled(bool on) { writeBackEnabled = on; }
    bool canWriteBack()       const { return supportsWriteBack && !writeProtectedHeader; }
    bool hasUnsavedChanges() const { return anyDirty; }
    bool isSynthVolumeMounted() const { return isSynthVolume; }

    /// Recent block-I/O activity, used by MainWindow's auto-turbo. A ProDOS
    /// hard disk has no "motor" line like the Disk II, so we treat any access
    /// to the streaming data port as activity and decay it over a few UI
    /// frames (hysteresis) so a multi-block load stays in turbo end-to-end.
    /// Lock-free: the data port is touched on the CPU thread while the UI
    /// thread polls at 60 Hz.
    bool isBusy() const
    {
        return activityTicks.load(std::memory_order_relaxed) > 0;
    }
    /// Called once per UI frame by the turbo poller to bleed off activity.
    void tickActivityDecay()
    {
        uint32_t v = activityTicks.load(std::memory_order_relaxed);
        if (v) activityTicks.store(v - 1, std::memory_order_relaxed);
    }
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
    int slot;
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

    // Auto-turbo busy signal: reloaded on every data-port access, decayed one
    // step per UI frame by tickActivityDecay(). kBusyHysteresisFrames keeps
    // turbo engaged across the short gaps between consecutive block reads in a
    // single ProDOS load.
    static constexpr uint32_t kBusyHysteresisFrames = 8;
    mutable std::atomic<uint32_t> activityTicks{0};

    void buildRom();
    uint8_t readDataByte();
    void    writeDataByte(uint8_t v);
};

#endif // POM2_PRODOS_HARD_DISK_CARD_H
