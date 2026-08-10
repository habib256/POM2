// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpSerialTransport — SP-over-SLIP across a USB CDC-ACM serial line, i.e.
// against a PHYSICAL FujiNet board plugged into the host's USB port.
//
// The FujiNet spec blesses this explicitly: SP-over-SLIP "can be used on any
// medium providing a transparent, duplex, lossless byte stream. Examples are
// a TCP connection or a USB CDC-ACM connection." The AppleWin fork implements
// the same thing as `devrelay/service/COMConnection`.
//
// ── How this differs from TCP, and why the difference is in this file ─────
//
//   * THERE IS NO CONNECTION ESTABLISHMENT. A serial line is either there or
//     not. So OPENING THE DEVICE *is* the connect event, and "waiting for a
//     peer" is really "waiting for the device node to appear" — which
//     doubles as hot-plug detection, for free.
//   * THE PEER MAY STILL BE BOOTING when the port opens. A board that was
//     just plugged in enumerates its USB endpoint before its firmware is
//     ready to answer SmartPort. That is the session layer's problem (it
//     retries the device sweep), but it is the reason this transport reports
//     "peer acquired" on a successful open rather than waiting for traffic.
//   * OPENING IT WRONG REBOOTS THE HARDWARE. See SerialPort.h trap 1 — the
//     ESP32 auto-reset circuit hangs off DTR/RTS. That defence lives in
//     SerialPort, not here, so anything else that ever opens a serial device
//     inherits it.

#include "SpTransport.h"

#include "Logger.h"

#include <utility>

namespace pom2 {

SpSerialTransport::SpSerialTransport(std::string path, int baud)
    : baud_(baud), path_(std::move(path))
{}

SpSerialTransport::~SpSerialTransport() { dropPeer(); }

void SpSerialTransport::setPath(std::string path)
{
    std::lock_guard<std::mutex> lk(statusMtx_);
    path_ = std::move(path);
}

bool SpSerialTransport::isOpen() const
{
    // No lock: the UI thread asks this every frame with stateMutex held, and
    // mtx_ can be held for a whole read timeout.
    return open_.load();
}

bool SpSerialTransport::pollForPeer(int timeoutMs)
{
    if (stopping_.load()) return false;
    if (open_.load()) return false;            // already have our peer

    // Decide what to open. An empty path means "auto": take the single
    // candidate if there is exactly one, and otherwise stay idle rather than
    // guessing — opening the wrong device would drive DTR/RTS at whatever
    // else is plugged in.
    std::string target;
    {
        std::lock_guard<std::mutex> lk(statusMtx_);
        target = path_;
    }
    if (target.empty()) {
        const auto candidates = SerialPort::enumerate();
        if (candidates.size() == 1) {
            target = candidates[0].path;
        } else {
            std::lock_guard<std::mutex> lk(statusMtx_);
            lastError_ = candidates.empty()
                             ? "no serial device found"
                             : "several serial devices found — pick one";
            return false;
        }
    }

    const int baud = baud_.load();
    std::lock_guard<std::mutex> lk(mtx_);
    if (stopping_.load() || port_.isOpen()) return false;
    if (!port_.open(target, baud)) {
        // Not logged every poll: the worker calls this a couple of times a
        // second while the board is unplugged, and "device not found" is the
        // normal idle state, not an incident. The panel shows lastError().
        std::lock_guard<std::mutex> st(statusMtx_);
        lastError_ = port_.lastError();
        return false;
    }

    {
        std::lock_guard<std::mutex> st(statusMtx_);
        openPath_ = target;
        lastError_.clear();
    }
    open_.store(true);
    log().info("FujiNet", "SP-over-SLIP serial port opened: " + target +
                              " @ " + std::to_string(baud));
    (void)timeoutMs;   // nothing to wait ON — the open either works or not
    return true;
}

bool SpSerialTransport::writeAll(const uint8_t* p, std::size_t n)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!port_.isOpen()) return false;
    if (port_.writeAll(p, n)) return true;
    std::lock_guard<std::mutex> st(statusMtx_);
    lastError_ = port_.lastError();
    return false;
}

int SpSerialTransport::readSome(uint8_t* p, std::size_t n, int timeoutMs)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!port_.isOpen()) return -1;
    const int r = port_.readSome(p, n, timeoutMs);
    if (r < 0) {
        std::lock_guard<std::mutex> st(statusMtx_);
        lastError_ = port_.lastError();
    }
    return r;
}

void SpSerialTransport::dropPeer()
{
    std::string closed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!port_.isOpen()) return;
        port_.close();
        open_.store(false);
        std::lock_guard<std::mutex> st(statusMtx_);
        closed = openPath_;
        openPath_.clear();
    }
    log().info("FujiNet", "SP-over-SLIP serial port closed: " + closed);
}

void SpSerialTransport::shutdown()
{
    // A serial pollForPeer never parks in a long wait (it either opens the
    // device or reports "not there"), so there is nothing to interrupt —
    // just latch the stop so a poll racing us does not reopen the port after
    // the owner asked us to stop.
    //
    // The latch is atomic rather than mtx_-guarded because shutdown() is the
    // one method that must stay callable while a read is in flight, and
    // readSome holds mtx_ for its whole timeout.
    stopping_.store(true);
}

std::string SpSerialTransport::describe() const
{
    // statusMtx_ only — see the header. Taking the I/O mutex here parked the
    // UI thread (and, through stateMutex, the CPU worker) behind every read
    // timeout of a peer that had stopped answering.
    std::lock_guard<std::mutex> lk(statusMtx_);
    if (open_.load())
        return openPath_ + " @ " + std::to_string(baud_.load());
    if (!path_.empty())  return "waiting for " + path_;
    return "waiting for a serial device";
}

} // namespace pom2
