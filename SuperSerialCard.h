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
// $Cn0C = $31) and provides a tiny PR#n / IN#n hook that hands character
// I/O off to the device-select range. Boot from $Cs00 isn't supported —
// the SSC was rarely a boot device on real hardware.
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
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

class M6502;

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

    /// Wire the slot IRQ line to the CPU. Safe to leave null (the card
    /// still runs polled). Once set, RDRF transitions (gated by
    /// `cmdReg.bit1 == 0` — RX IRQ enable per the 6551 spec) and DCD/DSR
    /// transitions raise the line; status read or programmed reset
    /// lowers it (matches MAME `mos6551.cpp::read_status` /
    /// `write_reset`). Pattern mirrors `MockingboardCard::setCpuIrqLine`.
    void setCpuIrqLine(M6502* cpu) { cpu_ = cpu; }

    bool isListening()      const { return listening; }
    bool clientConnected()  const { return connected;  }
    uint16_t getPort()      const { return port;       }
    uint64_t bytesRx()      const { return rxCount;    }
    uint64_t bytesTx()      const { return txCount;    }
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

private:
    int slot;
    std::array<uint8_t, 256> rom{};
    std::atomic<bool> listening { false };
    std::atomic<bool> connected { false };
    uint16_t port = kDefaultPort;

    int listenFd = -1;
    int clientFd = -1;
    std::thread worker;
    std::atomic<bool> stopRequested { false };

    // ACIA register state.
    uint8_t cmdReg     = 0x00;
    uint8_t ctlReg     = 0x00;
    // Optional latches the ROM may probe but our model doesn't care about.
    uint8_t lastDip1   = 0xA8;     // 19200 8N1, full duplex
    uint8_t lastDip2   = 0x60;     // CR + LF, no echo, etc.

    // IRQ state — `irqPending_` is the latched "something happened" flag
    // (cleared by status read or programmed reset, MAME-style); the
    // public IRQ line follows `irqAsserted_` so we only call setIRQ on
    // transitions. `cpu_` may be null when the card is plugged headlessly.
    M6502* cpu_         = nullptr;
    bool   irqPending_  = false;
    bool   irqAsserted_ = false;

    /// Latch a new IRQ source (RDRF transition, DCD/DSR change) and push
    /// the line to the CPU if it transitioned. Caller must hold
    /// `bufferMtx` if it touched `rxBuf` to compute the source — but the
    /// CPU asserter call itself does not need the mutex.
    void raiseRxIrq();
    /// Clear `irqPending_` and lower the line. Called from status read
    /// and from programmed reset.
    void clearIrq();
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
