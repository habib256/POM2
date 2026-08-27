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

#include "FujiNetNetDevice.h"

#include "Logger.h"
#include "ThreadGuard.h"
#include "SocketUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <cstring>

#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// No host sockets under Emscripten: the browser build has no TCP to offer, so
// the whole fetch path compiles out and the device politely refuses. The
// guest sees an open that fails, which is the truth there.
#if !POM2_HAS_SOCKETS

namespace pom2 {

FujiNetNetDevice::~FujiNetNetDevice() = default;

bool FujiNetNetDevice::fetchHttp(const std::string&, uint16_t, const std::string&)
{
    return false;
}

bool FujiNetNetDevice::open(const std::string& devicespec)
{
    close();
    error_       = kNetErrGeneral;
    description_ = devicespec + " — no host network in this build";
    log().warn("FujiNet", "built-in N: unavailable — this build has no host sockets");
    return false;
}

void FujiNetNetDevice::close()
{
    open_ = false;
    body_.clear();
    cursor_ = 0;
}

void FujiNetNetDevice::status(uint8_t out[4]) const
{
    out[0] = out[1] = out[2] = 0;
    out[3] = error_;
}

std::size_t FujiNetNetDevice::read(uint8_t*, std::size_t) { return 0; }

}  // namespace pom2

#else   // POM2_HAS_SOCKETS

namespace pom2 {

namespace {

/// Error codes in the firmware's numbering, which guest code compares
/// against directly (network_data.h NDEV_STATUS).
// The error bytes are part of the `N:` contract (FujiNetNetwork.h), not of
// this implementation — a fake N: has to report the same ones.
constexpr uint8_t kErrSuccess      = kNetErrSuccess;
constexpr uint8_t kErrGeneral      = kNetErrGeneral;
constexpr uint8_t kErrFileNotFound = kNetErrFileNotFound;

std::string upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Split "N:HTTP://host:port/path" into its parts. The guest usually shouts
/// the whole thing in upper case, and the HOST half of a URL is
/// case-insensitive — but the PATH is not, so only the scheme and host get
/// folded and the path is passed through exactly as typed.
bool parseSpec(const std::string& spec, std::string& host, uint16_t& port,
               std::string& path)
{
    std::string s = spec;
    // Strip the "N:" (or "N1:".."N8:") device prefix.
    const std::string u = upper(s);
    if (u.size() >= 2 && u[0] == 'N') {
        const std::size_t colon = s.find(':');
        if (colon != std::string::npos && colon <= 2) s = s.substr(colon + 1);
    }
    const std::string us = upper(s);
    if (us.rfind("HTTP://", 0) == 0)       s = s.substr(7);
    else if (us.rfind("TCP://", 0) == 0)   s = s.substr(6);
    else if (us.rfind("HTTPS://", 0) == 0) return false;   // no TLS, see header
    else if (us.find("://") != std::string::npos) return false;

    const std::size_t slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : s.substr(slash);
    if (path.empty()) path = "/";

    port = 80;
    const std::size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        const long p = std::strtol(hostport.c_str() + colon + 1, nullptr, 10);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
        hostport = hostport.substr(0, colon);
    }
    // Host names are case-insensitive; lower-casing keeps the Host: header
    // conventional rather than SHOUTING at the server.
    for (char& c : hostport) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    host = hostport;
    return !host.empty();
}


// ── Bounded host I/O ──────────────────────────────────────────────────────
//
// This runs on the CPU thread INSIDE a SmartPort call, and the emulation
// worker holds the state mutex for the whole slice (EmulationController.cpp,
// `runCpuSlice` under `stateMtx`). So every wait here is bounded, and the
// whole exchange shares ONE deadline: otherwise the emulated machine — UI,
// menus and the FujiNet panel's own Restart button included — freezes solid
// for as long as some host on the internet feels like stalling.
//
// SO_SNDTIMEO/SO_RCVTIMEO are not enough for that, twice over:
//   * They do not bound connect(). Measured 2026-08-21 on macOS against
//     192.0.2.1 (TEST-NET-1, swallows SYNs): connect() returned after 75 s
//     with the option asking for 8. That is 75 s of frozen emulator.
//   * A per-recv timeout bounds each call, never the transfer. A server
//     drip-feeding one byte just inside the timeout keeps the loop alive
//     forever — an unbounded freeze, not a slow page.
constexpr int kConnectTimeoutMs = 5000;

/// A guard, not a policy: the guest has 128 KB of RAM and STATUS can only
/// announce 512 bytes at a time, so a runaway response must not grow POM2's
/// heap without bound.
constexpr std::size_t kMaxBody = 512u * 1024u;

using SteadyPoint = std::chrono::steady_clock::time_point;

/// Milliseconds left before `deadline`, never negative.
int msLeft(SteadyPoint deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - now).count());
}

