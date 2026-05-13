// POM2 Apple II Emulator
// Copyright (C) 2026

#include "AiControlServer.h"

#include "Apple2Display.h"
#include "DiskIICard.h"
#include "EmulationController.h"
#include "Logger.h"
#include "M6502.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace pom2 {

namespace {

constexpr size_t kMaxBodyBytes    = 1 << 20;   // 1 MiB hard cap on body
constexpr size_t kMaxHeaderBytes  = 64 * 1024; // 64 KiB on the request preamble
constexpr int    kRecvTimeoutMs   = 4000;      // per-client read deadline

// ─── String / parsing helpers ────────────────────────────────────────────

std::string toLowerAscii(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::pair<std::string,std::string>> parseQuery(const std::string& q)
{
    std::vector<std::pair<std::string,std::string>> out;
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        size_t eq = q.find('=', i);
        std::string key, val;
        if (eq == std::string::npos || eq > amp) {
            key = q.substr(i, amp - i);
        } else {
            key = q.substr(i, eq - i);
            val = q.substr(eq + 1, amp - eq - 1);
        }
        out.emplace_back(std::move(key), std::move(val));
        i = amp + 1;
    }
    return out;
}

std::string queryParam(const std::string& q, const std::string& key)
{
    for (const auto& kv : parseQuery(q)) {
        if (kv.first == key) return kv.second;
    }
    return {};
}

/// Minimal extractor for `"key":<literal>` and `"key":"<quoted>"` shapes.
/// POM2's API surface uses flat one-level JSON only — no nested objects or
/// arrays — so a hand-rolled scanner is enough and avoids dragging in a
/// JSON dependency. Accepts unquoted tokens (numbers, true/false), quoted
/// strings with `\"`, `\\`, `\n`, `\r`, `\t` escapes. Unknown keys → empty
/// string; the caller supplies the default.
std::string jsonGetString(const std::string& body, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size() || body[pos] != ':') return {};
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    if (pos >= body.size()) return {};
    if (body[pos] == '"') {
        ++pos;
        std::string out;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                const char esc = body[pos + 1];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '"': out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    default:   out.push_back(esc);  break;
                }
                pos += 2;
            } else {
                out.push_back(body[pos]);
                ++pos;
            }
        }
        return out;
    }
    std::string out;
    while (pos < body.size() && body[pos] != ',' && body[pos] != '}' &&
           !std::isspace(static_cast<unsigned char>(body[pos]))) {
        out.push_back(body[pos]);
        ++pos;
    }
    return out;
}

bool jsonGetInt(const std::string& body, const std::string& key, long& out)
{
    const std::string s = jsonGetString(body, key);
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        // Accept decimal or 0x… hex for convenience.
        const int base = (s.size() > 2 && s[0] == '0' && (s[1]=='x'||s[1]=='X'))
                       ? 16 : 10;
        out = std::stol(s, &pos, base);
        return pos > 0;
    } catch (...) {
        return false;
    }
}

