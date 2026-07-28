// Uthernet II / W5100 smoke test — pins the hardware-TCP model against
// AppleWin `source/Uthernet2.cpp` + `source/W5100.h` (MAME has no W5100
// device) and the WIZnet W5100 datasheet v1.2.8.
//
// The card is driven the way a 6502 driver would drive it: through the
// four-register indirect window on the slot's $C0nX space. Two transports
// are exercised:
//
//   * TCP — against a real loopback listener this test opens itself.
//     This is the path every period IRC / telnet / FTP client takes, and
//     it needs no NetworkBackend at all, which is the whole point of the
//     Uthernet II: it works with no libslirp and no privileges.
//   * MACRAW — against a LoopbackNetworkBackend, no real network.
//
// What this gates:
//
//   * Indirect-window decode — `Uthernet2.cpp:1411-1472`, `W5100.h:5-12`.
//     Only A0/A1 are decoded, so the four registers alias four times
//     across $C0nX; auto-increment follows MR bit 1 and wraps inside each
//     8 KB buffer rather than spilling into the next region.
//   * Power-on register values — `Uthernet2.cpp:1398-1408`. RTR = 0x07D0,
//     RCR = 8, RMSR/TMSR = 0x55, and PTIMER = 0 advertises virtual DNS.
//   * MR RST — `Uthernet2.cpp:1293-1302`. A soft reset clears registers
//     but preserves the indirect data address (Uthernet II manual p.10).
//   * RMSR/TMSR buffer carve — `Uthernet2.cpp:441-485`. 2-bit size codes
//     per socket, clamped at the end of the 8 KB region.
//   * A full TCP session: OPEN → CONNECT → SEND → RECV, with the ring
//     pointers moved the way a driver moves them
//     (`Uthernet2.cpp:844-895`, `:704-745`, `:520-547`).
//   * MACRAW framing — `Uthernet2.cpp:661-680`. The in-band length
//     INCLUDES the two length bytes.
//   * Snapshot round-trip, including the rule that a live TCP socket must
//     come back CLOSED rather than pretending to still be connected.

#include "NetworkBackend.h"
#include "UthernetIICard.h"
#include "W5100Device.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using pom2::UthernetIICard;

// $C0nX offsets. Only A0/A1 are decoded, so these are the canonical
// $C0n4-$C0n7 group reduced to its two significant bits.
constexpr uint8_t kMode   = 0x4;
constexpr uint8_t kAddrHi = 0x5;
constexpr uint8_t kAddrLo = 0x6;
constexpr uint8_t kData   = 0x7;

void setAddress(UthernetIICard& card, uint16_t addr)
{
    card.deviceSelectWrite(kAddrHi, static_cast<uint8_t>((addr >> 8) & 0xFF));
    card.deviceSelectWrite(kAddrLo, static_cast<uint8_t>(addr & 0xFF));
}

uint8_t readAt(UthernetIICard& card, uint16_t addr)
{
    setAddress(card, addr);
    return card.deviceSelectRead(kData);
}

void writeAt(UthernetIICard& card, uint16_t addr, uint8_t value)
{
    setAddress(card, addr);
    card.deviceSelectWrite(kData, value);
}

/// W5100 16-bit registers are big-endian.
void writeWordAt(UthernetIICard& card, uint16_t addr, uint16_t value)
{
    writeAt(card, addr, static_cast<uint8_t>((value >> 8) & 0xFF));
    writeAt(card, static_cast<uint16_t>(addr + 1), static_cast<uint8_t>(value & 0xFF));
}

