# NyxOS network stack

NyxOS has a **from-scratch TCP/IP stack** written in freestanding C — no ported
library. It speaks the real wire protocols (Ethernet, ARP, IPv4, ICMP, UDP, TCP) up
through DHCP, DNS, HTTP and **TLS 1.2**, exposes a BSD-style **socket layer** to
userspace, and ships a built-in loopback echo server so socket programs have
something to talk to with no NIC at all. Everything is integer-only (the kernel is
built `-mno-sse`) and, today, **poll-driven** rather than interrupt-driven.

This document is the map: the layers, how packets flow in and out, the concurrency
model, the socket API, and where each protocol lives.

---

## At a glance

| File | Layer / role |
|---|---|
| `kernel/drivers/net/rtl8139.c` | RTL8139 NIC driver — DMA RX/TX rings, `rtl8139_send_packet` / `rtl8139_receive_packet` |
| `kernel/net/ethernet.c` | Link layer — `eth_send` frames, `eth_poll` drains the NIC and dispatches by EtherType |
| `kernel/net/arp.c` | ARP — `arp_resolve` (IP→MAC), request/reply cache |
| `kernel/net/ip.c` | IPv4 — `ip_send`, `ip_handle_packet`, the loopback ring (`ip_loopback_poll`) |
| `kernel/net/icmp.c` | ICMP — `icmp_ping` / echo request+reply |
| `kernel/net/udp.c` | UDP — `udp_send`, `udp_register_listener`, `udp_handle_packet` |
| `kernel/net/tcp.c` | TCP — connections, handshake, retransmit queue, checksum validation |
| `kernel/net/dhcp.c` | DHCP client — `dhcp_request` leases an address |
| `kernel/net/dns.c` | DNS client — `dns_resolve`, random transaction IDs, a bounds-safe response parser |
| `kernel/net/http.c` | HTTP/1.1 client — `http_get`, `http_parse_response` |
| `kernel/crypto/tls/tls.c` | TLS 1.2 — `tls_https_fetch`, X.509 chain validation |
| `kernel/net/net.c` | The socket layer (`nsock_*`), the poll driver (`kernel_poll_net`), the loopback echo |

---

## The layer cake

```
   userspace socket() / connect() / send() / recv()      (syscalls)
        |
   net.c  nsock_*        <- socket table, UDP receive rings, blocking busy-poll
        |
   tcp.c / udp.c         <- transport: connections & datagrams
        |
   ip.c  (+ icmp.c)      <- IPv4 routing, checksums, the loopback ring
        |
   arp.c                 <- IP -> MAC resolution
        |
   ethernet.c            <- framing + EtherType dispatch (inbound)
        |
   rtl8139.c   /   loopback ring     <- the wire, or 127.0.0.1 in-memory
```

Higher-level clients — **DHCP, DNS, HTTP, TLS, ICMP ping**, and the **echo** service —
sit beside the socket layer and drive TCP/UDP directly.

---

## Interfaces

Up to eight interfaces live in `net_interfaces[8]` (`net_iface_t`, defined in
`kernel/core/kernel.h`):

```c
typedef struct {
    char     name[16];         // "lo", "eth0", ...
    uint8_t  mac[6];
    uint32_t ip, netmask, gateway, mtu, flags;
    uint32_t tx_packets, rx_packets;
    void*    driver_data;
} net_iface_t;
```

`init_net()` always creates **`lo` = 127.0.0.1** (MTU 65536), initialises ARP, then
probes for an RTL8139. IPv4 addresses are stored in **network byte order with the
first octet in the low byte** (so `127.0.0.1` is `0x0100007F`) — the same layout
`ip_send` puts on the wire, which is why no byte-swapping is needed between the
interface table and the transmit path.

---

## Poll-driven, not interrupt-driven

There is **no primary network IRQ handler**. Everything is pumped by one function,
`kernel_poll_net()` (`net.c`), which each blocking socket call (and the kernel
clients) invoke in a bounded busy-loop:

```c
ip_loopback_poll();        // deliver self-addressed (127.0.0.1) packets
for each non-lo iface: eth_poll(i);   // drain the NIC RX ring, dispatch inbound
ip_pending_poll();        // send queued packets once a neighbour is known
tcp_tick();                // drive TCP retransmit timers
tcp_echo_poll();           // service the built-in echo server (port 7)
```

