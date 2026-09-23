/* bbs/hal/reu.c - RAM Expansion Unit (REU) implementation. */

#include "bbs/hal/reu.h"
#include "bbs/cfg.h"
#include "bbs/rel.h"
#include "bbs/hal/disk.h"
#include "bbs/config.h"
#include <string.h>
#include <stdio.h>

/* REU registers at $DF00 — standard CBM 17xx layout */
#define REU_BASE_ADDR 0xDF00

#define R_STATUS   0x00   /* Status  (read): bit4=present, bit6=EOB      */
#define R_CMD      0x01   /* Command (write)                              */
#define R_C64_LO   0x02   /* C64 base address low                        */
#define R_C64_HI   0x03   /* C64 base address high                       */
#define R_REU_LO   0x04   /* REU address low                             */
#define R_REU_MID  0x05   /* REU address mid                             */
#define R_REU_HI   0x06   /* REU address high                            */
#define R_LEN_LO   0x07   /* Transfer length low                         */
#define R_LEN_HI   0x08   /* Transfer length high                        */

#define CMD_STASH      0x90   /* Execute | Autoload | C64->REU */
#define CMD_FETCH      0x91   /* Execute | Autoload | REU->C64 */
#define STATUS_EOB     0x40   /* bit 6: end-of-block */
#define STATUS_PRESENT 0x10   /* bit 4: REU present  */

/* REU size constants (KB) */
#define REU_SIZE_NONE   0
#define REU_SIZE_128KB  128
#define REU_SIZE_256KB  256
#define REU_SIZE_512KB  512
#define REU_SIZE_1MB    1024
#define REU_SIZE_2MB    2048
#define REU_SIZE_4MB    4096
#define REU_SIZE_8MB    8192
#define REU_SIZE_16MB   16384

/* DMA staging buffer (RECORD_SIZE_MSG_IDX = 63 bytes). Placed in the KERNAL
 * cassette buffer at $03BC-$03FA, immediately above the modem RX ring
 * ($033C-$03BB). The BBS never touches tape, so this RAM is free — keeping the
/* DMA staging buffer — RECORD_SIZE_MSG_IDX (63) bytes. Placed in the KERNAL
 * cassette buffer ($03BC-$03FA), just above the modem RX ring ($033C-$03BB).
 * The BBS never uses tape, so this RAM is free; keeping the buffer out of the
 * resident region ($0880-$9700), which is at its hard memory ceiling. The
 * buffer is always written before it is read, so it needs no zero-init.
 * (A named const pointer, not a cast macro — oscar64 won't index either a
 * cast-expression pointer or a pointer-to-array deref.) */
static u8 * const s_dma_buf = (u8 *)0x03BCu;

static u16 s_compose_len = 0;

#define BODY_CACHE_SLOTS 1
static struct {
    u8     board_id;
    u16    msg_id;
    u8     bank;
    u16    offset;
    u16    len;
    bool_t valid;
} s_body_cache[BODY_CACHE_SLOTS];
static u8 s_body_evict = 0;

/* ----------------------------------------------------------------------- */
/* DMA helpers                                                              */
/* ----------------------------------------------------------------------- */

static void dma_stash(const void *c64_ptr, u8 bank, u16 reu_off, u16 len)
{
    volatile u8 *r = (volatile u8 *)REU_BASE_ADDR;
    u16 addr = (u16)(uintptr_t)c64_ptr;
    r[R_C64_LO]  = (u8)(addr & 0xFF);
    r[R_C64_HI]  = (u8)(addr >> 8);
    r[R_REU_LO]  = (u8)(reu_off & 0xFF);
    r[R_REU_MID] = (u8)(reu_off >> 8);
    r[R_REU_HI]  = bank;
    r[R_LEN_LO]  = (u8)(len & 0xFF);
    r[R_LEN_HI]  = (u8)(len >> 8);
    r[R_CMD]     = CMD_STASH;
    while ((r[R_STATUS] & STATUS_EOB) == 0) ;
}

