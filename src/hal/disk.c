/* CBM IEC sequential disk I/O using oscar64 kernalio. */
#include "bbs/hal/disk.h"
#include "bbs/config.h"
#include "bbs/cfg.h"
#include <c64/kernalio.h>
#include <string.h>

char disk_errmsg[40] = { 0 };

/* Scratch channel 15 and re-open for status read. */
static u8 read_status(u8 device)
{
    int r;
    krnio_close(CFG_FNUM_CMD);
    krnio_setnam("");
    krnio_open(CFG_FNUM_CMD, device, 15);
    r = krnio_gets(CFG_FNUM_CMD, disk_errmsg, (int)sizeof(disk_errmsg));
    krnio_close(CFG_FNUM_CMD);
    if (r <= 0) return 99;
    return (u8)((disk_errmsg[0] - '0') * 10 + (disk_errmsg[1] - '0'));
}

static bbs_err_t check_status(u8 device)
{
    u8 code = read_status(device);
    if (code < 20)  return BBS_OK;
    if (code == 62) return BBS_ENOTFOUND;
    return BBS_EIO;
}

static u8 s_open_device = 0;

/* Last successfully-selected (device, partition) — see disk_select_partition().
 * 0xFF is not a valid CBM device number, so it doubles as the "nothing
 * cached" sentinel without a separate valid flag. */
static u8 s_part_device    = 0xFF;
static u8 s_part_partition = 0;

#ifdef T64_STORE_SEQ
/* Absolute folder path per section, registered by disk_set_section_path()
 * once cfg_init() has read CONFIG. Pointers only — see disk.h. */
static const char *s_section_path[CFG_SECTION_COUNT] = { 0, 0, 0, 0, 0 };

/* Marker-file arrival check runs once per section, the first time its
 * folder is selected. CD: to a wrong/missing folder returns DOS status 0
 * (measured on hardware) and silently leaves the cursor where it was, so
 * there is no error to catch at the CD: layer itself — only opening a file
 * known to live in the real folder and reading its DOS status proves the
 * folder is correct. 62 = file not found is the only trustworthy signal;
 * disk_open() returning BBS_OK is not (KERNAL OPEN succeeds on any device
 * that answers, regardless of whether the file exists). */
static bool_t s_section_verified[CFG_SECTION_COUNT] = { FALSE, FALSE, FALSE, FALSE, FALSE };

/* Last folder path actually CD:'d to, alongside the device it was sent on.
 * gfiles commonly share the system folder (a different section index,
 * same path) — without this every gfile read issued a redundant CD:
 * because the cache above keys on section index, not resolved path. */
static const char *s_last_path = 0;

void disk_set_section_path(u8 index, const char *path)
{
    if (index < CFG_SECTION_COUNT) s_section_path[index] = path;
}

/* Opens the marker directly through krnio rather than disk_open(): the
 * partition is already current, so going through the public open path
 * would recurse back into disk_select_partition() for no benefit and cost
 * scarce resident code space. */
static bbs_err_t disk_verify_section_marker(u8 device)
{
    u8 status;

    krnio_setnam("0:T64.SIEC,S,R");
    krnio_open(CFG_FNUM_DATA, device, 2);
    krnio_close(CFG_FNUM_DATA);
    status = disk_status(device);
    /* Any DOS error, not just 62: a device that is absent, a wrong file
     * type or a bad name would otherwise pass and mark the section verified
     * for the rest of the run. */
    if (status >= 20) return BBS_EIO;
    return BBS_OK;
}

#endif /* T64_STORE_SEQ */

