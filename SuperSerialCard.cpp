// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SuperSerialCard.h"
#include "Logger.h"
#include "M6502.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr size_t kBufCap   = 4096;     // bounded ring buffer (TX and RX)
constexpr size_t kTailCap  = 256;      // recent-bytes peephole

// MAME `mos6551.cpp:46` — internal baud-rate divider table indexed by
// control bits 0-3. Index 0 is the "16x external clock" mode (effectively
// unconstrained for POM2). Otherwise baud = xtal / (divider * 16) with
// the SSC's 1.8432 MHz crystal — the resulting standard rates run from
// 50 baud (index 1) to 19200 (index 15).
constexpr int kInternalDivider[16] = {
    1, 2304, 1536, 1048, 856, 768, 384, 192, 96, 64, 48, 32, 24, 16, 12, 6
};
constexpr double kSscXtalHz = 1843200.0;

// MAME `mos6551.cpp:49-55`: per-cmd[2:3] table of {tx_irq, rts_active, brk}.
// Only `tx_irq` is consulted in POM2 (TDRE is pinned high so the IRQ never
// fires anyway, but we still mirror the state so cmdReg readback matches a
// real driver's expectation).
constexpr bool kTxIrqEnableByCmd[4] = { false, true, false, false };

double baudIndexToBytesPerSec(uint8_t idx)
{
    if (idx == 0) return 0.0;     // 16x ext clk — treat as unconstrained
    const int divider = kInternalDivider[idx & 0xF];
    if (divider <= 0) return 0.0;
    const double baud = kSscXtalHz / (divider * 16.0);
    // 8-N-1 framing: 1 start + 8 data + 1 stop = 10 bit-times per byte.
    // (We don't refine by extraStop/wordlength since this is the most
    // common configuration and the error is ≤10% — negligible vs the
    // wall-clock jitter of the worker's 2 ms idle.)
    return baud / 10.0;
}

// Telnet IAC = $FF. We swallow IAC + 2 bytes (the most common 3-byte
// commands: WILL/WONT/DO/DONT) so a stock `telnet` client's option
// negotiation doesn't leak garbage bytes into the Apple II keyboard.
// IAC IAC ($FF $FF) is a literal escaped $FF — pass one $FF through.
size_t swallowTelnetIac(uint8_t* data, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n; ++r) {
        if (data[r] != 0xFF) {
            data[w++] = data[r];
            continue;
        }
        if (r + 1 >= n) break;          // partial; drop the trailing IAC
        const uint8_t cmd = data[r + 1];
        if (cmd == 0xFF) {              // escaped literal
            data[w++] = 0xFF;
            r += 1;
        } else if (cmd >= 0xFB && cmd <= 0xFE) {
            // WILL / WONT / DO / DONT — 3-byte command, drop all three.
            r += 2;
        } else {
            r += 1;                     // 2-byte command, drop both.
        }
    }
    return w;
}

// normalizeLineEndings moved to a public static member (see header) so it
// can be unit-tested directly; definition follows the anonymous namespace.

}  // namespace

// Line-ending normalisation for telnet-sourced RX: a stock telnet client
// sends CR LF on ENTER and bare LF on some line-mode tools, but the Apple
// II expects CR alone. Applying once here on RX symmetrises the rxBuf and
// keyboard-sink consumers.
// Transformations:
//   * drop NUL ($00) FIRST — per RFC 854 a telnet client transmits a bare
//     carriage return as the two-byte sequence CR NUL. The NUL must be
//     dropped before it can touch `prevCR`; otherwise the CR-state is lost
//     and a following LF (CR NUL LF) is wrongly emitted as a second CR.
//   * collapse CR LF → CR (strip the LF after a CR).
//   * map bare LF → CR.
// Buffer mutated in place, returns new length.
size_t SuperSerialCard::normalizeLineEndings(uint8_t* data, size_t n)
{
    size_t w = 0;
    bool prevCR = false;
    for (size_t r = 0; r < n; ++r) {
        uint8_t c = data[r];
        if (c == 0) continue;                       // drop NUL before prevCR
        if (c == '\n' && prevCR) { prevCR = false; continue; }
        prevCR = (c == '\r');
        if (c == '\n') c = '\r';
        data[w++] = c;
    }
    return w;
}

