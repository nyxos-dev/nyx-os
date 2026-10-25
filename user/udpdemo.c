#include "libc.h"

/* udpdemo — userspace UDP sockets (v5.8.55). Single process over loopback:
 *   rx = socket(SOCK_DGRAM); bind(rx, 9001)     -- a receiver on UDP port 9001
 *   tx = socket(SOCK_DGRAM); sendto(tx, ..., 9001) -- send it a datagram
 *   recvfrom(rx, ..., &ip, &port)               -- receive it, with the sender's addr
 * Exercises SOCK_DGRAM + sendto()/recvfrom() end to end, fully self-contained
 * (no NIC/host). sendto queues the datagram into the loopback ring; recvfrom
 * busy-polls the net, which delivers it into rx's queue. */

static volatile int owner;

static int wait_return_test(void) {
    enum { RECEIVE_PORT = 9247, SEND_PORT = 9248 };
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0) return 1;
    if (bind(rx, INADDR_ANY, RECEIVE_PORT)) { close(rx); return 1; }
    owner = 1;
    int child = (int)fork();
    if (child < 0) { close(rx); return 1; }
    if (!child) {
        /* Create a separate sender: fork does not inherit socket descriptors. */
        owner = 2;
        int tx = socket(AF_INET, SOCK_DGRAM, 0);
        if (tx < 0) exit(1);
        int ok = bind(tx, INADDR_ANY, SEND_PORT) == 0;
        usleep(400000);
        if (ok) ok = sendto(tx, "LATE", 4, 0, inet_ipv4(127, 0, 0, 1), RECEIVE_PORT) == 4;
        usleep(400000);
        close(tx);
        exit(ok && owner == 2 ? 0 : 1);
    }
    printf("udpdemo: waiting for delayed data and empty-queue timeout\n");
    char data[8] = {0};
    unsigned int ip = 0;
    int port = 0, status = -1;
    int n = recvfrom(rx, data, sizeof(data), 0, &ip, &port);
    int ok = n == 4 && !memcmp(data, "LATE", 4) && owner == 1 &&
             ip == inet_ipv4(127, 0, 0, 1) && port == SEND_PORT;
    memset(data, '!', sizeof(data));
    ip = 123; port = 456;
    int end = recvfrom(rx, data, sizeof(data), 0, &ip, &port);
    close(rx);
    long reaped = waitpid(child, &status);
    ok = ok && end == -1 && !memcmp(data, "!!!!!!!!", 8) && ip == 123 && port == 456 &&
         owner == 1 && reaped == child && status == 0;
    printf("udpdemo: wait-return %s (bytes=%d timeout=%d child=%d)\n",
           ok ? "PASS" : "FAIL", n, end, status);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--wait-return")) return wait_return_test();
    if (argc != 1) { printf("Usage: udpdemo [--wait-return]\n"); return 1; }
    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0)                          { printf("udpdemo: socket() failed\n"); return 1; }
    if (bind(rx, INADDR_ANY, 9001) != 0) { printf("udpdemo: bind() failed\n");   return 1; }
    printf("udpdemo: receiver bound to UDP port 9001 (fd=%d)\n", rx);

    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0)                          { printf("udpdemo: socket() failed\n"); return 1; }

    const char* m = "hello over UDP";
    int slen = strlen(m);
    int r = sendto(tx, m, slen, 0, inet_ipv4(127, 0, 0, 1), 9001);
    printf("udpdemo: sent %d bytes to 127.0.0.1:9001\n", r);

    char buf[64];
    unsigned int sip = 0;
    int sport = 0;
    int n = recvfrom(rx, buf, sizeof(buf) - 1, 0, &sip, &sport);   /* blocks for the datagram */
    if (n > 0) {
        buf[n] = '\0';
        printf("udpdemo: received \"%s\" (%d bytes) from %d.%d.%d.%d:%d\n",
               buf, n, sip & 0xFF, (sip >> 8) & 0xFF, (sip >> 16) & 0xFF, (sip >> 24) & 0xFF, sport);
    } else {
        printf("udpdemo: recvfrom() returned %d\n", n);
    }

    close(tx);
    close(rx);
    int ok = (n == slen && !memcmp(buf, m, slen));
    printf("udpdemo: %s\n", ok ? "OK -- UDP sockets work!" : "MISMATCH");
    return ok ? 0 : 1;
}
