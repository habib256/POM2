// POM2's built-in `N:` against a stub HTTP server, in-process.
//
// No network dependency: the stub binds 127.0.0.1 on an ephemeral port, so
// this runs in CI. What it pins is what actually cost debugging time when the
// device was written against a real guest:
//
//   * The devicespec is NOT the whole control list. The guest sends
//     aux1 (open mode), aux2 (translation), THEN the spec — measured off the
//     wire from the FujiNet Contiki browser as
//     `04 00 4E 3A 68 74 74 70 ...`. Taking the list verbatim put two binary
//     bytes in front of the URL and every single open failed with an empty
//     host. That offset is the card's job, so what this file pins is the
//     parsing either side of it: "N:" prefix, scheme, host, port, path.
//   * Only the BODY reaches the guest. Guest-side browsers parse HTML, not
//     response headers.
//   * STATUS says "connected" while bytes remain and drops when they run
//     out — guest read loops end on that flag, so getting it wrong either
//     truncates the page or spins forever.
//   * The byte count is capped at 512, because guest code sizes its buffer
//     from it.

#include "FujiNetNetDevice.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
void check(bool cond, const char* what)
{
    if (cond) std::printf("[ OK ] %s\n", what);
    else    { std::printf("FAIL: %s\n", what); ++failures; }
}

/// Serves one canned response, then closes — which is what HTTP/1.0 with
/// `Connection: close` asks for and what the device relies on to find EOF.
struct HttpStub {
    std::string  body;
    /// Answer with headers and HALF the body, then hold the connection open
    /// and say nothing more — a server that stalls mid-page, which is the
    /// case a per-recv timeout cannot bound.
    bool         stall = false;
    int          listenFd = -1;
    uint16_t     port     = 0;
    std::thread  th;
    std::string  seenRequest;
    std::atomic<bool> done{false};
    std::atomic<bool> stopping{false};

    bool start()
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        int one = 1;
        ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        socklen_t al = sizeof a;
        if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&a), &al) != 0) return false;
        port = ntohs(a.sin_port);
        if (::listen(listenFd, 2) != 0) return false;

        th = std::thread([this] {
            const int c = ::accept(listenFd, nullptr, nullptr);
            if (c < 0) return;
            char buf[2048];
            const ssize_t r = ::recv(c, buf, sizeof buf - 1, 0);
            if (r > 0) { buf[r] = '\0'; seenRequest = buf; }
            std::string resp =
                "HTTP/1.0 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Server: stub\r\n\r\n";
            resp += stall ? body.substr(0, body.size() / 2) : body;
            ::send(c, resp.data(), resp.size(), 0);
            if (stall) {
                // Outlive the device's deadline without ever sending EOF.
                while (!stopping.load()) {
                    struct timespec ts{0, 20 * 1000 * 1000};
                    ::nanosleep(&ts, nullptr);
                }
            }
            ::close(c);
            done.store(true);
        });
        return true;
    }

    void stop()
    {
        stopping.store(true);
        if (listenFd >= 0) { ::shutdown(listenFd, SHUT_RDWR); ::close(listenFd); listenFd = -1; }
        if (th.joinable()) th.join();
    }
};

}  // namespace

