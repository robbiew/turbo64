/**
 * TURBO/64 BBS — Main Entry Point and Kernel Loop
 *
 * Boot sequence:
 *  1. Load configuration (config file)
 *  2. Initialize modem (ACIA)
 *  3. Display SysOp local status
 *  4. Main loop: wait for carrier → new session → logoff → repeat
 */

#include <stdio.h>
#include <string.h>

/* Oscar64 headers */
#include <c64/vic.h>
#include <c64/kernalio.h>   /* krnio_setnam/krnio_load — main() loads OVL_BOOT before boot_sequence() runs from it */

/* BBS headers */
#include "bbs/version.h"
#include "bbs/config.h"
#include "bbs/types.h"
#include "bbs/err.h"
#include "bbs/cfg.h"
#include "bbs/net.h"
#include "bbs/session.h"
#include "bbs/menu.h"
#include "bbs/sysop.h"
#include "bbs/callers.h"
#include "bbs/rel.h"
#include "bbs/records.h"
#include "bbs/users.h"
#include "bbs/hal/disk.h"
#include "bbs/hal/term.h"
#include "bbs/hal/clock.h"
#include "bbs/hal/reu.h"

/* Core region: code/data/BSS/heap in $0880-$9700 (~36.4 KB).
 * Overlay zone at $9700-$BFFF holds the MSGS/WFC overlays (10,496 bytes).
 * BASIC ROM ($A000-$BFFF) is banked out in main() so those addresses are RAM.
 * Stack lives in always-RAM at $C000-$D000. $C000-$C3FF (bottom 1KB) is also
 * used as VIC screen RAM during 80-col spy sessions; stack grows down from $CFFF
 * and session call depth stays well under 3KB so there is no overlap at runtime.
 * Boundary: $9D80 → $9B00 (reading loop) → $9800 (msg scan) → $9700 (subj[16]). */
#pragma region( main, 0x0880, 0x9700, , , {code, data, bss, heap} )
#pragma region( cstk, 0xC000, 0xD000, , , {stack} )
#pragma heapsize(0)

/* MSGS overlay: bulletin + messages + editor loaded on demand from disk.
 * $9700-$C000 = 10,496 bytes.
 * NEARLY FULL as of v0.1.0 (<100 bytes) — after touching bulletin/messages/
 * editor/usrptr, check the `regions` table in build/c64/BOOT-*.map.
 * Bank ID 1 → file "OVL_MSGS" on disk (oscar64: pragma name uppercased). */
#pragma overlay( ovl_msgs, 1 )
#pragma section( msgs_code, 0 )
#pragma section( msgs_data, 0 )
#pragma section( msgs_bss,  0, , , bss )
#pragma region( msgs, 0x9700, 0xC000, , 1, {msgs_code, msgs_data, msgs_bss} )

/* WFC overlay: all WFC draw functions, wfc_display/update/init, RTC helpers.
 * Bank 2 — same address zone as MSGS; the two are mutually exclusive at runtime.
 * Loaded at boot and reloaded after any session where OVL_MSGS displaced it. */
#pragma overlay( ovl_wfc, 2 )
#pragma section( wfc_code, 0 )
#pragma section( wfc_data, 0 )
#pragma section( wfc_bss,  0, , , bss )
#pragma region( wfc, 0x9700, 0xC000, , 2, {wfc_code, wfc_data, wfc_bss} )

/* BOOT overlay: boot_sequence() itself, plus config-load code (cfg_init +
 * its parse helpers), run once at boot and are then dead weight in the
 * resident region.  Bank 3 — same address zone as MSGS/WFC; boot strictly
 * precedes any session, so wfc_init/the first session freely overwrites it.
 * main() loads this overlay directly (see main()'s doc comment) BEFORE
 * calling boot_sequence() — boot_sequence can no longer trigger its own
 * load the way cfg_init used to, since cfg_init() is called from inside it. */
