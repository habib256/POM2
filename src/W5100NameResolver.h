// POM2 Apple II Emulator
// Copyright (C) 2026
//
// W5100NameResolver — the virtual-DNS half of the Uthernet II.
//
// The real W5100 has no name resolution; POM2 adds it so a guest can write a
// hostname into the socket's DNS registers and get an address back
// (`Uthernet2.cpp:1012-1037`). That is a HOST lookup, and host lookups block:
// the whole design here exists to keep a slow or dead resolver from stalling
// the CPU thread, which calls this under stateMutex.
//
// Three rules, and each one is load-bearing:
//   * the lookup runs off-thread with a BOUNDED wait. A timed-out lookup is
//     not cancelled — it parks its answer in a mailbox that poll() drains on
//     the CPU thread, so the cache stays single-threaded.
//   * in-flight lookups are CAPPED. A guest looping OPEN over random
//     hostnames would otherwise pile up resolver threads without bound.
//   * the mailbox is shared by shared_ptr, so a lookup that outlives the card
//     (the user pulled it out of the slot mid-resolve) has nowhere unsafe to
//     write.
//
// The lookup itself is injectable so a test can resolve deterministically,
// with no network and no waiting.

#ifndef POM2_W5100_NAME_RESOLVER_H
#define POM2_W5100_NAME_RESOLVER_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pom2 {

class W5100NameResolver
{
public:
    /// Blocking host lookup, returning an address in network byte order or 0.
    /// Runs on a resolver thread, never on the caller's.
    using LookupFn = std::function<uint32_t(const std::string&)>;

    /// An empty lookup selects the production getaddrinfo adapter.
    explicit W5100NameResolver(LookupFn lookup = {});

    W5100NameResolver(const W5100NameResolver&) = delete;
    W5100NameResolver& operator=(const W5100NameResolver&) = delete;

    enum class Status : std::uint8_t {
        Resolved,   ///< `address` is usable now (cached, or answered in time)
        Pending,    ///< timed out; the answer may arrive via poll() later
        Refused,    ///< too many lookups already in flight
        Failed,     ///< the resolver answered, and the answer was "no"
    };

    struct Result {
        Status   status  = Status::Failed;
        uint32_t address = 0;   ///< network byte order
    };

    /// Resolve within `waitMs`. Answers from the cache without waiting.
    Result resolve(const std::string& name, int waitMs);

    /// Fold any late answers into the cache. CPU thread only — that is what
    /// keeps the cache free of a lock.
    void poll();

    void clearCache();

    std::size_t cacheSize() const { return cache_.size(); }

    /// Bound on concurrent resolver threads, and on the cache.
    static constexpr int         kMaxInFlight = 8;
    static constexpr std::size_t kMaxCache    = 512;

private:
    struct Pending {
        std::string name;
        uint32_t    address = 0;
    };

    /// Written by detached resolver threads, drained by poll().
    struct Mailbox {
        std::mutex           mutex;
        std::vector<Pending> pending;
        int                  inFlight = 0;
    };

    LookupFn                        lookup_;
    std::map<std::string, uint32_t> cache_;
    std::shared_ptr<Mailbox>        mailbox_ = std::make_shared<Mailbox>();
};

} // namespace pom2

#endif // POM2_W5100_NAME_RESOLVER_H
