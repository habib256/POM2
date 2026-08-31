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
#include <utility>

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
    /// Two-phase mount, phase 2 — forwards to the backing store.
    bool     adoptImage(Block512Backing::PreparedImage&& p) override
    { return backing_.adoptImage(std::move(p)); }
    bool     detachImage(Block512Backing::PendingWriteBack& out) override
    {
        if (!(backing_.isLoaded() && backing_.hasUnsavedChanges() &&
              backing_.isWriteBackEnabled() && !backing_.isWriteProtected()))
            return true;                 // nothing to write: out stays invalid
        out = backing_.takeWriteBack();
        return true;
    }
    void     clearDirtyBlocks() override { backing_.clearDirty(); }
    bool     eject() override;
    const std::string& path()      const override { return backing_.path(); }
    const std::string& lastError() const override { return backing_.lastError(); }

    bool     isWriteBackEnabled() const override { return backing_.isWriteBackEnabled(); }
    void     setWriteBackEnabled(bool on) override { backing_.setWriteBackEnabled(on); }
    bool     saveDirty() override { return backing_.saveDirty(); }
    bool     hasUnsavedChanges() const override { return backing_.hasUnsavedChanges(); }

private:
    Block512Backing backing_;
};

} // namespace pom2

#endif // POM2_SMARTPORT_HDV_UNIT_H
