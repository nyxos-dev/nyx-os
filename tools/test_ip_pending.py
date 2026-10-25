#!/usr/bin/env python3
"""Actual IPv4 TX/ARP lookup with mocked Ethernet, time and locks; no live NIC."""
from pathlib import Path
import argparse,re,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-dir',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];folder=a.source_dir or root/'kernel/net'
ip=(folder/'ip.c').read_text(encoding='utf-8');arp=(folder/'arp.c').read_text(encoding='utf-8')
arp=arp[arp.index('int arp_resolve('):arp.index('// KAT for the ARP input parser')]
ip=re.sub(r'^#include[^\n]*\n','',ip,flags=re.M)
prefix=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { char name[16]; uint32_t ip,netmask,gateway,tx_packets,rx_packets; } net_iface_t;
static net_iface_t net_interfaces[8];
typedef int spinlock_t;
#define SPINLOCK_INIT 0
static uint64_t spin_lock_irqsave(spinlock_t* l){if(*l)abort();*l=1;return 42;}
static void spin_unlock_irqrestore(spinlock_t* l,uint64_t f){if(!*l||f!=42)abort();*l=0;}
static uint16_t htons(uint16_t x){return (x<<8)|(x>>8);}
#define ntohs htons
static void* kmalloc(uint32_t n){return malloc(n);}
static void kfree(void* p){free(p);}
static uint32_t ticks,known,last_lookup,last_request;static int tx,requests,polls,fail_tx;
static uint8_t sent[65536];static uint32_t sent_len;static int sent_iface;
static unsigned checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %d\n",__LINE__);}}while(0)
static uint32_t get_ticks(void){return ticks;}
int arp_cache_lookup(uint32_t ip,uint8_t* mac){last_lookup=ip;if(ip!=known)return 0;memset(mac,0xAA,6);return 1;}
void arp_send_request(uint32_t ip,int iface){CHECK(iface==1);requests++;last_request=ip;}
void kernel_poll_net(void){polls++;}
static unsigned char inb(unsigned short port){(void)port;return 0;}
#define IP4_OCTETS(ip) ((ip)&255),(((ip)>>8)&255),(((ip)>>16)&255),(((ip)>>24)&255)
int eth_send(const uint8_t* mac,uint16_t type,const uint8_t* data,uint32_t len,int iface){
    CHECK(type==0x800 && (mac[0]==0xAA || mac[0]==0xFF));CHECK(len<=sizeof(sent));
    if(fail_tx)return -1;
    memcpy(sent,data,len);sent_len=len;sent_iface=iface;tx++;return len;
}
void udp_handle_packet(uint8_t* p,uint32_t n,uint32_t s){(void)p;(void)n;(void)s;}
void icmp_handle_packet(uint8_t* p,uint32_t n,uint32_t s){(void)p;(void)n;(void)s;}
void tcp_handle_packet(uint8_t* p,uint32_t n,uint32_t s,uint32_t d){(void)p;(void)n;(void)s;(void)d;}
'''
tests=r'''
static uint32_t addr(int last){return 10u|(2u<<16)|((uint32_t)last<<24);}
static void reset(void){
    ticks+=4000;known=0;ip_pending_poll();tx=requests=polls=fail_tx=0;sent_len=0;
    memset(net_interfaces,0,sizeof(net_interfaces));
    strcpy(net_interfaces[0].name,"lo");net_interfaces[0].ip=0x0100007F;
    strcpy(net_interfaces[1].name,"eth0");net_interfaces[1].ip=addr(15);
    net_interfaces[1].netmask=0x00FFFFFF;net_interfaces[1].gateway=addr(2);
}
int main(void){
    uint8_t payload[]={1,2,3,4};reset();
    CHECK(ip_send(addr(100),6,payload,4,-1)==24);CHECK(!tx&&!polls&&requests==1);
    CHECK(last_request==addr(100));payload[0]=99;known=addr(100);ip_pending_poll();
    CHECK(tx==1&&sent_len==24&&sent[20]==1&&sent_iface==1);CHECK(ip_checksum(sent,20)==0);
    ip_pending_poll();CHECK(tx==1);CHECK(ip_send(addr(100),17,payload,4,-1)==24&&tx==2);
    CHECK(sent[20]==99&&requests==1);
    reset();uint32_t remote=0x01010101;CHECK(ip_send(remote,6,payload,4,-1)==24);
    CHECK(last_request==addr(2));known=addr(2);ip_pending_poll();CHECK(tx==1);
    CHECK(((ip_header_t*)sent)->dst_ip==remote&&((ip_header_t*)sent)->src_ip==addr(15));
    reset();uint32_t start=ticks;CHECK(ip_send(addr(100),17,payload,4,1)==24);
    ticks=start+999;ip_pending_poll();CHECK(requests==1&&!tx);
    ticks=start+1000;ip_pending_poll();CHECK(requests==2&&!tx);
    ticks=start+2999;ip_pending_poll();CHECK(requests==3&&!tx);
    ticks=start+3000;known=addr(100);ip_pending_poll();CHECK(requests==3&&!tx);
    reset();ticks=UINT32_MAX-100;start=ticks;CHECK(ip_send(addr(100),17,payload,4,1)==24);
    ticks=start+1000;ip_pending_poll();CHECK(requests==2);
    ticks=start+3000;known=addr(100);ip_pending_poll();CHECK(!tx);
    reset();for(int i=0;i<16;i++)CHECK(ip_send(addr(100),17,payload,4,1)==24);
    CHECK(ip_send(addr(100),17,payload,4,1)==-1);known=addr(100);ip_pending_poll();CHECK(tx==16);
    ip_pending_poll();CHECK(tx==16);CHECK(ip_send(addr(100),17,payload,4,1)==24&&tx==17);
    for(int kind=0;kind<4;kind++){
        reset();CHECK(ip_send(addr(100),17,payload,4,1)==24);known=addr(100);
        if(kind==0)net_interfaces[1].ip++;else if(kind==1)net_interfaces[1].netmask++;
        else if(kind==2)net_interfaces[1].gateway++;else net_interfaces[1].name[0]=0;
        ip_pending_poll();CHECK(!tx);
    }
    reset();CHECK(ip_send(0xFFFFFFFF,17,payload,4,1)==24&&tx==1&&requests==0);
    CHECK(ip_send(addr(255),17,payload,4,1)==24&&tx==2&&requests==0);
    CHECK(ip_send(0x0100007F,17,payload,4,-1)==24&&tx==2);ip_loopback_poll();
    CHECK(ip_send(addr(100),17,payload,4,8)==-1);
    CHECK(ip_send(addr(100),17,0,1,1)==-1);
    CHECK(ip_send(addr(100),17,payload,65535,1)==-1);
    uint8_t big[2029]={0};CHECK(ip_send(addr(100),17,big,sizeof(big),1)==-1);
    CHECK(ip_send(addr(100),17,big,2028,1)==2048);known=addr(100);ip_pending_poll();CHECK(sent_len==2048);
    reset();CHECK(ip_send(addr(100),17,payload,4,1)==24);known=addr(100);fail_tx=1;
    ip_pending_poll();CHECK(!tx);fail_tx=0;ip_pending_poll();CHECK(tx==1&&sent[20]==99);
    reset();CHECK(ipv4_rx_selftest()==0);CHECK(polls==0);
    printf("IPv4 pending TX: %u checks, %u failures (mocked Ethernet, time and locks)\n",checks,failures);
    return failures?1:0;
}
'''
if 'void ip_pending_poll(void)' not in ip:
    tests=r'''
int main(void){strcpy(net_interfaces[1].name,"eth0");net_interfaces[1].ip=0x0f02000a;
net_interfaces[1].netmask=0x00ffffff;uint8_t p[4]={1,2,3,4};
CHECK(ip_send(0x6402000a,6,p,4,-1)==24);CHECK(polls==0);CHECK(tx==0);
printf("Baseline cold TX: %u checks, %u failures; recursive polls=%d\n",checks,failures,polls);return failures?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='nyx-ip-pending-') as tmp:
    f=Path(tmp);(f/'test.c').write_text(prefix+arp+ip+tests,encoding='utf-8')
    subprocess.run(['gcc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-function','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(f/'test.c'),'-o',str(f/'test')],check=True)
    subprocess.run([str(f/'test')],check=True)
