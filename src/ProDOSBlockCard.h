// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ProDOSBlockCard — the image-management interface shared by the two HDV-class
// block-device cards: the synthetic ProDOSHardDiskCard (AppleWin lineage) and
// the MAME-faithful CffaCard (real firmware over an emulated ATA chip). The
// HDV Library, the disk-turbo poller, and settings persistence target a card
// through this interface so both kinds plug into the same UI uniformly.
//
// Both implementers are also SlotPeripherals; this is an orthogonal mix-in for
// the host (MainWindow) side, not the bus side.

#ifndef POM2_PRODOS_BLOCK_CARD_H
#define POM2_PRODOS_BLOCK_CARD_H

#include "MountableMediaCard.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class ProDOSBlockCard : public MountableMediaCard
{
public:
    virtual ~ProDOSBlockCard() = default;

    virtual int  getSlot() const = 0;

    virtual bool loadImage(const std::string& path) = 0;
    virtual bool loadImageFromBytes(std::vector<uint8_t> bytes,
                                    const std::string& label,
                                    const std::string& hostFolder) = 0;
    virtual void ejectImage() = 0;
    virtual bool saveDirty() = 0;

    virtual bool isImageLoaded() const = 0;
    virtual const std::string& getImagePath() const = 0;
    virtual const std::string& getLastError() const = 0;
    virtual size_t getBlockCount() const = 0;

    virtual bool isWriteProtected()   const = 0;
    virtual bool isWriteBackEnabled() const = 0;
    virtual void setWriteBackEnabled(bool on) = 0;
    virtual bool canWriteBack()       const = 0;
    virtual bool hasUnsavedChanges()  const = 0;

    virtual bool isBusy() const = 0;
    virtual void tickActivityDecay() = 0;

    // ── MountableMediaCard: one fixed bay over the single-image API. ────
    // Both HDV-class cards (ProDOSHardDiskCard, CffaCard) implement the
    // pure virtuals above, so they gain the bay interface here for free —
    // the Slot Manager renders them generically alongside SmartPort units.
    int bayCount() const override { return 1; }

    MediaBayInfo bayInfo(int bay) const override
    {
        MediaBayInfo info;
        if (bay != 0) return info;
        info.loaded            = isImageLoaded();
        info.path              = getImagePath();
        info.lastError         = getLastError();
        info.blockCount        = static_cast<uint32_t>(getBlockCount());
        info.writeProtected    = isWriteProtected();
        info.writeBackEnabled  = isWriteBackEnabled();
        info.supportsWriteBack = canWriteBack();
        return info;
    }

    bool mountBay(int bay, const std::string& path, std::string& errOut) override
    {
        if (bay != 0) { errOut = "invalid bay"; return false; }
        if (!loadImage(path)) { errOut = getLastError(); return false; }
        return true;
    }

    void ejectBay(int bay) override { if (bay == 0) ejectImage(); }
    void setBayWriteBack(int bay, bool on) override
    {
        if (bay == 0) setWriteBackEnabled(on);
    }
};

} // namespace pom2

#endif // POM2_PRODOS_BLOCK_CARD_H
