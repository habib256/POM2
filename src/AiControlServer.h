// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AiControlServer — HTTP/1.1 localhost bridge for AI-agent control of POM2.
//
// Inspired by `paleotronic/microm8-cln` (`remint/`, `fastserv/`) and by
// modern MCP/AI-agent emulator drivers. Lets an external process — typically
// an AI agent like Claude Code via curl/MCP — drive POM2 the same way a
// human drives it from the UI: type at the keyboard, reset, mount disks,
// peek/poke RAM, save and load snapshots, scrub speed, grab the framebuffer.
//
// Wire format: plain HTTP/1.1 with JSON bodies. Hand-rolled parser, no
// third-party dependency — the SSC already proved a 200-line TCP listener
// is the right scope for POM2's "minimum external deps" policy.
//
// Endpoints (all opt-in via settings.ai_control_enable; default off):
//
//   GET  /status              → {profile, cpu_mode, mode, cycles_per_frame,
//                                cpu:{pc,a,x,y,p,sp,cycles}, disks:[...]}
//   POST /reset               → body {"kind":"soft|hard|cold"} (default hard)
//   GET  /cpu                 → CPU register dump
//   POST /cpu                 → body {"pc":?, "a":?, "x":?, "y":?} — set regs
//   GET  /mem?addr=N&len=N    → {"addr":N,"data":"FFEE..."} (hex, len ≤ 4096)
//   POST /mem?addr=N          → body {"data":"FFEE..."} — bulk write (RAM)
//   POST /keyboard            → body {"text":"..."} or {"raw":"..."} — paste
//   POST /disk                → body {"slot":6,"drive":0,"path":"..."} — insert
//   POST /eject               → body {"slot":6,"drive":0} — eject
//   POST /snapshot/save       → body {"path":"....pom2snap"} — save path MUST
//                                end with `.pom2snap` so an agent can't
//                                clobber unrelated files inside cwd.
//   POST /snapshot/load       → body {"path":"..."} — any cwd-relative file;
//                                magic-byte check inside rejects non-snapshots.
//   POST /speed               → body {"cycles_per_frame":N} OR {"preset":"1x|2x|max"}
//   GET  /screen.ppm          → binary PPM of the live framebuffer
//   POST /mouse               → body {"dx":?,"dy":?} signed Apple-cursor delta
//                                (±127/call) OR {"x":?,"y":?} absolute counter,
//                                {"btn":0|1}, {"reset":1}. Drives the Mouse
//                                Card's host-motion input exactly as
//                                MainWindow::onMouseMove would — lets an agent
//                                exercise mouse-driven apps headlessly.
//
// Authentication: optional shared-secret in the `X-POM2-Token` header. When
// the configured token is empty, requests are accepted unauthenticated
// (loopback-only listener already limits exposure to local processes; an
// AI agent on the same machine doesn't need to fight a token round-trip).
//
// Threading: one worker thread, one client at a time. Each request acquires
// `EmulationController::stateMutex()` for the slice of work that touches
// CPU/Memory/slot state, mirrors the rule the UI thread already follows.
// Memory's own paste-queue mutex covers /keyboard without needing stateMtx.

#ifndef POM2_AI_CONTROL_SERVER_H
#define POM2_AI_CONTROL_SERVER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmulationController;
class M6502;
class Memory;
class DiskIICard;
class ProDOSHardDiskCard;
class Apple2Display;
class MouseCard;

namespace pom2 {

struct SystemProfileSnapshot {
    std::string name;          // "Apple ][+", "Apple //e Enhanced", …
    std::string cpuMode;       // "nmos" or "65c02"
};

class AiControlServer
{
public:
    static constexpr uint16_t kDefaultPort = 6503;     // distinct from SSC's 6502

    AiControlServer() = default;
    ~AiControlServer();

    AiControlServer(const AiControlServer&)            = delete;
    AiControlServer& operator=(const AiControlServer&) = delete;

    /// Wire the controller + display once the emulator is fully constructed.
    /// All pointers are non-owning; the server holds them across requests but
    /// never deletes them. Disk/HDV pointers may be null — the corresponding
    /// endpoints then return 503.
    ///
    /// Threading: when `ctrl_` is already bound (i.e., this is a re-attach
    /// after a profile switch), the caller MUST hold `ctrl->stateMutex()`
    /// for the duration of the call. Handlers read disk6_/hdv5_ under the
    /// same mutex, so this serialises the pointer swap against in-flight
    /// requests. On the very first call (no worker thread alive yet) the
    /// lock is optional.
    void attach(EmulationController* ctrl,
                Apple2Display*       display,
                DiskIICard*          disk6,
                ProDOSHardDiskCard*  hdv5);

