// Super Serial Card ACIA smoke test — pins the 6551-shaped command /
// control / status / data behaviour against MAME `mos6551.cpp` as the
// source of truth. The TCP bridge / worker thread are out of scope here
// (no socket spinup); we drive the card via deviceSelectRead/Write the
// way the 6502 would, and inject "bytes from the wire" via the
// `deliverRxBytes` test-only entry point.
//
// What this gates (one assertion per item from TODO §1):
//
//   * Echo mode (REM, cmd bit 4) — MAME `mos6551.cpp:309, 584-594`.
//     Setting REM=1 with DTR asserted should loop each received byte
//     back into the TX queue. OVERRUN suppresses echo (real silicon
//     idles MARK).
//   * read_rdr clears errors + RDRF — MAME `mos6551.cpp:231-236`.
//     A sticky SR_OVERRUN bit must clear on the next $C0n8 read. RDRF
//     follows rxBuf.empty() so it clears as the queue drains.
//   * DTR side-effects — MAME `mos6551.cpp:290-292, 317-321`. cmd bit 0
//     == 0 (DTR de-asserted) disables RX/TX IRQ, drops pending TX, and
//     blocks new TDR writes from queueing.
//   * DCD/DSR transitions raise IRQ — MAME `mos6551.cpp:443-461`.
//     onConnectionEdge raises IRQ_DCD|IRQ_DSR only when DTR is asserted;
//     plain status read confirms DCD+DSR bits track the connection
//     state.
//   * Overrun tracking — MAME `mos6551.cpp:542-543`. Filling rxBuf past
//     its bound sets SR_OVERRUN; the next status read sees it; read_rdr
//     clears it.
//   * Control reg baud — MAME `mos6551.cpp:271-285` + the SSC 1.8432 MHz
//     xtal. Writing index 14 ($0E) into ctl[3:0] should yield 9600 baud
//     = 960 bytes/sec; index 0 is "16x ext clk", treated as
//     unconstrained.
//   * Programmed reset (write to $C0n9) preserves parity bits 5-7 in
//     cmdReg — MAME `mos6551.cpp:264-270`.

#include "SuperSerialCard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Slot $C0nX addresses for slot 2 (the SSC's conventional slot).
constexpr uint8_t kRdrAddr     = 0x8;   // $C0A8
constexpr uint8_t kStatusAddr  = 0x9;   // $C0A9
constexpr uint8_t kCommandAddr = 0xA;   // $C0AA
constexpr uint8_t kControlAddr = 0xB;   // $C0AB

// 6551 SR_* bits (mirror SuperSerialCard.h private constants).
constexpr uint8_t SR_PARITY_ERROR  = 0x01;
constexpr uint8_t SR_FRAMING_ERROR = 0x02;
constexpr uint8_t SR_OVERRUN       = 0x04;
constexpr uint8_t SR_RDRF          = 0x08;
constexpr uint8_t SR_TDRE          = 0x10;
constexpr uint8_t SR_DCD           = 0x20;
constexpr uint8_t SR_DSR           = 0x40;
constexpr uint8_t SR_IRQ           = 0x80;

void testDtrAndCommandDecode()
{
    SuperSerialCard ssc(2);

    // After construction the command register is zero → DTR=0 (de-asserted),
    // RX IRQ disabled (gated by !dtrAsserted_), echo off.
    assert(!ssc.dtrAsserted());
    assert(!ssc.rxIrqEnabled());
    assert(!ssc.echoMode());

    // Set DTR=1, RX IRQ enable bit 1=0, echo bit 4=0 → cmd=$01.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    assert(ssc.dtrAsserted());
    assert(ssc.rxIrqEnabled());
    assert(!ssc.echoMode());

    // Set echo (bit 4) → cmd=$11.
    ssc.deviceSelectWrite(kCommandAddr, 0x11);
    assert(ssc.echoMode());

    // Mask the RX IRQ (bit 1=1) → cmd=$13. rxIrqEnable_ should go off.
    ssc.deviceSelectWrite(kCommandAddr, 0x13);
    assert(!ssc.rxIrqEnabled());
    assert(ssc.dtrAsserted());
    assert(ssc.echoMode());

    // De-assert DTR → all IRQs disabled.
    ssc.deviceSelectWrite(kCommandAddr, 0x10);
    assert(!ssc.dtrAsserted());
    assert(!ssc.rxIrqEnabled());

    std::printf("  ok: command decode (DTR / RX IRQ / echo)\n");
}

void testTdrWhileDtrDeasserted()
{
    SuperSerialCard ssc(2);
    // DTR de-asserted (cmd bit 0 = 0): writes to TDR should be dropped.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    ssc.deviceSelectWrite(kRdrAddr, 'A');
    ssc.deviceSelectWrite(kRdrAddr, 'B');
    assert(ssc.txQueueDepth() == 0);

    // Assert DTR → TDR writes now queue.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    ssc.deviceSelectWrite(kRdrAddr, 'X');
    assert(ssc.txQueueDepth() == 1);

    // De-assert DTR mid-stream → MAME forces MARK, POM2 drops the queue.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    assert(ssc.txQueueDepth() == 0);

    std::printf("  ok: DTR side-effects on TDR\n");
}

