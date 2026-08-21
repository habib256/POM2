#include "TnfsClient.h"

#include "SocketUtil.h"

#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace pom2 {

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

uint16_t get16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

/// TNFS has its OWN result table -- it is NOT errno, however much the names
/// look like it (ACCESS_DENIED is 0x09 here and 0x0D in errno, and so on).
/// Reference: fujinet-firmware lib/TNFSlib/tnfslib.h TNFS_RESULT_*. Only the
/// codes a caller can act on are named; the rest are reported numerically
/// rather than guessed at.
const char* statusName(uint8_t s)
{
    switch (s) {
    case 0x00: return "success";
    case 0x01: return "operation not permitted";
    case 0x02: return "file not found";
    case 0x03: return "I/O error";
    case 0x04: return "no such device";
    case 0x06: return "bad file number";
    case 0x07: return "try again";
    case 0x09: return "access denied";
    case 0x0A: return "resource busy";
    case 0x0C: return "not a directory";
    case 0x0D: return "is a directory";
    case 0x0E: return "invalid argument";
    case 0x10: return "too many files open";
    case 0x14: return "read-only filesystem";
    case 0x15: return "name too long";
    case 0x16: return "function unimplemented";
    case 0x20: return "stale handle";
    case 0x21: return "end of file";
    case 0xFF: return "invalid handle (usually: the session id was not adopted)";
    default:   return nullptr;
    }
}

std::string statusText(uint8_t s)
{
    const char* n = statusName(s);
    if (n) return n;
    char buf[32];
    std::snprintf(buf, sizeof buf, "TNFS status 0x%02X", s);
    return buf;
}

}  // namespace

TnfsClient::~TnfsClient() { unmount(); }

// ── Transport ─────────────────────────────────────────────────────────────

bool TnfsClient::openTransport(const std::string& host, uint16_t port,
                               bool wantTcp, std::string& errOut)
{
    closeTransport();

    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = wantTcp ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = wantTcp ? IPPROTO_TCP : IPPROTO_UDP;

    const std::string portStr = std::to_string(port);
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) {
        errOut = "cannot resolve " + host;
        return false;
    }

    bool ok = false;
    for (addrinfo* a = res; a; a = a->ai_next) {
        socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (!isValidSocket(s)) continue;
        disableSigpipe(s);

        // A blocking connect with no timeout can wedge the caller for the
        // stack's own retry budget (a minute or more on some hosts), and this
        // runs on the CPU thread. UDP "connect" only fixes the peer address,
        // so it cannot block; TCP gets the socket timeout below, which BSD
        // stacks apply to connect() as well.
        timeval tv{};
        tv.tv_sec  = timeoutMs_ / 1000;
        tv.tv_usec = (timeoutMs_ % 1000) * 1000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof tv);
        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof tv);

        if (::connect(s, a->ai_addr, static_cast<socklen_c>(a->ai_addrlen)) == 0) {
            fd_  = s;
            tcp_ = wantTcp;
            ok   = true;
            break;
        }
        closeHostSocketValue(s);
    }
    ::freeaddrinfo(res);

    if (!ok) {
        errOut = std::string(wantTcp ? "TCP" : "UDP") + " connect to " + host +
                 ":" + portStr + " failed: " + lastSocketErrorText();
    }
    return ok;
}

void TnfsClient::closeTransport()
{
    if (isValidSocket(fd_)) closeHostSocket(fd_);
    fd_ = kInvalidSocket;
}

