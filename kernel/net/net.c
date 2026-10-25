#include "../core/kernel.h"
#include "tcp.h"
#include "../core/spinlock.h"

extern net_iface_t net_interfaces[8];

// Userspace sockets. A socket is a thin handle over the tcp.c/udp.c stack; the
// process fd table (syscall.c) stores UFD_SOCK_MAKE(id) and routes read/write/
// close (TCP) or sendto/recvfrom (UDP) here. SOCK_STREAM = TCP, SOCK_DGRAM = UDP.
#define MAX_SOCKETS 32
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

// A UDP socket buffers received datagrams (with their source) in a small ring;
// inbound datagrams arrive via udp_handle_packet -> nsock_udp_deliver, and
// recvfrom dequeues. The ring is kmalloc'd per DGRAM socket so the nsock table
// stays small (a datagram entry is ~1.5 KB).
#define UDP_DGRAM_MAX 1472       // max payload (Ethernet MTU - IP(20) - UDP(8))
#define UDP_QUEUE     4          // datagrams buffered per socket
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[UDP_DGRAM_MAX];
} dgram_t;

typedef struct {
    int in_use;
    int type;            // SOCK_STREAM (TCP) or SOCK_DGRAM (UDP)
    int tcp_conn;        // TCP: connected/accepted conn id, LISTEN conn id, or -1
    uint16_t local_port; // bind()'s port (or an ephemeral auto-bind on first sendto)
    int listening;       // TCP: 1 once listen() put it into passive-open
    dgram_t* dq;         // UDP: receive ring (UDP_QUEUE entries), else NULL
    int dq_head, dq_tail;
} nsock_t;

static nsock_t nsocks[MAX_SOCKETS];

// SMP net lock (T1 track, v5.9.24). The TCP/UDP/IP stack is NON-reentrant, and the
// socket table + per-socket UDP rings above are shared. The old guard was
// preempt_disable(): it kept irq_scheduler_tick from switching tasks mid-operation
// on the local core, so one process couldn't re-enter the stack while another was
// inside it — but it means NOTHING to a second core, which under smpbalance can enter
// the stack at the same instant. net_lock is the subsystem's single "giant lock":
// every atomic stack operation and every socket-table / dq-ring mutation takes it.
// spin_lock_irqsave does BOTH jobs at once — the spin gives cross-core exclusion, the
// cli gives the same no-task-switch-here property preempt_disable had (the timer
// can't fire to trigger a switch). The blocking socket calls RELEASE it during their
// idle busy-wait — exactly where they released preemption before — so peers on any
// core keep driving the stack; the lock never spans a wait. It transitively covers
// ip.c's loopback ring, since every ip_send / ip_loopback_poll reaches that ring only
// through a net_lock holder. The NIC is poll-driven (eth_poll under kernel_poll_net —
// there is no net IRQ handler in the irq_install_handler table), so nothing takes
// net_lock from interrupt context. Nesting: net_lock -> kmalloc_lock only (ip_send /
// create / close allocate); never the reverse, and no other subsystem lock nests with
// it. NOTE: the kernel-internal clients (dhcp/dns/ping/http) still enter the stack
// outside net_lock — not reachable by a userspace socket workload, left as follow-up.
static spinlock_t net_lock = SPINLOCK_INIT;

extern int rtl8139_init(void);

extern void arp_init(void);

void init_net(void) {
    memset_asm(nsocks, 0, sizeof(nsocks));
    memset_asm(net_interfaces, 0, sizeof(net_interfaces));

    net_iface_t* lo = &net_interfaces[0];
    strcpy(lo->name, "lo");
    lo->ip = 0x0100007F;        // 127.0.0.1 in network order (first octet = low byte)
    lo->netmask = 0x000000FF;   // 255.0.0.0
    lo->mtu = 65536;
    lo->flags = 1;
    printf("[NET] Loopback: 127.0.0.1\n");

    arp_init();

    int ret = rtl8139_init();
    if (ret == 0) printf("[NET] RTL8139 NIC initialized\n");
    else printf("[NET] No RTL8139 NIC found (will retry on kernel_poll)\n");

    printf("[NET] Stack ready\n");
}

