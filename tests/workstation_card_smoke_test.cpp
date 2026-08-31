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

// WorkstationCard — the card as it actually plugs, not as a harness.
//
// `scc8530_workstation_firmware` proved the SCC by driving the firmware over
// a shimmed flat bus. This is the same firmware over the card's REAL bus:
// `Memory::ForeignBus`, the card's own RAM, its `$7x00` I/O, its interval
// timer and its `$7C00` ROM banking, paced through `advanceCycles` exactly as
// the slot bus paces it in a running machine. If the map in
// `WorkstationCard.h` is wrong anywhere, this is where it shows.
//
// ROM-gated: SKIPs cleanly when roms/341-0358-A.bin is absent.

#include "Memory.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "WorkstationCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string findRom()
{
    for (const char* p : { "roms/341-0358-A.bin", "../roms/341-0358-A.bin",
                           "../../roms/341-0358-A.bin", "roms/341-0358-a.bin",
                           "../roms/341-0358-a.bin", "../../roms/341-0358-a.bin" }) {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return {};
}

/// Give the card the cycle budget of `frames` PAL-ish frames, in the same
/// ~4096-cycle chunks the CPU worker hands the slot bus.
void runFrames(pom2::WorkstationCard& card, int frames)
{
    for (int f = 0; f < frames; ++f)
        for (int c = 0; c < 5; ++c)
            card.advanceCycles(4096);
}

} // namespace

