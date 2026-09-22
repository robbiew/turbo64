/* CFGREAD - does cfg_init() actually populate the section paths?
 *
 * Every other diagnostic sets bbs_cfg directly and reads USR LOG fine, while
 * the real boot parses CONFIG via cfg_init() and then reports "USR LOG: EMPTY".
 * That is the last untested difference. If init_system comes back empty,
 * disk_select_partition() treats an empty path as "stay put" (hal/disk.c), so
 * USR LOG gets opened from the tree root where it does not exist — which would
 * explain the failure exactly. */
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
#include "bbs/net.h"

#ifdef T64_STORE_SEQ

static u8 dev = 11;
static u8 buf[RECORD_SIZE_USER];

static void show(const char *label, const char *p)
{
    printf("  %-5s L%u [%s]\n", label, (unsigned)strlen(p), p);
}

int main(void)
{
    bbs_err_t ec, eo, ep, er;
    rel_handle_t h;
    u8 got = 0;

    printf("\x93\x8e");
    printf("CFG_INIT VS USR LOG\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    /* Boot order matters and is the whole point. cfg_set_defaults() — which
     * runs inside cfg_init() — clears bbs_cfg.reu_enabled (cfg.c), and
     * rel_open() refuses to work without it. The real boot_sequence() calls
     * cfg_init() FIRST and only then reu_detect() (via
     * rel_seq_require_storage()), so reproduce that order exactly rather than
     * detecting up front. */
    disk_cmd(dev, "CD:/USB1/TURBO64");
    ec = cfg_init();
    printf("CFG_INIT E%u\n", (unsigned)ec);
    printf("REU AFTER CFG: EN%u\n", (unsigned)bbs_cfg.reu_enabled);
    rel_seq_require_storage();
    printf("REQ OK  EN%u SZ%u\n", (unsigned)bbs_cfg.reu_enabled,
           (unsigned)bbs_cfg.reu_detected_size);
    rel_seq_sweep();
    /* The last untested difference from the real boot: net_init() runs between
     * the sweep and the USR LOG check. */
    printf("NET_INIT E%u\n", (unsigned)net_init());
    printf("  DEVSYS %u DRVSYS %u\n",
           (unsigned)bbs_cfg.device_system, (unsigned)bbs_cfg.drive_system);
    show("SYS", bbs_cfg.init_system);
    show("MSGS", bbs_cfg.init_msgs);
    show("GFILE", bbs_cfg.init_gfiles);

    rel_reset();
    eo = rel_open(bbs_cfg.device_system, bbs_cfg.drive_system,
                  "USR LOG", RECORD_SIZE_USER, &h);
    ep = er = BBS_EFATAL;
    memset(buf, 0, sizeof(buf));
    if (eo == BBS_OK) {
        ep = rel_position(h, 1);
        er = rel_read(h, buf, RECORD_SIZE_USER, &got);
        rel_close(h);
    }
    printf("READ O%u P%u R%u G%u ID%u %s\n",
           (unsigned)eo, (unsigned)ep, (unsigned)er, (unsigned)got,
           (unsigned)buf[0], buf[0] == 1 ? "OK" : "BAD");

    /* rel_open() above left the cursor inside SYSTEM/ via disk_select_
     * partition(). SoftIEC's current directory is drive state that survives
     * a C64 reset and a power cycle, so leaving it there breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("\nDONE.\n");
    getch();
    return 0;
}

#else

int main(void) {
    printf("\x93\x8eCFGREAD NEEDS T64_STORE_SEQ\n");
    getch();
    return 0;
}

#endif /* T64_STORE_SEQ */