bool fromHex(char c, uint8_t& nib)
{
    if (c >= '0' && c <= '9') { nib = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { nib = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { nib = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out)
{
    if (hex.size() % 2) return false;
    out.clear();
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        uint8_t hi, lo;
        if (!fromHex(hex[i], hi) || !fromHex(hex[i + 1], lo)) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string bytesToHex(const uint8_t* data, size_t n)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void applyRecvTimeout(int fd, int timeoutMs)
{
    struct timeval tv{};
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

bool sendAll(int fd, const char* buf, size_t n)
{
    while (n > 0) {
        const ssize_t s = ::send(fd, buf, n, MSG_NOSIGNAL);
        if (s <= 0) {
            if (s < 0 && errno == EINTR) continue;
            return false;
        }
        buf += s;
        n   -= static_cast<size_t>(s);
    }
    return true;
}

std::string cpuModeName(M6502::CpuMode m)
{
    return m == M6502::CpuMode::CMOS ? "65c02" : "nmos";
}

std::string runModeName(EmulationController::Mode m)
{
    switch (m) {
        case EmulationController::Mode::Stopped: return "stopped";
        case EmulationController::Mode::Running: return "running";
        case EmulationController::Mode::Step:    return "step";
    }
    return "?";
}

} // namespace

// ─── Request helpers ──────────────────────────────────────────────────────

std::string AiControlServer::Request::headerValue(const std::string& name) const
{
    const std::string want = toLowerAscii(name);
    for (const auto& kv : headers) {
        if (toLowerAscii(kv.first) == want) return kv.second;
    }
    return {};
}

// ─── Lifecycle ────────────────────────────────────────────────────────────

AiControlServer::~AiControlServer()
{
    stop();
}

void AiControlServer::attach(EmulationController* ctrl,
                             Apple2Display*       display,
                             DiskIICard*          disk6,
                             ProDOSHardDiskCard*  hdv5)
{
    ctrl_    = ctrl;
    display_ = display;
    disk6_   = disk6;
    hdv5_    = hdv5;
}

bool AiControlServer::start(uint16_t port)
{
    stop();
    if (!ctrl_) {
        pom2::log().warn("AICtrl", "start() called before attach() — refusing");
        return false;
    }
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        pom2::log().warn("AICtrl", std::string("socket() failed: ") + std::strerror(errno));
        return false;
    }
    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        pom2::log().warn("AICtrl",
            "bind 127.0.0.1:" + std::to_string(port) + " failed: " +
            std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 4) < 0) {
        pom2::log().warn("AICtrl", std::string("listen() failed: ") +
                         std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    port_           = port;
    stopRequested_  = false;
    running_        = true;
    worker_         = std::thread(&AiControlServer::runWorker, this);
    pom2::log().info("AICtrl",
        "listening on 127.0.0.1:" + std::to_string(port_) +
        " — POST/GET to drive the emulator from an AI agent");
    return true;
}

void AiControlServer::stop()
{
    if (!running_ && !worker_.joinable()) return;
    stopRequested_ = true;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (worker_.joinable()) worker_.join();
    running_ = false;
}

void AiControlServer::runWorker()
{
    while (!stopRequested_) {
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        const int fd = ::accept(listenFd_,
                                reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (stopRequested_) break;
            if (errno == EINTR) continue;
            break;
        }
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        {
            std::lock_guard<std::mutex> lk(mtx_);
            lastClient_ = ::inet_ntoa(peer.sin_addr);
        }
        handleClient(fd);
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        ++requestsServed_;
    }
}

// ─── Request parsing ──────────────────────────────────────────────────────

bool AiControlServer::readRequest(int fd, Request& req)
{
    applyRecvTimeout(fd, kRecvTimeoutMs);

    std::string buffer;
    buffer.reserve(2048);
    size_t headerEnd = std::string::npos;
    char chunk[2048];
    while (headerEnd == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return false;
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got <= 0) return false;
        buffer.append(chunk, chunk + got);
        headerEnd = buffer.find("\r\n\r\n");
    }

    // Request line: "METHOD path?query HTTP/x.y"
    const size_t lineEnd = buffer.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd == 0) return false;
    std::istringstream iss(buffer.substr(0, lineEnd));
    std::string url;
    iss >> req.method >> url;
    if (req.method.empty() || url.empty()) return false;
    const size_t q = url.find('?');
    if (q == std::string::npos) {
        req.path  = url;
        req.query.clear();
    } else {
        req.path  = url.substr(0, q);
        req.query = url.substr(q + 1);
    }

    // Header lines until the blank line.
    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        const size_t eol = buffer.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd) break;
        const std::string line = buffer.substr(pos, eol - pos);
        pos = eol + 2;
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name  = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        size_t v = 0;
        while (v < value.size() && (value[v] == ' ' || value[v] == '\t')) ++v;
        value.erase(0, v);
        req.headers.emplace_back(std::move(name), std::move(value));
    }

    // Body bounded by Content-Length. Whatever already landed in `buffer`
    // past the header terminator counts toward the body, plus extra recv()s
    // until we have the full length.
    const size_t bodyStart = headerEnd + 4;
    const std::string clStr = req.headerValue("Content-Length");
    if (!clStr.empty()) {
        long cl = 0;
        try { cl = std::stol(clStr); } catch (...) { return false; }
        if (cl < 0 || static_cast<size_t>(cl) > kMaxBodyBytes) return false;
        req.body.reserve(static_cast<size_t>(cl));
        if (bodyStart < buffer.size()) {
            req.body.append(buffer, bodyStart, std::string::npos);
        }
        while (req.body.size() < static_cast<size_t>(cl)) {
            const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
            if (got <= 0) return false;
            req.body.append(chunk, chunk + got);
        }
        if (req.body.size() > static_cast<size_t>(cl)) {
            req.body.resize(static_cast<size_t>(cl));
        }
    }
    return true;
}

