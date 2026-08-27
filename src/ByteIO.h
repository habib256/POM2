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

// ByteIO — tiny little-endian pack/unpack helpers for snapshot blobs.
// One shared home for the LE primitives that card/chip serialization would
// otherwise hand-roll per file. Append-to-vector for writing; a cursor
// Reader for reading (the caller bounds-checks the total length first, then
// reads fields without per-field guards).

#ifndef POM2_BYTE_IO_H
#define POM2_BYTE_IO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {
namespace byteio {

inline void putU8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
inline void putU16(std::vector<uint8_t>& o, uint16_t v)
{
    o.push_back(static_cast<uint8_t>(v & 0xFF));
    o.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
inline void putU32(std::vector<uint8_t>& o, uint32_t v)
{
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
inline void putU64(std::vector<uint8_t>& o, uint64_t v)
{
    for (int i = 0; i < 8; ++i) o.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

// Sequential reader over a fixed byte span. `has(k)` checks before a group
// of reads; the u*() accessors assume the bytes are present.
struct Reader {
    const uint8_t* p;
    std::size_t    n;
    std::size_t    pos = 0;

    Reader(const uint8_t* data, std::size_t len) : p(data), n(len) {}

    bool has(std::size_t k) const { return pos + k <= n; }

    uint8_t  u8()  { return p[pos++]; }
    uint16_t u16() { uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8)); pos += 2; return v; }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[pos++]) << (8 * i); return v; }
    uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[pos++]) << (8 * i); return v; }
};

}  // namespace byteio
}  // namespace pom2

#endif  // POM2_BYTE_IO_H
