/* DIR - non-destructive directory listing of a device/partition. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <c64/kernalio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/hal/disk.h"

int main(void)
{
    u8 dev = 10, part = 1;
    u16 n = 0;
    int c;
    char line[42];
    bool_t stop = FALSE;

    printf("\x93\x8e");
    printf("DIR OF DEV/PART\n");
    printf("DEVICE (8-11)? ");
    for (;;) { c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\nPARTITION (0-4)? ", (unsigned)dev);
    for (;;) { c = getch(); if (c >= '0' && c <= '4') { part = (u8)(c - '0'); break; } }
    printf("%u\n\n", (unsigned)part);

    if (disk_select_partition(dev, part) != BBS_OK) {
        printf("CP%u FAILED: %02u\n", (unsigned)part, disk_status(dev));
        printf("\nDONE.\n"); getch(); return 0;
    }

    krnio_setnam("$");
    if (!krnio_open(CFG_FNUM_DATA, dev, 0)) {
        printf("DIR OPEN FAILED\n"); printf("\nDONE.\n"); getch(); return 0;
    }
    line[0] = 0;

    /* Parse the directory as the BASIC-program image it actually is, rather
       than filtering for printable bytes: each entry is link(2) + a 16-bit
       BASIC line number that IS the block count + text + NUL. The old
       printable-only filter dropped that word by construction, so every
       count — including the trailing "BLOCKS FREE" total — was invisible.
       That total is usually the number you came here for. */
    (void)krnio_getch(CFG_FNUM_DATA);   /* load address lo */
    (void)krnio_getch(CFG_FNUM_DATA);   /* load address hi */
    n = 2;

    while (!stop && n < 8000) {
        int l0 = krnio_getch(CFG_FNUM_DATA);
        int l1 = krnio_getch(CFG_FNUM_DATA);
        int b0, b1;
        u16 blocks;
        u8 col;
        if (l0 < 0 || l1 < 0) break;
        n += 2;
        if ((l0 | l1) == 0) break;      /* null link = end of directory */

        b0 = krnio_getch(CFG_FNUM_DATA);
        b1 = krnio_getch(CFG_FNUM_DATA);
        if (b0 < 0 || b1 < 0) break;
        n += 2;
        blocks = (u16)((b0 & 0xFF) | ((b1 & 0xFF) << 8));

        col = 0;
        for (;;) {
            int v = krnio_getch(CFG_FNUM_DATA);
            char ch;
            if (v < 0) { stop = TRUE; break; }
            n++;
            ch = (char)(v & 0xFF);
            if (ch == 0) break;
            if (ch >= 0x20 && ch < 0x7F && col < 39) line[col++] = ch;
        }
        line[col] = 0;
        printf("%-5u %s\n", (unsigned)blocks, line);
    }
    krnio_clrchn();
    krnio_close(CFG_FNUM_DATA);
    printf("\n%u BYTES. DONE.\n", n);
    getch();
    return 0;
}