#pragma overlay( ovl_boot, 3 )
#pragma section( boot_code, 0 )
#pragma section( boot_data, 0 )
#pragma section( boot_bss,  0, , , bss )
#pragma region( boot, 0x9700, 0xC000, , 3, {boot_code, boot_data, boot_bss} )

/* DOORS overlay: door menu UI (action_doors_menu).  Bank 4 — same address zone.
 * door_run + wrappers + enter_door stay RESIDENT because door_run's krnio_load
 * overwrites $9700-$BFFF (this overlay's code), so the caller must not be in
 * the overlay while the door is running.  door_run reloads OVL_DOORS after
 * enter_door() returns, before returning to action_doors_menu's call site. */
#pragma overlay( ovl_doors, 4 )
#pragma section( doors_code, 0 )
#pragma section( doors_data, 0 )
#pragma section( doors_bss,  0, , , bss )
#pragma region( doors, 0x9700, 0xC000, , 4, {doors_code, doors_data, doors_bss} )

/* FILES overlay: file list, Punter protocol send/receive.  Bank 5.
 * xfer_punter_send/recv stay RESIDENT (they call krnio_load which overwrites
 * this zone); files_run and the Punter implementation live here. */
#pragma overlay( ovl_files, 5 )
#pragma section( files_code, 0 )
#pragma section( files_data, 0 )
#pragma section( files_bss,  0, , , bss )
#pragma region( files, 0x9700, 0xC000, , 5, {files_code, files_data, files_bss} )

/* ZMODEM overlay: bank 6.  Loaded on demand by xfer_zmodem_send/recv in
 * the resident xfer.c shim; never called from within another overlay. */
#pragma overlay( ovl_zmodem, 6 )
#pragma section( zmodem_code, 0 )
#pragma section( zmodem_data, 0 )
#pragma section( zmodem_bss,  0, , , bss )
#pragma region( zmodem, 0x9700, 0xC000, , 6, {zmodem_code, zmodem_data, zmodem_bss} )

/* AUTH overlay: bank 7. src/features/auth.c splits three ways, by caller:
 *   - auth_prompt_login lives HERE (auth_code/auth_data), loaded on demand
 *     from session.c's login call site and reloaded back to OVL_WFC right
 *     after, same pattern as OVL_MSGS. Its only caller (session.c) is
 *     resident and it calls nothing outside bank 7 — a clean load/call/
 *     reload, no cross-bank risk.
 *   - auth_register_new, auth_validate_handle, and the two static handle-
 *     validation helpers it calls (auth_is_all_digits/auth_is_reserved_
 *     handle — inlined into it) live in wfc_code (bank 2) instead: their
 *     callers are newuser.c's registration flow and each other
 *     (auth_register_new calls auth_validate_handle), which are themselves
 *     compiled into wfc_code, not resident. Putting them in a DIFFERENT
 *     overlay bank (e.g. here) would have a bank-2 caller executing
 *     whatever bank actually happens to be resident at $9700, not the
 *     intended callee — overlay code isn't addressable across banks. wfc_code
 *     is the correct, intra-bank home; see the pragma switches in auth.c.
 *   - auth_password_matches, auth_validate_password, auth_check_access stay
 *     resident (default section) — the first is shared by both
 *     auth_prompt_login and auth_validate_password; the latter two have no
 *     caller anywhere in the tree and are dead-stripped regardless of
 *     section. */
#pragma overlay( ovl_auth, 7 )
#pragma section( auth_code, 0 )
#pragma section( auth_data, 0 )
#pragma section( auth_bss,  0, , , bss )
#pragma region( auth, 0x9700, 0xC000, , 7, {auth_code, auth_data, auth_bss} )

/**
 * main_print()
 *
 * Local printf that works with Oscar64 runtime.
 */
static void main_print(const char *text) {
  printf("%s", text);
}

/**
 * main_print_upper()
 *
 * Print to the local uppercase/graphics console, forcing ASCII letters to
 * uppercase. Config-identity strings (BBS name, sysop name, sysop status) are
 * stored mixed-case for caller-facing output, but the local console charset
 * has no lowercase glyphs — lowercase bytes would render as graphics junk.
 */
