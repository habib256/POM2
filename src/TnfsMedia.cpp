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

#include "TnfsMedia.h"

#include "AtomicFileReplace.h"
#include "Logger.h"
#include "TnfsClient.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace pom2 {

namespace {

namespace fs = std::filesystem;

/// Characters a cache file name may keep. Everything else becomes '_': the
/// path comes off a remote server and lands in the user's filesystem, so it
/// is not to be trusted with '/', '..' or a drive letter.
bool nameSafe(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           c == '.' || c == '-' || c == '_' || c == ' ';
}

/// A flat, collision-resistant cache name for a remote path. The directories
/// are folded into the name rather than recreated on disk — a server-supplied
/// tree is exactly what a path-traversal bug is made of, and nothing here
/// needs the shape.
std::string cacheNameFor(const std::string& host, const std::string& path)
{
    std::string flat = host + path;
    for (char& c : flat) if (!nameSafe(c)) c = '_';
    // Keep the tail: it carries the extension, and classifyDiskForSlot reads
    // the extension to decide which drive the image belongs in. But a pure
    // tail truncation dropped the HOST (it sits at the front): two servers
    // mirroring the same ≥120-char path collided on one cache file, and the
    // existence-only cache check then served server A's bytes as server B's
    // disk — silently, offline, and poisonable on purpose. Prefix a hash of
    // the FULL host+path so every distinct URL keeps a distinct key.
    constexpr std::size_t kMaxName = 120;
    if (flat.size() > kMaxName) {
        std::uint64_t h = 1469598103934665603ull;          // FNV-1a 64
        for (unsigned char c : flat) { h ^= c; h *= 1099511628211ull; }
        char hex[17];
        for (int i = 15; i >= 0; --i) {
            hex[i] = "0123456789abcdef"[h & 0xF];
            h >>= 4;
        }
        hex[16] = '\0';
        flat = std::string(hex) + "_" +
               flat.substr(flat.size() - (kMaxName - 17));
    }
    return flat;
}

} // namespace

bool parseTnfsUrl(const std::string& url, std::string& host, std::uint16_t& port,
                  std::string& path)
{
    std::string s = url;

    // The scheme is optional; anything else is not ours.
    std::string lower = s;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("tnfs://", 0) == 0) {
        s = s.substr(7);
    } else if (lower.find("://") != std::string::npos) {
        return false;
    }

    const std::size_t slash = s.find('/');
    if (slash == std::string::npos) return false;      // a host with no file
    std::string hostport = s.substr(0, slash);
    path = s.substr(slash);
    if (hostport.empty() || path.size() < 2) return false;

    port = TnfsClient::kDefaultPort;
    const std::size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        const std::string ps = hostport.substr(colon + 1);
        if (ps.empty()) return false;
        long p = 0;
        for (char c : ps) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
            p = p * 10 + (c - '0');
            if (p > 65535) return false;
        }
        if (p <= 0) return false;
        port = static_cast<std::uint16_t>(p);
        hostport = hostport.substr(0, colon);
    }
    if (hostport.empty()) return false;
    host = hostport;
    return true;
}

TnfsFetchResult tnfsFetchImage(const std::string& url,
                               const std::string& cacheDir)
{
    TnfsFetchResult r;

    std::string host, path;
    std::uint16_t port = TnfsClient::kDefaultPort;
    if (!parseTnfsUrl(url, host, port, path)) {
        r.error = "not a TNFS URL: \"" + url +
                  "\" (want tnfs://host[:port]/path/to/image.po)";
        return r;
    }

    // ── The cache, checked BEFORE any socket is opened ───────────────────
    // Existence is the whole test: every fetch below lands through
    // writeFileAtomic, so a file that is here is a COMPLETE previous fetch —
    // a half-written one cannot survive to be mounted as a truncated disk.
    // That is what lets the check be offline, which is the point. A boot disk
    // fetched yesterday still boots on a train today.
    //
    // The trade is staleness: a server that replaces an image keeps serving
    // the old one to POM2 until the user clears the cache. TNFS offers no
    // ETag and no cheap content hash, so the alternative is re-reading the
    // whole file to find out — which costs exactly what the cache saves.
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    const fs::path dest = fs::path(cacheDir) / cacheNameFor(host, path);
    if (fs::exists(dest, ec)) {
        const auto sz = fs::file_size(dest, ec);
        if (!ec && sz > 0) {
            r.ok        = true;
            r.fromCache = true;
            r.localPath = dest.string();
            r.bytes     = static_cast<std::uint32_t>(sz);
            log().info("TNFS", "cached " + dest.filename().string() + " (" +
                               std::to_string(sz) + " bytes) — no fetch");
            return r;
        }
    }

    TnfsClient c;
    std::string err;
    // MOUNT the ROOT and address the file by its absolute path. Mounting the
    // file's own directory would work too, but servers differ on whether a
    // sub-path is mountable at all, and the root always is.
    if (!c.mount(host, port, "/", err)) {
        r.error = "TNFS mount " + host + " failed: " + err;
        return r;
    }
    r.usedTcp = c.usingTcp();

    std::uint32_t size = 0;
    if (!c.fileSize(path, size, err)) {
        r.error = "TNFS stat " + path + " failed: " + err;
        return r;
    }
    if (size == 0) {
        r.error = "TNFS " + path + " is empty";
        return r;
    }
    if (size > kTnfsMaxImageBytes) {
        r.error = "TNFS " + path + " is " + std::to_string(size) +
                  " bytes, over the " + std::to_string(kTnfsMaxImageBytes) +
                  "-byte ceiling";
        return r;
    }
    r.bytes = size;

    const int handle = c.openFile(path, err);
    if (handle < 0) {
        r.error = "TNFS open " + path + " failed: " + err;
        return r;
    }

    log().info("TNFS", "fetching " + host + path + " (" +
                       std::to_string(size) + " bytes over " +
                       (r.usedTcp ? "TCP" : "UDP") + ")");

    std::vector<std::uint8_t> bytes(size);
    // readAt already loops over the protocol's 525-byte cap; the outer chunk
    // exists so a long transfer can report progress instead of looking hung.
    constexpr std::uint32_t kChunk = 64u * 1024u;
    std::uint32_t done = 0;
    std::uint32_t nextReport = kChunk * 8;
    while (done < size) {
        const std::uint32_t n = std::min(kChunk, size - done);
        if (!c.readAt(handle, done, bytes.data() + done, n, err)) {
            c.closeFile(handle);
            r.error = "TNFS read " + path + " at " + std::to_string(done) +
                      " failed: " + err;
            return r;
        }
        done += n;
        if (done >= nextReport && done < size) {
            log().info("TNFS", "  " + std::to_string(done) + " / " +
                               std::to_string(size) + " bytes");
            nextReport += kChunk * 8;
        }
    }
    c.closeFile(handle);

    // Atomic, like every other write-back in POM2: a half-written cache file
    // that survives a crash would be mounted as a truncated disk next time,
    // and a truncated disk is the failure nobody diagnoses.
    if (!writeFileAtomic(dest, bytes.data(), bytes.size(), ec)) {
        r.error = "cannot write " + dest.string() + ": " + ec.message();
        return r;
    }

    r.ok        = true;
    r.localPath = dest.string();
    log().info("TNFS", "fetched " + dest.filename().string() + " (" +
                       std::to_string(size) + " bytes)");
    return r;
}

} // namespace pom2