uint16_t readWordAt(UthernetIICard& card, uint16_t addr)
{
    const uint8_t hi = readAt(card, addr);
    const uint8_t lo = readAt(card, static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint16_t socketBase(size_t i)
{
    return static_cast<uint16_t>(pom2::kW5100S0Base + (i << 8));
}

/// A TCP listener on 127.0.0.1, port chosen by the kernel.
class LocalListener
{
public:
    LocalListener()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd_ >= 0);
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;   // let the kernel pick
        assert(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        assert(::listen(fd_, 1) == 0);

        socklen_t len = sizeof(addr);
        assert(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        port_ = ntohs(addr.sin_port);
    }

    ~LocalListener()
    {
        if (client_ >= 0) ::close(client_);
        if (fd_ >= 0) ::close(fd_);
    }

    uint16_t port() const { return port_; }

    /// Accept the pending connection, waiting up to `timeoutMs`.
    bool acceptOne(int timeoutMs)
    {
        pollfd pfd{ fd_, POLLIN, 0 };
        if (::poll(&pfd, 1, timeoutMs) <= 0) return false;
        client_ = ::accept(fd_, nullptr, nullptr);
        return client_ >= 0;
    }

    /// Read up to `max` bytes from the accepted client.
    std::string read(size_t max, int timeoutMs)
    {
        pollfd pfd{ client_, POLLIN, 0 };
        if (::poll(&pfd, 1, timeoutMs) <= 0) return {};
        std::vector<char> buf(max);
        const ssize_t n = ::recv(client_, buf.data(), buf.size(), 0);
        if (n <= 0) return {};
        return std::string(buf.data(), static_cast<size_t>(n));
    }

    void write(const std::string& s)
    {
        const ssize_t n = ::send(client_, s.data(), s.size(), 0);
        assert(n == static_cast<ssize_t>(s.size()));
    }

private:
    int      fd_     = -1;
    int      client_ = -1;
    uint16_t port_   = 0;
};

/// Give the card a chance to service its host sockets, up to `tries`
/// polling rounds with a short sleep between them.
void pumpCard(UthernetIICard& card, int tries = 200)
{
    for (int i = 0; i < tries; ++i) {
        card.advanceCycles(UthernetIICard::kPollIntervalCycles);
        if (i % 8 == 7) ::usleep(1000);
    }
}

// ── Tests ─────────────────────────────────────────────────────────────

void testPowerOnDefaults()
{
    UthernetIICard card(3);

    // Retry time 0x07D0 (200 ms) and retry count 8 — datasheet defaults.
    assert(readAt(card, pom2::kW5100Rtr0) == 0x07);
    assert(readAt(card, pom2::kW5100Rtr1) == 0xD0);
    assert(readAt(card, pom2::kW5100Rcr)  == 0x08);
    // 0x55 = 2 KB per socket in both directions.
    assert(readAt(card, pom2::kW5100Rmsr) == 0x55);
    assert(readAt(card, pom2::kW5100Tmsr) == 0x55);
    // PTIMER == 0 is how software detects the virtual-DNS extension.
    assert(card.chip().virtualDnsEnabled());
    assert(readAt(card, pom2::kW5100Ptimer) == 0x00);

    // ...and with the extension off it reads its hardware default.
    card.chip().setVirtualDnsEnabled(false);
    assert(readAt(card, pom2::kW5100Ptimer) == 0x28);

    // Every socket starts closed with 2 KB of each ring.
    for (size_t i = 0; i < pom2::W5100Device::kSocketCount; ++i) {
        const auto info = card.chip().socketInfo(i);
        assert(info.status == pom2::kW5100SnSrClosed);
        assert(info.rxCapacity == 2048);
        assert(info.txCapacity == 2048);
    }

    std::printf("  power-on defaults OK\n");
}

void testIndirectWindowDecode()
{
    UthernetIICard card(3);

    // Only A0/A1 are decoded: $C0n0 aliases MODE, $C0n1 aliases ADDR_HI,
    // and so on (`W5100.h:6`).
    card.deviceSelectWrite(0x0, pom2::kW5100MrAi);
    assert(card.chip().modeRegister() == pom2::kW5100MrAi);
    assert(card.deviceSelectRead(0x4) == pom2::kW5100MrAi);
    assert(card.deviceSelectRead(0xC) == pom2::kW5100MrAi);

    // Auto-increment on: consecutive data reads walk the address.
    setAddress(card, pom2::kW5100Rtr0);
    (void)card.deviceSelectRead(kData);
    assert(card.chip().dataAddress() == pom2::kW5100Rtr1);
    (void)card.deviceSelectRead(kData);
    assert(card.chip().dataAddress() == pom2::kW5100Rcr);

    // The window wraps inside each 8 KB buffer instead of spilling into
    // the next region (Uthernet II manual p.12 bottom).
    setAddress(card, static_cast<uint16_t>(pom2::kW5100RxBase - 1));
    (void)card.deviceSelectRead(kData);
    assert(card.chip().dataAddress() == pom2::kW5100TxBase);

    // Auto-increment off: the address is sticky.
    card.deviceSelectWrite(kMode, 0x00);
    setAddress(card, pom2::kW5100Rtr0);
    (void)card.deviceSelectRead(kData);
    assert(card.chip().dataAddress() == pom2::kW5100Rtr0);

    std::printf("  indirect window decode OK\n");
}

void testSoftResetPreservesAddress()
{
    UthernetIICard card(3);

    writeAt(card, pom2::kW5100Sipr0, 192);
    setAddress(card, 0x4321);
    card.deviceSelectWrite(kMode, pom2::kW5100MrRst);

    // Registers cleared...
    assert(readAt(card, pom2::kW5100Sipr0) == 0);
    // ...but the indirect data address survives, per the manual. (readAt
    // above moved it, so check the chip directly after a fresh set.)
    UthernetIICard card2(3);
    setAddress(card2, 0x4321);
    card2.deviceSelectWrite(kMode, pom2::kW5100MrRst);
    assert(card2.chip().dataAddress() == 0x4321);

    std::printf("  MR RST OK\n");
}

void testBufferCarve()
{
    UthernetIICard card(3);

    // 0x00 = 1 KB to every socket; the top 4 KB of each region is unused.
    writeAt(card, pom2::kW5100Rmsr, 0x00);
    writeAt(card, pom2::kW5100Tmsr, 0x00);
    for (size_t i = 0; i < pom2::W5100Device::kSocketCount; ++i) {
        assert(card.chip().socketInfo(i).rxCapacity == 1024);
        assert(card.chip().socketInfo(i).txCapacity == 1024);
    }

    // 0x03 = socket 0 asks for the whole 8 KB; the rest get clamped to
    // nothing rather than running off the end of the region.
    writeAt(card, pom2::kW5100Rmsr, 0x03);
    assert(card.chip().socketInfo(0).rxCapacity == 8192);
    assert(card.chip().socketInfo(1).rxCapacity == 0);
    assert(card.chip().socketInfo(2).rxCapacity == 0);
    assert(card.chip().socketInfo(3).rxCapacity == 0);

    std::printf("  RMSR/TMSR carve OK\n");
}

void testTcpSession()
{
    LocalListener listener;
    UthernetIICard card(3);
    // Deliberately no NetworkBackend: TCP must work without one.
    assert(card.backend() == nullptr);

    constexpr size_t kSock = 0;
    const uint16_t base = socketBase(kSock);

    // Our IP, so the ARP/IPRAW helpers have something sane; irrelevant to
    // TCP, which goes straight out a host socket.
    writeAt(card, pom2::kW5100Sipr0, 127);
    writeAt(card, pom2::kW5100Sipr0 + 1, 0);
    writeAt(card, pom2::kW5100Sipr0 + 2, 0);
    writeAt(card, pom2::kW5100Sipr0 + 3, 1);

    // Socket 0 in TCP mode, local port 1234.
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnMr), pom2::kW5100SnMrTcp);
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnPort0), 1234);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrOpen);
    assert(readAt(card, static_cast<uint16_t>(base + pom2::kW5100SnSr)) ==
           pom2::kW5100SnSrInit);

    // Destination = 127.0.0.1 : listener port.
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr0), 127);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr0 + 1), 0);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr0 + 2), 0);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr3), 1);
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDport0),
                listener.port());

    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrConnect);

    // A non-blocking connect reports SYNSENT until poll() promotes it —
    // exactly what the real chip does while the handshake is in flight.
    assert(listener.acceptOne(2000));
    pumpCard(card);
    const uint8_t sr = readAt(card, static_cast<uint16_t>(base + pom2::kW5100SnSr));
    assert(sr == pom2::kW5100SnSrEstablished);

    // ── SEND ──────────────────────────────────────────────────────────
    // Stage payload in the TX ring at the write pointer, advance
    // SN_TX_WR, then issue SEND (`Uthernet2.cpp:844-895`).
    const std::string outbound = "NICK pom2\r\n";
    const uint16_t txBase = pom2::kW5100TxBase;   // socket 0 starts here
    for (size_t i = 0; i < outbound.size(); ++i) {
        writeAt(card, static_cast<uint16_t>(txBase + i),
                static_cast<uint8_t>(outbound[i]));
    }
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnTxWr0),
                static_cast<uint16_t>(outbound.size()));
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrSend);

    const std::string received = listener.read(256, 2000);
    assert(received == outbound);

    // The read pointer caught up with the write pointer, so the free size
    // is back to the full ring.
    assert(readWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnTxRd0)) ==
           outbound.size());
    assert(readWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnTxFsr0)) == 2048);

    // ── RECV ──────────────────────────────────────────────────────────
    const std::string inbound = ":server 001 pom2 :Welcome\r\n";
    listener.write(inbound);

    // Reading SN_RX_RSR is what pulls data off the host socket — the
    // chip is polled, it has no interrupt line on this card.
    uint16_t pending = 0;
    for (int i = 0; i < 200 && pending == 0; ++i) {
        pending = readWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnRxRsr0));
        if (pending == 0) ::usleep(1000);
    }
    assert(pending == inbound.size());

    // TCP has no in-band header, so the payload starts at SN_RX_RD.
    const uint16_t rxBase = pom2::kW5100RxBase;
    const uint16_t rxRd = readWordAt(card,
        static_cast<uint16_t>(base + pom2::kW5100SnRxRd0));
    std::string got;
    for (uint16_t i = 0; i < pending; ++i) {
        got.push_back(static_cast<char>(
            readAt(card, static_cast<uint16_t>(rxBase + ((rxRd + i) % 2048)))));
    }
    assert(got == inbound);

    // Advance the read pointer and RECV to re-sync the staged size.
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnRxRd0),
                static_cast<uint16_t>(rxRd + pending));
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrRecv);
    assert(readWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnRxRsr0)) == 0);

    // ── CLOSE ─────────────────────────────────────────────────────────
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrClose);
    assert(readAt(card, static_cast<uint16_t>(base + pom2::kW5100SnSr)) ==
           pom2::kW5100SnSrClosed);

    std::printf("  TCP session OK (sent %zu, received %zu bytes, no backend)\n",
                outbound.size(), inbound.size());
}

