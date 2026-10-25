#include "../core/kernel.h"
#include "../core/spinlock.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ip_header_t;

extern int eth_send(const uint8_t* dst_mac, uint16_t type, const uint8_t* data, uint32_t len, int iface_idx);
extern int arp_cache_lookup(uint32_t ip, uint8_t* mac);
extern void arp_send_request(uint32_t ip, int iface_idx);
extern int arp_resolve(uint32_t ip, uint8_t* mac, int iface_idx);
extern void udp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip);
extern void icmp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip);
extern void tcp_handle_packet(uint8_t* packet, uint32_t len, uint32_t src_ip, uint32_t dst_ip);
void ip_handle_packet(uint8_t* packet, uint32_t len);   // fwd decl for loopback re-inject

static uint16_t ip_checksum(const uint8_t* data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i += 2) {
        sum += ((uint16_t)data[i] << 8) | data[i+1];
        if (sum & 0xFFFF0000) { sum = (sum & 0xFFFF) + (sum >> 16); }
    }
    return ~(sum & 0xFFFF);
}

// ---- Loopback delivery ------------------------------------------------------
// Packets addressed to one of our own IPs (127.0.0.1 or a NIC's own address)
// must never hit the wire — a real stack delivers them via the loopback path.
// We copy the finished IP packet into a small ring and re-inject it into
// ip_handle_packet() from ip_loopback_poll(), which the net poll loop drains.
// Using a ring (instead of recursing straight into the handler) keeps
// reentrancy bounded when a handler replies to a looped packet (e.g. an ICMP
// echo request whose reply is itself looped back).
#define LO_QUEUE_LEN 16
#define LO_MAX_PKT   2048
static uint8_t  lo_ring[LO_QUEUE_LEN][LO_MAX_PKT];
static uint16_t lo_ring_len[LO_QUEUE_LEN];
static volatile int lo_head = 0, lo_tail = 0;

// TTL of the most recently accepted inbound IP packet — reported by ping.
static uint8_t g_last_rx_ttl = 64;
uint8_t ip_last_rx_ttl(void) { return g_last_rx_ttl; }

static int is_local_ip(uint32_t ip) {
    if ((ip & 0xFF) == 0x7F) return 1;           // 127.0.0.0/8 (net order: low byte)
    for (int i = 0; i < 8; i++)
        if (net_interfaces[i].name[0] && net_interfaces[i].ip == ip) return 1;
    return 0;
}

// Which interface's address sources a looped packet: the one that owns the
// destination IP, else loopback (index 0, 127.0.0.1).
static int loopback_src_iface(uint32_t dst_ip) {
    for (int i = 0; i < 8; i++)
        if (net_interfaces[i].name[0] && net_interfaces[i].ip == dst_ip) return i;
    return 0;
}

static void loopback_enqueue(const uint8_t* pkt, uint32_t len) {
    if (len > LO_MAX_PKT) return;
    int next = (lo_head + 1) % LO_QUEUE_LEN;
    if (next == lo_tail) return;                 // ring full: drop (an overrun)
    memcpy(lo_ring[lo_head], pkt, len);
    lo_ring_len[lo_head] = (uint16_t)len;
    lo_head = next;
    net_interfaces[0].tx_packets++;              // lo: a packet queued for self-delivery
}

// Drain queued loopback packets into the normal receive path. A handler may
// enqueue a reply mid-drain (echo request -> echo reply); the while-loop picks
// it up in the same pass, so a loopback ping resolves in one poll.
void ip_loopback_poll(void) {
    while (lo_tail != lo_head) {
        int i = lo_tail;
        lo_tail = (lo_tail + 1) % LO_QUEUE_LEN;
        net_interfaces[0].rx_packets++;          // lo: a self-addressed packet delivered
        ip_handle_packet(lo_ring[i], lo_ring_len[i]);
    }
}

// A cold neighbour must not poll the stack while a TCP/socket lock is held.
// Keep a bounded copy until the normal poll loop learns its MAC. TCP still owns
// retransmission; datagrams are accepted when queued, not when delivered.
#define IP_PENDING_COUNT 16
#define IP_PENDING_BYTES 2048
#define IP_PENDING_TTL   3000u
#define IP_PENDING_RETRY 1000u
typedef struct {
    uint8_t data[IP_PENDING_BYTES];
    uint32_t len, next_hop, queued, requested;
    uint32_t source, netmask, gateway;
    int iface;
} ip_pending_t;
static ip_pending_t ip_pending[IP_PENDING_COUNT];
static spinlock_t ip_pending_lock = SPINLOCK_INIT;

