#include "libc.h"

/* sockdemo — userspace TCP sockets (v5.8.52). Connects to the kernel's built-in
 * loopback echo service on 127.0.0.1:7, sends a line, reads the echo back, and
 * prints it. Exercises the real path: socket() -> connect() (drives the 3-way
 * handshake) -> write() (tcp_send) -> read() (blocks on tcp_recv) -> close().
 * Fully self-contained over loopback — no NIC or external host needed. */

static volatile int owner;

static int fd_lifetime_test(void) {
    int ok = 1;
    const unsigned int loopback = inet_ipv4(127, 0, 0, 1);
    for (int type = SOCK_STREAM; type <= SOCK_DGRAM; type++) {
        int source = socket(AF_INET, type, 0), alias = dup(source);
        if (source < 0 || alias < 0) return 1;
        close(source);
        ok = ok && bind(alias, INADDR_ANY, 19001) == 0;
        close(alias);
        source = socket(AF_INET, type, 0); alias = dup(source);
        if (source < 0 || alias < 0) return 1;
        close(source);
        int other = socket(AF_INET, type, 0);
        close(alias);
        ok = ok && other >= 0 && bind(other, INADDR_ANY, 19002) == 0;
        close(other);
        int completed = 0;
        for (int i = 0; i < 40; i++) {
            int a = socket(AF_INET, type, 0), b = socket(AF_INET, type, 0);
            if (a < 0 || b < 0) {
                if (a >= 0) close(a);
                if (b >= 0) close(b);
                break;
            }
            int replaced = dup2(a, b) == b;
            int source_alive = bind(a, INADDR_ANY, 19003) == 0;
            close(a);
            int destination_alive = bind(b, INADDR_ANY, 19003) == 0;
            close(b);
            if (!replaced || !source_alive || !destination_alive) break;
            completed++;
        }
        ok = ok && completed == 40;
        printf("sockdemo: fd-lifetime type=%d cycles=%d/40\n", type, completed);
    }
    if (!ok) { printf("sockdemo: fd-lifetime FAIL ownership\n"); return 1; }

    int rx = socket(AF_INET, SOCK_DGRAM, 0), tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0 || tx < 0 || bind(rx, INADDR_ANY, 19004)) return 1;
    int alias = dup(rx); close(rx);
    char data[16] = {0}; unsigned int ip = 0; int port = 0;
    ok = alias >= 0 && sendto(tx, "ALIAS", 5, 0, loopback, 19004) == 5;
    if (ok) ok = recvfrom(alias, data, sizeof(data), 0, &ip, &port) == 5 &&
                 !memcmp(data, "ALIAS", 5) && ip == loopback;
    close(alias); close(tx);
    printf("sockdemo: UDP alias %s\n", ok ? "PASS" : "FAIL");
    if (!ok) return 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, loopback, 7)) return 1;
    alias = dup(fd); close(fd);
    ok = alias >= 0 && write(alias, "TCP_ALIAS", 9) == 9;
    if (ok) ok = read(alias, data, sizeof(data)) == 9 && !memcmp(data, "TCP_ALIAS", 9);
    int destination = socket(AF_INET, SOCK_DGRAM, 0);
    ok = ok && destination >= 0 && dup2(alias, destination) == destination;
    close(alias);
    if (ok) ok = write(destination, "REPLACED", 8) == 8 &&
                 read(destination, data, sizeof(data)) == 8 && !memcmp(data, "REPLACED", 8);
    close(destination);
    printf("sockdemo: TCP alias/replacement %s\n", ok ? "PASS" : "FAIL");
    if (!ok) return 1;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, INADDR_ANY, 19005) || listen(listener, 1)) return 1;
    int child = (int)fork();
    if (child < 0) { close(listener); return 1; }
    if (!child) {
        int client = socket(AF_INET, SOCK_STREAM, 0);
        int sent = client >= 0 && connect(client, loopback, 19005) == 0;
        usleep(200000);
        if (sent) sent = write(client, "ACCEPTED", 8) == 8;
        usleep(200000); close(client); exit(sent ? 0 : 1);
    }
    fd = accept(listener); alias = dup(fd); close(fd);
    ok = alias >= 0 && read(alias, data, sizeof(data)) == 8 && !memcmp(data, "ACCEPTED", 8);
    close(alias); close(listener);
    int status = -1;
    ok = waitpid(child, &status) == child && status == 0 && ok;
    printf("sockdemo: accepted alias %s\n", ok ? "PASS" : "FAIL");
    printf("sockdemo: fd-lifetime %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int wait_return_test(void) {
    enum { TEST_PORT = 9246 };
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    if (bind(listener, INADDR_ANY, TEST_PORT) || listen(listener, 1)) {
        close(listener);
        return 1;
    }
    owner = 1;
    int child = (int)fork();
    if (child < 0) { close(listener); return 1; }
    if (!child) {
        /* Socket descriptors are not inherited by fork on NyxOS. */
        owner = 2;
        int tx = socket(AF_INET, SOCK_STREAM, 0);
        if (tx < 0) exit(1);
        usleep(400000);
        int ok = connect(tx, inet_ipv4(127, 0, 0, 1), TEST_PORT) == 0;
        usleep(400000);
        if (ok) ok = write(tx, "LATE", 4) == 4 && owner == 2;
        usleep(400000);
        close(tx);
        exit(ok ? 0 : 1);
    }
    printf("sockdemo: waiting for delayed accept and read\n");
    int fd = accept(listener), status = -1;
    char data[8] = {0};
    int n = fd < 0 ? -1 : read(fd, data, sizeof(data));
    int ok = n == 4 && !memcmp(data, "LATE", 4) && owner == 1;
    /* The current socket API returns zero for either closure or its timeout. */
    int end = fd < 0 ? -1 : read(fd, data, sizeof(data));
    if (fd >= 0) close(fd);
    close(listener);
    long reaped = waitpid(child, &status);
    ok = ok && end == 0 && owner == 1 && reaped == child && status == 0;
    printf("sockdemo: wait-return %s (bytes=%d end=%d child=%d)\n",
           ok ? "PASS" : "FAIL", n, end, status);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--fd-lifetime")) return fd_lifetime_test();
    if (argc == 2 && !strcmp(argv[1], "--wait-return")) return wait_return_test();
    if (argc != 1) { printf("Usage: sockdemo [--wait-return|--fd-lifetime]\n"); return 1; }
    printf("sockdemo: opening a TCP socket...\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("sockdemo: socket() failed\n"); return 1; }

    printf("sockdemo: connecting to 127.0.0.1:7 (echo)...\n");
    if (connect(fd, inet_ipv4(127, 0, 0, 1), 7) != 0) {
        printf("sockdemo: connect() failed\n");
        close(fd);
        return 1;
    }
    printf("sockdemo: connected (fd=%d)\n", fd);

    const char* msg = "hello from userspace sockets!";
    int mlen = strlen(msg);
    if (write(fd, msg, mlen) != mlen) {
        printf("sockdemo: write() failed\n");
        close(fd);
        return 1;
    }
    printf("sockdemo: sent %d bytes: \"%s\"\n", mlen, msg);

    char buf[128];
    int n = read(fd, buf, sizeof(buf) - 1);   /* blocks until the echo arrives */
    if (n <= 0) {
        printf("sockdemo: no echo (read returned %d)\n", n);
        close(fd);
        return 1;
    }
    buf[n] = '\0';
    printf("sockdemo: got echo (%d bytes): \"%s\"\n", n, buf);

    close(fd);
    int ok = (n == mlen && !memcmp(buf, msg, mlen));
    printf("sockdemo: %s\n", ok ? "OK -- echo matched, sockets work!" : "MISMATCH");
    return ok ? 0 : 1;
}
