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

// FujiNetLink — the SmartPort command surface FujiNetCard talks to.
//
// The card issues SmartPort commands; it does not care that the far end is a
// SLIP frame over loopback, a USB CDC device or, in a test, a table of canned
// replies. SpOverSlipLink is the production implementation and keeps every
// transport concern — framing, timeouts, the helper process, resync — to
// itself.
//
// The shared wire vocabulary (SpCommand / SpError / SpDeviceType / SpDevice)
// lives here rather than in the transport, because it belongs to the protocol
// both sides speak.

#ifndef POM2_FUJINET_LINK_H
#define POM2_FUJINET_LINK_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

enum SpCommand : uint8_t {
    kSpStatus     = 0x00,
    kSpReadBlock  = 0x01,
    kSpWriteBlock = 0x02,
    kSpFormat     = 0x03,
    kSpControl    = 0x04,
    kSpInit       = 0x05,
    kSpOpen       = 0x06,
    kSpClose      = 0x07,
    kSpRead       = 0x08,
    kSpWrite      = 0x09,
};

/// SmartPort status/error codes POM2 produces itself (the peer supplies its
/// own for everything else). $27 = I/O error, $28 = no device connected.
enum SpError : uint8_t {
    kSpOk          = 0x00,
    kSpBadCommand  = 0x01,
    kSpIoError     = 0x27,
    kSpNoDevice    = 0x28,
    kSpWriteProt   = 0x2B,
};

/// DIB device-type bytes, verbatim from the FujiNet firmware's
/// `lib/bus/iwm/iwm.h` (SP_TYPE_BYTE_*). NOT the generic Apple IIgs SmartPort
/// type list — FujiNet allocates its own $1x range for its virtual devices.
enum SpDeviceType : uint8_t {
    kSpType35Disk   = 0x01,
    kSpTypeHardDisk = 0x02,
    kSpTypeScsi     = 0x03,
    kSpTypeFuji     = 0x10,   ///< the Fuji control device itself
    kSpTypeNetwork  = 0x11,   ///< the N: device
    kSpTypeCpm      = 0x12,
    kSpTypeClock    = 0x13,
    kSpTypePrinter  = 0x14,
    kSpTypeModem    = 0x15,
};

/// One device the peer reported, as decoded from its DIB (Status code $03).
struct SpDevice {
    uint8_t     unit     = 0;   ///< unit number the guest addresses
    std::string name;           ///< DIB ID string, trimmed
    uint8_t     type     = 0;
    uint8_t     subtype  = 0;
    uint32_t    blocks   = 0;   ///< 0 for character devices (N:, printer, …)

    /// Is this the printer?
    ///
    /// KEYED ON THE NAME, not the type byte, because the firmware's
    /// `iwmPrinter::create_dib_reply_packet` (lib/device/iwm/printer.cpp:32)
    /// sets `dib.type = SP_TYPE_BYTE_FUJINET_MODEM` — the printer advertises
    /// itself as a MODEM. That is an upstream copy-paste bug; the DIB name is
    /// "PRINTER" and is correct. The proper type byte is accepted as well, so
    /// this keeps working once upstream fixes it — and a real modem is not
    /// mistaken for a printer because its name is "MODEM".
    bool isPrinter() const
    { return name == "PRINTER" || type == kSpTypePrinter; }
};

/// The command surface. Every method is a SmartPort call; nothing here
/// exposes a socket, a thread or a frame.
class FujiNetLink
{
public:
    struct Response {
        bool                 replied = false;  ///< a matching frame came back
        uint8_t              status  = 0;      ///< SmartPort status from peer
        std::vector<uint8_t> data;
        bool ok() const { return replied && status == kSpOk; }
    };

    virtual ~FujiNetLink() = default;

    virtual bool                  isConnected() const = 0;
    virtual std::vector<SpDevice> devices() const = 0;
    virtual std::size_t           deviceCount() const = 0;

    virtual Response status(uint8_t unit, uint8_t statusCode) = 0;
    virtual Response readBlock(uint8_t unit, uint32_t block) = 0;
    virtual Response writeBlock(uint8_t unit, uint32_t block,
                                const uint8_t* data, std::size_t n) = 0;
    virtual Response format(uint8_t unit) = 0;
    virtual Response control(uint8_t unit, uint8_t controlCode,
                             const uint8_t* list, std::size_t n) = 0;
    virtual Response init(uint8_t unit) = 0;
    virtual Response open(uint8_t unit) = 0;
    virtual Response close(uint8_t unit) = 0;
    virtual Response read(uint8_t unit, uint16_t byteCount,
                          uint32_t address) = 0;
    virtual Response write(uint8_t unit, uint16_t byteCount,
                           uint32_t address, const uint8_t* data,
                           std::size_t n) = 0;

    /// A guest reset does not tear the link down — the peer keeps its state —
    /// but the card must tell it the guest restarted.
    virtual void notifyGuestReset() = 0;
    /// Re-enumerate after the peer's device list may have changed.
    virtual void resync() = 0;
};

} // namespace pom2

#endif // POM2_FUJINET_LINK_H
