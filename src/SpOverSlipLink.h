// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpOverSlipLink — the SmartPort-over-SLIP session layer.
//
// Owns everything between "a byte pipe" (SpTransport) and "a SmartPort call"
// (FujiNetCard): request sequence numbers, the bounded round trip, device
// enumeration, and the worker thread that watches for a peer appearing or
// dying. It knows nothing about the 6502 and nothing about which transport it
// holds.
//
// Protocol reference: the FujiNet wiki page "Apple II SP over SLIP"
// (https://github.com/FujiNetWIFI/fujinet-firmware/wiki/Apple-II-SP-over-SLIP,
// revision of 2025-01-25), which supplements the Apple IIc Technical Reference
// ch. 6 and the Apple IIgs Firmware Reference ch. 7. Cross-checked against the
// FujiNet AppleWin fork's `source/devrelay/`.
//
// ── Wire format ───────────────────────────────────────────────────────────
//
// Every request is an 11-byte header, optionally followed by data; every
// response is a 2-byte header, optionally followed by data. The header's
// first byte is always the REQUEST SEQUENCE NUMBER, which exists for one
// specific failure: an Apple II reset after a request was sent but before its
// response was read leaves that response sitting in a TCP or USB buffer, so
// the NEXT call would read somebody else's answer. Matching the number on the
// way back is what makes a guest reset survivable.
//
//   off  0    request sequence number
//   off  1    command number
//   off  2    parameter count
//   off  3    SmartPort unit number
//   off  4-5  reserved ($00)
//   off  6-10 command-specific (status code / block number / byte count +
//             address), zero-padded
//
// Multi-byte fields are LITTLE-ENDIAN, matching the SmartPort parameter lists
// they are copied from (block number low/mid/high, as ProDOS lays it out at
// $46/$47).
//
// ── Threading ─────────────────────────────────────────────────────────────
//
// `transact()` and every typed call run on the CPU thread, inside a SmartPort
// call, under EmulationController's stateMutex — blocking, bounded by
// `timeoutMs()` (250 ms by default). See docs/fujinet_plan.md § 9 for why
// blocking there is the right answer and not a compromise.
//
// The worker thread only acquires and releases peers, and enumerates devices
// when one appears. Calls from the two threads serialise on `callMtx_`, so
// two requests can never be in flight at once and a response can never be
// matched to the wrong caller.

#ifndef POM2_SP_OVER_SLIP_LINK_H
#define POM2_SP_OVER_SLIP_LINK_H

#include "SlipFramer.h"
#include "SpTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {

/// SmartPort command numbers, as the spec's tables name them.
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

class SpOverSlipLink
{
public:
    /// Default round-trip budget. Loopback answers in microseconds and a real
    /// board over USB in single-digit milliseconds, so this is ~50x headroom
    /// for the normal case while keeping a dead peer to a quarter-second
    /// stall — one dropped frame, once, instead of a full second.
    static constexpr int kDefaultTimeoutMs = 250;
    static constexpr int kMinTimeoutMs     = 50;
    static constexpr int kMaxTimeoutMs     = 5000;

    /// Ceiling on the INIT enumeration sweep.
    ///
    /// This was 8, and 8 is wrong for the device POM2 exists to talk to. A
    /// FujiNet's SmartPort chain is not "up to eight drives": it is the disk
    /// slots FOLLOWED BY the Fuji control device, the `N:` network device,
    /// the clock, the printer, the modem and CP/M (lib/device/iwm/ in the
    /// firmware — disk, iwmFuji, network, clock, modem, cpm). With the sweep
    /// stopping at 8 the enumeration saw eight FUJINET_DISK_* units and
    /// nothing else, and — because the guest's "how many devices?" call is
    /// answered locally from that count — the guest never probed past unit 8
    /// either. Every non-disk FujiNet function was therefore invisible to
    /// guest software: NETCAT got as far as "NETWORK NOT FOUND".
    ///
    /// The sweep stops on the first unit whose INIT does not answer $00, so
    /// this is only a safety ceiling, not a cost: a chain of two still costs
    /// two round trips. SmartPort itself addresses up to 127 units; 32 is far
    /// past anything a FujiNet presents while still bounding a peer that
    /// answers $00 to everything.
    static constexpr uint8_t kMaxUnits = 32;

    struct Response {
        bool                 replied = false;  ///< a matching frame came back
        uint8_t              status  = 0;      ///< SmartPort status from peer
        std::vector<uint8_t> data;
        /// True only when the peer answered AND reported success.
        bool ok() const { return replied && status == kSpOk; }
    };

    struct Stats {
        uint64_t calls    = 0;
        uint64_t timeouts = 0;
        uint64_t stale    = 0;   ///< responses discarded on sequence mismatch
        uint64_t bytesOut = 0;
        uint64_t bytesIn  = 0;
    };

    SpOverSlipLink();
    ~SpOverSlipLink();

    SpOverSlipLink(const SpOverSlipLink&)            = delete;
    SpOverSlipLink& operator=(const SpOverSlipLink&) = delete;

    // ── Transport selection ───────────────────────────────────────────────
    // Exclusive: TCP or serial, never both. Two peers would mean two device
    // number spaces to merge, which no real configuration needs. Changing
    // transport while running restarts the worker.

    enum class Mode { Off, Tcp, Serial };

    void setTcpMode(uint16_t port);
    void setSerialMode(std::string devicePath, int baud);
    void setOff();
    Mode mode() const { return mode_; }

    /// The configured transport parameters, whether or not that transport is
    /// the active one — the panel edits both and the host persists both, so a
    /// user who switches TCP → serial → TCP does not lose their port.
    uint16_t           tcpPort()    const { return tcpPort_; }
    const std::string& serialPath() const { return serialPath_; }
    int                serialBaud() const { return serialBaud_; }