static void main_print_upper(const char *text) {
  char c;
  while ((c = *text++) != 0) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 0x20);
    putchar(c);
  }
}

/* boot_sequence() runs exactly once and is boot_code/boot_data, same as
 * cfg_init()'s parse helpers (cfg.c) and rel_seq_sweep() (rel_seq.c) —
 * see main()'s doc comment for why main() must load OVL_BOOT itself before
 * calling this, rather than boot_sequence()'s own cfg_init() call doing it
 * (the old, pre-move arrangement). __noinline keeps oscar64 from folding
 * this back into main() at -Oo, which would put its ~1000 bytes of code
 * back in the resident region this move exists to clear. */
#ifdef T64_BOOT_OVERLAY
#pragma code(boot_code)
#pragma data(boot_data)
#endif

#ifdef T64_BOOT_OVERLAY
/* OVL_BOOT / resident-image pair guard.
 *
 * cfg_init() is resident but cfg_load_impl() is in this overlay, so the
 * resident image carries a hardcoded JSR to wherever THAT link placed
 * cfg_load_impl. That address moves on any change to boot-path code, and the
 * SIEC and non-SIEC links place it differently from each other on top of that.
 * Nothing at runtime checks that the OVL_BOOT sitting on disk came from the
 * same link as the BOOT-*.PRG that loaded it, and nothing in the build or
 * deploy tooling distinguishes them either: `make c64` and `make c64-siec`
 * both write the same build/c64/ovl_boot.prg, last build wins.
 *
 * Deploy a BOOT-*.PRG without its matching OVL_BOOT (or vice versa) and that
 * JSR lands mid-instruction inside whatever function now occupies the address.
 * The machine usually survives and returns an arbitrary value, which is how
 * this presents: boot printing "ERROR: CONFIG LOAD FAILED" for an `err` that
 * cfg_load_impl() has no return statement capable of producing. The failure
 * looks like memory corruption in the caller and is not.
 *
 * The stamp is initialized data, so #pragma data(boot_data) above really does
 * put it in the overlay (only *initialized* data is redirected — plain
 * zero-init statics stay in the resident bss; see the s_usrlog_* note below).
 * Its address is fixed by this link, so a foreign OVL_BOOT is caught by the
 * bytes at that address not matching. Read through a volatile pointer so
 * oscar64 cannot fold the comparison against the initializer it can see. */
static u8 s_boot_ovl_stamp[4] = { 'T', '6', '4', 'B' };
#endif

/* USR LOG boot-check scratch: same hazard class as src/data/users.c's
 * s_reu_scratch (src/data/users.c:25-30) — oscar64 can overlay a stack slot
 * with another live variable depending on code layout. Proven on real
 * BOOT-SIEC hardware: with these as plain locals, boot_sequence() logged
 * "check = BBS_ENOTFOUND, got = 30, buf[0] = 1" after a rel_read() that
 * either DMA-wrote 30 bytes into buf (rel_seq.c) or didn't touch it at all
 * on the BBS_ENOTFOUND path — a combination the callee cannot produce, so
 * `check` itself was stack garbage, not the read result. Making buf, check,
 * and got file-static (this fixed address is immune to the overlay) was the
 * one change that turned it into a clean "USR LOG: OK" boot.
 * Unconditional rather than #ifdef T64_STORE_SEQ: rel.c's rel_read() reads a
 * KERNAL channel byte-by-byte instead of DMA-ing, so the REL backend has
 * never shown this failure, but the overlay is a property of oscar64's
 * stack allocator, not of which rel_* backend is linked, so the hazard is
 * latent there too — one code path beats two, and 32 bytes is cheap next to
 * both builds' free BSS. Note this function being boot_code/boot_data (see
 * the pragma switch above) does NOT move these into the boot overlay's own
 * bss: oscar64 only redirects *initialized* data via #pragma data(); plain
 * zero-init statics still land in the default bss section, i.e. the
 * resident main region — confirmed in the build map (BSSEnd advanced by
 * exactly sizeof(s_usrlog_buf)+2). */
