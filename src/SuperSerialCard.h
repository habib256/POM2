// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SuperSerialCard — Apple Super Serial Card (slot 2 by convention).
// Models a 6551 ACIA register set wired to a TCP listener so a host
// terminal (telnet, screen, minicom-over-tty) can talk to the running
// Apple II/IIe as a serial peer.
//
// Soft-switch map (slot N at $C080+N*16; for slot 2 → $C0A0-$C0AF):
//
//   $C0nA-$C0nB   DIP-switch reads (we report a sane default config)
//   $C0nC-$C0nD   ACIA reset / status mirror
//   $C0nE         Reserved (reads $00)
//   $C0nF         Reserved (reads $00)
//   $C0n8         ACIA data register (read = pop RX byte; write = push TX)
//   $C0n9         ACIA status register
//                   bit 7 = IRQ                     (we hold low)
//                   bit 6 = DSR                     (1 when client connected)
//                   bit 5 = DCD                     (mirror of DSR)
//                   bit 4 = TDRE: TX register empty (always 1 — TCP buffers it)
//                   bit 3 = RDRF: RX register full  (1 when bytes are queued)
//                   bit 0..2 = framing/parity/overrun (always 0)
//
// Slot ROM ($Cs00-$CsFF, s=2 → $C200-$C2FF) advertises the SSC
// auto-detection signature ($Cn05 = $38, $Cn07 = $18, $Cn0B = $01,
// $Cn0C = $31), the Pascal 1.1 firmware-protocol entry table at $Cn0D-$Cn10
// (PINIT/PREAD/PWRITE/PSTATUS routine offsets), and a tiny PR#n / IN#n hook
// that hands character I/O off to the device-select range. Boot from $Cs00
// isn't supported — the SSC was rarely a boot device on real hardware.
//
// TCP bridge: a worker thread listens on 127.0.0.1:`port` (default 6502)
// and accepts at most one client at a time. Bytes flow through two
// 4 KB ring buffers under a mutex. Newlines are passed through verbatim;
// telnet IAC negotiation is silently dropped so a vanilla `telnet`
// binary connects cleanly without hand-shaking.

#ifndef POM2_SUPER_SERIAL_CARD_H
#define POM2_SUPER_SERIAL_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

class SuperSerialCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 2;
    static constexpr uint16_t kDefaultPort = 6502;

    /// Construct with the slot number this card will be plugged into.
    /// The slot is baked into the slot ROM (PR#n / IN#n trampolines and
    /// the absolute device-select addresses inside the spin-on-TDRE
    /// output / spin-on-RDRF input routines), so changing it after
    /// construction would require rebuilding the ROM — pass the right
    /// slot up front.
    explicit SuperSerialCard(int slot = kDefaultSlot);
    ~SuperSerialCard() override;

    int getSlot() const { return slot; }

    /// Start listening on 127.0.0.1:port. Returns false if the bind fails;
    /// the card stays plugged but `clientConnected()` will always be false.
    bool startListening(uint16_t port);
    /// Tear down the listener and any active connection. Safe to call from
    /// the UI thread; the worker is joined before returning.
    void stopListening();

    /// Optional "telnet → keyboard" bridge. When set, every byte the
    /// SSC receives over TCP is *also* forwarded to this sink (typically
    /// `Memory::queueKey`). That makes the SSC a self-testing console:
    /// the host can telnet in, type `PR#2 / IN#2` directly into BASIC
    /// (no chicken-and-egg of needing IN#N before the keyboard listens
    /// on slot 2), then drive the Apple II via the same telnet socket.
    /// Bytes still land in the ACIA RX queue too, so a real IN#2 path
    /// works once the host has activated it.
    void setKeyboardSink(std::function<void(uint8_t)> sink)
    {
        std::lock_guard<std::mutex> lk(bufferMtx);
        keyboardSink = std::move(sink);
    }

    bool isListening()      const { return listening; }
    bool clientConnected()  const { return connected;  }
    uint16_t getPort()      const { return port;       }
    uint64_t bytesRx()      const { return rxCount;    }
    uint64_t bytesTx()      const { return txCount;    }

    /// Raw-mode toggle: when true, skip telnet IAC ($FF) stripping AND
    /// line-ending normalisation on RX, so 8-bit binary protocols
    /// (XMODEM / Kermit / ADTPro) see every byte verbatim. Default
    /// false (telnet text mode, expected for keyboard / terminal use).
    /// The TCP listener is always raw at the socket level — this flag
    /// only gates POM2's RX-side filtering. Persisted as
    /// `ssc_raw_mode`.
    void setRawMode(bool raw) { rawMode_ = raw; }
    bool rawMode()   const    { return rawMode_; }

    /// Inject bytes as if they had just arrived on the TCP socket. The
    /// path matches the worker thread's: SR_OVERRUN on ring overflow,
    /// RX IRQ raise gated by `rxIrqEnable_`, echo-mode loopback into the
    /// TX queue. Test-only public entry point — production code uses the
    /// worker thread which calls the same method internally.
    void deliverRxBytes(const uint8_t* data, size_t n);

    /// Telnet RX line-ending normaliser (drop NUL, CR LF → CR, LF → CR).
    /// Public + static so it is unit-testable in isolation; the RX worker
    /// calls it on every non-raw inbound chunk. Mutates `data` in place,
    /// returns the new length. RFC 854: a bare CR arrives as CR NUL.
    static size_t normalizeLineEndings(uint8_t* data, size_t n);

    /// Telnet IAC filter — a PERSISTENT state machine (member state) so an
    /// IAC sequence split across recv() chunks, and the variable-length
    /// IAC SB … IAC SE subnegotiation, are handled correctly. Mutates `data`
    /// in place, returns the kept length. Public for unit testing (the RX
    /// worker calls it on every non-raw inbound chunk). Call resetTelnet()
    /// at the start of each connection.
    size_t processTelnetRx(uint8_t* data, size_t n);
    void   resetTelnet() { telnetState_ = TelnetState::Text; }

    // Test/debug introspection — read-only reflection of the decoded
    // command/control register state.
    double  bytesPerSecond() const { return bytesPerSecond_; }
    bool    dtrAsserted()    const { return dtrAsserted_;    }
    bool    echoMode()       const { return echoMode_;       }
    bool    rxIrqEnabled()   const { return rxIrqEnable_;    }
    uint8_t statusErrorBits()const { return statusErrors_;   }
    uint8_t irqState()       const { return irqState_;       }
    size_t  rxQueueDepth()   const;
    size_t  txQueueDepth()   const;
    /// Snapshot of "what the Apple II most recently received" (RX) and
    /// "most recently sent over TCP" (TX). Cheap circular ring of the last
    /// ~256 bytes; used by the status panel as a debug peephole.
    std::string recentTxText() const;     // Apple II → telnet
    std::string recentRxText() const;     // telnet  → Apple II

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Super Serial"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    onReset() override;
    void    onUnplug() override;
    // CPU-thread hook: apply a worker-thread-pending IRQ-line change here so
    // assertIrq() (which mutates non-atomic SlotPeripheral state) is only
    // ever called from the CPU thread. See raiseIrqSource()/pushIrqLine().
    void    advanceCycles(int cycles) override;