/* Every process exit must leave the drive's persistent navigation cursor at
 * "home" — wherever the install's own binaries live — not wherever the last
 * op left it. Both builds have exactly this disease: the cursor (SoftIEC's
 * CD: target, sd2iec's CP<n> partition) is drive state that survives a
 * reset and a power cycle, so a stranded cursor breaks the operator's very
 * next LOAD"BOOT...",device with FILE NOT FOUND (same disease commit
 * e57b968 fixed for src-diag/, see cfgread.c's tail comment; verified on a
 * physical sd2iec left on partition 2 by a REL CONFIGURE session while the
 * install lived in partition 1). One function, two bodies — mirrors
 * disk_select_partition()'s own #ifdef shape, since "home" resolves
 * differently per build but the exit-time obligation is identical.
 *
 * T64_STORE_SEQ: there is no config field for the tree root; it is the
 * parent of the DEV_SYSTEM folder, captured once by cfg_init() via
 * disk_set_root_from() (see s_root_cmd below for why once, not at use).
 *
 * A CD: to a wrong-but-existing folder is harmless (nothing here reads a
 * result), and CD: to a nonexistent one leaves the cursor unmoved (measured
 * on hardware) — either way this can never be worse than the stranded
 * cursor it replaces.
 *
 * REL: "home" is NOT partition 0 — it's whatever partition the install's
 * binaries actually occupy, i.e. bbs_cfg.drive_system. This just re-invokes
 * disk_select_partition() with the config's own device/partition: that
 * function already contains the correct partition-0 handling (send nothing
 * at all — see its comment, which is explicit that this must stay
 * byte-for-byte quiet for a partition-0 install) plus the redundant-CP
 * cache, so duplicating either here would risk drifting from them. Before
 * cfg_init() has run, bbs_cfg.drive_system is still its CFG_DRIVE_DEFAULT
 * (0) zero value, so this is correctly a no-op too — the same pre-init
 * no-op the T64_STORE_SEQ body gets from init_system still being empty. */
#ifdef T64_STORE_SEQ
/* "CD:<tree root>", captured ONCE by cfg_init() from the DEV_SYSTEM path it
 * just read (the parent of the SYSTEM folder — migrate-d81.py always lays
 * the sections out as siblings under one root). Captured rather than
 * re-derived at use, because CONFIGURE's device editor rewrites
 * bbs_cfg.init_system right before cfg_save(): deriving then would send
 * the save to the NEW tree's parent, or nowhere for a bare device, while
 * the next boot still reads CONFIG from where BOOT-SIEC was loaded. Empty
 * until captured, and stays empty for a bare-device DEV_SYSTEM (no tree),
 * in which case disk_cd_root() fails closed. */
static char s_root_cmd[3 + CFG_INIT_MAX];

void disk_set_root_from(const char *system_path)
{
    u8 i, cut = 0xFF;
    for (i = 0; system_path[i]; i++) {
        if (system_path[i] == '/') cut = i;
    }
    s_root_cmd[0] = '\0';
    if (cut == 0xFF) return;                 /* no '/' at all: not a tree */
    s_root_cmd[0] = 'C'; s_root_cmd[1] = 'D'; s_root_cmd[2] = ':';
    if (cut == 0) {                          /* "/SYSTEM": the tree IS "/" */
        s_root_cmd[3] = '/'; s_root_cmd[4] = '\0';
        return;
    }
    for (i = 0; i < cut; i++) s_root_cmd[3 + i] = system_path[i];
    s_root_cmd[3 + cut] = '\0';
}

/* CD to the captured tree root. Fails closed (BBS_EIO) when no root is
 * known, so a caller writing CONFIG never silently lands in whatever
 * section the cursor happens to be in. disk_cmd() invalidates the section
 * cache, so the next section select re-issues its CD. */
bbs_err_t disk_cd_root(u8 device)
{
    if (s_root_cmd[0] == '\0') return BBS_EIO;
    return disk_cmd(device, s_root_cmd);
}
#endif

void disk_reset_cursor_root(u8 device)
{
#ifdef T64_STORE_SEQ
    (void)disk_cd_root(device);   /* nothing captured yet (pre-cfg_init): no-op */
#else
    disk_select_partition(device, bbs_cfg.drive_system);
#endif
}

