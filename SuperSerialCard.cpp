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

}  // namespace

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
                size_t n = swallowTelnetIac(scratch, static_cast<size_t>(got));
                if (n > 0) {
                    // Snapshot the keyboard sink under the same lock that
                    // protects the RX queues, then drop the lock before
                    // calling out — `Memory::queueKey` takes its own
                    // mutex and we MUST NOT hold ours during that.
                    std::function<void(uint8_t)> sink;
                    {
                        std::lock_guard<std::mutex> lk(bufferMtx);
                        for (size_t i = 0; i < n; ++i) {
                            if (rxBuf.size() >= kBufCap) rxBuf.pop_front();
                            rxBuf.push_back(scratch[i]);
                            rxTail.push_back(scratch[i]);
                            if (rxTail.size() > kTailCap) rxTail.pop_front();
                        }
                        sink = keyboardSink;
                    }
                    rxCount += n;
                    // RDRF transition: if bytes just arrived AND the
                    // command register's RX IRQ enable bit allows it
                    // (cmd bit 1 = 0 means RX IRQ enabled, MAME
                    // mos6551.cpp:290-292), latch the IRQ and push to
                    // the CPU. Real 6551 also fires on TDRE rising edge
                    // when cmd bits 2-3 = 01, but POM2 keeps TDRE
                    // pinned at 1 (TCP buffers TX) so that path is
                    // currently a no-op.
                    if (!(cmdReg & 0x02)) raiseRxIrq();
                    if (sink) {
                        // Forward each byte to the Apple II keyboard latch.
                        // Telnet line-endings: a stock telnet client sends
                        // CR LF for ENTER, but Apple II only knows CR. We
                        // strip the LF when it follows a CR.
                        bool prevCR = false;
                        for (size_t i = 0; i < n; ++i) {
                            uint8_t c = scratch[i];
                            if (c == '\n' && prevCR) { prevCR = false; continue; }
                            prevCR = (c == '\r');
                            // Map LF → CR for line-mode clients that don't
                            // send CR. Strip nulls outright (telnet's
                            // standalone LF terminator on some clients).
                            if (c == '\n') c = '\r';
                            if (c == 0)    continue;
                            sink(c);
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

            // Drain TX buffer to socket.
            std::vector<uint8_t> outChunk;
            {
                std::lock_guard<std::mutex> lk(bufferMtx);
                if (!txBuf.empty()) {
                    outChunk.reserve(txBuf.size());
                    while (!txBuf.empty()) {
                        outChunk.push_back(txBuf.front());
                        txBuf.pop_front();
                    }
                }
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
    }
}

void SuperSerialCard::onReset()
{
    cmdReg = 0;
    ctlReg = 0;
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        txBuf.clear();
        rxBuf.clear();
    }
    clearIrq();
}

void SuperSerialCard::raiseRxIrq()
{
    irqPending_ = true;
    pushIrqLine();
}

void SuperSerialCard::clearIrq()
{
    irqPending_ = false;
    pushIrqLine();
}

void SuperSerialCard::pushIrqLine()
{
    if (irqPending_ == irqAsserted_) return;
    irqAsserted_ = irqPending_;
    if (cpu_) cpu_->setIRQ(irqAsserted_ ? 1 : 0);
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
                if (rxBuf.empty()) return 0;
                const uint8_t b = rxBuf.front();
                rxBuf.pop_front();
                return b;
            }
            case 0x1: {  // status register
                uint8_t s = 0x10;     // TDRE — TCP buffers TX
                {
                    std::lock_guard<std::mutex> lk(bufferMtx);
                    if (!rxBuf.empty()) s |= 0x08;     // RDRF
                }
                if (connected) s |= 0x60;              // DCD + DSR
                if (irqPending_) s |= 0x80;            // SR_IRQ
                // MAME `mos6551.cpp:237-250`: status read clears
                // `m_irq_state` and re-evaluates the line. Without this,
                // every IRQ-driven driver (ProTERM, MODEM.MGR, GS/OS
                // SerialPort) spins forever waiting for the ACK.
                clearIrq();
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
    switch (reg) {
        case 0x0: {  // TDR (data register) — push to TX queue.
            // Real 6551 transmits all 8 bits up to `m_wordlength`.
            // Previously POM2 stripped bit 7 unconditionally, which
            // broke any 8-bit-clean transfer (XMODEM, YMODEM, ZMODEM,
            // Kermit-binary, BinSCII, ADTPro upload). The bit-7 strip
            // is a *terminal* policy, not a UART one — let the host
            // terminal decide via telnet binary mode if it wants 7-bit.
            std::lock_guard<std::mutex> lk(bufferMtx);
            if (txBuf.size() >= kBufCap) txBuf.pop_front();
            txBuf.push_back(v);
            txTail.push_back(v);
            if (txTail.size() > kTailCap) txTail.pop_front();
            ++txCount;
            break;
        }
        case 0x1:
            // Programmed reset (write to status). MAME
            // `mos6551.cpp:264-270`: clears OVERRUN, clears
            // IRQ_DCD|IRQ_DSR, then `write_command(m_command & ~0x1f)`
            // — **preserves parity bits 5-7**. The previous
            // `cmdReg = 0` wiped the parity config too, leaving
            // drivers in undefined state after a soft reset.
            cmdReg &= ~uint8_t{0x1F};
            clearIrq();
            break;
        case 0x2: cmdReg = v; break;
        case 0x3: ctlReg = v; break;
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