bool TnfsClient::sendRecvTcp(const uint8_t* pkt, std::size_t n,
                             std::vector<uint8_t>& raw, std::string& errOut)
{
    if (sendNoSignal(fd_, pkt, n) != static_cast<iolen_t>(n)) {
        errOut = "TNFS send failed: " + lastSocketErrorText();
        return false;
    }

    // No length prefix on the wire (see the header note). One response packet
    // arrives per request, so a single recv() normally brings the whole thing
    // -- but a split segment is legal TCP, so top up to the header, and for a
    // READ to the byte count that response declares. Getting this wrong
    // desynchronises the session on the first large block rather than
    // failing loudly.
    uint8_t buf[kHeaderSize + kMaxPayload];
    std::size_t have = 0;
    auto pull = [&](std::size_t want) -> bool {
        while (have < want) {
            const iolen_t r = ::recv(fd_, reinterpret_cast<char*>(buf + have),
                                     static_cast<iolen_t>(want - have), 0);
            if (r <= 0) return false;
            have += static_cast<std::size_t>(r);
        }
        return true;
    };

    const iolen_t first = ::recv(fd_, reinterpret_cast<char*>(buf),
                                 static_cast<iolen_t>(sizeof buf), 0);
    if (first <= 0) { errOut = "TNFS: no response (TCP)"; return false; }
    have = static_cast<std::size_t>(first);

    if (!pull(kHeaderSize + 1)) {              // header + status byte
        errOut = "TNFS: truncated response header";
        return false;
    }
    if (buf[3] == kTnfsRead && buf[kHeaderSize] == 0x00) {
        if (!pull(kHeaderSize + 3)) {          // status + 16-bit count
            errOut = "TNFS: truncated READ header";
            return false;
        }
        const std::size_t want = kHeaderSize + 3 + get16(buf + kHeaderSize + 1);
        if (want > sizeof buf) { errOut = "TNFS: oversized READ reply"; return false; }
        if (!pull(want)) { errOut = "TNFS: truncated READ payload"; return false; }
        have = want;
    }

    raw.assign(buf, buf + have);
    return true;
}

bool TnfsClient::sendRecvUdp(const uint8_t* pkt, std::size_t n,
                             std::vector<uint8_t>& raw, std::string& errOut)
{
    uint8_t buf[kHeaderSize + kMaxPayload];
    for (int attempt = 0; attempt < retries_; ++attempt) {
        if (sendNoSignal(fd_, pkt, n) != static_cast<iolen_t>(n)) {
            errOut = "TNFS send failed: " + lastSocketErrorText();
            return false;
        }
        const iolen_t r = ::recv(fd_, reinterpret_cast<char*>(buf),
                                 static_cast<iolen_t>(sizeof buf), 0);
        if (r < kHeaderSize) continue;                    // timeout, retry
        // A reply carrying an older sequence number is a straggler from a
        // previous attempt: drop it and keep waiting on this one.
        if (buf[2] != pkt[2]) { --attempt; continue; }
        raw.assign(buf, buf + r);
        return true;
    }
    errOut = "TNFS: no response after " + std::to_string(retries_) +
             " attempts (UDP)";
    return false;
}

bool TnfsClient::transact(uint8_t command, const uint8_t* payload,
                          std::size_t n, std::vector<uint8_t>& reply,
                          std::string& errOut)
{
    if (!isValidSocket(fd_)) { errOut = "TNFS: not connected"; return false; }
    if (n > kMaxPayload)     { errOut = "TNFS: payload too large"; return false; }

    std::vector<uint8_t> pkt;
    pkt.reserve(kHeaderSize + n);
    pkt.push_back(static_cast<uint8_t>(session_ & 0xFF));
    pkt.push_back(static_cast<uint8_t>(session_ >> 8));
    pkt.push_back(sequence_++);
    pkt.push_back(command);
    pkt.insert(pkt.end(), payload, payload + n);

    std::vector<uint8_t> raw;
    const bool ok = tcp_ ? sendRecvTcp(pkt.data(), pkt.size(), raw, errOut)
                         : sendRecvUdp(pkt.data(), pkt.size(), raw, errOut);
    if (!ok) return false;
    if (raw.size() < static_cast<std::size_t>(kHeaderSize)) {
        errOut = "TNFS: runt response";
        return false;
    }
    // The server assigns the session id in the MOUNT response HEADER, so the
    // header is stripped here rather than in the transports -- both of them
    // hand back the whole packet and this is the one place that knows what
    // the header means.
    replySession_ = get16(raw.data());
    reply.assign(raw.begin() + kHeaderSize, raw.end());
    return true;
}

// ── Session ───────────────────────────────────────────────────────────────