/// A DNS lookup that cannot outlive the deadline.
///
/// There is no portable async resolver, and getaddrinfo() itself blocks for
/// as long as the resolver chain takes — resolv.conf timeout x attempts x
/// servers, tens of seconds against an unreachable DNS server. On the CPU
/// thread under the emulator's state mutex that is the whole window frozen,
/// unpaintable, with the panel's own Stop button out of reach. So the lookup
/// runs on its own thread and is ABANDONED if it overruns: the thread frees
/// its own result and exits whenever the resolver finally answers.
struct Lookup {
    std::mutex mtx;
    bool       abandoned = false;
    addrinfo*  res       = nullptr;
};

bool resolveBounded(const std::string& host, const std::string& portStr,
                    int timeoutMs, addrinfo** out)
{
    *out = nullptr;
    if (timeoutMs <= 0) return false;

    auto shared = std::make_shared<Lookup>();
    std::promise<bool> ready;
    std::future<bool>  fut = ready.get_future();

    // Guarded: an exception escaping a detached thread calls std::terminate().
    // The promise needs the same care — leaving it unset makes the waiter's
    // fut.get() below throw future_error on the CALLING thread, so a lookup
    // that dies has to answer "false" rather than answer nothing. `settled`
    // also covers the abandoned path, where by contract nobody is waiting and
    // the promise is deliberately left alone.
    std::thread([shared, host, portStr, p = std::move(ready)]() mutable {
        bool settled = false;
        pom2::runGuarded("FujiNet", [&] {
            addrinfo hints{};
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo* r  = nullptr;
            const bool ok = (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &r) == 0) && r;

            std::lock_guard<std::mutex> lk(shared->mtx);
            if (shared->abandoned) {           // nobody is waiting any more
                if (r) ::freeaddrinfo(r);
                settled = true;
                return;
            }
            shared->res = r;
            p.set_value(ok);
            settled = true;
        });
        if (!settled) p.set_value(false);
    }).detach();

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(shared->mtx);
        shared->abandoned = true;
        if (shared->res) { ::freeaddrinfo(shared->res); shared->res = nullptr; }
        return false;
    }

    const bool ok = fut.get();
    std::lock_guard<std::mutex> lk(shared->mtx);
    *out = shared->res;
    shared->res = nullptr;
    return ok && *out;
}

/// connect() bounded by an explicit wait — the same non-blocking pattern
/// W5100Device already uses. Leaves the socket NON-BLOCKING on success,
/// which is what the transfer below wants.
bool connectBounded(const addrinfo* a, int timeoutMs, socket_t& out)
{
    socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (!isValidSocket(s)) return false;
    disableSigpipe(s);
    if (!setNonBlocking(s)) { closeHostSocketValue(s); return false; }

    if (::connect(s, a->ai_addr, static_cast<socklen_c>(a->ai_addrlen)) != 0) {
        if (!errInProgress(lastSocketError()) ||
            waitSocket(s, SocketWait::Write, timeoutMs) != WaitResult::Ready ||
            connectResult(s) != 0) {
            closeHostSocketValue(s);
            return false;
        }
    }
    out = s;
    return true;
}

}  // namespace

FujiNetNetDevice::~FujiNetNetDevice() { close(); }

