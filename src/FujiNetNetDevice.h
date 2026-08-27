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

// FujiNetNetDevice — POM2's OWN `N:` network device.
//
// The FujiNet's headline feature is `N:`: a deported TCP/IP stack the guest
// drives with simple commands, so an Apple II gets HTTP without running a byte
// of TCP/IP itself. POM2 normally relays that to a real FujiNet — but the
// desktop firmware build answers the guest's open with success and then never
// opens a socket at all (measured 2026-08-21 on the peer's own descriptors;
// the same firmware opens real TCP for TNFS on the same machine), so on that
// peer `N:` is inert and no amount of relaying helps.
//
// This is the answer: when the built-in network device is enabled, POM2 serves
// the `N:` unit itself, out of host sockets. The rest of the chain — disks,
// CONFIG, the clock — still goes to the peer, which handles them well. See
// TODO § [Network] for the "native + relay coexisting" decision this
// implements.
//
// ── Protocol ──────────────────────────────────────────────────────────────
//
// Command bytes are ASCII letters, from the firmware's own table
// (`include/fujiCommandID.h`):
//
//     'O' 0x4F  OPEN    control list carries the devicespec, e.g.
//                       "N:HTTP://THEOLDNET.COM/"
//     'C' 0x43  CLOSE
//     'S' 0x53  STATUS  4-byte reply: bytes-waiting (LE 16), connected, error
//     'R' 0x52  READ    (data itself comes back through SmartPort READ)
//     'W' 0x57  WRITE
//     'E' 0x45  GET_ERROR
//
// The status reply's byte count is capped at 512 by the firmware, and this
// follows suit: guest code sizes its buffer from that number.
//
// ── Scope, deliberately ───────────────────────────────────────────────────
//
// HTTP over plain TCP, which is what the retro web actually serves (and what
// theoldnet.com — the reason this exists — speaks). No TLS: that would mean
// carrying a TLS stack, and `docs/fujinet_plan.md` § 2 is right that
// reimplementing the firmware's whole network stack buys nothing. No SSH, no
// JSON parser, no `N:` filesystem verbs. A real FujiNet board over USB
// remains the way to get all of those; this exists so that the machine can
// browse at all when the peer's own `N:` cannot.

#ifndef POM2_FUJINET_NET_DEVICE_H
#define POM2_FUJINET_NET_DEVICE_H

#include "FujiNetNetwork.h"
#include "SocketCompat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

// The kNet* command bytes live in FujiNetNetwork.h — they are protocol,
// and the card decodes them without knowing this implementation exists.

class FujiNetNetDevice : public FujiNetNetwork {
public:
    /// The firmware caps a status reply's byte count at 512 and guest code
    /// sizes its buffer from it, so promising more would overrun the guest.
    static constexpr uint16_t kMaxStatusAvail = 512;

    ~FujiNetNetDevice();

    /// Open a devicespec: "N:HTTP://host[:port]/path" (the "N:" and the
    /// scheme are case-insensitive, as the guest usually shouts them). The
    /// whole response body is fetched now and buffered — the guest then drains
    /// it with STATUS/READ, which is exactly the shape its code expects and
    /// avoids holding a socket open across an emulated machine's lifetime.
    bool open(const std::string& devicespec) override;
    void close() override;
    bool isOpen() const override { return open_; }

    /// 4-byte status: bytes waiting (LE 16), connected flag, error code.
    void status(uint8_t out[4]) const override;

    /// Copy up to `n` buffered bytes out. Returns how many were copied.
    std::size_t read(uint8_t* dst, std::size_t n) override;

    /// Bytes still unread.
    std::size_t available() const override { return body_.size() - cursor_; }

    /// Last error, in the firmware's numbering: 1 = success, 144 = general
    /// failure, 170 = file not found. Guest code compares against these.
    uint8_t lastError() const { return error_; }

    /// What the panel shows. Empty when nothing has been opened.
    const std::string& describe() const { return description_; }

    /// Wall-clock budget for a whole fetch — DNS, connect, request and body
    /// together. It exists because this runs on the CPU thread with the
    /// emulator's state mutex held, so it is really "how long may the emulated
    /// machine freeze". Lowered by the tests so the truncation rule can be
    /// pinned in under a second.
    void setFetchDeadlineMs(int ms) { deadlineMs_ = ms; }
    int  fetchDeadlineMs() const    { return deadlineMs_; }

private:
    bool fetchHttp(const std::string& host, uint16_t port,
                   const std::string& path);

    int                  deadlineMs_ = 12000;
    bool                 open_   = false;
    uint8_t              error_  = 1;      ///< 1 = SUCCESS in the firmware's table
    std::vector<uint8_t> body_;
    std::size_t          cursor_ = 0;
    std::string          description_;
};

}  // namespace pom2

#endif  // POM2_FUJINET_NET_DEVICE_H
