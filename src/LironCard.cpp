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

#include "LironCard.h"

#include "FloppySoundSink.h"
#include "Logger.h"
#include "ResourcePaths.h"

#include <cstdio>
#include <array>
#include <cstdlib>
#include <fstream>

namespace pom2 {

namespace {

// The BMOW / Yellowstone dump, whose layout the loader below assumes:
//   0x000-0x0FF  copyright string ("Firmware written by …"), NOT a slot page
//   0x100-0x7FF  seven $Cn00 pages, one per slot 1..7
//   0x800-0xFFF  the $C800-$CFFF expansion half
constexpr std::size_t kRomBytes      = 4096;
constexpr std::size_t kExpansionBase = 2048;
constexpr std::size_t kExpansionSize = 2048;

}  // namespace

LironCard::LironCard(int slot)
    : slot_(slot)
{
    for (int i = 0; i < kDrives; ++i) drives_[i].setImage(&images_[i]);

    // MAME-shaped callbacks, minus the MIG. `SmartPortHub` does the same job
    // for the //c+; a Liron's chain is simpler, so the card is its own hub.
    iwm_.setPhasesCallback([this](uint8_t p) { onPhases(p); });
    iwm_.setDevselCallback([this](uint8_t d) { onDevsel(d); });
    iwm_.setSel35Callback([](bool) {});   // no MIG, nothing to route

    const std::string path = pom2::findResource("roms/liron.rom");
    if (path.empty()) {
        lastError_ = "roms/liron.rom not found";
        pom2::log().warn("Liron", lastError_ + " — card is inert");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "cannot open " + path;
        pom2::log().warn("Liron", lastError_);
        return;
    }
    rom_.resize(kRomBytes);
    f.read(reinterpret_cast<char*>(rom_.data()),
           static_cast<std::streamsize>(kRomBytes));
    if (static_cast<std::size_t>(f.gcount()) != kRomBytes) {
        lastError_ = path + " is not a 4 KB dump";
        pom2::log().warn("Liron", lastError_);
        rom_.clear();
        return;
    }
    romLoaded_ = true;
    pom2::log().info("Liron",
        "Loaded real controller ROM (" + path + ") — slot " +
        std::to_string(slot_) + ", firmware-driven IWM");
}

void LironCard::setFloppySound(FloppySoundSink* fs)
{
    for (auto& d : drives_) d.setFloppySound(fs);
}

// ── The bus ──────────────────────────────────────────────────────────────

bool LironCard::busHandshakeActive() const
{
    // PH1 (CA1) and PH3 (LSTRB) both high, with the port enabled. That is
    // what $C800's scan asserts before polling SENSE, and holding LSTRB high
    // through a status read is something no disk transaction ever does — so
    // it is an unambiguous marker for "talking to the bus, not the disk".
    const uint8_t ph = iwm_.phases();
    const bool on = (ph & 0x02) && (ph & 0x08) && (iwm_.control() & 0x10);
    if (busTrace()) {
        static int lastPh = -1;
        if (ph != lastPh) {
            std::fprintf(stderr, "[SPBUS] phases=%X ctrl=%02X addressed=%d\n",
                         ph, iwm_.control(), on ? 1 : 0);
            lastPh = ph;
        }
    }
    return on;
}

void LironCard::busHostWrote(uint8_t v)
{
    // A write once a reply exists opens a NEW transaction. Without this the
    // command buffer accumulates every packet of the session, the reply stays
    // armed from the first one, and the firmware — which happily issues a
    // whole enumeration and then a boot read — gets the same answer forever.
    if (!busReply_.empty()) {
        busCommand_.clear();
        busReply_.clear();
        busReplyPos_ = 0;
    }
    busCommand_.push_back(v);
    busProgress_.commandTaken = true;
    busProgress_.commandBytes = busCommand_.size();
    if (busTrace()) std::fprintf(stderr, "[SPBUS] host -> %02X\n", v);
}

bool LironCard::busTrace()
{
    static const bool on = std::getenv("POM2_TRACE_SMARTPORT_BUS") != nullptr;
    return on;
}

namespace {

/// Decode one SmartPort bus packet body. The frame is
///   FF… $C3 | seven header bytes | odd section | groups | chk1 chk2 | $C8
/// where every byte on the wire carries bit 7 (that is what "byte ready"
/// means to the IWM shifter), the odd section is a high-bits byte followed by
/// `oddCount` bytes, and each group is a high-bits byte followed by seven.
/// The high-bits byte supplies bit 7 for the bytes that follow it, most
/// significant first — bit 6 for the first byte, and so on.
bool decodeBusPacket(const std::vector<uint8_t>& wire,
                     std::array<uint8_t, 7>& header,
                     std::vector<uint8_t>& body)
{
    std::size_t i = 0;
    while (i < wire.size() && wire[i] != 0xC3) ++i;
    if (i >= wire.size()) return false;          // no packet start
    ++i;
    if (wire.size() - i < 7) return false;
    for (int h = 0; h < 7; ++h) header[static_cast<std::size_t>(h)] =
        static_cast<uint8_t>(wire[i++] & 0x7F);
    const uint8_t oddCount   = header[5];
    const uint8_t groupCount = header[6];

    body.clear();
    if (oddCount) {
        if (i >= wire.size()) return false;
        const uint8_t high = wire[i++];
        for (int k = 0; k < oddCount; ++k) {
            if (i >= wire.size()) return false;
            const uint8_t bit = (high << (k + 1)) & 0x80;
            body.push_back(static_cast<uint8_t>((wire[i++] & 0x7F) | bit));
        }
    }
    for (int g = 0; g < groupCount; ++g) {
        if (i >= wire.size()) return false;
        const uint8_t high = wire[i++];
        for (int k = 0; k < 7; ++k) {
            if (i >= wire.size()) return false;
            const uint8_t bit = (high << (k + 1)) & 0x80;
            body.push_back(static_cast<uint8_t>((wire[i++] & 0x7F) | bit));
        }
    }
    return true;
}

/// Append `n` bytes as an odd section or as a group: the high-bits byte, then
/// the bytes with bit 7 forced. Mirrors the decoder above, and the ROM's own
/// tables at $CA27/$CA37/$CA47/$CA57 — those hold nothing but $80/$00 masks
/// keyed on the marker's bits, which is the same statement in silicon.
void appendBusGroup(std::vector<uint8_t>& out, const uint8_t* data, int n)
{
    uint8_t high = 0x80;
    for (int k = 0; k < n; ++k)
        if (data[k] & 0x80) high |= static_cast<uint8_t>(0x40 >> k);
    out.push_back(high);
    for (int k = 0; k < n; ++k)
        out.push_back(static_cast<uint8_t>(data[k] | 0x80));
}

}  // namespace

void LironCard::busBuildReply()
{
    // Reply framing, read out of the firmware's own decoder at $C985 and the
    // slot page's bulk reader at $C52B: $C3, then seven header bytes into
    // $0051 down to $004B (first byte on the wire lands highest), then
    // `$004C` odd bytes into the caller's buffer, then `$004B` groups of
    // seven, then the checksum as a 4-and-4 pair, then $C8.
    std::array<uint8_t, 7> hdr{};
    std::vector<uint8_t>   body;
    const bool parsed = decodeBusPacket(busCommand_, hdr, body);

    // Which call is this? The enumeration and the boot use the same wire and
    // the same routines, but NOT the same buffer: the scan's reply lands in a
    // few bytes of zero page, and answering it with 512 bytes walks over
    // $004B/$004C — the very fields that say how long the packet is. (That
    // mistake reads exactly like a checksum failure, which is a good way to
    // lose an hour.) The boot's parameter block at $CF16 is
    // `01 50 00 08 00 00 …`: command $01 = read a block, buffer $0800.
    const uint8_t cmd = (parsed && !body.empty()) ? body[0] : 0xFF;
    const bool    isRead = (cmd == 0x01);
    uint32_t block = 0;
    if (parsed && body.size() >= 7)
        block = static_cast<uint32_t>(body[4]) |
                (static_cast<uint32_t>(body[5]) << 8) |
                (static_cast<uint32_t>(body[6]) << 16);

    uint8_t sector[512] = {};
    bool haveBlock = false;
    if (isRead)
        haveBlock = images_[0].isLoaded() && block < Disk35Image::kBlockCount &&
                    images_[0].readBlock(block, sector);

    busProgress_.packetParsed = busProgress_.packetParsed || parsed;
    busProgress_.bodyBytes    = body.size();
    busProgress_.commandByte  = cmd;
    ++busProgress_.transactions;
    if (busTrace()) {
        std::fprintf(stderr, "[SPBUS] cmd=$%02X parsed=%d body=%zu", cmd,
                     parsed ? 1 : 0, body.size());
        if (isRead) std::fprintf(stderr, " block=%u served=%d", block,
                                 haveBlock ? 1 : 0);
        std::fprintf(stderr, "\n");
    }

    busReply_.clear();
    busReplyPos_ = 0;
    busReply_.insert(busReply_.end(), { 0xFF, 0xFF, 0xFF, 0xC3 });

    // 512 = 1 odd byte + 73 groups of seven; a status answer carries none.
    // The odd bytes land at the head of the caller's buffer and the groups
    // follow ($C9AC: $56 := $54 + $4C).
    const int kOdd    = isRead ? 1  : 0;
    const int kGroups = isRead ? 73 : 0;
    const std::array<uint8_t, 7> reply = {
        0x00,                              // → $0051
        0x00,                              // → $0050
        0x00,                              // → $004F
        0x00,                              // → $004E
        0x01,                              // → $004D — non-zero, or the scan
                                           //   discards the device
        static_cast<uint8_t>(kOdd),        // → $004C
        static_cast<uint8_t>(kGroups),     // → $004B
    };
    uint8_t checksum = 0;
    for (uint8_t b : reply) {
        const uint8_t wire = static_cast<uint8_t>(b | 0x80);
        busReply_.push_back(wire);
        checksum ^= wire;              // the header folds in as WIRE bytes
    }

    if (isRead) {
        appendBusGroup(busReply_, sector, kOdd);
        checksum ^= sector[0];
        for (int g = 0; g < kGroups; ++g) {
            const uint8_t* p = sector + kOdd + g * 7;
            appendBusGroup(busReply_, p, 7);
            for (int k = 0; k < 7; ++k) checksum ^= p[k];
        }
    }

    // 4-and-4: the receiver recovers C as ((chk2 << 1) | 1) & chk1.
    busReply_.push_back(static_cast<uint8_t>(checksum | 0xAA));
    busReply_.push_back(static_cast<uint8_t>((checksum >> 1) | 0xAA));
    busReply_.push_back(0xC8);

    if (busTrace())
        std::fprintf(stderr, "[SPBUS] reply armed (%zu bytes)\n",
                     busReply_.size());
}

bool LironCard::busHostReads(uint8_t& out)
{
    if (busReplyPos_ >= busReply_.size()) return false;
    out = busReply_[busReplyPos_++];
    return true;
}

uint8_t LironCard::deviceSelectRead(uint8_t low4)
{
    if (!romLoaded_) return 0xFF;
    iwm_.tick(cycles_);
    // SEL is sampled on every access rather than only on a devsel edge: the
    // firmware flips it ($C0nA / $C0nB) to pick the head *without* changing
    // the selected drive, and on a Sony that same line is bit 3 of the
    // register address. Reading a register with a stale SEL answers for the
    // wrong side of the disk.
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
    const uint8_t v = iwm_.read(low4);
    if (!busEnabled_) return v;
    busAddressed_ = busHandshakeActive();
    // The receive routine's cue: after sending a command the firmware calls
    // $C960, which re-asserts PH1+LSTRB and then READS $C0nD before it starts
    // waiting for SENSE. Nothing else in the transaction reads that offset,
    // and the phase lines stay up across the whole exchange — so an edge on
    // the handshake is not available to trigger on, but this access is.
    if (low4 == 0x0D && !busCommand_.empty() && busReply_.empty())
        busBuildReply();

    // The phase pattern ADDRESSES the device; it does not gate every byte.
    // The firmware drops PH1 as soon as it starts reading the reply ($C982
    // `LDA $C081,X`), so a per-access gate on the handshake hands the rest of
    // the packet back to an empty IWM. A transaction stays live from the
    // first command byte until the last reply byte has been taken.
    const bool inTransaction = !busCommand_.empty() ||
                               busReplyPos_ < busReply_.size();
    if (!busAddressed_ && !inTransaction) return v;

    // Q6/Q7 both low = the data register. In write mode the firmware polls it
    // for the handshake's "ready" bit; in read mode it wants the device's
    // next byte. Bit 7 means "there is one" in both directions, which is why
    // every SmartPort byte on the wire has it set.
    if ((iwm_.control() & 0xC0) == 0x00) {
        uint8_t reply = 0;
        if (busHostReads(reply)) {
            if (busReplyPos_ >= busReply_.size()) {
                busProgress_.replyDelivered = true;
                if (busTrace())
                    std::fprintf(stderr, "[SPBUS] reply fully read\n");
            }
            return reply;
        }
        return 0x00;                      // nothing to say yet
    }
    if ((iwm_.control() & 0xC0) == 0x80) {
        // Write handshake. Bit 7 = "latch free, send the next byte"; bit 6 is
        // the underrun flag the firmware waits to see CLEAR after its last
        // byte ("has the shifter drained?"). POM2 takes bytes instantly and
        // has nothing in flight, so both answers are "yes, go on" — leaving
        // bit 6 set parks the firmware in the drain loop at $C92C forever.
        return 0x80;
    }
    if ((iwm_.control() & 0xC0) == 0x40) {
        // Status: bit 7 is SENSE, which on this bus is the device's
        // attention line rather than a disk's write-protect. High says "a
        // device is here" to the scan's poll; it must drop once the command
        // packet has been taken, because the firmware's next loop waits for
        // exactly that before it reads the reply.
        // High before a command (presence) and again once a reply is armed
        // (attention); low in between, which is the acknowledgement the
        // firmware waits for after sending.
        const bool sense = busCommand_.empty() ||
                           (busReplyPos_ < busReply_.size());
        if (sense && busCommand_.empty()) busProgress_.probeAnswered = true;
        return static_cast<uint8_t>((v & 0x7F) | (sense ? 0x80 : 0x00));
    }
    return v;
}

void LironCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (!romLoaded_) return;
    iwm_.tick(cycles_);
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
    // The access itself moves Q6/Q7, so what makes a write a DATA write is
    // the control register AFTER it — which is how the IWM decides too
    // (controlAccess → dataW). Testing it before missed every byte whose own
    // access completed the Q6+Q7 pair, and that includes the packet's first
    // bytes: the sender establishes write mode with `STA $C08F,X` carrying
    // the first sync byte.
    iwm_.write(low4, v);
    busAddressed_ = busEnabled_ && busHandshakeActive();
    // …and only while the drive is enabled. The IWM makes the same test —
    // an odd-offset write with Q6+Q7 is a DATA byte when the device is
    // active and the MODE register otherwise — and mistaking the mode write
    // for a command byte poisons the packet before it starts, which then
    // reads as "the probe failed".
    if (busEnabled_ && !iwm_.isIdle() &&
        (iwm_.control() & 0xC0) == 0xC0 && (low4 & 1))
        busHostWrote(v);
}