void kernel_poll_net(void) {
    extern void eth_poll(int iface_idx);
    extern void ip_loopback_poll(void);
    extern void ip_pending_poll(void);
    extern void tcp_tick(void);
    // Serialized (v5.8.78; SMP net_lock v5.9.24): the loopback ring drain and the TCP
    // state machine are NOT reentrant, and syscalls run preemptibly, so two processes
    // polling the net concurrently used to interleave here — one draining the ring
    // while another's SYN-ACK was in flight, garbling each other's state until a
    // connect() timed out. preempt_disable() fixed that within one core; net_lock now
    // serializes it across cores too (the cli inside irqsave still keeps this core from
    // switching tasks mid-poll). Held only for the poll itself — the blocking socket
    // calls release it during their idle delay, so peers on any core keep progressing.
    uint64_t fl = spin_lock_irqsave(&net_lock);
    ip_loopback_poll();   // deliver any self-addressed (loopback) packets
    for (int i = 0; i < 8; i++) {
        if (net_interfaces[i].name[0] && strcmp(net_interfaces[i].name, "lo") != 0) {
            eth_poll(i);
        }
    }
    ip_pending_poll();    // send queued packets after learning neighbour MACs
    tcp_tick();           // drive TCP retransmit timers
    tcp_echo_poll();      // service the built-in loopback echo (port 7)
    spin_unlock_irqrestore(&net_lock, fl);
}

// ---- Userspace TCP socket layer -------------------------------------------
// Each blocking call drives the stack by polling in place (kernel_poll_net),
// the same way dhcp_request/dns_resolve do: these run in a process's syscall
// context, so a bounded busy-poll with an I/O delay between iterations advances
// BOTH endpoints (the client and the loopback echo server) without yielding.

static int nsock_valid(int s) {
    return s >= 0 && s < MAX_SOCKETS && nsocks[s].in_use;
}

int nsock_create(int domain, int type, int protocol) {
    (void)domain; (void)protocol;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -1;
    // net_lock: the free-slot scan + claim must be atomic against a concurrent
    // create/accept on another core, or both would seize the same slot.
    uint64_t fl = spin_lock_irqsave(&net_lock);
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!nsocks[i].in_use) {
            nsocks[i].type = type;
            nsocks[i].tcp_conn = -1;
            nsocks[i].local_port = 0;
            nsocks[i].listening = 0;
            nsocks[i].dq = NULL;
            nsocks[i].dq_head = nsocks[i].dq_tail = 0;
            if (type == SOCK_DGRAM) {
                nsocks[i].dq = (dgram_t*)kmalloc(sizeof(dgram_t) * UDP_QUEUE);
                if (!nsocks[i].dq) { spin_unlock_irqrestore(&net_lock, fl); return -1; }  // slot left free
            }
            nsocks[i].in_use = 1;                 // last, so a kmalloc fail leaves it free
            spin_unlock_irqrestore(&net_lock, fl);
            return i;
        }
    }
    spin_unlock_irqrestore(&net_lock, fl);
    return -1;
}

int nsock_connect(int s, uint32_t ip, uint16_t port) {
    if (!nsock_valid(s) || nsocks[s].type != SOCK_STREAM) return -1;   // TCP only
    static uint16_t ephemeral = 40000;            // client source-port pool
    // The port pick + conn-table alloc + SYN must be one atomic step. `ephemeral++`
    // is a non-atomic read-modify-write: two concurrent connects that interleaved
    // here got the SAME source port, so the second client's SYN matched the first's
    // server conn (find_conn_by_tuple keys on src_port) instead of spawning its own
    // child — no SYN-ACK ever came back and that connect() hung in SYN_SENT. Holding
    // preemption across the whole pick fixes it (a concurrent connect can't run).
    uint64_t fl = spin_lock_irqsave(&net_lock);
    uint16_t sport = ephemeral++;
    if (ephemeral >= 60000) ephemeral = 40000;
    int c = tcp_connect(ip, port, sport);
    spin_unlock_irqrestore(&net_lock, fl);
    if (c < 0) return -1;
    nsocks[s].tcp_conn = c;
    // Drive the 3-way handshake to completion: tcp_connect fired the SYN; the
    // SYN-ACK is processed on poll. Bounded busy-wait (same pattern as HTTP).
    for (int i = 0; i < 3000; i++) {
        kernel_poll_net();
        int st = tcp_state(c);
        if (st == TCP_STATE_ESTABLISHED) return 0;
        if (st == TCP_STATE_CLOSED) break;        // reset / handshake gave up
        sleep(1);   // yield the CPU to the scheduler between polls instead of busy-spinning (#62)
    }
    return -1;
}

