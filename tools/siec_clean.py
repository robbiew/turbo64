#!/usr/bin/env python3
"""Classify a live SoftIEC deploy tree's contents as remove/keep, and diff a
live listing against the local deploy manifest.

Used by `tools/deploy.sh siec --clean`. This tool never talks to the C64
Ultimate itself — deploy.sh owns every network call (via c64u), gated by
--execute exactly like every other c64u invocation in that script. This
tool only classifies data deploy.sh already fetched (`c64u fs ls --json`)
against the manifest deploy.sh already built (the local siec-tree).

Why this exists: SoftIEC derives the CBM filename by stripping a
host-side type-marker extension, so a diagnostic scratch file named
"USR LOG.seq" and the real "USR LOG" database present to the C64 as the
SAME name — which one opens is undefined. A stale binary left over from
before BOOT-SIEC.prg / CONFIGURE-SIEC.prg became fixed, version-independent
names is the same class of danger — e.g. a pre-rename BOOT-0.3.1-SIEC.prg
sitting beside the current BOOT-SIEC.prg. Getting a remove decision WRONG
here can destroy a user database, so every rule below defaults to keeping:
an entry that isn't positively matched by a remove rule is reported
unrecognized and left alone, never deleted on the assumption it is junk.

Two subcommands:
  classify   pre-upload: decide what's safe to remove before a fresh
             upload, so a stale/colliding file is never left beside a
             file with the same effective CBM name.
  verify     post-upload: diff a fresh listing against the manifest to
             catch both leftovers (a remove-rule match that is somehow
             still present) and upload failures (a manifest entry that
             never actually arrived).
"""
import argparse
import json
import re
import sys

SECTIONS = ("SYSTEM", "MSGS", "FILES", "DOORS")

# Diagnostic PRGs built from src-diag/ (see Makefile's `diag` target and
# CLAUDE.md). Matched against the name with any .prg/.PRG suffix stripped.
DIAG_PRGS = {
    "SIECPROBE", "SEQTEST", "SEQNAME", "USRREAD", "USRSWEEP", "CFGREAD",
    "PTEST", "RELTEST", "CPTEST", "DIR", "EXISTS", "CLEAN", "WIPE",
    "COPYALL",
}

# Probe scratch artifacts left behind by src-diag/siecprobe.c.
PROBE_SCRATCH_PREFIXES = ("PERF.DAT", "PERF2.DAT", "SP1")
PROBE_SCRATCH_DIRS = {"STRAND"}

# Never removed under any rule below, regardless of what else matches.
# Exact names (no suffix variants expected) vs. prefixes (counters/pointers
# that carry a numeric or .NEW-style suffix written by the BBS itself).
PROTECTED_EXACT = {"USR LOG", "USR PROF", "ACCESS", "CALLERS", "T64.SIEC"}
PROTECTED_PREFIXES = (
    "syscnt", "USR.PTR", "USR.DAY", "BOARDS", "UDS", "VOTE1", "DOORS",
)
PROTECTED_PATTERNS = (
    re.compile(r"^B\d+\.(IDX|TXT)", re.IGNORECASE),
    re.compile(r"^UD\d+", re.IGNORECASE),
)

BOOT_RE = re.compile(r"^BOOT[-.].*\.PRG$", re.IGNORECASE)
CONFIGURE_RE = re.compile(r"^CONFIGURE[-.].*\.PRG$", re.IGNORECASE)
OVL_RE = re.compile(r"^ovl_.*\.prg$", re.IGNORECASE)
SEQ_RE = re.compile(r"^(.*)\.seq$", re.IGNORECASE)


def _cbm_key(name):
    """The CBM name SoftIEC presents a host file under: case-folded, with a
    trailing ".seq" type marker removed. Two host names with equal keys are
    the same file to the C64."""
    m = SEQ_RE.match(name)
    return (m.group(1) if m else name).lower()


def is_protected(name):
    if name in PROTECTED_EXACT:
        return True
    for prefix in PROTECTED_PREFIXES:
        if name.upper().startswith(prefix.upper()):
            return True
    for pat in PROTECTED_PATTERNS:
        if pat.match(name):
            return True
    return False


def is_diag_prg(name):
    base = name
    if base.upper().endswith(".PRG"):
        base = base[:-4]
    return base.upper() in DIAG_PRGS


def is_probe_scratch(name):
    upper = name.upper()
    return any(upper.startswith(p.upper()) for p in PROBE_SCRATCH_PREFIXES)