int main()
{
    // ── A fetch, end to end ───────────────────────────────────────────────
    {
        HttpStub stub;
        stub.body = "<html><body>Hello Apple II</body></html>";
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        const std::string spec = "N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/index.html";
        check(net.open(spec), "opens N:HTTP://host:port/path");
        check(net.isOpen(), "reports itself open");

        // Only the body, never the headers.
        std::vector<uint8_t> got(net.available());
        const std::size_t n = net.read(got.data(), got.size());
        const std::string text(reinterpret_cast<const char*>(got.data()), n);
        check(text == stub.body, "hands the guest the BODY, headers stripped");

        // The request must carry a Host: header and the path as typed —
        // lower-casing the path would break case-sensitive servers.
        check(stub.seenRequest.find("GET /index.html ") != std::string::npos,
              "requests the path exactly as given");
        check(stub.seenRequest.find("Host: 127.0.0.1") != std::string::npos,
              "sends a Host: header");

        // Drained: connected must now be false, or a guest read loop spins.
        uint8_t st[4];
        net.status(st);
        check(st[0] == 0 && st[1] == 0, "no bytes waiting once drained");
        check(st[2] == 0, "connected clears when the body runs out");
        net.close();
        stub.stop();
    }

    // ── The 512-byte cap, and reading across it ───────────────────────────
    {
        HttpStub stub;
        stub.body.assign(1300, 'x');
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        check(net.open("N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/"),
              "opens a larger document");
        check(net.available() == 1300, "buffers the whole body");

        uint8_t st[4];
        net.status(st);
        const uint16_t avail = static_cast<uint16_t>(st[0] | (st[1] << 8));
        check(avail == pom2::FujiNetNetDevice::kMaxStatusAvail,
              "status caps the announced count at 512");
        check(st[2] == 1, "connected while bytes remain");

        std::vector<uint8_t> all;
        uint8_t chunk[512];
        for (;;) {
            const std::size_t k = net.read(chunk, sizeof chunk);
            if (!k) break;
            all.insert(all.end(), chunk, chunk + k);
        }
        check(all.size() == 1300, "reads reassemble across the cap");
        check(std::string(all.begin(), all.end()) == stub.body, "content survives");
        stub.stop();
    }

    // ── Specs it must refuse rather than mangle ───────────────────────────
    {
        pom2::FujiNetNetDevice net;
        check(!net.open("N:HTTPS://example.invalid/"),
              "refuses https (no TLS here) instead of pretending");
        check(!net.open("N:FTP://example.invalid/"), "refuses an unknown scheme");
        check(!net.open("N:"), "refuses an empty spec");
    }

    // ── A stalled server must not become a half page ──────────────────────
    //
    // This runs on the CPU thread with the emulator's state mutex held, so the
    // fetch carries ONE deadline for the whole exchange. Two things are pinned
    // here: the deadline is honoured at all (a per-recv timeout never bounds a
    // server that drip-feeds just inside it), and what comes out is an ERROR —
    // never the bytes that did arrive. A truncated document the guest cannot
    // tell from a whole one is the failure nobody can diagnose from the Apple
    // II side.
    {
        HttpStub stub;
        stub.stall = true;
        stub.body.assign(4000, 'y');
        if (!stub.start()) { std::printf("FAIL: cannot start the HTTP stub\n"); return 1; }

        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(600);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = net.open("N:HTTP://127.0.0.1:" + std::to_string(stub.port) + "/");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

        check(!ok, "a stalled server fails the open instead of succeeding short");
        check(ms < 5000, "the whole fetch is bounded by its deadline");
        check(net.available() == 0, "no partial body is left reachable after a failure");
        check(!net.isOpen(), "and the device does not report itself open");
        stub.stop();
    }

    // ── A blackholed host must not freeze the machine ─────────────────────
    //
    // 192.0.2.1 is TEST-NET-1: it swallows SYNs rather than refusing them.
    // SO_SNDTIMEO does NOT bound connect() — measured 2026-08-21 on macOS, a
    // connect asking for 8 s returned after 75. With the state mutex held that
    // is 75 s of frozen emulator, so the connect now gets an explicit wait.
    {
        pom2::FujiNetNetDevice net;
        net.setFetchDeadlineMs(800);
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = net.open("N:HTTP://192.0.2.1/");
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        check(!ok, "an unreachable host fails rather than hanging");
        // Generous on purpose: a host that RSTs instead of dropping fails
        // instantly and passes too. What must never happen is the OS default.
        check(ms < 30000, "a blackholed host is bounded by the deadline, not by the OS");
        if (ms >= 30000) std::printf("      (took %lld ms)\n", (long long)ms);
    }

    if (failures) { std::printf("fujinet_net_device: %d failure(s)\n", failures); return 2; }
    std::printf("fujinet_net_device OK\n");
    return 0;
}
