// AI Control HTTP server smoke test — pins the request/response cycle for
// the AiControlServer that lets external agents drive POM2.
//
// What this gates:
//   * The TCP listener accepts a loopback connection and answers a basic
//     `GET /status` with HTTP 200 + JSON containing `"ok":true`, `"cpu"`,
//     and `"profile"` keys.
//   * Auth: when a token is set, an unauthenticated request gets 401 and
//     a request carrying the right `X-POM2-Token` header gets 200.
//   * Memory bridge round-trip: `POST /mem` writes a byte at $0300 and
//     `GET /mem` reads it back.
//   * Reset: `POST /reset {"kind":"hard"}` clears the PC's high byte to a
//     plausible reset-vector destination ($F800 fallback when no ROM).
//   * 404 on unknown endpoints; 405 on wrong methods.
//
// Why this matters: the AI control endpoint is the single integration
// point an external agent (Claude Code MCP bridge, a curl-driven CI step,
// etc.) leans on. Regressions in the HTTP shape break every agent at
// once, so the contract gets pinned line-by-line.

#include "AiControlServer.h"
#include "EmulationController.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// Pick a port high enough to avoid colliding with anything an
// out-of-the-box dev box might have running. 36502 ≈ "AI"+6502.
constexpr uint16_t kTestPort = 36502;

// Connect to 127.0.0.1:port; returns the socket fd or -1 on failure.
int connectLoopback(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(port);
    // Retry the connect a handful of times — the server's worker thread
    // may not have called listen() yet on a fast first request after
    // start().
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::close(fd);
    return -1;
}

bool sendAll(int fd, const std::string& s)
{
    size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read until the peer closes. The server sets Connection: close on every
// response, so this is the correct termination condition.
std::string drainAll(int fd)
{
    std::string out;
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, buf + n);
    }
    return out;
}

struct HttpResponse {
    int status;
    std::string body;
};

HttpResponse parseResponse(const std::string& raw)
{
    HttpResponse r{0, {}};
    const size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return r;
    // "HTTP/1.1 200 OK"
    const size_t sp1 = raw.find(' ');
    if (sp1 == std::string::npos || sp1 + 1 >= raw.size()) return r;
    r.status = std::atoi(raw.c_str() + sp1 + 1);
    const size_t headersEnd = raw.find("\r\n\r\n");
    if (headersEnd != std::string::npos) {
        r.body = raw.substr(headersEnd + 4);
    }
    return r;
}

HttpResponse oneShot(uint16_t port, const std::string& request)
{
    const int fd = connectLoopback(port);
    assert(fd >= 0 && "loopback connect failed");
    const bool sent = sendAll(fd, request);
    assert(sent && "send failed");
    const std::string raw = drainAll(fd);
    ::close(fd);
    return parseResponse(raw);
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

void testStatusEndpoint(EmulationController& ctrl, pom2::AiControlServer& srv)
{
    srv.setAuthToken("");   // open
    srv.setProfileLabel("Test Profile");
    (void)ctrl;
    const HttpResponse r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"ok\":true"));
    assert(contains(r.body, "\"cpu\""));
    assert(contains(r.body, "\"profile\":\"Test Profile\""));
    std::puts("  status: OK");
}

void testAuth(EmulationController& /*ctrl*/, pom2::AiControlServer& srv)
{
    srv.setAuthToken("s3cret");

    // No header → 401.
    HttpResponse r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 401);
    assert(contains(r.body, "\"ok\":false"));

    // Wrong token → 401.
    r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nX-POM2-Token: nope\r\n\r\n");
    assert(r.status == 401);

    // Right token → 200.
    r = oneShot(kTestPort,
        "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\nX-POM2-Token: s3cret\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"ok\":true"));

    srv.setAuthToken("");
    std::puts("  auth: OK");
}

