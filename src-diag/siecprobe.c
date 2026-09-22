/* SIECPROBE - the four unknowns gating the SoftIEC SEQ backend.
 *
 * WHY incremental printing: this runs on hardware unreachable from the dev
 * machine, and a hang is diagnosed from the last line left on screen. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <c64/kernalio.h>
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/reu.h"

static u8 dev = 11;

static u8 probe_exists(const char *name)
{
    u8 st;
    if (disk_open(dev, 0, name, DISK_READ) != BBS_OK) return 255;
    disk_close();
    st = disk_status(dev);
    return st;
}

static void t1_rename_suffix(void)
{
    u8 st;
    printf("T1 RENAME SUFFIX\n");
    disk_scratch(dev, 0, "SP1.NEW");
    disk_scratch(dev, 0, "SP1");
    if (disk_open(dev, 0, "SP1.NEW", DISK_WRITE) != BBS_OK) {
        printf("  OPEN FAIL\n"); return;
    }
    disk_putline("X");
    disk_close();
    disk_rename(dev, 0, "SP1.NEW", "SP1");
    st = probe_exists("SP1");
    printf("  SP1 ST%u %s\n", (unsigned)st, st == 0 ? "OK" : "BAD");
    st = probe_exists("SP1.NEW");
    printf("  SP1.NEW ST%u %s\n", (unsigned)st,
           st == 62 ? "GONE-OK" : "STILL-THERE");
}

static void t3_cd_recovers(void)
{
    u8 st;
    printf("T3 CD RECOVERS\n");
    disk_cmd(dev, "CD:STRAND");
    st = probe_exists("SP1");
    printf("  STRANDED ST%u\n", (unsigned)st);
    disk_cmd(dev, "CD:/USB1/TURBO64");
    st = probe_exists("SP1");
    printf("  AFTER CD ST%u %s\n", (unsigned)st, st == 0 ? "RECOVERED" : "STUCK");
}

static void t4_cd_missing(void)
{
    u8 st;
    printf("T4 CD MISSING\n");
    disk_cmd(dev, "CD:/USB1/NOSUCHDIR");
    st = disk_status(dev);
    printf("  CD ST%u\n", (unsigned)st);
    st = probe_exists("SP1");
    printf("  CURSOR ST%u %s\n", (unsigned)st,
           st == 0 ? "UNMOVED" : "MOVED-OR-LOST");
    disk_cmd(dev, "CD:/USB1/TURBO64");
}

static u32 jiffies(void)
{
    /* Read twice and retry on mismatch: the IRQ can tick mid-read. */
    u32 a, b;
    do {
        a = ((u32)(*(volatile u8 *)0xA0) << 16) |
            ((u32)(*(volatile u8 *)0xA1) << 8) |
             (u32)(*(volatile u8 *)0xA2);
        b = ((u32)(*(volatile u8 *)0xA0) << 16) |
            ((u32)(*(volatile u8 *)0xA1) << 8) |
             (u32)(*(volatile u8 *)0xA2);
    } while (a != b);
    return a;
}

static void report(const char *label, u32 t0, u32 t1, u16 bytes)
{
    u32 j = t1 - t0;
    if (j == 0) j = 1;
    /* (u32) casts are load-bearing: 8600 * 60 overflows 16 bits.
     * (unsigned long) casts satisfy cppcheck's %lu type check; oscar64's
     * u32 is already the same width. */
    printf("  %s %luJ %lu.%luS", label, (unsigned long)j,
           (unsigned long)(j / 60), (unsigned long)(((j % 60) * 10) / 60));
    if (bytes) printf(" %luB/S", (unsigned long)(((u32)bytes * 60) / j));
    printf("\n");
}

static u8 s_buf[64];

/* M1: whole-file read into REU, the lazy-load cost per record set. */
static void m1_read(u16 bytes)
{
    u32 t0, t1;
    u16 total = 0;

    printf("M1 READ %uB\n", (unsigned)bytes);
    t0 = jiffies();
    if (disk_open(dev, 0, "PERF.DAT", DISK_READ) != BBS_OK) {
        printf("  OPEN FAIL\n"); return;
    }
    for (;;) {
        i16 got = disk_read(s_buf, sizeof(s_buf));
        if (got <= 0) break;
        reu_data_put(total, s_buf, (u16)got);
        total += (u16)got;
    }
    disk_close();
    t1 = jiffies();
    report("BULK", t0, t1, total);
}

