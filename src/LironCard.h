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

// LironCard — the Apple II 3.5" Disk Controller (Apple 670-0186, "Liron"),
// running its OWN firmware over a real IWM.
//
// POM2 already had a card called SmartPortCard on this hardware's name, and
// the two are not rivals: that one answers ProDOS's block calls from the
// host with an invented streaming port, borrowing only the Liron dump's
// identity bytes. It works, it needs no bit-cell emulation, and it is the
// right thing for a user who wants a 3.5" volume on a //e. This is the other
// half — the card as silicon:
//
//   * the real 4 KB EPROM (`roms/liron.rom`, the BMOW/Yellowstone dump)
//     executing on the 6502, both its $Cn00 page and its $C800 half;
//   * a real `IWMDevice` behind $C0nX, which the firmware drives itself;
//   * `Sony35Drive` mechanisms with zoned GCR under the head.
//
// Nothing is served from the host: ProDOS boots because the firmware read
// the sectors. That is the whole point, and it is only worth having because
// it is now possible — the IWM's bit-cell walker could not recover a Sony
// sector until 2026-09-01 (see `sony35_iwm_read_path`), so a card written
// before that would have been a card that did not boot.
//
// **Wiring, and the one thing that differs from the //c+.** On a //c+ the
// MIG gate array selects the drive and drives head-select. A Liron has no
// MIG: the IWM's own SEL line (control bit 5, $C0nA/$C0nB) is head select,
// and it is also bit 3 of the Sony's register address — `regSelect()` is
// `{ HDSEL, CA2, CA1, CA0 }`. So SEL is forwarded to `Sony35Drive::ssW`
// here, where `SmartPortHub` forwards the MIG's $C240/$C260 instead.
//
// Deliberately out of scope, as TODO § Storage has always said: the UniDisk
// 3.5's drive-side 65C02. This card drives the *dumb* Apple 3.5 Drive, which
// is what the firmware's GCR path talks to; an intelligent UniDisk would
// need its own processor emulated inside the drive.

#ifndef POM2_LIRON_CARD_H
#define POM2_LIRON_CARD_H

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "MountableMediaCard.h"
#include "SlotPeripheral.h"
#include "Sony35Drive.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class FloppySoundSink;

namespace pom2 {

class LironCard : public SlotPeripheral, public MountableMediaCard
{
public:
    static constexpr int kDefaultSlot = 5;
    static constexpr int kDrives      = 2;   // the port daisy-chains two

    explicit LironCard(int slot = kDefaultSlot);

    // ── SlotPeripheral ───────────────────────────────────────────────────
    std::string_view name() const override { return "Apple II 3.5\" (Liron)"; }

    /// $C0nX — the IWM, all sixteen registers, no interception. The
    /// firmware's mode/status/handshake dance is its own business.
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;

    /// $Cn00 — the dump's per-slot page. The EPROM carries eight of them
    /// (offsets 0x100..0x7FF) that differ only in the slot number they load
    /// into X, so the firmware knows where it lives without self-modifying
    /// code. Offset 0x000 is a copyright string, not a page.
    uint8_t slotRomRead(uint8_t low8) override;

    /// $C800-$CFFF — the upper 2 KB of the dump, where the GCR routines and
    /// the SmartPort dispatcher live.
    uint8_t expansionRomRead(uint16_t offset) override;

    void advanceCycles(int cycles) override;
    void onReset() override;

    // ── MountableMediaCard ───────────────────────────────────────────────
    // Two fixed 3.5" bays, the daisy chain the real port carries. No type
    // select (a Liron drives 3.5" mechanisms and nothing else) and no
    // two-phase mount: a `Disk35Image` has no `Block512Backing` to prepare
    // off the lock, so the base class's opt-in defaults decline and callers
    // fall back to `mountBay` — the same path `SmartPortCard`'s 3.5" unit
    // takes today.
    int          bayCount() const override { return kDrives; }
    MediaBayInfo bayInfo(int bay) const override;
    bool         mountBay(int bay, const std::string& path,
                          std::string& errOut) override;
    bool         ejectBay(int bay) override;
    void         setBayWriteBack(int bay, bool on) override;