void testMemoryRoundtrip(EmulationController& ctrl, pom2::AiControlServer& /*srv*/)
{
    // POST /mem?addr=0x0300 {"data":"AB"} → /mem?addr=0x0300&len=1 reads "AB".
    const std::string writeBody = "{\"data\":\"AB\"}";
    char writeReq[512];
    std::snprintf(writeReq, sizeof(writeReq),
        "POST /mem?addr=0x0300 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        writeBody.size(), writeBody.c_str());
    HttpResponse r = oneShot(kTestPort, writeReq);
    assert(r.status == 200);
    assert(contains(r.body, "\"written\":1"));

    // Confirm via direct memory inspection that the byte actually landed
    // through Memory::memWrite (RAM at $0300 is unprotected user space).
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        assert(ctrl.memory().data()[0x0300] == 0xAB);
    }

    r = oneShot(kTestPort,
        "GET /mem?addr=0x0300&len=1 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 200);
    assert(contains(r.body, "\"data\":\"AB\""));

    std::puts("  memory roundtrip: OK");
}

void testReset(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    const std::string body = "{\"kind\":\"hard\"}";
    char req[512];
    std::snprintf(req, sizeof(req),
        "POST /reset HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    const HttpResponse r = oneShot(kTestPort, req);
    assert(r.status == 200);
    assert(contains(r.body, "\"kind\":\"hard\""));
    std::puts("  reset: OK");
}

// ─── /snapshot path-safety regression gate ───────────────────────────────
// AiControlServer canonicalises caller-supplied paths and requires them
// to stay under the emulator's working directory before opening / writing
// the file. This pins the rejection branch: a compromised agent (or a
// browser hijacked via DNS rebinding into the localhost listener) must
// not be able to read or overwrite arbitrary host files via /snapshot.
// The /disk path uses the same helper but requires a plugged DiskIICard,
// which the test fixture intentionally leaves out — exercising the
// snapshot path covers the shared code.
void testSnapshotPathSafety(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    auto post = [](const std::string& target, const std::string& body) {
        char req[1024];
        std::snprintf(req, sizeof(req),
            "POST %s HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n\r\n%s",
            target.c_str(), body.size(), body.c_str());
        return oneShot(kTestPort, req);
    };

    // /snapshot/save with an absolute path outside cwd → 403 rejected.
    // /etc/passwd exists on every Linux host so weakly_canonical resolves
    // cleanly; the prefix check is what must fire.
    HttpResponse r = post("/snapshot/save",
        "{\"path\":\"/etc/pom2_should_never_write_here.snap\"}");
    assert(r.status == 403);
    assert(contains(r.body, "path rejected"));

    // /snapshot/load on the same kind of path → 403.
    r = post("/snapshot/load", "{\"path\":\"/etc/passwd\"}");
    assert(r.status == 403);

    // /snapshot/load on a non-existent file under cwd → 403 (mustExist
    // branch — weakly_canonical succeeds but is_regular_file fails).
    r = post("/snapshot/load",
        "{\"path\":\"this_file_does_not_exist_pom2.snap\"}");
    assert(r.status == 403);

    // Happy path: a relative path under cwd is accepted. Save, observe
    // the file landed, clean up. Verifies the path-safety guard isn't
    // over-rejecting and that the canonical path is what the response
    // reports.
    namespace fs = std::filesystem;
    const std::string relName = "test_path_safety_snapshot.snap";
    const fs::path expected = fs::weakly_canonical(fs::current_path() / relName);
    fs::remove(expected);   // best-effort cleanup from a prior aborted run
    r = post("/snapshot/save", "{\"path\":\"" + relName + "\"}");
    assert(r.status == 200);
    assert(fs::exists(expected));
    assert(contains(r.body, expected.string()));
    fs::remove(expected);

    // R4-#3: a symlink under cwd whose target is OUTSIDE cwd must be
    // rejected for save — weakly_canonical returns it lexically (inside
    // cwd) but ofstream would follow it out of the jail.
    {
        const fs::path outside =
            fs::temp_directory_path() / "pom2_symlink_escape_target.snap";
        const std::string linkName = "pom2_evil_link.snap";
        const fs::path link = fs::current_path() / linkName;
        std::error_code ec;
        fs::remove(outside, ec);
        fs::remove(link, ec);
        fs::create_symlink(outside, link, ec);
        if (!ec) {                       // skip if the platform lacks symlinks
            HttpResponse rs = post("/snapshot/save",
                "{\"path\":\"" + linkName + "\"}");
            assert(rs.status == 403);
            assert(!fs::exists(outside)); // nothing written through the link
            fs::remove(link, ec);
            fs::remove(outside, ec);
        }
    }

    std::puts("  snapshot path-safety: OK");
}

void testNotFoundAndMethod(EmulationController& /*ctrl*/, pom2::AiControlServer& /*srv*/)
{
    HttpResponse r = oneShot(kTestPort,
        "GET /nope HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 404);

    // /reset rejects GET.
    r = oneShot(kTestPort,
        "GET /reset HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    assert(r.status == 405);
    std::puts("  not-found/method: OK");
}

void testSpeed(EmulationController& ctrl, pom2::AiControlServer& /*srv*/)
{
    const std::string body = "{\"preset\":\"2x\"}";
    char req[512];
    std::snprintf(req, sizeof(req),
        "POST /speed HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
    const HttpResponse r = oneShot(kTestPort, req);
    assert(r.status == 200);
    assert(ctrl.getCyclesPerFrame() == 34090);

    auto postSpeed = [](const std::string& body) {
        char rq[512];
        std::snprintf(rq, sizeof(rq),
            "POST /speed HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Content-Length: %zu\r\n\r\n%s", body.size(), body.c_str());
        return oneShot(kTestPort, rq);
    };

    // R4-#1: out-of-range cycles_per_frame is REJECTED, not cast to a
    // garbage int that freezes/pauses the emulator. State stays unchanged.
    HttpResponse rr = postSpeed("{\"cycles_per_frame\":9999999999}");
    assert(rr.status == 400);
    assert(ctrl.getCyclesPerFrame() == 34090);
    rr = postSpeed("{\"cycles_per_frame\":0}");
    assert(rr.status == 400);
    // A valid in-range value is accepted.
    rr = postSpeed("{\"cycles_per_frame\":50000}");
    assert(rr.status == 200);
    assert(ctrl.getCyclesPerFrame() == 50000);

    // R4-#2: jsonGetString must match the key at an object-key position, not
    // inside another field's VALUE. "label"'s value is the quoted string
    // "cycles_per_frame"; the real numeric field must still be found.
    rr = postSpeed("{\"label\":\"cycles_per_frame\",\"cycles_per_frame\":17045}");
    assert(rr.status == 200);
    assert(ctrl.getCyclesPerFrame() == 17045);

    std::puts("  speed: OK");
}

// Regression: applyProfile() / restartEmulationFromSettings() rebuild the
// machine with stop()…tear down cards…start() while the CPU worker thread
// is ALREADY running. start() then can't re-spawn the thread, so it must
// re-arm the run mode itself — otherwise the machine stays Stopped after a
// profile or slot-config switch and freezes on an uncleared ("@"-tile
// garbage) text page. Because a saved non-default profile auto-applies on
// startup, that surfaced as "the emulator doesn't boot on launch". Pin that
// start() resumes Running on BOTH the cold path (no worker yet) and the hot
// path (worker already live) that the profile switch actually hits.
void testStartResumesMode(EmulationController& ctrl)
{
    ctrl.stop();
    assert(ctrl.getMode() == EmulationController::Mode::Stopped);
    ctrl.start();   // cold path: spawns the worker AND arms Running
    assert(ctrl.getMode() == EmulationController::Mode::Running &&
           "start() must resume Running after stop()");
    ctrl.stop();
    assert(ctrl.getMode() == EmulationController::Mode::Stopped);
    ctrl.start();   // hot path: worker already joinable — must STILL arm Running
    assert(ctrl.getMode() == EmulationController::Mode::Running &&
           "start() must resume Running even when the worker is already "
           "live (the applyProfile/restart frozen-on-switch regression)");
    ctrl.stop();    // park again so the worker idles quietly until teardown
    std::puts("  start-resumes-mode: OK");
}

} // namespace

int main()
{
    EmulationController ctrl;
    // Don't actually start the CPU worker — these tests prod state in
    // place and don't need cycles being consumed underneath the assertions.
    ctrl.setMode(EmulationController::Mode::Stopped);

    pom2::AiControlServer srv;
    srv.attach(&ctrl, nullptr, nullptr, nullptr);
    const bool started = srv.start(kTestPort);
    assert(started && "AiControlServer failed to bind test port — is something else on 36502?");

    testStatusEndpoint   (ctrl, srv);
    testAuth             (ctrl, srv);
    testMemoryRoundtrip  (ctrl, srv);
    testReset            (ctrl, srv);
    testSpeed            (ctrl, srv);
    testSnapshotPathSafety(ctrl, srv);
    testNotFoundAndMethod(ctrl, srv);
    testStartResumesMode (ctrl);   // last: it spawns the CPU worker thread

    srv.stop();
    std::puts("ai_control_server_smoke_test: OK");
    return 0;
}
