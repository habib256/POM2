// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
//
// Small shared POSIX-socket idioms for the two TCP workers (AiControlServer
// HTTP + SuperSerialCard telnet bridge). Two hard-won rules live here so
// they are fixed ONCE:
//
//   1. poll()-before-accept: `shutdown(listenFd, SHUT_RDWR)` wakes a
//      blocking accept() on Linux but NOT on macOS/BSD — a worker parked
//      in accept() there never returns and the owner hangs in join().
//      This deadlock was found and fixed in AiControlServer, then
//      re-discovered months later in SuperSerialCard (whose destructor
//      runs under stateMutex during profile switches — a UI-thread hang).
//
//   2. SIGPIPE-proof send: a peer that vanished uncleanly must surface as
//      an error return, not kill the process. Linux spells that
//      MSG_NOSIGNAL; macOS/BSD have no such flag — there the socket needs
//      SO_NOSIGPIPE set once after accept (disableSigpipe).
//
// Not used by the Emscripten build (no sockets there); both includers
// guard their network paths with #ifndef __EMSCRIPTEN__.

#ifndef POM2_SOCKET_UTIL_H
#define POM2_SOCKET_UTIL_H

#ifndef __EMSCRIPTEN__

#include <cerrno>
#include <cstddef>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace pom2 {

enum class PollAccept {
    Accepted,   ///< outFd holds a connected client
    Retry,      ///< timeout / EINTR / EAGAIN — re-check your stop flag, loop
    Shutdown,   ///< listen fd gone (owner shut it down) — exit the worker
};

/// One iteration of the poll-then-accept idiom (rule 1 above). Polls
/// `listenFd` for up to `timeoutMs`, then accepts. The caller loops on
/// Retry so its stop flag is re-checked at least every `timeoutMs`.
inline PollAccept pollAcceptOnce(int listenFd, int timeoutMs,
                                 int& outFd, sockaddr_in& peer)
{
    outFd = -1;
    if (listenFd < 0) return PollAccept::Shutdown;

    struct pollfd pfd{};
    pfd.fd     = listenFd;
    pfd.events = POLLIN;
    const int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr < 0)  return (errno == EINTR) ? PollAccept::Retry
                                         : PollAccept::Shutdown;
    if (pr == 0) return PollAccept::Retry;                    // timeout
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return PollAccept::Shutdown;   // fd invalidated under us

    socklen_t peerLen = sizeof(peer);
    const int fd = ::accept(listenFd,
                            reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (fd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return PollAccept::Retry;
        return PollAccept::Shutdown;   // listening socket closed under us
    }
    outFd = fd;
    return PollAccept::Accepted;
}

/// Arm rule 2 on a fresh client socket. No-op where MSG_NOSIGNAL covers it.
inline void disableSigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

/// send() that cannot raise SIGPIPE (rule 2). Pair with disableSigpipe()
/// on platforms without MSG_NOSIGNAL.
inline ssize_t sendNoSignal(int fd, const void* buf, size_t n)
{
#ifdef MSG_NOSIGNAL
    return ::send(fd, buf, n, MSG_NOSIGNAL);
#else
    return ::send(fd, buf, n, 0);
#endif
}

} // namespace pom2

#endif // !__EMSCRIPTEN__
#endif // POM2_SOCKET_UTIL_H
