/**
 * CONFIGURE — Config Admin Module
 *
 * Edit BBS configuration. Writes changes to the "CONFIG" file via cfg_save().
 * [1] SETTINGS — identity, access, toggles
 * [2] DEVICES  — CBM device/drive for system, msgs, files, doors, gfiles
 * [3] BAUD RATE — modem speed select
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "bbs/cfg.h"
#include "bbs/config.h"
#include "bbs/devspec.h"
#include "bbs/err.h"
#include "bbs/sstatus.h"
#include "bbs/access.h"
#include "ui/ui.h"

/* Edit buffers */
static char s_bbs_name[32];
static char s_bbs_city[32];
static char s_sysop[21];
static char s_sysop_status[21];
static char s_new_lvl[2];
static char s_allow[4];   /* "ON" / "OFF" */
static char s_promptcur[4];   /* "ON" / "OFF" */
static char s_lowart[4];   /* "ON" / "OFF" — PETSCII lowercase/mixed-case art */
static char s_idle[4];
static char s_dev_sys[CFG_VALUE_MAX];
static char s_dev_msgs[CFG_VALUE_MAX];
static char s_dev_files[CFG_VALUE_MAX];
static char s_dev_doors[CFG_VALUE_MAX];
static char s_dev_gfiles[DEVSPEC_BUF_MAX];
static char s_baud[6];

static const char *s_baud_opts[] = {
  "300","1200","2400","9600","19200","38400"
};
#define BAUD_OPTS_COUNT 6

static char s_modem[6];
static const char *s_modem_opts[] = { "AUTO", "VICE", "U64" };
#define MODEM_OPTS_COUNT 3

/* ---- ACCESS LEVELS screen state ---- */
static access_level_t s_levels[ACCESS_LEVEL_COUNT];
static char s_lname[13];
static char s_lcalls[4];   /* "0".."255"  */
static char s_lmins[5];    /* "0".."1440" */
static char s_lflag[8][4]; /* "ON"/"OFF" per flag */

static const char *const s_flag_labels[8] = {
  "POSTANON", "PAGESYSOP", "SENDMAIL", "JOINPOLLS",
  "UPLOAD",   "NOTIMELIM", "NOCALLLIM", "SYSOP"
};
static const u8 s_flag_bits[8] = {
  ACCESS_F_POST_ANON, ACCESS_F_PAGE_SYSOP, ACCESS_F_SEND_MAIL, ACCESS_F_JOIN_POLLS,
  ACCESS_F_UPLOAD,    ACCESS_F_NO_TIME_LIMIT, ACCESS_F_NO_CALL_LIMIT, ACCESS_F_SYSOP
};
/* One-letter code per flag (same order as s_flag_bits) for the list FLAGS column. */
static const char s_flag_chars[8] = { 'A', 'P', 'S', 'J', 'U', 'T', 'C', 'Y' };

/* ------------------------------------------------------------------ */
/* Per-field validators                                                 */
/* ------------------------------------------------------------------ */

static const char *validate_new_lvl(const char *val, void *ctx)
{
  (void)ctx;
  if (val[0] < '0' || val[0] > '4' || val[1] != '\0')
    return "LEVEL MUST BE 0-4.";
  return 0;
}

static const char *validate_idle(const char *val, void *ctx)
{
  int n;
  const char *p = val;
  (void)ctx;
  if (val[0] == '\0') return "ENTER 0-99 (0=OFF).";
  for (; *p; p++)
    if (*p < '0' || *p > '9') return "DIGITS ONLY.";
  n = atoi(val);
  if (n < 0 || n > 99) return "ENTER 0-99 (0=OFF).";
  return 0;
}

static const char *validate_device(const char *val, void *ctx)
{
  u8 device;
  u8 drive;
  char init[CFG_INIT_MAX];
  (void)ctx;
  if (cfg_parse_device_spec(val, &device, &drive, init, sizeof(init)) == FALSE) {
    return "USE N OR N;D:;CMD.";
  }
  if (device < 8 || device > 31) return "DEV MUST BE 8-31.";
  return 0;
}