uint8_t LironCard::slotRomRead(uint8_t low8)
{
    if (!romLoaded_ || slot_ < 1 || slot_ > 7) return 0xFF;
    return rom_[static_cast<std::size_t>(slot_) * 256 + low8];
}

uint8_t LironCard::expansionRomRead(uint16_t offset)
{
    if (!romLoaded_ || offset >= kExpansionSize) return 0xFF;
    return rom_[kExpansionBase + offset];
}

void LironCard::advanceCycles(int cycles)
{
    if (!romLoaded_ || cycles <= 0) return;
    cycles_ += static_cast<uint64_t>(cycles);
    // Same reason EmulationController ticks the //c+'s IWM once a frame: the
    // drive-disable timer has to drain even while the firmware is off doing
    // something else, or the motor never stops.
    iwm_.tick(cycles_);
}

void LironCard::onReset()
{
    iwm_.reset();
    for (auto& d : drives_) d.reset();
    active_ = -1;
    busAddressed_ = false;
    busCommand_.clear();
    busReply_.clear();
    busReplyPos_ = 0;
    busProgress_ = BusProgress{};
    retargetIwm();
}

// ── The chain ────────────────────────────────────────────────────────────

void LironCard::onDevsel(uint8_t devsel)
{
    // EXPERIMENT: SEL is head select on this card, so devsel must not pick
    // the drive. Both enables land on the first bay.
    const int want = (devsel != 0) ? 0 : -1;
    if (want == active_) return;
    active_ = want;
    retargetIwm();
}