static bbs_err_t   s_usrlog_check;
static u8          s_usrlog_buf[RECORD_SIZE_USER];
static u8          s_usrlog_got;
/* Same group, same reason: rel_open() writes the handle, then rel_position(),
 * rel_read() and rel_close() all consume it, so it is live across three calls.
 * As a local it landed at $ce91 inside boot_sequence@stack, a range oscar64
 * also hands to session_display_file/display_msg/fentry_add/msg_index_page. */
static rel_handle_t s_usrlog_h;

/* The ACIA status byte is read once and used twice — printed, then bit-tested
 * for DSR — with printf() in between. Confirmed in BOOT-0.3.1-SIEC.asm: as a
 * plain local oscar64 parked it in the shared zero-page temp T0 ($53) at $991b
 * and re-read it at $9925, i.e. across the printf call, so anything printf's
 * tree does to T0 lands on the DSR line.
 *
 * volatile, and that is load-bearing: file-static alone does NOT move a scalar
 * out of the shared temps. oscar64 sees a static whose address is never taken,
 * proves it is function-local, and register-allocates it right back into T0 —
 * verified by diffing the map and the generated code, which came out
 * byte-identical to the pre-conversion build with no symbol emitted at all.
 * The s_usrlog_* group above escapes that only because rel_read()/rel_open()
 * take their addresses. For a scalar the pointer is never taken, so volatile
 * is what forces the real BSS load/store this fix depends on. */
static volatile u8 s_boot_acia;

/**
 * boot_sequence()
 *
 * Initialize BBS on startup.
 *
 * Returns:
 *   BBS_OK     — ready to accept calls
 *   BBS_EFATAL — fatal initialization error
 */
