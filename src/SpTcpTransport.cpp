// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpTcpTransport — SP-over-SLIP across a loopback TCP connection.
//
// POM2 LISTENS and the FujiNet connects in, which is the direction both
// reference implementations use: the FujiNet AppleWin fork's
// `devrelay/service/Listener.cpp` ("Creates a Connection object, which is how
// SP device(s) will register itself with our listener") and
// `fujinet-go-apple2-desktop`, which joins its embedded firmware to the
// emulator over loopback TCP 1985. Keeping that direction and that port means
// an existing FujiNet configuration works against POM2 with nothing changed.
//
// Binds 127.0.0.1 only. The peer is a program on the same machine; exposing
// a SmartPort bus — which can read and write the guest's disks — to the LAN
// is not something a user should be able to do by accident.

#include "SpTransport.h"

#include "Logger.h"
#include "SocketUtil.h"

#include <cstring>

namespace pom2 {

SpTcpTransport::SpTcpTransport(uint16_t port) : port_(port) {}

SpTcpTransport::~SpTcpTransport()
{
    dropPeer();
    stopListening();
}

#if !POM2_HAS_SOCKETS

// Emscripten: no host sockets at all. The card still constructs and plugs so
// the rest of the machine is unaffected; it simply never finds a peer.
bool SpTcpTransport::startListening(std::string& errOut)
{ errOut = "host sockets are not available in this build"; return false; }
void SpTcpTransport::stopListening() {}
bool SpTcpTransport::isListening() const { return false; }
bool SpTcpTransport::isOpen() const { return false; }
bool SpTcpTransport::pollForPeer(int) { return false; }
bool SpTcpTransport::writeAll(const uint8_t*, std::size_t) { return false; }
int  SpTcpTransport::readSome(uint8_t*, std::size_t, int) { return -1; }
void SpTcpTransport::dropPeer() {}
void SpTcpTransport::shutdown() {}
std::string SpTcpTransport::describe() const { return "TCP unavailable in this build"; }

#else

bool SpTcpTransport::startListening(std::string& errOut)
{
    if (isListening()) return true;
    ensureSocketStack();

    const socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!isValidSocket(fd)) {
        errOut = "socket(): " + lastSocketErrorText();
        return false;
    }
    // NOT raw SO_REUSEADDR — on Winsock that would let any local process
    // take the port from under us while we are listening. See SocketCompat.h
    // trap 6.
    setListenerBindPolicy(fd);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = hostToNet16(port_);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);   // loopback ONLY

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        errOut = "bind(127.0.0.1:" + std::to_string(port_) + "): " +
                 lastSocketErrorText();
        socket_t tmp = fd;
        closeHostSocket(tmp);
        return false;
    }
    if (::listen(fd, 2) != 0) {
        errOut = "listen(): " + lastSocketErrorText();
        socket_t tmp = fd;
        closeHostSocket(tmp);
        return false;
    }

    listenFd_.store(fd);
    log().info("FujiNet", "listening for SP-over-SLIP on 127.0.0.1:" +
                              std::to_string(port_));
    return true;
}

void SpTcpTransport::stopListening()
{
    // Contract: the worker must already be joined. Closing a listening
    // descriptor a thread is parked in accept() on is the use-after-close
    // race SocketUtil.h rule 1 exists to prevent.
    socket_t fd = listenFd_.exchange(kInvalidSocket);
    closeHostSocketValue(fd);
}

bool SpTcpTransport::isListening() const
{ return isValidSocket(listenFd_.load()); }

bool SpTcpTransport::isOpen() const
{ return isValidSocket(clientFd_.load()); }

bool SpTcpTransport::pollForPeer(int timeoutMs)
{
    const socket_t lfd = listenFd_.load();
    if (!isValidSocket(lfd)) return false;

    socket_t    fd   = kInvalidSocket;
    sockaddr_in peer{};
    // poll-then-accept, never a bare blocking accept: shutdown() must be able
    // to wake this on macOS/BSD too, where shutdown() on a listener does not
    // unblock accept().
    switch (pollAcceptOnce(lfd, timeoutMs, fd, peer)) {
    case PollAccept::Retry:    return false;
    case PollAccept::Shutdown: return false;
    case PollAccept::Accepted: break;
    }

    if (isOpen()) {
        // One peer at a time — two would mean two SmartPort device-number
        // spaces to merge, which no real configuration needs. Refuse the
        // newcomer rather than silently swapping the live link out from
        // under an in-flight call.
        log().warn("FujiNet", "second SP-over-SLIP peer refused (already "
                              "connected to " + describe() + ")");
        closeHostSocketValue(fd);
        return false;
    }

    disableSigpipe(fd);
    // SmartPort traffic is small request/response packets and the emulated
    // 6502 is stalled waiting for each one, so Nagle's delayed-ACK pairing
    // would add up to 40 ms to every single disk block.
    setSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        clientFd_.store(fd);
        peerText_ = peerAddressText(peer) + ":" +
                    std::to_string(netToHost16(peer.sin_port));
    }
    log().info("FujiNet", "SP-over-SLIP peer connected from " + describe());
    return true;
}

bool SpTcpTransport::writeAll(const uint8_t* p, std::size_t n)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const socket_t fd = clientFd_.load();
    if (!isValidSocket(fd)) return false;

    std::size_t sent = 0;
    while (sent < n) {
        const iolen_t w = sendNoSignal(fd, p + sent, n - sent);
        if (w > 0) { sent += static_cast<std::size_t>(w); continue; }
        const int e = lastSocketError();
        if (errInterrupted(e)) continue;
        if (errWouldBlock(e)) {
            // The socket is blocking, so this is rare; wait for room rather
            // than spinning.
            if (waitSocket(fd, SocketWait::Write, 250) == WaitResult::Ready)
                continue;
            return false;
        }
        return false;
    }
    return true;
}

int SpTcpTransport::readSome(uint8_t* p, std::size_t n, int timeoutMs)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const socket_t fd = clientFd_.load();
    if (!isValidSocket(fd)) return -1;

    switch (waitSocket(fd, SocketWait::Read, timeoutMs)) {
    case WaitResult::Timeout: return 0;
    case WaitResult::Failed:  return -1;
    case WaitResult::Ready:   break;
    }

    const iolen_t got = recvSocket(fd, p, n);
    if (got > 0) return static_cast<int>(got);
    if (got == 0) return -1;                 // orderly close = peer gone
    const int e = lastSocketError();
    if (errInterrupted(e) || errWouldBlock(e)) return 0;
    return -1;
}

void SpTcpTransport::dropPeer()
{
    socket_t fd = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        fd = clientFd_.exchange(kInvalidSocket);
        peerText_.clear();
    }
    if (isValidSocket(fd)) {
        closeHostSocketValue(fd);
        log().info("FujiNet", "SP-over-SLIP peer disconnected");
    }
}

void SpTcpTransport::shutdown()
{
    // Wake, do not close: a descriptor closed under a thread parked in
    // recv()/accept() can be recycled by the next socket() on another thread.
    shutdownBoth(listenFd_.load());
    shutdownBoth(clientFd_.load());
}

std::string SpTcpTransport::describe() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!peerText_.empty()) return peerText_;
    if (isValidSocket(listenFd_.load()))
        return "listening on 127.0.0.1:" + std::to_string(port_);
    return "idle";
}

#endif // POM2_HAS_SOCKETS

} // namespace pom2
