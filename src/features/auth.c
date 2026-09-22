/**
 * TURBO/64 BBS — Authentication Module (Implementation)
 *
 * User login, registration, password validation.
 */

#include "bbs/auth.h"
#include "bbs/users.h"
#include "bbs/cfg.h"
#include "bbs/io.h"
#include <string.h>
#include <stdio.h>

/**
 * auth_password_matches()
 *
 * Compare a password attempt against a stored 4-byte hash.
 */
static bool_t auth_password_matches(u8 user_id, const char *attempt,
                                    const char *stored_hash) {
  char hash[4];

  user_hash_password(user_id, attempt, hash);
  return (strncmp(hash, stored_hash, 4) == 0) ? TRUE : FALSE;
}

/**
 * auth_validate_password()
 *
 * Check if provided password matches stored password hash.
 */
bbs_err_t auth_validate_password(const char *handle, const char *password_attempt) {
  user_record_t user;
  u8 user_id;

  /* Find user by handle */
  user_id = user_by_handle(handle, bbs_cfg.device_system);
  if (user_id == 0) {
    return BBS_ENOTFOUND;
  }

  /* Load user record */
  if (user_by_id(user_id, &user, bbs_cfg.device_system) != BBS_OK) {
    return BBS_EIO;
  }

  if (!auth_password_matches(user_id, password_attempt, user.password)) {
    return BBS_EPERM;  /* Bad password */
  }

  return BBS_OK;
}

/**
 * auth_check_access()
 *
 * Verify user's access level meets minimum requirement.
 */
bbs_err_t auth_check_access(const user_record_t *user, u8 min_level) {
  if (!user) {
    return BBS_EBADARG;
  }

  if (user->access_level < min_level) {
    return BBS_EPERM;
  }

  return BBS_OK;
}

/* auth_is_reserved_handle / auth_is_all_digits are static helpers called
 * only from auth_validate_handle, just below, which lives in wfc_code —
 * they used to inline into it there for free, but moving the caller to
 * wfc_code (see its comment) stopped that: oscar64 materialised them as
 * separate RESIDENT symbols instead, i.e. resident code reachable only
 * from bank 2. Placing them in wfc_code alongside their only caller closes
 * that gap, whether or not oscar64 chooses to re-inline them there. */
#ifdef T64_BOOT_OVERLAY
#pragma code(wfc_code)
#pragma data(wfc_data)
#endif

/**
 * auth_is_reserved_handle()
 *
 * Check if a handle is in the reserved list (case-insensitive).
 * Uses strcasecmp-like comparison without the overhead of an array.
 */
static u8 auth_is_reserved_handle(const char *handle) {
  /* Reserved names: SYSOP, GUEST, SYSTEM, BBS, ADMIN */
  const char *reserved[] = { "SYSOP", "GUEST", "SYSTEM", "BBS", "ADMIN" };
  u8 i, j;

  if (!handle || handle[0] == 0) {
    return FALSE;
  }

  for (i = 0; i < 5; i++) {
    u8 match = 1;
    for (j = 0; j < 15; j++) {
      u8 ca = (u8)handle[j];
      u8 cb = (u8)reserved[i][j];
      if (ca >= 'a' && ca <= 'z') ca -= 0x20;
      if (ca != cb) {
        match = 0;
        break;
      }
      if (ca == 0) break;
    }
    if (match) return TRUE;
  }
  return FALSE;
}

/**
 * auth_is_all_digits()
 *
 * Check if handle contains only digits (0-9).
 */
static u8 auth_is_all_digits(const char *handle) {
  u8 i;

  if (!handle || handle[0] == 0) {
    return FALSE;
  }

  for (i = 0; i < 15; i++) {
    u8 ch = (u8)handle[i];
    if (ch == 0) break;
    if (ch < '0' || ch > '9') return FALSE;
  }

  return TRUE;  /* All chars were digits */
}

/* auth_validate_handle lives in wfc_code (same switch as the two helpers
 * above), the same bank as both of its callers — newuser.c's registration
 * flow and auth_register_new, just below — so every call to it is
 * intra-bank. See the ovl_auth region comment in main.c for why bank 7
 * (auth_prompt_login's bank) is wrong for it and bank 2 is right. */
/**
 * auth_validate_handle()
 *
 * Validate a handle/username for signup.
 */
bbs_err_t auth_validate_handle(const char *handle, u8 device) {
  u8 len;

  if (!handle) {
    return BBS_EBADARG;
  }

  len = (u8)strlen(handle);

  /* Validate length: 2-15 characters */
  if (len < 2 || len > 15) {
    return BBS_EBADARG;
  }

  /* Cannot be all digits */
  if (auth_is_all_digits(handle)) {
    return BBS_EBADARG;
  }

  /* Cannot be a reserved name */
  if (auth_is_reserved_handle(handle)) {
    return BBS_EEXIST;
  }

  /* Must be unique (not already in use) */
  if (user_by_handle(handle, device) != 0) {
    return BBS_EEXIST;
  }

  return BBS_OK;
}


/* auth_prompt_login is called only from session.c (resident) and calls only
 * resident data-layer functions (users.c/usrptr.c have no overlay pragmas
 * on the paths it uses), so it is safe to displace into its own overlay —
 * see the ovl_auth region comment in main.c. auth_register_new and
 * auth_validate_handle, above, do NOT live here: both are called from
 * newuser.c's registration flow, which itself lives in the WFC overlay
 * (bank 2) — an overlay-to-overlay call would land on whatever bank is
 * actually loaded, not the callee, so they live in wfc_code instead. */
