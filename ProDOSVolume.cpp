// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ProDOSVolume.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

constexpr std::size_t kBlockBytes        = 512;
constexpr std::size_t kVolDirEntriesK0   = 12;       // entries in block 2 (after vol header)
constexpr std::size_t kVolDirEntriesKN   = 13;       // entries in blocks 3, 4, 5
constexpr std::size_t kVolDirTotalSlots  = kVolDirEntriesK0 + 3 * kVolDirEntriesKN;  // 51
constexpr std::size_t kEntryLength       = 39;
constexpr std::size_t kSaplingMaxBytes   = 131072;   // 256 blocks × 512
constexpr std::size_t kBootBlocks        = 2;
constexpr std::size_t kVolDirBlocks      = 4;
constexpr std::size_t kBitmapBlock       = 6;
constexpr std::size_t kFirstDataBlock    = kBootBlocks + kVolDirBlocks + 1; // = 7
constexpr std::size_t kMaxVolumeBlocks   = 4096;     // 1 bitmap block × 512 × 8

constexpr std::uint8_t kStorageSeedling = 0x1;
constexpr std::uint8_t kStorageSapling  = 0x2;
constexpr std::uint8_t kStorageVolDir   = 0xF;

struct PreparedFile {
    std::string                 prodosName;
    std::uint8_t                fileType   = 0;
    std::uint16_t               auxType    = 0;
    std::vector<std::uint8_t>   data;
    std::uint8_t                storageType = kStorageSeedling;
    std::size_t                 dataBlocks  = 0;
    std::size_t                 indexBlocks = 0;     // 0 for seedling, 1 for sapling
};

std::uint8_t fileTypeFromExtension(const std::string& ext)
{
    // Common ProDOS file types (cf. Apple ProDOS Tech Ref):
    //   $00 typeless / unknown
    //   $04 TXT
    //   $06 BIN
    //   $FA INT (Integer BASIC)
    //   $FC BAS (Applesoft)
    //   $FF SYS
    std::string e = ext;
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e == ".bas") return 0xFC;
    if (e == ".bin") return 0x06;
    if (e == ".sys") return 0xFF;
    if (e == ".txt") return 0x04;
    if (e == ".int") return 0xFA;
    return 0x06;  // default BIN
}

std::string sanitiseProDOSName(const std::string& hostName)
{
    fs::path p(hostName);
    std::string raw = p.filename().string();

    // Strip well-known extensions so they don't eat into the 15-char ProDOS
    // name budget (e.g. "HELLO.BAS" → "HELLO" with file_type=BAS). Other
    // extensions stay; the dot is allowed in ProDOS names.
    static const char* kStripExts[] = {
        ".bas", ".bin", ".sys", ".txt", ".int",
        ".dsk", ".po",  ".do",  ".hdv", ".2mg"
    };
    for (const char* xe : kStripExts) {
        const std::size_t xn = std::strlen(xe);
        if (raw.size() <= xn) continue;
        std::string tail = raw.substr(raw.size() - xn);
        for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (tail == xe) {
            raw.resize(raw.size() - xn);
            break;
        }
    }

    // Restrict to A-Z 0-9 . — replace anything else with '.'.
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 'a' && uc <= 'z')      out += static_cast<char>(uc - 'a' + 'A');
        else if ((uc >= 'A' && uc <= 'Z') || (uc >= '0' && uc <= '9') || uc == '.')
                                         out += static_cast<char>(uc);
        else                             out += '.';
    }
    // ProDOS names must start with a letter.
    if (out.empty() || !(out[0] >= 'A' && out[0] <= 'Z')) {
        out = "A" + out;
    }
    if (out.size() > 15) out.resize(15);
    return out;
}

std::string uniqueName(const std::string& base,
                       std::unordered_map<std::string, int>& used)
{
    if (used.find(base) == used.end()) {
        used[base] = 0;
        return base;
    }
    for (int i = 1; i < 1000; ++i) {
        const std::string suffix = "." + std::to_string(i);
        std::string cand = base;
        if (cand.size() + suffix.size() > 15) cand.resize(15 - suffix.size());
        cand += suffix;
        if (used.find(cand) == used.end()) {
            used[cand] = 0;
            return cand;
        }
    }
    return base;  // pathological — give up
}

inline void put16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
inline void put24(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
}

