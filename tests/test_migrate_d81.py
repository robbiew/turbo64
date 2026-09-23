"""Unit tests for the .d81 -> SoftIEC SEQ migration."""
import os, sys, pathlib, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))

import importlib
mig = importlib.import_module("migrate-d81")

fails = 0

def check(name, got, want):
    global fails
    if got != want:
        print(f"FAIL {name}: got {got!r} want {want!r}")
        fails += 1

# A REL file is preallocated to its maximum; the SEQ file must stop at the
# last record with any non-zero byte.
check("trim.empty",  mig.trim_records(bytes(90), 30), b"")
check("trim.one",    mig.trim_records(b"\x01" + bytes(89), 30), b"\x01" + bytes(29))
check("trim.sparse",
      mig.trim_records(bytes(30) + b"\x05" + bytes(29) + bytes(30), 30),
      bytes(30) + b"\x05" + bytes(29))
check("trim.full",   mig.trim_records(b"\x01" * 60, 30), b"\x01" * 60)

# A partial trailing record is padded out, never truncated mid-record.
check("trim.ragged", mig.trim_records(b"\x01" * 45, 30), b"\x01" * 45 + bytes(15))

check("trim.zero_size", mig.trim_records(b"\x01" * 30, 0), b"")

# CBM DOS marks a never-written REL record with $FF in its first byte: such
# records are not data. Trailing ones are dropped; one in the middle is
# blanked so a reader never sees a field of 255.
check("trim.ff_marker_trailing", mig.trim_records(b"\xff" + bytes(39) + b"\xff" + bytes(39), 40), b"")
check("trim.ff_marker_middle",
      mig.trim_records(b"\x01" + bytes(39) + b"\xff" + bytes(39) + b"\x02" + bytes(39), 40),
      b"\x01" + bytes(39) + bytes(40) + b"\x02" + bytes(39))
check("trim.ff_with_data_kept", mig.trim_records(b"\xff\x01" + bytes(38), 40), b"\xff\x01" + bytes(38))

check("spec.system", mig.device_spec(11, "/USB1/TURBO64", "SYSTEM"),
      "11;/USB1/TURBO64/SYSTEM")
check("spec.trailing_slash", mig.device_spec(11, "/USB1/TURBO64/", "MSGS"),
      "11;/USB1/TURBO64/MSGS")

# 23 chars is what cfg_t holds; anything longer is a migration-time error,
# not a silent truncation the SysOp discovers at boot.
try:
    mig.device_spec(11, "/USB1/AAAAAAAAAAAAAAAAAAAA", "SYSTEM")
    check("spec.toolong", "no raise", "ValueError")
except ValueError:
    pass

# --- classify_entry: which disk entries get migrated, and where -----------

# Fixed REL sets still resolve by name (case-insensitive against the disk).
check("classify.rel_fixed", mig.classify_entry("usr log", "rel"),
      ("USR LOG", "SYSTEM", 30))

# Per-board / per-area REL sets that RECORD_SETS cannot see: only reachable
# once a board or file area has been created, so they must be discovered
# from the disk listing rather than assumed.
check("classify.board_idx", mig.classify_entry("b3.idx", "rel"),
      ("B3.IDX", "MSGS", mig.RECORD_SIZE_MSG_IDX))
check("classify.ud_area", mig.classify_entry("ud2", "rel"),
      ("UD2", "FILES", mig.RECORD_SIZE_FILE_ENTRY))
# "uds" itself is the fixed set, not a dynamic UD<n> — must not double-match.
check("classify.uds_is_fixed", mig.classify_entry("uds", "rel"),
      ("UDS", "FILES", 40))

# ACCESS and CALLERS are SEQ, so RECORD_SETS (REL-only) skips them; this is
# the gap Part 1 exists to close.
check("classify.access", mig.classify_entry("access", "seq"),
      ("ACCESS", "SYSTEM", None))
check("classify.callers", mig.classify_entry("callers", "seq"),
      ("CALLERS", "SYSTEM", None))

