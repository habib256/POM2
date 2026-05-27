// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Block512Backing — shared 512-byte block backing store for ProDOS
// hard-disk style cards. Owns the in-memory image, the 2IMG/.2mg container
// envelope (header + any trailer preserved bit-for-bit on write-back),
// medium write-protect, dirty-block tracking, opt-in host-file write-back,
// and host-folder synth volumes.
//
// Two cards share it (DEV.md § ProDOSHardDiskCard / § CffaCard):
//   - ProDOSHardDiskCard  — synthetic streaming port → byte-level access.
//   - AtaBlockDevice / CffaCard — MAME-faithful ATA → block-level access.
//
// Extracted verbatim from ProDOSHardDiskCard (2026-05-24, P1 § Cartes de
// stockage MAME-fidèles) so behaviour — and the hdv_* pin tests — are
// unchanged. eject() does NOT auto-save; the owning card decides whether to
// flush first (it has the policy context, e.g. save-on-eject).

#ifndef POM2_BLOCK512_BACKING_H
#define POM2_BLOCK512_BACKING_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class Block512Backing
{
public:
    static constexpr size_t kBlockBytes = 512;
    // ProDOS block NUMBERS are 16-bit, so the highest addressable block index is
    // $FFFF — which means a volume can hold up to 65536 blocks (indices
    // 0..$FFFF) = exactly 32 MiB. The HDV card's uint16_t selectedBlock reaches
    // every one of them. Many real 32 MiB raw .hdv dumps (e.g. A2DeskTop) are
    // exactly 65536 blocks; only 65537+ (index $10000, unreachable) is rejected.
    static constexpr size_t kMaxBlocks  = 0x10000;  // 65536 blocks (indices 0..$FFFF)

    /// Load a raw .hdv or 2IMG/.2mg image. Parses + strips the 2IMG header,
    /// validates ProDOS block order, 512-byte multiple, and ≤65536 blocks.
    /// On failure leaves the store empty and sets lastError().
    bool loadImage(const std::string& path);

    /// Replace the image with synthesised bytes (e.g. a host-folder volume).
    /// `hostFolder` non-empty marks it a synth volume whose write-back decodes
    /// back into that folder. Must be a non-zero multiple of 512.
    bool loadFromBytes(std::vector<uint8_t> bytes, const std::string& label,
                       const std::string& hostFolder);

    /// Drop the image. Does NOT auto-save — the owning card flushes first if
    /// its policy says so (see ProDOSHardDiskCard::ejectImage).
    void eject();

    /// Persist dirty blocks to the source (.hdv/.2mg in-place rewrite that
    /// preserves header + trailer; OR synth-folder decode). No-op (returns
    /// true) when write-back is off, the medium is WP, or nothing is dirty.
    bool saveDirty();

    bool   isLoaded()   const { return loaded_; }
    size_t blockCount() const { return image_.size() / kBlockBytes; }
    const std::string& path()      const { return path_; }
    const std::string& hostFolder() const { return hostFolder_; }
    const std::string& lastError() const { return lastError_; }

    bool isWriteProtected()   const { return wpHeader_; }
    bool isSynthVolume()      const { return synth_; }
    bool isWriteBackEnabled() const { return writeBack_; }
    void setWriteBackEnabled(bool on) { writeBack_ = on; }
    bool canWriteBack()       const { return supportsWriteBack_ && !wpHeader_; }
    bool hasUnsavedChanges()  const { return anyDirty_; }

    /// Block-level access (ATA path). Returns false when blk is out of range.
    /// readBlock copies 512 bytes into dst512; writeBlock copies from src512
    /// and marks the block dirty (no-op + false when the medium is WP).
    bool readBlock (uint32_t blk, uint8_t* dst512) const;
    bool writeBlock(uint32_t blk, const uint8_t* src512);

    /// Byte-level access (streaming HDV port). `absolute` = blk*512 + offset.
    /// readByte returns 0xFF out of range; writeByte is a no-op out of range
    /// or when WP, and marks the containing block dirty otherwise.
    uint8_t readByte (size_t absolute) const;
    void    writeByte(size_t absolute, uint8_t v);

    /// Auto-turbo busy signal — any access bumps it; the UI bleeds it off one
    /// step per frame so a multi-block transfer stays in turbo end-to-end.
    bool isBusy() const
    {
        return activityTicks_.load(std::memory_order_relaxed) > 0;
    }
    void tickActivityDecay()
    {
        uint32_t v = activityTicks_.load(std::memory_order_relaxed);
        if (v) activityTicks_.store(v - 1, std::memory_order_relaxed);
    }

private:
    void markDirty(uint32_t blk);
    void bumpActivity() const
    {
        activityTicks_.store(kBusyHysteresisFrames, std::memory_order_relaxed);
    }

    std::vector<uint8_t> image_;
    std::vector<uint8_t> headerBytes_;   // 2IMG container bytes [0..dataOffset)
    size_t  dataOffset_ = 0;
    size_t  dataLength_ = 0;
    std::vector<bool> dirtyBlocks_;
    bool    anyDirty_          = false;
    bool    writeBack_         = false;
    bool    wpHeader_          = false;
    bool    supportsWriteBack_ = false;
    bool    synth_             = false;
    std::string hostFolder_;
    std::string path_;
    std::string lastError_;
    bool    loaded_ = false;

    static constexpr uint32_t kBusyHysteresisFrames = 8;
    mutable std::atomic<uint32_t> activityTicks_{0};
};

} // namespace pom2

#endif // POM2_BLOCK512_BACKING_H