// cppcheck-suppress constParameterPointer
static void dma_fetch(void *c64_ptr, u8 bank, u16 reu_off, u16 len)
{
    volatile u8 *r = (volatile u8 *)REU_BASE_ADDR;
    u16 addr = (u16)(uintptr_t)c64_ptr;
    r[R_C64_LO]  = (u8)(addr & 0xFF);
    r[R_C64_HI]  = (u8)(addr >> 8);
    r[R_REU_LO]  = (u8)(reu_off & 0xFF);
    r[R_REU_MID] = (u8)(reu_off >> 8);
    r[R_REU_HI]  = bank;
    r[R_LEN_LO]  = (u8)(len & 0xFF);
    r[R_LEN_HI]  = (u8)(len >> 8);
    r[R_CMD]     = CMD_FETCH;
    while ((r[R_STATUS] & STATUS_EOB) == 0) ;
}

/* Detection stash/fetch: take explicit c64/reu address as u16/u32 (no pointer).
 * Used only by reu_detect_internal() so the volatile register pointer stays local. */
static void det_stash(volatile u8 *r, u16 c64a, u32 reu_a)
{
    r[R_C64_LO]  = (u8)(c64a & 0xFF);
    r[R_C64_HI]  = (u8)(c64a >> 8);
    r[R_REU_LO]  = (u8)(reu_a & 0xFF);
    r[R_REU_MID] = (u8)((reu_a >> 8) & 0xFF);
    r[R_REU_HI]  = (u8)((reu_a >> 16) & 0xFF);
    r[R_LEN_LO]  = 1;
    r[R_LEN_HI]  = 0;
    r[R_CMD]     = CMD_STASH;
    while ((r[R_STATUS] & STATUS_EOB) == 0) ;
}

static void det_fetch(volatile u8 *r, u16 c64a, u32 reu_a)
{
    r[R_C64_LO]  = (u8)(c64a & 0xFF);
    r[R_C64_HI]  = (u8)(c64a >> 8);
    r[R_REU_LO]  = (u8)(reu_a & 0xFF);
    r[R_REU_MID] = (u8)((reu_a >> 8) & 0xFF);
    r[R_REU_HI]  = (u8)((reu_a >> 16) & 0xFF);
    r[R_LEN_LO]  = 1;
    r[R_LEN_HI]  = 0;
    r[R_CMD]     = CMD_FETCH;
    while ((r[R_STATUS] & STATUS_EOB) == 0) ;
}

/* ----------------------------------------------------------------------- */
/* Detection                                                                */
/* ----------------------------------------------------------------------- */

/* DMA scratch for detection — a dedicated 2-byte buffer, NOT a hardcoded low
 * address. reu_detect() runs from boot_sequence() while main() is still on the
 * call stack, and the old fixed $0900/$0901 scratch sat INSIDE main()'s own
 * code (main spans ~$0880-$0da4). The size probe scribbled sentinels onto those
 * two bytes of live code — harmless only as long as they weren't reached again,
 * until -dNOFLOAT reshuffled main() onto them and it broke the boot on real
 * hardware (VICE tolerated it). A BSS buffer is never code, so it's always safe. */
static volatile u8 s_det_scratch[2];