static const char *validate_lvl_name(const char *val, void *ctx)
{
  const char *p = val;
  (void)ctx;
  if (val[0] == '\0') return "NAME REQUIRED.";
  for (; *p; p++) if (*p == ',') return "NO COMMAS ALLOWED.";
  return 0;
}

static const char *validate_lvl_calls(const char *val, void *ctx)
{
  const char *p = val;
  int n;
  (void)ctx;
  if (val[0] == '\0') return "ENTER 0-255.";
  for (; *p; p++) if (*p < '0' || *p > '9') return "DIGITS ONLY.";
  n = atoi(val);
  if (n < 0 || n > 255) return "ENTER 0-255.";
  return 0;
}

static const char *validate_lvl_mins(const char *val, void *ctx)
{
  const char *p = val;
  int n;
  (void)ctx;
  if (val[0] == '\0') return "ENTER 0-1440.";
  for (; *p; p++) if (*p < '0' || *p > '9') return "DIGITS ONLY.";
  n = atoi(val);
  if (n < 0 || n > 1440) return "ENTER 0-1440.";
  return 0;
}

/* ------------------------------------------------------------------ */

#define FIELD_INIT(f, lbl, buf, mlen) \
  (f).label        = (lbl);          \
  (f).value        = (buf);          \
  (f).max_len      = (mlen);         \
  (f).current_len  = (int)strlen(buf); \
  (f).dirty        = 0;              \
  (f).validate     = NULL;           \
  (f).validate_ctx = NULL;           \
  (f).is_toggle    = 0;              \
  (f).case_mode    = UI_CASE_UPPER

static void do_settings(u8 device)
{
  ui_edit_field_t f[8];
  bbs_err_t err;
  int sel;

  strncpy(s_bbs_name, bbs_cfg.bbs_name,   31); s_bbs_name[31] = '\0';
  strncpy(s_bbs_city, bbs_cfg.bbs_city,   31); s_bbs_city[31] = '\0';
  strncpy(s_sysop,    bbs_cfg.sysop_name, 19); s_sysop[19]    = '\0';
  sstatus_load(s_sysop_status, (u8)sizeof(s_sysop_status));   /* separate STATUS file */
  s_new_lvl[0] = (char)('0' + (bbs_cfg.new_user_level > 4 ? 1 : bbs_cfg.new_user_level));
  s_new_lvl[1] = '\0';
  strcpy(s_allow, bbs_cfg.allow_new_users ? "ON" : "OFF");
  strcpy(s_promptcur, bbs_cfg.prompt_cursor ? "ON" : "OFF");
  strcpy(s_lowart, bbs_cfg.petscii_lower_art ? "ON" : "OFF");

  FIELD_INIT(f[0], "BBSNAME",  s_bbs_name, 31);
  FIELD_INIT(f[1], "BBSCITY",  s_bbs_city, 31);
  FIELD_INIT(f[2], "SYSOPNAME",s_sysop,    19);
  FIELD_INIT(f[3], "NEWUSRLVL",s_new_lvl,   1); f[3].validate = validate_new_lvl;
  FIELD_INIT(f[4], "ALLOWNEW", s_allow,      3); f[4].is_toggle = 1;
  FIELD_INIT(f[5], "SYSOPSTAT", s_sysop_status, 20);
  FIELD_INIT(f[6], "PROMPTCUR", s_promptcur, 3); f[6].is_toggle = 1;
  FIELD_INIT(f[7], "LOWERART", s_lowart, 3); f[7].is_toggle = 1;

  f[0].case_mode = UI_CASE_MIXED;  /* BBSNAME */
  f[1].case_mode = UI_CASE_MIXED;  /* BBSCITY */
  f[2].case_mode = UI_CASE_MIXED;  /* SYSOPNAME */
  f[5].case_mode = UI_CASE_MIXED;  /* SYSOPSTAT */

  sel = ui_edit_form("CONFIG: SETTINGS", f, 8);
  if (sel == -1) return;

  strncpy(bbs_cfg.bbs_name,   s_bbs_name, sizeof(bbs_cfg.bbs_name)   - 1);
  strncpy(bbs_cfg.bbs_city,   s_bbs_city, sizeof(bbs_cfg.bbs_city)   - 1);
  strncpy(bbs_cfg.sysop_name, s_sysop,    sizeof(bbs_cfg.sysop_name) - 1);
  bbs_cfg.bbs_name[sizeof(bbs_cfg.bbs_name)-1]     = '\0';
  bbs_cfg.bbs_city[sizeof(bbs_cfg.bbs_city)-1]     = '\0';
  bbs_cfg.sysop_name[sizeof(bbs_cfg.sysop_name)-1] = '\0';
  bbs_cfg.new_user_level  = (u8)(s_new_lvl[0] - '0');
  bbs_cfg.allow_new_users = (strcmp(s_allow, "ON") == 0) ? TRUE : FALSE;
  bbs_cfg.prompt_cursor   = (strcmp(s_promptcur, "ON") == 0) ? TRUE : FALSE;
  bbs_cfg.petscii_lower_art = (strcmp(s_lowart, "ON") == 0) ? TRUE : FALSE;

  err = cfg_save();
  if (err != BBS_OK) { ui_error("SAVE FAILED."); return; }
  sstatus_save(s_sysop_status);   /* status lives in its own STATUS file */
  ui_status("SAVED", (const char *[]){"CONFIG WRITTEN TO DISK."}, 1);
}