static int ip_pending_enqueue(const uint8_t* packet, uint32_t len,
                              uint32_t next_hop, int iface) {
    if (len > IP_PENDING_BYTES) return -1;
    uint64_t fl = spin_lock_irqsave(&ip_pending_lock);
    int result = -1;
    for (int i = 0; i < IP_PENDING_COUNT; i++) {
        ip_pending_t* p = &ip_pending[i];
        if (p->len) continue;
        memcpy(p->data, packet, len);
        p->len = len; p->next_hop = next_hop; p->iface = iface;
        p->queued = p->requested = get_ticks();
        p->source = net_interfaces[iface].ip;
        p->netmask = net_interfaces[iface].netmask;
        p->gateway = net_interfaces[iface].gateway;
        result = (int)len;
        break;
    }
    spin_unlock_irqrestore(&ip_pending_lock, fl);
    return result;
}

void ip_pending_poll(void) {
    uint64_t fl = spin_lock_irqsave(&ip_pending_lock);
    uint32_t now = get_ticks();
    for (int i = 0; i < IP_PENDING_COUNT; i++) {
        ip_pending_t* p = &ip_pending[i];
        if (!p->len) continue;
        net_iface_t* iface = &net_interfaces[p->iface];
        if (!iface->name[0] || iface->ip != p->source ||
            iface->netmask != p->netmask || iface->gateway != p->gateway ||
            (uint32_t)(now - p->queued) >= IP_PENDING_TTL) {
            p->len = 0;
            continue;
        }
        uint8_t mac[6];
        if (arp_cache_lookup(p->next_hop, mac)) {
            if (eth_send(mac, 0x0800, p->data, p->len, p->iface) >= 0)
                p->len = 0;
        } else if ((uint32_t)(now - p->requested) >= IP_PENDING_RETRY) {
            arp_send_request(p->next_hop, p->iface);
            p->requested = now;
        }
    }
    spin_unlock_irqrestore(&ip_pending_lock, fl);
}

int ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t* data, uint32_t len, int iface_idx) {
    if (len > 65535u - sizeof(ip_header_t) || (len && !data)) return -1;
    int is_loopback = is_local_ip(dst_ip);
    int pending = 0;
    uint32_t next_hop = dst_ip;

    uint8_t mac[6];
    if (is_loopback) {
        // Deliver to ourselves: source from the interface that owns dst (lo for
        // 127.0.0.1) and skip ARP/eth — the packet is queued for re-injection.
        iface_idx = loopback_src_iface(dst_ip);
    } else {
        if (iface_idx < 0) {
            // Skip loopback ("lo", index 0) when auto-selecting — its netmask/gateway
            // are zero, which would disable subnet/gateway routing for e.g. TCP
            // (send_segment passes -1). Prefer the first real NIC.
            for (int i = 0; i < 8; i++) {
                if (net_interfaces[i].name[0] && strcmp(net_interfaces[i].name, "lo") != 0) {
                    iface_idx = i; break;
                }
            }
            if (iface_idx < 0) return -1;
        }

        if (iface_idx >= 8 || !net_interfaces[iface_idx].name[0]) return -1;

        // Broadcast destinations (limited 255.255.255.255 or the subnet directed
        // broadcast) go to the broadcast MAC directly — ARP-resolving them never
        // gets a reply and would hang the send (this broke DHCP DISCOVER).
        uint32_t nm = net_interfaces[iface_idx].netmask;
        uint32_t my_ip = net_interfaces[iface_idx].ip;
        uint32_t subnet_bcast = nm ? (my_ip | ~nm) : 0xFFFFFFFF;
        if (dst_ip == 0xFFFFFFFF || dst_ip == subnet_bcast) {
            for (int i = 0; i < 6; i++) mac[i] = 0xFF;
        } else {
            // Off-subnet destinations are reached via the default gateway: ARP-resolve
            // the gateway's MAC, not the (remote) destination's. Without this, every
            // internet destination failed ARP and no packet was sent.
            if (nm && (dst_ip & nm) != (my_ip & nm) && net_interfaces[iface_idx].gateway)
                next_hop = net_interfaces[iface_idx].gateway;
            pending = !arp_resolve(next_hop, mac, iface_idx);
        }
    }

    uint32_t packet_len = sizeof(ip_header_t) + len;
    uint8_t* packet = (uint8_t*)kmalloc(packet_len);
    if (!packet) return -1;

    ip_header_t* ip = (ip_header_t*)packet;
    ip->ver_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_len = htons((uint16_t)packet_len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = net_interfaces[iface_idx].ip;
    ip->dst_ip = dst_ip;

    if (len > 0 && data) memcpy(packet + sizeof(ip_header_t), data, len);

    // ip_checksum returns the network-order value as a host integer; htons puts
    // the bytes in network order in the header (a plain store reversed them,
    // giving a bad checksum that made QEMU/slirp silently drop every IP packet).
    uint16_t ck = ip_checksum(packet, sizeof(ip_header_t));
    ip->checksum = htons(ck);

    if (is_loopback) {
        loopback_enqueue(packet, packet_len);
        kfree(packet);
        return (int)packet_len;
    }

    int result = pending ? ip_pending_enqueue(packet, packet_len, next_hop, iface_idx)
                         : eth_send(mac, 0x0800, packet, packet_len, iface_idx);
    kfree(packet);
    return result;
}

// ---- IPv4 header validation (pure, testable core of the RX gate) ------------
// Validate an IPv4 header from UNTRUSTED wire data before any field is trusted.
// The real header length comes from IHL (don't assume 20 bytes) and must fit the
// packet; the effective length clamps to the IP total-length field rather than the
// frame length, so Ethernet padding past total_len is not miscounted as payload (a
// padded 58->60 byte SYN-ACK once added 2 phantom bytes to the TCP ack); and the
// header checksum must verify over exactly the header span (before this gate the RX
// checksum went unchecked and IP options with IHL>5 were counted as payload, so a
// bad-checksum or malformed-IHL header reached TCP/UDP/ICMP unchecked). Returns 1
// and, on accept, the header length in *out_ihl and the clamped total length in
// *out_iplen; returns 0 for any malformed header. Kept pure (no dispatch) so the CI
// self-test can pin the accept/drop decision against hostile packets.
static int ipv4_hdr_check(const uint8_t* packet, uint32_t len,
                          uint32_t* out_ihl, uint32_t* out_iplen) {
    if (len < sizeof(ip_header_t)) return 0;
    const ip_header_t* ip = (const ip_header_t*)packet;
    uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0F) * 4;
    uint16_t ip_total = ntohs(ip->total_len);
    uint32_t ip_len = (ip_total >= sizeof(ip_header_t) && ip_total <= len) ? ip_total : len;
    if (ihl < sizeof(ip_header_t) || ihl > ip_len) return 0;   // malformed header length
    if (ip_checksum(packet, ihl) != 0) return 0;               // corrupt header
    if (out_ihl)   *out_ihl   = ihl;
    if (out_iplen) *out_iplen = ip_len;
    return 1;
}

void ip_handle_packet(uint8_t* packet, uint32_t len) {
    uint32_t ihl, ip_len;
    if (!ipv4_hdr_check(packet, len, &ihl, &ip_len)) return;   // drop malformed/corrupt headers
    ip_header_t* ip = (ip_header_t*)packet;

    uint32_t dst_ip = ip->dst_ip;
    for (int i = 0; i < 8; i++) {
        if (!net_interfaces[i].name[0]) continue;
        if (net_interfaces[i].ip == dst_ip || dst_ip == 0xFFFFFFFF) {
            uint8_t protocol = ip->protocol;
            g_last_rx_ttl = ip->ttl;   // reported by ping
            uint8_t* payload = packet + ihl;
            uint32_t payload_len = ip_len - ihl;
            uint32_t src_ip = ip->src_ip;
            if (protocol == IP_PROTO_UDP) udp_handle_packet(payload, payload_len, src_ip);
            else if (protocol == IP_PROTO_ICMP) icmp_handle_packet(payload, payload_len, src_ip);
            else if (protocol == IP_PROTO_TCP) tcp_handle_packet(payload, payload_len, src_ip, dst_ip);
            return;
        }
    }
}

