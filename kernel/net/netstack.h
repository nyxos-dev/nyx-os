#ifndef NYX_NET_NETSTACK_H
#define NYX_NET_NETSTACK_H
// Network stack public API — the entry points exported by the driver + protocol layers
// (drivers/net/rtl8139.c, net/ethernet.c/arp.c/ip.c/udp.c/icmp.c/dhcp.c/dns.c, and net.c's
// TCP + userspace-socket layer). Consumers: the syscall layer (SYS_SOCKET/CONNECT/…), the
// shell's net commands (ping/dhcp/ifconfig), Selene, and the compositor's poll loop.
//
// This was split out of the core/kernel.h god-header as one cohesive module boundary — the
// same incremental per-subsystem header split the launchers rung began (see the
// architecture-modularity thread). kernel.h re-includes this header, so every existing
// includer keeps seeing these prototypes unchanged, while a net-only consumer can include
// just this focused header. Depends only on the fixed-width integer types.
#include "../core/types.h"

int rtl8139_init(void);
void eth_poll(int iface_idx);
void arp_init(void);
int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, const uint8_t* data, uint32_t len, int iface_idx);

// Userspace TCP socket layer (net.c) — backs SYS_SOCKET/SYS_CONNECT and the
// socket branches of SYS_READ/WRITE/CLOSE. Returns a small net-socket id.
int nsock_create(int domain, int type, int protocol);
int nsock_connect(int s, uint32_t ip, uint16_t port);   // blocks until ESTABLISHED
int nsock_bind(int s, uint32_t ip, uint16_t port);      // record the local port
int nsock_listen(int s, int backlog);                   // passive open on the bound port
int nsock_accept(int s);                                // blocks; new socket id for the peer
int nsock_send(int s, const void* buf, int len);
int nsock_recv(int s, void* buf, int len);              // blocks until data or EOF
int nsock_sendto(int s, const void* buf, int len, uint32_t ip, uint16_t port);     // UDP
int nsock_recvfrom(int s, void* buf, int len, uint32_t* src_ip, uint16_t* src_port); // UDP, blocks
int nsock_udp_deliver(uint16_t dst_port, uint8_t* data, uint32_t len, uint32_t src_ip, uint16_t src_port);
int nsock_retain(int s);    // add a descriptor owner, or -1 if invalid/overflow
int nsock_close(int s);     // drop an owner; only the final close tears down
int nsock_readable(int s);   // poll(): 1 if the socket has data/EOF ready to read
void tcp_echo_init(void);   // start the loopback TCP echo service (port 7)
void tcp_echo_poll(void);   // drive it from the net poll loop
void udp_register_listener(uint16_t port, void (*handler)(uint8_t*, uint32_t, uint32_t, uint16_t));
int dhcp_request(int iface_idx);
void cmd_dhcp(int argc, char** argv);
uint32_t dns_resolve(const char* hostname, int iface_idx);
void dns_set_server(uint32_t ip);
uint32_t dns_get_server(void);
int icmp_ping(uint32_t dst_ip, int count, int iface_idx);
int tcp_init(void);
int tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port);
int tcp_send(int conn_id, const uint8_t* data, uint32_t len);
int tcp_recv(int conn_id, uint8_t* buf, uint32_t max_len);
int tcp_close(int conn_id);

#endif // NYX_NET_NETSTACK_H
