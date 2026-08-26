// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SuperSerialTcpTransport.h"

#include "Logger.h"
#include "Pom2Build.h"
#include "SocketCompat.h"
#include "SocketUtil.h"
#include "SuperSerialCard.h"
#include "SuperSerialTransport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pom2 {
namespace {

constexpr size_t kBufferCapacity = 4096;

class SuperSerialTcpTransport final : public SuperSerialTransport
{
public:
    explicit SuperSerialTcpTransport(SuperSerialCard& card) : card_(card) {}
    ~SuperSerialTcpTransport() override { stop(); }

    bool start(uint16_t newPort) override
    {
#if !POM2_HAS_SOCKETS
        port_ = newPort;
        log().info("SSC", "telnet listener disabled in WASM build");
        return false;
#else
        if (listening_ && newPort == port_ && worker_.joinable()) return true;
        stop();
        port_ = newPort;

        ensureSocketStack();
        const socket_t lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        listenFd_ = lfd;
        if (!isValidSocket(lfd)) {
            log().warn("SSC", "socket() failed: " + lastSocketErrorText());
            return false;
        }

        setListenerBindPolicy(lfd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port_);
        if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            log().warn("SSC", "bind 127.0.0.1:" + std::to_string(port_) +
                " failed: " + lastSocketErrorText());
            closeHostSocketValue(lfd);
            listenFd_ = kInvalidSocket;
            return false;
        }
        if (port_ == 0) {
            sockaddr_in bound{};
            socklen_c boundLen = sizeof(bound);
            if (::getsockname(lfd, reinterpret_cast<sockaddr*>(&bound),
                              &boundLen) != 0) {
                log().warn("SSC", "getsockname() failed: " +
                    lastSocketErrorText());
                closeHostSocketValue(lfd);
                listenFd_ = kInvalidSocket;
                return false;
            }
            port_ = ntohs(bound.sin_port);
        }
        if (::listen(lfd, 1) != 0) {
            log().warn("SSC", "listen() failed: " + lastSocketErrorText());
            closeHostSocketValue(lfd);
            listenFd_ = kInvalidSocket;
            return false;
        }

        stopRequested_ = false;
        pacingReset_ = true;
        listening_ = true;
        worker_ = std::thread(&SuperSerialTcpTransport::run, this);
        log().info("SSC", "listening on 127.0.0.1:" +
            std::to_string(port_) + " (telnet to connect to slot " +
            std::to_string(card_.getSlot()) + ")");
        return true;
#endif
    }

    void stop() override
    {
#if !POM2_HAS_SOCKETS
        listening_ = false;
        card_.setTransportConnected(false);
#else
        if (!listening_ && !worker_.joinable()) {
            card_.setTransportConnected(false);
            return;
        }
        stopRequested_ = true;
        {
            std::lock_guard<std::mutex> life(fdLifeMtx_);
            shutdownBoth(clientFd_.load());
            shutdownBoth(listenFd_.load());
        }
        if (worker_.joinable()) worker_.join();
        {
            std::lock_guard<std::mutex> life(fdLifeMtx_);
            closeHostSocketValue(listenFd_.exchange(kInvalidSocket));
            closeHostSocketValue(clientFd_.exchange(kInvalidSocket));
        }
        listening_ = false;
        card_.setTransportConnected(false);
#endif
    }

    bool isListening() const override { return listening_; }
    uint16_t port() const override { return port_; }
    void resetPacing() override { pacingReset_ = true; }

private:
#if POM2_HAS_SOCKETS
    void closeClient()
    {
        std::lock_guard<std::mutex> life(fdLifeMtx_);
        const socket_t fd = clientFd_.exchange(kInvalidSocket);
        if (isValidSocket(fd)) {
            shutdownBoth(fd);
            closeHostSocketValue(fd);
        }
        card_.setTransportConnected(false);
    }