// ─── Response helpers ─────────────────────────────────────────────────────

void AiControlServer::sendResponse(int fd,
                                   int status,
                                   const std::string& contentType,
                                   const std::string& body)
{
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 201: reason = "Created"; break;
        case 204: reason = "No Content"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 500: reason = "Internal Server Error"; break;
        case 503: reason = "Service Unavailable"; break;
        default:  reason = "Status"; break;
    }
    char head[512];
    const int n = std::snprintf(head, sizeof(head),
        "HTTP/1.1 %d %s\r\n"
        "Server: POM2-AICtrl\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status, reason, contentType.c_str(), body.size());
    if (n <= 0) return;
    sendAll(fd, head, static_cast<size_t>(n));
    if (!body.empty()) sendAll(fd, body.data(), body.size());
}

void AiControlServer::sendJsonError(int fd, int status, const std::string& message)
{
    const std::string b = "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}";
    sendResponse(fd, status, "application/json", b);
}

void AiControlServer::sendJsonOk(int fd, const std::string& body)
{
    // `body` is expected to be either an empty JSON object `{}` or a body
    // starting with `{` and ending with `}`. We inject `"ok":true` after
    // the opening brace so every successful response shares the same shape.
    if (body.size() < 2 || body.front() != '{' || body.back() != '}') {
        sendResponse(fd, 200, "application/json",
                     "{\"ok\":true,\"data\":" + body + "}");
        return;
    }
    if (body.size() == 2) {
        sendResponse(fd, 200, "application/json", "{\"ok\":true}");
        return;
    }
    std::string merged = "{\"ok\":true,";
    merged.append(body.begin() + 1, body.end());
    sendResponse(fd, 200, "application/json", merged);
}

bool AiControlServer::checkAuth(const Request& req) const
{
    std::string configured;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        configured = authToken_;
    }
    if (configured.empty()) return true;
    return req.headerValue("X-POM2-Token") == configured;
}

// ─── Dispatch ─────────────────────────────────────────────────────────────