private:
    int slot;
    std::array<uint8_t, 256> rom{};
    std::atomic<bool> listening { false };
    std::atomic<bool> connected { false };
    uint16_t port = kDefaultPort;

    // Persistent telnet IAC parser state (worker thread only). Survives
    // recv() chunk boundaries so a split IAC / SB sequence parses correctly.
    enum class TelnetState { Text, Iac, Opt, Sb, SbIac };
    TelnetState telnetState_ = TelnetState::Text;

    // Atomic so the UI/dtor thread can shutdown() these to wake the worker
    // out of accept()/recv() without a torn read, and so close() happens
    // exactly once (the worker is the sole closer of clientFd). See
    // stopListening()/closeClient().
    std::atomic<int> listenFd { -1 };
    std::atomic<int> clientFd { -1 };
    std::thread worker;
    std::atomic<bool> stopRequested { false };

    // 6551 status-register bit layout (MAME `mos6551.h:53-61`).
    static constexpr uint8_t SR_PARITY_ERROR  = 0x01;
    static constexpr uint8_t SR_FRAMING_ERROR = 0x02;
    static constexpr uint8_t SR_OVERRUN       = 0x04;
    static constexpr uint8_t SR_RDRF          = 0x08;
    static constexpr uint8_t SR_TDRE          = 0x10;
    static constexpr uint8_t SR_DCD           = 0x20;
    static constexpr uint8_t SR_DSR           = 0x40;
    static constexpr uint8_t SR_IRQ           = 0x80;

    // 6551 internal IRQ-source mask (MAME `mos6551.h:71-77`). RX IRQ +
    // DCD/DSR change are the only sources POM2 generates; TDRE-on-empty
    // is a no-op here because we pin TDRE high (TCP buffers TX).
    static constexpr uint8_t IRQ_DCD  = 0x01;
    static constexpr uint8_t IRQ_DSR  = 0x02;
    static constexpr uint8_t IRQ_RDRF = 0x04;
    static constexpr uint8_t IRQ_TDRE = 0x08;

    // ACIA register state.
    uint8_t cmdReg     = 0x00;
    uint8_t ctlReg     = 0x00;
    // Optional latches the ROM may probe but our model doesn't care about.
    uint8_t lastDip1   = 0xA8;     // 19200 8N1, full duplex
    uint8_t lastDip2   = 0x60;     // CR + LF, no echo, etc.

    // Decoded command-register state (mirrors MAME `mos6551.cpp::write_command`).
    // dtrAsserted_  := cmd bit 0 == 1 (real DTR pin pulled low = device ready)
    // rxIrqEnable_  := !cmd[1] && dtrAsserted_  — see MAME `mos6551.cpp:292`
    // echoMode_     := cmd bit 4 — see MAME `mos6551.cpp:309`
    bool dtrAsserted_ = false;
    bool rxIrqEnable_ = false;
    bool echoMode_    = false;
    // Raw mode: bypass telnet IAC strip + LF/CR normalisation on RX.
    // For 8-bit binary protocols (XMODEM / Kermit / ADTPro). Persisted
    // as `ssc_raw_mode`. Atomic so the TCP worker thread can read it
    // without holding bufferMtx.
    std::atomic<bool> rawMode_{false};

    // Decoded control-register state. Stored for completeness; only the
    // baud-rate index actually drives behaviour (TX drain pacing).
    uint8_t  wordLength_     = 8;
    bool     extraStop_      = false;
    uint8_t  baudIndex_      = 0;       // ctl[3:0]
    double   bytesPerSecond_ = 0.0;     // 0 → unconstrained (16x ext clk)

    // Persistent status flags (RDRF/TDRE/DCD/DSR computed dynamically at
    // read-time, but OVERRUN/FRAMING/PARITY are sticky — cleared only by
    // read of RDR per MAME `mos6551.cpp:234`).
    uint8_t statusErrors_ = 0;

    // IRQ state (MAME-style mask). The pin level pushed to the bus is
    // simply `irqState_ != 0`; edge debouncing is handled by the base
    // class's `assertIrq()` so we don't cache an extra bool here.
    // Atomic: the TCP worker raises IRQ sources while the CPU thread reads
    // status / clears them. (The CPU IRQ *line* is only ever driven from the
    // CPU thread — see raiseIrqSource/advanceCycles.)
    std::atomic<uint8_t> irqState_{0};
    // Set by the worker when it changes irqState_; the CPU thread's
    // advanceCycles() consumes it to drive assertIrq() on the CPU thread.
    std::atomic<bool> irqLineDirty_{false};

    // Connection-edge tracking for DCD/DSR IRQ generation. Both bits move
    // together in this model (SSC + telnet has no separate carrier-vs-DTR
    // signalling), but we keep two IRQ source bits to mirror MAME.
    bool prevConnected_ = false;

    // TX rate-limit accounting (worker thread). `lastDrainTime_` is the
    // last wall-clock at which we replenished `sendBudget_`. Both reset on
    // every control-reg write so a change of baud rate doesn't dump a
    // backlog at once.
    double sendBudget_ = 0.0;
    std::chrono::steady_clock::time_point lastDrainTime_;

    /// Apply a write to the command register: decode DTR/echo/RX-IRQ,
    /// clear pending RX IRQ when its enable bit goes off (MAME
    /// `mos6551.cpp:293-296`), force TX MARK when DTR de-asserts.
    /// Caller must hold `bufferMtx`.
    void applyCommandReg(uint8_t v);
    /// Apply a write to the control register: decode word length, stop
    /// bits, baud-rate divider, recompute `bytesPerSecond_`, reset the
    /// rate-limit accumulator. Caller must hold `bufferMtx`.
    void applyControlReg(uint8_t v);
    /// Programmed reset (write to status register, MAME
    /// `mos6551.cpp:264-270`): clear OVERRUN, clear DCD/DSR IRQ sources,
    /// then `write_command(cmd & ~0x1F)` (preserve parity bits 5-7).
    /// Caller must hold `bufferMtx`.
    void applyProgrammedReset();
    /// Called from the TCP worker when a client connects or disconnects.
    /// Mirrors MAME's "DCD/DSR pin change → status XOR → IRQ if !DTR"
    /// logic (`mos6551.cpp:443-461`) but driven by connect events rather
    /// than per-bit-clock polling.
    void onConnectionEdge(bool nowConnected);

    /// Latch new IRQ sources and push the line if it transitioned.
    /// Caller must hold `bufferMtx`.
    void raiseIrqSource(uint8_t mask);
    /// Clear specific IRQ sources (e.g. status read clears all, RDR read
    /// clears IRQ_RDRF). Caller must hold `bufferMtx`.
    void clearIrqSource(uint8_t mask);
    /// Apply `irqState_ != 0` to the CPU pin if it differs from the
    /// previously asserted level. Caller must hold `bufferMtx`.
    void pushIrqLine();

    // TX (Apple II → TCP) and RX (TCP → Apple II) ring buffers.
    mutable std::mutex bufferMtx;
    std::deque<uint8_t> txBuf;     // bytes the CPU wrote, awaiting socket send
    std::deque<uint8_t> rxBuf;     // bytes from the socket, awaiting CPU read
    std::atomic<uint64_t> rxCount { 0 };
    std::atomic<uint64_t> txCount { 0 };
    // Recent-bytes peephole (latest ~256 bytes in each direction).
    std::deque<uint8_t> rxTail;
    std::deque<uint8_t> txTail;
    // Optional keyboard injection: each TCP RX byte is also handed to
    // this callback so a telnet session lands characters in BASIC's
    // keyboard latch even when IN#n hasn't been run yet.
    std::function<void(uint8_t)> keyboardSink;

    void buildRom();
    void runWorker();
    void closeClient();
};

#endif // POM2_SUPER_SERIAL_CARD_H
