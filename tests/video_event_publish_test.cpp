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

// Per-video-frame publication of the beam-racing event log — pins the
// producer/consumer handoff that replaced the per-worker-tick bracket.
//
// Old model: the worker opened the log each CPU tick and the UI *stole* it at
// vsync (takeVideoEvents closed the bracket; every event recorded until the
// next tick was silently dropped). Under PAL (worker 50 Hz, UI 60 Hz) ~1 UI
// render in 6 landed twice inside one tick and saw an EMPTY log → mid-scanline
// effects (French Touch *Mad Effect* / DIX) flickered at the 10 Hz beat.
//
// New model: recording is continuous; Memory::advanceCycles publishes the
// completed {frame-start state, events} at each video-frame boundary
// (65 × 262 NTSC / 65 × 312 PAL cycles) and takeVideoEvents returns a COPY of
// the last published frame — consumable any number of times, nothing dropped.
// The legacy synchronous bracket (beginVideoEventFrame) is kept for the
// beam-racing render tests and pinned here too.

#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_TEXT  = 0xC051;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t SET_PAGE2 = 0xC055;

void advance(Memory& mem, uint64_t cycles)
{
    while (cycles > 0) {
        const int slice = static_cast<int>(std::min<uint64_t>(cycles, 1000));
        mem.advanceCycles(slice);
        cycles -= static_cast<uint64_t>(slice);
    }
}

}  // namespace

int main()
{
    // ── 1. PAL: nothing surfaces before the 20280-cycle boundary; the
    // published frame is a re-consumable copy; the next frame's events do
    // not leak into it. ──────────────────────────────────────────────────
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.setVideoStandard(VideoStandard::PAL);
        const VideoTiming& t = pom2VideoTiming(VideoStandard::PAL);
        const uint64_t frame =
            static_cast<uint64_t>(t.cyclesPerScanline) * t.scanlinesPerFrame;
        assert(frame == 20280);

        advance(mem, 100 * 65);          // beam at line 100
        mem.memRead(SET_PAGE2);          // PAGE2 edge, mid-frame
        assert(mem.takeVideoEvents().empty()
               && "no boundary crossed yet -> nothing published");

        advance(mem, frame - 100 * 65);  // cross into frame 1
        auto evs = mem.takeVideoEvents();
        assert(evs.size() == 1 && evs[0].scanline == 100
               && evs[0].kind == Memory::VideoEventKind::Page2);
        // COPY semantics: a second consumer (the 60 Hz UI re-rendering the
        // same 50 Hz frame) sees the identical log again.
        auto evs2 = mem.takeVideoEvents();
        assert(evs2.size() == 1 && evs2[0].scanline == 100);
        // Frame 0 started from the default state (page 1).
        assert(!mem.getDisplayStateAtFrameStart().page2);

        // An event in frame 1 must NOT leak into the published frame 0...
        mem.memRead(SET_TEXT);
        assert(mem.takeVideoEvents().size() == 1
               && "frame-1 events stay unpublished until its boundary");
        // ...and surfaces after frame 1 completes, with the frame-start
        // snapshot now carrying the PAGE2 flip from frame 0.
        advance(mem, frame);
        auto evs3 = mem.takeVideoEvents();
        assert(evs3.size() == 1
               && evs3[0].kind == Memory::VideoEventKind::TextMode);
        assert(mem.getDisplayStateAtFrameStart().page2);
    }

    // ── 2. NTSC: boundary at 65 × 262 = 17030 (one cycle short → still the
    // recording frame). ──────────────────────────────────────────────────
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        advance(mem, 17029);
        mem.memRead(CLR_TEXT);
        assert(mem.takeVideoEvents().empty());
        advance(mem, 1);
        assert(mem.takeVideoEvents().size() == 1);
    }

    // ── 3. Reset drops both logs — no ghost segments replayed against the
    // wiped soft-switch state. ───────────────────────────────────────────
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.setVideoStandard(VideoStandard::PAL);
        mem.memRead(SET_PAGE2);
        advance(mem, 20280);             // publish frame 0 (1 event)
        assert(!mem.takeVideoEvents().empty());
        mem.resetSoftSwitches();
        assert(mem.takeVideoEvents().empty());
        assert(!mem.getDisplayStateAtFrameStart().page2);
    }

    // ── 4. Legacy synchronous bracket (tests contract) unchanged: take
    // closes the bracket, later events are dropped, second take is empty. ─
    {
        Memory mem;
        mem.setCycleCounter(100 * 65 + 30);
        mem.beginVideoEventFrame();
        mem.memRead(SET_PAGE2);
        auto evs = mem.takeVideoEvents();
        assert(evs.size() == 1 && evs[0].scanline == 100);
        mem.memRead(SET_PAGE1);          // bracket closed → dropped
        assert(mem.takeVideoEvents().empty());
    }

    // ── 5. Boundary-straddling instruction: its event is stamped past the
    // frame boundary (cycleCounter + currentInstructionCycles wraps to
    // scanline ~0) but publication runs after the instruction — the event
    // must be carried into the NEW frame, not published into the closing
    // one (which applied the switch a frame early). ──────────────────────
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        // STA $C055 at $0300 — a 4-cycle absolute store on the PAGE2 switch.
        mem.memWrite(0x0300, 0x8D);
        mem.memWrite(0x0301, 0x55);
        mem.memWrite(0x0302, 0xC0);
        cpu.setProgramCounter(0x0300);
        // Park the beam 2 cycles before the NTSC boundary: the store's
        // event stamp = 17028 + 4 = 17032 ≥ 17030 → belongs to frame 1.
        advance(mem, 17030 - 2);
        cpu.step();
        assert(mem.getCycleCounter() >= 17030
               && "the instruction must have crossed the boundary");
        // Frame 0 was published by the step's advanceCycles — WITHOUT the
        // straddling event.
        assert(mem.takeVideoEvents().empty()
               && "boundary-crossing event must not close into frame 0");
        // It surfaces with frame 1, stamped at the top of the frame.
        advance(mem, 17030);
        auto evs = mem.takeVideoEvents();
        assert(evs.size() == 1
               && evs[0].kind == Memory::VideoEventKind::Page2
               && evs[0].scanline == 0);
    }

    std::printf("video_event_publish OK\n");
    return 0;
}
