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

#pragma once

// SlotRomBuilder — a bounded hand-assembler for a 256-byte slot ROM page.
//
// WHY THIS EXISTS. Seven POM2 cards hand-assemble their $Cn00 page as raw
// opcode bytes, and every one of them used to do it the same unchecked way:
//
//     for (uint8_t b : bytes) rom[pc++] = b;
//
// `pc` is a uint8_t, so a routine that outgrows its budget does not fail. It
// silently writes over whatever the layout put after it, and — because the
// page is exactly 256 bytes — it wraps to $Cn00 rather than trapping.
//
// That is not hypothetical. On 2026-08-27 the SmartPort card's write-block
// routine was found running from $Cn9C to $CnD5, straight THROUGH the ProDOS
// STATUS routine that the dispatch table's `JMP $CnC0` pointed at. STATUS had
// been dead code for weeks: every ProDOS STATUS call landed mid-instruction
// inside the write loop, executed an illegal opcode and fell into the
// write path's I/O-error branch, so a volume scanner (BITSY, ProDOS ONLINE)
// got $27 "I/O error" from a perfectly healthy bay and could never size the
// device. Nothing failed a test, because nothing executed it.
//
// So a region here declares where it ENDS, and running past that end sets a
// sticky flag the card publishes and a test reads. Two failure modes are
// caught, not one:
//
//   * overflow    — a region grew into its neighbour  (the bug above)
//   * misaligned  — a region no longer ends where the layout says it does,
//                   which matters because these ROMs are full of hand-computed
//                   branch displacements (`BEQ +55` to reach the next routine).
//                   A routine that SHRINKS breaks those just as thoroughly as
//                   one that grows, and leaves no trace in the bytes.
//
// This is a guard, not the cure. The cure is a real mini-assembler with
// symbolic labels and computed branches (TODO P1-1), which makes both failure
// modes unrepresentable instead of merely detected. Until then, every
// hand-assembled region in the repo goes through this class.

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace pom2 {

/// A slot ROM page is one 6502 page: $Cn00-$CnFF.
inline constexpr std::size_t kSlotRomBytes = 256;

class SlotRomBuilder {
public:
    explicit SlotRomBuilder(std::array<uint8_t, kSlotRomBytes>& rom)
        : rom_(rom) {}

    /// Open the region [start, limit). `limit` is EXCLUSIVE: it is the offset
    /// of whatever comes next, so regions can be declared by chaining the
    /// layout's own constants without off-by-one arithmetic.
    SlotRomBuilder& region(unsigned start, unsigned limit)
    {
        pc_    = start;
        limit_ = limit;
        return *this;
    }

    /// Append `bytes` to the open region, stopping at its limit.
    SlotRomBuilder& emit(std::initializer_list<uint8_t> bytes)
    {
        for (uint8_t b : bytes) {
            if (pc_ >= limit_ || pc_ >= kSlotRomBytes) { overflow_ = true; return *this; }
            rom_[pc_++] = b;
        }
        return *this;
    }

    /// Open a region and fill it in one call — the common case for a layout
    /// whose routines sit at fixed addresses rather than chaining.
    SlotRomBuilder& put(unsigned start, unsigned limit,
                        std::initializer_list<uint8_t> bytes)
    {
        return region(start, limit).emit(bytes);
    }

    /// Assert the open region ended exactly at `offset`. Use it wherever
    /// something else hard-codes that address — a branch displacement, a
    /// dispatch table entry, a `JMP` in a sibling routine.
    SlotRomBuilder& expectEnd(unsigned offset)
    {
        if (pc_ != offset) misaligned_ = true;
        return *this;
    }

    /// Offset the next byte would be written to.
    unsigned pc() const { return pc_; }
    /// Bytes still free in the open region.
    unsigned room() const { return pc_ < limit_ ? limit_ - pc_ : 0u; }

    /// Sticky: a region ran past its limit.
    bool overflowed() const { return overflow_; }
    /// Sticky: a region did not end where `expectEnd` said it would.
    bool misaligned() const { return misaligned_; }
    /// Either of the above. This is what a card publishes to its test.
    bool layoutError() const { return overflow_ || misaligned_; }

private:
    std::array<uint8_t, kSlotRomBytes>& rom_;
    unsigned pc_         = 0;
    unsigned limit_      = 0;
    bool     overflow_   = false;
    bool     misaligned_ = false;
};

/// Relative branch displacement from the branch OPCODE at `at` to `target`.
/// The operand follows the opcode, so the PC the CPU adds to is `at + 2`.
constexpr uint8_t slotRomRel(unsigned at, unsigned target)
{
    return static_cast<uint8_t>(target - (at + 2));
}

} // namespace pom2
