// SP-over-SLIP session-layer test — pins src/SpOverSlipLink.cpp.
//
// Drives the real link against a FAKE FUJINET running in-process on loopback:
// a thread that connects to POM2's listener, decodes SLIP frames with the
// same framer, and answers them the way the protocol says a FujiNet would.
// No FujiNet firmware, no hardware, no network beyond 127.0.0.1.
//
// What is pinned, worst-consequence first:
//
//   1. ENUMERATION STOPS AT THE RIGHT UNIT. The chain is discovered by
//      INIT-ing units until one answers non-zero. The FujiNet AppleWin fork
//      registers a device even for the unit whose INIT *failed* (it inserts
//      before testing its own `still_scanning` flag), so its count runs one
//      high; POM2 deliberately does not, and this test is what keeps that
//      divergence from being "fixed" back into a bug.
//   2. A BLOCK ROUND TRIP IS BYTE-EXACT. 512 bytes chosen to contain $C0 and
//      $DB in quantity, because those are what the framing escapes — a
//      broken escape corrupts disk data rather than failing loudly.
//   3. A STALE RESPONSE IS DISCARDED. This is the single reason the protocol
//      carries a request sequence number: after a guest reset, the response
//      to the pre-reset request is still sitting in the socket buffer, and
//      reading it as the answer to the NEXT call would hand ProDOS one
//      block's data labelled as another's.
//   4. A DEAD PEER TIMES OUT AND THE LINK SURVIVES. The emulated 6502 is
//      parked inside a JSR for the whole round trip, so "no answer" must
//      become a bounded stall and a clean I/O error, never a hang.

#include "SlipFramer.h"
#include "SpOverSlipLink.h"
#include "SocketCompat.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#if !POM2_HAS_SOCKETS

int main()
{
    std::puts("SKIP: built without host sockets");
    return 0;
}

#else

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace pom2;

using Handler = std::function<void(const std::vector<uint8_t>& req,
                                   std::vector<uint8_t>&       wire)>;

/// A fake FujiNet: connects to POM2's listener and answers SLIP frames.
class FakePeer
{
public:
    FakePeer(uint16_t port, Handler h) : handler_(std::move(h))
    {
        ensureSocketStack();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(isValidSocket(fd_));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = hostToNet16(port);
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        const int r = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr),
                                sizeof(addr));
        assert(r == 0);
        (void)r;
        th_ = std::thread(&FakePeer::run, this);
    }

    ~FakePeer() { stop(); }

    void stop()
    {
        if (stopped_.exchange(true)) return;
        shutdownBoth(fd_);
        if (th_.joinable()) th_.join();
        closeHostSocket(fd_);
    }

    /// Requests seen so far, decoded bodies.
    std::vector<std::vector<uint8_t>> seen()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return seen_;
    }

private:
    void run()
    {
        SlipFramer rx;
        uint8_t    buf[512];
        while (!stopped_.load()) {
            if (waitSocket(fd_, SocketWait::Read, 100) != WaitResult::Ready)
                continue;
            const iolen_t got = recvSocket(fd_, buf, sizeof(buf));
            if (got <= 0) break;
            for (iolen_t i = 0; i < got; ++i) {
                if (rx.feed(buf[i]) != SlipFramer::Feed::Frame) continue;
                const std::vector<uint8_t> req = rx.frame();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    seen_.push_back(req);
                }
                std::vector<uint8_t> wire;
                handler_(req, wire);
                if (!wire.empty())
                    sendSocket(fd_, wire.data(), wire.size());
            }
        }
    }

    Handler                            handler_;
    socket_t                           fd_ = kInvalidSocket;
    std::thread                        th_;
    std::atomic<bool>                  stopped_{false};
    std::mutex                         mtx_;
    std::vector<std::vector<uint8_t>>  seen_;
};

// ── Response builders, straight from the spec's tables ───────────────────

void pushResponse(std::vector<uint8_t>& wire, uint8_t seq, uint8_t status,
                  const std::vector<uint8_t>& payload = {})
{
    std::vector<uint8_t> body{ seq, status };
    body.insert(body.end(), payload.begin(), payload.end());
    SlipFramer::encode(body, wire);
}