#ifdef T64_BOOT_OVERLAY
#pragma code(auth_code)
#pragma data(auth_data)
#endif

/**
 * auth_prompt_login()
 *
 * Interactive login flow.
 * Accepts handle + password from session, validates, populates session->user.
 */
bbs_err_t auth_prompt_login(session_t *s) {
  bbs_err_t err;
  user_record_t user;
  u8 user_id;

  if (!s || !s->handle[0]) {
    return BBS_EBADARG;
  }

  /* Find user by handle */
  user_id = user_by_handle(s->handle, bbs_cfg.device_system);
  if (user_id == 0) {
    return BBS_ENOTFOUND;  /* User not found; offer new account */
  }

  /* Load user record */
  err = user_by_id(user_id, &user, bbs_cfg.device_system);
  if (err != BBS_OK) {
    return err;
  }

  /* Validate password — hash the entered password and compare directly
   * against the already-loaded user record.  The old path called
   * auth_validate_password() which re-looked-up the user from disk,
   * adding two redundant disk/REU operations and mapping any I/O or
   * lookup failure to BBS_EPERM (showing "INVALID LOGIN" even when
   * the real problem was a transient I/O error, not a wrong password). */
  if (!auth_password_matches(user_id, s->password, user.password)) {
    return BBS_EPERM;
  }

  /* Success! Populate session with user data */
  s->user = user;
  s->user_id = user_id;

  /* Load profile for WFC footer display (non-fatal if profile missing) */
  {
    user_profile_record_t prof;
    memset(&prof, 0, sizeof(prof));
    if (user_profile_by_id(user_id, &prof, bbs_cfg.device_system) == BBS_OK) {
        strncpy(s->reg_firstname, prof.firstname, sizeof(s->reg_firstname) - 1);
        strncpy(s->reg_lastname,  prof.lastname,  sizeof(s->reg_lastname)  - 1);
        strncpy(s->reg_location,  prof.location,  sizeof(s->reg_location)  - 1);
    }
  }

  return BBS_OK;
}

/* auth_register_new lives in wfc_code — the SAME overlay bank (2) as its
 * only caller, newuser.c's registration flow, which is itself compiled into
 * wfc_code (see newuser.c's header comment). This is an intra-bank call,
 * not the cross-bank hazard bank-7 auth_prompt_login had to avoid: WFC has
 * 989 bytes free ($9700-$C000 minus msgs/wfc content, per the `regions`
 * table in BOOT-*.map), comfortably covering this function's ~660 bytes.
 * auth_validate_handle (called from here AND from newuser.c directly)
 * stays resident rather than also moving here — it would fit, but there is
 * no need to spend WFC's remaining headroom for savings this task doesn't
 * need. */
#ifdef T64_BOOT_OVERLAY
#pragma code(wfc_code)
#pragma data(wfc_data)
#endif

/**
 * auth_register_new()
 *
 * New user registration flow.
 * Populates session->user with new account, saves to disk (USR LOG + USR PROF).
 */
bbs_err_t auth_register_new(session_t *s) {
  user_record_t user;
  user_profile_record_t profile;
  char hash[4];
  u8 new_id;
  bbs_err_t err;

  if (!s || !s->handle[0]) {
    return BBS_EBADARG;
  }

  /* Validate handle against signup rules */
  err = auth_validate_handle(s->handle, bbs_cfg.device_system);
  if (err != BBS_OK) {
    return err;
  }

  /* Find next available user ID */
  new_id = user_next_id(bbs_cfg.device_system);
  if (new_id == 0) {
    return BBS_EFULL;  /* User table full */
  }

  /* Create new user record */
  memset(&user, 0, sizeof(user));
  user.id = new_id;
  strncpy(user.handle, s->handle, sizeof(user.handle) - 1);
  user.handle[sizeof(user.handle) - 1] = '\0';

  /* Hash and store password */
  user_hash_password(new_id, s->password, hash);
  memcpy(user.password, hash, 4);

  /* Set defaults from config */
  user.access_level   = bbs_cfg.new_user_level;
  user.calls          = 1;
  user.downloads      = 0;
  user.uploads        = 0;
  user.credit_balance = bbs_cfg.new_user_credits;
  user.term_mode      = s->term_mode;
  user.term_width     = s->term_width;
  user.term_rows      = s->term_rows;

  if (user_save(&user, bbs_cfg.device_system) != BBS_OK) {
    return BBS_EIO;
  }

  /* Save profile record (non-fatal if USR PROF not yet initialized) */
  memset(&profile, 0, sizeof(profile));
  profile.id = new_id;
  strncpy(profile.email,     s->reg_email,     sizeof(profile.email)     - 1);
  strncpy(profile.firstname, s->reg_firstname, sizeof(profile.firstname) - 1);
  strncpy(profile.lastname,  s->reg_lastname,  sizeof(profile.lastname)  - 1);
  strncpy(profile.location,  s->reg_location,  sizeof(profile.location)  - 1);
  user_profile_save(&profile, bbs_cfg.device_system);

  /* Populate session */
  s->user      = user;
  s->user_id   = new_id;
  s->is_new_user = TRUE;

  return BBS_OK;
}

#ifdef T64_BOOT_OVERLAY
#pragma code(code)
#pragma data(data)
#endif
