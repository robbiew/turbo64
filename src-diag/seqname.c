/* SEQNAME - does a host file need a .SEQ extension to open as SEQ on SoftIEC?
 *
 * SoftIEC derives the CBM file type from the HOST filename extension, while
 * files written from the C64 get ".seq" appended automatically. So a content
 * file copied from a PC as "g.term" may not be openable as ",S,R" at all.
 * That decides whether the migration tool must rename every content file.
 *
 * Reports the raw DOS code: 62 = absent, 64 = present but wrong type, 0 = OK. */
#include <stdio.h>
#include <conio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/hal/disk.h"

static u8 dev = 11;

static void chk(const char *name)
{
    u8 code;
    if (disk_open(dev, 0, name, DISK_READ) == BBS_OK) disk_close();
    code = disk_status(dev);
    printf("  %-14s %02u %s\n", name, (unsigned)code,
           code == 62 ? "ABSENT" :
           (code == 64 ? "WRONG TYPE" : (code < 20 ? "OK" : "?")));
}

int main(void)
{
    printf("\x93\x8e");
    printf("SEQ NAME TEST\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    disk_cmd(dev, "CD:/USB1/TURBO64/SYSTEM");
    printf("IN SYSTEM/\n");
    chk("G.TERM");
    chk("G.TERM.SEQ");
    chk("G.LOGIN");
    chk("CONFIG");
    chk("T64.SIEC");

    /* SoftIEC's current directory is drive state that survives a C64 reset
     * and a power cycle — leaving the cursor in SYSTEM/ breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("\nDONE.\n");
    getch();
    return 0;
}
