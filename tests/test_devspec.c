/* Host unit tests for the CONFIG device-spec grammar. */
#include "host.h"
#include "bbs/devspec.h"

int main(void) {
    u8 dev, drv;
    char loc[24];
    char buf[DEVSPEC_BUF_MAX];

#ifndef T64_STORE_SEQ
    dev = 0; drv = 9; loc[0] = 'x';
    EXPECT_EQ("bare.ok", devspec_parse("8", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("bare.dev", dev, 8);
    EXPECT_EQ("bare.drv", drv, 0);
    EXPECT_STR("bare.loc", loc, "");

    EXPECT_EQ("part.ok", devspec_parse("8;2:", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("part.dev", dev, 8);
    EXPECT_EQ("part.drv", drv, 2);
    EXPECT_STR("part.loc", loc, "");

    EXPECT_EQ("init.ok", devspec_parse("8;2:;I0", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("init.dev", dev, 8);
    EXPECT_EQ("init.drv", drv, 2);
    EXPECT_STR("init.loc", loc, "I0");

    EXPECT_EQ("ws.ok", devspec_parse("  10;1:;I0  ", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("ws.dev", dev, 10);
    EXPECT_STR("ws.loc", loc, "I0");

    EXPECT_EQ("empty", devspec_parse("", &dev, &drv, loc, sizeof(loc)), 0);
    EXPECT_EQ("null.value", devspec_parse(0, &dev, &drv, loc, sizeof(loc)), 0);
    EXPECT_EQ("null.loc", devspec_parse("8", &dev, &drv, 0, sizeof(loc)), 0);
    EXPECT_EQ("zero.len", devspec_parse("8", &dev, &drv, loc, 0), 0);

    /* loc longer than the buffer is truncated, never overflowed */
    EXPECT_EQ("trunc.ok",
              devspec_parse("8;0:;ABCDEFGHIJKLMNOPQRSTUVWXYZ", &dev, &drv, loc, 8), 1);
    EXPECT_STR("trunc.loc", loc, "ABCDEFG");

    devspec_format(buf, 8, 0, "");
    EXPECT_STR("fmt.bare", buf, "8");
    devspec_format(buf, 8, 2, "");
    EXPECT_STR("fmt.part", buf, "8;2:");
    devspec_format(buf, 8, 2, "I0");
    EXPECT_STR("fmt.init", buf, "8;2:;I0");

    /* round trip */
    devspec_format(buf, 10, 3, "I0");
    EXPECT_EQ("rt.ok", devspec_parse(buf, &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("rt.dev", dev, 10);
    EXPECT_EQ("rt.drv", drv, 3);
    EXPECT_STR("rt.loc", loc, "I0");
#else
    dev = 0; drv = 7; loc[0] = 'x';
    EXPECT_EQ("siec.ok",
              devspec_parse("11;/USB1/BBS/SYSTEM", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("siec.dev", dev, 11);
    EXPECT_EQ("siec.drv.untouched", drv, 7);
    EXPECT_STR("siec.loc", loc, "/USB1/BBS/SYSTEM");

    EXPECT_EQ("siec.bare.ok", devspec_parse("11", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("siec.bare.dev", dev, 11);
    EXPECT_STR("siec.bare.loc", loc, "");

    EXPECT_EQ("siec.ws.ok",
              devspec_parse("  11;/SD/T64  ", &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_STR("siec.ws.loc", loc, "/SD/T64");

    /* 23 chars is the maximum a char[24] field holds */
    EXPECT_EQ("siec.max.ok",
              devspec_parse("11;/USB1/AAAAAAAAAAAAAAAAA", &dev, &drv, loc, 24), 1);
    EXPECT_STR("siec.max.loc", loc, "/USB1/AAAAAAAAAAAAAAAAA");

    EXPECT_EQ("siec.trunc.ok",
              devspec_parse("11;/USB1/AAAAAAAAAAAAAAAAAAAAAAAAAA",
                            &dev, &drv, loc, 24), 1);
    EXPECT_EQ("siec.trunc.len", strlen(loc), 23);

    EXPECT_EQ("siec.empty", devspec_parse("", &dev, &drv, loc, sizeof(loc)), 0);
    EXPECT_EQ("siec.null", devspec_parse(0, &dev, &drv, loc, sizeof(loc)), 0);

    devspec_format(buf, 11, 0, "/USB1/BBS/SYSTEM");
    EXPECT_STR("siec.fmt", buf, "11;/USB1/BBS/SYSTEM");
    devspec_format(buf, 11, 0, "");
    EXPECT_STR("siec.fmt.bare", buf, "11");

    devspec_format(buf, 11, 0, "/SD/T64/MSGS");
    EXPECT_EQ("siec.rt.ok", devspec_parse(buf, &dev, &drv, loc, sizeof(loc)), 1);
    EXPECT_EQ("siec.rt.dev", dev, 11);
    EXPECT_STR("siec.rt.loc", loc, "/SD/T64/MSGS");
#endif

    return test_summary("devspec");
}