int main()
{
    const std::string romPath = findRom();
    if (romPath.empty()) {
        std::printf("workstation_card_smoke: SKIP "
                    "(roms/341-0358-A.bin not found)\n");
        return 0;
    }

    // ─── A card with no firmware is inert, and says so ───────────────────
    {
        pom2::WorkstationCard bare(4);
        assert(!bare.romLoaded());
        assert(bare.slotRomRead(0x00) == 0xFF);
        assert(bare.expansionRomRead(0x000) == 0xFF);
        bare.advanceCycles(4096);           // must not crash with no CPU
        std::printf("  ok: a card with no ROM is inert\n");
    }

    pom2::WorkstationCard card(4);
    assert(card.loadRom(romPath));
    assert(card.romLoaded());

    // Reset leaves the card CPU on the dump's own vector, running the upper
    // 32 KiB half.
    assert(card.cardPc() == 0xC000);
    assert(card.romHighHalf());
    std::printf("  ok: cold reset enters $C000 in the high ROM half\n");

    // Collect LLAP frames from the start: the card's node-address
    // acquisition burst happens about 2.6 s in, before anything below runs.
    std::vector<std::vector<uint8_t>> frames;
    card.scc().setFrameCallback([&](int ch, const std::vector<uint8_t>& f) {
        assert(ch == pom2::Scc8530Device::CHAN_A);
        frames.push_back(f);
    });

    // ─── The firmware boots ──────────────────────────────────────────────
    // The self-test finishes well inside 4 s of card time; the far-call
    // trampoline checked further down is written at about 5.3 s, so give it
    // 8 and leave margin either side.
    runFrames(card, 400);

    assert(!card.cardHalted() && "the card CPU executed STP — it ran off the map");
    assert(card.cardPc() != pom2::WorkstationCard::kPostHaltPc &&
           "the firmware halted at its POST error loop");
    assert(card.postErrors() == 0x00 && "the firmware reported a POST failure");
    assert(card.statusCode() != 0x04 && "$7000 holds the POST failure code");
    assert(card.postPassed());
    std::printf("  ok: the card's 65C02 completed the power-on self-test "
                "(PC=$%04X, status code $%02X)\n",
                card.cardPc(), card.statusCode());

    // ─── ...and configures the SCC for LocalTalk ─────────────────────────
    const auto& scc = card.scc();
    const int A = pom2::Scc8530Device::CHAN_A;
    assert((scc.peekWr(A, 4) & 0x30) == 0x20 && "WR4 should select SDLC");
    assert((scc.peekWr(A, 4) & 0xC0) == 0x00 && "SDLC needs the x1 clock");
    assert((scc.peekWr(A, 3) & 0x05) == 0x05 && "receiver + address search on");
    assert((scc.peekWr(A, 11) & 0x60) == 0x60 && "receive clock from the DPLL");
    assert(scc.txRate(A) == 230400 && "should end at the LocalTalk bit rate");
    std::printf("  ok: SCC left in SDLC at %u bit/s\n", scc.txRate(A));

    // ─── ...and then talks LocalTalk ─────────────────────────────────────
    // This is the end of the chain and the part no unit test could fake:
    // Apple's LAP driver, on the card's own CPU, through POM2's SDLC
    // framing, puts real LLAP frames on the wire. `0B 0B 81` is lapENQ —
    // LocalTalk node-address acquisition, where a node proposes an address
    // (here $0B) and asks whether anyone already owns it.
    assert(!frames.empty() && "the card should be transmitting LLAP frames");
    bool sawEnq = false, sawBroadcast = false;
    uint8_t node = 0;
    for (const auto& f : frames) {
        if (f.size() == 3 && f[0] == f[1] && f[2] == 0x81) { sawEnq = true; node = f[0]; }
        if (f.size() >= 3 && f[0] == 0xFF) sawBroadcast = true;
    }
    assert(sawEnq && "no lapENQ: the card is not acquiring a node address");
    assert(node != 0x00 && node != 0xFF && "a LocalTalk node ID is 1..254");
    // The SDLC address filter follows the node it chose, which is how the
    // card will only see frames meant for it.
    assert(scc.peekWr(A, 6) == node);
    assert(sawBroadcast && "the card should also broadcast once it has a node");
    std::printf("  ok: the card acquired LocalTalk node $%02X and sent %zu frames\n",
                node, frames.size());
    card.scc().setFrameCallback(nullptr);

    // ─── ...and publishes both to the Apple II ───────────────────────────
    // "ATLK" at $CnF9 is the card's identity on the bus — it is neither a
    // Pascal 1.1 device nor a ProDOS block device, so software finds it by
    // this and not by $Cn05/$Cn07. `workstation_card_cardcat` is the same
    // fact checked from outside, by real guest software.
    assert(card.signaturePublished() && "the ATLK signature is not on the bus");
    assert(card.slotRomRead(0xF9) == 'A' && card.slotRomRead(0xFA) == 'T' &&
           card.slotRomRead(0xFB) == 'L' && card.slotRomRead(0xFC) == 'K');
    // And the node it acquired is published at $CnF0, matching the SDLC
    // address filter the chip is running.
    assert(card.localTalkNode() == node);
    assert(card.slotRomRead(pom2::WorkstationCard::kPageNodeId) == node);
    std::printf("  ok: publishes \"ATLK\" at $CnF9 and node $%02X at $CnF0\n",
                card.localTalkNode());

    // ─── The ROM banking really banked ───────────────────────────────────
    // The far-call trampoline the firmware relocates to $42D1 writes $7C00,
    // and it is the only way the card reaches the other 32 KiB half. Its
    // presence in card RAM is the direct evidence, and the shape is fixed:
    // PHA / LDA $40BB / STA $7C00 / PLA / JSR $CC32.
    assert(card.cardRam(0x42D1) == 0x48 && "PHA");
    assert(card.cardRam(0x42D2) == 0xAD && "LDA abs");
    assert(card.cardRam(0x42D5) == 0x8D && "STA abs");
    assert(card.cardRam(0x42D6) == 0x00);
    assert(card.cardRam(0x42D7) == 0x7C && "…to $7C00, the bank select");
    std::printf("  ok: the RAM bank-switch trampoline is where the map says\n");

    // ─── The Apple II sees the shared page, both ways ────────────────────
    // $Cn00-$CnFF is card RAM $0200, so the firmware's own driver image is
    // what the host reads — and a host write lands in card RAM.
    bool nonZero = false;
    for (int i = 0; i < 256; ++i)
        if (card.slotRomRead(static_cast<uint8_t>(i)) != 0) { nonZero = true; break; }
    assert(nonZero && "the card should have published its driver page");
    for (int i = 0; i < 0x100; ++i)
        assert(card.slotRomRead(static_cast<uint8_t>(i)) ==
               card.cardRam(static_cast<uint16_t>(pom2::WorkstationCard::kSharedPage + i)));

    card.slotRomWrite(0xE7, 0xC4);          // what the driver does at $C80C
    assert(card.cardRam(pom2::WorkstationCard::kSharedPage + 0xE7) == 0xC4);
    std::printf("  ok: $Cn00 is a read/write window on card RAM $0200\n");

    // ─── The expansion ROM is the dump's own host-side driver ────────────
    // $C800 begins CLD / PHP / SEI / LDA #$50 / STA $C080,X — the sequence
    // that identified this slice as Apple II code in the first place.
    assert(card.expansionRomRead(0x000) == 0xD8);   // CLD
    assert(card.expansionRomRead(0x001) == 0x08);   // PHP
    assert(card.expansionRomRead(0x002) == 0x78);   // SEI
    assert(card.expansionRomRead(0x005) == 0x9D);   // STA abs,X
    assert(card.expansionRomRead(0x007) == 0xC0);   // …$C080,X
    std::printf("  ok: $C800-$CFFF serves the host-side driver\n");

    // ─── The host strobes are recorded, not invented ─────────────────────
    assert(card.hostStrobeLog().empty());
    assert(card.deviceSelectRead(0x0) == 0xFF);
    card.deviceSelectWrite(0x0, 0x71);
    assert(card.hostStrobeLog().size() == 2);
    assert(card.hostStrobeLog()[1].reg == 0x0 &&
           card.hostStrobeLog()[1].value == 0x71 &&
           card.hostStrobeLog()[1].write);
    std::printf("  ok: $C0nX accesses are logged (semantics still open)\n");

    // ─── Snapshot round-trip ─────────────────────────────────────────────
    std::vector<uint8_t> blob;
    card.appendSnapshotState(blob);
    assert(!blob.empty());
    const uint16_t pcBefore  = card.cardPc();
    const uint8_t  ramBefore = card.cardRam(0x0200);

    const uint8_t wr4Before  = card.scc().peekWr(A, 4);
    const uint32_t rateBefore = card.scc().txRate(A);

    runFrames(card, 20);                    // move it on
    card.scc().reset();                     // and scramble the chip
    assert(card.scc().txRate(A) != rateBefore);

    card.loadSnapshotState(blob.data(), blob.size());
    assert(card.cardPc() == pcBefore);
    assert(card.cardRam(0x0200) == ramBefore);
    // The SCC's register file rides along, so a rewind does not land the
    // chip on its reset values and wait for the firmware to notice.
    assert(card.scc().peekWr(A, 4) == wr4Before);
    assert(card.scc().txRate(A) == rateBefore);

    // A foreign blob must be ignored, not trusted: a slot can hold a
    // different card than it did when the snapshot was taken.
    std::vector<uint8_t> foreign(blob.size(), 0x5A);
    card.loadSnapshotState(foreign.data(), foreign.size());
    assert(card.cardPc() == pcBefore);
    card.loadSnapshotState(blob.data(), 8);         // truncated
    assert(card.cardPc() == pcBefore);
    std::printf("  ok: snapshot round-trips and rejects foreign blobs\n");

    // ─── It keeps running after the reset the slot bus can send ──────────
    card.onReset();
    assert(card.cardPc() == 0xC000);
    assert(card.romHighHalf());
    assert(card.cardRam(0x42D1) == 0x00 && "reset should clear card RAM");
    runFrames(card, 400);
    assert(card.postErrors() == 0x00 && "the firmware should re-pass its POST");
    assert(card.scc().txRate(A) == 230400);
    std::printf("  ok: reset re-boots the card cleanly\n");

    // ─── Plugged into a real slot bus, seen from the Apple II ────────────
    // Everything above drove the card directly. This is the path a guest
    // takes: Memory's $Cn00 decode, the slot bus, the card. It is also the
    // only place `advanceCycles` reaches the card the way the CPU worker
    // sends it, so a wiring mistake in the bus shows up here and nowhere
    // else.
    {
        Memory mem;
        auto plugged = std::make_unique<pom2::WorkstationCard>(4);
        assert(plugged->loadRom(romPath));
        auto* live = plugged.get();
        mem.slotBus().plug(4, std::move(plugged));
        assert(mem.slotBus().hasActiveCards());

        // 8 s of card time, delivered the way Memory::advanceCycles does.
        for (int i = 0; i < 2000; ++i) mem.advanceCycles(4096);

        assert(live->postErrors() == 0x00);
        assert(live->scc().txRate(pom2::Scc8530Device::CHAN_A) == 230400);

        // The guest reads the card's driver page at $C400 for slot 4, and
        // it is the shared RAM page the card published.
        bool sawCode = false;
        for (int i = 0; i < 256; ++i) {
            const uint8_t viaBus = mem.memRead(static_cast<uint16_t>(0xC400 + i));
            assert(viaBus == live->cardRam(
                       static_cast<uint16_t>(pom2::WorkstationCard::kSharedPage + i)));
            if (viaBus != 0x00 && viaBus != 0xFF) sawCode = true;
        }
        assert(sawCode && "the $Cn00 window should carry the card's driver");
        std::printf("  ok: plugged in slot 4, the guest reads the shared page "
                    "through Memory\n");
    }

    std::printf("OK workstation_card_smoke\n");
    return 0;
}