    /// True once the EPROM was found and loaded. Without it the card is
    /// inert — there is no synthetic fallback here on purpose: a synthesised
    /// ROM would be a different card, and POM2 already has that card
    /// (`SmartPortCard`).
    bool romLoaded() const { return romLoaded_; }
    const std::string& lastError() const { return lastError_; }

    /// Enable the byte-level SmartPort **bus** responder.
    ///
    /// OFF by default, and that default is the honest one: the responder is
    /// unfinished. It answers the presence handshake and completes one
    /// command/header exchange with the firmware — real, hard-won, and
    /// traced — but it cannot yet decode a command or serve a block payload,
    /// so the firmware advances one step further and then waits. With it off,
    /// the card behaves like a Liron with nothing on its port: the scan
    /// reports ProDOS $28 and the machine carries on, which is a defensible
    /// state to ship. With it on, you can watch the protocol
    /// (`POM2_TRACE_SMARTPORT_BUS=1`) and pick the work up where it stopped.
    ///
    /// When the //c needs this too — its bank-1 firmware is the same code —
    /// this belongs in its own translation unit rather than in a card.
    void setBusResponderEnabled(bool on) { busEnabled_ = on; }
    bool busResponderEnabled() const { return busEnabled_; }

    /// How far the last bus exchange got, for tests and diagnostics.
    struct BusProgress {
        bool     probeAnswered  = false;  // SENSE went high for the scan
        bool     commandTaken   = false;  // the host sent a command packet
        bool     packetParsed   = false;  // …and it decoded as a packet
        bool     replyDelivered = false;  // the reply was read back in full
        std::size_t commandBytes = 0;
        std::size_t bodyBytes    = 0;
        uint8_t     commandByte  = 0xFF;  // body[0]
        int         transactions = 0;
    };
    BusProgress busProgress() const { return busProgress_; }

    /// Mechanical sound sink, shared with the rest of the 3.5" stack.
    void setFloppySound(FloppySoundSink* fs);

    int slot() const { return slot_; }

    // Diagnostics for tests and the inspector.
    const IWMDevice&   iwm()      const { return iwm_; }
    const Sony35Drive& drive(int i) const { return drives_[i]; }

private:
    int  slot_       = kDefaultSlot;
    bool romLoaded_  = false;
    std::string lastError_;
    std::vector<uint8_t> rom_;          // the 4 KB dump, verbatim

    IWMDevice                          iwm_;
    std::array<Disk35Image, kDrives>   images_;
    std::array<Sony35Drive, kDrives>   drives_;

    // ── The SmartPort bus, at the byte level ─────────────────────────────
    // The firmware's device scan is not talking to a disk: it drives PH1 and
    // LSTRB high, then exchanges BYTES through the IWM's data register with
    // an intelligent device (a UniDisk 3.5 carries its own 65C02). POM2 has
    // no such drive and will not emulate that processor — but the bus
    // protocol itself is a byte stream, so it can be answered at the byte
    // level, which is the same seam `SmartPortCard` already uses one layer
    // up. See docs/lle_vs_hle.md.
    //
    // `busAddressed_` is the handshake POM2 answers: PH1 + LSTRB high with
    // the drive enabled. While it holds, this card substitutes its own bytes
    // for the IWM's on the data register — the IWM still sees every access,
    // so its control/mode state stays live and the disk path is untouched.
    bool busEnabled_   = false;
    bool busAddressed_ = false;
    BusProgress busProgress_;
    /// Bytes the host has written since the last packet terminator ($C8).
    std::vector<uint8_t> busCommand_;
    /// What the device is still to hand back, front first.
    std::vector<uint8_t> busReply_;
    std::size_t          busReplyPos_ = 0;

    static bool busTrace();
    void busBuildReply();
    bool busHandshakeActive() const;
    void busHostWrote(uint8_t v);
    bool busHostReads(uint8_t& out);

    /// Which drive the IWM's devsel currently points at, or -1 for none.
    /// The Liron's port is a daisy chain: devsel 1 is the first drive, 2 the
    /// second, and 0 means the IWM has dropped both (motor-off drain).
    int      active_ = -1;
    uint64_t cycles_ = 0;

    void onPhases(uint8_t phases);
    void onDevsel(uint8_t devsel);
    void retargetIwm();
};

}  // namespace pom2

#endif  // POM2_LIRON_CARD_H