bbs_err_t disk_select_partition(u8 device, u8 partition)
{
#ifdef T64_STORE_SEQ
    char cmd[28];
    const char *path;

    /* The tree root is not a section: CD there via the same derivation the
     * exit path uses. disk_cmd() invalidates the cache, so the next real
     * section select re-issues its CD instead of trusting a stale entry. */
    if (partition == CFG_SECTION_ROOT) {
        return disk_cd_root(device);
    }
    if (partition >= CFG_SECTION_COUNT) return BBS_EIO;
    path = s_section_path[partition];
    if (!path || path[0] == '\0') return BBS_OK;

    if (s_part_device == device &&
        (s_part_partition == partition ||
         (s_last_path && strcmp(s_last_path, path) == 0))) {
        return BBS_OK;
    }

    sprintf(cmd, "CD:%s", path);
    if (disk_cmd(device, cmd) != BBS_OK) {
        /* Never leave a stale cache entry behind: the next call must retry
         * the switch rather than assume the cursor moved. */
        s_part_device = 0xFF;
        return BBS_EIO;
    }

    /* Must cache AFTER disk_cmd() returns — see the CP<n> path below for why
     * (disk_cmd() unconditionally invalidates this cache). */
    s_part_device    = device;
    s_part_partition = partition;
    s_last_path      = path;

    if (!s_section_verified[partition]) {
        bbs_err_t verify_err = disk_verify_section_marker(device);
        if (verify_err != BBS_OK) {
            s_part_device = 0xFF;
            return verify_err;
        }
        s_section_verified[partition] = TRUE;
    }
    return BBS_OK;
#else
    char cmd[8];
    bbs_err_t err;

    /* CFG_DRIVE_DEFAULT is 0, so this is the common case for every install
     * that has never touched the partition field. Sending "CP0" is harmless
     * on an unpartitioned disk (measured on hardware), but skipping it
     * entirely guarantees byte-for-byte identical IEC traffic to pre-fix
     * behaviour for those installs. Do not "simplify" this into always
     * sending CP — that property is load-bearing. */
    if (partition == 0) {
        return BBS_OK;
    }

    if (s_part_device == device && s_part_partition == partition) {
        return BBS_OK;
    }

    sprintf(cmd, "CP%u", (unsigned)partition);
    err = disk_cmd(device, cmd);
    if (err != BBS_OK) {
        return err;   /* disk_cmd() already invalidated the cache below */
    }

    /* Must cache AFTER disk_cmd() returns. disk_cmd() unconditionally
     * invalidates this cache (it has to — an arbitrary command, including a
     * SysOp's per-device init string, can itself contain "CP"), and that
     * includes the CP<n> command we just sent. Caching before the call
     * would be wiped out by our own request. */
    s_part_device    = device;
    s_part_partition = partition;
    return BBS_OK;
#endif
}

/* Load an overlay PRG (a P"OVL_..." PETSCII literal) from bbs_cfg.device_system.
 *
 * In T64_STORE_SEQ builds the drive's directory cursor is persistent state
 * (see disk_select_partition() above); every overlay load must position it
 * first or krnio_load silently resolves against whatever folder a prior
 * operation left the cursor in and fails without telling anyone (the exact
 * bug this function exists to close — see git history for the WFC-overlay
 * incident it replaces). All overlays load from the system section (index
 * 0) because that is where the cursor already sits for nearly all of the
 * BBS's runtime (content opens go through device_system/drive_system too);
 * only OVL_BOOT and CONFIG stay at the tree root, loaded before cfg_init()
 * has registered any section path to CD to. In the REL build,
 * disk_select_partition() is skipped entirely — overlay loads have never
 * needed or sent a CP<n> and must not start now.
 *
 * Returns BBS_EIO if positioning or the load itself fails. Callers must
 * not transfer control into the overlay region when this returns non-OK:
 * the load failing leaves whatever was resident there before, valid or
 * not, which is exactly the stale-overlay hazard this guards against. */
bbs_err_t disk_load_overlay(const char *name)
{
#ifdef T64_STORE_SEQ
    if (disk_select_partition(bbs_cfg.device_system, bbs_cfg.drive_system) != BBS_OK) {
        return BBS_EIO;
    }
#endif
    krnio_setnam(name);
    if (!krnio_load(1, bbs_cfg.device_system, 1)) {
        return BBS_EIO;
    }
    return BBS_OK;
}

