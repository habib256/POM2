// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
//
// Small shared socket idioms for the two TCP workers (AiControlServer HTTP +
// SuperSerialTcpTransport telnet bridge). Two hard-won rules live here so
// they are fixed ONCE:
//
//   1. poll()-before-accept: `shutdown(listenFd, SHUT_RDWR)` wakes a
//      blocking accept() on Linux but NOT on macOS/BSD — a worker parked
//      in accept() there never returns and the owner hangs in join().
//      This deadlock was found and fixed in AiControlServer, then
//      re-discovered months later in the SSC TCP transport (whose shutdown
//      runs under stateMutex during profile switches — a UI-thread hang).
//      Windows is a third variant of the same problem: closesocket() on a
//      listener does wake a blocked accept(), but the handle is then free
//      to be recycled by the next socket() on another thread, so the
//      wait-then-accept shape is what makes the shutdown sequence safe
//      everywhere rather than on Linux only.
//
//   2. SIGPIPE-proof send: a peer that vanished uncleanly must surface as
//      an error return, not kill the process. Linux spells that
//      MSG_NOSIGNAL; macOS/BSD have no such flag — there the socket needs
//      SO_NOSIGPIPE set once after accept (disableSigpipe). Windows has no
//      SIGPIPE at all, so both are no-ops there.
//
// The POSIX-vs-Winsock differences themselves are NOT here — they live in
// SocketCompat.h, which this header is written against. Not used by the
// Emscripten build (no sockets there); both includers guard their network
// paths with #if POM2_HAS_SOCKETS.

#ifndef POM2_SOCKET_UTIL_H
#include "Pom2Build.h"
#define POM2_SOCKET_UTIL_H

#if POM2_HAS_SOCKETS

#include "SocketCompat.h"

#include <cstddef>

namespace pom2 {

enum class PollAccept {
    Accepted,   ///< outFd holds a connected client
    Retry,      ///< timeout / EINTR / EAGAIN — re-check your stop flag, loop
    Shutdown,   ///< listen fd gone (owner shut it down) — exit the worker
};

/// One iteration of the wait-then-accept idiom (rule 1 above). Waits on
/// `listenFd` for up to `timeoutMs`, then accepts. The caller loops on
/// Retry so its stop flag is re-checked at least every `timeoutMs`.
inline PollAccept pollAcceptOnce(socket_t listenFd, int timeoutMs,
                                 socket_t& outFd, sockaddr_in& peer)
{
    outFd = kInvalidSocket;
    if (!isValidSocket(listenFd)) return PollAccept::Shutdown;

    switch (waitSocket(listenFd, SocketWait::Read, timeoutMs)) {
    case WaitResult::Timeout:  return PollAccept::Retry;
    case WaitResult::Failed:   return PollAccept::Shutdown;
    case WaitResult::Ready:    break;
    }

    socklen_c peerLen = sizeof(peer);
    const socket_t fd = ::accept(listenFd,
                                 reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (!isValidSocket(fd)) {
        const int e = lastSocketError();
        if (errInterrupted(e) || errWouldBlock(e)) return PollAccept::Retry;
        return PollAccept::Shutdown;   // listening socket closed under us
    }
    outFd = fd;
    return PollAccept::Accepted;
}

/// Arm rule 2 on a fresh client socket. No-op where MSG_NOSIGNAL covers it
/// (Linux) or where SIGPIPE does not exist (Windows).
inline void disableSigpipe(socket_t fd)
{
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

/// send() that cannot raise SIGPIPE (rule 2). Pair with disableSigpipe()
/// on platforms without MSG_NOSIGNAL. Thin alias kept so the two workers
/// read the same as before the Winsock port.
inline iolen_t sendNoSignal(socket_t fd, const void* buf, std::size_t n)
{
    return sendSocket(fd, buf, n);
}

} // namespace pom2

#endif // POM2_HAS_SOCKETS
#endif // POM2_SOCKET_UTIL_H