SuperSerialCard::SuperSerialCard(int slotNum)
    : slot(slotNum)
{
    buildRom();
}

SuperSerialCard::~SuperSerialCard()
{
    stopListening();
}

bool SuperSerialCard::startListening(uint16_t newPort)
{
    if (listening) {
        if (newPort == port) return true;
        stopListening();
    }
    port = newPort;
    listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        pom2::log().warn("SSC", std::string("socket() failed: ") + std::strerror(errno));
        return false;
    }
    int yes = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        pom2::log().warn("SSC",
            "bind 127.0.0.1:" + std::to_string(port) + " failed: " +
            std::strerror(errno));
        ::close(listenFd);
        listenFd = -1;
        return false;
    }
    if (::listen(listenFd, 1) < 0) {
        pom2::log().warn("SSC", std::string("listen() failed: ") +
                         std::strerror(errno));
        ::close(listenFd);
        listenFd = -1;
        return false;
    }

    stopRequested = false;
    listening     = true;
    worker        = std::thread(&SuperSerialCard::runWorker, this);
    pom2::log().info("SSC",
        "listening on 127.0.0.1:" + std::to_string(port) +
        " (telnet to connect to slot " + std::to_string(slot) + ")");
    return true;
}

void SuperSerialCard::stopListening()
{
    if (!listening && !worker.joinable()) return;
    stopRequested = true;
    closeClient();
    if (listenFd >= 0) {
        ::shutdown(listenFd, SHUT_RDWR);
        ::close(listenFd);
        listenFd = -1;
    }
    if (worker.joinable()) worker.join();
    listening = false;
}

void SuperSerialCard::closeClient()
{
    if (clientFd >= 0) {
        ::shutdown(clientFd, SHUT_RDWR);
        ::close(clientFd);
        clientFd = -1;
    }
    connected = false;
}