int nsock_bind(int s, uint32_t ip, uint16_t port) {
    (void)ip;                                     // single-homed: bind records the port
    if (!nsock_valid(s)) return -1;
    uint64_t fl = spin_lock_irqsave(&net_lock);
    nsocks[s].local_port = port;
    spin_unlock_irqrestore(&net_lock, fl);
    return 0;
}

int nsock_listen(int s, int backlog) {
    (void)backlog;
    if (!nsock_valid(s) || nsocks[s].local_port == 0) return -1;
    uint64_t fl = spin_lock_irqsave(&net_lock);
    int l = tcp_listen(nsocks[s].local_port);     // passive open in tcp.c
    spin_unlock_irqrestore(&net_lock, fl);
    if (l < 0) return -1;
    nsocks[s].tcp_conn = l;
    nsocks[s].listening = 1;
    return 0;
}

int nsock_accept(int s) {
    if (!nsock_valid(s) || !nsocks[s].listening || nsocks[s].tcp_conn < 0) return -1;
    int lc = nsocks[s].tcp_conn;
    // Block (busy-poll) until a client's handshake completes on our listen port,
    // then hand it out as a fresh socket. tcp_accept returns an ESTABLISHED child.
    for (int i = 0; i < 6000; i++) {
        // tcp_accept + the free-slot claim are one net_lock critical section, so a
        // second accept on another core can't grab the same child or the same slot.
        uint64_t fl = spin_lock_irqsave(&net_lock);
        int child = tcp_accept(lc);
        int result = -2;                          // -2 => nothing yet, keep polling
        if (child >= 0) {
            result = -1;                          // table full unless a slot is found
            for (int j = 0; j < MAX_SOCKETS; j++) {
                if (!nsocks[j].in_use) {
                    nsocks[j].in_use = 1;
                    nsocks[j].type = SOCK_STREAM;
                    nsocks[j].tcp_conn = child;
                    nsocks[j].local_port = nsocks[s].local_port;
                    nsocks[j].listening = 0;
                    result = j;                   // caller wraps this in a new fd
                    break;
                }
            }
        }
        spin_unlock_irqrestore(&net_lock, fl);
        if (result != -2) return result;          // got a child (slot j, or -1 full)
        kernel_poll_net();
        sleep(1);   // yield the CPU to the scheduler between polls instead of busy-spinning (#62)
    }
    return -1;                                    // timed out with no client
}

int nsock_send(int s, const void* buf, int len) {
    if (!nsock_valid(s) || nsocks[s].tcp_conn < 0 || len < 0) return -1;
    // tcp_send returns the on-wire segment length (IP+TCP+payload); a socket
    // write() must report the number of *payload* bytes accepted, so map any
    // success to `len` and only a hard failure to -1.
    uint64_t fl = spin_lock_irqsave(&net_lock);   // atomic conn mutate + loopback enqueue
    int r = tcp_send(nsocks[s].tcp_conn, (const uint8_t*)buf, (uint32_t)len);
    spin_unlock_irqrestore(&net_lock, fl);
    return (r < 0) ? -1 : len;
}