static u16 reu_detect_internal(void)
{
    volatile u8 *r = (volatile u8 *)REU_BASE_ADDR;
    volatile u8 *buf  = &s_det_scratch[0];   /* 1-byte stash source */
    volatile u8 *back = &s_det_scratch[1];   /* 1-byte fetch dest   */
    u16 buf_a  = (u16)(uintptr_t)buf;
    u16 back_a = (u16)(uintptr_t)back;

    /* Halt any running DMA, then check Status bit 4 = REU present. */
    r[R_CMD] = 0x00;
    if ((r[R_STATUS] & STATUS_PRESENT) == 0) {
        return REU_SIZE_NONE;
    }

    /* Size probe via wrap-around:
     * Seed $000000 with a known byte, then write a distinct sentinel to
     * each power-of-two boundary. After each write, fetch back $000000:
     * if it changed, the boundary wrapped — that's the REU size.
     *
     * Boundaries: $020000=128KB, $040000=256KB, $080000=512KB,
     *   $100000=1MB, $200000=2MB, $400000=4MB, $800000=8MB.
     * No 16MB boundary to test — if nothing wraps, report 16MB.
     */

    /* Seed base */
    buf[0] = 0xAA;
    det_stash(r, buf_a, 0x000000UL);

#define PROBE(sentinel, boundary, retval)       \
    buf[0] = (sentinel);                        \
    det_stash(r, buf_a, (boundary));            \
    back[0] = 0x00;                             \
    det_fetch(r, back_a, 0x000000UL);           \
    if (back[0] == (sentinel)) { return (retval); }

    PROBE(0xBB, 0x020000UL, REU_SIZE_128KB)
    PROBE(0xCC, 0x040000UL, REU_SIZE_256KB)
    PROBE(0xDD, 0x080000UL, REU_SIZE_512KB)
    PROBE(0xEE, 0x100000UL, REU_SIZE_1MB)
    PROBE(0xE1, 0x200000UL, REU_SIZE_2MB)
    PROBE(0xE2, 0x400000UL, REU_SIZE_4MB)
    PROBE(0xE3, 0x800000UL, REU_SIZE_8MB)

#undef PROBE

    /* No wrap detected up to 8MB boundary -> 16MB (hardware max) */
    return REU_SIZE_16MB;
}

/* Returns the detected size in KB (0 if absent). Also records it in
 * bbs_cfg.reu_detected_size / reu_enabled. main.c prints the return value, so
 * it must be the real size, not a bool. */
/* Boot-only (boot_sequence() and rel_seq_require_storage(), both of which
 * run from the boot overlay), so it lives there too and costs nothing
 * resident. The editor build has no overlays and keeps it in main. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif
u16 reu_detect(void)
{
    u16 sz = reu_detect_internal();
    bbs_cfg.reu_detected_size = sz;
    bbs_cfg.reu_enabled       = (sz != REU_SIZE_NONE) ? TRUE : FALSE;
    return sz;
}
#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

u8 reu_bank_count(void)
{
    switch (bbs_cfg.reu_detected_size) {
        case REU_SIZE_NONE:  return 0;
        case REU_SIZE_128KB: return 2;
        case REU_SIZE_256KB: return 4;
        default:             return 8;  /* 512 KB and above */
    }
}

/* ----------------------------------------------------------------------- */
/* Bank 0: active board message index                                       */
/* ----------------------------------------------------------------------- */

bbs_err_t reu_index_load(u8 board_id, u8 device)
{
    rel_handle_t h;
    char fname[16];
    u8   got;
    u16  rec;
    bbs_err_t err;

    sprintf(fname, "B%u.IDX", (unsigned)board_id);

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    err = rel_open(device, bbs_cfg.drive_msgs, fname, RECORD_SIZE_MSG_IDX, &h);
    if (err != BBS_OK) return BBS_EIO;

    err = BBS_OK;
    for (rec = 1; rec <= CFG_MSG_MAX_PER_BOARD; rec++) {
        memset(s_dma_buf, 0, RECORD_SIZE_MSG_IDX);
        err = rel_position(h, rec);
        if (err != BBS_OK) break;

        err = rel_read(h, s_dma_buf, RECORD_SIZE_MSG_IDX, &got);
        if (err != BBS_OK || got < 4) break;

        /* DMA each record to Bank 0 at offset (rec-1)*32 */
        dma_stash(s_dma_buf, 0, (u16)((rec - 1) * RECORD_SIZE_MSG_IDX),
                  RECORD_SIZE_MSG_IDX);
    }

    rel_close(h);
    /* Zero-fill slots beyond what was loaded — REU is not reset between board
     * switches, so stale records from a previous load must be cleared. */
    memset(s_dma_buf, 0, RECORD_SIZE_MSG_IDX);
    for (; rec <= CFG_MSG_MAX_PER_BOARD; rec++) {
        dma_stash(s_dma_buf, 0, (u16)((rec - 1) * RECORD_SIZE_MSG_IDX),
                  RECORD_SIZE_MSG_IDX);
    }
    /* BBS_ENOTFOUND on the last record means end-of-file — that's normal */
    return (err == BBS_ENOTFOUND) ? BBS_OK : err;
}

