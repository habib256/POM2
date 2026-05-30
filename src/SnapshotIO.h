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
//   "CPU"      16 bytes: PC(2) A X Y status SP cpuMode (6) + cycle count(8).
//              IRQ/NMI lines are NOT persisted — they are transient bus
//              signals re-asserted by the cards on each MMIO access, so they
//              self-correct within a frame of resuming.
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
//
// Both classes have a file backend (the original) AND an in-memory backend:
//   SnapshotWriter(std::vector<uint8_t>&)  — accumulate the blob in RAM
//   SnapshotReader(const uint8_t*, size_t) — parse a blob already in RAM
// The memory backend is what the rewind ring buffer (RewindBuffer) uses to
// serialize/restore machine state at 60 Hz without touching the filesystem.
// The wire format is byte-identical between the two backends.

#ifndef POM2_SNAPSHOT_IO_H
#define POM2_SNAPSHOT_IO_H

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
    /// File-backed: writes the snapshot straight to `path` (truncating any
    /// existing file). `good()` is false if the file could not be opened.
    explicit SnapshotWriter(const std::string& path);
    /// Memory-backed: accumulates the snapshot in `sink`, which is filled
    /// when the writer is destroyed (flush-on-close). `sink` must outlive
    /// the writer. Always `good()`.
    explicit SnapshotWriter(std::vector<uint8_t>& sink);
    ~SnapshotWriter();

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
    void emitHeader();   // magic + version + flags

    std::ofstream         fileStream_;       // engaged for the file ctor
    std::stringstream     memStream_;        // engaged for the memory ctor
    std::vector<uint8_t>* sink_ = nullptr;   // non-null ⟺ memory-backed
    std::ostream&         out;               // bound to whichever is live
};

class SnapshotReader
{
public:
    /// File-backed: opens and parses the snapshot at `path`.
    explicit SnapshotReader(const std::string& path);
    /// Memory-backed: parses a snapshot already resident in RAM. The bytes
    /// are copied in, so `data` need not outlive the reader. `length == 0`
    /// (or `data == nullptr`) yields a non-good reader (no valid header).
    SnapshotReader(const uint8_t* data, std::size_t length);
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
    void parseHeader();   // validate magic/version, prime cursor

    std::ifstream     fileStream_;   // engaged for the file ctor
    std::stringstream memStream_;    // engaged for the memory ctor
    std::istream&  in;               // bound to whichever is live
    bool           ok = false;
    uint32_t       ver = 0;
    std::string    errorMsg;
    std::streampos cursor{};
    std::streampos sectionEnd{};
    std::streamoff fileSize_ = 0;   // total file size; nextSection bounds against it
};

} // namespace pom2

#endif // POM2_SNAPSHOT_IO_H