void testMacRaw()
{
    UthernetIICard card(3);
    auto backend = std::make_unique<pom2::LoopbackNetworkBackend>();
    auto* raw = backend.get();
    card.setBackend(std::move(backend));

    constexpr size_t kSock = 0;   // MACRAW is socket 0 only on the W5100
    const uint16_t base = socketBase(kSock);

    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnMr), pom2::kW5100SnMrMacRaw);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrOpen);
    assert(readAt(card, static_cast<uint16_t>(base + pom2::kW5100SnSr)) ==
           pom2::kW5100SnSrMacRaw);

    // Build a frame addressed to our own MAC so the RX filter takes it.
    const std::array<uint8_t, 6> ourMac = card.chip().macAddress();
    std::vector<uint8_t> frame(64, 0);
    std::memcpy(frame.data(), ourMac.data(), 6);
    std::memcpy(frame.data() + 6, ourMac.data(), 6);
    frame[12] = 0x08; frame[13] = 0x06;   // ARP
    for (size_t i = 14; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>(i);

    // Transmit through the ring: stage, advance SN_TX_WR, SEND.
    for (size_t i = 0; i < frame.size(); ++i)
        writeAt(card, static_cast<uint16_t>(pom2::kW5100TxBase + i), frame[i]);
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnTxWr0),
                static_cast<uint16_t>(frame.size()));
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrSend);
    assert(raw->queued() == 1);

    // The loopback hands it straight back; reading SN_RX_RSR pulls it in.
    const uint16_t pending =
        readWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnRxRsr0));
    // MACRAW's in-band length INCLUDES the two length bytes, so the ring
    // holds 2 + 64 bytes (`Uthernet2.cpp:661-680`).
    assert(pending == frame.size() + 2);

    const uint16_t rxBase = pom2::kW5100RxBase;
    const uint16_t declared = static_cast<uint16_t>(
        (readAt(card, rxBase) << 8) | readAt(card, static_cast<uint16_t>(rxBase + 1)));
    assert(declared == frame.size() + 2);

    for (size_t i = 0; i < frame.size(); ++i) {
        assert(readAt(card, static_cast<uint16_t>(rxBase + 2 + i)) == frame[i]);
    }

    std::printf("  MACRAW OK (%zu-byte frame round-tripped)\n", frame.size());
}

