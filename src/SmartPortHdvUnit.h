// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortHdvUnit — block-level ProDOS HDV / 2MG image as a `SmartPortUnit`.
// A thin adapter over the shared `pom2::Block512Backing` (the same store the
// HDV-class cards use), so the 2IMG envelope parsing, dirty tracking, medium
// write-protect, and host-file write-back live in ONE tested place rather
// than being re-implemented per consumer. This unit only maps the
// SmartPortUnit interface onto that store so it can plug into a SmartPortCard
// chain alongside 3.5" units.
//
// Supports:
//   * Raw .hdv  — whole file is a stream of 512-byte ProDOS blocks
//   * .2mg      — 64-byte 2IMG header + ProDOS block data (format must = 1)
//
// Hard caps: ≥ 1 block, ≤ 65536 blocks (32 MB ProDOS-8 ceiling). The 2MG
// write-back path preserves the original header verbatim. Synth-from-folder
// volumes are deliberately NOT exposed here; for that, plug a separate
// `ProDOSHardDiskCard` (which keeps the existing `prodos_folder/` UX).

#ifndef POM2_SMARTPORT_HDV_UNIT_H
#define POM2_SMARTPORT_HDV_UNIT_H

#include "Block512Backing.h"
#include "SmartPortUnit.h"

#include <cstdint>
#include <string>

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

    bool     isLoaded()         const override { return backing_.isLoaded(); }
    // Reflects ONLY the real medium WP flag (2MG header), not the host-file
    // write-back preference — so ProDOS sees a read/write volume by default.
    // Persisting RAM writes to the file is the separate write-back opt-in.
    bool     isWriteProtected() const override { return backing_.isWriteProtected(); }
    uint32_t blockCount() const override {
        return static_cast<uint32_t>(backing_.blockCount());
    }
    bool     readBlock (uint32_t idx, uint8_t* out) const override;
    bool     writeBlock(uint32_t idx, const uint8_t* in) override;

    bool     loadImage(const std::string& path) override;
    void     eject() override;
    const std::string& path()      const override { return backing_.path(); }
    const std::string& lastError() const override { return backing_.lastError(); }

    bool     isWriteBackEnabled() const override { return backing_.isWriteBackEnabled(); }
    void     setWriteBackEnabled(bool on) override { backing_.setWriteBackEnabled(on); }
    bool     saveDirty() override { return backing_.saveDirty(); }

private:
    Block512Backing backing_;
};

} // namespace pom2

#endif // POM2_SMARTPORT_HDV_UNIT_H
