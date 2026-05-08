// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ProDOSVolume.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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

constexpr std::uint8_t kStorageSeedling     = 0x1;
constexpr std::uint8_t kStorageSapling      = 0x2;
constexpr std::uint8_t kStorageSubdirEntry  = 0xD;
constexpr std::uint8_t kStorageSubdirHeader = 0xE;
constexpr std::uint8_t kStorageVolDir       = 0xF;

constexpr std::uint8_t kFileTypeDir         = 0x0F;

constexpr std::size_t  kMaxRecursionDepth   = 16;

struct PreparedFile {
    std::string                 prodosName;
    std::uint8_t                fileType   = 0;
    std::uint16_t               auxType    = 0;
    std::vector<std::uint8_t>   data;
    std::uint8_t                storageType = kStorageSeedling;
    std::size_t                 dataBlocks  = 0;
    std::size_t                 indexBlocks = 0;     // 0 for seedling, 1 for sapling
    // Filled at layout: keyPointer (= seedling data block, sapling index block).
    std::size_t                 firstBlock  = 0;
};

struct PreparedDir;

// Order of children within a directory. We mix files and subdirs into a
// single iteration order (alphabetical by ProDOS name). `index` is into
// either `files` or `subdirs` of the owning PreparedDir.
struct DirChild {
    bool        isDir = false;
    std::size_t index = 0;
};

struct PreparedDir {
    std::string                                  prodosName;     // empty for vol root
    std::vector<PreparedFile>                    files;
    std::vector<std::unique_ptr<PreparedDir>>    subdirs;
    std::vector<DirChild>                        order;
    // Layout fields (filled in pass 2):
    std::size_t   firstDirBlock = 0;     // volume dir = 2; subdirs allocated.
    std::size_t   numDirBlocks  = 1;     // volume dir = 4.
    std::size_t   parentDirBlock  = 0;
    std::uint8_t  parentEntrySlot = 0;   // 1-based, ProDOS convention; 0 = vol.
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
                    std::uint32_t eof, std::uint16_t headerPointer = 2)
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
    put16(dst + 0x25, headerPointer);
}

void writeSubdirEntryImpl(std::uint8_t* dst, const PreparedDir& sd,
                          std::uint16_t keyPointer, std::uint16_t blocksUsed,
                          std::uint16_t headerPointer)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(sd.prodosName.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageSubdirEntry << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, sd.prodosName.data(), nameLen);
    dst[0x10] = kFileTypeDir;
    put16(dst + 0x11, keyPointer);
    put16(dst + 0x13, blocksUsed);
    const std::uint32_t eof = static_cast<std::uint32_t>(blocksUsed) *
                              static_cast<std::uint32_t>(kBlockBytes);
    put24(dst + 0x15, eof);
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xE3;
    put16(dst + 0x1F, 0);   // aux_type = 0 for subdirs
    put16(dst + 0x25, headerPointer);
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