void AiControlServer::handleClient(int fd)
{
    Request req;
    if (!readRequest(fd, req)) {
        sendJsonError(fd, 400, "malformed request");
        return;
    }
    // CORS preflight — bypasses auth so a browser-hosted agent can probe
    // the API. Auth still gates the real request that follows.
    if (req.method == "OPTIONS") {
        char head[256];
        const int n = std::snprintf(head, sizeof(head),
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-POM2-Token\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n");
        if (n > 0) sendAll(fd, head, static_cast<size_t>(n));
        return;
    }
    if (!checkAuth(req)) {
        sendJsonError(fd, 401, "missing or invalid X-POM2-Token");
        return;
    }

    if (req.path == "/status")               return handleStatus(fd, req);
    if (req.path == "/reset")                return handleReset(fd, req);
    if (req.path == "/cpu") {
        return req.method == "GET" ? handleCpuGet(fd, req)
                                   : handleCpuSet(fd, req);
    }
    if (req.path == "/mem") {
        return req.method == "GET" ? handleMemGet(fd, req)
                                   : handleMemSet(fd, req);
    }
    if (req.path == "/keyboard")             return handleKeyboard(fd, req);
    if (req.path == "/disk")                 return handleDiskInsert(fd, req);
    if (req.path == "/eject")                return handleDiskEject(fd, req);
    if (req.path == "/snapshot/save")        return handleSnapshotSave(fd, req);
    if (req.path == "/snapshot/load")        return handleSnapshotLoad(fd, req);
    if (req.path == "/speed")                return handleSpeed(fd, req);
    if (req.path == "/screen.ppm")           return handleScreen(fd, req);

    sendJsonError(fd, 404, "no such endpoint: " + req.path);
}

// ─── Endpoint implementations ─────────────────────────────────────────────

void AiControlServer::handleStatus(int fd, const Request& /*req*/)
{
    std::string profile;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        profile = profileLabel_;
    }
    // Snapshot the CPU/memory state under stateMutex so we don't sample a
    // half-written PC or A register mid-instruction.
    M6502& cpu = ctrl_->cpu();
    EmulationController::Mode mode = ctrl_->getMode();
    int cpf = ctrl_->getCyclesPerFrame();

    uint16_t pc; uint8_t a, x, y, p, sp; uint64_t cycles;
    std::string cpuMode;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        pc = cpu.getProgramCounter();
        a  = cpu.getAccumulator();
        x  = cpu.getXRegister();
        y  = cpu.getYRegister();
        p  = cpu.getStatusRegister();
        sp = cpu.getStackPointer();
        cycles = ctrl_->memory().getCycleCounter();
        cpuMode = cpuModeName(cpu.getCpuMode());
    }

    // Disk status. Locks the same mutex briefly — DiskIICard introspection
    // is read-only but still touches the shared head-position fields.
    std::string disks = "[]";
    if (disk6_) {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        std::ostringstream oss;
        oss << "[";
        for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
            if (d) oss << ",";
            oss << "{\"slot\":6,\"drive\":" << d
                << ",\"path\":\"" << jsonEscape(disk6_->getDiskPath(d)) << "\""
                << ",\"loaded\":" << (disk6_->isDiskLoaded(d) ? "true" : "false")
                << ",\"track\":" << disk6_->getCurrentTrack(d)
                << "}";
        }
        oss << "]";
        disks = oss.str();
    }

    std::ostringstream oss;
    oss << "{"
        << "\"profile\":\""        << jsonEscape(profile) << "\","
        << "\"cpu_mode\":\""       << cpuMode              << "\","
        << "\"mode\":\""           << runModeName(mode)    << "\","
        << "\"cycles_per_frame\":" << cpf                  << ","
        << "\"requests_served\":"  << requestsServed_.load() << ","
        << "\"cpu\":{"
            << "\"pc\":"     << pc     << ","
            << "\"a\":"      << +a     << ","
            << "\"x\":"      << +x     << ","
            << "\"y\":"      << +y     << ","
            << "\"p\":"      << +p     << ","
            << "\"sp\":"     << +sp    << ","
            << "\"cycles\":" << cycles
        << "},"
        << "\"disks\":" << disks
        << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleReset(int fd, const Request& req)
{
    if (req.method != "POST") {
        sendJsonError(fd, 405, "POST only"); return;
    }
    std::string kind = jsonGetString(req.body, "kind");
    if (kind.empty()) kind = "hard";
    if      (kind == "soft") ctrl_->softReset();
    else if (kind == "hard") ctrl_->hardReset();
    else if (kind == "cold") ctrl_->coldBoot();
    else { sendJsonError(fd, 400, "kind must be soft|hard|cold"); return; }
    sendJsonOk(fd, "{\"kind\":\"" + kind + "\"}");
}

