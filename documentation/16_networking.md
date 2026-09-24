# Networking

This document describes the TCP/IP protocol stack of LufiraOS: a from-scratch, fully polled Ethernet/ARP/IPv4/ICMP/TCP implementation living in `kernel/net/`, built on top of the RTL8139 Ethernet driver (`kernel/drivers/net/rtl8139.c`, documented in [`07_drivers.md`](07_drivers.md)). The shell commands built on this stack (`ifconfig`/`ping`/`wget`) are documented in [`14_shell_commands.md`](14_shell_commands.md#network-commands); this document covers the stack itself.

---

## Table of Contents

1. [Overview](#overview)
2. [Network Configuration](#network-configuration)
3. [Ethernet Layer](#ethernet-layer)
4. [Address Resolution Protocol (ARP)](#address-resolution-protocol-arp)
5. [IPv4](#ipv4)
6. [ICMP](#icmp)
7. [TCP](#tcp)
   - [Connection State](#connection-state)
   - [Connection Lifecycle](#connection-lifecycle)
8. [Byte Order and Checksum Helpers](#byte-order-and-checksum-helpers)
9. [Dependencies](#dependencies)
10. [Known Limitations](#known-limitations)
11. [Conclusion](#conclusion)

---

## Overview

`kernel/net/` implements just enough of the TCP/IP stack for the shell's `ifconfig`/`ping`/`wget` commands to work against a QEMU SLIRP (or similarly well-behaved) link:

- **Ethernet** (`eth.c`/`.h`) – frame construction and ethertype dispatch.
- **ARP** (`arp.c`/`.h`) – address resolution with a small round-robin cache and a blocking-poll resolve call.
- **IPv4** (`ip.c`/`.h`) – header handling, checksum, next-hop routing, protocol dispatch.
- **ICMP** (`icmp.c`/`.h`) – echo request/reply, driving the `ping` command.
- **TCP** (`tcp.c`/`.h`) – a minimal single-connection blocking client, enough for `wget`'s HTTP/1.0 GET.

**Explicitly not implemented:**
- **No DNS** – every command that takes a remote endpoint (`ping`, `wget`) takes a literal IPv4 address.
- **No DHCP** – the IP configuration is a static compile-time default, changeable only by the `ifconfig` shell command.
- **No UDP at all** – there is no `udp.c`, no UDP header type, and `ip_receive()`'s protocol dispatch only recognises `IP_PROTO_ICMP` (1) and `IP_PROTO_TCP` (6); any other protocol number, UDP (17) included, is silently dropped.
- **No IP fragmentation/reassembly** – `ip_receive()` drops any packet with the More-Fragments flag set or a nonzero fragment offset (see [IPv4](#ipv4)); `ip_send()` never fragments outgoing payloads either, it simply refuses payloads larger than one Ethernet frame can carry.
- **No TCP retransmission or congestion control** – segments are sent once; if they're lost, the stack relies on the caller's own timeout (e.g. `wget`'s ~10s no-data deadline) to give up.
- **Single connection/resolve/ping at a time** – one global ARP cache, one global `tcp_conn_t`, one global "ping in flight" state. There is no per-socket abstraction.

**Design Philosophy:**
- **Polled, not interrupt-driven** – `net_poll()` runs once per PIT tick (100 Hz), called from `timer_irq_handler()` right after `usb_poll()` (`kernel/system/timer/pit.c`). This matches the rest of this kernel's driver architecture, which has no APIC/MSI-X support and polls hardware from the timer tick rather than fielding per-device interrupts (see [`07_drivers.md`](07_drivers.md) for USB/AC'97, which follow the same pattern).
- **Correctness over completeness** – every layer implements only the subset of its protocol that the shell's three network commands actually exercise, and says so in source comments rather than pretending to be a general-purpose stack.
- **Blocking, synchronous calls at the top** – `arp_resolve()`, `icmp_ping_wait()`, `tcp_connect()`, and `tcp_close()` all block the calling shell command with an internal bounded poll loop (the same "spin, poll the device, check a timeout in real PIT ticks" pattern used by `xhci_wait_for_event()` in the USB stack), rather than exposing asynchronous/callback-based networking.

---

## Network Configuration

The stack ships with a static default matching QEMU's user-mode networking (SLIRP) subnet, defined in `kernel/net/net.c`:

| Setting | Default Value |
|---------|----------------|
| IP address | `10.0.2.15` |
| Netmask | `255.255.255.0` |
| Gateway | `10.0.2.2` |

```c
typedef struct {
    uint32_t our_ip;
    uint32_t netmask;
    uint32_t gateway;
} net_config_t;
```

All three fields are stored as `uint32_t` in **host byte order** (`a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d`) — the network/wire byte order only appears at the point headers are assembled or parsed, via `htonl()`/`ntohl()`.

**API:**

| Function | Description |
|----------|-------------|
| `void net_init(void)` | Brings up the RTL8139 (`rtl8139_init()`) and logs readiness via `klog()` if found; safe to call even with no card present. |
| `void net_poll(void)` | Polls the NIC for received frames and runs them through the whole stack (`eth_receive()` → `arp`/`ip` → `icmp`/`tcp`). Called once per PIT tick. |
| `void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway)` | Overwrites the single global `net_config_t`. |
| `const net_config_t *net_get_config(void)` | Returns a pointer to the current configuration. |

`net_set_config()`/`net_get_config()` back the `ifconfig` shell command: called with no arguments it prints the current configuration (plus the card's MAC, via `rtl8139_get_mac()`); called with `<ip> <netmask> <gateway>` it overwrites the global config in place — there is no persistence across reboots and no validation beyond `ip_str_to_addr()` parsing successfully.

---

## Ethernet Layer

`eth.h`/`eth.c` implement plain Ethernet II framing — no 802.1Q VLAN tags, no jumbo frames.

```c
#define ETHERTYPE_IP  0x0800u
#define ETHERTYPE_ARP 0x0806u

#define ETH_HEADER_LEN   14
#define ETH_MTU_PAYLOAD  1500

typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype; // network order
} eth_header_t;
```

**API:**

| Function | Description |
|----------|-------------|
| `int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const void *payload, uint16_t len)` | Builds a 14-byte header (source = the card's own MAC from `rtl8139_get_mac()`) in front of `payload` and sends the whole frame via `rtl8139_send()`. Returns `-1` if no card was found or `len` exceeds `ETH_MTU_PAYLOAD` (1500). |
| `void eth_receive(const uint8_t *frame, uint16_t len)` | Called from `rtl8139_poll()` for every received frame. Drops anything shorter than the 14-byte header, then dispatches on `ethertype`: `0x0806` → `arp_receive()`, `0x0800` → `ip_receive()`. Any other ethertype is silently discarded. |

---

## Address Resolution Protocol (ARP)

`arp.c`/`.h` implement a minimal ARP cache and the standard request/reply exchange.

**Cache:** a fixed 16-entry array (`ARP_CACHE_SIZE`). A lookup that misses the cache is followed by an insert; when the cache is full, insertion evicts entries **round-robin** (`arp_next_slot`, incremented modulo 16 on every new IP) — this is a FIFO-style eviction, not LRU.

```c
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;
```

**API:**

| Function | Description |
|----------|-------------|
| `int arp_resolve(uint32_t ip, uint8_t out_mac[6])` | Cache hit: returns immediately (`0`). Cache miss: broadcasts an ARP request, then blocks, polling the NIC (`rtl8139_poll()`) and re-checking the cache once per millisecond, up to a ~2000 ms timeout measured in real PIT ticks (`pit_get_ticks()`, 200 ticks at 10 ms each — the code comments explicitly warn against counting `pit_wait_ms(1)` iterations instead, since that undercounts by 10x, a bug the USB Mass Storage code hit once already). Returns `-1` on timeout or if no NIC was found. |
| `void arp_receive(const uint8_t *payload, uint16_t len)` | Called from `eth_receive()` on ARP frames. Opportunistically learns the sender's IP→MAC mapping into the cache from **every** ARP packet seen (request or reply) — the common minimal-ARP-stack shortcut. If the packet is a request for our own configured IP, replies with an ARP reply carrying our MAC. |

The wire format (`arp_packet_t`) is the standard 28-byte Ethernet/IPv4 ARP packet (`htype`/`ptype`/`hlen`/`plen`/`oper`/`sha`/`spa`/`tha`/`tpa`), with `ARP_HTYPE_ETHERNET` (1) and `ARP_PTYPE_IPV4` (0x0800).

---

## IPv4

`ip.c`/`.h` implement a bare IPv4 header, no options.

```c
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_HEADER_LEN 20

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   // 4<<4 | 5 (no options)
    uint8_t  tos;
    uint16_t total_length;  // network order, header included
    uint16_t id;            // network order
    uint16_t flags_frag;    // network order
    uint8_t  ttl;
    uint8_t  protocol;      // IP_PROTO_*
    uint16_t checksum;      // network order
    uint32_t src;           // network order
    uint32_t dst;           // network order
} ip_header_t;
```

**Routing (`ip_send()`):** the next hop is chosen by a same-subnet test — `((dst_ip ^ our_ip) & netmask) == 0` — sending directly to `dst_ip` if it matches, otherwise routing via the configured gateway. Either way the next hop's MAC is resolved with `arp_resolve()` before the frame goes out. TTL is always set to 64, and each outgoing packet gets a fresh, incrementing IP ID (`ip_id_counter`).

**Receive path (`ip_receive()`)** drops a packet if:
- IP version isn't 4, or IHL isn't exactly 5 (any options present are unsupported).
- The header checksum (`net_checksum()` over the 20-byte header) doesn't come out to zero.
- `total_length` is out of range for the frame actually received.
- The **More Fragments** flag is set, or the fragment offset is nonzero — i.e. **any fragmented packet is dropped outright**, logged via `DLOG("[IP] fragmented packet dropped (unsupported)\n")`. There is no reassembly buffer anywhere in the stack. This is an accepted simplification: nothing in this kernel's networking use cases (ICMP echo payloads, or TCP — which already segments its own data below the MTU) produces or requires fragmented IP traffic, so building a reassembly path would add real complexity for no exercised benefit.
- The destination address doesn't match our configured IP.

Packets that pass all of the above are dispatched by `protocol`: `IP_PROTO_ICMP` (1) → `icmp_receive()`, `IP_PROTO_TCP` (6) → `tcp_receive()`. **There is no `IP_PROTO_UDP` (17) case, and no UDP implementation anywhere in the tree** — any other protocol number is silently dropped with no log line.

**API:**

| Function | Description |
|----------|-------------|
| `int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len)` | Routes, resolves the next-hop MAC, builds the header, and sends via `eth_send()`. Returns `-1` if `len` exceeds what fits unfragmented (`ETH_MTU_PAYLOAD - IP_HEADER_LEN` = 1480 bytes) or if ARP resolution of the next hop fails. |
| `void ip_receive(const uint8_t *frame, uint16_t len)` | Called from `eth_receive()` on IP frames; validates and dispatches as described above. |

---

## ICMP

`icmp.c`/`.h` implement only Echo Request/Reply (types 8 and 0) — no other ICMP message types (destination unreachable, time exceeded, redirect, etc.) are generated or interpreted.

**Responder side:** `icmp_receive()` answers any Echo Request addressed to us with an Echo Reply carrying the same identifier/sequence/payload, so the machine is itself pingable.

**Single-outstanding-ping model:** the `ping` shell command sends one Echo Request at a time and blocks for its reply before sending the next, backed by one static `icmp_ping_state_t` (`g_ping`) tracking the destination IP, id/seq, and send/receive timestamps of the *current* ping only:

```c
typedef struct {
    int      waiting;
    uint32_t dst_ip;
    uint16_t id;
    uint16_t seq;
    uint64_t send_tick;
    int      received;
    uint64_t recv_tick;
} icmp_ping_state_t;
```

**API:**

| Function | Description |
|----------|-------------|
| `int icmp_ping_send(uint32_t dst_ip, uint16_t id, uint16_t seq)` | Builds and sends an 8-byte ICMP header + 32-byte payload (`'a'..'w'` repeating), arming `g_ping` for the matching reply. Returns `-1` on an `ip_send()` failure (including an ARP timeout on the route). |
| `int icmp_ping_wait(uint32_t timeout_ms, uint32_t *out_rtt_ms)` | Blocks, polling the NIC and re-checking `g_ping.received`, until the matching Echo Reply arrives or `timeout_ms` elapses. On success, `*out_rtt_ms` is the round-trip time rounded to the PIT's 10 ms tick resolution. |

The ICMP checksum is written into the packet with manual byte splitting (`packet[2] = csum >> 8; packet[3] = csum`) rather than an `htons()` assignment — see [Byte Order and Checksum Helpers](#byte-order-and-checksum-helpers) for why that distinction matters here.

---

## TCP

`tcp.c`/`.h` implement a deliberately minimal, blocking TCP **client** — enough to run `wget`'s HTTP/1.0 GET against a well-behaved local link, not a general-purpose transport.

### Connection State

Exactly one connection exists at a time, held in a single global `tcp_conn_t`:

```c
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_SYN_SENT,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_REMOTE_CLOSED,
} tcp_conn_state_t;

typedef struct {
    tcp_conn_state_t state;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t snd_next;
    uint32_t rcv_next;

    uint8_t  stage_buf[TCP_MAX_SEGMENT_DATA]; // 1460 bytes
    uint16_t stage_len;
    int      has_data;
} tcp_conn_t;
```

Local ports are handed out from an incrementing counter starting at 49152 (`g_next_local_port`), wrapping back to 49152 on overflow. The initial sequence number is derived from `pit_get_ticks()` rather than a proper random source — acceptable because there is only ever one connection, so there's no risk of colliding with a stale one.

Received data is staged into a single 1460-byte buffer (`stage_buf`, matching `TCP_MAX_SEGMENT_DATA`) with a `has_data` flag: while `has_data` is set, `tcp_receive()` will not accept any further data segment, in-order or not, until the caller consumes the staged data via `tcp_ack_consumed()`. Combined with the ordering check below, this means the stack has **no reassembly buffer and no out-of-order handling** — a segment that arrives with `seq != rcv_next`, or while the previous chunk is still unconsumed, is silently dropped. There is also **no retransmission timer** anywhere in the stack: `tcp_send()` sends each segment exactly once and never resends it, relying entirely on the caller's own higher-level timeout (e.g. `wget`'s ~10 second no-data deadline, see [`14_shell_commands.md`](14_shell_commands.md#network-commands)) to notice a stall and give up. This scope was chosen deliberately: it's sufficient for `wget` fetching a file over QEMU's SLIRP network (or another local, effectively lossless link), where reordering and loss are rare enough that a general-purpose sliding-window/retransmission implementation wasn't worth the added complexity for the one use case this kernel actually has for TCP.

### Connection Lifecycle

| Function | Signature | Description |
|----------|-----------|--------------|
| Connect | `int tcp_connect(uint32_t remote_ip, uint16_t remote_port)` | Active open: sends SYN, blocks polling the NIC for a matching SYN-ACK (up to ~3000 ms, 300 PIT ticks), sends the final ACK. Returns `0` on `ESTABLISHED`, `-1` on timeout or an RST from the peer. Any prior connection state is simply overwritten — there's only ever one. |
| Send | `int tcp_send(const void *data, uint16_t len)` | Fails immediately (`-1`) unless the connection is `ESTABLISHED`. Otherwise splits `data` into up to 1460-byte (`TCP_MAX_SEGMENT_DATA`) chunks and sends each as a separate `PSH|ACK` segment, advancing `snd_next` as it goes. No retransmission if a segment is lost. |
| Receive (poll) | `int tcp_recv_poll(uint8_t **out_ptr, uint16_t *out_len)` | Polls the NIC once. If an in-order, unconsumed chunk is staged, returns `1` with `*out_ptr`/`*out_len` pointing at `stage_buf` (valid until the next `tcp_recv_poll()`/`tcp_ack_consumed()` call); otherwise returns `0`. |
| Acknowledge | `void tcp_ack_consumed(void)` | Must be called exactly once after each successful `tcp_recv_poll()`. Clears `has_data` (freeing the stage buffer for the next chunk) and sends the ACK for the consumed bytes — the ACK is deliberately deferred until here rather than sent the moment data arrives, so a stage buffer that got truncated at 1460 bytes never acknowledges bytes it actually discarded. |
| Remote-close check | `int tcp_is_remote_closed(void)` | Returns `1` once the peer has sent FIN (state `TCP_STATE_REMOTE_CLOSED`) — the caller's cue to stop reading and call `tcp_close()`. |
| Close | `void tcp_close(void)` | Sends FIN|ACK if the connection was `ESTABLISHED` or `REMOTE_CLOSED`, then best-effort waits (up to ~1000 ms, 100 PIT ticks) for the connection to reach `CLOSED`. Forces local state to `CLOSED` unconditionally afterward regardless of whether a clean close was observed — a client that no longer needs the socket isn't obligated to wait indefinitely for a graceful teardown. |
| Receive (dispatch) | `void tcp_receive(uint32_t src_ip, const void *payload, uint16_t len)` | Called from `ip_receive()` on TCP segments. Validates source IP/ports match the single active connection, handles RST (→ `CLOSED`), the SYN-ACK handshake step, in-order data staging, and FIN (→ `REMOTE_CLOSED`, with an immediate ACK). Anything that doesn't match the current connection is silently dropped. |

The TCP checksum, per RFC 793, covers a 12-byte IPv4 pseudo-header (source/dest IP, zero byte, protocol, TCP length) that is never itself transmitted; `tcp_send_segment()` builds pseudo-header + real header + data in one scratch buffer purely to compute the checksum over it, then sends only the real segment (header + data) via `ip_send()`.

---

## Byte Order and Checksum Helpers

Because x86-64 is little-endian and all network wire formats are big-endian, `net.h` defines the standard conversion pair:

```c
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) { /* byte-swaps all four bytes */ }
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }
```

`ntohs()`/`ntohl()` are just aliases for `htons()`/`htonl()` — byte-swapping is its own inverse.

**Internet checksum:** `uint16_t net_checksum(const void *data, uint32_t len)` implements the classic one's-complement-of-the-one's-complement-sum-of-16-bit-words algorithm (RFC 1071), used identically by IP, ICMP, and TCP. Its return value is already in "high byte first" form. This has a subtle but important consequence documented in `icmp.c`: most call sites write the result into a `uint16_t` header field with a normal `hdr->checksum = htons(net_checksum(...))` assignment (the `htons()` is what puts it in the right in-memory byte order for a little-endian store), but `icmp.c`'s two checksum sites (`icmp_ping_send()`, `icmp_receive()`) instead split the result by hand — `packet[2] = csum >> 8; packet[3] = csum;` — because they're writing directly into a raw `uint8_t` buffer rather than through a struct field. Calling `htons()` in that path would swap the bytes a **second** time and produce a wrong checksum; this was a real, previously-shipped bug confirmed by inspecting a live packet capture (QEMU SLIRP was silently dropping outgoing ICMP echoes with bad checksums, so no reply was ever seen) before being fixed.

**String formatting helpers** (`net.c`) exist because the console's `printf()` (`drivers/console/console.c`) has no `%02x`/zero-padding support at all — only bare `%x` (see [`07_drivers.md`](07_drivers.md#console-driver)) — so the stack cannot rely on `printf("%02x:%02x...")` to render addresses. Instead it hand-rolls its own:

| Function | Description |
|----------|-------------|
| `int ip_str_to_addr(const char *s, uint32_t *out_ip)` | Parses `"a.b.c.d"` into a host-order `uint32_t`. Stops at the end of the string or the first space (callers often hand it the rest of a shell command line). Returns `0` on success, `-1` on any malformed input. |
| `void ip_addr_to_str(uint32_t ip, char *out)` | Formats a host-order IP into `"a.b.c.d"` (`out` needs at least 16 bytes). |
| `void mac_addr_to_str(const uint8_t mac[6], char *out)` | Formats a MAC into lower-case `"xx:xx:xx:xx:xx:xx"` (`out` needs at least 18 bytes), via a small local `hex_digit_lower()` helper that pads every byte to exactly two digits — precisely what `printf("%x")` cannot do. |

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| Networking (`kernel/net/`) | RTL8139 driver (`drivers/net/rtl8139.c`) | Actual frame transmit/receive; `eth_send()`/`eth_receive()` sit directly on top of `rtl8139_send()`/the driver's receive-frame callback. |
| RTL8139 driver | PCI (`drivers/pci/pci.c`) | `pci_find_device(0x10EC, 0x8139)` locates the card and its I/O BAR. |
| RTL8139 driver | PMM (`system/mm/pmm.c`) | `pmm_alloc_contiguous_pages()` for the receive ring buffer (must be physically contiguous for the NIC's DMA), `pmm_alloc_page()` per transmit buffer. |
| Networking | PIT (`system/timer/pit.c`) | `net_poll()` is driven from the timer tick; `arp_resolve()`/`icmp_ping_wait()`/`tcp_connect()`/`tcp_close()` all measure their bounded timeouts in real PIT ticks (`pit_get_ticks()`) rather than wall-clock time. |
| `wget` (shell) | LufiraFS (`fs/lufirafs/lufirafs.c`) | Writes the downloaded body directly via `lufirafs_create()`/`lufirafs_truncate()`/`lufirafs_write()`/`lufirafs_sync()` — called straight against the global `lufirafs` instance, not routed through the VFS abstraction the way syscalls are (see [`13_syscalls.md`](13_syscalls.md#file-operations)). |
| Networking | Console (`drivers/console/console.c`) | `printf()` for shell-visible output, `DLOG()` (devmode-gated, see [`04_logging.md`](04_logging.md)) for internal diagnostics like dropped fragments or ARP failures. |

---

## Known Limitations

- **No DNS** – only literal IPv4 addresses are accepted anywhere in the stack or the shell commands built on it.
- **No DHCP** – IP configuration is a static default (`10.0.2.15/24`, gateway `10.0.2.2`), changed only by `ifconfig`, never persisted across reboots.
- **No UDP** – there is no UDP header type, no `udp.c`, and `ip_receive()`'s protocol dispatch has no case for protocol 17.
- **No IP fragmentation or reassembly** – fragmented packets are dropped on receive; nothing is ever fragmented on send.
- **No TCP retransmission or congestion control** – a lost segment is never resent; the stack relies entirely on the caller's own timeout.
- **Single connection/resolve/ping at a time** – one global TCP connection, one global ARP resolve state, one global ping state; no socket table, no concurrent transfers.
- **In-order-only TCP receive** – any out-of-order or duplicate segment is silently dropped rather than buffered.
- **Fully polled, no interrupts** – consistent with the rest of the driver stack, but it means networking throughput and latency are bounded by the 100 Hz PIT tick rate (`net_poll()` runs once per 10 ms) rather than reacting to hardware events immediately.
- **Untested on real hardware** – the stack has only been exercised against QEMU's user-mode networking (SLIRP); real NICs, real routers, or a lossier/reordering network path are all unverified territory.

---

## Conclusion

LufiraOS's network stack is a compact, from-scratch Ethernet/ARP/IPv4/ICMP/TCP implementation sized to exactly what its three shell commands need — configuring an interface, pinging a host, and fetching a file over HTTP — rather than a general-purpose TCP/IP stack. Its fully polled design mirrors the rest of the kernel's driver architecture, and its scope is documented deliberately narrow: no DNS, no DHCP, no UDP, no fragmentation, and no TCP retransmission or congestion control. That scope is more than adequate for a hobby kernel talking to a QEMU SLIRP link, and the blocking/single-connection model keeps the implementation small enough to read in one sitting.

For more details, refer to the source code in `kernel/net/` and `kernel/drivers/net/rtl8139.c`.

---

**Document Version:** 1.0
**Last Updated:** September 2026
**Project:** LufiraOS
