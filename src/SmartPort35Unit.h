// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPort35Unit — wraps an owned `Disk35Image` for plug-in into a
// `SmartPortCard` chain. Owned (not borrowed) so each Liron-card unit
// has independent media — the //c+ on-board hub keeps its own pair
// of `Disk35Image` instances via `EmulationController`. On a //c+
// profile the slot card isn't plugged anyway (built-in slot config),
// so no sharing concern.
//
// Block layout: 800 K = 1600 × 512 B. Sony 3.5" media only; raw 5.25"
// won't load through this unit.

#ifndef POM2_SMARTPORT_35_UNIT_H
#define POM2_SMARTPORT_35_UNIT_H

#include "SmartPortUnit.h"
#include "Disk35Image.h"

#include <memory>

namespace pom2 {

class SmartPort35Unit : public SmartPortUnit
{
public:
    static constexpr std::string_view kKindKey   = "35";
    static constexpr std::string_view kKindLabel = "3.5\" 800K";

    SmartPort35Unit();
    ~SmartPort35Unit() override;

    std::string_view kindKey()   const override { return kKindKey; }
    std::string_view kindLabel() const override { return kKindLabel; }

    bool     isLoaded()         const override { return img_.isLoaded(); }
    bool     isWriteProtected() const override { return img_.isWriteProtected(); }
    uint32_t blockCount()       const override;
    bool     readBlock (uint32_t idx, uint8_t* out) const override;
    bool     writeBlock(uint32_t idx, const uint8_t* in) override;

    bool     loadImage(const std::string& path) override;
    void     eject() override;
    const std::string& path()      const override { return img_.path(); }
    const std::string& lastError() const override { return lastError_; }

    bool     isWriteBackEnabled() const override { return img_.isWriteBackEnabled(); }
    void     setWriteBackEnabled(bool on) override { img_.setWriteBackEnabled(on); }
    bool     saveDirty() override { return img_.saveDirty(); }

    /// Escape hatch for the panel UI when it needs to talk to the
    /// underlying image (e.g. for tracks-changed indicators). Borrowed.
    Disk35Image& image() { return img_; }
    const Disk35Image& image() const { return img_; }

private:
    Disk35Image img_;
    std::string lastError_;
};

} // namespace pom2

#endif // POM2_SMARTPORT_35_UNIT_H