/* M2: whole-file write byte-at-a-time - what region_flush() would actually do. */
static void m2_write_slow(u16 bytes)
{
    u32 t0, t1;
    u16 i;

    printf("M2 WRITE %uB PUTC\n", (unsigned)bytes);
    disk_scratch(dev, 0, "PERF.DAT");
    t0 = jiffies();
    if (disk_open(dev, 0, "PERF.DAT", DISK_WRITE) != BBS_OK) {
        printf("  OPEN FAIL\n"); return;
    }
    for (i = 0; i < bytes; i++) disk_putc((char)('A' + (i & 15)));
    disk_close();
    t1 = jiffies();
    report("PUTC", t0, t1, bytes);
}

/* M3: the same write via krnio_write() - is a bulk HAL path worth adding? */
static void m3_write_bulk(u16 bytes)
{
    u32 t0, t1;
    u16 done = 0, i;

    printf("M3 WRITE %uB BULK\n", (unsigned)bytes);
    for (i = 0; i < sizeof(s_buf); i++) s_buf[i] = (u8)('A' + (i & 15));
    disk_scratch(dev, 0, "PERF2.DAT");
    t0 = jiffies();
    if (disk_open(dev, 0, "PERF2.DAT", DISK_WRITE) != BBS_OK) {
        printf("  OPEN FAIL\n"); return;
    }
    while (done < bytes) {
        u16 chunk = (u16)(bytes - done);
        if (chunk > sizeof(s_buf)) chunk = sizeof(s_buf);
        krnio_write(CFG_FNUM_DATA, (const char *)s_buf, (int)chunk);
        done += chunk;
    }
    disk_close();
    t1 = jiffies();
    report("BULK", t0, t1, bytes);
}

/* M4: linear search over 100 user records in REU vs the same over disk.
 * user_find_by_handle() scans; this is whether the REU tier earns its keep. */
static void m4_search(void)
{
    u32 t0, t1;
    u16 i;

    printf("M4 SEARCH 100X30\n");
    t0 = jiffies();
    for (i = 0; i < 100; i++) {
        reu_data_get((u16)(i * 30), s_buf, 30);
        if (s_buf[1] == 0xFF) break;   /* never matches; forces the full scan */
    }
    t1 = jiffies();
    report("REU ", t0, t1, 0);

    t0 = jiffies();
    if (disk_open(dev, 0, "PERF.DAT", DISK_READ) == BBS_OK) {
        for (i = 0; i < 100; i++) {
            i16 got = disk_read(s_buf, 30);
            if (got <= 0) break;
        }
        disk_close();
        t1 = jiffies();
        report("DISK", t0, t1, 0);
    } else {
        printf("  OPEN FAIL\n");
    }
}

/* M5: the durability tax per flush, and one existence probe (x36 = boot sweep). */
static void m5_recovery(void)
{
    u32 t0, t1;
    u8 i;

    printf("M5 RECOVERY\n");
    t0 = jiffies();
    disk_scratch(dev, 0, "PERF2.DAT");
    disk_rename(dev, 0, "PERF.DAT", "PERF2.DAT");
    t1 = jiffies();
    report("SCR+REN", t0, t1, 0);

    t0 = jiffies();
    for (i = 0; i < 10; i++) (void)probe_exists("NOSUCH.DAT");
    t1 = jiffies();
    report("10 PROBES", t0, t1, 0);

    disk_rename(dev, 0, "PERF2.DAT", "PERF.DAT");
}

int main(void)
{
    printf("\x93\x8e");
    printf("SIECPROBE\nDEV? ");
    for (;;) { int c = getch();
        if (c >= '8' && c <= '9') { dev = (u8)(c - '0'); break; }
        if (c == '1') { dev = 10; break; }
        if (c == '2') { dev = 11; break; } }
    printf("%u\n\n", (unsigned)dev);

    disk_cmd(dev, "CD:/USB1/TURBO64");
    t1_rename_suffix();
    t3_cd_recovers();
    t4_cd_missing();

    printf("\nT2 IS MANUAL - SEE README\n");

    printf("REU %u KB\n", (unsigned)reu_detect());
    if (reu_data_available()) {
        m2_write_slow(3000);
        m1_read(3000);
        m4_search();
        m3_write_bulk(3000);
        m2_write_slow(8600);
        m1_read(8600);
        m5_recovery();
    } else {
        printf("NO REU DATA TIER - M1-M5 SKIPPED\n");
        printf("  SIZE %u KB BANKS %u\n",
               (unsigned)bbs_cfg.reu_detected_size, (unsigned)reu_bank_count());
    }

    /* T3/T4 above deliberately strand the cursor mid-test to measure CD:
     * recovery, and already CD back themselves before returning — this is
     * a final safety net, not a substitute for that. SoftIEC's current
     * directory is drive state that survives a C64 reset and a power
     * cycle, so ending anywhere but the tree root breaks the next
     * LOAD"BOOTSIEC",11 from BASIC with FILE NOT FOUND. */
    disk_cmd(dev, "CD:/USB1/TURBO64");

    printf("DONE.\n");
    getch();
    return 0;
}