void AiControlServer::handleCpuGet(int fd, const Request& /*req*/)
{
    M6502& cpu = ctrl_->cpu();
    std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
    std::ostringstream oss;
    oss << "{"
        << "\"pc\":"     << cpu.getProgramCounter() << ","
        << "\"a\":"      << +cpu.getAccumulator()   << ","
        << "\"x\":"      << +cpu.getXRegister()     << ","
        << "\"y\":"      << +cpu.getYRegister()     << ","
        << "\"p\":"      << +cpu.getStatusRegister()<< ","
        << "\"sp\":"     << +cpu.getStackPointer()  << ","
        << "\"cpu_mode\":\"" << cpuModeName(cpu.getCpuMode()) << "\","
        << "\"cycles\":" << ctrl_->memory().getCycleCounter()
        << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleCpuSet(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    M6502& cpu = ctrl_->cpu();
    std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
    long v = 0;
    if (jsonGetInt(req.body, "pc", v)) cpu.setProgramCounter(static_cast<uint16_t>(v & 0xFFFF));
    // A/X/Y/P/SP setters are not currently exposed on M6502 — only PC has
    // an accessor (used by the Klaus harness). Punt on the rest for now;
    // PC alone covers the "jump to a routine" use case agents need most.
    sendJsonOk(fd, "{}");
}

void AiControlServer::handleMemGet(int fd, const Request& req)
{
    if (req.method != "GET") { sendJsonError(fd, 405, "GET only"); return; }
    long addr = -1, len = -1;
    try { addr = std::stol(queryParam(req.query, "addr"), nullptr, 0); } catch (...) {}
    try { len  = std::stol(queryParam(req.query, "len"),  nullptr, 0); } catch (...) {}
    if (addr < 0 || addr >= 0x10000) { sendJsonError(fd, 400, "addr out of range"); return; }
    if (len  < 0 || len  > 4096)     { sendJsonError(fd, 400, "len must be 0..4096"); return; }
    if (addr + len > 0x10000) len = 0x10000 - addr;

    std::vector<uint8_t> buf(static_cast<size_t>(len));
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        const uint8_t* mem = ctrl_->memory().data();
        std::memcpy(buf.data(), mem + addr, static_cast<size_t>(len));
    }
    std::ostringstream oss;
    oss << "{\"addr\":" << addr
        << ",\"len\":" << len
        << ",\"data\":\"" << bytesToHex(buf.data(), buf.size()) << "\"}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleMemSet(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    long addr = -1;
    try { addr = std::stol(queryParam(req.query, "addr"), nullptr, 0); } catch (...) {}
    if (addr < 0 || addr >= 0x10000) { sendJsonError(fd, 400, "addr out of range"); return; }

    const std::string hex = jsonGetString(req.body, "data");
    if (hex.empty()) { sendJsonError(fd, 400, "missing \"data\" hex string"); return; }
    std::vector<uint8_t> bytes;
    if (!hexToBytes(hex, bytes)) { sendJsonError(fd, 400, "data is not even-length hex"); return; }
    if (addr + static_cast<long>(bytes.size()) > 0x10000) {
        sendJsonError(fd, 400, "write overflows address space"); return;
    }
    size_t written = 0;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        Memory& mem = ctrl_->memory();
        for (size_t i = 0; i < bytes.size(); ++i) {
            // memWrite respects ROM protection and routes through soft-
            // switches; that's exactly what we want for "drive the Apple
            // II as a peer" — agents should not be able to overwrite the
            // monitor ROM by accident.
            mem.memWrite(static_cast<uint16_t>(addr + i), bytes[i]);
            ++written;
        }
    }
    std::ostringstream oss;
    oss << "{\"addr\":" << addr << ",\"written\":" << written << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleKeyboard(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string text = jsonGetString(req.body, "text");
    const std::string raw  = jsonGetString(req.body, "raw");
    if (text.empty() && raw.empty()) {
        sendJsonError(fd, 400, "supply \"text\" or \"raw\""); return;
    }
    size_t n = 0;
    if (!text.empty()) n += ctrl_->memory().pasteText(text);
    if (!raw.empty())  n += ctrl_->memory().pasteRawKeys(raw.data(), raw.size());
    sendJsonOk(fd, "{\"queued\":" + std::to_string(n) + "}");
}