bool TnfsClient::mount(const std::string& host, uint16_t port,
                       const std::string& path, std::string& errOut)
{
    unmount();
    host_ = host;
    port_ = port ? port : kDefaultPort;

    // MOUNT payload: protocol version (minor, major), then mount point, user
    // and password, each NUL-terminated. POM2 mounts anonymously — the spec
    // allows it and every public server this targets is anonymous.
    std::vector<uint8_t> body;
    body.push_back(0x00);                 // version minor
    body.push_back(0x01);                 // version major -> 1.0
    const std::string mp = path.empty() ? "/" : path;
    body.insert(body.end(), mp.begin(), mp.end());
    body.push_back(0x00);
    body.push_back(0x00);                 // user
    body.push_back(0x00);                 // password

    // TCP first, UDP second — see the header note on why the fallback earns
    // its keep.
    std::string tcpErr, udpErr;
    for (int pass = 0; pass < 2; ++pass) {
        const bool wantTcp = (pass == 0);
        std::string& err = wantTcp ? tcpErr : udpErr;
        if (!openTransport(host_, port_, wantTcp, err)) continue;

        session_  = 0;
        sequence_ = 0;
        std::vector<uint8_t> reply;
        if (!transact(kTnfsMount, body.data(), body.size(), reply, err)) {
            closeTransport();
            continue;
        }
        if (reply.empty() || reply[0] != 0x00) {
            err = "MOUNT refused: " +
                  (reply.empty() ? std::string("empty reply")
                                 : statusText(reply[0]));
            closeTransport();
            continue;
        }
        // MOUNT is the one command whose answer matters in its HEADER: that
        // is where the server puts the session id it just assigned, and every
        // later request must echo it back. Miss this and the session stays 0,
        // which a strict server answers with TNFS_RESULT_INVALID_HANDLE
        // (0xFF) on the very next command — the symptom this cost a debugging
        // round to recognise, because MOUNT itself still reports success.
        session_ = replySession_;
        mounted_ = true;
        return true;
    }

    errOut = "TNFS mount failed. TCP: " + (tcpErr.empty() ? "n/a" : tcpErr) +
             " | UDP: " + (udpErr.empty() ? "n/a" : udpErr);
    closeTransport();
    return false;
}

void TnfsClient::unmount()
{
    if (mounted_ && isValidSocket(fd_)) {
        std::vector<uint8_t> reply;
        std::string err;
        transact(kTnfsUnmount, nullptr, 0, reply, err);
    }
    mounted_ = false;
    session_ = 0;
    closeTransport();
}

// ── Directories ───────────────────────────────────────────────────────────

bool TnfsClient::listDir(const std::string& path, std::vector<DirEntry>& out,
                         std::string& errOut)
{
    out.clear();
    if (!mounted_) { errOut = "TNFS: not mounted"; return false; }

    std::vector<uint8_t> body(path.begin(), path.end());
    body.push_back(0x00);
    std::vector<uint8_t> reply;
    if (!transact(kTnfsOpenDir, body.data(), body.size(), reply, errOut))
        return false;
    if (reply.size() < 2 || reply[0] != 0x00) {
        errOut = "OPENDIR " + path + ": " +
                 (reply.empty() ? "empty reply" : statusText(reply[0]));
        return false;
    }
    const uint8_t handle = reply[1];

    // READDIR yields one NUL-terminated name per call and ends with a
    // non-zero status (EOF is reported as an error code, not a flag).
    for (;;) {
        std::vector<uint8_t> r;
        if (!transact(kTnfsReadDir, &handle, 1, r, errOut)) break;
        if (r.size() < 2 || r[0] != 0x00) break;          // end of directory
        const char* name = reinterpret_cast<const char*>(r.data() + 1);
        const std::size_t max = r.size() - 1;
        const std::size_t len = ::strnlen(name, max);
        if (len == 0) break;
        DirEntry e;
        e.name = std::string(name, len);
        // The plain READDIR carries no type flag; a trailing '/' is the
        // convention servers use, and OPENDIRX (which does carry flags) is
        // deliberately not used here — it is not universally implemented.
        if (!e.name.empty() && e.name.back() == '/') {
            e.isDir = true;
            e.name.pop_back();
        }
        if (e.name != "." && e.name != "..") out.push_back(std::move(e));
    }

    std::vector<uint8_t> r;
    std::string ignored;
    transact(kTnfsCloseDir, &handle, 1, r, ignored);
    return true;
}