static __noinline bbs_err_t boot_sequence(void) {
  bbs_err_t err;

  /* Clear screen, set colors */
  vic.color_border = 0;  /* Black border */
  vic.color_back = 0;    /* Black background */

  main_print("\x93\x8e");  /* PETSCII: CLR/HOME, uppercase/graphics mode (default) */
  main_print("\x05\x9e");  /* Light cyan text */

  main_print("TURBO/64 BBS V");
  main_print(BBS_RELEASE_VERSION_COMPACT);
  main_print("\n\n");

  /* Load configuration */
  main_print("LOADING SETUP...\n");
  err = cfg_init();
  if (err != BBS_OK && err != BBS_ENOTFOUND) {
    main_print("ERROR: CONFIG LOAD FAILED\n");
    return BBS_EFATAL;
  }

#ifdef T64_STORE_SEQ
  /* rel_seq_require_storage() and rel_seq_sweep() both live in boot_code,
   * same as boot_sequence() itself (see main()'s doc comment) — calling them
   * from here is safe as long as nothing between main()'s OVL_BOOT load and
   * this point loads a different overlay (OVL_MSGS/OVL_WFC/etc). Nothing
   * does: cfg_init() (just above) no longer loads anything itself.
   *
   * REU check before the marker check: the marker probe goes through
   * disk_open(), and a missing REU means the storage layer is unusable
   * regardless of what the marker says. Both before rel_seq_sweep(), which
   * touches the database. */
  rel_seq_require_storage();

  /* \n, not \r: the next line printed is "  BBS: " + bbs_cfg.bbs_name — an
   * \r here would leave this line's tail on screen whenever the configured
   * name is shorter than "RECOVERING...". */
  main_print("RECOVERING...\n");
  rel_seq_sweep();
#endif

  main_print("  BBS: ");
  main_print_upper(bbs_cfg.bbs_name);
  main_print("\n");
  main_print("  SYSOP: ");
  main_print_upper(bbs_cfg.sysop_name);
  main_print("\n");
  if (cfg_send_drive_init(bbs_cfg.device_system, bbs_cfg.init_system) != BBS_OK) {
    main_print("ERROR: SYSTEM DRIVE INIT FAILED\n");
    return BBS_EFATAL;
  }

  /* Initialize modem (ACIA/SwiftLink) */
  main_print("\nINIT MODEM...\n");
  err = net_init();
  if (err != BBS_OK) {
    main_print("ERROR: MODEM INIT FAILED\n");
    return BBS_EFATAL;
  }
  s_boot_acia = net_acia_status();
  printf("  ACIA STATUS: $%02X\n", (unsigned)s_boot_acia);
  /* $10 = TX empty (normal idle). Bit 6=0 means DSR active. */
  main_print(((s_boot_acia & 0x40) != 0) ? "  DSR: INACTIVE\n" : "  DSR: ACTIVE!\n");
  main_print("  DEVICE: ");
  if (bbs_cfg.device_system != bbs_cfg.device_msgs) {
    printf("SYSTEM=%u/%u MSGS=%u/%u",
           bbs_cfg.device_system, bbs_cfg.drive_system,
           bbs_cfg.device_msgs, bbs_cfg.drive_msgs);
  } else {
    printf("DEFAULT=%u/%u", bbs_cfg.device_system, bbs_cfg.drive_system);
  }
  main_print("\n");

  /* Detect REU (RAM Expansion Unit) if present */
  main_print("\nCHECKING REU...\n");
  {
#ifdef T64_STORE_SEQ
    /* rel_seq_require_storage() above already called reu_detect() (and
     * halted if none was found) — a second call here would repeat the same
     * KERNAL DMA probe for no new information, just read back what it
     * recorded. */
    u16 reu_sz = bbs_cfg.reu_detected_size;
#else
    /* reu_detect() already records size + enabled in bbs_cfg; it returns the
     * detected size in KB (0 = absent). */
    u16 reu_sz = reu_detect();
#endif
    if (reu_sz == 0) {
      main_print("  REU: NOT FOUND\n");
    } else if (reu_sz >= 1024) {
      printf("  REU: %u MB\n", (unsigned)(reu_sz >> 10));  /* /1024 via shift */
    } else {
      printf("  REU: %u KB\n", reu_sz);
    }
  }

  /* Check if USR LOG file exists and has data */
  main_print("\nCHECKING USR LOG...\n");
  {
    s_usrlog_check = rel_open(bbs_cfg.device_system, bbs_cfg.drive_system,
                               "USR LOG", RECORD_SIZE_USER, &s_usrlog_h);
    if (s_usrlog_check == BBS_OK) {
      /* Verify file has data by attempting to read record 1 (SYSOP) */
      s_usrlog_got = 0;
      s_usrlog_check = rel_position(s_usrlog_h, 1);
      if (s_usrlog_check == BBS_OK) {
        s_usrlog_check = rel_read(s_usrlog_h, s_usrlog_buf, RECORD_SIZE_USER, &s_usrlog_got);
      }
      rel_close(s_usrlog_h);
      /* Verify record 1 has SYSOP user (ID=1, not 0 which means empty) */
      if (s_usrlog_check == BBS_OK && s_usrlog_got > 0 && s_usrlog_buf[0] == 1) {
        main_print("  USR LOG: OK\n");
#ifndef T64_STORE_SEQ
        /* Load the user-record cache into REU (robust file-static DMA path);
         * report whether it's serving from REU or falling back to disk.
         * Under T64_STORE_SEQ this whole cache is redundant — rel_open()
         * already keeps USR LOG resident in REU via rel_seq.c's own region
         * map — so there is nothing honest left to report here; the line
         * is dropped rather than printed with a hardcoded, misleading
         * answer. */
        user_cache_load(bbs_cfg.device_system);
        main_print(user_cache_active() ? "  USER CACHE: ON (REU)\n"
                                       : "  USER CACHE: OFF (DISK)\n");
#endif
      } else {
        main_print("  USR LOG: EMPTY\n");
#ifdef T64_STORE_SEQ
        /* Do NOT scratch here. Under rel_seq.c, rel_open() never creates
         * anything on disk — it only stages data in REU and persists on
         * flush (region_load() treats DOS 62 "not found" and a genuine read
         * failure the same way: an empty in-REU region). So a failed check
         * below is not proof this is a stub rel_open() just made; it can
         * just as easily be a real, populated USR LOG that failed to read
         * (wrong CD:, a KERNAL channel left open by something between
         * cfg_init() and here, etc). Scratching on that guess is exactly
         * the bug that deleted a live 101-user database on real hardware —
         * leave the file alone and make the sysop investigate instead. */
        main_print("\nERROR: USR LOG COULD NOT BE READ\n");
        main_print("FILE ON DISK WAS NOT DELETED.\n");
        main_print("DO NOT REINITIALIZE WITH CONFIGURE\n");
        main_print("UNLESS NO DATABASE EXISTS - CHECK THE\n");
        main_print("REU AND STORAGE PATH, THEN REBOOT.\n");
#else
        /* Clean up the empty file that was auto-created by rel_open: under
         * the REL backend (this file only) rel_open() creates the file on
         * disk immediately if it did not already exist, so reaching here
         * means an empty stub that this same boot just made, and removing
         * it is safe. This reasoning does not hold for T64_STORE_SEQ — see
         * the branch above. */
        disk_scratch(bbs_cfg.device_system, bbs_cfg.drive_system, "USR LOG");
        main_print("\nERROR: USR LOG FILE NOT INITIALIZED\n");
        main_print("RUN CONFIGURE-");
        main_print(BBS_RELEASE_VERSION_COMPACT);
        main_print(".PRG TO INITIALIZE\n");
        main_print("THE USER DATABASE BEFORE RUNNING BOOT.\n");
#endif
        return BBS_EFATAL;
      }
    } else {
      main_print("  USR LOG: NOT FOUND\n");
      main_print("\nERROR: USR LOG FILE NOT FOUND\n");
      main_print("RUN CONFIGURE-");
      main_print(BBS_RELEASE_VERSION_COMPACT);
      main_print(".PRG TO INITIALIZE\n");
      main_print("THE USER DATABASE BEFORE RUNNING BOOT.\n");
      return BBS_EFATAL;
    }
  }

  main_print("\nCHECKING FOR REAL TIME CLOCK...\n");

  /* Initialize menu system */
  err = menu_init();
  if (err != BBS_OK) {
    main_print("ERROR: MENU SYSTEM INIT FAILED\n");
    return BBS_EFATAL;
  }

  return BBS_OK;
}

