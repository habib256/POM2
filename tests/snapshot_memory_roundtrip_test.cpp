// Round-trip + file-parity test for SnapshotIO's in-memory backend — the
// storage primitive the rewind ring buffer is built on (Phase 0).
//
// Verifies that:
//   • SnapshotWriter(std::vector&) produces byte-identical output to the
//     file writer (the wire format must not depend on the backend), and
//   • SnapshotReader(ptr,len) reads it back faithfully, including a clean
//     EOF and a graceful (non-good) result on an empty blob.

#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main()
{
    namespace fs = std::filesystem;

    const std::vector<uint8_t> cpu = { 0x12, 0x34, 0x56, 0x78 };
    std::vector<uint8_t> mem(8192);
    for (size_t i = 0; i < mem.size(); ++i)
        mem[i] = static_cast<uint8_t>(i * 7 + 1);   // non-trivial, includes 0x00/0xFF

    // ── Write to memory ───────────────────────────────────────────────────
    std::vector<uint8_t> blob;
    {
        pom2::SnapshotWriter w(blob);
        assert(w.good());
        w.writeSection("CPU", cpu.data(), cpu.size());
        w.writeSection("MEM", mem.data(), mem.size());
    }                                   // dtor flushes into `blob`
    assert(!blob.empty());

    // ── Write the same content to a file, compare byte-for-byte ───────────
    const fs::path tmp = fs::temp_directory_path() / "pom2_snapshot_mem_parity.snap";
    {
        pom2::SnapshotWriter w(tmp.string());
        assert(w.good());
        w.writeSection("CPU", cpu.data(), cpu.size());
        w.writeSection("MEM", mem.data(), mem.size());
    }
    std::vector<uint8_t> fileBytes;
    {
        std::ifstream f(tmp.string(), std::ios::binary);
        assert(f.good());
        fileBytes.assign(std::istreambuf_iterator<char>(f),
                         std::istreambuf_iterator<char>());
    }
    assert(blob.size() == fileBytes.size());
    assert(std::memcmp(blob.data(), fileBytes.data(), blob.size()) == 0);

    // ── Read the in-memory blob back ──────────────────────────────────────
    pom2::SnapshotReader r(blob.data(), blob.size());
    assert(r.good());
    assert(r.version() == pom2::kSnapshotVersion);

    std::string name;
    uint32_t    len = 0;

    assert(r.nextSection(name, len));
    assert(name == "CPU" && len == cpu.size());
    std::vector<uint8_t> gotCpu(len);
    r.readBytes(gotCpu.data(), len);
    assert(std::memcmp(gotCpu.data(), cpu.data(), len) == 0);

    assert(r.nextSection(name, len));
    assert(name == "MEM" && len == mem.size());
    std::vector<uint8_t> gotMem(len);
    r.readBytes(gotMem.data(), len);
    assert(std::memcmp(gotMem.data(), mem.data(), len) == 0);

    assert(!r.nextSection(name, len));      // clean EOF
    assert(r.good());                       // EOF is not an error

    // ── Empty blob must not be mistaken for a valid snapshot ──────────────
    {
        pom2::SnapshotReader er(nullptr, 0);
        assert(!er.good());                 // no header → not a POM2 snapshot
    }

    fs::remove(tmp);
    std::printf("SnapshotIO memory backend: OK (round-trip + file parity)\n");
    return 0;
}
