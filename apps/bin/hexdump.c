/* hexdump: show the bytes of a file. Usage: hexdump <file> */
#include <vortex.h>

int main(int argc, char **argv)
{
    unsigned char row[16];
    struct vx_stat st;

    if (argc < 2) {
        printf("usage: hexdump <file>\n");
        return 1;
    }
    if (vx_stat(argv[1], &st) < 0 || st.is_dir) {
        printf("hexdump: %s: no such file\n", argv[1]);
        return 1;
    }
    for (unsigned off = 0; off < st.size && off < 256; off += 16) {
        int n = vx_read_file(argv[1], row, sizeof(row), off);
        if (n <= 0) break;
        printf("%08x ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf(" %02x", row[i]);
            else printf("   ");
        }
        printf("  ");
        for (int i = 0; i < n; i++) putchar(row[i] >= 32 && row[i] < 127 ? row[i] : '.');
        putchar('\n');
    }
    if (st.size > 256) printf("... (%u bytes total)\n", st.size);
    return 0;
}