int nsock_recv(int s, void* buf, int len) {
    if (!nsock_valid(s) || nsocks[s].tcp_conn < 0 || len <= 0) return -1;
    int c = nsocks[s].tcp_conn;
    // Block (busy-poll) until some data arrives, the peer fully closes, or a
    // timeout. Returns >0 bytes, or 0 for EOF — like a real recv().
    for (int i = 0; i < 6000; i++) {
        uint64_t fl = spin_lock_irqsave(&net_lock);   // atomic recv_buf drain vs. a poll's append
        int n = tcp_recv(c, (uint8_t*)buf, (uint32_t)len);
        spin_unlock_irqrestore(&net_lock, fl);
        if (n > 0) return n;
        if (tcp_state(c) == TCP_STATE_CLOSED) return 0;   // closed, nothing buffered
        kernel_poll_net();
        sleep(1);   // lock released: yield to the scheduler so peers/RX progress (not busy-spin) (#62)
    }
    return 0;                                     // timed out -> treat as EOF
}

// ---- UDP (SOCK_DGRAM) socket layer ------------------------------------------

int nsock_sendto(int s, const void* buf, int len, uint32_t ip, uint16_t port) {
    if (!nsock_valid(s) || nsocks[s].type != SOCK_DGRAM || len < 0) return -1;
    // A UDP datagram must fit in one packet: there is no IP fragmentation here, so an
    // over-MTU send is silently dropped by loopback (LO_MAX_PKT) or by a real NIC/switch
    // (frame > 1500) while ip_send still reports "sent" — a false success. Reject it up
    // front with an error (POSIX EMSGSIZE) so the app knows to chunk. RX already clamps to
    // the same bound (nsock_udp_deliver), so this makes TX symmetric with RX.
    if (len > UDP_DGRAM_MAX) return -1;
    uint64_t fl = spin_lock_irqsave(&net_lock);   // auto-bind counter + non-reentrant udp_send
    if (nsocks[s].local_port == 0) {              // auto-bind an ephemeral source port
        static uint16_t eph = 48000;
        nsocks[s].local_port = eph++;
        if (eph >= 60000) eph = 48000;
    }
    int r = udp_send(ip, port, nsocks[s].local_port, (const uint8_t*)buf, (uint32_t)len, -1);
    spin_unlock_irqrestore(&net_lock, fl);
    return (r < 0) ? -1 : len;                    // report payload bytes, not on-wire length
}

int nsock_recvfrom(int s, void* buf, int len, uint32_t* src_ip, uint16_t* src_port) {
    if (!nsock_valid(s) || nsocks[s].type != SOCK_DGRAM || !nsocks[s].dq || len <= 0) return -1;
    // Block (busy-poll) until a datagram is queued, or a timeout. Datagrams are
    // enqueued by nsock_udp_deliver from the receive path during these polls.
    for (int i = 0; i < 6000; i++) {
        uint64_t fl = spin_lock_irqsave(&net_lock);
        if (nsocks[s].dq && nsocks[s].dq_head != nsocks[s].dq_tail) {   // dq re-checked: a close may have freed it
            dgram_t* d = &nsocks[s].dq[nsocks[s].dq_tail];
            int n = (int)d->len < len ? (int)d->len : len;
            memcpy(buf, d->data, n);
            if (src_ip)   *src_ip   = d->src_ip;
            if (src_port) *src_port = d->src_port;
            nsocks[s].dq_tail = (nsocks[s].dq_tail + 1) % UDP_QUEUE;
            spin_unlock_irqrestore(&net_lock, fl);
            return n;
        }
        spin_unlock_irqrestore(&net_lock, fl);
        kernel_poll_net();
        sleep(1);   // yield the CPU to the scheduler between polls instead of busy-spinning (#62)
    }
    return -1;                                     // timed out, no datagram
}

// Called from udp_handle_packet: enqueue an inbound datagram to the DGRAM socket
// bound to dst_port, if any. Returns 1 if a socket claimed this port (delivered,
// or dropped-because-full), 0 if none (so the caller falls back to kernel
// listeners like dhcp/dns). Always reached with net_lock already held (via
// kernel_poll_net's RX path), so it must NOT take the lock itself — the caller's
// hold serializes this dq enqueue against a recvfrom dequeue on another core.
int nsock_udp_deliver(uint16_t dst_port, uint8_t* data, uint32_t len,
                      uint32_t src_ip, uint16_t src_port) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!nsocks[i].in_use || nsocks[i].type != SOCK_DGRAM || !nsocks[i].dq) continue;
        if (nsocks[i].local_port != dst_port) continue;
        int next = (nsocks[i].dq_head + 1) % UDP_QUEUE;
        if (next == nsocks[i].dq_tail) return 1;  // ring full -> drop (still ours)
        dgram_t* d = &nsocks[i].dq[nsocks[i].dq_head];
        uint32_t n = len > UDP_DGRAM_MAX ? UDP_DGRAM_MAX : len;
        memcpy(d->data, data, n);
        d->len = (uint16_t)n;
        d->src_ip = src_ip;
        d->src_port = src_port;
        nsocks[i].dq_head = next;
        return 1;
    }
    return 0;
}