void writeFileEntry(std::uint8_t* dst, const PreparedFile& f,
                    std::uint16_t keyPointer, std::uint16_t blocksUsed,
                    std::uint32_t eof)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(f.prodosName.size());
    dst[0x00] = static_cast<std::uint8_t>((f.storageType << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, f.prodosName.data(), nameLen);
    dst[0x10] = f.fileType;
    put16(dst + 0x11, keyPointer);
    put16(dst + 0x13, blocksUsed);
    put24(dst + 0x15, eof);
    // creation date_time = 0 (no metadata)
    dst[0x1C] = 0;     // version
    dst[0x1D] = 0;     // min_version
    dst[0x1E] = 0xE3;  // access: full
    put16(dst + 0x1F, f.auxType);
    // last_mod date_time = 0
    put16(dst + 0x25, 2);  // header_pointer = volume directory key block
}

void writeVolumeHeader(std::uint8_t* dst, const std::string& name,
                       std::uint16_t fileCount, std::uint16_t totalBlocks)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(name.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageVolDir << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, name.data(), nameLen);
    // reserved 0x10..0x17 = 0
    // creation date_time = 0
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xC3;  // access: read/write/destroy/rename, no backup-needed
    dst[0x1F] = static_cast<std::uint8_t>(kEntryLength);
    dst[0x20] = static_cast<std::uint8_t>(kVolDirEntriesKN);
    put16(dst + 0x21, fileCount);
    put16(dst + 0x23, static_cast<std::uint16_t>(kBitmapBlock));
    put16(dst + 0x25, totalBlocks);
}

// Clear (= used) the bits for blocks [first, lastEx) in the bitmap block.
void markBitmapUsed(std::vector<std::uint8_t>& image,
                    std::size_t first, std::size_t lastEx)
{
    std::uint8_t* bm = image.data() + kBitmapBlock * kBlockBytes;
    for (std::size_t b = first; b < lastEx; ++b) {
        const std::size_t byteIdx = b >> 3;
        const std::size_t bitIdx  = 7 - (b & 7);
        bm[byteIdx] &= static_cast<std::uint8_t>(~(1u << bitIdx));
    }
}

} // namespace