def classify_entry(section, name, manifest):
    """Return (decision, reason). decision is one of:
    REMOVE, KEEP (recognized/protected), KEEP_UNRECOGNIZED.
    """
    if section == "ROOT" and name in SECTIONS:
        return ("SKIP", "section directory, not a file")
    if name in (".", ".."):
        return ("SKIP", "directory marker")

    if name.upper() == "STRAND":
        return ("REMOVE", "probe scratch directory (src-diag/siecprobe.c CD:STRAND)")

    if (section, name) in manifest:
        return ("KEEP", "part of this deploy")

    # Collision: two host files that SoftIEC presents under one CBM name.
    # Compared case-insensitively with any ".seq" marker stripped from BOTH
    # sides — SoftIEC matches names without regard to case (the stick is
    # FAT), so "callers.seq" and "CALLERS" are the same CBM file. Measured
    # on hardware 2026-09-21: the C64 reads the extensionless copy but its
    # scratch/write create and update the ".seq" one, so whichever of the
    # pair this deploy is NOT writing is a stale shadow and must go. This
    # runs before is_protected() on purpose: a stale "CALLERS" beside the
    # deploy's "callers.seq" would otherwise be kept as protected data.
    key = _cbm_key(name)
    for msection, mname in manifest:
        if msection == section and _cbm_key(mname) == key:
            return ("REMOVE",
                     f"collides with deploy file {mname!r} — SoftIEC strips "
                     f"the type-marker extension and ignores case, so both "
                     f"answer to the same CBM name")

    if is_protected(name):
        return ("KEEP", "protected: user/message/file-area data or runtime counter")

    if is_diag_prg(name):
        return ("REMOVE", "known src-diag/ diagnostic PRG")

    if is_probe_scratch(name):
        return ("REMOVE", "probe scratch file (src-diag/siecprobe.c)")

    if BOOT_RE.match(name):
        return ("REMOVE", "stale/foreign BOOT binary (not part of this deploy)")

    if CONFIGURE_RE.match(name):
        return ("REMOVE", "stale/foreign CONFIGURE binary (not part of this deploy)")

    if OVL_RE.match(name):
        return ("REMOVE", "ovl_*.prg not part of this deploy (wrong version or location)")

    return ("KEEP_UNRECOGNIZED", "not matched by any rule — left alone, review manually")


def load_manifest(path):
    """manifest file: one relative path per line, e.g. 'CONFIG' or
    'SYSTEM/USR LOG'. Returns a set of (section, name) tuples.
    """
    entries = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if "/" in line:
                section, name = line.split("/", 1)
            else:
                section, name = "ROOT", line
            entries.add((section, name))
    return entries


def load_listing(path):
    """Parse one `c64u fs ls <dir> --json` capture. Returns [name, ...].

    Deliberately defensive and narrow: this only recognizes a handful of
    plausible JSON shapes (a bare list, or a dict wrapping the list under
    a common key). Anything else raises ValueError — the caller aborts
    rather than guessing, because a wrong guess here classifies against
    fabricated data.
    """
    with open(path, encoding="utf-8") as f:
        data = json.load(f)

    if isinstance(data, list):
        items = data
    elif isinstance(data, dict):
        items = None
        for key in ("entries", "files", "items", "data", "results"):
            if isinstance(data.get(key), list):
                items = data[key]
                break
        if items is None:
            raise ValueError(
                f"{path}: JSON object has none of the expected list keys "
                f"(entries/files/items/data/results); got keys {list(data.keys())}"
            )
    else:
        raise ValueError(f"{path}: unexpected top-level JSON type {type(data).__name__}")

    names = []
    for item in items:
        if isinstance(item, str):
            names.append(item)
            continue
        if not isinstance(item, dict):
            raise ValueError(f"{path}: unexpected entry type {type(item).__name__}: {item!r}")
        name = None
        for key in ("name", "Name", "filename", "Filename"):
            if key in item:
                name = item[key]
                break
        if name is None:
            raise ValueError(f"{path}: entry has no recognizable name field: {item!r}")
        names.append(name)
    return names


def parse_listing_args(pairs):
    """--listing SECTION=path.json, repeatable. Returns {section: [names]}."""
    out = {}
    for pair in pairs:
        if "=" not in pair:
            sys.exit(f"--listing must be SECTION=path.json, got: {pair!r}")
        section, path = pair.split("=", 1)
        if section != "ROOT" and section not in SECTIONS:
            sys.exit(f"--listing section must be ROOT or one of {SECTIONS}, got: {section!r}")
        try:
            out[section] = load_listing(path)
        except (OSError, ValueError, json.JSONDecodeError) as e:
            sys.exit(f"failed to parse listing for {section}: {e}")
    return out