// Subdirectory header sits at offset 4 of the subdir's first block. Layout
// is parallel to writeVolumeHeader but with storage_type=$E + parent
// pointers + the $75 sentinel byte at offset 0x14.
void writeSubdirHeader(std::uint8_t* dst, const std::string& name,
                       std::uint16_t fileCount,
                       std::uint16_t parentBlock,
                       std::uint8_t  parentEntrySlot)
{
    std::memset(dst, 0, kEntryLength);
    const std::uint8_t nameLen = static_cast<std::uint8_t>(name.size());
    dst[0x00] = static_cast<std::uint8_t>((kStorageSubdirHeader << 4) | (nameLen & 0x0F));
    std::memcpy(dst + 1, name.data(), nameLen);
    dst[0x14] = 0x75;                                      // ProDOS reserved sentinel
    dst[0x1C] = 0;
    dst[0x1D] = 0;
    dst[0x1E] = 0xC3;
    dst[0x1F] = static_cast<std::uint8_t>(kEntryLength);
    dst[0x20] = static_cast<std::uint8_t>(kVolDirEntriesKN);
    put16(dst + 0x21, fileCount);
    put16(dst + 0x23, parentBlock);
    dst[0x25] = parentEntrySlot;                            // 1-based
    dst[0x26] = static_cast<std::uint8_t>(kEntryLength);
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

// Number of directory blocks needed to hold `entryCount` entries. Block 0 of
// any directory holds 12 entries (after the 39-byte header at offset 4) and
// every subsequent block holds 13. Always ≥ 1.
std::size_t numDirBlocksFor(std::size_t entryCount)
{
    if (entryCount <= kVolDirEntriesK0) return 1;
    return 1 + (entryCount - kVolDirEntriesK0 + kVolDirEntriesKN - 1)
                 / kVolDirEntriesKN;
}

// Recursively populate a PreparedDir from the given host folder. `usedNames`
// is per-directory (subdir name collisions don't conflict with parent dir
// names). `result` accumulates filesIncluded / filesSkipped counters.
void scanHostFolder(const fs::path& hostPath, PreparedDir& dir,
                    std::size_t depth, ProDOSBuildResult& result)
{
    if (depth > kMaxRecursionDepth) {
        pom2::log().warn("ProDOSVol",
            "skipping subtree, recursion depth exceeded: " + hostPath.string());
        return;
    }

    std::error_code ec;
    if (!fs::is_directory(hostPath, ec)) return;

    std::vector<fs::path> children;
    for (const auto& entry : fs::directory_iterator(hostPath, ec)) {
        // Skip dotfiles (e.g. .DS_Store) — they pollute the synth volume
        // with platform metadata the guest can't make sense of.
        const std::string nm = entry.path().filename().string();
        if (!nm.empty() && nm.front() == '.') continue;
        if (entry.is_regular_file(ec) || entry.is_directory(ec)) {
            children.push_back(entry.path());
        }
    }
    std::sort(children.begin(), children.end());

    std::unordered_map<std::string, int> usedNames;
    for (const auto& path : children) {
        const std::size_t directChildCount = dir.order.size();
        const std::size_t budget =
            (depth == 0) ? kVolDirTotalSlots : (1u << 16);  // subdirs: large soft cap
        if (directChildCount >= budget) {
            ++result.filesSkipped;
            continue;
        }

        if (fs::is_directory(path, ec)) {
            auto sub = std::make_unique<PreparedDir>();
            sub->prodosName = uniqueName(sanitiseProDOSName(path.filename().string()),
                                         usedNames);
            scanHostFolder(path, *sub, depth + 1, result);
            sub->numDirBlocks = numDirBlocksFor(sub->order.size());
            dir.order.push_back({true, dir.subdirs.size()});
            dir.subdirs.push_back(std::move(sub));
            continue;
        }

        // Regular file (existing logic, mostly).
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
            pf.dataBlocks  = 1;
            pf.indexBlocks = 0;
        } else {
            pf.storageType = kStorageSapling;
            pf.dataBlocks  = (fsize + kBlockBytes - 1) / kBlockBytes;
            pf.indexBlocks = 1;
        }
        ++result.filesIncluded;
        dir.order.push_back({false, dir.files.size()});
        dir.files.push_back(std::move(pf));
    }
}

// Pointer to the entry slot at 0-based `slot` within `dirBlock`. For the
// volume directory's block 2, slot 0 is the volume header (caller must
// avoid it for file entries). For subdirs the same is true of slot 0 of
// the first dir block (subdir header).
std::uint8_t* dirSlotPtr(std::uint8_t* image, std::size_t dirBlock,
                         std::size_t slot)
{
    return image + dirBlock * kBlockBytes + 4 + slot * kEntryLength;
}

// Convert a 0-based "child index" within a directory to (dirBlock, slot)
// pair. The first dir block's slot 0 is reserved for the header, so child 0
// lands at (firstBlock, slot 1). Subsequent blocks use all 13 slots.
//
// Returns the 1-based ProDOS slot number (1..13) in `prodosSlot1Based`.
void childIndexToBlockSlot(std::size_t firstBlock, std::size_t childIndex,
                           std::size_t& outBlock, std::size_t& outSlotIn,
                           std::uint8_t& prodosSlot1Based)
{
    if (childIndex < kVolDirEntriesK0) {
        outBlock         = firstBlock;
        outSlotIn        = childIndex + 1;     // skip header
        prodosSlot1Based = static_cast<std::uint8_t>(outSlotIn + 1);  // ProDOS counts from 1
        return;
    }
    const std::size_t rest    = childIndex - kVolDirEntriesK0;
    const std::size_t blkOff  = rest / kVolDirEntriesKN + 1;          // +1 = next block
    const std::size_t slot    = rest % kVolDirEntriesKN;
    outBlock         = firstBlock + blkOff;
    outSlotIn        = slot;
    prodosSlot1Based = static_cast<std::uint8_t>(slot + 1);
}

