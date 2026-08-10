// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpTransport — the byte pipe underneath SP-over-SLIP, and its two concrete
// forms.
//
// The FujiNet spec says SP-over-SLIP "can be used on any medium providing a
// transparent, duplex, lossless byte stream. Examples are a TCP connection or
// a USB CDC-ACM connection." POM2 implements both, and this interface is the
// seam that keeps the session layer (SpOverSlipLink) from caring which one it
// holds:
//
//   SpTcpTransport     POM2 listens on 127.0.0.1:1985; a FujiNet *desktop
//                      build* connects in. The everyday case.
//   SpSerialTransport  POM2 opens a serial device; a *physical FujiNet board*
//                      answers over its USB CDC-ACM port.
//
// This mirrors the split the FujiNet AppleWin fork uses (`devrelay/service/
// Connection.h` + `TCPConnection` / `COMConnection`), for the same reason:
// the protocol code should be written once.
//
// ── Threading contract ────────────────────────────────────────────────────
//
// Two threads touch a transport, and they do different jobs:
//
//   * The LINK WORKER calls `pollForPeer()` — accept a TCP connection, or
//     open the serial device once it appears. Worker thread only.
//   * The CPU THREAD calls `writeAll()` / `readSome()` from inside a
//     SmartPort call, under EmulationController's stateMutex.
//
// Implementations serialise those with an internal mutex held for the whole
// of `readSome`/`writeAll` — safe because both are bounded by the caller's
// timeout (250 ms by default), so the worker never waits long to tear down a
// dead peer.
//
// `shutdown()` is the ONE method that must be callable from any thread: it
// wakes a `pollForPeer()` parked in a wait. Destroying the transport (and
// closing the listening socket / device) is only legal once the worker has
// been joined — the same rule SuperSerialCard documents at length, for the
// same reason (close + recv on one fd from two threads is a use-after-free).

#ifndef POM2_SP_TRANSPORT_H
#define POM2_SP_TRANSPORT_H

#include "Pom2Build.h"
#include "SerialPort.h"
#include "SocketCompat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace pom2 {

class SpTransport
{
public:
    virtual ~SpTransport() = default;

    /// True when a peer is attached and I/O can be attempted.
    virtual bool isOpen() const = 0;

    /// Try to acquire a peer, waiting at most `timeoutMs`. Returns true when
    /// a peer was JUST acquired (the session layer takes that as its cue to
    /// enumerate devices). Worker thread only.
    virtual bool pollForPeer(int timeoutMs) = 0;

    /// Write the whole buffer. false = the peer died; the caller should drop
    /// the link. Blocking, but only ever called with small buffers.
    virtual bool writeAll(const uint8_t* p, std::size_t n) = 0;

    /// Read up to `n` bytes, waiting at most `timeoutMs` for the first one.
    ///   > 0  bytes read
    ///   = 0  nothing arrived within the timeout (not an error)
    ///   < 0  the peer died
    virtual int readSome(uint8_t* p, std::size_t n, int timeoutMs) = 0;

    /// Drop the current peer but stay ready to acquire another one.
    virtual void dropPeer() = 0;

    /// Wake a `pollForPeer()` in flight so the worker can exit. Any thread.
    virtual void shutdown() = 0;

    /// One line for the panel and the log ("listening on :1985",
    /// "/dev/ttyACM0 @ 115200", "connected 127.0.0.1:54xxx").
    virtual std::string describe() const = 0;
};

// ── TCP ───────────────────────────────────────────────────────────────────

/// Listens on loopback and accepts ONE peer at a time. A second connection
/// while one is live is closed immediately rather than queued: two peers
/// would mean two SmartPort device-number spaces to merge, which no real
/// configuration needs.
///
/// Default port 1985 — the port `fujinet-go-apple2-desktop` uses, so an
/// existing FujiNet configuration works against POM2 untouched.
class SpTcpTransport final : public SpTransport
{
public:
    static constexpr uint16_t kDefaultPort = 1985;

    explicit SpTcpTransport(uint16_t port = kDefaultPort);
    ~SpTcpTransport() override;

    /// Bind + listen. Must be called before the worker starts polling.
    /// Returns false with a human-readable reason in `errOut`.
    bool startListening(std::string& errOut);
    /// Close the listening socket. Illegal while a worker may still be inside
    /// `pollForPeer()` — join it first.
    void stopListening();
    bool isListening() const;

    uint16_t port() const { return port_; }

    bool        isOpen() const override;
    bool        pollForPeer(int timeoutMs) override;
    bool        writeAll(const uint8_t* p, std::size_t n) override;
    int         readSome(uint8_t* p, std::size_t n, int timeoutMs) override;
    void        dropPeer() override;
    void        shutdown() override;
    std::string describe() const override;

private:
    uint16_t            port_;
    /// Serialises I/O against peer teardown: held for the whole of
    /// readSome/writeAll, so dropPeer() cannot close a descriptor another
    /// thread is inside recv() on (the use-after-close hazard
    /// SuperSerialCard documents). Both are bounded by the caller's timeout,
    /// so the wait is short.
    mutable std::mutex  mtx_;
#if POM2_HAS_SOCKETS
    /// Atomic so `shutdown()` can wake a blocked wait from any thread
    /// WITHOUT closing the descriptor under it — same split as the SSC.
    std::atomic<socket_t> listenFd_{kInvalidSocket};
    std::atomic<socket_t> clientFd_{kInvalidSocket};
#endif
    std::string         peerText_;        ///< guarded by mtx_
};

// ── Serial (USB CDC-ACM) ──────────────────────────────────────────────────

/// Opens a serial device and treats THE OPEN as the connect event — a serial
/// line has no connection establishment to wait on. While disconnected the
/// worker retries the open every poll, which doubles as hot-plug detection.
class SpSerialTransport final : public SpTransport
{
public:
    /// `path` empty = "auto": take the single candidate from
    /// SerialPort::enumerate(), or stay idle if there is not exactly one.
    explicit SpSerialTransport(std::string path = std::string{},
                               int baud = SerialPort::kDefaultBaud);
    ~SpSerialTransport() override;

    const std::string& path() const { return path_; }
    void setPath(std::string path);
    int  baud() const { return baud_; }
    void setBaud(int b) { baud_ = b; }

    /// Why the last open() failed, in words the panel can show — including
    /// the permission case, which is the most likely first-contact failure
    /// on Linux ("add your user to the dialout group").
    const std::string& lastError() const { return lastError_; }

    bool        isOpen() const override;
    bool        pollForPeer(int timeoutMs) override;
    bool        writeAll(const uint8_t* p, std::size_t n) override;
    int         readSome(uint8_t* p, std::size_t n, int timeoutMs) override;
    void        dropPeer() override;
    void        shutdown() override;
    std::string describe() const override;

private:
    std::string        path_;
    int                baud_;
    mutable std::mutex mtx_;          ///< guards port_ + openPath_
    SerialPort         port_;
    std::string        openPath_;     ///< the device actually opened
    std::string        lastError_;
    bool               stopping_ = false;
};

} // namespace pom2

#endif // POM2_SP_TRANSPORT_H