#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif

/**
 * main_loop()
 *
 * WFC-driven BBS main loop.
 *
 * Loop:
 *   1. Display C*BASE-style WFC screen
 *   2. Poll modem + keyboard via wfc_update()
 *   3. On carrier: run session
 *   4. After session: log to activity buffer, redraw WFC
 */
static void main_loop(void) {
  session_t session;
  // cppcheck-suppress variableScope
  bbs_err_t err;
  u16 got;
  clock_tod_t sess_start;
  u16 elapsed;

  wfc_display();

  /* Arm the Timer-B RX interrupt now that boot disk I/O is done, so the
   * inbound CONNECT result code is captured without single-byte overrun. */
  net_irq_arm();

  for (;;) {
    /* Poll modem + keyboard; updates time field each second */
    err = wfc_update();

    if (err == BBS_EQUIT) {
      break;  /* graceful exit — returns to BASIC */
    }

    if (err == BBS_EAGAIN) {
      /* ── New caller detected (modem or local sysop logon) ── */
      {
        bool_t local = wfc.local_logon;
        wfc.local_logon = FALSE;   /* consume flag */
        err = session_init(&session, local);
      }
      if (err != BBS_OK) {
        wfc_display();   /* redraw and keep waiting */
        continue;
      }

      /* ── Terminal detection for remote callers (first connection) ── */
      clock_read(&sess_start);
      if (!session.is_local) {
        term_detect_all(&session);
      }
      /* PETSCII sessions run in lowercase/text charset so mixed-case content
       * renders correctly. Detection's g.login display may have left graphics mode. */
      if (session.petscii_lower) {
        session_emit_charset(&session, 0x0Eu);
      }
      session_spy_init(&session);

      /* Display WFC overlay screen during session */
      wfc_display_session(&session);

      /* Run session until logoff or carrier drop */
      while (session.state != SESS_IDLE && session.state != SESS_ERROR) {
        if (!session.is_local) {
          net_rx((void *)0, 0, &got);
          if (net_state() != NET_CONNECTED) {
            break;
          }
        }
        err = session_step(&session);
        if (err != BBS_OK) {
          session.state = SESS_ERROR;
          session.error = err;
        }

        /* Resident SysOp tick: action keys + 80-col status line (once/second). */
        session_spy_poll();

        /* PETSCII 40-col spy panel (WFC overlay; 80-col handled by poll above). */
        {
          clock_tod_t sess_now;
          u16 sess_elapsed;
          clock_read(&sess_now);
          sess_elapsed = clock_elapsed(&sess_start, &sess_now);
          wfc_update_session(&session, sess_elapsed);
        }
      }

      /* Log caller — only once a real user has authenticated (user_id != 0).
       * Callers who hang up before login, and bots that connect without logging
       * in, never get an entry, so they don't clutter the WFC activity log or
       * the on-disk CALLERS file (and a half-typed/garbage handle like a leaked
       * "NO CARRIER" string is never recorded). */
      if (session.user_id != 0) {
        clock_tod_t sess_end;
        clock_read(&sess_end);
        elapsed = clock_elapsed(&sess_start, &sess_end);
        wfc_log_session(session.handle, bbs_cfg.baud_rate, elapsed);
        callers_log(&session, elapsed);
      }

      session_done(&session);
      if (!session.is_local) {
        net_disconnect();
        /* Pump ACIA to let NET_DROPPING → NET_IDLE settle */
        net_rx((void *)0, 0, &got);
        net_rx((void *)0, 0, &got);
      }

      /* Redraw full WFC screen */
      wfc_display();
    }
  }
}

