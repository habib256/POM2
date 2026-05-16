// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPort35Unit.h"

namespace pom2 {

SmartPort35Unit::SmartPort35Unit() = default;

SmartPort35Unit::~SmartPort35Unit()
{
    // Best-effort write-back on destruction (e.g. card unplugged).
    // No-op when write-back is off or nothing is dirty.
    img_.saveDirty();
}

uint32_t SmartPort35Unit::blockCount() const
{
    return img_.isLoaded() ? 1600u : 0u;   // 800 K / 512 B
}

bool SmartPort35Unit::readBlock(uint32_t idx, uint8_t* out) const
{
    if (!img_.isLoaded() || !out) return false;
    return img_.readBlock(idx, out);
}

bool SmartPort35Unit::writeBlock(uint32_t idx, const uint8_t* in)
{
    if (!img_.isLoaded() || img_.isWriteProtected() || !in) return false;
    return img_.writeBlock(idx, in);
}

bool SmartPort35Unit::loadImage(const std::string& path)
{
    // Auto-save the outgoing image's dirty blocks so a user-driven
    // swap doesn't quietly lose mid-session writes (same UX as
    // DiskIICard::insertDisk).
    img_.saveDirty();
    const bool ok = img_.loadFile(path);
    lastError_ = ok ? std::string{} : img_.lastError();
    return ok;
}

void SmartPort35Unit::eject()
{
    img_.eject();
    lastError_.clear();
}

} // namespace pom2