    /// Null the slot-card pointers. Caller MUST hold the controller's
    /// stateMutex. Use this BEFORE the slot bus tears down its plugged
    /// cards during a profile switch — between detach() and the next
    /// attach(), card-touching endpoints return 503 instead of
    /// dereferencing freed memory. Pairs with attach().
    void detach();

    /// Update the cached display-side metadata that `/status` reports.
    /// Called by MainWindow whenever the profile or token changes.
    void setProfileLabel(const std::string& label) {
        std::lock_guard<std::mutex> lk(mtx_);
        profileLabel_ = label;
    }
    void setAuthToken(const std::string& token) {
        std::lock_guard<std::mutex> lk(mtx_);
        authToken_ = token;
    }

    /// Start the TCP listener on `127.0.0.1:port`. Returns false on bind/
    /// listen failure (the previous instance, if any, is torn down first).
    /// Thread-safe.
    bool start(uint16_t port);
    /// Stop the listener and join the worker. Safe to call repeatedly.
    void stop();

    bool     isRunning() const { return running_.load(); }
    uint16_t getPort()   const { return port_;           }
    uint64_t requestsServed() const { return requestsServed_.load(); }
    std::string lastClientAddr() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return lastClient_;
    }

private:
    // ─── Bound emulator handles (non-owning) ─────────────────────────────
    EmulationController* ctrl_    = nullptr;
    Apple2Display*       display_ = nullptr;
    DiskIICard*          disk6_   = nullptr;
    ProDOSHardDiskCard*  hdv5_    = nullptr;

    // ─── Listener state ──────────────────────────────────────────────────
    mutable std::mutex     mtx_;
    std::atomic<bool>      running_       { false };
    std::atomic<bool>      stopRequested_ { false };
    std::atomic<uint64_t>  requestsServed_{ 0 };
    int                    listenFd_ = -1;
    uint16_t               port_     = kDefaultPort;
    std::thread            worker_;
    std::string            authToken_;
    std::string            profileLabel_;
    std::string            lastClient_;

    // ─── /mouse running state ────────────────────────────────────────────
    // Running Apple-cursor counters mirrored from MainWindow's own
    // mouseAppleX/Y, so an agent can feed deltas across many requests and
    // the card sees a continuous 8-bit counter (the MCU firmware computes
    // motion via wrap-corrected subtraction — see MouseCard::updateAxis).
    // Guarded by ctrl_->stateMutex() when written (same as the card read).
    uint8_t mouseAccumX_ = 0;
    uint8_t mouseAccumY_ = 0;
    bool    mouseBtn_    = false;

    void runWorker();
    void handleClient(int fd);

    // HTTP request shape (parsed in-place from the socket).
    struct Request {
        std::string method;             // "GET", "POST", …
        std::string path;               // path portion of the URL
        std::string query;              // raw query string (no leading '?')
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        std::string headerValue(const std::string& name) const;
    };

    /// Drain a single HTTP request from `fd` into `req`. Returns false if
    /// the peer closed before a full request landed, or if the request
    /// looks malformed (oversized body, missing terminator, etc.) — in
    /// which case the caller should send a 400 and close.
    bool readRequest(int fd, Request& req);
    /// Send a response. Body may be binary; pass the right Content-Type.
    void sendResponse(int fd,
                      int status,
                      const std::string& contentType,
                      const std::string& body);
    void sendJsonError(int fd, int status, const std::string& message);
    void sendJsonOk   (int fd, const std::string& body);

    // ─── Request handlers ────────────────────────────────────────────────
    void handleStatus  (int fd, const Request& req);
    void handleReset   (int fd, const Request& req);
    void handleCpuGet  (int fd, const Request& req);
    void handleCpuSet  (int fd, const Request& req);
    void handleMemGet  (int fd, const Request& req);
    void handleMemSet  (int fd, const Request& req);
    void handleKeyboard(int fd, const Request& req);
    void handleDiskInsert(int fd, const Request& req);
    void handleDiskEject (int fd, const Request& req);
    void handleSnapshotSave(int fd, const Request& req);
    void handleSnapshotLoad(int fd, const Request& req);
    void handleSpeed   (int fd, const Request& req);
    void handleScreen  (int fd, const Request& req);
    void handleMouse   (int fd, const Request& req);

    /// True when the request carries a valid auth token (or when no token
    /// is configured server-side). Caller still has to send the 401 — this
    /// is just the predicate.
    bool checkAuth(const Request& req) const;
};

} // namespace pom2

#endif // POM2_AI_CONTROL_SERVER_H
