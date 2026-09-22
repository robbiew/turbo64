/* USRREAD - why does BOOT-SIEC report "USR LOG: EMPTY" on a migrated tree?
 *
 * Splits the boot check into its parts: does the SEQ open see the file at all,
 * how many bytes does a raw read return, and what does the rel_seq backend
 * make of record 1. Reports the raw DOS code at each step because disk_open()
 * returning BBS_OK is never evidence a file exists. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/records.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/reu.h"
#include "bbs/rel.h"

static u8 dev = 11;
static u8 buf[64];

static void raw_open(const char *name)
{
    u8 st;
    i16 n = -1;
    if (disk_open(dev, 0, name, DISK_READ) == BBS_OK) {
        n = disk_read(buf, 32);
        disk_close();
    }
    st = disk_status(dev);
    printf("  %-10s ST%02u N%d B0=%u\n", name, (unsigned)st,
           (int)n, (unsigned)buf[0]);
}

int main(void)
{
    rel_handle_t h;
    bbs_err_t eo, ep, er;
    u8 got = 0;

    printf("\x93\x8e");
    printf("USR LOG READ TEST\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    printf("REU %u KB\n", (unsigned)reu_detect());
    disk_cmd(dev, "CD:/USB1/TURBO64/SYSTEM");

    printf("RAW SEQ OPENS\n");
    raw_open("USR LOG");
    raw_open("USR PROF");
    raw_open("T64.SIEC");

    printf("REL_SEQ PATH\n");
    disk_set_section_path(0, "/USB1/TURBO64/SYSTEM");
    rel_reset();
    eo = rel_open(dev, 0, "USR LOG", RECORD_SIZE_USER, &h);
    ep = er = BBS_EFATAL;
    memset(buf, 0, sizeof(buf));
    if (eo == BBS_OK) {
        ep = rel_position(h, 1);
        er = rel_read(h, buf, RECORD_SIZE_USER, &got);
        rel_close(h);
    }
    printf("  OPEN E%u POS E%u READ E%u\n",
           (unsigned)eo, (unsigned)ep, (unsigned)er);
    printf("  GOT %u ID %u NAME %c%c%c%c%c\n", (unsigned)got,
           (unsigned)buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

    /* SoftIEC's current directory is drive state that survives a C64 reset
     * and a power cycle — leaving the cursor in SYSTEM/ breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("\nDONE.\n");
    getch();
    return 0;
}