// KAT for the IPv4 RX header gate (ipv4_hdr_check), which runs on UNTRUSTED wire
// data. Pins the one's-complement checksum math against the canonical worked-example
// header (RFC 1071 style), then asserts the accept/drop decision across a battery of
// malformed headers: a short frame, a bad IHL (too small, or options that run off the
// packet), a corrupted header byte, and the two length-clamp cases (Ethernet padding
// past total_len, and a total_len that overruns the frame). Runs offline in the CI
// self-test battery. Convention: 0 = PASS, else the failing case number.
int ipv4_rx_selftest(void) {
    // Canonical 20-byte header (IHL=5); its header checksum is 0xB861.
    static const uint8_t KA[20] = {
        0x45,0x00, 0x00,0x73, 0x00,0x00, 0x40,0x00,
        0x40,0x11, 0xB8,0x61, 0xC0,0xA8,0x00,0x01, 0xC0,0xA8,0x00,0xC7
    };
    // 1. verify property: the checksum over a valid header folds to 0
    if (ip_checksum(KA, 20) != 0) return 1;
    // 2. known answer: with the checksum field zeroed, the computed value is 0xB861
    { uint8_t h[20]; for (int i = 0; i < 20; i++) h[i] = KA[i]; h[10] = 0; h[11] = 0;
      if (ip_checksum(h, 20) != 0xB861) return 2; }

    // Self-consistent header-only packet (total_len = 20) for the gate tests.
    uint8_t p[20];
    for (int i = 0; i < 20; i++) p[i] = KA[i];
    p[2] = 0x00; p[3] = 0x14;                      // total_len = 20
    p[10] = 0; p[11] = 0;                          // zero, then fill the checksum
    uint16_t ck = ip_checksum(p, 20);
    p[10] = (uint8_t)(ck >> 8); p[11] = (uint8_t)(ck & 0xFF);

    uint32_t ihl = 0, iplen = 0;
    // 3. valid header, exact-length frame -> accept, ihl=20, ip_len=20
    if (!ipv4_hdr_check(p, 20, &ihl, &iplen) || ihl != 20 || iplen != 20) return 3;
    // 4. frame shorter than a header -> drop
    if (ipv4_hdr_check(p, 19, 0, 0)) return 4;
    // 5. IHL too small (4 words = 16 bytes) -> drop
    { uint8_t q[20]; for (int i = 0; i < 20; i++) q[i] = p[i]; q[0] = 0x44;
      if (ipv4_hdr_check(q, 20, 0, 0)) return 5; }
    // 6. IHL claims options (15 words = 60 bytes) that overrun a 20-byte frame -> drop
    { uint8_t q[20]; for (int i = 0; i < 20; i++) q[i] = p[i]; q[0] = 0x4F;
      if (ipv4_hdr_check(q, 20, 0, 0)) return 6; }
    // 7. corrupted header byte -> checksum fails -> drop
    { uint8_t q[20]; for (int i = 0; i < 20; i++) q[i] = p[i]; q[16] ^= 0x01;
      if (ipv4_hdr_check(q, 20, 0, 0)) return 7; }
    // 8. Ethernet padding: a 60-byte frame carrying a total_len=20 header -> accept but
    //    strip the 40 pad bytes (ip_len must be 20, not 60)
    { uint8_t q[60]; for (int i = 0; i < 20; i++) q[i] = p[i]; for (int i = 20; i < 60; i++) q[i] = 0xEE;
      ihl = iplen = 0;
      if (!ipv4_hdr_check(q, 60, &ihl, &iplen) || iplen != 20) return 8; }
    // 9. total_len overruns the frame -> clamp ip_len to the frame length (20)
    { uint8_t q[20]; for (int i = 0; i < 20; i++) q[i] = p[i];
      q[2] = 0x03; q[3] = 0xE8;                    // total_len = 1000
      q[10] = 0; q[11] = 0; uint16_t c = ip_checksum(q, 20);
      q[10] = (uint8_t)(c >> 8); q[11] = (uint8_t)(c & 0xFF);
      ihl = iplen = 0;
      if (!ipv4_hdr_check(q, 20, &ihl, &iplen) || iplen != 20) return 9; }
    return 0;
}