static void do_devices(void)
{
  ui_edit_field_t f[5];
  bbs_err_t err;
  int sel;

  cfg_format_device_spec(s_dev_sys,   bbs_cfg.device_system, bbs_cfg.drive_system, bbs_cfg.init_system);
  cfg_format_device_spec(s_dev_msgs,  bbs_cfg.device_msgs,   bbs_cfg.drive_msgs,   bbs_cfg.init_msgs);
  cfg_format_device_spec(s_dev_files, bbs_cfg.device_files,  bbs_cfg.drive_files,  bbs_cfg.init_files);
  cfg_format_device_spec(s_dev_doors, bbs_cfg.device_doors,  bbs_cfg.drive_doors,  bbs_cfg.init_doors);
  cfg_format_device_spec(s_dev_gfiles,bbs_cfg.device_gfiles, bbs_cfg.drive_gfiles, bbs_cfg.init_gfiles);

  FIELD_INIT(f[0], "SYSDEV",   s_dev_sys,   CFG_VALUE_MAX - 1); f[0].validate = validate_device;
  FIELD_INIT(f[1], "MSGDEV",   s_dev_msgs,  CFG_VALUE_MAX - 1); f[1].validate = validate_device;
  FIELD_INIT(f[2], "FILEDEV",  s_dev_files, CFG_VALUE_MAX - 1); f[2].validate = validate_device;
  FIELD_INIT(f[3], "DOORDEV",  s_dev_doors, CFG_VALUE_MAX - 1); f[3].validate = validate_device;
  FIELD_INIT(f[4], "GFILEDEV", s_dev_gfiles,DEVSPEC_BUF_MAX - 1); f[4].validate = validate_device;

  sel = ui_edit_form("CONFIG: DEVICES", f, 5);
  if (sel == -1) return;

  cfg_parse_device_spec(s_dev_sys,   &bbs_cfg.device_system, &bbs_cfg.drive_system,
                        bbs_cfg.init_system, (u8)sizeof(bbs_cfg.init_system));
  cfg_parse_device_spec(s_dev_msgs,  &bbs_cfg.device_msgs,   &bbs_cfg.drive_msgs,
                        bbs_cfg.init_msgs,   (u8)sizeof(bbs_cfg.init_msgs));
  cfg_parse_device_spec(s_dev_files, &bbs_cfg.device_files,  &bbs_cfg.drive_files,
                        bbs_cfg.init_files,  (u8)sizeof(bbs_cfg.init_files));
  cfg_parse_device_spec(s_dev_doors, &bbs_cfg.device_doors,  &bbs_cfg.drive_doors,
                        bbs_cfg.init_doors,  (u8)sizeof(bbs_cfg.init_doors));
  cfg_parse_device_spec(s_dev_gfiles,&bbs_cfg.device_gfiles, &bbs_cfg.drive_gfiles,
                        bbs_cfg.init_gfiles, (u8)sizeof(bbs_cfg.init_gfiles));

  err = cfg_save();
  if (err != BBS_OK) { ui_error("SAVE FAILED."); return; }
  ui_status("SAVED", (const char *[]){"DEVICE CONFIG WRITTEN TO DISK."}, 1);
}

