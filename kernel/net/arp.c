#include "../core/kernel.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IP       0x0800
#define ARP_HLEN 6
#define ARP_PLEN 4
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2
#define ARP_CACHE_SIZE 16

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} arp_packet_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_cache_t;

static arp_cache_t arp_cache[ARP_CACHE_SIZE];

extern int eth_send(const uint8_t* dst_mac, uint16_t type, const uint8_t* data, uint32_t len, int iface_idx);
static uint8_t arp_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void arp_init(void) {
    // No static entries: the previous gateway entry used host byte order and our
    // own MAC (wrong). The gateway (10.0.2.2 under QEMU) answers ARP, so it is
    // resolved dynamically like any other host.
    memset_asm(arp_cache, 0, sizeof(arp_cache));
}

int arp_cache_lookup(uint32_t ip, uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_cache_add(uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip == ip) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = 1;
            return;
        }
    }
    // Replace oldest
    memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].ip = ip;
}

// Read-only accessors for the `arp` command (kernel.c). Iterate 0..arp_cache_size()-1;
// arp_cache_entry() fills *ip/mac for a VALID slot and returns 1, else returns 0.
int arp_cache_size(void) { return ARP_CACHE_SIZE; }
int arp_cache_entry(int idx, uint32_t* ip, uint8_t* mac) {
    if (idx < 0 || idx >= ARP_CACHE_SIZE || !arp_cache[idx].valid) return 0;
    if (ip)  *ip  = arp_cache[idx].ip;
    if (mac) memcpy(mac, arp_cache[idx].mac, 6);
    return 1;
}

void arp_send_request(uint32_t target_ip, int iface_idx) {
    arp_packet_t arp;
    arp.htype = htons(ARP_HTYPE_ETHERNET);
    arp.ptype = htons(ARP_PTYPE_IP);
    arp.hlen = ARP_HLEN;
    arp.plen = ARP_PLEN;
    arp.oper = htons(ARP_OP_REQUEST);
    memcpy(arp.sender_mac, net_interfaces[iface_idx].mac, 6);
    // IPs are stored network-order (low byte = first octet), so the wire bytes
    // are the uint32 bytes in ascending order.
    uint32_t sip = net_interfaces[iface_idx].ip;
    arp.sender_ip[0] = sip & 0xFF;
    arp.sender_ip[1] = (sip >> 8) & 0xFF;
    arp.sender_ip[2] = (sip >> 16) & 0xFF;
    arp.sender_ip[3] = (sip >> 24) & 0xFF;
    memset_asm(arp.target_mac, 0, 6);
    arp.target_ip[0] = target_ip & 0xFF;
    arp.target_ip[1] = (target_ip >> 8) & 0xFF;
    arp.target_ip[2] = (target_ip >> 16) & 0xFF;
    arp.target_ip[3] = (target_ip >> 24) & 0xFF;
    eth_send(arp_broadcast, 0x0806, (uint8_t*)&arp, sizeof(arp_packet_t), iface_idx);
}

void arp_handle_packet(uint8_t* packet, uint32_t len) {
    if (len < sizeof(arp_packet_t)) return;
    arp_packet_t* arp = (arp_packet_t*)packet;
    // Only process well-formed IPv4-over-Ethernet ARP (RFC 826 checks the hardware and
    // protocol types first). The sender_mac/sender_ip and target_* fields sit at fixed
    // offsets that assume hlen=6/plen=4; a packet with a different hardware/protocol type
    // or address lengths lays those fields out elsewhere, so caching the sender or
    // matching target_ip would act on misread bytes. Real ARP on our links is always
    // Ethernet/IPv4, so this rejects only malformed or non-IPv4 ARP.
    if (ntohs(arp->htype) != ARP_HTYPE_ETHERNET || ntohs(arp->ptype) != ARP_PTYPE_IP ||
        arp->hlen != ARP_HLEN || arp->plen != ARP_PLEN) return;
    uint16_t oper = ntohs(arp->oper);
    uint32_t sender_ip = (uint32_t)arp->sender_ip[0] |
                         ((uint32_t)arp->sender_ip[1] << 8) |
                         ((uint32_t)arp->sender_ip[2] << 16) |
                         ((uint32_t)arp->sender_ip[3] << 24);
    arp_cache_add(sender_ip, arp->sender_mac);
    if (oper == ARP_OP_REQUEST) {
        for (int i = 0; i < 8; i++) {
            if (!net_interfaces[i].name[0]) continue;
            uint32_t iface_ip = net_interfaces[i].ip;
            uint32_t target_ip = (uint32_t)arp->target_ip[0] |
                                 ((uint32_t)arp->target_ip[1] << 8) |
                                 ((uint32_t)arp->target_ip[2] << 16) |
                                 ((uint32_t)arp->target_ip[3] << 24);
            if (iface_ip == target_ip) {
                arp_packet_t reply;
                reply.htype = arp->htype;
                reply.ptype = arp->ptype;
                reply.hlen = ARP_HLEN;
                reply.plen = ARP_PLEN;
                reply.oper = htons(ARP_OP_REPLY);
                memcpy(reply.sender_mac, net_interfaces[i].mac, 6);
                memcpy(reply.sender_ip, arp->target_ip, 4);
                memcpy(reply.target_mac, arp->sender_mac, 6);
                memcpy(reply.target_ip, arp->sender_ip, 4);
                eth_send(arp->sender_mac, 0x0806, (uint8_t*)&reply, sizeof(arp_packet_t), i);
                break;
            }
        }
    }
}

