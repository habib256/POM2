// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MountableMediaCard — host-side capability mix-in for any slot card that
// exposes one or more *mountable media bays* (a hard-disk image, a 3.5"
// volume, a SmartPort unit, …). It lets the GUI — chiefly the consolidated
// Slot Manager panel — render and drive media on ANY such card generically,
// without a per-card-type `if (cardKey == "...")` ladder.
//
// This is the same "orthogonal host-side mix-in, not a bus concern" pattern
// as ProDOSBlockCard.h: implementers are also SlotPeripherals, but the bus
// dispatch path never touches this interface.
//
// Relationship to ProDOSBlockCard: ProDOSBlockCard already exposes a full
// single-image API (loadImage / ejectImage / getBlockCount / write-back …),
// so it *implements* MountableMediaCard here as a single fixed bay. The two
// HDV-class cards (ProDOSHardDiskCard, CffaCard) therefore gain the bay
// interface for free. SmartPortCard implements it directly over its 2 units,
// advertising per-bay type selection (empty / 3.5" / HDV).

#ifndef POM2_MOUNTABLE_MEDIA_CARD_H
#define POM2_MOUNTABLE_MEDIA_CARD_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pom2 {

/// Read-only view of one media bay, built by the card for the GUI snapshot.
struct MediaBayInfo
{
    std::string kindLabel;        // media kind in this bay ("3.5\" 800K",
                                  // "ProDOS HDV", …); empty = use card label
    std::string path;             // mounted image path ("" = nothing mounted)
    std::string lastError;        // last mount/load error ("" = none)
    uint32_t    blockCount       = 0;
    bool        loaded           = false;
    bool        writeProtected   = false;
    bool        writeBackEnabled  = false;
    bool        supportsWriteBack = true;
    // True when the user may CHOOSE the media kind for this bay (SmartPort
    // units: empty / 3.5" / HDV). Block cards have a fixed kind → false.
    bool        supportsTypeSelect = false;
    std::string typeKey;          // current bay type key ("" / "35" / "hdv")
};

class MountableMediaCard
{
public:
    virtual ~MountableMediaCard() = default;

    /// Number of mountable bays. HDV/CFFA = 1, SmartPort = 2.
    virtual int bayCount() const = 0;

    /// Snapshot of bay `bay` (0-based). Out-of-range → default-constructed.
    virtual MediaBayInfo bayInfo(int bay) const = 0;

    /// Mount `path` into `bay`. On failure returns false and fills `errOut`.
    virtual bool mountBay(int bay, const std::string& path,
                          std::string& errOut) = 0;

    /// Eject `bay` (saves dirty blocks first when write-back is on).
    virtual void ejectBay(int bay) = 0;

    /// Toggle per-bay write-back (save-on-eject).
    virtual void setBayWriteBack(int bay, bool on) = 0;

    /// Type-select bays only: the (key,label) options the user may pick
    /// (e.g. {{"","(empty)"},{"35","3.5\" 800K"},{"hdv","ProDOS HDV"}}).
    /// Default: none (fixed-kind card).
    virtual std::vector<std::pair<std::string, std::string>>
    bayTypeOptions(int /*bay*/) const { return {}; }

    /// Type-select bays only: swap the media kind in `bay` to `kindKey`
    /// (drops any currently-mounted media). No-op for fixed-kind cards.
    virtual void setBayType(int /*bay*/, const std::string& /*kindKey*/) {}
};

} // namespace pom2

#endif // POM2_MOUNTABLE_MEDIA_CARD_H