void SuperSerialCard::runWorker()
{
    while (!stopRequested) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        const int fd = ::accept(listenFd,
                                reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (stopRequested) break;
            if (errno == EINTR) continue;
            // Listening socket closed under us — bail.
            break;
        }
        // Disable Nagle so single-character writes from the Apple II
        // appear at the telnet client immediately.
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        // Make read non-blocking so the worker can drain TX between RX
        // arrivals without sleeping on input.
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        clientFd  = fd;
        connected = true;
        onConnectionEdge(true);
        pom2::log().info("SSC",
            std::string("client connected from ") +
            ::inet_ntoa(peer.sin_addr));

        // Bridge loop: pull bytes from socket → rxBuf, push txBuf → socket.
        // Sleep briefly between iterations to avoid a hot spin when both
        // queues are idle.
        uint8_t scratch[256];
        while (!stopRequested && clientFd >= 0) {
            const ssize_t got = ::recv(clientFd, scratch, sizeof(scratch), 0);
            if (got > 0) {
                size_t n = static_cast<size_t>(got);
                // Raw mode: skip both filters so XMODEM/Kermit/ADTPro
                // see every byte (including $FF and bare LFs).
                if (!rawMode_.load(std::memory_order_relaxed)) {
                    n = swallowTelnetIac(scratch, n);
                    n = normalizeLineEndings(scratch, n);
                }
                if (n > 0) {
                    deliverRxBytes(scratch, n);
                    // Snapshot the keyboard sink under the same lock the
                    // queues use, then drop the lock before calling out —
                    // `Memory::queueKey` takes its own mutex and we MUST
                    // NOT hold ours during that.
                    std::function<void(uint8_t)> sink;
                    {
                        std::lock_guard<std::mutex> lk(bufferMtx);
                        sink = keyboardSink;
                    }
                    if (sink) {
                        // Line endings already normalised above — just
                        // forward each byte to the Apple II keyboard latch.
                        for (size_t i = 0; i < n; ++i) {
                            sink(scratch[i]);
                        }
                    }
                }
            } else if (got == 0) {
                // Peer closed.
                pom2::log().info("SSC", "client disconnected");
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                pom2::log().info("SSC",
                    std::string("recv error: ") + std::strerror(errno));
                break;
            }

            // Drain TX buffer to socket. Rate-limited if a baud-rate
            // divider has been programmed; otherwise dumped wholesale.
            std::vector<uint8_t> outChunk;
            {
                std::lock_guard<std::mutex> lk(bufferMtx);
                const auto now = std::chrono::steady_clock::now();
                if (bytesPerSecond_ > 0.0) {
                    const double dt = std::chrono::duration<double>(
                        now - lastDrainTime_).count();
                    // Cap the budget at one ring's worth so a long idle
                    // followed by a flush doesn't dump a backlog at once
                    // — real silicon clocks one byte at a time, not in
                    // bursts.
                    sendBudget_ += dt * bytesPerSecond_;
                    if (sendBudget_ > static_cast<double>(kBufCap))
                        sendBudget_ = static_cast<double>(kBufCap);
                    const size_t take = static_cast<size_t>(sendBudget_);
                    size_t n = 0;
                    while (n < take && !txBuf.empty()) {
                        outChunk.push_back(txBuf.front());
                        txBuf.pop_front();
                        ++n;
                    }
                    sendBudget_ -= static_cast<double>(n);
                } else if (!txBuf.empty()) {
                    outChunk.reserve(txBuf.size());
                    while (!txBuf.empty()) {
                        outChunk.push_back(txBuf.front());
                        txBuf.pop_front();
                    }
                }
                lastDrainTime_ = now;
            }
            if (!outChunk.empty() && clientFd >= 0) {
                const ssize_t sent = ::send(clientFd, outChunk.data(),
                                            outChunk.size(), 0);
                if (sent < 0) {
                    pom2::log().info("SSC",
                        std::string("send error: ") + std::strerror(errno));
                    break;
                }
            }

            // Idle backoff. Tight enough that interactive typing feels
            // instantaneous; slow enough to avoid burning a core.
            ::usleep(2000);
        }
        closeClient();
        onConnectionEdge(false);
    }
}

void SuperSerialCard::deliverRxBytes(const uint8_t* data, size_t n)
{
    if (n == 0) return;
    bool armRxIrq = false;
    bool echoLoopback = false;
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        for (size_t i = 0; i < n; ++i) {
            // MAME `mos6551.cpp:542-543`: SR_RDRF still set on the next
            // byte's arrival → set SR_OVERRUN. We model the same on ring
            // overflow — a host driver that hasn't drained rxBuf loses
            // the oldest unread byte and gets OVERRUN flagged on its
            // next status read. Critical for Kermit-CRC / XMODEM-CRC
            // retransmit logic.
            if (rxBuf.size() >= kBufCap) {
                rxBuf.pop_front();
                statusErrors_ |= SR_OVERRUN;
            }
            rxBuf.push_back(data[i]);
            rxTail.push_back(data[i]);
            if (rxTail.size() > kTailCap) rxTail.pop_front();
        }
        armRxIrq = rxIrqEnable_;
        echoLoopback = echoMode_;
        // MAME `mos6551.cpp:584-594`: REM=1 routes the RX line to TX
        // (unless OVERRUN is set, in which case the line idles high).
        // POM2 doesn't have bit-time accuracy, so the byte-level
        // equivalent is to push each received byte straight into txBuf.
        if (echoLoopback && !(statusErrors_ & SR_OVERRUN)) {
            for (size_t i = 0; i < n; ++i) {
                if (txBuf.size() >= kBufCap) txBuf.pop_front();
                txBuf.push_back(data[i]);
                txTail.push_back(data[i]);
                if (txTail.size() > kTailCap) txTail.pop_front();
                ++txCount;
            }
        }
        if (armRxIrq) raiseIrqSource(IRQ_RDRF);
    }
    rxCount += n;
}

