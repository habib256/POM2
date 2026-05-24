// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SnapshotIO — binary read/write primitives for the POM2 snapshot format.
// Ported from POM1, magic + namespace renamed.
//
// Format (versioned, little-endian throughout — POM2 is host-side LE only):
//
//   "POM2SNAP" (8 bytes)               magic
//   uint32_t  version                  format version (current = 2)
//   uint32_t  flags                    reserved, 0 for now
//
//   Section: 8-byte fixed name (NUL-padded) + uint32_t length + payload
//
// Suggested section roster for Apple II:
//
//   "CPU"      M6502::serialize() — PC, A, X, Y, SP, status, IRQ/NMI flags
//   "MEM"      main 64 KB RAM (restored through writable[] — ROM preserved)
//   "MEX"      (v2) aux RAM + Language-Card RAM + RamWorks banks + paging
//              soft-switches (iieMemMode) + LC latch flags + DisplayState
//   "CASS"     CassetteDevice — loaded path, recorded buffer, position
//   "SLOT0".."SLOT7"   per-slot peripheral payloads (each card decides)
//
// Unknown sections are skipped at load time (forward compat). Cards that
// don't have state to persist write a zero-length section, or simply
// don't appear at all.
//
// No compression, no checksum (yet — could be a v2 sweetener). The file
// is small (~64 KB + slot payloads) and the use case is "snapshot now,
// reload now" rather than archival cold storage.

#ifndef POM2_SNAPSHOT_IO_H
#define POM2_SNAPSHOT_IO_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

namespace pom2 {

inline constexpr char     kSnapshotMagic[8] = {'P','O','M','2','S','N','A','P'};
// v2 adds the "MEX" section (aux RAM, Language-Card RAM, RamWorks banks,
// paging soft-switches, DisplayState) so IIe/IIc state restores fully.
// The reader accepts any version <= kSnapshotVersion, so v1 files still load.
inline constexpr uint32_t kSnapshotVersion  = 2;
inline constexpr std::size_t kSectionNameLen = 8;

class SnapshotWriter
{
public:
    explicit SnapshotWriter(const std::string& path);
    ~SnapshotWriter() = default;

    bool good() const { return out.good(); }

    void writeU8 (uint8_t  v);
    void writeU16(uint16_t v);
    void writeU32(uint32_t v);
    void writeU64(uint64_t v);
    void writeBytes(const void* data, std::size_t length);

    /// Begin a named section. Writes the 8-byte name and a placeholder
    /// length; returns a handle the caller passes back to endSection()
    /// once the payload is written. Sections cannot nest.
    struct SectionHandle {
        std::streampos lengthSlot{};
        std::streampos payloadStart{};
    };
    SectionHandle beginSection(std::string_view name);
    void          endSection(SectionHandle handle);

    /// One-shot helper for sections backed by a contiguous buffer.
    void writeSection(std::string_view name, const void* data, std::size_t length);

private:
    std::ofstream out;
};

class SnapshotReader
{
public:
    explicit SnapshotReader(const std::string& path);
    ~SnapshotReader() = default;

    /// True iff construction parsed a valid POM2 snapshot header AND no
    /// read since has set failbit/badbit. EOF after consuming all
    /// sections is normal — nextSection returns false at EOF.
    bool     good()    const { return ok && !in.fail(); }
    uint32_t version() const { return ver; }
    const std::string& error() const { return errorMsg; }

    uint8_t  readU8();
    uint16_t readU16();
    uint32_t readU32();
    uint64_t readU64();
    void     readBytes(void* data, std::size_t length);

    /// Iterate sections in file order. On success, fills `name` (NUL-
    /// trimmed) and `length`. The caller is then expected to either
    /// readBytes(buf, length) to consume the payload, or call
    /// skipCurrentSection() to move past it.
    bool nextSection(std::string& name, std::uint32_t& length);
    void skipCurrentSection();

    bool atSectionBoundary() const { return cursor == sectionEnd; }

private:
    std::ifstream  in;
    bool           ok = false;
    uint32_t       ver = 0;
    std::string    errorMsg;
    std::streampos cursor{};
    std::streampos sectionEnd{};
};

} // namespace pom2

#endif // POM2_SNAPSHOT_IO_H
