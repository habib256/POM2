// Round-trip smoke test for POM2's SnapshotIO. Writes a synthetic file
// with two named sections, re-opens it, verifies header + payload bytes.

#include "SnapshotIO.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

int main()
{
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "pom2_snapshot_smoke.snap";

    const std::vector<uint8_t> cpuPayload = { 0x12, 0x34, 0x56, 0x78 };
    const std::vector<uint8_t> memPayload(8192, 0xAA);

    // Write.
    {
        pom2::SnapshotWriter w(tmp.string());
        assert(w.good());
        w.writeSection("CPU", cpuPayload.data(), cpuPayload.size());
        w.writeSection("MEM", memPayload.data(), memPayload.size());
    }

    // Read.
    pom2::SnapshotReader r(tmp.string());
    assert(r.good());
    assert(r.version() == pom2::kSnapshotVersion);

    std::string name;
    uint32_t    len = 0;

    assert(r.nextSection(name, len));
    assert(name == "CPU");
    assert(len == cpuPayload.size());
    std::vector<uint8_t> got(len);
    r.readBytes(got.data(), len);
    assert(std::memcmp(got.data(), cpuPayload.data(), len) == 0);

    assert(r.nextSection(name, len));
    assert(name == "MEM");
    assert(len == memPayload.size());
    std::vector<uint8_t> got2(len);
    r.readBytes(got2.data(), len);
    assert(std::memcmp(got2.data(), memPayload.data(), len) == 0);

    // No more sections.
    assert(!r.nextSection(name, len));

    // Cleanup.
    fs::remove(tmp);
    std::printf("SnapshotIO smoke: OK\n");
    return 0;
}
