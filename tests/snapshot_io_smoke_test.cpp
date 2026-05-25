// Round-trip smoke test for POM2's SnapshotIO. Writes a synthetic file
// with two named sections, re-opens it, verifies header + payload bytes.

#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
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

    // ── Malformed-file hardening ───────────────────────────────────────────
    // A crafted snapshot whose section length field runs past EOF must be
    // rejected by nextSection() rather than handed to a consumer that would
    // size an allocation (or an over-read) from the attacker-controlled
    // length. Pins the fileSize_ bound in SnapshotReader::nextSection.
    auto writeRaw = [](const fs::path& p, const std::vector<uint8_t>& bytes) {
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        assert(f);
        if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    };
    // Valid 16-byte header: magic + version 2 (LE) + flags 0.
    const std::vector<uint8_t> header = {
        'P','O','M','2','S','N','A','P',
        0x02,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00
    };
    auto withSection = [&](std::vector<uint8_t> name8, uint32_t length,
                           size_t payloadBytes) {
        std::vector<uint8_t> f = header;
        name8.resize(8, 0);
        f.insert(f.end(), name8.begin(), name8.end());
        f.push_back(length & 0xFF);
        f.push_back((length >> 8) & 0xFF);
        f.push_back((length >> 16) & 0xFF);
        f.push_back((length >> 24) & 0xFF);
        f.insert(f.end(), payloadBytes, 0xCD);  // actual payload (may be short)
        return f;
    };

    const fs::path bad = fs::temp_directory_path() / "pom2_snapshot_bad.snap";

    // (a) Huge length (0xFFFFFFFF), zero payload → would be a 4 GB allocation.
    writeRaw(bad, withSection({'E','V','I','L'}, 0xFFFFFFFFu, 0));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());                       // header is valid…
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));           // …but the section is rejected.
        assert(!br.good());                      // reader latched into error.
        assert(br.error().find("exceeds file size") != std::string::npos);
    }

    // (b) Length declares 100 bytes but only 10 are present (truncated payload).
    writeRaw(bad, withSection({'B','I','G'}, 100, 10));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));
    }

    // (c) Header + name but the length field itself is truncated (2 of 4 bytes).
    {
        std::vector<uint8_t> f = header;
        const std::vector<uint8_t> nm = {'T','R','U','N','C',0,0,0};
        f.insert(f.end(), nm.begin(), nm.end());
        f.push_back(0x10); f.push_back(0x00);    // only 2 bytes of the u32 length
        writeRaw(bad, f);
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(!br.nextSection(n, l));
    }

    // (d) A length that exactly fills the file must still be accepted (the
    //     bound is "exceeds", not "equals" — guard against an off-by-one).
    writeRaw(bad, withSection({'F','I','T'}, 16, 16));
    {
        pom2::SnapshotReader br(bad.string());
        assert(br.good());
        std::string n; uint32_t l = 0;
        assert(br.nextSection(n, l));
        assert(n == "FIT" && l == 16);
        assert(!br.nextSection(n, l));           // and then clean EOF.
        assert(br.good());                       // EOF is not an error.
    }

    fs::remove(bad);

    // Cleanup.
    fs::remove(tmp);
    std::printf("SnapshotIO smoke: OK (round-trip + malformed-file hardening)\n");
    return 0;
}