size_t SuperSerialCard::rxQueueDepth() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return rxBuf.size();
}

size_t SuperSerialCard::txQueueDepth() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    return txBuf.size();
}

void SuperSerialCard::onReset()
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    txBuf.clear();
    rxBuf.clear();
    statusErrors_ = 0;
    sendBudget_   = 0.0;
    lastDrainTime_ = std::chrono::steady_clock::now();
    // Full hardware reset (Ctrl-Reset). MAME `mos6551.cpp:117-134`:
    //   write_command(0); write_control(0); m_irq_state = 0;
    // applyCommandReg(0) will lower DTR (→ disable both IRQs), force MARK,
    // and clear any pending IRQ source.
    applyCommandReg(0);
    applyControlReg(0);
    irqState_ = 0;
    pushIrqLine();
}

void SuperSerialCard::onUnplug()
{
    // Drop this slot's contribution to the wire-OR IRQ so the
    // aggregator doesn't keep the bit stuck once the card vanishes.
    irqState_ = 0;
    pushIrqLine();
}

void SuperSerialCard::applyCommandReg(uint8_t v)
{
    // MAME `mos6551.cpp:286-323`. `m_dtr` in MAME tracks the inverted pin
    // (output_dtr(!(cmd&1))), so dtrAsserted_ here = (cmd & 1) — the
    // "device ready, please interrupt me" state.
    cmdReg = v;
    const bool prevDtr = dtrAsserted_;
    dtrAsserted_ = (v & 0x01) != 0;
    rxIrqEnable_ = !((v >> 1) & 1) && dtrAsserted_;
    const int txCtl = (v >> 2) & 0x3;
    const bool txIrqEnable = kTxIrqEnableByCmd[txCtl] && dtrAsserted_;
    (void)txIrqEnable;  // never consulted — TDRE is pinned high
    echoMode_ = (v & 0x10) != 0;

    // Pending-IRQ cleanup when an enable bit goes off, MAME
    // `mos6551.cpp:293-307`.
    if (!rxIrqEnable_ && (irqState_ & IRQ_RDRF)) {
        irqState_ &= ~uint8_t{IRQ_RDRF};
    }
    if (!txIrqEnable && (irqState_ & IRQ_TDRE)) {
        irqState_ &= ~uint8_t{IRQ_TDRE};
    }

    // DTR transitions: MAME `mos6551.cpp:317-321` — when DTR is
    // de-asserted (m_dtr=1, i.e. cmd bit 0 == 0 here), force TX MARK +
    // output_txd(1). For POM2 that means dropping the pending TX buffer
    // and the rate-limit budget — the line is held high, no bytes go
    // out. Going the other way (de-assert → assert) doesn't auto-resume:
    // the driver writes new bytes to TDR which we'll then send.
    if (prevDtr && !dtrAsserted_) {
        txBuf.clear();
        sendBudget_ = 0.0;
    }
    pushIrqLine();
}

void SuperSerialCard::applyControlReg(uint8_t v)
{
    // MAME `mos6551.cpp:271-285`. We don't model rx/tx clock direction
    // (bit 4) since POM2 has no external clock source; word length and
    // extra-stop are stored but not enforced on TX (a TX bit-clip would
    // break the 8-bit-clean policy we already documented in the previous
    // bit-7-strip removal); only the baud-rate divider drives behaviour.
    ctlReg = v;
    baudIndex_       = v & 0x0F;
    wordLength_      = static_cast<uint8_t>(8 - ((v >> 5) & 0x3));
    extraStop_       = (v & 0x80) != 0;
    bytesPerSecond_  = baudIndexToBytesPerSec(baudIndex_);
    sendBudget_      = 0.0;
    lastDrainTime_   = std::chrono::steady_clock::now();
}

