// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "W5100HostSockets.h"

#include "Logger.h"
#include "Pom2Build.h"
#include "SocketCompat.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#if POM2_HAS_SOCKETS
#include <chrono>
#include <future>
#include <thread>
#endif

namespace pom2 {
namespace {

#if POM2_HAS_SOCKETS

uint32_t hostByName(const std::string& name)
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &result) != 0 || !result)
        return 0;

    uint32_t address = 0;
    for (addrinfo* p = result; p; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr) continue;
        sockaddr_in sa{};
        std::memcpy(&sa, p->ai_addr,
                    std::min(sizeof(sa), static_cast<size_t>(p->ai_addrlen)));
        address = sa.sin_addr.s_addr;
        break;
    }
    freeaddrinfo(result);
    return address;
}

class NativeW5100Socket final : public W5100HostSocket
{
public:
    NativeW5100Socket(socket_t fd, W5100SocketKind kind)
        : fd_(fd), kind_(kind) {}

    ~NativeW5100Socket() override { closeHostSocket(fd_); }

    W5100ConnectResult connect(uint32_t address, uint16_t port) override
    {
        sockaddr_in destination{};
        destination.sin_family      = AF_INET;
        destination.sin_addr.s_addr = address;
        destination.sin_port        = port;

        const int res = ::connect(fd_,
            reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
        if (res == 0) return W5100ConnectResult::Connected;
        const int e = lastSocketError();
        return errInProgress(e) ? W5100ConnectResult::InProgress
                                : W5100ConnectResult::Failed;
    }

    W5100ConnectResult pollConnect() override
    {
        const WaitResult readiness = waitSocket(fd_, SocketWait::Write, 0);
        if (readiness == WaitResult::Timeout)
            return W5100ConnectResult::InProgress;
        if (readiness == WaitResult::Failed)
            return W5100ConnectResult::Failed;
        return connectResult(fd_) == 0 ? W5100ConnectResult::Connected
                                       : W5100ConnectResult::Failed;
    }

    W5100ReceiveResult receive(uint8_t* data, std::size_t capacity) override
    {
        sockaddr_in source{};
        socklen_c sourceLen = sizeof(source);
        const iolen_t got = ::recvfrom(fd_, reinterpret_cast<char*>(data),
            static_cast<int>(capacity), 0,
            reinterpret_cast<sockaddr*>(&source), &sourceLen);

        if (got >= 0) {
            if (got == 0 && kind_ == W5100SocketKind::Tcp)
                return { W5100IoStatus::Closed, 0, 0, 0 };
            return { W5100IoStatus::Ok, static_cast<std::size_t>(got),
                     source.sin_addr.s_addr, source.sin_port };
        }

        const int e = lastSocketError();
        if (errWouldBlock(e) || errInterrupted(e))
            return { W5100IoStatus::WouldBlock, 0, 0, 0 };
        if (kind_ == W5100SocketKind::Udp && errDatagramDiscard(e))
            return { W5100IoStatus::Discarded, 0, 0, 0 };
        return { W5100IoStatus::Failed, 0, 0, 0 };
    }

    W5100SendResult send(const uint8_t* data, std::size_t size,
                         uint32_t address, uint16_t port) override
    {
        iolen_t sent = -1;
        if (kind_ == W5100SocketKind::Udp) {
            sockaddr_in destination{};
            destination.sin_family      = AF_INET;
            destination.sin_addr.s_addr = address;
            destination.sin_port        = port;
            sent = ::sendto(fd_, reinterpret_cast<const char*>(data),
                            static_cast<int>(size), 0,
                            reinterpret_cast<const sockaddr*>(&destination),
                            sizeof(destination));
        } else {
            sent = sendSocket(fd_, data, size);
        }

        if (sent >= 0)
            return { W5100IoStatus::Ok, static_cast<std::size_t>(sent) };
        const int e = lastSocketError();
        if (errWouldBlock(e) || errInterrupted(e))
            return { W5100IoStatus::WouldBlock, 0 };
        return { W5100IoStatus::Failed, 0 };
    }

private:
    socket_t         fd_ = kInvalidSocket;
    W5100SocketKind  kind_;
};

#endif // POM2_HAS_SOCKETS

struct PendingDns {
    std::string name;
    uint32_t    address = 0;
};

struct DnsMailbox {
    std::mutex              mutex;
    std::vector<PendingDns> pending;
    int                     inFlight = 0;
};

class NativeW5100SocketFactory final : public W5100SocketFactory
{
public:
    std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) override
    {
#if !POM2_HAS_SOCKETS
        (void)kind;
        log().warn("W5100", "TCP/UDP sockets are unavailable in this build");
        return {};
#else
        ensureSocketStack();
        const int type = kind == W5100SocketKind::Tcp ? kSockStream : kSockDgram;
        const int protocol = kind == W5100SocketKind::Tcp ? kIpProtoTcp : kIpProtoUdp;
        socket_t fd = ::socket(AF_INET, type, protocol);
        if (!isValidSocket(fd)) {
            log().warn("W5100", "socket() failed: " + lastSocketErrorText());
            return {};
        }
        if (!setNonBlocking(fd)) {
            log().warn("W5100", "non-blocking mode failed: " +
                                 lastSocketErrorText());
            closeHostSocket(fd);
            return {};
        }
        if (kind == W5100SocketKind::Udp) disableUdpConnReset(fd);
        return std::make_unique<NativeW5100Socket>(fd, kind);
#endif
    }