// Lay down the data + (if sapling) index block for `f`. Updates `nextBlock`
// linearly, mirroring the original buildVolumeFromFolder allocation order
// so flat-volume tests stay byte-identical to the pre-subdir layout.
// Sets `f.firstBlock` (the seedling data block, or the sapling index block).
void writeFileData(std::vector<std::uint8_t>& image, PreparedFile& f,
                   std::size_t& nextBlock)
{
    auto blockPtr = [&](std::size_t b) { return image.data() + b * kBlockBytes; };
    if (f.storageType == kStorageSeedling) {
        const std::size_t db = nextBlock++;
        f.firstBlock = db;
        if (!f.data.empty()) {
            std::memcpy(blockPtr(db), f.data.data(),
                        std::min<std::size_t>(f.data.size(), kBlockBytes));
        }
        markBitmapUsed(image, db, db + 1);
    } else {
        const std::size_t idxBlk = nextBlock++;
        f.firstBlock = idxBlk;
        markBitmapUsed(image, idxBlk, idxBlk + 1);
        std::uint8_t* idx = blockPtr(idxBlk);
        for (std::size_t i = 0; i < f.dataBlocks; ++i) {
            const std::size_t db = nextBlock++;
            idx[i]       = static_cast<std::uint8_t>(db & 0xFF);
            idx[256 + i] = static_cast<std::uint8_t>((db >> 8) & 0xFF);
            const std::size_t off = i * kBlockBytes;
            const std::size_t len = std::min<std::size_t>(kBlockBytes, f.data.size() - off);
            std::memcpy(blockPtr(db), f.data.data() + off, len);
            markBitmapUsed(image, db, db + 1);
        }
    }
}

