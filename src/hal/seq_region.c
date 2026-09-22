/* REU bank-2 region map for the SEQ record backend. */
#include "bbs/seq_region.h"
#include <string.h>

static const char * const s_names[8] = {
    "USR LOG", "USR.PTR", "USR.DAY", "USR PROF",
    "BOARDS",  "UDS",     "VOTE1",   "DOORS"
};

static const u16 s_offset[REGION_COUNT_MAX] = {
    0x4000, 0x4BB8, 0x5B58, 0x5E78,
    0x8010, 0x8380, 0x84C0, 0x87E0,
    0x8A60
};

static const u16 s_capacity[REGION_COUNT_MAX] = {
    3000, 4000, 800, 8600,
    880,  320,  800, 640,
    12600
};

static bool_t all_digits(const char *p, const char *end)
{
    if (p >= end) return FALSE;
    while (p < end) {
        if (*p < '0' || *p > '9') return FALSE;
        p++;
    }
    return TRUE;
}

u8 seq_region_for_name(const char *name)
{
    u8 i;
    u8 len;

    if (!name || name[0] == '\0') return SEQ_REGION_NONE;

    for (i = 0; i < 8; i++) {
        if (strcmp(name, s_names[i]) == 0) return i;
    }

    len = (u8)strlen(name);

    /* "UD<n>" — guard against "UDS", which is a fixed set matched above */
    if (len > 2 && name[0] == 'U' && name[1] == 'D' &&
        all_digits(name + 2, name + len)) {
        return SEQ_REGION_WINDOW;
    }

    /* "B<n>.IDX" — B<n>.TXT and B<n>.TMP are not managed here */
    if (len > 5 && name[0] == 'B' &&
        strcmp(name + len - 4, ".IDX") == 0 &&
        all_digits(name + 1, name + len - 4)) {
        return SEQ_REGION_WINDOW;
    }

    return SEQ_REGION_NONE;
}

u16 seq_region_offset(u8 idx)
{
    return (idx < REGION_COUNT_MAX) ? s_offset[idx] : 0;
}

u16 seq_region_capacity(u8 idx)
{
    return (idx < REGION_COUNT_MAX) ? s_capacity[idx] : 0;
}

bool_t seq_tmp_name(char *buf, const char *name)
{
    u8 len;

    if (!buf || !name) return FALSE;
    len = (u8)strlen(name);
    if (len == 0 || len + 4 > 16) return FALSE;

    strcpy(buf, name);
    strcpy(buf + len, ".NEW");
    return TRUE;
}

seq_recover_t seq_recover_action(bool_t name_exists, bool_t tmp_exists)
{
    if (!tmp_exists) return SEQ_RECOVER_NONE;
    return name_exists ? SEQ_RECOVER_DROP_TMP : SEQ_RECOVER_PROMOTE;
}