    uint32_t resolveHostname(const std::string& name, int waitMs) override
    {
        pollResolver();
        const auto cached = dnsCache_.find(name);
        if (cached != dnsCache_.end()) return cached->second;

#if !POM2_HAS_SOCKETS
        (void)waitMs;
        log().warn("W5100", "virtual DNS is unavailable in this build");
        return 0;
#else
        {
            std::lock_guard<std::mutex> lk(mailbox_->mutex);
            constexpr int kMaxDnsInFlight = 8;
            if (mailbox_->inFlight >= kMaxDnsInFlight) {
                log().warn("W5100", "too many DNS lookups in flight — '" +
                                    name + "' not attempted");
                return 0;
            }
        }

        auto future = std::async(std::launch::async,
                                 [name]() { return hostByName(name); });
        if (future.wait_for(std::chrono::milliseconds(waitMs)) ==
            std::future_status::ready) {
            const uint32_t resolved = future.get();
            storeDns(name, resolved);
            return resolved;
        }

        auto shared = std::make_shared<std::future<uint32_t>>(std::move(future));
        auto mailbox = mailbox_;
        {
            std::lock_guard<std::mutex> lk(mailbox->mutex);
            ++mailbox->inFlight;
        }
        std::thread([name, shared, mailbox]() {
            const uint32_t late = shared->get();
            std::lock_guard<std::mutex> lk(mailbox->mutex);
            mailbox->pending.push_back({ name, late });
            --mailbox->inFlight;
        }).detach();
        log().info("W5100", "DNS lookup for '" + name +
                            "' still in flight — retry the connection");
        return 0;
#endif
    }

    void pollResolver() override
    {
        std::vector<PendingDns> ready;
        {
            std::lock_guard<std::mutex> lk(mailbox_->mutex);
            if (mailbox_->pending.empty()) return;
            ready.swap(mailbox_->pending);
        }
        for (const PendingDns& p : ready) storeDns(p.name, p.address);
    }

    void clearResolverCache() override { dnsCache_.clear(); }

private:
    void storeDns(const std::string& name, uint32_t address)
    {
        if (dnsCache_.size() >= 512) dnsCache_.clear();
        dnsCache_[name] = address;
        if (address == 0)
            log().warn("W5100", "could not resolve '" + name + "'");
    }

    std::map<std::string, uint32_t> dnsCache_;
    std::shared_ptr<DnsMailbox> mailbox_ = std::make_shared<DnsMailbox>();
};

} // namespace

std::unique_ptr<W5100SocketFactory> makeW5100HostSocketFactory()
{
    return std::make_unique<NativeW5100SocketFactory>();
}

} // namespace pom2
