#include "libc.h"

/* sockdemo — userspace TCP sockets (v5.8.52). Connects to the kernel's built-in
 * loopback echo service on 127.0.0.1:7, sends a line, reads the echo back, and
 * prints it. Exercises the real path: socket() -> connect() (drives the 3-way
 * handshake) -> write() (tcp_send) -> read() (blocks on tcp_recv) -> close().
 * Fully self-contained over loopback — no NIC or external host needed. */

static volatile int owner;

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
    if (argc == 2 && !strcmp(argv[1], "--wait-return")) return wait_return_test();
    if (argc != 1) { printf("Usage: sockdemo [--wait-return]\n"); return 1; }
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