bbs_err_t reu_index_flush(u8 board_id, u8 device)
{
    rel_handle_t h;
    char fname[16];
    u16  rec;
    bbs_err_t err;

    sprintf(fname, "B%u.IDX", (unsigned)board_id);

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    err = rel_open(device, bbs_cfg.drive_msgs, fname, RECORD_SIZE_MSG_IDX, &h);
    if (err != BBS_OK) return BBS_EIO;

    for (rec = 1; rec <= CFG_MSG_MAX_PER_BOARD; rec++) {
        dma_fetch(s_dma_buf, 0, (u16)((rec - 1) * RECORD_SIZE_MSG_IDX),
                  RECORD_SIZE_MSG_IDX);

        /* Stop at first all-zero record (unused slot) */
        if (s_dma_buf[0] == 0 && s_dma_buf[1] == 0) break;

        err = rel_position(h, rec);
        if (err != BBS_OK) { rel_close(h); return BBS_EIO; }

        err = rel_write(h, s_dma_buf, RECORD_SIZE_MSG_IDX);
        if (err != BBS_OK) { rel_close(h); return BBS_EIO; }
    }

    rel_close(h);
    return BBS_OK;
}

void reu_index_get(u16 msg_id, msg_index_record_t *out)
{
    u16 off = (u16)((msg_id - 1) * RECORD_SIZE_MSG_IDX);
    dma_fetch(s_dma_buf, 0, off, RECORD_SIZE_MSG_IDX);

    out->msg_id         = (u16)s_dma_buf[0] | ((u16)s_dma_buf[1] << 8);
    out->parent_id      = (u16)s_dma_buf[2] | ((u16)s_dma_buf[3] << 8);
    out->thread_root_id = (u16)s_dma_buf[4] | ((u16)s_dma_buf[5] << 8);
    out->author_id      = (u16)s_dma_buf[6] | ((u16)s_dma_buf[7] << 8);
    out->date           = (u16)s_dma_buf[8] | ((u16)s_dma_buf[9] << 8);
    out->to_id          = (u16)s_dma_buf[10] | ((u16)s_dma_buf[11] << 8);
    out->flags          = s_dma_buf[12];
    out->reply_count    = s_dma_buf[13];
    out->body_offset    = (u16)s_dma_buf[14] | ((u16)s_dma_buf[15] << 8);
    out->body_len       = (u16)s_dma_buf[16] | ((u16)s_dma_buf[17] << 8);
    memcpy(out->net_origin_bbs, &s_dma_buf[18], 8);
    out->net_origin_id  = (u16)s_dma_buf[26] | ((u16)s_dma_buf[27] << 8);
    out->month    = s_dma_buf[28];
    out->day      = s_dma_buf[29];
    out->year_yy  = s_dma_buf[30];
    { u8 k; for (k = 0; k < 30; k++) out->subj[k] = (char)s_dma_buf[32 + k]; }
    out->subj[30] = '\0';
}