void SuperSerialCard::applyProgrammedReset()
{
    // MAME `mos6551.cpp:264-270`. Programmed reset clears OVERRUN +
    // DCD/DSR IRQ sources, then write_command(cmd & ~0x1F) — preserves
    // parity bits 5-7. The previous POM2 code wiped cmdReg entirely,
    // leaving any parity config in undefined state.
    statusErrors_ &= ~uint8_t{SR_OVERRUN};
    irqState_ &= ~uint8_t{IRQ_DCD | IRQ_DSR};
    applyCommandReg(cmdReg & ~uint8_t{0x1F});
}

void SuperSerialCard::onConnectionEdge(bool nowConnected)
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    if (prevConnected_ == nowConnected) return;
    prevConnected_ = nowConnected;
    // MAME `mos6551.cpp:443-461`: a DCD or DSR pin change toggles the
    // matching status bit and raises IRQ_DCD/IRQ_DSR — but only when DTR
    // is asserted. ProTERM, MODEM.MGR and similar carrier-aware drivers
    // arm DTR before watching for IRQ-driven connect/disconnect notices;
    // a plain `IN#2 / PR#2` listing run leaves DTR low and won't be
    // woken by us.
    if (dtrAsserted_) {
        raiseIrqSource(IRQ_DCD | IRQ_DSR);
    }
}

void SuperSerialCard::raiseIrqSource(uint8_t mask)
{
    irqState_ |= mask;
    pushIrqLine();
}

void SuperSerialCard::clearIrqSource(uint8_t mask)
{
    irqState_ &= ~mask;
    pushIrqLine();
}

void SuperSerialCard::pushIrqLine()
{
    // Edge debounce + slot routing live in SlotPeripheral::assertIrq;
    // this method just maps the mask down to a boolean line level.
    assertIrq(irqState_ != 0);
}

uint8_t SuperSerialCard::slotRomRead(uint8_t low8)
{
    return rom[low8];
}

uint8_t SuperSerialCard::deviceSelectRead(uint8_t low4)
{
    // Address decode (MAME `a2ssc.cpp:339-353`):
    //   bit 3 set ($C0n8-$C0nF) → ACIA, with **A0-A1 only** decoded
    //     so $C0nC-$C0nF mirror $C0n8-$C0nB.
    //   bit 3 clear ($C0n0-$C0n7) → 74LS259 DIP-switch reads, with bits
    //     1/0 selecting which DSW to AND-mask: bit 1 clear ⇒ AND DSW1,
    //     bit 0 clear ⇒ AND DSW2 (so $C0n0 returns DSW1 & DSW2,
    //     $C0n1 returns DSW1, $C0n2 returns DSW2, $C0n3 returns 0xFF).
    if (low4 & 0x08) {
        const uint8_t reg = low4 & 0x03;
        switch (reg) {
            case 0x0: {  // RDR (data register)
                std::lock_guard<std::mutex> lk(bufferMtx);
                uint8_t b = 0;
                if (!rxBuf.empty()) {
                    b = rxBuf.front();
                    rxBuf.pop_front();
                }
                // MAME `mos6551.cpp:231-236`: read of RDR clears
                // PARITY_ERROR / FRAMING_ERROR / OVERRUN / RDRF. RDRF
                // here is computed from `rxBuf.empty()` at read time so
                // it clears for free; the sticky error flags need an
                // explicit reset. Also drop IRQ_RDRF so a polled driver
                // that *doesn't* read the status register (rare but
                // legal — the SSC spec lets you arrive directly at RDR
                // once you know a byte is there) still releases the
                // line. Mirror of MAME drop-on-RDR via `update_irq`.
                statusErrors_ &= ~uint8_t{SR_PARITY_ERROR |
                                          SR_FRAMING_ERROR |
                                          SR_OVERRUN};
                clearIrqSource(IRQ_RDRF);
                return b;
            }
            case 0x1: {  // status register
                std::lock_guard<std::mutex> lk(bufferMtx);
                uint8_t s = SR_TDRE;                   // TCP buffers TX
                s |= statusErrors_;
                if (!rxBuf.empty()) s |= SR_RDRF;
                if (connected) s |= (SR_DCD | SR_DSR);
                if (irqState_ != 0) s |= SR_IRQ;
                // MAME `mos6551.cpp:237-250`: status read clears
                // `m_irq_state` and re-evaluates the line. Without this,
                // every IRQ-driven driver (ProTERM, MODEM.MGR, GS/OS
                // SerialPort) spins forever waiting for the ACK.
                clearIrqSource(0xFF);
                return s;
            }
            case 0x2: return cmdReg;
            case 0x3: return ctlReg;
        }
    }
    // DIP-switch readback ($C0n0-$C0n7).
    uint8_t result = 0xFF;
    if (!(low4 & 0x02)) result &= lastDip1;
    if (!(low4 & 0x01)) result &= lastDip2;
    return result;
}

void SuperSerialCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Only $C0n8-$C0nF carries the ACIA registers (A0-A1 mirror); writes
    // to $C0n0-$C0n7 hit the 74LS259 latch which we don't model.
    if (!(low4 & 0x08)) return;
    const uint8_t reg = low4 & 0x03;
    std::lock_guard<std::mutex> lk(bufferMtx);
    switch (reg) {
        case 0x0: {  // TDR (data register) — push to TX queue.
            // Real 6551 transmits all 8 bits up to `m_wordlength`.
            // Previously POM2 stripped bit 7 unconditionally, which
            // broke any 8-bit-clean transfer (XMODEM, YMODEM, ZMODEM,
            // Kermit-binary, BinSCII, ADTPro upload). The bit-7 strip
            // is a *terminal* policy, not a UART one — let the host
            // terminal decide via telnet binary mode if it wants 7-bit.
            //
            // DTR de-asserted (cmd bit 0 == 0) parks the transmitter at
            // MARK in MAME (`mos6551.cpp:317-321`) — drop the byte on
            // the floor rather than queue it for a future re-assert.
            if (!dtrAsserted_) break;
            if (txBuf.size() >= kBufCap) txBuf.pop_front();
            txBuf.push_back(v);
            txTail.push_back(v);
            if (txTail.size() > kTailCap) txTail.pop_front();
            ++txCount;
            break;
        }
        case 0x1: applyProgrammedReset(); break;
        case 0x2: applyCommandReg(v);     break;
        case 0x3: applyControlReg(v);     break;
    }
}

std::string SuperSerialCard::recentTxText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    std::string out;
    out.reserve(txTail.size());
    for (uint8_t b : txTail) {
        const uint8_t c = b & 0x7F;
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) :
                      (c == '\r' || c == '\n') ? '\n' : '.');
    }
    return out;
}

std::string SuperSerialCard::recentRxText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx);
    std::string out;
    out.reserve(rxTail.size());
    for (uint8_t b : rxTail) {
        out.push_back((b >= 0x20 && b < 0x7F) ? static_cast<char>(b) :
                      (b == '\r' || b == '\n') ? '\n' : '.');
    }
    return out;
}