/// A SmartPort DIB: status byte, 3-byte block count, ID length, 16-byte ID,
/// type, subtype, 2-byte version.
std::vector<uint8_t> makeDib(const std::string& name, uint32_t blocks,
                             uint8_t type, uint8_t subtype)
{
    std::vector<uint8_t> d;
    d.push_back(0xF8);                                   // general status
    d.push_back(static_cast<uint8_t>(blocks & 0xFF));
    d.push_back(static_cast<uint8_t>((blocks >> 8) & 0xFF));
    d.push_back(static_cast<uint8_t>((blocks >> 16) & 0xFF));
    d.push_back(static_cast<uint8_t>(name.size()));
    for (size_t i = 0; i < 16; ++i)
        d.push_back(i < name.size() ? static_cast<uint8_t>(name[i]) : ' ');
    d.push_back(type);
    d.push_back(subtype);
    d.push_back(0x01);
    d.push_back(0x00);
    return d;
}

/// The canonical fake: two block devices, nothing at unit 3.
void standardHandler(const std::vector<uint8_t>& req, std::vector<uint8_t>& wire)
{
    assert(req.size() >= 11);
    const uint8_t seq  = req[0];
    const uint8_t cmd  = req[1];
    const uint8_t unit = req[3];

    switch (cmd) {
    case kSpInit:
        pushResponse(wire, seq, unit <= 2 ? 0x00 : 0x01);
        return;

    case kSpStatus: {
        const uint8_t code = req[6];
        if (code == 0x03) {
            pushResponse(wire, seq, 0x00,
                         makeDib(unit == 1 ? "FUJINET" : "SD", 0x1234, 0x02, 0x20));
        } else {
            pushResponse(wire, seq, 0x00, { 0xF8, 0x34, 0x12, 0x00 });
        }
        return;
    }

    case kSpReadBlock: {
        const uint32_t block = static_cast<uint32_t>(req[6]) |
                               (static_cast<uint32_t>(req[7]) << 8) |
                               (static_cast<uint32_t>(req[8]) << 16);
        std::vector<uint8_t> data(512);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>((block * 7 + i * 3) & 0xFF);
        // Make sure the framing escapes are exercised on every read.
        data[0] = 0xC0; data[1] = 0xDB; data[2] = 0xC0; data[511] = 0xDB;
        pushResponse(wire, seq, 0x00, data);
        return;
    }

    case kSpWriteBlock:
        pushResponse(wire, seq, 0x00);
        return;

    default:
        pushResponse(wire, seq, 0x00);
        return;
    }
}

// ── Harness ──────────────────────────────────────────────────────────────

/// Start a link on a free loopback port. CI runs tests in parallel, so a
/// fixed port would be flaky; walk a small range instead.
uint16_t startLink(SpOverSlipLink& link)
{
    std::string err;
    for (uint16_t p = 19850; p < 19890; ++p) {
        link.setTcpMode(p);
        if (link.start(err)) return p;
    }
    std::fprintf(stderr, "could not bind any test port: %s\n", err.c_str());
    assert(false);
    return 0;
}

/// Spin until `pred` or the deadline. Returns whether it came true.
template <typename F>
bool waitFor(F pred, int timeoutMs = 4000)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

// ── 1. Enumeration ───────────────────────────────────────────────────────
void testEnumeration()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.isConnected(); }));
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    const auto devs = link.devices();
    assert(devs.size() == 2);               // NOT 3 — see header comment
    assert(devs[0].unit == 1);
    assert(devs[0].name == "FUJINET");      // DIB decoded, padding trimmed
    assert(devs[0].blocks == 0x1234);
    assert(devs[0].type == 0x02);
    assert(devs[1].unit == 2);
    assert(devs[1].name == "SD");

    peer.stop();
    link.stop();
}

// ── 2. A block round trip ────────────────────────────────────────────────
void testReadWriteBlock()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    const auto r = link.readBlock(1, 0x000102);
    assert(r.ok());
    assert(r.data.size() == 512);
    // The bytes the framing has to escape must survive verbatim.
    assert(r.data[0] == 0xC0);
    assert(r.data[1] == 0xDB);
    assert(r.data[2] == 0xC0);
    assert(r.data[511] == 0xDB);
    for (size_t i = 3; i < 511; ++i)
        assert(r.data[i] == static_cast<uint8_t>((0x000102 * 7 + i * 3) & 0xFF));

    // A write must put the 3-byte block number on the wire little-endian and
    // carry the payload after the 11-byte header.
    std::vector<uint8_t> out(512, 0xDB);
    out[7] = 0xC0;
    const auto w = link.writeBlock(2, 0x0300FF, out.data(), out.size());
    assert(w.ok());

    bool found = false;
    for (const auto& req : peer.seen()) {
        if (req.size() != 11 + 512 || req[1] != kSpWriteBlock) continue;
        assert(req[3] == 2);                       // unit
        assert(req[6] == 0xFF);                    // block low
        assert(req[7] == 0x00);                    // block mid
        assert(req[8] == 0x03);                    // block high
        assert(std::memcmp(req.data() + 11, out.data(), out.size()) == 0);
        found = true;
    }
    assert(found);

    peer.stop();
    link.stop();
}

