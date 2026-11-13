/* systest: exercises process isolation and memory management.
 * Usage: systest null|kernel|priv|badptr|malloc|spin */
#include <vortex.h>

static int test_malloc(void)
{
    enum { N = 64, SIZE = 64 * 1024 };
    static unsigned char *blocks[N];
    unsigned sum = 0;

    for (int i = 0; i < N; i++) {                   /* 4 MiB in total */
        blocks[i] = malloc(SIZE);
        if (!blocks[i]) { printf("malloc failed at block %d\n", i); return 1; }
        memset(blocks[i], i, SIZE);
    }
    for (int i = 0; i < N; i++) sum += blocks[i][SIZE - 1];
    for (int i = 0; i < N; i += 2) free(blocks[i]);
    char *again = malloc(SIZE / 2);                 /* must reuse a freed block */
    int reused = (unsigned char *)again >= blocks[0] && (unsigned char *)again < blocks[N - 1];
    printf("malloc: %d blocks, checksum %u, reuse %s\n", N, sum, reused ? "ok" : "FAILED");
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    if (strcmp(mode, "null") == 0) {
        printf("writing to address 0...\n");
        *(volatile int *)0 = 1;
    } else if (strcmp(mode, "kernel") == 0) {
        printf("reading kernel memory at 0x100000...\n");
        printf("got %d (isolation is broken!)\n", *(volatile int *)0x100000);
    } else if (strcmp(mode, "priv") == 0) {
        printf("executing a privileged instruction (cli)...\n");
        __asm__ volatile("cli");
    } else if (strcmp(mode, "badptr") == 0) {
        int r1 = vx_write((const char *)0x100000, 16);          /* kernel memory */
        int r2 = vx_read_file("/etc/motd", (void *)0x100000, 16, 0);
        int r3 = vx_stat((const char *)8, NULL);
        printf("system calls with bad pointers returned %d %d %d\n", r1, r2, r3);
        return 0;
    } else if (strcmp(mode, "malloc") == 0) {
        return test_malloc();
    } else if (strcmp(mode, "spin") == 0) {
        printf("spinning forever without system calls (kill me)...\n");
        for (;;) { }
    } else {
        printf("usage: systest null|kernel|priv|badptr|malloc|spin\n");
        return 1;
    }
    printf("still alive (protection is broken!)\n");
    return 1;
}