ProDOSBuildResult buildVolumeFromFolder(const std::string& hostFolder,
                                        const std::string& volumeName,
                                        std::vector<std::uint8_t>& outImage)
{
    ProDOSBuildResult result;

    // Sanitise the volume name once.
    std::string vname = sanitiseProDOSName(volumeName);
    if (vname.empty()) vname = "HOST";

    // Enumerate host files. Missing folder is not an error — empty volume.
    std::vector<fs::path> hostPaths;
    std::error_code ec;
    if (fs::is_directory(hostFolder, ec)) {
        for (const auto& entry : fs::directory_iterator(hostFolder, ec)) {
            if (entry.is_regular_file()) hostPaths.push_back(entry.path());
        }
        std::sort(hostPaths.begin(), hostPaths.end());
    }

    // Prepare files: read, sanitise, classify. Skip oversize / overflow.
    std::vector<PreparedFile> files;
    std::unordered_map<std::string, int> usedNames;
    for (const auto& path : hostPaths) {
        if (files.size() >= kVolDirTotalSlots) {
            ++result.filesSkipped;
            continue;
        }
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            pom2::log().warn("ProDOSVol",
                "skipping unreadable file: " + path.filename().string());
            ++result.filesSkipped;
            continue;
        }
        f.seekg(0, std::ios::end);
        const std::size_t fsize = static_cast<std::size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        if (fsize > kSaplingMaxBytes) {
            pom2::log().warn("ProDOSVol",
                "skipping oversized file (>128 KB): " + path.filename().string());
            ++result.filesSkipped;
            continue;
        }

        PreparedFile pf;
        pf.data.resize(fsize);
        if (fsize > 0) {
            f.read(reinterpret_cast<char*>(pf.data.data()),
                   static_cast<std::streamsize>(fsize));
            if (!f) {
                pom2::log().warn("ProDOSVol",
                    "short read, skipping: " + path.filename().string());
                ++result.filesSkipped;
                continue;
            }
        }

        pf.fileType   = fileTypeFromExtension(path.extension().string());
        pf.auxType    = 0;
        pf.prodosName = uniqueName(sanitiseProDOSName(path.filename().string()),
                                   usedNames);

        if (fsize <= kBlockBytes) {
            pf.storageType = kStorageSeedling;
            pf.dataBlocks  = 1;          // one block always allocated, even for 0-byte files
            pf.indexBlocks = 0;
        } else {
            pf.storageType = kStorageSapling;
            pf.dataBlocks  = (fsize + kBlockBytes - 1) / kBlockBytes;
            pf.indexBlocks = 1;
        }
        files.push_back(std::move(pf));
    }

    // Compute total volume blocks.
    std::size_t totalBlocks = kFirstDataBlock;
    for (const auto& f : files) totalBlocks += f.dataBlocks + f.indexBlocks;
    if (totalBlocks > kMaxVolumeBlocks) {
        result.error = "synthesised volume exceeds 2 MB (1 bitmap block limit)";
        return result;
    }
    if (totalBlocks < kFirstDataBlock) totalBlocks = kFirstDataBlock;

    outImage.assign(totalBlocks * kBlockBytes, 0);
    auto blockPtr = [&](std::size_t b) { return outImage.data() + b * kBlockBytes; };

    // Volume directory pointers. Block 2 is key; blocks 3, 4, 5 extend it.
    put16(blockPtr(2) + 0, 0);   put16(blockPtr(2) + 2, 3);
    put16(blockPtr(3) + 0, 2);   put16(blockPtr(3) + 2, 4);
    put16(blockPtr(4) + 0, 3);   put16(blockPtr(4) + 2, 5);
    put16(blockPtr(5) + 0, 4);   put16(blockPtr(5) + 2, 0);

    // Volume header at offset 4 of block 2.
    writeVolumeHeader(blockPtr(2) + 4, vname,
                      static_cast<std::uint16_t>(files.size()),
                      static_cast<std::uint16_t>(totalBlocks));

    // Bitmap: initialise all bits within total_blocks as FREE, then clear
    // the structural blocks (0..6).
    {
        std::uint8_t* bm = blockPtr(kBitmapBlock);
        for (std::size_t b = 0; b < totalBlocks; ++b) {
            const std::size_t byteIdx = b >> 3;
            const std::size_t bitIdx  = 7 - (b & 7);
            bm[byteIdx] |= static_cast<std::uint8_t>(1u << bitIdx);
        }
    }
    markBitmapUsed(outImage, 0, kFirstDataBlock);

    // Lay out file data and write directory entries.
    auto entryDest = [&](std::size_t idx) -> std::uint8_t* {
        // idx 0..11 → block 2 slots 0..11 (after 4 hdr + 39 vol-header)
        // idx 12..24 → block 3 slots 0..12
        // idx 25..37 → block 4
        // idx 38..50 → block 5
        if (idx < kVolDirEntriesK0) {
            return blockPtr(2) + 4 + kEntryLength + idx * kEntryLength;
        }
        idx -= kVolDirEntriesK0;
        const std::size_t dirBlock = 3 + (idx / kVolDirEntriesKN);
        const std::size_t slot     = idx % kVolDirEntriesKN;
        return blockPtr(dirBlock) + 4 + slot * kEntryLength;
    };

    std::size_t nextBlock = kFirstDataBlock;
    std::size_t entryIdx  = 0;
    for (const auto& f : files) {
        std::uint16_t keyPointer = 0;
        std::uint16_t blocksUsed = 0;
        const std::uint32_t eof  = static_cast<std::uint32_t>(f.data.size());

        if (f.storageType == kStorageSeedling) {
            const std::size_t db = nextBlock++;
            keyPointer = static_cast<std::uint16_t>(db);
            blocksUsed = 1;
            if (!f.data.empty()) {
                std::memcpy(blockPtr(db), f.data.data(),
                            std::min<std::size_t>(f.data.size(), kBlockBytes));
            }
            markBitmapUsed(outImage, db, db + 1);
        } else {
            const std::size_t idxBlk = nextBlock++;
            keyPointer = static_cast<std::uint16_t>(idxBlk);
            blocksUsed = static_cast<std::uint16_t>(f.dataBlocks + 1);
            markBitmapUsed(outImage, idxBlk, idxBlk + 1);
            std::uint8_t* idx = blockPtr(idxBlk);
            for (std::size_t i = 0; i < f.dataBlocks; ++i) {
                const std::size_t db = nextBlock++;
                idx[i]       = static_cast<std::uint8_t>(db & 0xFF);
                idx[256 + i] = static_cast<std::uint8_t>((db >> 8) & 0xFF);

                const std::size_t off = i * kBlockBytes;
                const std::size_t len = std::min<std::size_t>(kBlockBytes, f.data.size() - off);
                std::memcpy(blockPtr(db), f.data.data() + off, len);
                markBitmapUsed(outImage, db, db + 1);
            }
        }

        writeFileEntry(entryDest(entryIdx++), f, keyPointer, blocksUsed, eof);
    }

    result.ok            = true;
    result.filesIncluded = files.size();
    result.totalBlocks   = totalBlocks;
    return result;
}

} // namespace pom2
