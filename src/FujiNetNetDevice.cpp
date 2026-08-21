#include "FujiNetNetDevice.h"

#include "Logger.h"
#include "SocketUtil.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace pom2 {

namespace {

/// Error codes in the firmware's numbering, which guest code compares
/// against directly (network_data.h NDEV_STATUS).
constexpr uint8_t kErrSuccess      = 1;
constexpr uint8_t kErrGeneral      = 144;
constexpr uint8_t kErrFileNotFound = 170;

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

}  // namespace

FujiNetNetDevice::~FujiNetNetDevice() { close(); }

bool FujiNetNetDevice::fetchHttp(const std::string& host, uint16_t port,
                                 const std::string& path)
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        error_ = kErrFileNotFound;          // the guest reads this as "no such host"
        return false;
    }

    socket_t fd = kInvalidSocket;
    for (addrinfo* a = res; a; a = a->ai_next) {
        socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (!isValidSocket(s)) continue;
        disableSigpipe(s);
        // Bounded: this runs on the CPU thread inside a SmartPort call, so an
        // unreachable host must not wedge the emulated machine for the
        // stack's own retry budget.
        timeval tv{};
        tv.tv_sec = 8;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
        if (::connect(s, a->ai_addr, static_cast<socklen_c>(a->ai_addrlen)) == 0) {
            fd = s;
            break;
        }
        closeHostSocketValue(s);
    }
    ::freeaddrinfo(res);
    if (!isValidSocket(fd)) { error_ = kErrGeneral; return false; }

    // HTTP/1.0 on purpose: it ends the body at EOF, so there is no chunked
    // transfer-encoding to unpick. `Connection: close` says the same thing to
    // a server that answers 1.1 anyway.
    std::string req = "GET " + path + " HTTP/1.0\r\n"
                      "Host: " + host + "\r\n"
                      "User-Agent: POM2-FujiNet/1.0\r\n"
                      "Connection: close\r\n\r\n";
    if (sendNoSignal(fd, req.data(), req.size()) != static_cast<iolen_t>(req.size())) {
        closeHostSocketValue(fd);
        error_ = kErrGeneral;
        return false;
    }

    std::vector<uint8_t> raw;
    uint8_t buf[4096];
    for (;;) {
        const iolen_t r = ::recv(fd, reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (r <= 0) break;
        raw.insert(raw.end(), buf, buf + r);
        // A guard, not a policy: the guest has 128 KB of RAM and the status
        // reply can only ever announce 512 bytes at a time, so a runaway
        // response must not grow POM2's heap without bound.
        if (raw.size() > 512u * 1024u) break;
    }
    closeHostSocketValue(fd);

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