void testSnapshotRoundTrip()
{
    LocalListener listener;
    UthernetIICard card(3);

    constexpr size_t kSock = 1;
    const uint16_t base = socketBase(kSock);

    // Something distinctive in the common registers...
    writeAt(card, pom2::kW5100Sipr0, 10);
    writeAt(card, pom2::kW5100Sipr0 + 1, 0);
    writeAt(card, pom2::kW5100Sipr0 + 2, 2);
    writeAt(card, pom2::kW5100Sipr0 + 3, 15);
    writeAt(card, pom2::kW5100Rmsr, 0x00);   // 1 KB per socket

    // ...and a live TCP socket.
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnMr), pom2::kW5100SnMrTcp);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrOpen);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr0), 127);
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDipr3), 1);
    writeWordAt(card, static_cast<uint16_t>(base + pom2::kW5100SnDport0), listener.port());
    writeAt(card, static_cast<uint16_t>(base + pom2::kW5100SnCr), pom2::kW5100SnCrConnect);
    assert(listener.acceptOne(2000));
    pumpCard(card);
    assert(readAt(card, static_cast<uint16_t>(base + pom2::kW5100SnSr)) ==
           pom2::kW5100SnSrEstablished);

    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    assert(!blob.empty());

    UthernetIICard restored(3);
    restored.loadSnapshotState(blob.data(), blob.size());

    // Registers and buffer geometry come back...
    assert(readAt(restored, pom2::kW5100Sipr0) == 10);
    assert(readAt(restored, pom2::kW5100Sipr0 + 3) == 15);
    assert(restored.chip().socketInfo(kSock).rxCapacity == 1024);
    // ...but the TCP connection does NOT: the peer moved on while the
    // ring was rewound and the fd is gone, so the socket must report
    // CLOSED rather than pretend to be established.
    assert(restored.chip().socketInfo(kSock).status == pom2::kW5100SnSrClosed);
    assert(!restored.chip().socketInfo(kSock).hasHostSocket);

    // A foreign blob in this slot must be ignored, not misparsed.
    std::vector<uint8_t> foreign(blob.size(), 0xAB);
    UthernetIICard untouched(3);
    untouched.loadSnapshotState(foreign.data(), foreign.size());
    assert(readAt(untouched, pom2::kW5100Sipr0) == 0);

    std::printf("  snapshot round-trip OK (%zu bytes)\n", blob.size());
}

} // namespace

int main()
{
    std::printf("Uthernet II / W5100 smoke test\n");
    testPowerOnDefaults();
    testIndirectWindowDecode();
    testSoftResetPreservesAddress();
    testBufferCarve();
    testTcpSession();
    testMacRaw();
    testSnapshotRoundTrip();
    std::printf("PASS\n");
    return 0;
}
