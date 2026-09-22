/* bbs/seq_region.h - REU bank-2 region map for the SEQ record backend.
 * Pure logic: no I/O, no REU access, no C64 headers. */
#ifndef BBS_SEQ_REGION_H
#define BBS_SEQ_REGION_H

#include "bbs/types.h"

#define SEQ_REGION_NONE    0xFFu
#define SEQ_REGION_WINDOW  8u    /* shared slot: UD<n> and B<n>.IDX */
#define SEQ_NAME_MAX       17u   /* 16-char CBM name + NUL */
#define REGION_COUNT_MAX   9u

/* Map a CBM filename to a region index, or SEQ_REGION_NONE if this backend
 * does not manage it. "UD<n>" and "B<n>.IDX" both map to SEQ_REGION_WINDOW. */
u8 seq_region_for_name(const char *name);

/* Byte offset of region `idx` within REU bank 2. Offsets below 0x4000 are
 * reserved for the pre-existing reu_data_* tier. */
u16 seq_region_offset(u8 idx);

/* Byte capacity of region `idx`. */
u16 seq_region_capacity(u8 idx);

/* Build "<name>.NEW" into `buf` (>= SEQ_NAME_MAX bytes).
 * ".NEW" rather than ".TMP" because src/data/messages.c already writes
 * B<n>.TMP for message-body compaction.
 * Returns FALSE if the result would exceed the 16-character CBM lookup limit. */
bool_t seq_tmp_name(char *buf, const char *name);

typedef enum {
    SEQ_RECOVER_NONE     = 0,  /* only <name>, or neither: nothing to do */
    SEQ_RECOVER_DROP_TMP = 1,  /* both: original intact, discard the partial */
    SEQ_RECOVER_PROMOTE  = 2   /* only <name>.NEW: complete, rename it in */
} seq_recover_t;

seq_recover_t seq_recover_action(bool_t name_exists, bool_t tmp_exists);

#endif /* BBS_SEQ_REGION_H */