Why polling? These calls run in a **process's syscall context**, and the PIT timer is
left masked, so a `tick_count`-based sleep would spin forever. Instead each blocking
call (`nsock_connect`, `nsock_recv`, `dhcp_request`, `dns_resolve`, `http_get`, …)
busy-polls with a short I/O delay between iterations, which advances **both** endpoints
over loopback without yielding.

**Interrupt groundwork (issue #62, in progress):** the RTL8139 RX/TX IRQ is now wired
(v6.4.92 for RX, v6.4.102 for TX completion) — the handler acks the ISR, bumps
`rx/tx` counters and sets an `rx_pending` flag, but does **not** re-enter the stack.
Polling stays the primary path so there is no regression; the remaining rungs (async
socket wakeup off `rx_pending`, a deferred/softirq ring drain) are still open.

---

## Concurrency — the `net_lock` giant lock

The TCP/UDP/IP stack is **non-reentrant**, and the socket table + per-socket UDP rings
are shared across cores. `net.c` guards all of it with a single spinlock, `net_lock`,
taken via `spin_lock_irqsave` (which both spins for cross-core exclusion **and** masks
interrupts so the timer can't switch tasks mid-operation). Two rules make it safe:

- **Every** atomic stack operation and every socket-table / `dq`-ring mutation takes it.
- Blocking calls **release** it during their idle busy-wait — the lock never spans a
  wait, so peers on other cores keep driving the stack.

It transitively covers `ip.c`'s loopback ring (only reached through a lock holder).
Because the NIC is poll-driven, nothing takes `net_lock` from interrupt context.

---

## The socket layer (`nsock_*`)

A socket is a thin handle over `tcp.c`/`udp.c`. The process fd table stores a tagged
`UFD_SOCK_MAKE(id)` and routes the relevant syscalls here:

| Call | TCP (`SOCK_STREAM`) | UDP (`SOCK_DGRAM`) |
|---|---|---|
| `nsock_create` | claim a slot | claim a slot + kmalloc a datagram ring |
| `nsock_connect` | `tcp_connect` + drive the 3-way handshake | — |
| `nsock_bind` / `nsock_listen` / `nsock_accept` | passive open; `accept` blocks for a child | bind records the port |
| `nsock_send` / `nsock_recv` | `tcp_send` / blocking `tcp_recv` | — |
| `nsock_sendto` / `nsock_recvfrom` | — | `udp_send` / dequeue from the ring |
| `nsock_retain` | add a descriptor owner | add a descriptor owner |
| `nsock_close` | final owner: `tcp_close` + free the slot | final owner: free the ring + slot |

Inbound datagrams are delivered by `nsock_udp_deliver` (called from `udp_handle_packet`
during a poll) into a small per-socket ring; `recvfrom` dequeues. If no socket owns the
port, the datagram falls through to a kernel listener (DHCP/DNS/echo).

### Descriptor ownership (6.5.254)

Each newly created or accepted socket starts with one descriptor reference.
`dup` and socket `dup2` add a reference; closing one alias leaves the others
usable. `dup2` retains the source before closing the destination, including
when both descriptors already name the same socket. Replacing a socket with
a file or pipe also releases the old socket through its own close operation.
Reference changes and final teardown share `net_lock`. An accepted TCP child
is closed if the socket table has no free slot to own it.

This closes sequential alias/reuse and replacement leaks. It does not serialize
the shared thread-group FD table or pin an in-flight read/connect across a
concurrent final close. Socket inheritance through `fork` remains unimplemented;
the child currently inherits pipe descriptors only. VFS `dup2` retains its
existing move behavior. These limits must not be inferred closed from reference
counting or the passing network workloads.

`tools/test_socket_lifetime.py` exercises the actual socket and syscall bodies
with mocked allocation, locks and transport. `exec /sockdemo.elf --fd-lifetime`
checks TCP/UDP aliases, repeated replacement, real loopback transfers and an
accepted connection with a separately created child socket.

---

## TCP

`tcp.c` implements a real connection state machine: the 3-way handshake, sequence /
acknowledgement tracking, and a clean close. Hardening landed as the tracked
issue-batch fixes and is locked by tests:

- **RX checksum validation** — `tcp_handle_packet` drops any segment whose checksum is
  wrong (issue #61), locked by the `tcpcksum` KAT.
- **Retransmit queue** — a second write before the first is ACKed no longer discards
  the unacked segment; an `rtx[]` queue with cumulative-ACK prefix-clearing and a
  per-segment RTO (issue #60).
- **Graceful close** — the FIN/ACK handshake completes properly (issue #59).
- **SMP-safe connection table** — one coarse `tcp_lock` over every public entry (#58).

## UDP, ICMP, ARP

- **UDP** (`udp.c`): `udp_send` builds and sends a datagram; `udp_register_listener`
  wires a kernel handler for a port; `udp_handle_packet` delivers to a socket first,
  then to a registered listener.
- **ICMP** (`icmp.c`): `icmp_ping` sends an echo request and matches the reply.
- **ARP** (`arp.c`): `arp_resolve` returns a cached MAC or sends a request and
  returns immediately. It does not enter receive polling from the transmit path.

### Unresolved neighbours (6.5.252)

`ip_send` can run with the socket and TCP locks already held. Polling the receive
stack there used to re-enter those locks through ARP resolution and hang a first
connection. IPv4 now retains a bounded copy of an unresolved packet; the normal
network poll loop learns the ARP reply and flushes pending transmissions.

| Bound | Value |
|---|---|
| Pending packets | 16 shared slots |
| Maximum queued IPv4 packet | 2048 bytes, including the IPv4 header |
| Queue lifetime | 3000 timer ticks (3 seconds with the current timer) |
| ARP retry interval | 1000 ticks per pending entry |

A full queue or oversized pending packet returns an error. A queued datagram is
accepted for transmission, not guaranteed to arrive. Pending entries expire;
changing the interface address, mask or gateway discards its old entries. A
temporary transmitter error retains the copy until a later poll or expiry.
TCP keeps its existing retransmission queue. Broadcasts, known neighbours and
loopback follow their existing immediate paths. This is not IP fragmentation,
multi-interface routing qualification or a redesign of socket lifetime.

`tools/test_ip_pending.py` compiles the actual IPv4 transmit and ARP lookup paths
with mocked Ethernet, clock and locks. Its bounds and retry checks complement
target tests; they do not establish real NIC or concurrent lock correctness.

---

## Higher-level clients

- **DHCP** (`dhcp.c`): `dhcp_request` runs the DISCOVER/OFFER/REQUEST/ACK exchange and
  fills in `ip` / `netmask` / `gateway`.
- **DNS** (`dns.c`): `dns_resolve` sends an A-query and parses the answer. It draws a
  **random 16-bit transaction ID** from the CSPRNG and rejects any reply whose ID
  doesn't match (anti-spoofing, issue #48); the response parser is bounds-safe against
  hostile packets and is locked by the `dns` KAT (v6.4.109).
- **HTTP** (`http.c`): `http_get` / `http_request` over TCP; `http_parse_response`
  parses status + headers + (chunked) body and is adversarially tested by `httpparse`.
- **TLS 1.2** (`kernel/crypto/tls/tls.c`): `tls_https_fetch` performs a handshake with
  supported X.509 checks against a small pinned trust set, enforced by default
  since 6.5.251. `tls_set_strict` selects the explicit development override; see
  [CRYPTO.md](CRYPTO.md) for validation and compatibility limits.
  The crypto underneath (SHA-256/512, AES-GCM, RSA, P-256/
  P-384, X25519) all has its own KATs.
- **Echo** (`net.c`, port 7, RFC 862): an always-on TCP+UDP loopback echo server so a
  userspace socket program has a peer with no external host required.

---

## Security posture

Untrusted network input is treated as hostile and the risky parsers are pinned by the
CI self-test battery (see `docs/ARCHITECTURE.md`): `tcpcksum` (TCP checksum), `dns`
(DNS response parser), `httpparse` (HTTP response parser). DNS uses random transaction
IDs; TLS enforces its supported authentication checks, with incomplete PKIX coverage.
Adding a new parser on the receive path
should come with an adversarial KAT in the same family.

---

## Extending the stack

- **A new UDP service:** `udp_register_listener(port, handler)` — the handler fires for
  datagrams to that port when no userspace socket has claimed it (see the echo server).
- **A new socket-backed protocol:** build on the `nsock_*` layer rather than touching
  `tcp.c`/`udp.c` directly.
- **Anything on the receive path** parses untrusted bytes — keep it bounds-safe and add
  a KAT (mirror `dns` / `httpparse` / `tcpcksum`).

See also `docs/ARCHITECTURE.md` (whole-system overview) and `docs/WALLPAPERS.md`
(the desktop render system).