/**
 * main()
 *
 * BBS entry point. Loads OVL_BOOT itself, before calling boot_sequence()
 * (which now lives in that overlay) — boot_sequence() can no longer trigger
 * its own load via cfg_init() the way it used to, since it would already
 * need to be resident in the overlay bank to run cfg_init() in the first
 * place. Uses the KERNAL current device ($BA, set by the LOAD that brought
 * this program in) rather than bbs_cfg.device_system/CFG_DEV_SYSTEM,
 * exactly as cfg_init()'s own (now-redundant, removed) load did: config
 * hasn't been read yet at this point, so bbs_cfg isn't populated, and a
 * compile-time default would read a BBS booted from a non-default device's
 * OVL_BOOT/CONFIG from the wrong drive. cfg_load_impl() (cfg.c) separately
 * reads $BA again to pick the matching CONFIG file — the two must agree,
 * and both reading the same live KERNAL variable is what guarantees that.
 */
int main(void)
{
  bbs_err_t err;

  /* Bank out BASIC ROM ($A000-$BFFF) so BSS placed there by the linker is
   * accessible as RAM.  Keeps KERNAL ($E000) and I/O ($D000) active. */
  *((volatile char *)0x01) = 0x36;

  /* Load the boot overlay (bank 3) before boot_sequence() can run from it.
   * Deliberately does NOT call disk_select_partition() (or the shared
   * disk_load_overlay() every later overlay load goes through) first: in
   * T64_STORE_SEQ builds that positions the drive at a section folder via
   * bbs_cfg.init_system, and cfg_init() — which reads CONFIG and populates
   * init_system — hasn't run yet at this point (it runs later, from inside
   * this same overlay). This load must and does rely on the drive's boot
   * cursor, i.e. wherever the LOAD that brought BOOT.PRG in left it — the
   * tree root, where OVL_BOOT and CONFIG both live for exactly this reason. */
  krnio_setnam(P"OVL_BOOT");
  if (!krnio_load(1, *(volatile u8 *)0xBA, 1)) {
    main_print("\nERROR: OVL_BOOT LOAD FAILED.\n");
    /* bbs_cfg.init_system/drive_system are still their pre-cfg_init()
     * defaults here, so disk_reset_cursor_root() is a no-op in both builds
     * — correctly: the cursor hasn't moved from the boot cursor yet either.
     * Called anyway rather than special-cased out, so this exit doesn't
     * silently rot if that ever changes. See the block below the final
     * return for the other half of this exit's cleanup (BASIC ROM
     * restore), needed on every exit path. */
    disk_reset_cursor_root(*(volatile u8 *)0xBA);
    *((volatile char *)0x01) = 0x37;
    return 1;
  }

#ifdef T64_BOOT_OVERLAY
  /* Verify the OVL_BOOT just loaded was built by the same link as this PRG
   * before jumping into it — see the stamp's definition for what a mismatched
   * pair does instead. */
  {
    const volatile u8 *stamp = s_boot_ovl_stamp;
    if (stamp[0] != 'T' || stamp[1] != '6' ||
        stamp[2] != '4' || stamp[3] != 'B') {
      main_print("\nERROR: OVL_BOOT/BOOT MISMATCH.\n");
      main_print("REDEPLOY ALL OVL_* WITH BOOT.\n");
      /* Still pre-cfg_init(): see the OVL_BOOT-load-failure exit above for
       * why this is a correct no-op here too. */
      disk_reset_cursor_root(*(volatile u8 *)0xBA);
      *((volatile char *)0x01) = 0x37;
      return 1;
    }
  }
#endif

  /* Boot sequence (now running from OVL_BOOT) */
  err = boot_sequence();
  if (err != BBS_OK) {
    main_print("\nBOOT FAILED.\n");
    /* This is the exit that matters most: boot_sequence() runs cfg_init()
     * and, under T64_STORE_SEQ, rel_seq_require_storage()/rel_open() too, so
     * a failure here typically means the cursor is already stranded inside
     * SYSTEM/ (or, in the REL build, a partition other than drive_system) —
     * exactly the state that breaks the operator's very next
     * LOAD"BOOT...",device retry. bbs_cfg.device_system may still be its
     * zero default if cfg_init() itself is what failed; disk_cmd() /
     * disk_select_partition() send to whatever device/partition that is,
     * which is the same "harmless if wrong, inert if the folder doesn't
     * exist" case documented on disk_reset_cursor_root(). */
    disk_reset_cursor_root(bbs_cfg.device_system);
    *((volatile char *)0x01) = 0x37;
    return 1;
  }

  /* wfc_init() loads OVL_WFC, displacing OVL_BOOT — safe now that
   * boot_sequence() has returned to resident code. */
  wfc_init();

  /* Main loop */
  main_loop();

  /* Graceful WFC exit (BBS_EQUIT) — the cursor could be anywhere a section
   * op last left it (msgs/files/gfiles), same stranded-cursor hazard as the
   * boot-failure exit above. */
  disk_reset_cursor_root(bbs_cfg.device_system);

  /* Restore BASIC ROM ($01 = $37) before returning to BASIC on every exit
   * path, fatal or not. Oscar64's startup RTS-es back to BASIC once main()
   * returns; if BASIC ROM is still banked out, that RTS runs into RAM (our
   * BSS at $A000-$BFFF) instead of BASIC code — observed on hardware as an
   * endless reboot loop instead of a READY. prompt a SysOp could act on.
   *
   * This RTS-to-BASIC only works because BOOT's BSSEnd (see the .map file)
   * stays below $A000 — BASIC's own post-LOAD workspace pointers land
   * comfortably above it, so ROM being banked back in here doesn't shadow
   * live BASIC state the way it does for CONFIGURE (src-editor/main.c's
   * BSSEnd sits at ~$B6xx, inside the $A000-$BFFF window, which is exactly
   * why that binary jumps to the KERNAL reset vector instead of RTSing).
   * If BOOT's BSSEnd ever grows past $A000, this RTS becomes exactly that
   * same hazard — check BSSEnd in the map after any change that grows the
   * resident region and switch to the same reset-vector jump if it crosses. */
  *((volatile char *)0x01) = 0x37;

  return 0;
}