    /// Start the worker. Returns false (with `errOut` filled) when the chosen
    /// transport could not even be armed — a TCP port already in use, say.
    bool start(std::string& errOut);
    /// Stop the worker, join it, and release the peer. Safe to call twice.
    void stop();
    bool isRunning() const { return running_.load(); }

    // ── State for the panel ───────────────────────────────────────────────
    bool        isConnected() const;
    std::string describe() const;
    std::string lastError() const;
    std::vector<SpDevice> devices() const;
    std::size_t deviceCount() const;
    Stats       stats() const;

    int  timeoutMs() const { return timeoutMs_.load(); }
    void setTimeoutMs(int ms);

    // ── SmartPort calls (CPU thread) ──────────────────────────────────────
    // `unit` is the SmartPort unit number, 1-based. Unit 0 addresses the bus
    // itself and is answered locally (see FujiNetCard), never forwarded.

    Response status(uint8_t unit, uint8_t statusCode);
    Response readBlock(uint8_t unit, uint32_t block);
    Response writeBlock(uint8_t unit, uint32_t block,
                        const uint8_t* data, std::size_t n);
    Response format(uint8_t unit);
    Response control(uint8_t unit, uint8_t controlCode,
                     const uint8_t* list, std::size_t n);
    Response init(uint8_t unit);
    Response open(uint8_t unit);
    Response close(uint8_t unit);
    Response read(uint8_t unit, uint16_t byteCount, uint32_t address);
    Response write(uint8_t unit, uint16_t byteCount, uint32_t address,
                   const uint8_t* data, std::size_t n);

    /// Guest reset. Bumps the sequence number so a response in flight for the
    /// pre-reset request can never be mistaken for an answer to the next one,
    /// then tells every enumerated device (Control code $00) so a modem drops
    /// its connection and a printer ejects its partial page — the behaviour
    /// the spec asks the Apple II side to provide.
    void notifyGuestReset();

    /// Move the sequence number on and drop any half-decoded frame, WITHOUT
    /// telling the devices anything. This is the rewind case: the guest's
    /// clock went backwards, so an in-flight response must not be matched to
    /// the next request — but the peer did not reset, and telling a modem to
    /// hang up because the *user* rewound would be wrong. See
    /// FujiNetCard::loadSnapshotState.
    void resync();

private:
    /// One request/response exchange. `fields` is the 5 command-specific
    /// bytes at offsets 6-10. Returns a Response whose `replied` is false on
    /// timeout or a dead peer.
    Response transact(uint8_t command, uint8_t paramCount, uint8_t unit,
                      const uint8_t fields[5],
                      const uint8_t* data, std::size_t dataLen);

    void workerLoop();
    void enumerateDevices();
    /// Peer teardown. TWO forms on purpose: `transact()` discovers the loss
    /// while it already holds `callMtx_`, and std::mutex is not recursive —
    /// taking it again there deadlocks the CPU thread with the emulated 6502
    /// parked mid-SmartPort-call, which is as bad as it sounds.
    void peerLostLocked();   ///< callMtx_ MUST be held

    /// Consecutive `transact()` calls that timed out with no reply. Guarded
    /// by `callMtx_` — transact() holds it for the whole exchange, and
    /// peerLostLocked() is only ever reached with it held.
    ///
    /// This exists because a bounded stall repeated without bound is not
    /// bounded. `transact()` waits up to `timeoutMs_` (250 ms default) inside
    /// a SmartPort call, on the CPU thread, holding the emulator's stateMutex
    /// — so a peer that ACCEPTS writes but never answers used to cost that
    /// much per call, for every call, forever: a ProDOS boot became a string
    /// of quarter-second freezes with the FujiNet panel's own Stop button
    /// unreachable, because drawing it needs the same mutex. A write failure
    /// already declared the peer lost; a silence did not.
    unsigned consecutiveTimeouts_ = 0;
    void handlePeerLost();   ///< callMtx_ must NOT be held

    uint8_t nextSequence();

    Mode                       mode_ = Mode::Off;
    uint16_t                   tcpPort_ = SpTcpTransport::kDefaultPort;
    std::string                serialPath_;
    int                        serialBaud_ = SerialPort::kDefaultBaud;

    std::unique_ptr<SpTransport> transport_;   ///< owned; swapped under stop()

    std::thread                worker_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          stopFlag_{false};
    std::atomic<int>           timeoutMs_{kDefaultTimeoutMs};

    /// Lifetime tracking for the CURRENT peer, so peerLostLocked() can say
    /// how long it held and how much it served. Written by the worker thread
    /// only (connect and loss both happen there).
    std::chrono::steady_clock::time_point peerSince_{};
    uint64_t peerCallsAtConnect_    = 0;
    uint64_t peerTimeoutsAtConnect_ = 0;
    void notePeerConnected();

    /// Serialises whole request/response exchanges. Held by transact() for
    /// its entire duration, so the worker's enumeration and the CPU thread's
    /// SmartPort calls can never have two requests in flight at once.
    mutable std::mutex         callMtx_;
    SlipFramer                 rx_;            ///< guarded by callMtx_
    uint8_t                    sequence_ = 0;  ///< guarded by callMtx_
    std::vector<uint8_t>       txBuf_;         ///< guarded by callMtx_

    mutable std::mutex         stateMtx_;      ///< guards devices_/lastError_
    std::vector<SpDevice>      devices_;
    std::string                lastError_;

    mutable std::mutex         statsMtx_;
    Stats                      stats_;
};

} // namespace pom2

#endif // POM2_SP_OVER_SLIP_LINK_H
