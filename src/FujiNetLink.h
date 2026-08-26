// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Device-side FujiNet protocol contract. It deliberately exposes neither
// threads, sockets, serial ports nor helper processes. Runtime adapters such
// as SpOverSlipLink implement this seam.

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

enum SpError : uint8_t {
    kSpOk          = 0x00,
    kSpBadCommand  = 0x01,
    kSpIoError     = 0x27,
    kSpNoDevice    = 0x28,
    kSpWriteProt   = 0x2B,
};

enum SpDeviceType : uint8_t {
    kSpType35Disk   = 0x01,
    kSpTypeHardDisk = 0x02,
    kSpTypeScsi     = 0x03,
    kSpTypeFuji     = 0x10,
    kSpTypeNetwork  = 0x11,
    kSpTypeCpm      = 0x12,
    kSpTypeClock    = 0x13,
    kSpTypePrinter  = 0x14,
    kSpTypeModem    = 0x15,
};

struct SpDevice {
    uint8_t     unit    = 0;
    std::string name;
    uint8_t     type    = 0;
    uint8_t     subtype = 0;
    uint32_t    blocks  = 0;

    // Some FujiNet firmware advertises PRINTER with the modem type byte.
    // Its stable DIB name is authoritative; the correct type remains valid.
    bool isPrinter() const
    { return name == "PRINTER" || type == kSpTypePrinter; }
};

class FujiNetLink
{
public:
    struct Response {
        bool                 replied = false;
        uint8_t              status  = 0;
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

    virtual void notifyGuestReset() = 0;
    virtual void resync() = 0;
};

} // namespace pom2

#endif // POM2_FUJINET_LINK_H
