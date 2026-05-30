// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SnapshotIO — see SnapshotIO.h for format documentation. Ported from
// POM1's identical implementation; only the magic constant and namespace
// changed.

#include "SnapshotIO.h"

#include <algorithm>
#include <cstring>

namespace pom2 {
namespace {

void writeFixedName(std::ostream& out, std::string_view name)
{
    char buf[kSectionNameLen]{};
    const std::size_t copy = std::min(name.size(), kSectionNameLen);
    std::memcpy(buf, name.data(), copy);
    out.write(buf, kSectionNameLen);
}

std::string readFixedName(std::istream& in)
{
    char buf[kSectionNameLen]{};
    in.read(buf, kSectionNameLen);
    std::size_t len = 0;
    while (len < kSectionNameLen && buf[len] != '\0') ++len;
    return std::string(buf, len);
}

} // namespace

// ─── Writer ───────────────────────────────────────────────────────────────
// Both ctors bind the `out` reference to the live backing stream, then emit
// the shared header. The ofstream/stringstream members are declared before
// `out`, so the reference is always bound to a fully-constructed object.
SnapshotWriter::SnapshotWriter(const std::string& path)
    : fileStream_(path, std::ios::binary | std::ios::trunc)
    , out(fileStream_)
{
    if (!out.good()) return;
    emitHeader();
}

SnapshotWriter::SnapshotWriter(std::vector<uint8_t>& sink)
    : sink_(&sink)
    , out(memStream_)
{
    emitHeader();
}

SnapshotWriter::~SnapshotWriter()
{
    // Memory backend: flush the accumulated bytes into the caller's vector.
    if (sink_) {
        const std::string s = memStream_.str();
        sink_->assign(reinterpret_cast<const uint8_t*>(s.data()),
                      reinterpret_cast<const uint8_t*>(s.data()) + s.size());
    }
}

void SnapshotWriter::emitHeader()
{
    out.write(kSnapshotMagic, sizeof(kSnapshotMagic));
    writeU32(kSnapshotVersion);
    writeU32(0);  // flags reserved
}

void SnapshotWriter::writeU8(uint8_t v) { out.put(static_cast<char>(v)); }
void SnapshotWriter::writeU16(uint16_t v)
{
    char b[2] = { static_cast<char>(v & 0xFF),
                  static_cast<char>((v >> 8) & 0xFF) };
    out.write(b, 2);
}
void SnapshotWriter::writeU32(uint32_t v)
{
    char b[4] = { static_cast<char>(v & 0xFF),
                  static_cast<char>((v >> 8) & 0xFF),
                  static_cast<char>((v >> 16) & 0xFF),
                  static_cast<char>((v >> 24) & 0xFF) };
    out.write(b, 4);
}
void SnapshotWriter::writeU64(uint64_t v)
{
    writeU32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    writeU32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}
void SnapshotWriter::writeBytes(const void* data, std::size_t length)
{
    if (length > 0)
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(length));
}

SnapshotWriter::SectionHandle
SnapshotWriter::beginSection(std::string_view name)
{
    SectionHandle h{};
    writeFixedName(out, name);
    h.lengthSlot = out.tellp();
    writeU32(0);
    h.payloadStart = out.tellp();
    return h;
}

void SnapshotWriter::endSection(SectionHandle h)
{
    const std::streampos endPos = out.tellp();
    const auto length = static_cast<uint32_t>(endPos - h.payloadStart);
    out.seekp(h.lengthSlot);
    writeU32(length);
    out.seekp(endPos);
}

void SnapshotWriter::writeSection(std::string_view name,
                                   const void* data,
                                   std::size_t length)
{
    SectionHandle h = beginSection(name);
    writeBytes(data, length);
    endSection(h);
}

// ─── Reader ───────────────────────────────────────────────────────────────
SnapshotReader::SnapshotReader(const std::string& path)
    : fileStream_(path, std::ios::binary)
    , in(fileStream_)
{
    if (!in.good()) {
        errorMsg = "cannot open snapshot file: " + path;
        return;
    }

    // Record the file size up front so nextSection() can reject a section
    // whose declared length runs past EOF — otherwise a crafted huge length
    // drives an unbounded allocation in the consumer (→ bad_alloc → crash).
    in.seekg(0, std::ios::end);
    fileSize_ = in.tellg();
    in.seekg(0, std::ios::beg);

    parseHeader();
}

SnapshotReader::SnapshotReader(const uint8_t* data, std::size_t length)
    : memStream_(length ? std::string(reinterpret_cast<const char*>(data), length)
                        : std::string(),
                 std::ios::in | std::ios::binary)
    , in(memStream_)
{
    // The whole blob is already resident, so the EOF bound nextSection()
    // enforces is simply its length.
    fileSize_ = static_cast<std::streamoff>(length);
    parseHeader();
}

void SnapshotReader::parseHeader()
{
    char magic[sizeof(kSnapshotMagic)]{};
    in.read(magic, sizeof(kSnapshotMagic));
    if (!in.good() ||
        std::memcmp(magic, kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
        errorMsg = "snapshot magic mismatch (not a POM2 snapshot)";
        return;
    }

    ver = readU32();
    (void)readU32();
    if (ver == 0 || ver > kSnapshotVersion) {
        errorMsg = "unsupported snapshot version " + std::to_string(ver);
        return;
    }
    ok = true;
    cursor     = in.tellg();
    sectionEnd = cursor;
}

uint8_t SnapshotReader::readU8()
{
    char c = 0;
    in.read(&c, 1);
    return static_cast<uint8_t>(c);
}
uint16_t SnapshotReader::readU16()
{
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>(b[0] | (uint16_t(b[1]) << 8));
}
uint32_t SnapshotReader::readU32()
{
    unsigned char b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return static_cast<uint32_t>(b[0])
         | (static_cast<uint32_t>(b[1]) << 8)
         | (static_cast<uint32_t>(b[2]) << 16)
         | (static_cast<uint32_t>(b[3]) << 24);
}
uint64_t SnapshotReader::readU64()
{
    uint64_t lo = readU32();
    uint64_t hi = readU32();
    return lo | (hi << 32);
}
void SnapshotReader::readBytes(void* data, std::size_t length)
{
    if (length > 0)
        in.read(static_cast<char*>(data),
                static_cast<std::streamsize>(length));
}

bool SnapshotReader::nextSection(std::string& name, std::uint32_t& length)
{
    if (!ok) return false;
    if (cursor != sectionEnd) {
        in.seekg(sectionEnd);
        cursor = sectionEnd;
    }
    if (in.peek() == EOF) return false;

    name = readFixedName(in);
    if (!in.good()) return false;
    length = readU32();
    if (!in.good()) return false;

    cursor     = in.tellg();
    sectionEnd = cursor + static_cast<std::streamoff>(length);
    // Reject a section whose payload runs past EOF (a crafted/truncated file).
    // This single bound protects every consumer from an over-read and from an
    // unbounded allocation sized by the file's own length field.
    if (sectionEnd > fileSize_ || sectionEnd < cursor) {
        errorMsg = "snapshot section length exceeds file size";
        ok = false;
        return false;
    }
    return true;
}

void SnapshotReader::skipCurrentSection()
{
    in.seekg(sectionEnd);
    cursor = sectionEnd;
}

} // namespace pom2
