// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortHdvUnit — block-level ProDOS HDV / 2MG image as a `SmartPortUnit`.
// Independent of `ProDOSHardDiskCard` (which wires its own slot ROM); this
// unit only handles the byte store + dirty tracking + load/save so it
// can plug into a `SmartPortCard` chain alongside 3.5" units.
//
// Supports:
//   * Raw .hdv  — whole file is a stream of 512-byte ProDOS blocks
//   * .2mg      — 64-byte 2IMG header + ProDOS block data (format must = 1)
//
// Hard caps: ≥ 1 block, ≤ 65536 blocks (32 MB ProDOS-8 ceiling). The 2MG
// write-back path preserves the original header verbatim (in-place rewrite
// of dirty blocks only — no truncation / regeneration). Synth-from-folder
// volumes are not supported here; for that, plug a separate
// `ProDOSHardDiskCard` (which keeps the existing `prodos_disk/` UX).

#ifndef POM2_SMARTPORT_HDV_UNIT_H
#define POM2_SMARTPORT_HDV_UNIT_H

#include "SmartPortUnit.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class SmartPortHdvUnit : public SmartPortUnit
{
public:
    static constexpr std::string_view kKindKey   = "hdv";
    static constexpr std::string_view kKindLabel = "ProDOS HDV";

    SmartPortHdvUnit();
    ~SmartPortHdvUnit() override;

    std::string_view kindKey()   const override { return kKindKey; }
    std::string_view kindLabel() const override { return kKindLabel; }

    bool     isLoaded()         const override { return loaded_; }
    bool     isWriteProtected() const override {
        return !writeBackEnabled_ || writeProtectedHeader_;
    }
    uint32_t blockCount() const override {
        return static_cast<uint32_t>(image_.size() / kBlockBytes);
    }
    bool     readBlock (uint32_t idx, uint8_t* out) const override;
    bool     writeBlock(uint32_t idx, const uint8_t* in) override;

    bool     loadImage(const std::string& path) override;
    void     eject() override;
    const std::string& path()      const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

    bool     isWriteBackEnabled() const override { return writeBackEnabled_; }
    void     setWriteBackEnabled(bool on) override { writeBackEnabled_ = on; }
    bool     saveDirty() override;

private:
    std::vector<uint8_t> image_;                  // 512-byte block stream
    std::vector<bool>    dirtyBlocks_;
    std::size_t          dataOffset_           = 0;   // bytes before block 0 in source file
    bool                 loaded_               = false;
    bool                 anyDirty_             = false;
    bool                 writeBackEnabled_     = false;
    bool                 writeProtectedHeader_ = false;
    std::string          path_;
    std::string          lastError_;
};

} // namespace pom2

#endif // POM2_SMARTPORT_HDV_UNIT_H