void LironCard::retargetIwm()
{
    // Unlike the //c+ hub there is no 5.25" path to protect here, so the
    // deselected case really does clear the IWM's floppy: a Liron with no
    // drive enabled must read as an empty spindle, not as whatever was
    // under the head last time.
    iwm_.setSony35(active_ >= 0 ? &drives_[active_] : nullptr);
    if (active_ >= 0) {
        drives_[active_].setSel(iwm_.sel());
        drives_[active_].ssW(iwm_.sel());
    }
}

void LironCard::onPhases(uint8_t phases)
{
    // Every drive on the chain, selected or not. CA0-CA2 and LSTRB are wired
    // straight through to the connector, so a drive sees them whether or not
    // its /ENBL is asserted — and the firmware sets the register address up
    // BEFORE it enables the drive. Forwarding only to the active drive threw
    // that address away and left every sense read answering for register 0.
    for (auto& d : drives_) {
        d.setSel(iwm_.sel());
        d.seekPhaseW(phases, iwm_.emuCycles());
    }
}

// ── Media bays ───────────────────────────────────────────────────────────

MediaBayInfo LironCard::bayInfo(int bay) const
{
    MediaBayInfo info;
    if (bay < 0 || bay >= kDrives) return info;
    const Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    info.kindLabel         = "3.5\" 800K";
    info.path              = img.path();
    info.lastError         = img.lastError();
    info.blockCount        = img.isLoaded() ? Disk35Image::kBlockCount : 0;
    info.loaded            = img.isLoaded();
    info.writeProtected    = img.isWriteProtected();
    info.writeBackEnabled  = img.isWriteBackEnabled();
    info.hasUnsavedChanges = img.hasUnsavedChanges();
    info.supportsWriteBack = true;
    return info;
}

bool LironCard::mountBay(int bay, const std::string& path, std::string& errOut)
{
    if (bay < 0 || bay >= kDrives) { errOut = "no such bay"; return false; }
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    if (!img.loadFile(path)) {
        errOut = img.lastError();
        return false;
    }
    // The drive has to be told, or its cached bit-cell stream still holds the
    // previous disk — and the firmware's media-change probe never fires.
    drives_[static_cast<std::size_t>(bay)].notifyMediaChange();
    errOut.clear();
    return true;
}

bool LironCard::ejectBay(int bay)
{
    if (bay < 0 || bay >= kDrives) return false;
    Disk35Image& img = images_[static_cast<std::size_t>(bay)];
    if (img.hasUnsavedChanges() && !img.isWriteProtected()) img.saveDirty();
    img.eject();
    drives_[static_cast<std::size_t>(bay)].notifyMediaChange();
    return true;
}

void LironCard::setBayWriteBack(int bay, bool on)
{
    if (bay < 0 || bay >= kDrives) return;
    images_[static_cast<std::size_t>(bay)].setWriteBackEnabled(on);
}

}  // namespace pom2