void AiControlServer::handleDiskInsert(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    if (!disk6_) { sendJsonError(fd, 503, "no Disk II card plugged"); return; }
    long slot  = 6;
    long drive = 0;
    jsonGetInt(req.body, "slot",  slot);
    jsonGetInt(req.body, "drive", drive);
    const std::string path = jsonGetString(req.body, "path");
    if (slot != 6) { sendJsonError(fd, 400, "only slot 6 is implemented"); return; }
    if (drive < 0 || drive >= DiskIICard::kDriveCount) {
        sendJsonError(fd, 400, "drive must be 0 or 1"); return;
    }
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }
    bool ok;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        ok = disk6_->insertDisk(static_cast<int>(drive), path);
    }
    if (!ok) {
        sendJsonError(fd, 400, "insert failed: " + disk6_->getLastError(static_cast<int>(drive)));
        return;
    }
    sendJsonOk(fd, "{\"slot\":6,\"drive\":" + std::to_string(drive) +
                   ",\"path\":\"" + jsonEscape(path) + "\"}");
}

void AiControlServer::handleDiskEject(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    if (!disk6_) { sendJsonError(fd, 503, "no Disk II card plugged"); return; }
    long drive = 0;
    jsonGetInt(req.body, "drive", drive);
    if (drive < 0 || drive >= DiskIICard::kDriveCount) {
        sendJsonError(fd, 400, "drive must be 0 or 1"); return;
    }
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        disk6_->ejectDisk(static_cast<int>(drive));
    }
    sendJsonOk(fd, "{\"drive\":" + std::to_string(drive) + "}");
}

void AiControlServer::handleSnapshotSave(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string path = jsonGetString(req.body, "path");
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }

    SnapshotWriter w(path);
    if (!w.good()) { sendJsonError(fd, 400, "cannot open " + path); return; }
    std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
    // Minimal payload: CPU regs (compact) + 64 KiB main RAM. Matches the
    // existing SnapshotIO section roster from the header docstring — disk
    // state is deliberately excluded per CLAUDE.md.
    M6502& cpu = ctrl_->cpu();
    {
        SnapshotWriter::SectionHandle h = w.beginSection("CPU");
        w.writeU16(cpu.getProgramCounter());
        w.writeU8 (cpu.getAccumulator());
        w.writeU8 (cpu.getXRegister());
        w.writeU8 (cpu.getYRegister());
        w.writeU8 (cpu.getStatusRegister());
        w.writeU8 (cpu.getStackPointer());
        w.writeU8 (cpu.getCpuMode() == M6502::CpuMode::CMOS ? 1 : 0);
        w.writeU64(ctrl_->memory().getCycleCounter());
        w.endSection(h);
    }
    w.writeSection("MEM", ctrl_->memory().data(), 0x10000);
    sendJsonOk(fd, "{\"path\":\"" + jsonEscape(path) + "\"}");
}

