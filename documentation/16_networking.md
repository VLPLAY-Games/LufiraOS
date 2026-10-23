# Networking

This document describes LufiraOS's network stack: a from-scratch, fully polled Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS/TLS 1.2/HTTP(S) implementation, built on top of one of three interchangeable Ethernet NIC drivers — RTL8139 and the Intel e1000/e1000e family, all documented at the hardware level in [`07_drivers.md`](07_drivers.md). IP configuration can come from a DHCP lease or fall back to a static default — see [DHCP](#dhcp). The shell commands built on the lower layers (`ifconfig`/`ping`/`wget`) are documented in [`14_shell_commands.md`](14_shell_commands.md); the package manager's use of the HTTP(S) client is documented in [`17_package_manager.md`](17_package_manager.md). This document covers the stack itself.

**As of v0.9.5, none of this runs in the kernel.** The entire stack — NIC drivers, Ethernet through TLS/HTTP, and all of its crypto — was ported, logic intact, into a single userspace daemon, `netd` (`lufira-packages/daemons/netd/`), running on top of the PCI-device-claim privilege model introduced the same release (`SYS_DRIVER_CLAIM_DEVICE`/`SYS_MMAP_MMIO`/`SYS_IO_IN`/`OUT`/`SYS_DMA_ALLOC` — see [`07_drivers.md`](07_drivers.md#userspace-driver-infrastructure)). `kernel/net/` and `kernel/drivers/net/` no longer exist. Every file path below refers to `lufira-packages/daemons/netd/<name>` unless stated otherwise — the filenames themselves (`eth.c`, `tcp.c`, `dns.c`, `tls.c`, `http_client.c`, `crypto/`, …) are unchanged from the kernel-resident version, only their location and how they reach the hardware changed. `SYS_NET_FETCH`/`SYS_PING`/`SYS_NET_CONFIG_GET`/`SET` now relay to this daemon via `service_call()` instead of calling a kernel-resident function directly; see [HTTP(S) Client and `SYS_NET_FETCH`](#https-client-and-sys_net_fetch) and [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295).

---

## Table of Contents

1. [Overview](#overview)
2. [Network Configuration](#network-configuration)
3. [Ethernet Layer](#ethernet-layer)
4. [DHCP](#dhcp)
5. [Address Resolution Protocol (ARP)](#address-resolution-protocol-arp)
6. [IPv4](#ipv4)
7. [ICMP](#icmp)
8. [TCP](#tcp)
9. [UDP](#udp)
10. [DNS](#dns)
11. [TLS 1.2](#tls-12)
12. [Randomness (`netrand`)](#randomness-netrand)
13. [HTTP(S) Client and `SYS_NET_FETCH`](#https-client-and-sys_net_fetch)
14. [Byte Order and Checksum Helpers](#byte-order-and-checksum-helpers)
15. [Dependencies](#dependencies)
16. [Known Limitations](#known-limitations)
17. [Conclusion](#conclusion)

---

## Overview

`daemons/netd/` is a full, if deliberately narrow, stack: Ethernet up through a real HTTPS client, with DNS in between.

- **Ethernet** (`eth.c`/`.h`) – frame construction and ethertype dispatch, sitting on top of whichever NIC driver was found at boot: RTL8139, or one of the two Intel families, e1000 (`e1000.c`) and e1000e (`e1000e.c`) — see [Ethernet Layer](#ethernet-layer).
- **DHCP** (`dhcp.c`/`.h`) – an optional DHCPv4 client tried once at `netd` startup to replace the static IP default with a real lease; see [DHCP](#dhcp).
- **ARP** (`arp.c`/`.h`) – address resolution with a small round-robin cache and a blocking-poll resolve call.
- **IPv4** (`ip.c`/`.h`) – header handling, checksum, next-hop routing, protocol dispatch.
- **ICMP** (`icmp.c`/`.h`) – echo request/reply, driving the `ping` command.
- **TCP** (`tcp.c`/`.h`) – a single-connection client with real stop-and-wait retransmission, an honest advertised receive window, and (v0.9.5) a randomized initial sequence number — see [Randomness (`netrand`)](#randomness-netrand).
- **UDP** (`udp.c`/`.h`) – send/receive datagrams; the transport underneath DNS.
- **DNS** (`dns.c`/`.h`) – a minimal resolver (one in-flight A-record query at a time), with a randomized transaction ID and source port as of v0.9.5.
- **TLS 1.2** (`tls.c`/`.h`, `crypto/`) – a from-scratch client: ECDHE X25519, AES-128-GCM, SNI, RSA signature verification of the server's key exchange.
- **HTTP(S) client** (`http_client.c`/`.h`) – `http_fetch()`, a one-call "download this URL" helper over TCP or TLS, exposed to userspace as `SYS_NET_FETCH`; as of v0.9.5, also `http_fetch_open()`/`_read()`/`_close()`, a streaming variant exposed as `SYS_NET_FETCH_OPEN`/`_READ`/`_CLOSE` (see [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295)).
- **`netrand`** (`netrand.c`/`.h`, v0.9.5) – a shared entropy-mixing helper used by both TCP and DNS; see [Randomness (`netrand`)](#randomness-netrand).

**Explicitly still not implemented:**
- **DHCP is one-shot, not a full client** – `dhcp_start()` is tried once at boot (see [DHCP](#dhcp)); there is no background lease renewal/rebinding timer and no `DHCPDECLINE`/`DHCPRELEASE` support. If no DHCP server answers within its boot-time budget, the stack falls back to the static compile-time default, same as before — still changeable at any time via the `ifconfig` shell command.
- **No IP fragmentation/reassembly** – `ip_receive()` drops any packet with the More-Fragments flag set or a nonzero fragment offset; `ip_send()` never fragments outgoing payloads either.
- **No TCP congestion control** – the receive side now has a real flow-control window (see [TCP](#tcp)), but there's still no slow-start/congestion-avoidance on the send side.
- **No TLS certificate chain validation** – see [TLS 1.2](#tls-12). This is the most important limitation in the whole stack to understand before relying on it for anything beyond fetching public files.
- **Single connection/resolve/ping/DNS-query at a time** – one global ARP cache, one global `tcp_conn_t`, one global "ping in flight" state, one DNS query in flight. There is no per-socket abstraction and no userspace socket API — everything above TCP is consumed through the one `SYS_NET_FETCH` syscall.

**Design Philosophy:**
- **Polled, not interrupt-driven** – `net_poll()` is still called on every pass, but, as of v0.9.5, from `netd`'s own `main()` loop (`daemons/netd/main.c`) rather than from the kernel's `timer_irq_handler()`: wait up to 100 ms on `sys_ipc_recv()` for a relayed client request, handle it if one arrived, then poll the NIC — no PIT tick is involved at all now that `netd` is an ordinary userspace process with no kernel-side hook. Still fully polled underneath, matching this project's driver architecture generally (see [`07_drivers.md`](07_drivers.md)), just driven by the daemon's own loop cadence instead of a 100 Hz interrupt.
- **Correctness over completeness** – every layer implements only the subset of its protocol the kernel's actual use cases exercise, and says so in source comments.
- **Blocking, synchronous calls all the way up** – `arp_resolve()`, `icmp_ping_wait()`, `tcp_connect()`, `dns_resolve()`, `tls_connect()`, and `http_fetch()` all block the caller with an internal bounded poll loop (the same "spin, poll the device, check a real-PIT-tick timeout" pattern used throughout this kernel), rather than exposing asynchronous/callback-based networking. `SYS_NET_FETCH` inherits this: the calling process (and, since the syscall runs with interrupts briefly re-enabled — see its own doc comment in `syscall.h` — the rest of the system too, cooperatively) is blocked for the whole fetch.

---

## Network Configuration

The stack ships with a static default matching QEMU's user-mode networking (SLIRP) subnet, defined in `net.c`. This default is what's active if no DHCP server answers at boot (see [DHCP](#dhcp)) — a real lease, when one is obtained, overwrites all four fields below via the same `net_set_config()`-equivalent path `ifconfig` uses:

| Setting | Default Value |
|---------|----------------|
| IP address | `10.0.2.15` |
| Netmask | `255.255.255.0` |
| Gateway | `10.0.2.2` |
| DNS server | `10.0.2.3` (SLIRP's built-in DNS forwarder) |

```c
typedef struct {
    uint32_t our_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
} net_config_t;
```

All fields are stored as `uint32_t` in **host byte order** (`a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d`) — wire byte order only appears where headers are assembled or parsed, via `htonl()`/`ntohl()`.

**API:**

| Function | Description |
|----------|-------------|
| `void net_init(void)` | Tries each driver in `g_nic_drivers[]` in turn — RTL8139, then e1000, then e1000e — and keeps the first one whose `found()` reports a card present (see [Ethernet Layer](#ethernet-layer)); logs readiness via `printf()`. Safe to call with no card present. |
| `void net_poll(void)` | Polls the active NIC for received frames and runs them through the whole stack (`eth_receive()` → `arp`/`ip` → `icmp`/`tcp`/`udp`). Called once per `netd` main-loop iteration (v0.9.5 — see [Design Philosophy](#overview) above; before that, once per PIT tick from the kernel). |
| `void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns_server)` | Overwrites all four fields of the single global `net_config_t`. `dhcp_start()` (see [DHCP](#dhcp)) is the only caller that ever changes `dns_server`; `ifconfig` always passes `net_get_config()->dns_server` straight back through, so in practice it still only changes `our_ip`/`netmask`/`gateway`. |
| `const net_config_t *net_get_config(void)` | Returns a pointer to the current configuration. |

`net_set_config()`/`net_get_config()` back both the `ifconfig` shell command and `dhcp_start()` — no persistence across reboots, no validation beyond `ip_str_to_addr()` parsing successfully (`ifconfig`) or the DHCP option parser (`dhcp_start()`).

---

## Ethernet Layer

`eth.h`/`eth.c` implement plain Ethernet II framing — no 802.1Q VLAN tags, no jumbo frames.

```c
#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ETH_HEADER_LEN   14
#define ETH_MTU_PAYLOAD  1500
```

**NIC drivers:** `eth.c` never calls a specific card's driver directly — `eth_send()` goes through `net_send_frame()`/`net_get_mac()` in `net.c`, which forward to whichever driver `net_init()` picked. Three are available, tried in this order (`g_nic_drivers[]` in `net.c`), the first one found winning:

| Driver | File | PCI vendor:device(s) | Notes |
|--------|------|----------------------|-------|
| RTL8139 | `rtl8139.c` | Realtek | I/O-port BAR0, the original driver (see [`07_drivers.md`](07_drivers.md)). |
| e1000 | `e1000.c` | `0x8086`, classic 8254x family (82540EM/82544/82545/82546) | MMIO BAR0, legacy 16-byte TX/RX descriptor rings. QEMU's `-device e1000` emulates the 82540EM (`0x100E`), the primary target this driver was checked against. |
| e1000e | `e1000e.c` | `0x8086`, PCIe 8257x/8258x family (82571–82583/ICH8-10) | Structurally near-identical to `e1000.c` — same register map, same descriptor format, different PCI device-id table. QEMU's `-device e1000e` emulates the 82574L (`0x10D3`). |

All three expose the same `nic_driver_t` shape (`init`/`found`/`get_mac`/`send`/`poll`) and, like RTL8139, are polled rather than interrupt-driven — `e1000_poll()`/`e1000e_poll()` check each descriptor's own Descriptor-Done bit directly, the same pattern as every other driver in this kernel (see [`07_drivers.md`](07_drivers.md)).

**API:**

| Function | Description |
|----------|-------------|
| `int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len)` | Builds a 14-byte header and sends the whole frame via the active NIC driver. Returns `-1` if no card was found or `len` exceeds `ETH_MTU_PAYLOAD` (1500). |
| `void eth_receive(const uint8_t *frame, uint16_t len)` | Called from the active NIC's `poll()` for every received frame; dispatches on `ethertype`: `0x0806` → `arp_receive()`, `0x0800` → `ip_receive()`. Anything else is silently discarded. |

---

## DHCP

`dhcp.c`/`.h` implement a minimal DHCPv4 client (RFC 2131/2132): DISCOVER → OFFER → REQUEST → ACK, broadcast over `udp.c`/`ip.c` exactly like everything else in this stack — one blocking call with a budget, no background renew/rebind timer, no `DHCPDECLINE`/`DHCPRELEASE`. It's tried once from `kernel.c`, right after `net_init()`:

```c
if (net_found()) {
    if (dhcp_start(4000) == 0) { /* lease applied */ }
    else { /* stays on the static default from net.c */ }
}
```

**Exchange:** each phase (DISCOVER→OFFER, then REQUEST→ACK) gets up to 3 broadcast attempts at ~3000 ms apiece, all bounded by the same overall deadline passed to `dhcp_start()`. Replies are matched back to this client by transaction ID (`xid`, seeded from the PIT tick count — not cryptographically random, but it doesn't need to be) and MAC address (`chaddr`), not by anything the server is trusted to get right. A `DHCPNAK` aborts immediately rather than retrying.

**What it requests/accepts:** the parameter request list asks for options 1 (subnet mask), 3 (router), and 6 (DNS server); only those three plus message type (53) and server identifier (54) are parsed out of the reply. Any field the server's `ACK` doesn't supply falls back to whatever `net_get_config()` already held — a lease is never allowed to blank out a working value.

**API:**

| Function | Description |
|----------|-------------|
| `int dhcp_start(uint32_t timeout_ms)` | Runs the full DISCOVER/OFFER/REQUEST/ACK exchange within `timeout_ms` (`kernel.c` uses 4000, deliberately shorter than `http_client.c`'s own total budget so a missing DHCP server doesn't noticeably extend boot time). On success, applies `yiaddr`/subnet-mask/router/DNS-server via `net_set_config()` and returns `0`. Returns `-1` on no card, no `OFFER`, a `NAK`, or a timeout — `net_get_config()` is left exactly as it was. |

This is what moved networking in this kernel from "edit a compile-time constant or run `ifconfig` by hand" to "just works," matching QEMU's SLIRP DHCP server (and any real DHCP server on real hardware) — `ifconfig` remains available afterward for manual overrides, same as before.

---

## Address Resolution Protocol (ARP)

`arp.c`/`.h` implement a minimal ARP cache and the standard request/reply exchange.

**Cache:** a fixed 16-entry array (`ARP_CACHE_SIZE`), evicted round-robin when full (FIFO-style, not LRU).

**API:**

| Function | Description |
|----------|-------------|
| `int arp_resolve(uint32_t ip, uint8_t out_mac[6])` | Cache hit: returns `0` immediately. Cache miss: broadcasts a request, then polls for up to ~2000 ms (measured in real PIT ticks, not loop iterations). Returns `-1` on timeout or no NIC. |
| `void arp_receive(const uint8_t *payload, uint16_t len)` | Learns the sender's IP→MAC from every ARP packet seen; replies if the packet requests our own IP. |

---

## IPv4

`ip.c`/`.h` implement a bare IPv4 header, no options.

```c
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u
#define IP_HEADER_LEN 20
```

**Routing (`ip_send()`):** same-subnet test (`((dst_ip ^ our_ip) & netmask) == 0`) picks direct delivery vs. the gateway; the next hop's MAC is resolved via `arp_resolve()`. TTL is always 64; each outgoing packet gets a fresh incrementing IP ID.

**Receive path (`ip_receive()`)** drops a packet on a bad version/IHL, a bad header checksum, an out-of-range `total_length`, any fragmentation (More-Fragments set or nonzero offset — no reassembly buffer exists anywhere in the stack), or a destination that isn't our configured IP. What passes is dispatched by `protocol`: ICMP → `icmp_receive()`, TCP → `tcp_receive()`, **UDP → `udp_receive()`**.

---

## ICMP

`icmp.c`/`.h` implement only Echo Request/Reply (types 8 and 0). `icmp_receive()` answers any Echo Request addressed to us (so the machine is pingable), and the `ping` shell command drives one outstanding Echo Request at a time via `icmp_ping_send()`/`icmp_ping_wait()`.

---

## TCP

`tcp.c`/`.h` implement a single-connection TCP **client**, blocking at every call. Exactly one connection exists at a time, in a global `tcp_conn_t`.

**Randomized ISN and local port (v0.9.5).** `tcp_connect()` previously picked the initial sequence number from `sys_gettick() * 65537 + 12345` and the local port from a plain incrementing counter (`g_next_local_port++`, starting at 49152) — both fully predictable to an off-path attacker who can guess or observe one connection, which matters for TCP because an attacker who can predict the ISN and local port of a *future* connection can inject spoofed segments into it without ever seeing the real traffic. Both now come from `netrand_u32()`/`netrand_u16()` (see [Randomness (`netrand`)](#randomness-netrand)) — `snd_next = netrand_u32()`, `local_port = 49152 + (netrand_u16() & 0x3FFF)`.

**Retransmission (send side):** stop-and-wait — one unacknowledged segment at a time, resent on a fixed 300 ms RTO (`TCP_RTO_TICKS`) up to 5 times (`TCP_MAX_RETRIES`) before the connection gives up and closes. No adaptive RTT estimation, no congestion window.

**Receive queue and flow control:** an in-order byte queue (`rx_fifo`, 8 KB) that *appends* newly-arrived in-order data rather than evicting what's already queued, and a real advertised receive window (`tcp_rx_window()`, capped at 4×MSS ≈ 5840 bytes — sized so a full window's worth of frames fits and drains within one `rtl8139_poll()` pass of the NIC's own RX ring) sent in every outgoing segment, including bare ACKs. The peer is told, honestly, how much room is left; it isn't expected to self-limit by guesswork. Out-of-order segments are still not buffered (no reassembly) and are simply not accepted — but they do get an immediate duplicate ACK, so the peer learns where to resume quickly rather than waiting out a retransmission timer.

> **This replaces an earlier single-segment design** that kept only one unconsumed 1460-byte buffer: a second segment arriving before the first was read by the caller was silently dropped, with no window update to warn the sender — survivable only because the sole caller at the time (`wget`) tolerated the resulting retransmission-recovery slowdown via a generous ~10-second no-data timeout. TLS's much tighter per-record budget made the data loss fatal, which is how the bug was found; see the "НАЙДЕННЫЙ БАГ" block at the top of `tcp.c` for the full packet-capture analysis.

**API:**

| Function | Signature | Description |
|----------|-----------|--------------|
| Connect | `int tcp_connect(uint32_t remote_ip, uint16_t remote_port)` | Active open; ~3000 ms budget including SYN retries. `0` = `ESTABLISHED`, `-1` = timeout/RST. |
| Send | `int tcp_send(const void *data, uint16_t len)` | Splits into ≤1460-byte segments, stop-and-wait with retransmission per segment. |
| Receive (poll) | `int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len)` | Polls the NIC once; returns `1` and a pointer to the next ≤1460-byte in-order chunk from `rx_fifo` (the *same* chunk on repeated calls until acknowledged), or `0`. |
| Acknowledge | `void tcp_ack_consumed(void)` | Pops the handed-out chunk from the queue and sends an ACK carrying the now-larger window — must be called exactly once per successful `tcp_recv_poll()`. |
| Remote-close check | `int tcp_is_remote_closed(void)` | `1` once the peer's FIN has been seen. |
| Close | `void tcp_close(void)` | FIN|ACK if established, best-effort wait (~1000 ms) for a clean close, then forces local `CLOSED` regardless. |
| Receive (dispatch) | `void tcp_receive(uint32_t src_ip, const void *payload, uint16_t len)` | Called from `ip_receive()`; validates, handles RST/handshake/data/FIN as above. |

---

## UDP

`udp.c`/`.h` — just enough UDP to carry DNS: send one datagram, listen for one reply on a given local port.

**API:**

| Function | Description |
|----------|-------------|
| `int udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t len)` | Builds the pseudo-header-checksummed UDP datagram and sends it via `ip_send()`. A computed checksum of exactly `0x0000` is sent as `0xFFFF` per RFC 768 (`0x0000` means "no checksum"). |
| `void udp_listen(uint16_t local_port)` / `void udp_stop_listen(void)` | Arms/disarms the single listen slot — call `udp_listen()` *before* sending the request you're expecting a reply to, or an early reply is lost. |
| `int udp_recv_poll(uint32_t *out_src_ip, uint16_t *out_src_port, uint8_t **out_ptr, uint16_t *out_len)` | Polls the NIC once; `1` and the datagram if one arrived on the listened port since the last call, else `0`. |
| `void udp_receive(uint32_t src_ip, const void *payload, uint16_t len)` | Called from `ip_receive()` on UDP datagrams. |

Like TCP, this is a single-outstanding-exchange model — no socket table, no concurrent listeners.

---

## DNS

`dns.c`/`.h` — a minimal resolver: one A-record query at a time, no caching, no CNAME following.

`int dns_resolve(const char *hostname, uint32_t *out_ip)` builds a standard DNS query (one question, type A, class IN), sends it via `udp_send()` to `net_get_config()->dns_server` on port 53, and polls for a matching response (by transaction ID) for up to ~3000 ms. It parses just enough of the response — skipping the echoed question section and each answer record's (possibly compressed) name — to find the first `A`/`IN` record and return its address. Returns `0` on success, `-1` on timeout, `NXDOMAIN`, a non-zero RCODE, or an answer section with no usable `A` record (e.g. a bare `CNAME` with no "glue" address).

**Randomized transaction ID and source port (v0.9.5).** The transaction ID previously came from a plain incrementing counter (`g_next_dns_txn_id++`, starting at 1), and the local UDP port was derived from it (`50000 + (txn_id % 10000)`) — so learning or guessing one revealed the other, and both were fully predictable to an off-path attacker trying to spoof a response before the real DNS server's reply arrives. Both are now independently drawn from `netrand_u16()` (see [Randomness (`netrand`)](#randomness-netrand)): `txn_id = netrand_u16()`, `local_port = 49152 + (netrand_u16() & 0x3FFF)` — the same local-port formula TCP now uses, drawn from a separate call so the two don't correlate.

This is what lets `dlpg sync`/`upgrade` (and, in principle, any future `SYS_NET_FETCH` caller) address a real hostname like `raw.githubusercontent.com` instead of a literal IP.

---

## TLS 1.2

`tls.c`/`.h`, plus `crypto/` (`sha256`, `hmac`, `aes_gcm`, `x25519`, `bignum`, `rsa_verify`) — a TLS 1.2 client written from scratch, with no dependency on any external crypto library (none exists anywhere in this project).

**Scope, deliberately narrow:**
- TLS 1.2 only — no 1.3, no session resumption/tickets, no renegotiation, no client certificates.
- Exactly one cipher suite offered: `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256` (`0xC02F`). A server that won't negotiate it fails the handshake.
- Key exchange: ECDHE over **X25519** (RFC 7748), with the `server_name` (SNI), `supported_groups`, `ec_point_formats`, and `signature_algorithms` (`rsa_pkcs1_sha256`) ClientHello extensions needed to get a modern TLS terminator (Fastly/GitHub, specifically) to agree to it.
- Records: AES-128-GCM (RFC 5288 nonce construction) once the handshake's `ChangeCipherSpec` is exchanged.

**Server authentication — read this carefully:** the client parses the leaf certificate out of the server's `Certificate` message with a minimal DER/ASN.1 walker just far enough to extract its RSA public key, and uses that key to verify the RSA PKCS#1v1.5 signature over the `ServerKeyExchange` message. This proves the party on the other end of the TCP connection actually controls the private key matching the certificate it presented — it defeats a passive or naive on-path attacker who doesn't hold a matching private key for *any* certificate. **It does not validate the certificate chain against any root CA trust store** — there is no bundled set of trusted roots, no expiry check, no hostname-vs-SAN check. An attacker who can present their own self-signed (or otherwise illegitimate) certificate is not stopped. There is also no hardware random number generator anywhere in this kernel; handshake randomness (`client_random`, the X25519 private key) is derived from `rdtsc` and PIT ticks mixed through SHA-256 — unpredictable enough on real hardware, much less so under deterministic emulation — so there is no real forward secrecy guarantee either. Treat this TLS implementation as "keeps an unsophisticated listener out," not as a hardened channel — good enough for fetching public package files over a QEMU SLIRP link, not a substitute for a real, audited TLS stack.

**API:**

| Function | Description |
|----------|-------------|
| `int tls_connect(uint32_t ip, uint16_t port, const char *sni_hostname)` | Full handshake including the ServerKeyExchange signature check above. `0` = success, `-1` = any failure (timeout, bad signature, unsupported cipher, alert, …). |
| `int tls_send(const void *data, uint16_t len)` | Encrypts and sends as one or more application-data records. |
| `int tls_recv(uint8_t *buf, uint16_t maxlen)` | Short-read semantics like a blocking socket: `>0` bytes, `0` clean close, `-1` error. |
| `void tls_close(void)` | Best-effort `close_notify` alert, then `tcp_close()`. |

The record/handshake-message layer reassembles across however many `tcp_recv_poll()` chunks and TLS records a message (notably a real-world `Certificate` chain) actually spans — this cannot be assumed to be "one TCP read, one TLS record, one handshake message" for anything but the smallest messages.

---

## Randomness (`netrand`)

`netrand.c`/`.h` (v0.9.5) is a small shared entropy-mixing module used independently by TCP (ISN and local port — see [TCP](#tcp)) and DNS (transaction ID and local port — see [DNS](#dns)), replacing the predictable counters both previously used. It's the same technique `tls.c`'s own `tls_gather_random()` already used for handshake randomness — `rdtsc`, a PIT-tick read, and a per-call counter, all mixed through a SHA-256-based pool — reimplemented as its own module rather than shared state with TLS's pool, so a DNS or TCP connection's randomness draw never has side effects on (or depends on) whatever state the TLS layer happens to be in.

```c
uint32_t netrand_u32(void);
uint16_t netrand_u16(void);
```

**What this fixes.** Before v0.9.5, TCP derived its initial sequence number from `sys_gettick() * 65537 + 12345` and its local port from a bare incrementing counter; DNS derived its transaction ID from a bare incrementing counter and its local port *from that same counter* (`50000 + (txn_id % 10000)`). All of these are predictable to an attacker who can observe or guess even one prior connection/query — which matters because TCP and DNS both authenticate a response only by a handful of values an off-path attacker could otherwise forge (sequence number plus ports for TCP; transaction ID plus ports for DNS). An attacker able to guess the next value could inject spoofed segments or a spoofed DNS answer before the real reply arrives, without ever being positioned to intercept the real traffic.

**What this does not fix.** Same caveat as `tls.c`'s own randomness (see [TLS 1.2](#tls-12)): there is no hardware random number generator anywhere in this project, so `netrand`'s entropy is only as unpredictable as `rdtsc`/PIT timing actually is — genuinely hard to predict on real hardware, much less so under deterministic emulation (QEMU/TCG). This closes the "trivially predictable, no guessing required" class of weakness, not "cryptographically unguessable under all conditions."

---

## HTTP(S) Client and `SYS_NET_FETCH`

`http_client.c`/`.h` implements `http_fetch()` — "download this URL into a buffer," the thing that actually backs `SYS_NET_FETCH`.

```c
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);
```

Parses `scheme://host[:port][/path]` (`http://` or `https://` only — no userinfo, no IPv6 literals), resolves `host` (a literal dotted IPv4 skips DNS; anything else goes through `dns_resolve()`), connects via `tcp_connect()` or `tls_connect()`, sends a minimal `GET ... HTTP/1.1` request with `Connection: close`, and parses the response: status line, headers (`Content-Length` or `Transfer-Encoding: chunked`, with a real chunked-decoder; absent both, reads until the connection closes), streaming the body straight into `out_buf`.

**Deliberately not supported:** redirects (3xx responses come back as an ordinary result with their own status code — the caller decides whether to follow `Location`), keep-alive/pipelining, compression, cookies, authentication, conditional requests.

Return value: body length (`>= 0`, always `<= out_cap`) on success; `*status_out` (if non-`NULL`) gets the HTTP status code whenever parsing got as far as the status line — **a 404 is a successful fetch of a 404 body, not an error**. Negative values are `HTTP_FETCH_EBADURL`/`EDNS`/`ECONNECT`/`ETLS`/`EHTTP`/`ENOSPC`/`ENODEV` (`http_client.h`) — these numeric values are load-bearing: they must stay in sync with `NET_FETCH_E*` in `libc/include/lufira/syscall.h`, since `sys_net_fetch()` passes `http_fetch()`'s return straight through to userspace unchanged.

**Reaching the kernel (as of v0.9.5).** `netd` runs as an ordinary userspace process, so `SYS_NET_FETCH`'s caller and `http_fetch()`'s actual execution are now in two different address spaces. The kernel relays the request to `netd` via `service_call()` (a `mailbox_send()`/`mailbox_recv()` pair with a fixed sender-pid check — see below) and brokers the response body through a shared-memory region (`shm.c`, sized to the caller's `out_cap`, see [`11_memory_management.md`](11_memory_management.md#shared-memory-sys_shm_create--sys_shm_attach)): `netd` attaches it (`SYS_SHM_ATTACH`), writes `http_fetch()`'s output directly into it, detaches, and replies; the kernel then reads the finished bytes straight out of the shared physical pages. `SYS_PING`/`SYS_NET_CONFIG_GET`/`SET` relay the same way, just without a shared-memory step (their payloads fit the ordinary 96-byte mailbox). **`service_call()`'s sender-pid check** (`syscall.c`) is itself a v0.9.5 fix: the relay now verifies that a reply actually came back from the pid the kernel sent the request *to*, closing a spoofing gap where another process could have raced in and answered on `netd`'s behalf.

**Streaming (`SYS_NET_FETCH_OPEN`/`_READ`/`_CLOSE`, v0.9.5).** `http_fetch_open()`/`http_fetch_read()`/`http_fetch_close()` (`http_client.h`) split the same connect/request/parse logic into three calls instead of one: `_open` does everything through parsing the response headers but does **not** read the body, reporting `Content-Length` if the server sent one; `_read` returns the next chunk of the body (up to the caller's buffer size) and can be called repeatedly; `_close` tears the connection down (idempotent — safe after a clean EOF, an error, or even if `_open` was never called). The TCP/TLS connection is kept open by `netd` *between* `_read` calls specifically so a caller reading in small chunks doesn't pay for a fresh TLS handshake on every call — this is what lets `dlpg install`/`update` stream a `.lpg` to disk in bounded-size chunks regardless of the package's real size (see [`17_package_manager.md`](17_package_manager.md)). Chunked transfer-encoding is rejected in streaming mode (`HTTP_FETCH_ECHUNKED`/`NET_FETCH_ECHUNKED`) — only the whole-body `http_fetch()`/`SYS_NET_FETCH` path decodes it. Full syscall contract in [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295).

`dlpg sync`/`dlpg install`/`dlpg update`/`dlpg upgrade` (see [`17_package_manager.md`](17_package_manager.md)) are the callers today — `sync` (fetching the small `index.json`) still uses the whole-body `SYS_NET_FETCH`; the others use the streaming variant for the actual `.lpg` download.

---

## Byte Order and Checksum Helpers

Because x86-64 is little-endian and all network wire formats are big-endian, `net.h` defines the standard conversion pair (`htons`/`ntohs`/`htonl`/`ntohl`) plus `uint16_t net_checksum(const void *data, uint32_t len)` (RFC 1071 one's-complement sum), used identically by IP, ICMP, TCP, and UDP.

**String formatting helpers** (`net.c`) exist because the console's `printf()` has no `%02x`/zero-padding support — `ip_str_to_addr()`, `ip_addr_to_str()`, `mac_addr_to_str()` hand-roll what `printf("%02x:%02x...")` can't do here.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| `netd` (`lufira-packages/daemons/netd/`) | Userspace Driver Infrastructure (`SYS_DRIVER_CLAIM_DEVICE`, `SYS_MMAP_MMIO`, `SYS_IO_IN`/`OUT`, `SYS_DMA_ALLOC` — [`07_drivers.md`](07_drivers.md#userspace-driver-infrastructure)) | Claims a NIC and reaches its registers/DMA buffers, since v0.9.5 this stack no longer runs in the kernel and has no direct hardware access of its own. |
| RTL8139/e1000/e1000e drivers (inside `netd`) | `netd`'s own claim on the device | Actual frame transmit/receive, whichever driver `net_init()` found first — see [`07_drivers.md`](07_drivers.md#network-drivers-rtl8139--e1000--e1000e). |
| Networking | `netd`'s own `main()` loop | `net_poll()` is driven from this loop's own cadence (v0.9.5), not a kernel timer tick; every blocking call (ARP/DHCP/ping/TCP/DNS/TLS/HTTP) still measures its bounded timeout against `sys_gettick()`. |
| DHCP (`dhcp.c`) | UDP (`udp.c`), Networking | One blocking `dhcp_start()` call at `netd` startup (`main.c`), broadcasting over the same UDP layer DNS uses. |
| TCP/DNS (`tcp.c`/`dns.c`) | `netrand.c` | Randomized ISN/local port and transaction ID/local port — see [Randomness (`netrand`)](#randomness-netrand). |
| TLS/crypto (`crypto/`) | Networking only | No external crypto library — SHA-256/HMAC/AES-GCM/X25519/bignum-RSA are all implemented from scratch, now inside `netd` rather than the kernel. |
| `SYS_NET_FETCH`/`_OPEN`/`_READ`/`_CLOSE`, `SYS_PING`, `SYS_NET_CONFIG_GET`/`SET` | `service_call()` relay to `netd` (`syscall.c`), `shm.c` for the response body | Validates the caller's pointers, relays the request to `netd` over the mailbox, and for `SYS_NET_FETCH`/`_OPEN`/`_READ` brokers the actual data through a kernel-created shared-memory region — see [HTTP(S) Client and `SYS_NET_FETCH`](#https-client-and-sys_net_fetch). |
| `wget`/`ping`/`ifconfig` (userspace, `lufira-packages`) | `13_syscalls.md`'s file/process syscalls | These commands are ordinary userspace packages, talking only to the syscalls above — none of them call into `netd` directly or know it exists. |

---

## Known Limitations

- **TLS has no certificate chain/root-CA validation and no hardware entropy source** — see [TLS 1.2](#tls-12). The single most important caveat in this document.
- **DHCP is a one-shot boot-time attempt, not a managed client** — see [DHCP](#dhcp): no lease renewal/rebinding, no `DHCPDECLINE`/`DHCPRELEASE`; a lease that would need renewing just keeps being used until the next reboot. Falls back to the static default (still changeable by `ifconfig`, never persisted) if no server answers.
- **No IP fragmentation or reassembly.**
- **No TCP congestion control** — flow control (the receive window) exists; congestion avoidance does not.
- **Single connection/resolve/ping/DNS-query/TLS-session at a time** — no socket table, no concurrency.
- **In-order-only TCP/UDP receive** — no reassembly of out-of-order data.
- **`SYS_NET_FETCH` blocks the calling process for the duration of the fetch** — same synchronous, single-flight model the rest of this stack has always had (streaming via `SYS_NET_FETCH_OPEN`/`_READ` changes *when* the blocking happens, not *whether* it does).
- **Fully polled, no interrupts** — as of v0.9.5, driven by `netd`'s own main-loop cadence rather than the kernel's PIT tick (see [Dependencies](#dependencies)), but no NIC interrupt is ever serviced either way.
- **Exactly one `netd` process at a time** — `SYS_NETD_REGISTER` is root-only and rejects a second registration, the same single-daemon model `SYS_WM_REGISTER`/`SYS_SOUNDD_REGISTER` use (see [`13_syscalls.md`](13_syscalls.md#sound-and-network-daemons-8990-9295)).
- **Untested on real hardware** — only exercised against QEMU's SLIRP networking (including, for the TLS/HTTP client, genuine live internet access through it).

---

## Conclusion

LufiraOS's network stack runs the full path from Ethernet frames to an HTTPS GET: Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS, a from-scratch TLS 1.2 client, and an HTTP(S) client exposed to userspace as both a one-shot and a streaming syscall pair. It is still narrow by design — one connection at a time, blocking calls throughout, no congestion control, and, most importantly, no certificate chain validation — but it is no longer limited to literal IP addresses and plain HTTP, and the package manager's `dlpg` subcommands depend on it reaching a real server on the real internet, not just a local QEMU link. It also no longer requires a single specific NIC or a hand-set static IP: RTL8139, e1000, and e1000e are all interchangeable at the `eth.c` layer, and `dhcp_start()` tries to get a real lease before falling back to the old static default. **As of v0.9.5**, none of this runs in the kernel at all — the entire stack moved into the `netd` userspace daemon on top of the new PCI-device-claim privilege model (see [`07_drivers.md`](07_drivers.md#userspace-driver-infrastructure)), with randomized TCP/DNS values (`netrand`, see [Randomness (`netrand`)](#randomness-netrand)) and a streaming fetch API (`SYS_NET_FETCH_OPEN`/`_READ`/`_CLOSE`) added in the same release.

For more details, refer to the source code in `lufira-packages/daemons/netd/` (including `crypto/`, `dhcp.c`, and `netrand.c`).

---

**Document Version:** 3.0
**Last Updated:** October 2026
**Project:** LufiraOS