static void do_baud(u8 device)
{
  bbs_err_t err;

  sprintf(s_baud, "%u", (unsigned)bbs_cfg.baud_rate);
  ui_screen_header("CONFIG: BAUD RATE");
  ui_select_field("BAUD RATE", s_baud, 5, s_baud_opts, BAUD_OPTS_COUNT);

  bbs_cfg.baud_rate = (u16)atoi(s_baud);
  err = cfg_save();
  if (err != BBS_OK) ui_error("SAVE FAILED.");
  else ui_status("SAVED", (const char *[]){"BAUD RATE UPDATED."}, 1);
}

/* Carrier-detection mode: AUTO (U64 probe), VICE (tcpser AT-strings), U64 (DSR). */
static void do_modem_type(u8 device)
{
  bbs_err_t err;

  strcpy(s_modem, bbs_cfg.modem_type == MODEM_VICE ? "VICE" :
                  bbs_cfg.modem_type == MODEM_U64  ? "U64"  : "AUTO");
  ui_screen_header("CONFIG: MODEM TYPE");
  ui_select_field("MODEM TYPE", s_modem, 5, s_modem_opts, MODEM_OPTS_COUNT);

  if      (strcmp(s_modem, "VICE") == 0) bbs_cfg.modem_type = MODEM_VICE;
  else if (strcmp(s_modem, "U64")  == 0) bbs_cfg.modem_type = MODEM_U64;
  else                                   bbs_cfg.modem_type = MODEM_AUTO;
  err = cfg_save();
  if (err != BBS_OK) ui_error("SAVE FAILED.");
  else ui_status("SAVED", (const char *[]){"MODEM TYPE UPDATED."}, 1);
}

static void do_idle(u8 device)
{
  ui_edit_field_t f;
  bbs_err_t err;

  sprintf(s_idle, "%u", (unsigned)bbs_cfg.idle_timeout_mins);

  ui_screen_header("CONFIG: IDLE TIMEOUT");
  printf("MINUTES OF KEYBOARD IDLE BEFORE\n");
  printf("A CALLER IS DROPPED. 0 = OFF.\n\n");

  FIELD_INIT(f, "IDLEMINS", s_idle, 2);
  f.validate = validate_idle;
  ui_edit_field_single(&f);

  bbs_cfg.idle_timeout_mins = (u8)atoi(s_idle);
  err = cfg_save();
  if (err != BBS_OK) ui_error("SAVE FAILED.");
  else ui_status("SAVED", (const char *[]){"IDLE TIMEOUT UPDATED."}, 1);
}

