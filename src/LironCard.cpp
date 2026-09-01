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
    for (int i = 0; i < kDrives; ++i) {
        drives_[i].setImage(&images_[i]);
        busUnits_[static_cast<std::size_t>(i)].bind(&images_[i]);
        bus_.setUnit(i, &busUnits_[static_cast<std::size_t>(i)]);
    }
    bus_.setUnitCount(kDrives);

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

bool LironCard::busAddressed() const
{
    // PH1 (CA1) and PH3 (LSTRB) both high, with the port enabled. That is
    // what $C800's scan asserts before polling SENSE, and holding LSTRB high
    // through a status read is something no disk transaction ever does — so
    // it is an unambiguous marker for "talking to the bus, not the disk".
    const uint8_t ph = iwm_.phases();
    return (ph & 0x02) && (ph & 0x08) && (iwm_.control() & 0x10);
}

bool LironCard::busLive() const
{
    return busEnabled_ && bus_.anyMedia();
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
    if (!busLive()) return v;
    // The phase pattern ADDRESSES the device; it does not gate every byte.
    // The firmware drops PH1 as soon as it starts reading the reply ($C982
    // `LDA $C081,X`), so a per-access gate on the handshake would hand the
    // rest of the packet back to an empty IWM. A transaction stays routed
    // from its first byte until the reply is consumed and REQ released.
    if (!busAddressed() && !bus_.active()) return v;

    switch (iwm_.control() & 0xC0) {
    case 0x00: {
        // Q6/Q7 both low = the data register in read mode: the device's next
        // byte, bit 7 meaning "there is one" — which is why every SmartPort
        // byte on the wire has it set. Nothing to say reads as $00.
        uint8_t b = 0;
        return bus_.hostReads(b) ? b : uint8_t{0x00};
    }
    case 0x80:
        // Write handshake. Bit 7 = "latch free, send the next byte"; bit 6 is
        // the underrun flag the firmware waits to see CLEAR after its last
        // byte ("has the shifter drained?"). POM2 takes bytes instantly and
        // has nothing in flight, so both answers are "yes, go on" — leaving
        // bit 6 set parks the firmware in the drain loop at $C92C for ever.
        return 0x80;
    case 0x40:
        // Status: bit 7 is SENSE, which on this bus is the device's ACK line
        // rather than a disk's write-protect. See SmartPortBusDevice for the
        // handshake it follows.
        return static_cast<uint8_t>((v & 0x7F) | (bus_.sense() ? 0x80 : 0x00));
    default:
        return v;
    }
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
    // …and only while the drive is enabled. The IWM makes the same test —
    // an odd-offset write with Q6+Q7 is a DATA byte when the device is
    // active and the MODE register otherwise — and mistaking the mode write
    // for a command byte poisons the packet before it starts, which then
    // reads as "the probe failed". The address gate keeps a Sony GCR write
    // (same register, same mode, no bus handshake) out of the packet buffer.
    if (busLive() && !iwm_.isIdle() &&
        (iwm_.control() & 0xC0) == 0xC0 && (low4 & 1) &&
        (busAddressed() || bus_.active()))
        bus_.hostWrote(v);
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
    bus_.reset();
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
    if (busLive()) {
        // An intelligent drive on the chain: the phase lines are the bus's
        // control lines, not a stepper's. PH0 is REQ; PH0 + PH2 together is
        // the bus reset the firmware issues before an INIT scan ($C9E5).
        if ((phases & 0x05) == 0x05) bus_.busReset();
        bus_.reqChanged((phases & 0x01) != 0);
        return;
    }
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