// Recursively emit `dir`: lay down its directory blocks (prev/next chain),
// header (volume or subdir), then for each child either write a file
// directory entry + its data, or recursively emit a child subdir then
// write the subdir entry.
//
// `nextBlock` is the next free data block. `dir.firstDirBlock` and
// `dir.numDirBlocks` must already be set by the caller (volume root: 2/4;
// subdirs: pre-allocated by emitDir's recursive caller).
void emitDir(std::vector<std::uint8_t>& image, PreparedDir& dir,
             std::size_t& nextBlock,
             std::size_t parentDirBlock, std::uint8_t parentEntrySlot,
             bool isVolumeRoot, const std::string& volumeName,
             std::uint16_t totalBlocks /* meaningful only for vol root */)
{
    auto blockPtr = [&](std::size_t b) { return image.data() + b * kBlockBytes; };

    // Linked list of dir blocks: prev=0, next=block+1, ..., last next=0.
    for (std::size_t i = 0; i < dir.numDirBlocks; ++i) {
        const std::size_t b      = dir.firstDirBlock + i;
        const std::size_t prev   = (i == 0) ? 0 : (b - 1);
        const std::size_t next   = (i + 1 < dir.numDirBlocks) ? (b + 1) : 0;
        put16(blockPtr(b) + 0, static_cast<std::uint16_t>(prev));
        put16(blockPtr(b) + 2, static_cast<std::uint16_t>(next));
    }

    // Mark the dir blocks as used in the bitmap.
    markBitmapUsed(image, dir.firstDirBlock,
                   dir.firstDirBlock + dir.numDirBlocks);

    // Header at offset 4 of the first dir block.
    const std::uint16_t childCount = static_cast<std::uint16_t>(dir.order.size());
    if (isVolumeRoot) {
        writeVolumeHeader(blockPtr(dir.firstDirBlock) + 4, volumeName,
                          childCount, totalBlocks);
    } else {
        writeSubdirHeader(blockPtr(dir.firstDirBlock) + 4, dir.prodosName,
                          childCount,
                          static_cast<std::uint16_t>(parentDirBlock),
                          parentEntrySlot);
    }

    // Walk children in the order they were inserted (alphabetical from the
    // scanner). For each child, either write a file entry + its data, or
    // recurse to emit the subdir before writing the subdir entry. We need
    // the subdir's keyPointer/blocksUsed BEFORE writing the parent entry,
    // so subdirs go first.
    const std::uint16_t headerPtr = static_cast<std::uint16_t>(dir.firstDirBlock);
    for (std::size_t childIdx = 0; childIdx < dir.order.size(); ++childIdx) {
        std::size_t  outBlock = 0;
        std::size_t  outSlot  = 0;
        std::uint8_t prodosSlot = 0;
        childIndexToBlockSlot(dir.firstDirBlock, childIdx,
                              outBlock, outSlot, prodosSlot);
        std::uint8_t* slot = dirSlotPtr(image.data(), outBlock, outSlot);

        const DirChild& dc = dir.order[childIdx];
        if (dc.isDir) {
            PreparedDir& sub = *dir.subdirs[dc.index];
            sub.firstDirBlock = nextBlock;
            nextBlock += sub.numDirBlocks;
            emitDir(image, sub, nextBlock,
                    /*parentDirBlock*/ outBlock,
                    /*parentEntrySlot*/ prodosSlot,
                    /*isVolumeRoot*/ false, volumeName, 0);
            writeSubdirEntryImpl(slot, sub,
                static_cast<std::uint16_t>(sub.firstDirBlock),
                static_cast<std::uint16_t>(sub.numDirBlocks),
                headerPtr);
        } else {
            PreparedFile& f = dir.files[dc.index];
            writeFileData(image, f, nextBlock);
            const std::uint16_t blocksUsed =
                static_cast<std::uint16_t>(f.dataBlocks + f.indexBlocks);
            writeFileEntry(slot, f,
                static_cast<std::uint16_t>(f.firstBlock),
                blocksUsed,
                static_cast<std::uint32_t>(f.data.size()),
                headerPtr);
        }
    }
}

} // namespace

// Recurse over the prepared tree and accumulate the total block count
// (dir blocks + file data + index blocks) for every node. Does NOT include
// structural blocks (0..6); the caller adds those.
namespace {
std::size_t totalBlocksForTree(const PreparedDir& dir, bool isVolumeRoot)
{
    std::size_t n = isVolumeRoot ? 0 : dir.numDirBlocks;     // vol dir is in 2..5
    for (const auto& f : dir.files) n += f.dataBlocks + f.indexBlocks;
    for (const auto& sd : dir.subdirs) n += totalBlocksForTree(*sd, false);
    return n;
}
}  // namespace

ProDOSBuildResult buildVolumeFromFolder(const std::string& hostFolder,
                                        const std::string& volumeName,
                                        std::vector<std::uint8_t>& outImage)
{
    ProDOSBuildResult result;

    // Sanitise the volume name once.
    std::string vname = sanitiseProDOSName(volumeName);
    if (vname.empty()) vname = "HOST";

    // Phase 1: walk the host folder tree (recursive).
    PreparedDir root;
    scanHostFolder(hostFolder, root, /*depth=*/0, result);
    root.firstDirBlock = 2;
    root.numDirBlocks  = kVolDirBlocks;        // vol dir always 4 blocks (51 slots)

    // Phase 2: total block count.
    std::size_t totalBlocks = kFirstDataBlock;     // 7 structural
    totalBlocks += totalBlocksForTree(root, /*isVolumeRoot=*/true);
    if (totalBlocks > kMaxVolumeBlocks) {
        result.error = "synthesised volume exceeds 2 MB (1 bitmap block limit)";
        return result;
    }
    if (totalBlocks < kFirstDataBlock) totalBlocks = kFirstDataBlock;

    outImage.assign(totalBlocks * kBlockBytes, 0);

    // Bitmap: initialise all bits within total_blocks as FREE, then mark
    // the 7 structural blocks USED. emitDir clears bits as it lays down
    // dir + file blocks.
    {
        std::uint8_t* bm = outImage.data() + kBitmapBlock * kBlockBytes;
        for (std::size_t b = 0; b < totalBlocks; ++b) {
            const std::size_t byteIdx = b >> 3;
            const std::size_t bitIdx  = 7 - (b & 7);
            bm[byteIdx] |= static_cast<std::uint8_t>(1u << bitIdx);
        }
    }
    markBitmapUsed(outImage, 0, kFirstDataBlock);

    // Phase 3: emit. emitDir handles the linked-list prev/next chain for
    // the volume's 4 dir blocks AND the parallel chain for each subdir.
    std::size_t nextBlock = kFirstDataBlock;
    emitDir(outImage, root, nextBlock,
            /*parentDirBlock=*/0, /*parentEntrySlot=*/0,
            /*isVolumeRoot=*/true, vname,
            static_cast<std::uint16_t>(totalBlocks));

    result.ok          = true;
    result.totalBlocks = totalBlocks;
    return result;
}