bool FujiNetNetDevice::fetchHttp(const std::string& host, uint16_t port,
                                 const std::string& path)
{
    const SteadyPoint deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(deadlineMs_);

    addrinfo* res = nullptr;
    if (!resolveBounded(host, std::to_string(port), msLeft(deadline), &res)) {
        error_ = kErrFileNotFound;          // the guest reads this as "no such host"
        log().warn("FujiNet", "built-in N: cannot resolve \"" + host + "\"");
        return false;
    }

    socket_t fd = kInvalidSocket;
    for (addrinfo* a = res; a; a = a->ai_next) {
        const int budget = std::min(kConnectTimeoutMs, msLeft(deadline));
        if (connectBounded(a, budget, fd)) break;
    }
    ::freeaddrinfo(res);
    if (!isValidSocket(fd)) { error_ = kErrGeneral; return false; }

    // HTTP/1.0 on purpose: it ends the body at EOF, so there is no chunked
    // transfer-encoding to unpick. `Connection: close` says the same thing to
    // a server that answers 1.1 anyway.
    const std::string req = "GET " + path + " HTTP/1.0\r\n"
                            "Host: " + host + "\r\n"
                            "User-Agent: POM2-FujiNet/1.0\r\n"
                            "Connection: close\r\n\r\n";

    // The socket is non-blocking, so a send can be short or refuse outright.
    std::size_t sent = 0;
    while (sent < req.size()) {
        const int left = msLeft(deadline);
        if (left <= 0 || waitSocket(fd, SocketWait::Write, left) != WaitResult::Ready) {
            closeHostSocketValue(fd);
            error_ = kErrGeneral;
            return false;
        }
        const iolen_t w = sendNoSignal(fd, req.data() + sent, req.size() - sent);
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        const int e = lastSocketError();
        if (!errWouldBlock(e) && !errInterrupted(e)) {
            closeHostSocketValue(fd);
            error_ = kErrGeneral;
            return false;
        }
    }

    std::vector<uint8_t> raw;
    uint8_t buf[4096];
    bool truncated = false;
    for (;;) {
        const int left = msLeft(deadline);
        if (left <= 0) { truncated = true; break; }
        const WaitResult wr = waitSocket(fd, SocketWait::Read, left);
        if (wr != WaitResult::Ready) { truncated = true; break; }

        const iolen_t r = ::recv(fd, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (r == 0) break;                  // clean EOF — the body is complete
        if (r < 0) {
            const int e = lastSocketError();
            if (errWouldBlock(e) || errInterrupted(e)) continue;
            truncated = true;
            break;
        }
        raw.insert(raw.end(), buf, buf + static_cast<std::size_t>(r));
        if (raw.size() > kMaxBody) { truncated = true; break; }
    }
    closeHostSocketValue(fd);

    // A short read is NOT a short page. Handing the guest half a document it
    // cannot tell from a whole one is the one failure nobody can diagnose from
    // the Apple II side, so say so instead.
    if (truncated) {
        error_ = kErrGeneral;
        log().warn("FujiNet", "built-in N: incomplete response from " + host + path +
                              " — " + std::to_string(raw.size()) +
                              " bytes before the deadline or the size cap; refusing to"
                              " hand the guest a half page");
        return false;
    }

    // Hand the guest the BODY only. Guest-side browsers parse HTML, not
    // response headers, and the FujiNet's own N: does the same split.
    static const uint8_t kCrLfCrLf[4] = { '\r', '\n', '\r', '\n' };
    auto it = std::search(raw.begin(), raw.end(), kCrLfCrLf, kCrLfCrLf + 4);
    if (it != raw.end()) body_.assign(it + 4, raw.end());
    else                 body_ = raw;      // headerless reply: pass it through

    cursor_ = 0;
    error_  = kErrSuccess;
    return true;
}

bool FujiNetNetDevice::open(const std::string& devicespec)
{
    close();

    std::string host, path;
    uint16_t port = 80;
    if (!parseSpec(devicespec, host, port, path)) {
        error_ = kErrGeneral;
        description_ = devicespec + " — unsupported (HTTP over TCP only)";
        log().warn("FujiNet", "built-in N: cannot open \"" + devicespec +
                              "\" — only HTTP over plain TCP is served here");
        return false;
    }

    const bool ok = fetchHttp(host, port, path);
    open_ = ok;
    // available()/read() are public and do not consult open_; a failed fetch
    // must not leave partial bytes reachable.
    if (!ok) { body_.clear(); cursor_ = 0; }
    description_ = devicespec + (ok ? " — " + std::to_string(body_.size()) + " B"
                                    : " — failed");
    log().info("FujiNet", std::string("built-in N: ") + (ok ? "fetched " : "failed ") +
                          host + path + (ok ? " (" + std::to_string(body_.size()) +
                                              " bytes)" : ""));
    return ok;
}

void FujiNetNetDevice::close()
{
    open_ = false;
    body_.clear();
    cursor_ = 0;
}

void FujiNetNetDevice::status(uint8_t out[4]) const
{
    const std::size_t avail = std::min<std::size_t>(available(), kMaxStatusAvail);
    out[0] = static_cast<uint8_t>(avail & 0xFF);
    out[1] = static_cast<uint8_t>((avail >> 8) & 0xFF);
    // "Connected" stays true while there is still body to hand over: guest
    // loops read until this clears, and dropping it early truncates the page.
    out[2] = (open_ && available() > 0) ? 1 : 0;
    out[3] = error_;
}

std::size_t FujiNetNetDevice::read(uint8_t* dst, std::size_t n)
{
    const std::size_t take = std::min(n, available());
    if (take) {
        std::memcpy(dst, body_.data() + cursor_, take);
        cursor_ += take;
    }
    return take;
}

}  // namespace pom2

#endif  // POM2_HAS_SOCKETS