// Nonblocking neighbour lookup: never enter the receive path from TX.
// ip_send retains a bounded packet copy; the normal poll loop learns the reply.
int arp_resolve(uint32_t ip, uint8_t* mac, int iface_idx) {
    if (arp_cache_lookup(ip, mac)) return 1;
    arp_send_request(ip, iface_idx);
    return 0;
}

// KAT for the ARP input parser (arp_handle_packet), which consumes UNTRUSTED wire data.
// Pins the anti-poisoning property: ONLY a well-formed IPv4-over-Ethernet ARP updates the
// cache — a packet with the wrong hardware/protocol type or address lengths, or a runt, is
// rejected BEFORE arp_cache_add, so it can never poison the cache with misread bytes. Uses
// REPLY packets (a reply is generated only for a REQUEST, so nothing hits the wire) and
// saves/restores the cache it touches. Convention: 0 = PASS, else the failing case number.
int arp_parse_selftest(void) {
    arp_cache_t saved[ARP_CACHE_SIZE];
    memcpy(saved, arp_cache, sizeof(arp_cache));
    int rc = 0;
    uint8_t mac[6];
    static const uint8_t M[6] = { 0xAA,0xBB,0xCC,0xDD,0xEE,0xFF };
    uint32_t ip1234 = 1u | (2u << 8) | (3u << 16) | (4u << 24);   // 1.2.3.4, net order

    arp_packet_t p;
    memset_asm(&p, 0, sizeof(p));
    p.htype = htons(ARP_HTYPE_ETHERNET);
    p.ptype = htons(ARP_PTYPE_IP);
    p.hlen = ARP_HLEN; p.plen = ARP_PLEN;
    p.oper = htons(ARP_OP_REPLY);
    memcpy(p.sender_mac, M, 6);
    p.sender_ip[0] = 1; p.sender_ip[1] = 2; p.sender_ip[2] = 3; p.sender_ip[3] = 4;

    // 1. a valid reply caches sender_ip -> sender_mac
    memset_asm(arp_cache, 0, sizeof(arp_cache));
    arp_handle_packet((uint8_t*)&p, sizeof(p));
    if (!arp_cache_lookup(ip1234, mac)) { rc = 1; goto done; }
    for (int b = 0; b < 6; b++) if (mac[b] != M[b]) { rc = 1; goto done; }

    // 2. wrong hardware type -> rejected, cache NOT poisoned
    { arp_packet_t q = p; q.htype = htons(99);
      memset_asm(arp_cache, 0, sizeof(arp_cache));
      arp_handle_packet((uint8_t*)&q, sizeof(q));
      if (arp_cache_lookup(ip1234, mac)) { rc = 2; goto done; } }
    // 3. wrong protocol type (IPv6) -> rejected
    { arp_packet_t q = p; q.ptype = htons(0x86DD);
      memset_asm(arp_cache, 0, sizeof(arp_cache));
      arp_handle_packet((uint8_t*)&q, sizeof(q));
      if (arp_cache_lookup(ip1234, mac)) { rc = 3; goto done; } }
    // 4. bad hardware address length -> rejected
    { arp_packet_t q = p; q.hlen = 8;
      memset_asm(arp_cache, 0, sizeof(arp_cache));
      arp_handle_packet((uint8_t*)&q, sizeof(q));
      if (arp_cache_lookup(ip1234, mac)) { rc = 4; goto done; } }
    // 5. runt (one byte short of a full ARP packet) -> rejected, no OOB read
    { memset_asm(arp_cache, 0, sizeof(arp_cache));
      arp_handle_packet((uint8_t*)&p, sizeof(p) - 1);
      if (arp_cache_lookup(ip1234, mac)) { rc = 5; goto done; } }

done:
    memcpy(arp_cache, saved, sizeof(arp_cache));
    return rc;
}
