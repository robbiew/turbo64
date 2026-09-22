"""Unit tests for tools/siec_clean.py — the --clean / data-keeping rules.

Every rule here encodes SoftIEC behaviour measured on a C64 Ultimate
(firmware 1.1.0, 2026-09-22): the C64 always writes lowercase "<name>.seq",
an extensionless file only comes from the PC, with both present the C64
reads the extensionless one, and a scratch removes both.
"""
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import siec_clean as sc

fails = 0

def check(name, got, want):
    global fails
    if got != want:
        print(f"FAIL {name}: got {got!r} want {want!r}")
        fails += 1

# A manifest as the current migrator writes it.
M = {("ROOT", "config.seq"), ("ROOT", "BOOT-SIEC.prg"), ("ROOT", "ovl_boot.prg"),
     ("SYSTEM", "usr log.seq"), ("SYSTEM", "callers.seq"), ("SYSTEM", "access.seq"),
     ("SYSTEM", "g.login.seq"), ("SYSTEM", "T64.SIEC"), ("SYSTEM", "ovl_wfc.prg"),
     ("MSGS", "T64.SIEC"), ("FILES", "uds.seq")}

def decide(section, name, live):
    return sc.classify_entry(section, name, M, live)

# --- CBM name key ---------------------------------------------------------
check("key.strip_seq", sc._cbm_key("USR LOG.seq"), "usr log")
check("key.plain", sc._cbm_key("CALLERS"), "callers")
check("key.other_ext_kept", sc._cbm_key("T64.SIEC"), "t64.siec")

# --- is_protected matches on the CBM name, any spelling --------------------
for n in ("usr log.seq", "USR LOG.seq", "USR LOG", "BOARDS.seq", "b3.idx.seq",
          "B3.TXT", "ud2.seq", "syscnt.seq", "status.seq", "USR.PTR.seq"):
    check(f"protected.{n}", sc.is_protected(n), True)
check("protected.not_gfile", sc.is_protected("g.login.seq"), False)
check("protected.not_config", sc.is_protected("CONFIG"), False)

# --- part of this deploy, ignoring case (the C64 writes BOARDS.seq) --------
check("deploy.exact", decide("SYSTEM", "callers.seq", ["callers.seq"]),
      ("KEEP", "part of this deploy"))
check("deploy.case", decide("SYSTEM", "CALLERS.seq", ["CALLERS.seq"]),
      ("KEEP", "part of this deploy"))
check("deploy.uds_case", decide("FILES", "UDS.seq", ["UDS.seq"]),
      ("KEEP", "part of this deploy"))

# --- old-migrator tree: extensionless data files are the LIVE data ---------
old_tree = ["USR LOG", "CALLERS", "ACCESS", "g.login", "T64.SIEC", "ovl_wfc.prg"]
check("old.usrlog_renamed", decide("SYSTEM", "USR LOG", old_tree),
      ("RENAME", "usr log.seq"))
check("old.gfile_renamed", decide("SYSTEM", "g.login", old_tree),
      ("RENAME", "g.login.seq"))
check("old.marker_kept", decide("SYSTEM", "T64.SIEC", old_tree),
      ("KEEP", "part of this deploy"))

# --- twin present: the BBS read one, the C64 wrote the other → hands off ---
twins = ["CALLERS", "callers.seq", "USR LOG"]
check("twin.conflict", decide("SYSTEM", "CALLERS", twins)[0], "CONFLICT")
check("twin.seq_side_kept", decide("SYSTEM", "callers.seq", twins),
      ("KEEP", "part of this deploy"))
check("twin.other_still_renamed", decide("SYSTEM", "USR LOG", twins),
      ("RENAME", "usr log.seq"))

# --- old extensionless CONFIG is renamed (then overwritten by the upload) --
check("old.config_renamed", decide("ROOT", "CONFIG", ["CONFIG"]),
      ("RENAME", "config.seq"))
# --- stale .seq shadow of a non-data deploy file is removed ---------------
check("shadow.marker_seq", decide("SYSTEM", "t64.siec.seq", ["T64.SIEC", "t64.siec.seq"])[0],
      "REMOVE")

# --- C64-written data not in the seed is protected, never touched ----------
check("live.boards", decide("MSGS", "BOARDS.seq", ["BOARDS.seq"])[0], "KEEP")
check("live.msg_body", decide("MSGS", "b1.txt.seq", ["b1.txt.seq"])[0], "KEEP")
check("live.status", decide("SYSTEM", "status.seq", ["status.seq"])[0], "KEEP")

# --- stale binaries / diagnostics still go ---------------------------------
check("stale.boot", decide("ROOT", "BOOT-0.3.1-SIEC.prg", [])[0], "REMOVE")
check("stale.diag", decide("ROOT", "PTEST.prg", [])[0], "REMOVE")
check("stale.ovl_wrong_place", decide("MSGS", "ovl_wfc.prg", [])[0], "REMOVE")

# --- unknown things are left alone ----------------------------------------
check("unknown.kept", decide("SYSTEM", "notes.txt", [])[0], "KEEP_UNRECOGNIZED")

print(f"siec_clean: {fails} failed")
sys.exit(1 if fails else 0)