bbs_err_t disk_open(u8 device, u8 drive, const char *name, disk_mode_t mode)
{
    char fname[48];
    const char *suffix;
    bbs_err_t err = disk_select_partition(device, drive);
    if (err != BBS_OK) return err;

    /* The partition is now selected as persistent drive state (CP<n>), so
     * every filename uses the literal drive-0 form — a 1581 only exposes
     * drive 0; the old "<drive>:" prefix was a CBM *drive number*, not a
     * partition number, and silently failed to open on real hardware. */
    switch (mode) {
    case DISK_READ:   suffix = ",S,R"; break;
    case DISK_WRITE:  sprintf(fname, "@0:%s,S,W", name);
                      goto do_open;
    case DISK_APPEND: suffix = ",S,A"; break;
    case DISK_OVER:   sprintf(fname, "@0:%s,S,W", name);
                      goto do_open;
    default:          suffix = ",S,W"; break;
    }
    sprintf(fname, "0:%s%s", name, suffix);

do_open:
    krnio_setnam(fname);
    if (!krnio_open(CFG_FNUM_DATA, device, 2)) {
        return BBS_EIO;
    }

    /* Reset per-file EOF state: krnio_pstatus[fnum] persists across close/open.
     * A prior read to EOF leaves KRNIO_EOF, causing the next krnio_gets to
     * return 0 immediately without touching the IEC bus. */
    krnio_pstatus[CFG_FNUM_DATA] = KRNIO_OK;
    s_open_device = device;
    return BBS_OK;
}

void disk_close(void)
{
    krnio_clrchn();
    krnio_close(CFG_FNUM_DATA);
    krnio_pstatus[CFG_FNUM_DATA] = KRNIO_OK; /* reset for next open */
    s_open_device = 0;
}

i16 disk_getc(void)
{
    int v = krnio_getch(CFG_FNUM_DATA);
    if (v < 0) return -1;
    if (krnio_pstatus[CFG_FNUM_DATA] & KRNIO_EOF) return -1;
    return (i16)(v & 0xFF);
}

i16 disk_read(u8 *buf, u8 len)
{
    /* krnio_read() does ONE krnio_chkin() + N×krnio_chrin() + ONE krnio_clrchn().
     * Dramatically faster than disk_getc() for bulk sequential reads:
     * krnio_getch() negotiates the IEC bus (CHKIN+CLRCHN) for every byte. */
    int r = krnio_read(CFG_FNUM_DATA, (char *)buf, (int)len);
    if (r < 0) return -1;
    return (i16)r;
}

i16 disk_gets(char *buf, u8 len)
{
    int r = krnio_gets(CFG_FNUM_DATA, buf, (int)len);
    if (r < 0) return -1;
    return (i16)r;
}

bbs_err_t disk_putc(char c)
{
    int r = krnio_putch(CFG_FNUM_DATA, c);
    return (r >= 0) ? BBS_OK : BBS_EIO;
}

/* Bulk write: one CHKOUT + N x CHROUT + one CLRCHN, amortising the channel
 * overhead that disk_putc() pays per byte. Measured 6.4x faster on SoftIEC.
 *
 * krnio_write() returns `num` whenever krnio_chkout() succeeds — it never
 * inspects KERNAL status (ST) between CHROUT calls, so a returned count
 * equal to `len` only proves the channel opened, not that the bytes reached
 * the drive. Check krnio_status() (KERNAL READST, $FFB7) right after: a
 * write that failed partway (disk full, device dropped) leaves it non-zero
 * even though krnio_write() already returned success. */
bbs_err_t disk_write(const u8 *buf, u8 len)
{
    int r = krnio_write(CFG_FNUM_DATA, (const char *)buf, (int)len);
    if (r != (int)len) return BBS_EIO;
    return (krnio_status() == KRNIO_OK) ? BBS_OK : BBS_EIO;
}