# --- host_name: the on-stick spelling of every migrated data file ---------

# Must be exactly what SoftIEC writes for "<NAME>,S,W" (lowercase + ".seq"):
# an extensionless PC-written copy survives the C64's scratch and shadows
# every later save (measured on hardware — see the module docstring).
check("host_name.callers", mig.host_name("CALLERS"), "callers.seq")
check("host_name.space", mig.host_name("USR LOG"), "usr log.seq")
check("host_name.dotted", mig.host_name("B3.IDX"), "b3.idx.seq")
check("host_name.gfile", mig.host_name("g.login 1 80"), "g.login 1 80.seq")

# --- merge_config: the image's settings survive, DEV_* are replaced -------

_specs = {"SYSTEM": "11;/USB1/T/SYSTEM", "MSGS": "11;/USB1/T/MSGS",
          "FILES": "11;/USB1/T/FILES", "DOORS": "11;/USB1/T/DOORS"}
_src = b"BBS_NAME=A New T/64 BBS\nSYSOP_NAME=SYSOP\nDEV_SYSTEM=8\nDEV_MSGS=9\nBAUD_RATE=38400\n"
_out = mig.merge_config(_src, _specs)
check("merge.cr_terminated", _out.endswith("\r") and "\n" not in _out, True)
_lines = _out.rstrip("\r").split("\r")
check("merge.settings_kept",
      [l for l in _lines if not l.startswith("DEV_")],
      ["BBS_NAME=A New T/64 BBS", "SYSOP_NAME=SYSOP", "BAUD_RATE=38400"])
check("merge.devices_replaced",
      [l for l in _lines if l.startswith("DEV_")],
      ["DEV_SYSTEM=11;/USB1/T/SYSTEM", "DEV_MSGS=11;/USB1/T/MSGS",
       "DEV_FILES=11;/USB1/T/FILES", "DEV_DOORS=11;/USB1/T/DOORS",
       "DEV_GFILES=11;/USB1/T/SYSTEM"])
# CR-terminated source (what cfg_save() writes on the C64) parses the same.
check("merge.cr_source",
      mig.merge_config(b"BBS_CITY=X\rDEV_FILES=9\r", _specs).split("\r")[0],
      "BBS_CITY=X")
# No source config at all still yields a valid device-only CONFIG.
check("merge.empty_source",
      mig.merge_config(b"", _specs).count("\r"), 5)

# Message bodies (SEQ, not REL) live alongside their B<n>.IDX in MSGS/.
check("classify.board_txt", mig.classify_entry("b7.txt", "seq"),
      ("B7.TXT", "MSGS", None))

# gfiles/menus/prompts keep their exact on-disk (lowercase) spelling.
check("classify.gfile", mig.classify_entry("g.login 1 80", "seq"),
      ("g.login 1 80", "SYSTEM", None))
check("classify.menu", mig.classify_entry("m.main", "seq"),
      ("m.main", "SYSTEM", None))
check("classify.prompt", mig.classify_entry("p.read", "seq"),
      ("p.read", "SYSTEM", None))

# The REL-build's legacy "config" SEQ data file is a different format from
# the SIEC CONFIG this tool writes; it must not be copied over verbatim.
check("classify.legacy_config_excluded", mig.classify_entry("config", "seq"), None)

# Anything unrecognized must come back None (reported, not silently copied
# or silently dropped) rather than raising or matching by accident.
check("classify.unknown_seq", mig.classify_entry("readme", "seq"), None)
check("classify.unknown_rel", mig.classify_entry("mystery", "rel"), None)


# --- CONFIG must land at the tree ROOT, never SYSTEM/ ----------------------