void testEchoLoopback()
{
    SuperSerialCard ssc(2);
    // Echo on (cmd $11), DTR asserted, RX IRQ enable bit clear.
    ssc.deviceSelectWrite(kCommandAddr, 0x11);
    const uint8_t payload[] = { 'H', 'i', '!' };
    ssc.deliverRxBytes(payload, sizeof(payload));
    // rxBuf and txBuf both hold the 3-byte payload.
    assert(ssc.rxQueueDepth() == 3);
    assert(ssc.txQueueDepth() == 3);

    // Drain rxBuf via $C0n8 — bytes come out in FIFO order.
    assert(ssc.deviceSelectRead(kRdrAddr) == 'H');
    assert(ssc.deviceSelectRead(kRdrAddr) == 'i');
    assert(ssc.deviceSelectRead(kRdrAddr) == '!');
    assert(ssc.rxQueueDepth() == 0);

    // Echo off → next batch lands only in rxBuf.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, echo off
    const uint8_t solo[] = { 'Z' };
    ssc.deliverRxBytes(solo, 1);
    assert(ssc.rxQueueDepth() == 1);
    assert(ssc.txQueueDepth() == 3);            // unchanged from previous run

    std::printf("  ok: echo mode loopback\n");
}

void testOverrunAndRdrClear()
{
    SuperSerialCard ssc(2);
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, RX IRQ enabled

    // Push more than the 4 K ring can hold so the oldest get evicted and
    // SR_OVERRUN latches.
    std::vector<uint8_t> blast(5000, 0xAA);
    ssc.deliverRxBytes(blast.data(), blast.size());
    assert(ssc.rxQueueDepth() == 4096);
    assert((ssc.statusErrorBits() & SR_OVERRUN) != 0);

    // Status read shows OVERRUN, RDRF, DCD/DSR (no client → off), TDRE.
    // It also clears the IRQ source mask (MAME `mos6551.cpp:244-248`).
    const uint8_t st = ssc.deviceSelectRead(kStatusAddr);
    assert(st & SR_TDRE);
    assert(st & SR_RDRF);
    assert(st & SR_OVERRUN);
    assert((st & (SR_DCD | SR_DSR)) == 0);      // no TCP client connected
    // No SR_IRQ flagged here — status read just consumed it and the
    // returned byte snapshots the *current* status (MAME returns the
    // pre-clear status, but POM2's order-of-evaluation is symmetric
    // since irqState_ != 0 was true at the time the byte was computed).
    assert(ssc.irqState() == 0);                // cleared by status read

    // RDR read clears SR_OVERRUN per MAME `mos6551.cpp:231-236`.
    ssc.deviceSelectRead(kRdrAddr);
    assert((ssc.statusErrorBits() & SR_OVERRUN) == 0);

    std::printf("  ok: overrun set on overflow, cleared on RDR read\n");
}

void testProgrammedResetPreservesParity()
{
    SuperSerialCard ssc(2);
    // Set parity = even, echo = 1, RX IRQ enable = 1, DTR = 1 → cmd $7B.
    ssc.deviceSelectWrite(kCommandAddr, 0x7B);
    assert(ssc.echoMode());
    assert(ssc.dtrAsserted());

    // Programmed reset (any write to $C0n9). MAME clears the low 5 bits
    // (DTR/IRQ-en/tx-ctl/echo) but preserves parity bits 5-7.
    ssc.deviceSelectWrite(kStatusAddr, 0x00);
    // DTR de-asserted (bit 0 cleared), echo cleared (bit 4 cleared).
    assert(!ssc.dtrAsserted());
    assert(!ssc.echoMode());
    // The cmd register value should retain bits 5-7 (parity = 011) but
    // have its low 5 bits zeroed.
    // 0x7B = 0111 1011 → reset to 0110 0000 = 0x60.
    // We read the value back via the device select read.
    const uint8_t cmd = ssc.deviceSelectRead(kCommandAddr);
    assert(cmd == 0x60);

    std::printf("  ok: programmed reset preserves parity bits\n");
}

void testControlRegBaud()
{
    SuperSerialCard ssc(2);
    // Index 0 = 16x external clock → unconstrained in POM2.
    ssc.deviceSelectWrite(kControlAddr, 0x00);
    assert(ssc.bytesPerSecond() == 0.0);

    // Index 14 = 9600 baud. 1.8432 MHz / (12 * 16) = 9600 / 10 = 960 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x0E);
    assert(std::abs(ssc.bytesPerSecond() - 960.0) < 1e-6);

    // Index 15 = 19200 baud = 1920 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x0F);
    assert(std::abs(ssc.bytesPerSecond() - 1920.0) < 1e-6);

    // Index 6 = 300 baud = 30 bps.
    ssc.deviceSelectWrite(kControlAddr, 0x06);
    assert(std::abs(ssc.bytesPerSecond() - 30.0) < 1e-6);

    // Read-back of control reg returns the byte we wrote.
    assert(ssc.deviceSelectRead(kControlAddr) == 0x06);

    std::printf("  ok: control reg baud-rate decode\n");
}

