// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortHdvUnit — thin adapter mapping SmartPortUnit onto the shared
// pom2::Block512Backing store. All the heavy lifting (2IMG parse, dirty
// tracking, write-back, WP) lives in Block512Backing and is exercised by the
// hdv_* / ata_block_device pin tests; this file only forwards.

#include "SmartPortHdvUnit.h"

namespace pom2 {

SmartPortHdvUnit::SmartPortHdvUnit() = default;

SmartPortHdvUnit::~SmartPortHdvUnit()
{
    // Best-effort write-back on destruction (e.g. card unplugged). No-op when
    // write-back is off, the medium is WP, or nothing is dirty.
    backing_.saveDirty();
}

bool SmartPortHdvUnit::readBlock(uint32_t idx, uint8_t* out) const
{
    if (!out) return false;
    return backing_.readBlock(idx, out);
}

bool SmartPortHdvUnit::writeBlock(uint32_t idx, const uint8_t* in)
{
    if (!in) return false;
    // Block512Backing accepts the write into RAM (marking the block dirty)
    // unless the medium is WP — write-back-off still gives a read/write
    // session, it just won't be flushed by saveDirty(). Same semantics as
    // the old hand-rolled store and ProDOSHardDiskCard.
    return backing_.writeBlock(idx, in);
}

bool SmartPortHdvUnit::loadImage(const std::string& path)
{
    // Flush pending writes on the outgoing image before swapping, so a
    // mid-session swap doesn't silently drop dirty blocks.
    backing_.saveDirty();
    return backing_.loadImage(path);
}

void SmartPortHdvUnit::eject()
{
    // Save-on-eject policy lives here (Block512Backing::eject never auto-
    // saves): flush first (no-op unless write-back is on + dirty + writable),
    // then drop the image.
    backing_.saveDirty();
    backing_.eject();
}

} // namespace pom2