// ── 3. The stale response the sequence number exists for ─────────────────
void testStaleResponseDiscarded()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    std::atomic<bool> injectStale{false};
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (injectStale.load() && req[1] == kSpReadBlock) {
            // A leftover answer from before a guest reset: right shape,
            // wrong sequence number. The link must throw it away and keep
            // waiting for the one it asked for.
            std::vector<uint8_t> bogus(512, 0xEE);
            pushResponse(wire, static_cast<uint8_t>(req[0] ^ 0x5A), 0x00, bogus);
        }
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.deviceCount() == 2; }));
    const uint64_t staleBefore = link.stats().stale;

    injectStale.store(true);
    const auto r = link.readBlock(1, 0x000005);
    assert(r.ok());
    assert(r.data.size() == 512);
    // The data returned is the REAL one, not the decoy.
    assert(r.data[100] != 0xEE ||
           r.data[100] == static_cast<uint8_t>((0x05 * 7 + 100 * 3) & 0xFF));
    assert(r.data[100] == static_cast<uint8_t>((0x05 * 7 + 100 * 3) & 0xFF));
    assert(link.stats().stale > staleBefore);

    peer.stop();
    link.stop();
}

// ── 4. A silent peer times out, and the link keeps working ───────────────
void testTimeoutAndRecovery()
{
    SpOverSlipLink link;
    link.setTimeoutMs(120);
    const uint16_t port = startLink(link);

    std::atomic<bool> mute{false};
    FakePeer peer(port, [&](const std::vector<uint8_t>& req,
                            std::vector<uint8_t>& wire) {
        if (mute.load() && req[1] == kSpReadBlock) return;   // answer nothing
        standardHandler(req, wire);
    });

    assert(waitFor([&] { return link.deviceCount() == 2; }));

    mute.store(true);
    const auto t0 = std::chrono::steady_clock::now();
    const auto bad = link.readBlock(1, 1);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    assert(!bad.replied);                 // caller turns this into $27
    assert(!bad.ok());
    assert(ms >= 100);                    // it really waited its budget...
    assert(ms < 2000);                    // ...and it really gave up
    assert(link.stats().timeouts >= 1);

    // The link must still be usable — a timeout is not a teardown.
    mute.store(false);
    const auto good = link.readBlock(1, 2);
    assert(good.ok());
    assert(good.data.size() == 512);

    peer.stop();
    link.stop();
}

// ── No peer at all: calls fail immediately, nothing hangs ────────────────
void testNoPeer()
{
    SpOverSlipLink link;
    startLink(link);

    assert(!link.isConnected());
    assert(link.deviceCount() == 0);

    const auto t0 = std::chrono::steady_clock::now();
    const auto r = link.readBlock(1, 0);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    assert(!r.replied);
    assert(ms < 100);        // no peer = no wait at all, not a full timeout

    link.stop();
}

// ── Guest reset: devices are told, and the sequence moves on ─────────────
void testGuestResetNotifiesDevices()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);

    FakePeer peer(port, standardHandler);
    assert(waitFor([&] { return link.deviceCount() == 2; }));

    link.notifyGuestReset();

    // Both units must have been told, with control code $00 — that is what
    // makes a modem hang up and a printer eject on Ctrl-Reset.
    int controls = 0;
    for (const auto& req : peer.seen())
        if (req.size() >= 11 && req[1] == kSpControl && req[6] == 0x00) ++controls;
    assert(controls == 2);

    peer.stop();
    link.stop();
}

// ── stop() must be safe with a peer attached and mid-poll ────────────────
void testCleanShutdown()
{
    SpOverSlipLink link;
    const uint16_t port = startLink(link);
    {
        FakePeer peer(port, standardHandler);
        assert(waitFor([&] { return link.isConnected(); }));
        link.stop();               // with the peer still connected
    }
    assert(!link.isConnected());
    link.stop();                   // idempotent
}

} // namespace

int main()
{
    testEnumeration();
    testReadWriteBlock();
    testStaleResponseDiscarded();
    testTimeoutAndRecovery();
    testNoPeer();
    testGuestResetNotifiesDevices();
    testCleanShutdown();

    std::puts("sp_over_slip_link: OK");
    return 0;
}

#endif // POM2_HAS_SOCKETS