static void do_access_edit(u8 device, u8 lvl)
{
  ui_edit_field_t f[11];
  access_level_t *r = &s_levels[lvl];
  char title[24];
  int sel;
  u8 i;

  strncpy(s_lname, r->name, sizeof(s_lname) - 1); s_lname[sizeof(s_lname) - 1] = '\0';
  sprintf(s_lcalls, "%u", (unsigned)r->calls_per_day);
  sprintf(s_lmins,  "%u", (unsigned)r->mins_per_day);
  for (i = 0; i < 8; i++)
    strcpy(s_lflag[i], (r->flags & s_flag_bits[i]) ? "ON" : "OFF");

  FIELD_INIT(f[0], "NAME",      s_lname,  12); f[0].case_mode = UI_CASE_MIXED;
                                               f[0].validate  = validate_lvl_name;
  FIELD_INIT(f[1], "CALLS/DAY", s_lcalls,  3); f[1].validate  = validate_lvl_calls;
  FIELD_INIT(f[2], "MINS/DAY",  s_lmins,   4); f[2].validate  = validate_lvl_mins;
  for (i = 0; i < 8; i++) {
    FIELD_INIT(f[3 + i], s_flag_labels[i], s_lflag[i], 3);
    f[3 + i].is_toggle = 1;
  }

  sprintf(title, "LVL %u: %s", (unsigned)lvl, r->name);
  sel = ui_edit_form(title, f, 11);
  if (sel == -1) return;

  strncpy(r->name, s_lname, sizeof(r->name) - 1); r->name[sizeof(r->name) - 1] = '\0';
  r->calls_per_day = (u8)atoi(s_lcalls);
  r->mins_per_day  = (u16)atoi(s_lmins);
  r->flags = 0;
  for (i = 0; i < 8; i++)
    if (strcmp(s_lflag[i], "ON") == 0) r->flags |= s_flag_bits[i];

  if (access_levels_save(s_levels, device) != BBS_OK) { ui_error("SAVE FAILED."); return; }
  ui_status("SAVED", (const char *[]){"ACCESS LEVEL WRITTEN TO DISK."}, 1);
}

static void admin_access_menu(u8 device)
{
  u8 i;
  u8 j;
  char fl[9];

  /* Load once; defaults are returned if the file does not exist yet, and the
   * first SAVE will create it (no re-INIT needed on older disks). */
  /* Load once; defaults are returned if the file does not exist yet, and the
   * first SAVE will create it (no re-INIT needed on older disks). */
  (void)access_levels_load(s_levels, device);

  for (;;) {
    char ch;
    ui_screen_header("ACCESS LEVELS");
    printf("LVL NAME         CALLS  MINS  FLAGS\n");
    printf("--- ------------ -----  ----  --------\n");
    for (i = 0; i < ACCESS_LEVEL_COUNT; i++) {
      for (j = 0; j < 8; j++)
        fl[j] = (s_levels[i].flags & s_flag_bits[j]) ? s_flag_chars[j] : '-';
      fl[8] = '\0';
      printf("%2u  %-12s %5u  %4u  %s\n",
             (unsigned)i, s_levels[i].name,
             (unsigned)s_levels[i].calls_per_day,
             (unsigned)s_levels[i].mins_per_day, fl);
    }
    printf("\n");
    printf("A=ANON P=PAGE S=MAIL J=POLL\n");
    printf("U=UPLD T=NOTM C=NOCL Y=SYSOP\n");
    printf("\n");
    printf("0-5 EDIT LEVEL  ");
    ui_hotkey_label('Q', "BACK");
    printf("\n");
    ch = ui_menu_input("", "012345Q");
    if (ch == 'Q' || ch == 27) return;
    do_access_edit(device, (u8)(ch - '0'));
  }
}

void admin_config_menu(u8 device)
{
  static const ui_menu_item_t items[] = {
    { '1', "SETTINGS" },
    { '2', "DEVICES" },
    { '3', "BAUD RATE" },
    { '4', "IDLE TIMEOUT" },
    { '5', "MODEM TYPE" },
    { '6', "ACCESS LEVELS" },
    { 'Q', "BACK" },
  };
  for (;;) {
    char ch;
    ui_menu_display("CONFIG", items, 7);
    ch = ui_menu_input("CHOICE:", "123456Q");
    if      (ch == '1') do_settings(device);
    else if (ch == '2') do_devices();
    else if (ch == '3') do_baud(device);
    else if (ch == '4') do_idle(device);
    else if (ch == '5') do_modem_type(device);
    else if (ch == '6') admin_access_menu(device);
    else return;
  }
}