    void run()
    {
        while (!stopRequested_) {
            sockaddr_in peer{};
            socket_t fd = kInvalidSocket;
            const auto accepted = pollAcceptOnce(listenFd_.load(), 200, fd, peer);
            if (accepted == PollAccept::Retry) continue;
            if (accepted == PollAccept::Shutdown) break;

            disableSigpipe(fd);
            setSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            setNonBlocking(fd);
            {
                std::lock_guard<std::mutex> life(fdLifeMtx_);
                if (stopRequested_) {
                    closeHostSocketValue(fd);
                    break;
                }
                clientFd_ = fd;
            }

            card_.resetTelnet();
            card_.setTransportConnected(true);
            pacingReset_ = true;
            log().info("SSC", "client connected from " + peerAddressText(peer));

            std::vector<uint8_t> pendingOut;
            std::array<uint8_t, kBufferCapacity> scratch{};
            while (!stopRequested_ && isValidSocket(clientFd_.load())) {
                const bool raw = card_.rawMode();
                const iolen_t got = recvSocket(
                    clientFd_.load(), scratch.data(), scratch.size());
                if (got > 0) {
                    size_t n = static_cast<size_t>(got);
                    if (!raw) n = card_.processTransportTextRx(scratch.data(), n);
                    if (n > 0)
                        card_.deliverTransportBytes(scratch.data(), n, !raw);
                } else if (got == 0) {
                    log().info("SSC", "client disconnected");
                    break;
                } else if (!errWouldBlock(lastSocketError())) {
                    log().info("SSC", "recv error: " + lastSocketErrorText());
                    break;
                }

                if (pendingOut.empty()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (pacingReset_.exchange(false)) {
                        sendBudget_ = 0.0;
                        lastDrainTime_ = now;
                    }

                    size_t capacity = scratch.size();
                    const double bytesPerSecond = card_.bytesPerSecond();
                    if (bytesPerSecond > 0.0) {
                        const double elapsed = std::chrono::duration<double>(
                            now - lastDrainTime_).count();
                        sendBudget_ = std::min(
                            static_cast<double>(kBufferCapacity),
                            sendBudget_ + elapsed * bytesPerSecond);
                        capacity = std::min(capacity,
                            static_cast<size_t>(sendBudget_));
                    }

                    const size_t drained = card_.drainTransportTx(
                        scratch.data(), capacity);
                    if (bytesPerSecond > 0.0)
                        sendBudget_ -= static_cast<double>(drained);
                    lastDrainTime_ = now;

                    pendingOut.reserve(drained * 2);
                    for (size_t i = 0; i < drained; ++i) {
                        if (raw) pendingOut.push_back(scratch[i]);
                        else SuperSerialCard::appendTelnetTxEscaped(
                            pendingOut, scratch[i]);
                    }
                }

                if (!pendingOut.empty() && isValidSocket(clientFd_.load())) {
                    const iolen_t sent = sendNoSignal(
                        clientFd_.load(), pendingOut.data(), pendingOut.size());
                    const int sendError = sent < 0 ? lastSocketError() : 0;
                    if (sent > 0) {
                        pendingOut.erase(pendingOut.begin(),
                            pendingOut.begin() + sent);
                    } else if (sent < 0 &&
                               (errWouldBlock(sendError) ||
                                errInterrupted(sendError))) {
                        // Backpressure: retain pendingOut for the next pass.
                    } else {
                        log().info("SSC", "send error: " +
                            socketErrorText(sendError));
                        break;
                    }
                }

                std::this_thread::sleep_for(std::chrono::microseconds(2000));
            }
            closeClient();
        }
        listening_ = false;
        card_.setTransportConnected(false);
    }
#endif

    SuperSerialCard& card_;
    std::atomic<bool> listening_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> pacingReset_{true};
    uint16_t port_ = SuperSerialCard::kDefaultPort;
    double sendBudget_ = 0.0;
    std::chrono::steady_clock::time_point lastDrainTime_ =
        std::chrono::steady_clock::now();

#if POM2_HAS_SOCKETS
    std::atomic<socket_t> listenFd_{kInvalidSocket};
    std::atomic<socket_t> clientFd_{kInvalidSocket};
    mutable std::mutex fdLifeMtx_;
    std::thread worker_;
#endif
};

} // namespace

std::unique_ptr<SuperSerialTransport>
makeSuperSerialTcpTransport(SuperSerialCard& card)
{
    return std::make_unique<SuperSerialTcpTransport>(card);
}

} // namespace pom2