void SuperSerialCard::buildRom()
{
    rom.fill(0xEA);     // NOP padding

    const uint8_t slotHi = static_cast<uint8_t>(0xC0 + slot);
    const uint8_t devLo  = static_cast<uint8_t>(0x80 + slot * 16);
    const uint8_t statusReg = static_cast<uint8_t>(devLo + 0x9);
    const uint8_t dataReg   = static_cast<uint8_t>(devLo + 0x8);

    auto putAt = [&](uint8_t addr, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) rom[addr++] = b;
    };

    // Apple-II PR#n / IN#n auto-config protocol:
    //   $Cn00: JSR'd by `PR#n` to install the output hook in CSWL/CSWH.
    //   $Cn00+1 byte alone is enough; we just JMP to a "real" entry past
    //   the signature bytes so the autodetect bytes ($Cn05, $Cn07,
    //   $Cn0B, $Cn0C) don't get executed.
    //
    // SSC autodetection signature — checked by Apple-PSCAN style firmware
    // and by ProDOS for its character-device probe. These bytes MUST sit
    // at fixed positions, so we route execution around them with a JMP.
    //   $Cn05 = $38   (sig 1)
    //   $Cn07 = $18   (sig 2)
    //   $Cn0B = $01   (firmware revision)
    //   $Cn0C = $31   (device class: serial-port aka "Communications")

    // PR#n entry at $Cn00 — jump past the signature region.
    putAt(0x00, { 0x4C, 0x20, slotHi });   // JMP $Cn20 (PR#n_entry)

    // IN#n entry at $Cn08 (dispatched by the IN# command). Apple BASIC
    // calls $Cn00 for both PR# and IN# but distinguishes by zero-page
    // contents; many ROMs publish IN#n at $Cn00+8. We jump to a separate
    // input-bind routine.
    putAt(0x08, { 0x4C, 0x40, slotHi });   // JMP $Cn40 (IN#n_entry)

    rom[0x05] = 0x38;
    rom[0x07] = 0x18;
    rom[0x0B] = 0x01;
    rom[0x0C] = 0x31;

    // PR#n entry at $Cn20 — patches CSWL/CSWH to point at the output
    // routine at $CnB0, then RTS so the BASIC interpreter resumes.
    putAt(0x20, {
        0xA9, 0xB0,            // LDA #<output_routine
        0x85, 0x36,            // STA $36   (CSWL)
        0xA9, slotHi,          // LDA #>output_routine
        0x85, 0x37,            // STA $37   (CSWH)
        0x60                   // RTS
    });

    // IN#n entry at $Cn40 — patches KSWL/KSWH (input vector).
    putAt(0x40, {
        0xA9, 0xE0,            // LDA #<input_routine
        0x85, 0x38,            // STA $38   (KSWL)
        0xA9, slotHi,          // LDA #>input_routine
        0x85, 0x39,            // STA $39   (KSWH)
        0x60                   // RTS
    });

    // Output routine at $CnB0 — Apple monitor convention: the byte to
    // write is in A (high bit set per the OUT vector spec). PHA once,
    // spin on TDRE (BEQ branches back to the LDA *after* PHA so we
    // don't push extra stack frames each iteration), then PLA + write.
    putAt(0xB0, {
        0x48,                  // PHA               $CnB0
        0xAD, statusReg, 0xC0, // LDA $C0n9         $CnB1  (loop target)
        0x29, 0x10,            // AND #$10  (TDRE)  $CnB4
        0xF0, 0xF9,            // BEQ -7 → $CnB1    $CnB6
        0x68,                  // PLA               $CnB8
        0x8D, dataReg, 0xC0,   // STA $C0n8         $CnB9
        0x60                   // RTS               $CnBC
    });

    // Input routine at $CnE0 — spin until RDRF, return byte in A with
    // bit 7 set (Apple keyboard convention).
    putAt(0xE0, {
        0xAD, statusReg, 0xC0, // LDA $C0n9
        0x29, 0x08,            // AND #$08  (RDRF)
        0xF0, 0xF9,            // BEQ -7 → loop
        0xAD, dataReg, 0xC0,   // LDA $C0n8
        0x09, 0x80,            // ORA #$80  (Apple keys are high-bit-set)
        0x60                   // RTS
    });
}