namespace {

const char* extFromFileType(std::uint8_t t)
{
    // Inverse of fileTypeFromExtension. Default to .bin for anything we
    // didn't originally produce — keeps round-trip safe.
    switch (t) {
        case 0x04: return ".txt";
        case 0xFA: return ".int";
        case 0xFC: return ".bas";
        case 0xFF: return ".sys";
        case 0x06: default: return ".bin";
    }
}

inline std::uint16_t rd16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(p[1]) << 8;
}
inline std::uint32_t rd24(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) |
           static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16;
}

// Atomic overwrite: write to `dest`.tmp then rename — no torn file even on
// crash. Returns true if the file was actually written (false = identical
// content already on disk so we skipped the touch).
bool writeFileAtomic(const fs::path& dest, const std::vector<std::uint8_t>& bytes)
{
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        std::ifstream in(dest, std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            const auto sz = static_cast<std::size_t>(in.tellg());
            if (sz == bytes.size()) {
                std::vector<std::uint8_t> have(sz);
                in.seekg(0, std::ios::beg);
                in.read(reinterpret_cast<char*>(have.data()),
                        static_cast<std::streamsize>(sz));
                if (in && have == bytes) return false;   // identical, no-op
            }
        }
    }
    fs::path tmp = dest;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return true;   // caller's error path will fire
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        if (!out) {
            fs::remove(tmp, ec);
            return true;
        }
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        // Cross-device rename can fail; fall back to copy + remove.
        fs::copy_file(tmp, dest, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    return true;
}

}  // namespace