// ── Files ─────────────────────────────────────────────────────────────────

int TnfsClient::openFile(const std::string& path, std::string& errOut)
{
    if (!mounted_) { errOut = "TNFS: not mounted"; return -1; }

    std::vector<uint8_t> body;
    put16(body, 0x0001);                  // open mode: read only
    put16(body, 0x0000);                  // create permissions: unused
    body.insert(body.end(), path.begin(), path.end());
    body.push_back(0x00);

    std::vector<uint8_t> reply;
    if (!transact(kTnfsOpen, body.data(), body.size(), reply, errOut)) return -1;
    if (reply.size() < 2 || reply[0] != 0x00) {
        errOut = "OPEN " + path + ": " +
                 (reply.empty() ? "empty reply" : statusText(reply[0]));
        return -1;
    }
    return reply[1];
}

bool TnfsClient::fileSize(const std::string& path, uint32_t& out,
                          std::string& errOut)
{
    if (!mounted_) { errOut = "TNFS: not mounted"; return false; }
    std::vector<uint8_t> body(path.begin(), path.end());
    body.push_back(0x00);

    std::vector<uint8_t> reply;
    if (!transact(kTnfsStat, body.data(), body.size(), reply, errOut)) return false;
    if (reply.size() < 9 || reply[0] != 0x00) {
        errOut = "STAT " + path + ": " +
                 (reply.empty() ? "empty reply" : statusText(reply[0]));
        return false;
    }
    // Reply: status, mode(2), uid(2), gid(2), size(4), ...
    const uint8_t* p = reply.data() + 7;
    out = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    return true;
}

bool TnfsClient::readAt(int handle, uint32_t offset, uint8_t* dst, uint32_t n,
                        std::string& errOut)
{
    if (!mounted_) { errOut = "TNFS: not mounted"; return false; }
    if (handle < 0) { errOut = "TNFS: bad handle"; return false; }

    // Seek once, then read forward: the server keeps the file position, so
    // one LSEEK per call is enough however many chunks the read takes.
    {
        std::vector<uint8_t> body;
        body.push_back(static_cast<uint8_t>(handle));
        body.push_back(0x00);             // SEEK_SET
        body.push_back(static_cast<uint8_t>(offset & 0xFF));
        body.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>((offset >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((offset >> 24) & 0xFF));
        std::vector<uint8_t> reply;
        if (!transact(kTnfsLseek, body.data(), body.size(), reply, errOut))
            return false;
        if (reply.empty() || reply[0] != 0x00) {
            errOut = "LSEEK: " + (reply.empty() ? std::string("empty reply")
                                                : statusText(reply[0]));
            return false;
        }
    }

    uint32_t done = 0;
    while (done < n) {
        const uint16_t chunk = static_cast<uint16_t>(
            (n - done > static_cast<uint32_t>(kMaxReadChunk))
                ? kMaxReadChunk : (n - done));
        std::vector<uint8_t> body;
        body.push_back(static_cast<uint8_t>(handle));
        put16(body, chunk);

        std::vector<uint8_t> reply;
        if (!transact(kTnfsRead, body.data(), body.size(), reply, errOut))
            return false;
        if (reply.size() < 3 || reply[0] != 0x00) {
            errOut = "READ: " + (reply.empty() ? std::string("empty reply")
                                               : statusText(reply[0]));
            return false;
        }
        const uint16_t got = get16(reply.data() + 1);
        if (got == 0 || reply.size() < static_cast<std::size_t>(3 + got)) {
            errOut = "READ: short reply (" + std::to_string(got) + " declared, " +
                     std::to_string(reply.size()) + " bytes)";
            return false;
        }
        std::memcpy(dst + done, reply.data() + 3, got);
        done += got;
    }
    return true;
}

void TnfsClient::closeFile(int handle)
{
    if (!mounted_ || handle < 0) return;
    const uint8_t h = static_cast<uint8_t>(handle);
    std::vector<uint8_t> reply;
    std::string ignored;
    transact(kTnfsClose, &h, 1, reply, ignored);
}

}  // namespace pom2