void reu_index_put(u16 msg_id, const msg_index_record_t *rec)
{
    u16 off = (u16)((msg_id - 1) * RECORD_SIZE_MSG_IDX);

    memset(s_dma_buf, 0, RECORD_SIZE_MSG_IDX);
    s_dma_buf[0]  = (u8)(rec->msg_id & 0xFF);
    s_dma_buf[1]  = (u8)(rec->msg_id >> 8);
    s_dma_buf[2]  = (u8)(rec->parent_id & 0xFF);
    s_dma_buf[3]  = (u8)(rec->parent_id >> 8);
    s_dma_buf[4]  = (u8)(rec->thread_root_id & 0xFF);
    s_dma_buf[5]  = (u8)(rec->thread_root_id >> 8);
    s_dma_buf[6]  = (u8)(rec->author_id & 0xFF);
    s_dma_buf[7]  = (u8)(rec->author_id >> 8);
    s_dma_buf[8]  = (u8)(rec->date & 0xFF);
    s_dma_buf[9]  = (u8)(rec->date >> 8);
    s_dma_buf[10] = (u8)(rec->to_id & 0xFF);
    s_dma_buf[11] = (u8)(rec->to_id >> 8);
    s_dma_buf[12] = rec->flags;
    s_dma_buf[13] = rec->reply_count;
    s_dma_buf[14] = (u8)(rec->body_offset & 0xFF);
    s_dma_buf[15] = (u8)(rec->body_offset >> 8);
    s_dma_buf[16] = (u8)(rec->body_len & 0xFF);
    s_dma_buf[17] = (u8)(rec->body_len >> 8);
    memcpy(&s_dma_buf[18], rec->net_origin_bbs, 8);
    s_dma_buf[26] = (u8)(rec->net_origin_id & 0xFF);
    s_dma_buf[27] = (u8)(rec->net_origin_id >> 8);
    s_dma_buf[28] = rec->month;
    s_dma_buf[29] = rec->day;
    s_dma_buf[30] = rec->year_yy;
    /* s_dma_buf[31] = reserved1, already zeroed */
    { u8 k; for (k = 0; k < 31; k++) s_dma_buf[32 + k] = (u8)rec->subj[k]; }

    dma_stash(s_dma_buf, 0, off, RECORD_SIZE_MSG_IDX);
}

/* ----------------------------------------------------------------------- */
/* Bank 1: compose buffer                                                   */
/* ----------------------------------------------------------------------- */

void reu_compose_init(void)
{
    s_compose_len = 0;
}

void reu_compose_putc(char c)
{
    u8 byte = (u8)c;
    if (s_compose_len >= CFG_EDITOR_MAX_CHARS) return;
    dma_stash(&byte, 1, s_compose_len, 1);
    s_compose_len++;
}

void reu_compose_puts(const char *s)
{
    while (*s) {
        reu_compose_putc(*s++);
    }
}

u16 reu_compose_len(void)
{
    return s_compose_len;
}

void reu_compose_truncate(u16 len)
{
    if (len < s_compose_len) s_compose_len = len;
}

void reu_compose_read(u16 offset, char *buf, u16 len)
{
    u16 total = s_compose_len;
    u16 avail, done;

    if (offset >= total) { return; }
    avail = total - offset;
    if (len > avail) len = avail;

    done = 0;
    while (done < len) {
        u16 chunk = len - done;
        if (chunk > 255) chunk = 255;
        dma_fetch(buf + done, 1, (u16)(offset + done), chunk);
        done += chunk;
    }
}

bbs_err_t reu_compose_commit(u8 board_id, u8 device,
                              const u16 *out_offset, u16 *out_len)
{
    char fname[16];
    u16  i;
    bbs_err_t err;

    /* Build just the filename — no drive prefix for disk_open */
    sprintf(fname, "B%u.TXT", (unsigned)board_id);

    err = cfg_send_drive_init(device, bbs_cfg.init_msgs);
    if (err != BBS_OK) return err;

    { disk_mode_t open_mode = (*out_offset == 0) ? DISK_WRITE : DISK_APPEND;
      err = disk_open(device, bbs_cfg.drive_msgs, fname, open_mode); }
    if (err != BBS_OK) return BBS_EIO;

    /* DMA fetch chunks from Bank 1, write byte-by-byte */
    for (i = 0; i < s_compose_len; i++) {
        dma_fetch(s_dma_buf, 1, i, 1);
        err = disk_putc((char)s_dma_buf[0]);
        if (err != BBS_OK) {
            disk_close();
            return BBS_EIO;
        }
    }

    disk_close();

    *out_len      = s_compose_len;
    s_compose_len = 0;
    /* out_offset is input-only (read above to pick WRITE vs APPEND) */

    return BBS_OK;
}

/* ----------------------------------------------------------------------- */
/* Bank 2: generic data tier                                                */
/* ----------------------------------------------------------------------- */

#define REU_DATA_BANK 2u   /* generic data tier occupies REU bank 2 */