bbs_err_t disk_puts(const char *s)
{
    int r = krnio_puts(CFG_FNUM_DATA, s);
    return (r >= 0) ? BBS_OK : BBS_EIO;
}

bbs_err_t disk_putline(const char *s)
{
    bbs_err_t e = disk_puts(s);
    if (e != BBS_OK) return e;
    return disk_putc('\r');
}

bool_t disk_eof(void)
{
    return (krnio_pstatus[CFG_FNUM_DATA] & KRNIO_EOF) ? TRUE : FALSE;
}

bbs_err_t disk_scratch(u8 device, u8 drive, const char *name)
{
    char cmd[40];
    bbs_err_t err = disk_select_partition(device, drive);
    if (err != BBS_OK) return err;
    sprintf(cmd, "S:%s", name);
    return disk_cmd(device, cmd);
}

bbs_err_t disk_rename(u8 device, u8 drive,
                      const char *old_name, const char *new_name)
{
    char cmd[48];
    bbs_err_t err = disk_select_partition(device, drive);
    if (err != BBS_OK) return err;
    sprintf(cmd, "R:%s=%s", new_name, old_name);
    return disk_cmd(device, cmd);
}

bbs_err_t disk_cmd(u8 device, const char *cmd)
{
    /* An arbitrary command can itself select a partition (a SysOp's
     * init_system/init_msgs/init_files/init_doors string is exactly that
     * workaround, sent via cfg_send_drive_init() before every disk op), so
     * the cache in disk_select_partition() can never be trusted to survive
     * a disk_cmd() call — including the CP<n> call disk_select_partition()
     * itself makes through here. Invalidate unconditionally. */
    s_part_device = 0xFF;
    krnio_setnam(cmd);
    krnio_open(CFG_FNUM_CMD, device, 15);
    krnio_close(CFG_FNUM_CMD);
    return check_status(device);
}

u8 disk_status(u8 device)
{
    return read_status(device);
}

void disk_name_bull(char *buf, u8 board, u16 post)
{
    sprintf(buf, "B.%d.%d", (int)board, (int)post);
}

void disk_name_mail(char *buf, u16 msg_id, u8 user_id)
{
    sprintf(buf, "E.%d.%d", (int)msg_id, (int)user_id);
}

/**
 * disk_build_term_filename()
 *
 * Phase B: Build terminal-aware filename candidates.
 * Generates 4 candidates in priority order:
 *   1. G.<NAME> <mode> <width>  (most specific)
 *   2. G.<NAME> <mode>          (mode only)
 *   3. G.<NAME> <width>         (width only)
 *   4. G.<NAME>                 (generic fallback)
 *
 * Names are uppercase because c1541 stores filenames as uppercase PETSCII
 * (ASCII a-z → 0x41-0x5A), and the C64 KERNAL does a byte-exact compare.
 *
 * Example: base="login", mode=1, width=80
 *   names[0] = "G.LOGIN 1 80"
 *   names[1] = "G.LOGIN 1"
 *   names[2] = "G.LOGIN 80"
 *   names[3] = "G.LOGIN"
 */
void disk_build_term_filename(term_filename_t *out,
                              char prefix,
                              const char *name,
                              u8 mode, u8 width)
{
    if (!out || !name) return;

    /* CBM DOS filenames are stored as uppercase PETSCII by c1541 (ASCII a-z
     * is converted to PETSCII A-Z, i.e. 0x41-0x5A).  Send uppercase bytes
     * from the C64 side so the directory comparison succeeds. */
    char upper[16];
    u8 j;
    char pfx = (prefix >= 'a' && prefix <= 'z') ? (char)(prefix - 32) : prefix;
    for (j = 0; j < (u8)(sizeof(upper) - 1) && name[j]; j++) {
        upper[j] = (name[j] >= 'a' && name[j] <= 'z')
                   ? (char)(name[j] - 32) : name[j];
    }
    upper[j] = '\0';

    sprintf(out->names[0], "%c.%s %d %d", pfx, upper, (int)mode, (int)width);
    sprintf(out->names[1], "%c.%s %d", pfx, upper, (int)mode);
    sprintf(out->names[2], "%c.%s %d", pfx, upper, (int)width);
    sprintf(out->names[3], "%c.%s", pfx, upper);
}

