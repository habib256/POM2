// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPortUnit — abstraction for a single block-level device plugged into
// a `SmartPortCard` (Liron-class). Each card holds up to N units (we cap
// at 2 for v1 since ProDOS 8's direct slot driver only sees drives 1/2;
// SmartPort extended protocol can reach further units but isn't wired
// here yet). The card dispatches READBLOCK / WRITEBLOCK / STATUS to the
// unit the ProDOS `$43` unit byte selects.
//
// Each concrete unit type owns its own storage. Two concrete units ship:
//   - `SmartPort35Unit`   — wraps a `Disk35Image` (800 K Sony 3.5")
//   - `SmartPortHdvUnit`  — wraps a raw / 2MG ProDOS HDV file (up to 32 MB)
//
// Adding more types later (e.g. a generic block-level disk that takes a
// .po image as a 280-block ProDOS volume) means subclassing this and
// listing it in `SmartPortUnit::kindKeyToInstance` so the slot config
// + settings persistence can recreate it after a restart.

#ifndef POM2_SMARTPORT_UNIT_H
#define POM2_SMARTPORT_UNIT_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace pom2 {

class SmartPortUnit
{
public:
    static constexpr size_t kBlockBytes = 512;

    virtual ~SmartPortUnit() = default;

    /// Short stable key for slot-config + settings persistence
    /// (`"35"`, `"hdv"`, …). Must be unique per concrete subclass.
    virtual std::string_view kindKey() const = 0;

    /// Human-readable label for the UI ("3.5\" 800K", "ProDOS HDV", …).
    virtual std::string_view kindLabel() const = 0;

    /// True when media is present. Empty drives still respond to STATUS
    /// (no-media bit set); READ/WRITE return false.
    virtual bool isLoaded() const = 0;

    /// Either the media is physically write-protected (e.g. WOZ flag,
    /// 2MG header) OR the user has not opted into write-back. Matches
    /// `Disk35Image::isWriteProtected` semantics.
    virtual bool isWriteProtected() const = 0;

    /// Block count for loaded media (0 when no media). 800K 3.5" = 1600;
    /// HDV varies. Used by the panel UI; the card's slot ROM doesn't
    /// surface this directly (ProDOS asks the volume itself).
    virtual uint32_t blockCount() const = 0;

    /// Copy block `idx` into `out`. Returns false on no-media or
    /// out-of-range.
    virtual bool readBlock (uint32_t idx, uint8_t* out) const = 0;

    /// Write `in` to block `idx`. Returns false on no-media,
    /// write-protected, or out-of-range. Writes are buffered in RAM
    /// and only persisted by `saveDirty` (or on `eject` via the
    /// owning card's wrapper).
    virtual bool writeBlock(uint32_t idx, const uint8_t* in) = 0;

    /// Mount media from `path`. Returns false on parse error / missing
    /// file; `lastError()` then has a human-readable diagnostic.
    /// Replaces any currently-loaded media (saves dirty first when
    /// write-back is on — same UX as the Disk II / HDV panels).
    virtual bool loadImage(const std::string& path) = 0;

    /// Persist + clear media. Auto-saves dirty blocks when write-back
    /// is enabled. Idempotent on empty drives.
    virtual void eject() = 0;

    /// Current image path; empty when nothing mounted.
    virtual const std::string& path() const = 0;

    /// Last load error (or empty when none). Cleared by `loadImage`.
    virtual const std::string& lastError() const = 0;

    /// Write-back toggle (save dirty blocks on eject / explicit save).
    /// Default off — the user opts in per-unit via the panel.
    virtual bool isWriteBackEnabled() const = 0;
    virtual void setWriteBackEnabled(bool on) = 0;

    /// Persist dirty blocks now. No-op when write-back is off or
    /// nothing is dirty. Returns false on I/O failure.
    virtual bool saveDirty() = 0;
};

/// Factory: create a fresh empty unit for the given kind key. Returns
/// nullptr for unknown keys. Concrete kinds live in their own .h/.cpp
/// pairs and the factory just lists them — keeps the dispatch
/// table in one place for slot config + settings restore.
std::unique_ptr<SmartPortUnit> makeSmartPortUnit(std::string_view kindKey);

} // namespace pom2

#endif // POM2_SMARTPORT_UNIT_H
