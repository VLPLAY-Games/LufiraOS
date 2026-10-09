# Networking

This document describes the network stack of LufiraOS: a from-scratch, fully polled Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS/TLS 1.2/HTTP(S) implementation living in `kernel/net/`, built on top of the RTL8139 Ethernet driver (`kernel/drivers/net/rtl8139.c`, documented in [`07_drivers.md`](07_drivers.md)). The shell commands built on the lower layers (`ifconfig`/`ping`/`wget`) are documented in [`14_shell_commands.md`](14_shell_commands.md); the package manager's use of the HTTP(S) client is documented in [`17_package_manager.md`](17_package_manager.md). This document covers the stack itself.

---

## Table of Contents

1. [Overview](#overview)
2. [Network Configuration](#network-configuration)
3. [Ethernet Layer](#ethernet-layer)
4. [Address Resolution Protocol (ARP)](#address-resolution-protocol-arp)
5. [IPv4](#ipv4)
6. [ICMP](#icmp)
7. [TCP](#tcp)
8. [UDP](#udp)
9. [DNS](#dns)
10. [TLS 1.2](#tls-12)
11. [HTTP(S) Client and `SYS_NET_FETCH`](#https-client-and-sys_net_fetch)
12. [Byte Order and Checksum Helpers](#byte-order-and-checksum-helpers)
13. [Dependencies](#dependencies)
14. [Known Limitations](#known-limitations)
15. [Conclusion](#conclusion)

---

## Overview

`kernel/net/` is a full, if deliberately narrow, stack: Ethernet up through a real HTTPS client, with DNS in between.

- **Ethernet** (`eth.c`/`.h`) – frame construction and ethertype dispatch.
- **ARP** (`arp.c`/`.h`) – address resolution with a small round-robin cache and a blocking-poll resolve call.
- **IPv4** (`ip.c`/`.h`) – header handling, checksum, next-hop routing, protocol dispatch.
- **ICMP** (`icmp.c`/`.h`) – echo request/reply, driving the `ping` command.
- **TCP** (`tcp.c`/`.h`) – a single-connection client with real stop-and-wait retransmission and an honest advertised receive window.
- **UDP** (`udp.c`/`.h`) – send/receive datagrams; the transport underneath DNS.
- **DNS** (`dns.c`/`.h`) – a minimal resolver (one in-flight A-record query at a time).
- **TLS 1.2** (`tls.c`/`.h`, `crypto/`) – a from-scratch client: ECDHE X25519, AES-128-GCM, SNI, RSA signature verification of the server's key exchange.
- **HTTP(S) client** (`http_client.c`/`.h`) – `http_fetch()`, a one-call "download this URL" helper over TCP or TLS, exposed to userspace as `SYS_NET_FETCH` (see [`13_syscalls.md`](13_syscalls.md#miscellaneous-65-66)).

**Explicitly still not implemented:**
- **No DHCP** – the IP configuration is a static compile-time default, changeable only by the `ifconfig` shell command.
- **No IP fragmentation/reassembly** – `ip_receive()` drops any packet with the More-Fragments flag set or a nonzero fragment offset; `ip_send()` never fragments outgoing payloads either.
- **No TCP congestion control** – the receive side now has a real flow-control window (see [TCP](#tcp)), but there's still no slow-start/congestion-avoidance on the send side.
- **No TLS certificate chain validation** – see [TLS 1.2](#tls-12). This is the most important limitation in the whole stack to understand before relying on it for anything beyond fetching public files.
- **Single connection/resolve/ping/DNS-query at a time** – one global ARP cache, one global `tcp_conn_t`, one global "ping in flight" state, one DNS query in flight. There is no per-socket abstraction and no userspace socket API — everything above TCP is consumed through the one `SYS_NET_FETCH` syscall.

**Design Philosophy:**
- **Polled, not interrupt-driven** – `net_poll()` runs once per PIT tick (100 Hz), called from `timer_irq_handler()` right after `usb_poll()` (`kernel/system/timer/pit.c`). This matches the rest of this kernel's driver architecture (see [`07_drivers.md`](07_drivers.md) for USB/AC'97, which follow the same pattern).
- **Correctness over completeness** – every layer implements only the subset of its protocol the kernel's actual use cases exercise, and says so in source comments.
- **Blocking, synchronous calls all the way up** – `arp_resolve()`, `icmp_ping_wait()`, `tcp_connect()`, `dns_resolve()`, `tls_connect()`, and `http_fetch()` all block the caller with an internal bounded poll loop (the same "spin, poll the device, check a real-PIT-tick timeout" pattern used throughout this kernel), rather than exposing asynchronous/callback-based networking. `SYS_NET_FETCH` inherits this: the calling process (and, since the syscall runs with interrupts briefly re-enabled — see its own doc comment in `syscall.h` — the rest of the system too, cooperatively) is blocked for the whole fetch.

---

## Network Configuration

The stack ships with a static default matching QEMU's user-mode networking (SLIRP) subnet, defined in `kernel/net/net.c`:

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
| `void net_init(void)` | Brings up the RTL8139 (`rtl8139_init()`) and logs readiness via `klog()` if found; safe to call even with no card present. |
| `void net_poll(void)` | Polls the NIC for received frames and runs them through the whole stack (`eth_receive()` → `arp`/`ip` → `icmp`/`tcp`/`udp`). Called once per PIT tick. |
| `void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway)` | Overwrites `our_ip`/`netmask`/`gateway` in the single global `net_config_t` (`dns_server` is untouched — `ifconfig` has no option to change it). |
| `const net_config_t *net_get_config(void)` | Returns a pointer to the current configuration. |

`net_set_config()`/`net_get_config()` back the `ifconfig` shell command — no persistence across reboots, no validation beyond `ip_str_to_addr()` parsing successfully.

---

## Ethernet Layer

`eth.h`/`eth.c` implement plain Ethernet II framing — no 802.1Q VLAN tags, no jumbo frames.

```c
#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ETH_HEADER_LEN   14
#define ETH_MTU_PAYLOAD  1500
```

**API:**

| Function | Description |
|----------|-------------|
| `int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len)` | Builds a 14-byte header and sends the whole frame via `rtl8139_send()`. Returns `-1` if no card was found or `len` exceeds `ETH_MTU_PAYLOAD` (1500). |
| `void eth_receive(const uint8_t *frame, uint16_t len)` | Called from `rtl8139_poll()` for every received frame; dispatches on `ethertype`: `0x0806` → `arp_receive()`, `0x0800` → `ip_receive()`. Anything else is silently discarded. |

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

This is what lets `dlpg sync`/`upgrade` (and, in principle, any future `SYS_NET_FETCH` caller) address a real hostname like `raw.githubusercontent.com` instead of a literal IP.

---

## TLS 1.2

`tls.c`/`.h`, plus `kernel/net/crypto/` (`sha256`, `hmac`, `aes_gcm`, `x25519`, `bignum`, `rsa_verify`) — a TLS 1.2 client written from scratch, with no dependency on any external crypto library (none exists anywhere in this freestanding kernel).

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

## HTTP(S) Client and `SYS_NET_FETCH`

`http_client.c`/`.h` implements `http_fetch()` — "download this URL into a buffer," the thing that actually backs `SYS_NET_FETCH` (syscall 66, see [`13_syscalls.md`](13_syscalls.md#miscellaneous-65-66)).

```c
int http_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out);
```

Parses `scheme://host[:port][/path]` (`http://` or `https://` only — no userinfo, no IPv6 literals), resolves `host` (a literal dotted IPv4 skips DNS; anything else goes through `dns_resolve()`), connects via `tcp_connect()` or `tls_connect()`, sends a minimal `GET ... HTTP/1.1` request with `Connection: close`, and parses the response: status line, headers (`Content-Length` or `Transfer-Encoding: chunked`, with a real chunked-decoder; absent both, reads until the connection closes), streaming the body straight into `out_buf`.

**Deliberately not supported:** redirects (3xx responses come back as an ordinary result with their own status code — the caller decides whether to follow `Location`), keep-alive/pipelining, compression, cookies, authentication, conditional requests.

Return value: body length (`>= 0`, always `<= out_cap`) on success; `*status_out` (if non-`NULL`) gets the HTTP status code whenever parsing got as far as the status line — **a 404 is a successful fetch of a 404 body, not an error**. Negative values are `HTTP_FETCH_EBADURL`/`EDNS`/`ECONNECT`/`ETLS`/`EHTTP`/`ENOSPC`/`ENODEV` (`http_client.h`) — these numeric values are load-bearing: they must stay in sync with `NET_FETCH_E*` in `libc/include/lufira/syscall.h`, since `sys_net_fetch()` passes `http_fetch()`'s return straight through to userspace unchanged.

`dlpg sync`/`dlpg upgrade` (see [`17_package_manager.md`](17_package_manager.md)) are the only callers today.

---

## Byte Order and Checksum Helpers

Because x86-64 is little-endian and all network wire formats are big-endian, `net.h` defines the standard conversion pair (`htons`/`ntohs`/`htonl`/`ntohl`) plus `uint16_t net_checksum(const void *data, uint32_t len)` (RFC 1071 one's-complement sum), used identically by IP, ICMP, TCP, and UDP.

**String formatting helpers** (`net.c`) exist because the console's `printf()` has no `%02x`/zero-padding support — `ip_str_to_addr()`, `ip_addr_to_str()`, `mac_addr_to_str()` hand-roll what `printf("%02x:%02x...")` can't do here.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| Networking (`kernel/net/`) | RTL8139 driver (`drivers/net/rtl8139.c`) | Actual frame transmit/receive. |
| RTL8139 driver | PCI, PMM | Card/BAR discovery; contiguous pages for the RX ring. |
| Networking | PIT (`system/timer/pit.c`) | `net_poll()` is driven from the timer tick; every blocking call (ARP/ping/TCP/DNS/TLS/HTTP) measures its bounded timeout in real PIT ticks. |
| TLS/crypto (`kernel/net/crypto/`) | Networking only | No external crypto library — SHA-256/HMAC/AES-GCM/X25519/bignum-RSA are all implemented from scratch in this kernel. |
| `SYS_NET_FETCH` | `http_client.c`, user-pointer validation (`syscall.c`) | Validates the URL string and output buffer, then calls `http_fetch()` directly, writing the result into the caller's buffer under its own page table. |
| `wget`/`ping`/`ifconfig` (userspace, `lufira-packages`) | `13_syscalls.md`'s file/process syscalls | These commands are ordinary userspace packages now, not kernel-native — they reach the kernel's networking only indirectly, through whatever plain file/process syscalls their implementation uses; the lower network stack itself has no direct syscall surface of its own except `SYS_NET_FETCH`. |

---

## Known Limitations

- **TLS has no certificate chain/root-CA validation and no hardware entropy source** — see [TLS 1.2](#tls-12). The single most important caveat in this document.
- **No DHCP** — static IP configuration, changed only by `ifconfig`, never persisted.
- **No IP fragmentation or reassembly.**
- **No TCP congestion control** — flow control (the receive window) exists; congestion avoidance does not.
- **Single connection/resolve/ping/DNS-query/TLS-session at a time** — no socket table, no concurrency.
- **In-order-only TCP/UDP receive** — no reassembly of out-of-order data.
- **`SYS_NET_FETCH` blocks the whole system for the duration of the fetch** — same synchronous, single-flight model the rest of this stack has always had.
- **Fully polled, no interrupts** — throughput/latency bounded by the 100 Hz PIT tick rate.
- **Untested on real hardware** — only exercised against QEMU's SLIRP networking (including, for the TLS/HTTP client, genuine live internet access through it).

---

## Conclusion

LufiraOS's network stack now runs the full path from Ethernet frames to an HTTPS GET: Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS, a from-scratch TLS 1.2 client, and an HTTP(S) client exposed to userspace as one syscall. It is still narrow by design — one connection at a time, blocking calls throughout, no congestion control, and, most importantly, no certificate chain validation — but it is no longer limited to literal IP addresses and plain HTTP, and the package manager's `dlpg sync`/`upgrade` now depend on it reaching a real server on the real internet, not just a local QEMU link.

For more details, refer to the source code in `kernel/net/` (including `kernel/net/crypto/`) and `kernel/drivers/net/rtl8139.c`.

---

**Document Version:** 2.0
**Last Updated:** October 2026
**Project:** LufiraOS