// poll(): 1 if this socket has data (or EOF) ready to read now, else 0. UDP is
// ready when a datagram is queued; TCP defers to tcp_recv_ready.
int nsock_readable(int s) {
    if (!nsock_valid(s)) return 1;                  // invalid -> a read errors: "ready"
    uint64_t fl = spin_lock_irqsave(&net_lock);
    int r;
    if (nsocks[s].type == SOCK_DGRAM)
        r = (nsocks[s].dq && nsocks[s].dq_head != nsocks[s].dq_tail) ? 1 : 0;
    else if (nsocks[s].tcp_conn < 0)
        r = 1;
    else
        r = tcp_recv_ready(nsocks[s].tcp_conn);
    spin_unlock_irqrestore(&net_lock, fl);
    return r;
}

int nsock_close(int s) {
    if (!nsock_valid(s)) return -1;
    // Whole teardown under net_lock: tcp_close mutates the conn table, and clearing
    // dq / in_use must be atomic against a concurrent poll (nsock_udp_deliver) or
    // recvfrom that would otherwise read a freed dq or a stale in_use.
    uint64_t fl = spin_lock_irqsave(&net_lock);
    if (nsocks[s].tcp_conn >= 0) tcp_close(nsocks[s].tcp_conn);
    if (nsocks[s].dq) { kfree(nsocks[s].dq); nsocks[s].dq = NULL; }   // UDP receive ring
    nsocks[s].in_use = 0;
    nsocks[s].tcp_conn = -1;
    spin_unlock_irqrestore(&net_lock, fl);
    return 0;
}

// ---- Built-in loopback echo service (port 7, TCP + UDP; RFC 862) -----------
// A tiny always-on, inetd-style echo server so a userspace socket program has
// something to talk to over loopback (127.0.0.1) — fully self-contained, no
// external host or NIC required. It also answers on the NIC address if reached.
#define ECHO_PORT 7
#define ECHO_MAX  16          // concurrent echo clients (server side); see TCP_MAX_CONNS
static int echo_listen = -1;
static int echo_conns[ECHO_MAX];

// UDP echo: bounce each datagram straight back to its sender. Registered as a
// kernel udp listener, so it only fires when no userspace socket claims port 7
// (nsock_udp_deliver runs first in udp_handle_packet).
static void udp_echo_handler(uint8_t* data, uint32_t len, uint32_t src_ip, uint16_t src_port) {
    udp_send(src_ip, src_port, ECHO_PORT, data, len, -1);
}

void tcp_echo_init(void) {
    for (int i = 0; i < ECHO_MAX; i++) echo_conns[i] = -1;
    echo_listen = tcp_listen(ECHO_PORT);
    udp_register_listener(ECHO_PORT, udp_echo_handler);   // echo UDP datagrams too
}

void tcp_echo_poll(void) {
    if (echo_listen < 0) return;
    int c;
    while ((c = tcp_accept(echo_listen)) >= 0) {          // adopt newly-accepted clients
        for (int i = 0; i < ECHO_MAX; i++)
            if (echo_conns[i] < 0) { echo_conns[i] = c; break; }
    }
    for (int i = 0; i < ECHO_MAX; i++) {                  // bounce back any received bytes
        int ec = echo_conns[i];
        if (ec < 0) continue;
        if (tcp_state(ec) == TCP_STATE_CLOSED) { echo_conns[i] = -1; continue; }
        uint8_t buf[512];
        int n = tcp_recv(ec, buf, sizeof(buf));
        if (n > 0) tcp_send(ec, buf, (uint32_t)n);
    }
}
