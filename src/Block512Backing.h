// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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
    ///
    /// Reads the file INLINE, so a caller holding `stateMutex` holds it across
    /// the whole read — up to 32 MiB, the largest single stall in the tree.
    /// UI callers want the two-phase pair below; this stays for the
    /// single-threaded callers (CLI, tests, the profile-switch remount).
    bool loadImage(const std::string& path);

    /// ── Two-phase mount ─────────────────────────────────────────────────
    /// What phase 1 hands to phase 2. Just bytes and the two facts about the
    /// FILE that phase 2 would otherwise have to go back to the filesystem
    /// for — so phase 2 makes no syscalls at all.
    struct PreparedImage {
        std::vector<uint8_t> bytes;
        std::string          path;
        /// False when the file could not be opened for writing. Probed in
        /// phase 1 because it is a syscall, and because a chmod-read-only
        /// image must mount write-protected rather than accept a session of
        /// writes and fail at flush time.
        bool                 hostWritable = true;
        bool                 valid        = false;
    };

    /// Phase 1, to be called WITHOUT `stateMutex`: read `path` whole, with the
    /// same size and emptiness gates loadImage() applies. Touches no object
    /// state — it is static for exactly that reason.
    static bool readImageFile(const std::string& path, PreparedImage& out,
                              std::string& error);

    /// Phase 2, to be called WITH `stateMutex` held: flush the outgoing
    /// medium, then parse and adopt what phase 1 read. All the 2IMG decoding
    /// and validation lives here, so nothing is skipped relative to
    /// loadImage() — this is the same code, reached from the other side.
    ///
    /// Re-reads under the lock in one case, deliberately: when the outgoing
    /// medium is the SAME file with unsaved changes, the prepared bytes
    /// predate the flush and installing them would roll the guest's writes
    /// back. Rare (write-back is opt-in and the paths must match exactly) and
    /// correct beats fast on the write-back path.
    bool adoptImage(PreparedImage&& prepared);

private:
    /// The parse-and-adopt half both entry points share. Performs NO file
    /// I/O — everything it needs is already in `prepared` — so it is the part
    /// that is cheap to run under the lock.
    bool adoptPrepared(PreparedImage&& prepared);

public:

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
    ///
    /// Does its file work INLINE, so a caller holding `stateMutex` freezes the
    /// machine for the whole read-modify-write + rename. That is the eject
    /// path's problem, and `takeWriteBack` + `commitWriteBack` below are the
    /// split that solves it; this stays for single-threaded callers and for
    /// the deliberate flush-before-destroy in `adoptImage`.
    bool saveDirty();

    /// ── Two-phase eject ─────────────────────────────────────────────────
    /// The mirror of `PreparedImage`: what phase 1 lifts OUT of the backing so
    /// phase 2 can write it with `stateMutex` released. Carries the decoded
    /// facts (offset, dirty block numbers and their bytes) so phase 2 needs
    /// nothing from the object — by then the medium may already be gone.
    struct PendingWriteBack {
        bool                  valid      = false;  ///< false → phase 2 no-ops
        bool                  synth      = false;
        std::string           path;               ///< file case
        std::string           hostFolder;         ///< synth case
        size_t                dataOffset = 0;
        size_t                dataLength = 0;
        std::vector<uint32_t> dirtyIndices;       ///< file case
        std::vector<uint8_t>  dirtyBytes;         ///< 512 × dirtyIndices
        std::vector<uint8_t>  synthImage;         ///< synth case: whole volume
    };

    /// Phase 1, to be called WITH `stateMutex` held: copy out exactly what
    /// `saveDirty()` would write. Memcpy only — no syscalls — so it is cheap
    /// under the lock. Does NOT clear the dirty flags: an eject drops the
    /// medium anyway, and a caller that means to keep it calls `clearDirty()`
    /// only once phase 2 has actually succeeded.
    PendingWriteBack takeWriteBack() const;

    /// Phase 2, to be called WITHOUT the lock: perform the deferred write.
    /// Static because the backing it came from may no longer exist.
    static bool commitWriteBack(PendingWriteBack&& pending,
                                std::string& error);

    /// Drop the dirty set after a successful `commitWriteBack`.
    void clearDirty();

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
