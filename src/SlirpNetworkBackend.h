// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlirpNetworkBackend — user-mode NAT for POM2's Ethernet cards, built on
// libslirp (the TCP/IP stack QEMU's `-net user` uses).
//
// Why slirp and not TAP/pcap
// --------------------------
// The CS8900A (Uthernet I) is a raw NIC: the Apple-side software carries
// its own stack, so the host has to bridge whole Ethernet frames. The two
// classic ways to do that — a TAP device or libpcap on a real interface —
// both need root (CAP_NET_ADMIN / CAP_NET_RAW). libslirp instead
// terminates the guest's IP inside our process and re-opens ordinary
// user-space sockets to the outside world: no privileges, no host
// configuration, works identically in CI. The cost is slirp's known
// limits — no inbound connections unless port-forwarded, no ICMP unless
// the host allows unprivileged ping sockets, and the guest cannot be
// reached from the LAN.
//
// Virtual network (libslirp defaults, same as QEMU)
// ------------------------------------------------
//   10.0.2.0/24     the virtual network
//   10.0.2.2        the virtual router / gateway (the host)
//   10.0.2.3        the virtual DNS server
//   10.0.2.15       what the DHCP server hands out — configure IP65 /
//                   Contiki with this if you skip DHCP
//
// Build gate: this file only compiles to something functional when
// POM2_HAVE_SLIRP is defined (CMake sets it when pkg-config finds
// `slirp`). Otherwise `create()` returns nullptr and callers fall back to
// NullNetworkBackend, so the cards stay pluggable on a slirp-less build.

#ifndef POM2_SLIRP_NETWORK_BACKEND_H
#define POM2_SLIRP_NETWORK_BACKEND_H

#include "NetworkBackend.h"

#include <memory>
#include <string>

namespace pom2 {

/// Build-time availability of the libslirp backend. Prefer this over
/// `#ifdef POM2_HAVE_SLIRP` in callers — the UI wants to *say* why
/// networking is unavailable, not silently hide the option.
bool slirpAvailable();

/// Construct a libslirp-backed backend, or nullptr when unavailable
/// (not compiled in, or slirp_new failed). `hostname` is what the
/// virtual DHCP server reports; empty picks libslirp's default.
std::unique_ptr<NetworkBackend> makeSlirpBackend(const std::string& hostname = {});

} // namespace pom2

#endif // POM2_SLIRP_NETWORK_BACKEND_H