bool_t reu_data_available(void)
{
    return (bbs_cfg.reu_enabled && reu_bank_count() >= 3) ? TRUE : FALSE;
}

void reu_data_put(u16 region_off, const void *src, u16 len)
{
    if (!reu_data_available()) return;
    dma_stash(src, REU_DATA_BANK, region_off, len);
}

void reu_data_get(u16 region_off, void *dst, u16 len)
{
    if (!reu_data_available()) return;
    dma_fetch(dst, REU_DATA_BANK, region_off, len);
}

/* ----------------------------------------------------------------------- */
/* Banks 3+: body cache                                                     */
/* ----------------------------------------------------------------------- */

bool_t reu_body_cached(u8 board_id, u16 msg_id)
{
    u8 i, max_slots;
    if (reu_bank_count() < 4) return FALSE;
    max_slots = reu_bank_count() - 3;
    if (max_slots > BODY_CACHE_SLOTS) max_slots = BODY_CACHE_SLOTS;
    for (i = 0; i < max_slots; i++) {
        if (s_body_cache[i].valid &&
            s_body_cache[i].board_id == board_id &&
            s_body_cache[i].msg_id   == msg_id) {
            return TRUE;
        }
    }
    return FALSE;
}

void reu_body_store(u8 board_id, u16 msg_id, const char *buf, u16 len)
{
    u8 slot, bank, max_slots;
    if (reu_bank_count() < 4) return;
    max_slots = reu_bank_count() - 3;
    if (max_slots > BODY_CACHE_SLOTS) max_slots = BODY_CACHE_SLOTS;
    slot = s_body_evict % max_slots;
    bank = 3 + slot;   /* one bank per slot, no aliasing */

    s_body_cache[slot].board_id = board_id;
    s_body_cache[slot].msg_id   = msg_id;
    s_body_cache[slot].bank     = bank;
    s_body_cache[slot].offset   = 0;
    s_body_cache[slot].len      = len;
    s_body_cache[slot].valid    = TRUE;

    dma_stash(buf, bank, 0, len);

    s_body_evict = (u8)((s_body_evict + 1) % max_slots);
}

// cppcheck-suppress constParameterPointer
bbs_err_t reu_body_fetch(u8 board_id, u16 msg_id, char *buf, u16 buf_len)
{
    u8 i, max_slots;
    if (reu_bank_count() < 4) return BBS_ENOTFOUND;
    max_slots = reu_bank_count() - 3;
    if (max_slots > BODY_CACHE_SLOTS) max_slots = BODY_CACHE_SLOTS;
    for (i = 0; i < max_slots; i++) {
        if (s_body_cache[i].valid &&
            s_body_cache[i].board_id == board_id &&
            s_body_cache[i].msg_id   == msg_id) {
            u16 len = s_body_cache[i].len;
            if (len > buf_len) len = buf_len;
            dma_fetch(buf, s_body_cache[i].bank, s_body_cache[i].offset, len);
            return BBS_OK;
        }
    }
    return BBS_ENOTFOUND;
}
bbs_err_t reu_body_fetch_at(u8 board_id, u16 msg_id,
                              char *buf, u8 len, u16 offset)
{
    u8 i, max_slots;
    if (reu_bank_count() < 4) return BBS_ENOTFOUND;
    max_slots = reu_bank_count() - 3;
    if (max_slots > BODY_CACHE_SLOTS) max_slots = BODY_CACHE_SLOTS;
    for (i = 0; i < max_slots; i++) {
        if (s_body_cache[i].valid &&
            s_body_cache[i].board_id == board_id &&
            s_body_cache[i].msg_id   == msg_id) {
            u16 avail;
            if (offset >= s_body_cache[i].len) return BBS_OK;
            avail = (u16)(s_body_cache[i].len - offset);
            if (avail > (u16)len) avail = (u16)len;
            dma_fetch(buf, s_body_cache[i].bank,
                      (u16)(s_body_cache[i].offset + offset), avail);
            return BBS_OK;
        }
    }
    return BBS_ENOTFOUND;
}