def cmd_classify(args):
    manifest = load_manifest(args.manifest)
    listings = parse_listing_args(args.listing)

    to_remove = []
    to_keep = []
    to_review = []

    for section in ("ROOT",) + SECTIONS:
        for name in listings.get(section, []):
            decision, reason = classify_entry(section, name, manifest)
            label = f"{section}/{name}" if section != "ROOT" else name
            if decision == "SKIP":
                continue
            elif decision == "REMOVE":
                to_remove.append((section, name, label, reason))
            elif decision == "KEEP":
                to_keep.append((label, reason))
            else:
                to_review.append((label, reason))

    print(f"=== siec --clean classification ({args.base}) ===")
    print()
    if to_remove:
        print(f"REMOVE ({len(to_remove)}):")
        for _, _, label, reason in to_remove:
            print(f"  - {label}  [{reason}]")
    else:
        print("REMOVE: nothing matched a remove rule.")
    print()
    if to_keep:
        print(f"KEEP, recognized ({len(to_keep)}):")
        for label, reason in to_keep:
            print(f"  - {label}  [{reason}]")
    print()
    if to_review:
        print(f"KEEP, UNRECOGNIZED — left alone, review manually ({len(to_review)}):")
        for label, reason in to_review:
            print(f"  - {label}  [{reason}]")
    else:
        print("KEEP, unrecognized: none.")

    if args.emit_removals:
        with open(args.emit_removals, "w", encoding="utf-8") as f:
            for section, name, _, _ in to_remove:
                sect_path = args.base if section == "ROOT" else f"{args.base}/{section}"
                f.write(f"{sect_path}/{name}\n")

    return 0


def cmd_verify(args):
    manifest = load_manifest(args.manifest)
    listings = parse_listing_args(args.listing)

    seen = set()
    leftovers = []
    unrecognized = []

    for section in ("ROOT",) + SECTIONS:
        for name in listings.get(section, []):
            decision, reason = classify_entry(section, name, manifest)
            if decision == "SKIP":
                continue
            seen.add((section, name))
            label = f"{section}/{name}" if section != "ROOT" else name
            if decision == "REMOVE":
                leftovers.append((label, reason))
            elif decision == "KEEP_UNRECOGNIZED":
                unrecognized.append((label, reason))

    missing = []
    for section, name in sorted(manifest):
        if (section, name) not in seen:
            label = f"{section}/{name}" if section != "ROOT" else name
            missing.append(label)

    print(f"=== siec post-deploy verification ({args.base}) ===")
    print()
    ok = True
    if missing:
        ok = False
        print(f"MISSING ({len(missing)}) — manifest entries not found remotely "
              f"(upload may have failed):")
        for label in missing:
            print(f"  - {label}")
    else:
        print("MISSING: none — every manifest file was found remotely.")
    print()
    if leftovers:
        ok = False
        print(f"LEFTOVER ({len(leftovers)}) — still present, matches a remove rule:")
        for label, reason in leftovers:
            print(f"  - {label}  [{reason}]")
    else:
        print("LEFTOVER: none.")
    print()
    if unrecognized:
        print(f"UNRECOGNIZED ({len(unrecognized)}) — not part of this deploy, left alone:")
        for label, reason in unrecognized:
            print(f"  - {label}  [{reason}]")
    else:
        print("UNRECOGNIZED: none.")

    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--manifest", required=True,
                         help="file listing relative paths this deploy writes, "
                              "one per line (e.g. 'CONFIG', 'SYSTEM/USR LOG')")
    common.add_argument("--base", required=True,
                         help="SoftIEC Default Path this tree lives at (for display "
                              "and for building --emit-removals full paths)")
    common.add_argument("--listing", action="append", default=[], metavar="SECTION=path.json",
                         help="repeatable: one `c64u fs ls <dir> --json` capture per "
                              "section (ROOT, SYSTEM, MSGS, FILES, DOORS)")

    p_classify = sub.add_parser("classify", parents=[common],
                                 help="pre-upload: what's safe to remove")
    p_classify.add_argument("--emit-removals", metavar="path",
                             help="write one full remote path per REMOVE decision, "
                                  "one per line, for the caller to act on")
    p_classify.set_defaults(func=cmd_classify)

    p_verify = sub.add_parser("verify", parents=[common],
                               help="post-upload: diff live listing against manifest")
    p_verify.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == "__main__":
    main()