/**
 * disk_open_with_fallback()
 *
 * Phase B: Open file with fallback chain.
 * Tries each candidate filename in priority order until one opens successfully.
 * Uses terminal mode and width to generate candidates.
 *
 * Returns:
 *   BBS_OK        — file opened and ready to read/write
 *   BBS_ENOTFOUND — no candidate file found
 *   BBS_EIO       — error opening file (other than not found)
 */
bbs_err_t disk_open_with_fallback(u8 device, u8 drive,
                                  char prefix,
                                  const char *base_name,
                                  disk_mode_t mode,
                                  u8 term_mode, u8 term_width)
{
    term_filename_t names;
    u8 i;
    // cppcheck-suppress variableScope
    bbs_err_t err;

    if (!base_name) {
        return BBS_EBADARG;
    }

    /* Build candidate filenames in priority order */
    disk_build_term_filename(&names, prefix, base_name, term_mode, term_width);

    /* Try each candidate in priority order */
    for (i = 0; i < 4; i++) {
        err = disk_open(device, drive, names.names[i], mode);

        /* Success: file opened */
        if (err == BBS_OK) {
            return BBS_OK;
        }

        /* File not found: try next candidate */
        if (err == BBS_ENOTFOUND) {
            continue;
        }

        /* Other error (disk error, permission denied, etc.):
         * Return error but don't continue fallback chain
         * (might be a real disk problem, not just file missing) */
        return err;
    }

    /* All candidates exhausted */
    return BBS_ENOTFOUND;
}

/* Parse disk status line for block counts.
 * CBM DOS status format (1581/SD2IEC):
 *   "00, OK,00,00\r"  — success, normal response
 *   "39, BLOCKS FREE.\r" — block count in first part
 *
 * The drive status for free blocks is returned by "B-E" command:
 *   sends "01\r02\r<free_lo>\r<free_hi>\r<total_lo>\r<total_hi>\r"
 *
 * This is a simplified parser that reads disk_status() error message
 * which contains block info when formatted properly. */
static bbs_err_t parse_blocks(u8 device, u16 *out_blocks, u8 is_free)
{
    u8 code = disk_status(device);

    /* disk_status reads into disk_errmsg and returns error code.
     * For free blocks, we parse the message. Format varies by drive type.
     * Standard approach: send "B-E" command and parse block count. */

    /* If status is 0 (OK) and we can parse the block count from errmsg... */
    if (code >= 20) {
        return BBS_EIO;  /* Disk error */
    }

    /* Parse block count from disk_errmsg (simplified).
     * Real implementation would be more sophisticated, but for now
     * we return 0 (indicating data not available) and defer to
     * a more complete block-reading implementation. */
    *out_blocks = 0;
    return BBS_OK;
}

/**
 * disk_free_blocks()
 *
 * Get disk free space in 254-byte blocks.
 * Note: Full implementation requires more sophisticated CBM DOS parsing.
 * This placeholder returns 0 (unknown) but can be extended.
 */
bbs_err_t disk_free_blocks(u8 device, u16 *out_free)
{
    if (!out_free) {
        return BBS_EBADARG;
    }
    /* Placeholder: return 0 blocks (unknown).
     * Real implementation would parse CBM DOS block info. */
    *out_free = 0;
    return BBS_OK;
}

/**
 * disk_total_blocks()
 *
 * Get disk total capacity in 254-byte blocks.
 * This is typically 3160 for 1581 (800KB), 683 for 1571 (170KB).
 */
bbs_err_t disk_total_blocks(u8 device, u16 *out_total)
{
    if (!out_total) {
        return BBS_EBADARG;
    }
    /* Placeholder: return safe default (3160 for 1581).
     * Real implementation would detect drive type. */
    *out_total = 3160;  /* 1581 default */
    return BBS_OK;
}