with tempfile.TemporaryDirectory() as td:
    specs = {
        "SYSTEM": "11;/USB1/TURBO64/SYSTEM",
        "MSGS":   "11;/USB1/TURBO64/MSGS",
        "FILES":  "11;/USB1/TURBO64/FILES",
        "DOORS":  "11;/USB1/TURBO64/DOORS",
    }
    os.makedirs(os.path.join(td, "SYSTEM"))
    mig.write_config(td, specs)
    check("config.at_root", os.path.isfile(os.path.join(td, "config.seq")), True)
    check("config.not_extensionless", os.path.isfile(os.path.join(td, "CONFIG")), False)
    check("config.not_in_system",
          os.path.isfile(os.path.join(td, "SYSTEM", "config.seq")), False)


# --- a missing overlay or binary is reported, not silently skipped --------

with tempfile.TemporaryDirectory() as td:
    # Nothing built yet: every required artifact should come back missing.
    missing = mig.find_missing_siec_artifacts(td)
    check("siec.all_missing_when_empty", "ovl_boot.prg" in missing, True)
    check("siec.all_missing_when_empty", "BOOT-SIEC.prg" in missing, True)
    check("siec.all_missing_when_empty", "CONFIGURE-SIEC.prg" in missing, True)
    check("siec.all_missing_when_empty", "ovl_auth.prg" in missing, True)
    check("siec.missing_count", len(missing),
          len(mig.ROOT_SIEC_ARTIFACTS) + len(mig.SYSTEM_SIEC_ARTIFACTS))

    # Build everything except one overlay: exactly that one is reported.
    for n in mig.ROOT_SIEC_ARTIFACTS:
        open(os.path.join(td, n), "w").close()
    for n in mig.SYSTEM_SIEC_ARTIFACTS:
        if n == "ovl_auth.prg":
            continue
        open(os.path.join(td, n), "w").close()
    missing = mig.find_missing_siec_artifacts(td)
    check("siec.one_missing", missing, ["ovl_auth.prg"])

    # Complete set: nothing missing, and copy_siec_artifacts places binaries
    # at the root vs SYSTEM/ per the verified layout.
    open(os.path.join(td, "ovl_auth.prg"), "w").close()
    check("siec.none_missing", mig.find_missing_siec_artifacts(td), [])

    outdir = os.path.join(td, "out")
    os.makedirs(os.path.join(outdir, "SYSTEM"))
    copied = mig.copy_siec_artifacts(td, outdir)
    check("siec.copied_count", len(copied),
          len(mig.ROOT_SIEC_ARTIFACTS) + len(mig.SYSTEM_SIEC_ARTIFACTS))
    check("siec.boot_at_root",
          os.path.isfile(os.path.join(outdir, "BOOT-SIEC.prg")), True)
    check("siec.configure_at_root",
          os.path.isfile(os.path.join(outdir, "CONFIGURE-SIEC.prg")), True)
    check("siec.ovl_boot_at_root",
          os.path.isfile(os.path.join(outdir, "ovl_boot.prg")), True)
    check("siec.ovl_wfc_in_system",
          os.path.isfile(os.path.join(outdir, "SYSTEM", "ovl_wfc.prg")), True)
    check("siec.ovl_wfc_not_at_root",
          os.path.isfile(os.path.join(outdir, "ovl_wfc.prg")), False)


# --- example door: missing is reported, not silently skipped --------------

with tempfile.TemporaryDirectory() as td:
    check("door.missing_when_empty", mig.find_missing_door(td), [mig.DOOR_PRG_SRC])
    check("door.copy_noop_when_missing", mig.copy_door_artifact(td, td), [])

    open(os.path.join(td, mig.DOOR_PRG_SRC), "w").close()
    check("door.none_missing_when_built", mig.find_missing_door(td), [])

    outdir = os.path.join(td, "out")
    os.makedirs(os.path.join(outdir, "DOORS"))
    copied = mig.copy_door_artifact(td, outdir)
    check("door.copied", copied, [mig.DOOR_PRG_DST])
    check("door.lands_in_doors_lowercase",
          os.path.isfile(os.path.join(outdir, "DOORS", mig.DOOR_PRG_DST)), True)


print(f"migrate_d81: {fails} failed")
sys.exit(1 if fails else 0)
