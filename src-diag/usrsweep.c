/* USRSWEEP - does the boot-time sweep break the USR LOG read that follows it?
 *
 * USRREAD proved rel_seq reads USR LOG correctly standalone, yet BOOT-SIEC
 * reports "USR LOG: EMPTY" and then scratches the file. The difference is what
 * runs first. This reproduces the boot order: populate bbs_cfg the way
 * cfg_init() would, read once, run rel_seq_sweep(), read again. If the second
 * read fails where the first succeeded, the sweep is the culprit.
 *
 * IMPORTANT (found in review, not yet run on hardware): rel_open()'s region
 * cache (s_loaded[] in rel_seq.c) is only cleared by rel_reset(). The first
 * try_read() below marks the USR LOG region loaded; without an intervening
 * rel_reset(), every later try_read() in this file serves the REU copy from
 * that first read and never touches the disk again — so POSTRQ/AFTER were
 * not actually re-reading USR LOG off disk, only replaying REU. rel_reset()
 * calls before POSTRQ and AFTER force a genuine region_load() each time, so
 * this now actually re-exercises the disk path rel_seq_require_storage() and
 * rel_seq_sweep() run through — including the CD: back to the system folder
 * that rel_seq_sweep() leaves pointed away from (its last touched section is
 * MSGS, not SYSTEM; see rel_seq_sweep()'s loop order in rel_seq.c). That
 * cross-section CD-then-first-load sequence is the one thing real boot does
 * that neither this file nor USRREAD exercised before this fix. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/records.h"
#include "bbs/cfg.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/reu.h"
#include "bbs/rel.h"

/* This diagnostic only makes sense in a T64_STORE_SEQ build — it drives
 * rel_seq_require_storage()/rel_seq_sweep()/disk_set_section_path(), which
 * only exist under that define (see rel.h, hal/disk.h), and the Makefile's
 * `diag` target always links it with -dT64_STORE_SEQ. Guarding the whole
 * body avoids a false cppcheck bufferAccessOutOfBounds on the non-SEQ build
 * of cfg_t, where bbs_cfg.init_system is char[16] (CFG_INIT_MAX) rather than
 * the SEQ build's char[24] — too small for the literal paths below, but a
 * config this file is never actually compiled under. */
#ifdef T64_STORE_SEQ

static u8 dev = 11;
static u8 buf[RECORD_SIZE_USER];

static void try_read(const char *label)
{
    rel_handle_t h;
    bbs_err_t eo, ep, er;
    u8 got = 0;

    eo = rel_open(dev, 0, "USR LOG", RECORD_SIZE_USER, &h);
    ep = er = BBS_EFATAL;
    memset(buf, 0, sizeof(buf));
    if (eo == BBS_OK) {
        ep = rel_position(h, 1);
        er = rel_read(h, buf, RECORD_SIZE_USER, &got);
        rel_close(h);
    }
    printf("  %-6s O%u P%u R%u G%u ID%u %s\n", label,
           (unsigned)eo, (unsigned)ep, (unsigned)er, (unsigned)got,
           (unsigned)buf[0], buf[0] == 1 ? "OK" : "BAD");
}

int main(void)
{
    printf("\x93\x8e");
    printf("SWEEP VS USR LOG\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    printf("REU %u KB\n", (unsigned)reu_detect());

    bbs_cfg.device_system = dev; bbs_cfg.drive_system = 0;
    bbs_cfg.device_msgs   = dev; bbs_cfg.drive_msgs   = 1;
    bbs_cfg.device_files  = dev; bbs_cfg.drive_files  = 2;
    bbs_cfg.device_doors  = dev; bbs_cfg.drive_doors  = 3;
    strcpy(bbs_cfg.init_system, "/USB1/TURBO64/SYSTEM");
    strcpy(bbs_cfg.init_msgs,   "/USB1/TURBO64/MSGS");
    strcpy(bbs_cfg.init_files,  "/USB1/TURBO64/FILES");
    strcpy(bbs_cfg.init_doors,  "/USB1/TURBO64/DOORS");
    disk_set_section_path(0, bbs_cfg.init_system);
    disk_set_section_path(1, bbs_cfg.init_msgs);
    disk_set_section_path(2, bbs_cfg.init_files);
    disk_set_section_path(3, bbs_cfg.init_doors);

    rel_reset();
    try_read("BEFORE");
    printf("REQUIRE...\n");
    rel_seq_require_storage();
    rel_reset();   /* force a real region_load(), not a REU-cache replay */
    try_read("POSTRQ");
    printf("SWEEPING...\n");
    rel_seq_sweep();
    rel_reset();   /* same: AFTER must hit disk, not the BEFORE/POSTRQ cache */
    try_read("AFTER");

    /* Every try_read() above went through rel_open() -> disk_select_
     * partition(), which CD:'d into SYSTEM/ and left the cursor there.
     * SoftIEC's current directory is drive state that survives a C64 reset
     * and a power cycle, so leaving it there breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("\nDONE.\n");
    getch();
    return 0;
}

#else

int main(void) {
    printf("\x93\x8eUSRSWEEP NEEDS T64_STORE_SEQ\n");
    getch();
    return 0;
}

#endif /* T64_STORE_SEQ */
