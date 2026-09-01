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
// IIcExternalSmartPort — the plain //c's external disk port, as far as an
// intelligent 3.5" drive is concerned.
//
// On a //c the same IWM drives the internal 5.25" and the rear connector; the
// enable line picks the drive. POM2 keeps the 5.25" on `DiskIICard` (its LSS
// is the write authority everywhere, and a second controller on those soft
// switches once sent DOS 3.3 into seek storms — `iic_diskii_no_iwm_conflict`
// pins that the machine's shared IWM stays out of it). So this port carries
// its OWN IWM, used as nothing more than a register tracker — Q6/Q7, enable,
// SEL, the phases — with no mechanism attached, and a `SmartPortBusDevice`
// that answers the bytes. It claims an access only while the firmware is
// addressing the bus (PH1 + LSTRB high with the port enabled) or a
// transaction is in flight, and only while a unit holds media; every other
// access falls through to the Disk II exactly as before.
//
// The units come from whatever card sits in the machine's slot 5 — the
// built-in SmartPort the //c profiles plug there — through
// `SlotPeripheral::smartPortBusUnit`, so the media panel is unchanged: what
// the user mounts on "slot 5" is what the firmware finds on the port.

#ifndef POM2_IIC_EXTERNAL_SMARTPORT_H
#define POM2_IIC_EXTERNAL_SMARTPORT_H

#include "IWMDevice.h"
#include "SmartPortBusDevice.h"
#include "SmartPortBusPort.h"

#include <cstdint>

class SlotBus;

namespace pom2 {

class IIcExternalSmartPort final : public SmartPortBusPort {
public:
    static constexpr int kDefaultSlot = 5;

    explicit IIcExternalSmartPort(SlotBus* slots, int slot = kDefaultSlot);

    bool live() override;
    bool read(uint8_t offset, uint64_t cycles, uint8_t& out) override;
    void write(uint8_t offset, uint8_t value, uint64_t cycles) override;
    void reset() override;

    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }

    const SmartPortBusDevice& device() const { return bus_; }
    const IWMDevice&          registers() const { return regs_; }

private:
    SlotBus*  slots_;
    int       slot_;
    bool      enabled_ = true;
    IWMDevice regs_;
    SmartPortBusDevice bus_;

    /// Refresh the unit list from the slot card. True when a card offers any.
    bool bind();
    bool addressed() const;
};

}  // namespace pom2

#endif  // POM2_IIC_EXTERNAL_SMARTPORT_H
