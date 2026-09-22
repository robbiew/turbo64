/* Host unit tests for the SEQ backend's REU region map and recovery table. */
#include "host.h"
#include "bbs/seq_region.h"

int main(void) {
    char buf[SEQ_NAME_MAX];

    EXPECT_EQ("idx.users",    seq_region_for_name("USR LOG"),  0);
    EXPECT_EQ("idx.usrptr",   seq_region_for_name("USR.PTR"),  1);
    EXPECT_EQ("idx.usrday",   seq_region_for_name("USR.DAY"),  2);
    EXPECT_EQ("idx.profiles", seq_region_for_name("USR PROF"), 3);
    EXPECT_EQ("idx.boards",   seq_region_for_name("BOARDS"),   4);
    EXPECT_EQ("idx.uds",      seq_region_for_name("UDS"),      5);
    EXPECT_EQ("idx.votes",    seq_region_for_name("VOTE1"),    6);
    EXPECT_EQ("idx.doors",    seq_region_for_name("DOORS"),    7);

    /* every per-area and per-board set shares the single WINDOW slot */
    EXPECT_EQ("idx.ud1",   seq_region_for_name("UD1"),    SEQ_REGION_WINDOW);
    EXPECT_EQ("idx.ud8",   seq_region_for_name("UD8"),    SEQ_REGION_WINDOW);
    EXPECT_EQ("idx.b1",    seq_region_for_name("B1.IDX"), SEQ_REGION_WINDOW);
    EXPECT_EQ("idx.b20",   seq_region_for_name("B20.IDX"), SEQ_REGION_WINDOW);

    /* UDS must not be mistaken for a UD<n>; B<n>.TXT is not an index file */
    EXPECT_EQ("idx.not.uds",  seq_region_for_name("UDS"),     5);
    EXPECT_EQ("idx.not.btxt", seq_region_for_name("B3.TXT"),  SEQ_REGION_NONE);
    EXPECT_EQ("idx.not.btmp", seq_region_for_name("B3.TMP"),  SEQ_REGION_NONE);
    EXPECT_EQ("idx.unknown",  seq_region_for_name("ACCESS"),  SEQ_REGION_NONE);
    EXPECT_EQ("idx.null",     seq_region_for_name(0),         SEQ_REGION_NONE);
    EXPECT_EQ("idx.empty",    seq_region_for_name(""),        SEQ_REGION_NONE);

    EXPECT_EQ("off.users",    seq_region_offset(0), 0x4000);
    EXPECT_EQ("off.usrptr",   seq_region_offset(1), 0x4BB8);
    EXPECT_EQ("off.usrday",   seq_region_offset(2), 0x5B58);
    EXPECT_EQ("off.profiles", seq_region_offset(3), 0x5E78);
    EXPECT_EQ("off.boards",   seq_region_offset(4), 0x8010);
    EXPECT_EQ("off.uds",      seq_region_offset(5), 0x8380);
    EXPECT_EQ("off.votes",    seq_region_offset(6), 0x84C0);
    EXPECT_EQ("off.doors",    seq_region_offset(7), 0x87E0);
    EXPECT_EQ("off.window",   seq_region_offset(SEQ_REGION_WINDOW), 0x8A60);

    EXPECT_EQ("cap.users",    seq_region_capacity(0), 3000);
    EXPECT_EQ("cap.usrptr",   seq_region_capacity(1), 4000);
    EXPECT_EQ("cap.usrday",   seq_region_capacity(2), 800);
    EXPECT_EQ("cap.profiles", seq_region_capacity(3), 8600);
    EXPECT_EQ("cap.boards",   seq_region_capacity(4), 880);
    EXPECT_EQ("cap.uds",      seq_region_capacity(5), 320);
    EXPECT_EQ("cap.votes",    seq_region_capacity(6), 800);
    EXPECT_EQ("cap.doors",    seq_region_capacity(7), 640);
    /* WINDOW is sized by B<n>.IDX (200 x 63), the larger of its two tenants */
    EXPECT_EQ("cap.window",   seq_region_capacity(SEQ_REGION_WINDOW), 12600);

    /* every region ends where the next begins (checks all 8 links, index 0
     * through WINDOW), and the last fits in bank 2 */
    {
        u8 i;
        for (i = 0; i < REGION_COUNT_MAX - 1; i++) {
            EXPECT_EQ("layout.contiguous",
                      seq_region_offset((u8)(i + 1)) ==
                          seq_region_offset(i) + seq_region_capacity(i), 1);
        }
    }
    EXPECT_EQ("layout.fits",
              (long)seq_region_offset(SEQ_REGION_WINDOW)
              + seq_region_capacity(SEQ_REGION_WINDOW) <= 0x10000L, 1);

    EXPECT_EQ("tmp.ok", seq_tmp_name(buf, "USR LOG"), 1);
    EXPECT_STR("tmp.name", buf, "USR LOG.NEW");
    EXPECT_EQ("tmp.b20.ok", seq_tmp_name(buf, "B20.IDX"), 1);
    EXPECT_STR("tmp.b20", buf, "B20.IDX.NEW");
    /* 12 chars + ".NEW" = 16, exactly at the CBM lookup limit — the ACCEPT
     * boundary paired with tmp.toolong's REJECT boundary one char over */
    EXPECT_EQ("tmp.12ok", seq_tmp_name(buf, "ABCDEFGHIJKL"), 1);
    EXPECT_STR("tmp.12name", buf, "ABCDEFGHIJKL.NEW");
    /* 13 chars + ".NEW" = 17, past the 16-char CBM lookup limit */
    EXPECT_EQ("tmp.toolong", seq_tmp_name(buf, "ABCDEFGHIJKLM"), 0);
    EXPECT_EQ("tmp.null", seq_tmp_name(buf, 0), 0);

    EXPECT_EQ("rec.none",    seq_recover_action(TRUE,  FALSE), SEQ_RECOVER_NONE);
    EXPECT_EQ("rec.neither", seq_recover_action(FALSE, FALSE), SEQ_RECOVER_NONE);
    EXPECT_EQ("rec.both",    seq_recover_action(TRUE,  TRUE),  SEQ_RECOVER_DROP_TMP);
    EXPECT_EQ("rec.promote", seq_recover_action(FALSE, TRUE),  SEQ_RECOVER_PROMOTE);

    EXPECT_EQ("count.max", REGION_COUNT_MAX, 9);

    return test_summary("seq_region");
}
