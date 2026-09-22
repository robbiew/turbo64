/* SEQTEST - does the SEQ+REU backend behave like REL, and does it persist?
 *
 * WHY two phases: writing then reading in one run only proves the REU cache
 * works. The value must survive rel_close's flush and a fresh rel_open. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/reu.h"
#include "bbs/rel.h"

int main(void)
{
    u8 dev = 11;
    rel_handle_t h;
    bbs_err_t eo, ew, ec, er;
    u8 buf[30], got = 0, first = 0, last = 0;

    printf("\x93\x8e");
    printf("SEQ BACKEND TEST\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    printf("REU %u KB\n", (unsigned)reu_detect());
    if (!reu_data_available()) { printf("NO REU - STOP\n"); getch(); return 0; }

    disk_set_section_path(0, "/USB1/TURBO64/SYSTEM");
    disk_scratch(dev, 0, "USR LOG");
    disk_scratch(dev, 0, "USR LOG.NEW");

    rel_reset();
    eo = rel_open(dev, 0, "USR LOG", 30, &h);
    ew = BBS_EFATAL; ec = BBS_EFATAL;
    if (eo == BBS_OK) {
        memset(buf, ' ', sizeof(buf));
        buf[0] = 1;
        rel_position(h, 1);
        ew = rel_write(h, buf, 30);
        buf[0] = 5;
        rel_position(h, 5);
        rel_write(h, buf, 30);
        ec = rel_close(h);
    }
    printf("OPEN  E%u\nWRITE E%u\nFLUSH E%u\n",
           (unsigned)eo, (unsigned)ew, (unsigned)ec);

    /* Reopen: reads must now come off disk through a fresh load. */
    rel_reset();
    er = BBS_EFATAL;
    if (rel_open(dev, 0, "USR LOG", 30, &h) == BBS_OK) {
        memset(buf, 0, sizeof(buf));
        rel_position(h, 1);
        er = rel_read(h, buf, 30, &got);
        first = buf[0];
        memset(buf, 0, sizeof(buf));
        rel_position(h, 5);
        rel_read(h, buf, 30, &got);
        last = buf[0];
        rel_close(h);
    }
    printf("READ  E%u g%u r1=%u r5=%u\n",
           (unsigned)er, (unsigned)got, (unsigned)first, (unsigned)last);

    printf("\n%s\n", (eo == BBS_OK && ec == BBS_OK && er == BBS_OK &&
                      got == 30 && first == 1 && last == 5)
                     ? "SEQ BACKEND WORKS" : "SEQ BACKEND FAILS");

    /* Crash between steps 1 and 2: both files exist, original wins. */
    disk_scratch(dev, 0, "VOTE1"); disk_scratch(dev, 0, "VOTE1.NEW");
    disk_open(dev, 0, "VOTE1",     DISK_WRITE); disk_putc('A'); disk_close();
    disk_open(dev, 0, "VOTE1.NEW", DISK_WRITE); disk_putc('B'); disk_close();
    rel_seq_recover(dev, 0, "VOTE1");
    printf("BOTH:    NEW %s\n",
           disk_open(dev, 0, "VOTE1.NEW", DISK_READ) == BBS_OK &&
           disk_status(dev) != 62 ? "KEPT-BAD" : "DROPPED-OK");
    disk_close();

    /* Crash between steps 2 and 3: only the NEW exists, promote it. */
    disk_scratch(dev, 0, "VOTE1");
    disk_open(dev, 0, "VOTE1.NEW", DISK_WRITE); disk_putc('B'); disk_close();
    rel_seq_recover(dev, 0, "VOTE1");
    printf("TMPONLY: VOTE1 %s\n",
           disk_open(dev, 0, "VOTE1", DISK_READ) == BBS_OK &&
           disk_status(dev) != 62 ? "PROMOTED-OK" : "LOST-BAD");
    disk_close();

    /* rel_open()/rel_seq_recover() above went through disk_select_
     * partition(), which CD:'d into SYSTEM/ and left the cursor there.
     * SoftIEC's current directory is drive state that survives a C64 reset
     * and a power cycle, so leaving it there breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("DONE.\n");
    getch();
    return 0;
}