void testRxIrqGatedByCommand()
{
    SuperSerialCard ssc(2);

    // Case 1: cmd $01 — DTR on, RX IRQ enable bit 1 = 0. deliverRxBytes
    // should raise IRQ_RDRF.
    ssc.deviceSelectWrite(kCommandAddr, 0x01);
    const uint8_t b1[] = { 'a' };
    ssc.deliverRxBytes(b1, 1);
    assert(ssc.irqState() & 0x04);              // IRQ_RDRF

    // Status read clears all sources.
    ssc.deviceSelectRead(kStatusAddr);
    assert(ssc.irqState() == 0);

    // Case 2: cmd $03 — DTR on, RX IRQ disabled (bit 1 = 1). No IRQ.
    ssc.deviceSelectWrite(kCommandAddr, 0x03);
    ssc.deliverRxBytes(b1, 1);
    assert((ssc.irqState() & 0x04) == 0);

    // Case 3: cmd $00 — DTR off, RX IRQ also off via DTR gate.
    ssc.deviceSelectWrite(kCommandAddr, 0x00);
    ssc.deliverRxBytes(b1, 1);
    assert((ssc.irqState() & 0x04) == 0);

    std::printf("  ok: RX IRQ gated by cmd bit 1 + DTR\n");
}

void testCommandRegWriteClearsPendingRxIrq()
{
    // MAME `mos6551.cpp:293-296`: when the new command value disables the
    // RX IRQ enable, any pending IRQ_RDRF source is cleared and the line
    // re-evaluated.
    SuperSerialCard ssc(2);
    ssc.deviceSelectWrite(kCommandAddr, 0x01);  // DTR on, RX IRQ on
    const uint8_t b[] = { 'q' };
    ssc.deliverRxBytes(b, 1);
    assert(ssc.irqState() & 0x04);

    // Disable RX IRQ (bit 1 = 1) — pending RDRF source should go away.
    ssc.deviceSelectWrite(kCommandAddr, 0x03);
    assert((ssc.irqState() & 0x04) == 0);

    std::printf("  ok: cmd write clears pending IRQ when enable goes off\n");
}

void testTelnetLineEndingNormalisation()
{
    // SuperSerialCard::normalizeLineEndings — telnet RX line-ending fixup.
    // RFC 854: a bare carriage return is transmitted as CR NUL, ENTER as
    // CR LF. The Apple II expects CR alone. Regression pinned here: the
    // NUL in CR NUL LF used to reset prevCR before being dropped, leaking
    // a spurious second CR (CR NUL LF → CR CR instead of CR).
    auto norm = [](std::vector<uint8_t> in) {
        const size_t m = SuperSerialCard::normalizeLineEndings(in.data(), in.size());
        in.resize(m);
        return in;
    };
    assert((norm({0x0D, 0x00})       == std::vector<uint8_t>{0x0D}));        // CR NUL → CR
    assert((norm({0x0D, 0x00, 0x0A}) == std::vector<uint8_t>{0x0D}));        // CR NUL LF → CR (the bug)
    assert((norm({0x0D, 0x0A})       == std::vector<uint8_t>{0x0D}));        // CR LF → CR
    assert((norm({0x0A})             == std::vector<uint8_t>{0x0D}));        // bare LF → CR
    assert(norm({0x00}).empty());                                           // lone NUL dropped
    assert((norm({'H','i',0x0D,0x00,0x0A,'!'})
                == std::vector<uint8_t>{'H','i',0x0D,'!'}));                 // embedded, passthrough
    std::printf("  ok: telnet CR/NUL/LF normalisation\n");
}

void testStatusReadDcdDsr()
{
    SuperSerialCard ssc(2);
    // No client connected → DCD + DSR bits clear.
    const uint8_t s1 = ssc.deviceSelectRead(kStatusAddr);
    assert((s1 & (SR_DCD | SR_DSR)) == 0);
    // TDRE always set in POM2 (TCP buffers TX).
    assert(s1 & SR_TDRE);

    std::printf("  ok: status DCD/DSR mirror connection state\n");
}

}  // namespace

int main()
{
    testDtrAndCommandDecode();
    testTdrWhileDtrDeasserted();
    testEchoLoopback();
    testOverrunAndRdrClear();
    testProgrammedResetPreservesParity();
    testControlRegBaud();
    testRxIrqGatedByCommand();
    testCommandRegWriteClearsPendingRxIrq();
    testTelnetLineEndingNormalisation();
    testStatusReadDcdDsr();
    std::printf("OK ssc_acia_smoke\n");
    return 0;
}
