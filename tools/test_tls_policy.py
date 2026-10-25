#!/usr/bin/env python3
"""Exercise actual TLS refusal blocks; X.509 verdicts/transport are mocked."""
from pathlib import Path
import argparse, re, subprocess, tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = (args.source or root / 'kernel/crypto/tls/tls.c').read_text(encoding='utf-8')
settings = source[source.index('static int tls_strict ='):source.index('static void put8')]
start = source.find('    if (!got_cert || chain_n == 0)')
if start < 0:
    start = source.index('    if (got_cert && chain_n > 0)')
gate = source[start:source.index('    // ECDHE key agreement', start)]
ske = re.search(r'        if \(tls_strict && got_ske && !ske_ok\) \{.*?\n        \}', source, re.S).group()
program = r'''
#include <stdint.h>
#include <stdio.h>
#define X509_OK 0
#define X509_INCOMPLETE 1
#define X509_FORGED (-1)
static int trust_result,host_result,date_result,closes,verifies;
static void tcp_close(int c) { if(c != 7) __builtin_trap(); closes++; }
static int x509_verify_chain(const uint8_t** c,uint32_t* l,int n,char* m,uint32_t cap) {
    (void)c;(void)l;(void)n; if(cap) m[0]=0;verifies++;return trust_result;
}
static int x509_check_host(const uint8_t* c,uint32_t n,const char* h) {(void)c;(void)n;(void)h;return host_result;}
static int x509_check_validity(const uint8_t* c,uint32_t n) {(void)c;(void)n;return date_result;}
#define TLSP(...) do { if(verbose) printf(__VA_ARGS__); } while(0)
''' + settings + r'''
static int certificate_gate(int got_cert,int chain_n) {
    static const uint8_t bytes[]={1};const uint8_t* chain_ptr[8]={bytes};uint32_t chain_len[8]={1};
    int conn=7,verbose=0;const char* host="policy.test";
''' + gate + r'''
    return 0;
}
static int key_gate(int got_ske,int ske_ok) {
    int conn=7,verbose=0;const char* host="policy.test";
''' + ske + r'''
    return 0;
}
static unsigned checks,failures;
#define CHECK(x) do { checks++;if(!(x)){failures++;printf("FAIL line %d\n",__LINE__);} } while(0)
int main(void) {
    CHECK(tls_get_strict()==1);
    for(int mode=0;mode<2;mode++) {
        tls_set_strict(mode);CHECK(tls_get_strict()==mode);
        for(int trust=-1;trust<=1;trust++) for(int host=-1;host<=1;host++) for(int date=-1;date<=2;date++) {
            trust_result=trust;host_result=host;date_result=date;closes=verifies=0;
            int denied=trust==X509_FORGED || (mode && (trust!=X509_OK || host || date));
            CHECK(certificate_gate(1,1)==(denied ? -1 : 0));CHECK(closes==denied);CHECK(verifies==1);
        }
        for(int cert=0;cert<2;cert++) for(int count=0;count<2;count++) {
            if(cert && count) continue;
            trust_result=host_result=date_result=0;closes=verifies=0;
            CHECK(certificate_gate(cert,count)==-1);CHECK(closes==1);CHECK(verifies==0);
        }
        for(int good=0;good<2;good++) {
            closes=0;CHECK(key_gate(1,good)==(mode && !good ? -1 : 0));CHECK(closes==(mode && !good));
        }
        trust_result=host_result=date_result=0;closes=0;
        CHECK(certificate_gate(1,1)==0 && closes==0);
    }
    tls_set_strict(-5);CHECK(tls_get_strict()==1);tls_set_strict(0);CHECK(tls_get_strict()==0);
    printf("TLS policy: %u checks, %u failures (mocked verdicts and transport)\n",checks,failures);
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix='nyx-tls-policy-') as temp:
    folder=Path(temp);c=folder/'policy.c';exe=folder/'policy'
    c.write_text(program,encoding='utf-8')
    subprocess.run(['gcc','-std=gnu99','-Wall','-Wextra','-Werror','-O1',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