void AiControlServer::handleSnapshotLoad(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    const std::string path = jsonGetString(req.body, "path");
    if (path.empty()) { sendJsonError(fd, 400, "missing \"path\""); return; }

    SnapshotReader r(path);
    if (!r.good()) { sendJsonError(fd, 400, "cannot read " + path + ": " + r.error()); return; }
    std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
    M6502& cpu = ctrl_->cpu();
    Memory& mem = ctrl_->memory();
    std::string name;
    uint32_t len = 0;
    while (r.nextSection(name, len)) {
        if (name == "CPU" && len >= 9) {
            const uint16_t pc = r.readU16();
            const uint8_t  a  = r.readU8();
            const uint8_t  x  = r.readU8();
            const uint8_t  y  = r.readU8();
            const uint8_t  p  = r.readU8();
            const uint8_t  sp = r.readU8();
            const uint8_t  cpuMode = r.readU8();
            // cycles read but currently no public setter — track for parity
            // but ignore on load for now (cycleCounter is only consulted by
            // paddle RC math; resetting it briefly is harmless).
            (void)r.readU64();
            cpu.setProgramCounter(pc);
            cpu.setCpuMode(cpuMode ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
            // A/X/Y/P/SP setters are not exposed; the snapshot's CPU
            // section is therefore informational on the round-trip for
            // those four — the file still records them so a future
            // M6502 setter API lights up the path without a format bump.
            (void)a; (void)x; (void)y; (void)p; (void)sp;
        } else if (name == "MEM" && len == 0x10000) {
            uint8_t* dst = mem.dataMutable();
            r.readBytes(dst, 0x10000);
        } else {
            r.skipCurrentSection();
        }
    }
    sendJsonOk(fd, "{\"path\":\"" + jsonEscape(path) + "\"}");
}

void AiControlServer::handleSpeed(int fd, const Request& req)
{
    if (req.method != "POST") { sendJsonError(fd, 405, "POST only"); return; }
    long cpf = -1;
    if (jsonGetInt(req.body, "cycles_per_frame", cpf) && cpf > 0) {
        ctrl_->setCyclesPerFrame(static_cast<int>(cpf));
    } else {
        const std::string preset = jsonGetString(req.body, "preset");
        if      (preset == "1x")  ctrl_->setCyclesPerFrame(17045);
        else if (preset == "2x")  ctrl_->setCyclesPerFrame(34090);
        else if (preset == "max") ctrl_->setCyclesPerFrame(1000000);
        else { sendJsonError(fd, 400, "supply cycles_per_frame or preset 1x|2x|max"); return; }
    }
    std::ostringstream oss;
    oss << "{\"cycles_per_frame\":" << ctrl_->getCyclesPerFrame() << "}";
    sendJsonOk(fd, oss.str());
}

void AiControlServer::handleScreen(int fd, const Request& /*req*/)
{
    if (!display_) { sendJsonError(fd, 503, "no display attached"); return; }
    // Render under stateMutex so we don't race the CPU thread mutating
    // VRAM mid-scan — same contract as MainWindow::render().
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;
    {
        std::lock_guard<std::mutex> lk(ctrl_->stateMutex());
        display_->render(ctrl_->memory());
        // Apple2Display packs pixels as `0xAABBGGRR` (RGBA in LE memory:
        // R G B A bytes) — see Apple2Display.cpp ctor + the MainWindow
        // upload site. We re-interpret as raw bytes and drop the alpha
        // byte to produce a PPM-friendly RGB triplet stream.
        const uint8_t* rgba = reinterpret_cast<const uint8_t*>(display_->pixels());
        w = display_->width();
        h = display_->height();
        rgb.resize(static_cast<size_t>(w) * h * 3);
        for (int i = 0; i < w * h; ++i) {
            rgb[i * 3 + 0] = rgba[i * 4 + 0];
            rgb[i * 3 + 1] = rgba[i * 4 + 1];
            rgb[i * 3 + 2] = rgba[i * 4 + 2];
        }
    }
    char header[64];
    const int hn = std::snprintf(header, sizeof(header), "P6\n%d %d\n255\n", w, h);
    std::string body;
    body.reserve(static_cast<size_t>(hn) + rgb.size());
    body.append(header, static_cast<size_t>(hn));
    body.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    sendResponse(fd, 200, "image/x-portable-pixmap", body);
}

} // namespace pom2