namespace {

// Walk one ProDOS directory (volume root or subdir) starting at `firstBlock`
// and recreate its contents under `hostFolder`. Recurses into subdir entries
// (storage_type $D) by creating a host subdirectory and calling itself.
// `depth` is bounded to avoid infinite loops on malformed volumes.
void decodeOneDir(const std::vector<std::uint8_t>& image,
                  std::size_t totalBlocks,
                  std::uint16_t firstBlock,
                  const std::string& hostFolder,
                  std::size_t depth,
                  ProDOSDecodeResult& r)
{
    if (depth > kMaxRecursionDepth) {
        pom2::log().warn("ProDOSVol",
            "decode: recursion depth exceeded under " + hostFolder);
        return;
    }

    auto blockPtr = [&](std::size_t b) -> const std::uint8_t* {
        return image.data() + b * kBlockBytes;
    };

    std::uint16_t curBlock = firstBlock;
    std::size_t   guard    = 0;
    while (curBlock != 0 && guard++ < 256) {
        if (curBlock >= totalBlocks) break;
        const std::uint8_t* blk = blockPtr(curBlock);

        for (std::size_t slot = 0; slot < kVolDirEntriesKN; ++slot) {
            const std::size_t off = 4 + slot * kEntryLength;
            if (off + kEntryLength > kBlockBytes) break;
            const std::uint8_t* e = blk + off;
            const std::uint8_t storage = static_cast<std::uint8_t>(e[0] >> 4);
            const std::uint8_t nameLen = e[0] & 0x0F;
            if (storage == 0 || nameLen == 0)            continue;   // empty
            if (storage == kStorageVolDir ||
                storage == kStorageSubdirHeader)         continue;   // headers

            std::string name(reinterpret_cast<const char*>(e + 1), nameLen);
            const std::uint16_t keyPtr = rd16(e + 0x11);
            if (keyPtr == 0 || keyPtr >= totalBlocks) {
                ++r.filesSkipped;
                continue;
            }

            // Subdirectory entry → recurse.
            if (storage == kStorageSubdirEntry) {
                while (!name.empty() && name.back() == '.') name.pop_back();
                if (name.empty()) { ++r.filesSkipped; continue; }
                const fs::path subDest = fs::path(hostFolder) / name;
                std::error_code ec;
                fs::create_directories(subDest, ec);
                if (ec) {
                    pom2::log().warn("ProDOSVol",
                        "cannot create subdir " + subDest.string() + ": " + ec.message());
                    ++r.filesSkipped;
                    continue;
                }
                decodeOneDir(image, totalBlocks, keyPtr, subDest.string(),
                             depth + 1, r);
                continue;
            }

            if (storage != kStorageSeedling && storage != kStorageSapling) {
                ++r.filesSkipped;                                    // tree / weird
                continue;
            }

            const std::uint8_t  fileType = e[0x10];
            const std::uint32_t eof      = rd24(e + 0x15);

            if (eof > kSaplingMaxBytes) {
                pom2::log().warn("ProDOSVol",
                    "skipping oversize file in decode: " + name);
                ++r.filesSkipped;
                continue;
            }

            std::vector<std::uint8_t> data;
            data.reserve(eof);

            if (storage == kStorageSeedling) {
                const std::uint8_t* d = blockPtr(keyPtr);
                const std::size_t   take = std::min<std::size_t>(eof, kBlockBytes);
                data.insert(data.end(), d, d + take);
            } else {
                // Sapling: keyPtr → index block. Bytes 0..255 hold low bytes
                // of data block #s; bytes 256..511 hold the high bytes.
                const std::uint8_t* idx = blockPtr(keyPtr);
                std::size_t remaining = eof;
                for (std::size_t i = 0; i < 256 && remaining > 0; ++i) {
                    const std::uint16_t db =
                        static_cast<std::uint16_t>(idx[i]) |
                        static_cast<std::uint16_t>(idx[256 + i]) << 8;
                    if (db == 0 || db >= totalBlocks) break;
                    const std::size_t take = std::min<std::size_t>(remaining, kBlockBytes);
                    const std::uint8_t* d = blockPtr(db);
                    data.insert(data.end(), d, d + take);
                    remaining -= take;
                }
                if (data.size() < eof) {
                    pom2::log().warn("ProDOSVol",
                        "sapling file truncated on decode: " + name);
                }
            }

            // Compose host filename: ProDOS name + extension from file_type.
            // Strip any trailing dot the synth path may have left.
            while (!name.empty() && name.back() == '.') name.pop_back();
            if (name.empty()) {
                ++r.filesSkipped;
                continue;
            }
            const fs::path dest = fs::path(hostFolder) /
                                  (name + extFromFileType(fileType));
            if (writeFileAtomic(dest, data)) {
                ++r.filesWritten;
            }
        }
        // Next directory block pointer is at offset 2 of every dir block.
        curBlock = rd16(blk + 2);
    }
}

}  // namespace

ProDOSDecodeResult decodeVolumeToFolder(const std::vector<std::uint8_t>& image,
                                        const std::string& hostFolder)
{
    ProDOSDecodeResult r;

    if (image.size() < (kFirstDataBlock * kBlockBytes)) {
        r.error = "volume image too small (need ≥ "
                  + std::to_string(kFirstDataBlock * kBlockBytes) + " bytes)";
        return r;
    }
    if ((image.size() % kBlockBytes) != 0) {
        r.error = "volume image is not a whole number of 512-byte blocks";
        return r;
    }
    const std::size_t totalBlocks = image.size() / kBlockBytes;

    std::error_code ec;
    fs::create_directories(hostFolder, ec);
    if (ec) {
        r.error = "cannot create host folder '" + hostFolder + "': " + ec.message();
        return r;
    }

    decodeOneDir(image, totalBlocks, /*firstBlock=*/2, hostFolder, /*depth=*/0, r);

    r.ok = true;
    return r;
}

} // namespace pom2
