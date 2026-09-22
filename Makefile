# TURBO/64 BBS — Makefile

ROOT        := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
OSCAR64     := $(ROOT)vendor/oscar64/bin/oscar64
OUTDIR      := $(ROOT)build/c64
# SIEC build gets its own output directory. oscar64 writes overlay PRGs
# (ovl_boot.prg, ovl_wfc.prg, ...) beside whatever -o= names, and the REL
# and SIEC builds compile the same overlay names to different addresses —
# sharing OUTDIR would let whichever variant built last silently overwrite
# the other's overlays underneath a still-matching binary.
OUTDIR_SIEC := $(OUTDIR)/siec

VERSION     := $(shell grep 'BBS_RELEASE_VERSION_COMPACT' $(ROOT)include/bbs/version.h | cut -d'"' -f2)

BOOT_PRG        := $(OUTDIR)/BOOT-$(VERSION).prg
CONFIGURE_PRG   := $(OUTDIR)/CONFIGURE-$(VERSION).prg
OVERLAYS_D64 := $(OUTDIR)/overlays.d64
MSGS_OVL_PRG  := $(OUTDIR)/ovl_msgs.prg
BOOT_OVL_PRG  := $(OUTDIR)/ovl_boot.prg
DOORS_OVL_PRG := $(OUTDIR)/ovl_doors.prg
FILES_OVL_PRG := $(OUTDIR)/ovl_files.prg
ZMODEM_OVL_PRG := $(OUTDIR)/ovl_zmodem.prg
AUTH_OVL_PRG := $(OUTDIR)/ovl_auth.prg

INCLUDES    := -i=$(ROOT)include -i=$(ROOT)vendor/oscar64/include -i=$(ROOT)src -i=$(ROOT)src/data
OFLAGS      := -Os -Oo

# BOOT-only defines: strip oscar64's dead printf float path (no %f in the
# codebase). Scoped to BOOT; the editor build is untouched.
# NOTE: -dNOLONG also applies (no %l in source) but currently crashes oscar64's
# code generator (assertion in NativeCodeGenerator.cpp CopyCode), so it's left
# off; NOFLOAT alone frees ~2.4KB, ample for current needs.
BOOT_DEFS   := -dNOFLOAT -dT64_BOOT_OVERLAY

DRVFLAGS    :=
ifdef T64_DRIVE_SYSTEM
DRVFLAGS    += -D=T64_DRIVE_SYSTEM=$(T64_DRIVE_SYSTEM)
endif
ifdef T64_DRIVE_MSGS
DRVFLAGS    += -D=T64_DRIVE_MSGS=$(T64_DRIVE_MSGS)
endif
ifdef T64_DRIVE_FILES
DRVFLAGS    += -D=T64_DRIVE_FILES=$(T64_DRIVE_FILES)
endif
ifdef T64_DRIVE_DOORS
DRVFLAGS    += -D=T64_DRIVE_DOORS=$(T64_DRIVE_DOORS)
endif

CFLAGS      := $(INCLUDES) $(OFLAGS) $(DRVFLAGS)

HAL_SRCS := src/err.c src/hal/clock.c src/hal/disk.c src/hal/rel.c src/hal/reu.c src/hal/term.c src/io/io.c src/net/at_response.c src/net/telnet_iac.c src/net/punter.c src/net/zmodem.c src/platform/cbmdos/net.c src/term/term.c src/term/cp437_ascii.c src/term/cp437_petscii.c src/term/ansi.c
DATA_SRCS := src/data/cfg.c src/data/users.c src/data/boards.c src/data/file_areas.c src/data/file_entries.c src/data/votes.c src/data/messages.c src/data/usrptr.c src/data/sstatus.c src/data/syscnt.c src/data/access.c src/data/usrday.c src/data/user_hash.c src/data/doors.c src/data/door_visible.c src/data/devspec.c
SESSION_SRCS := src/session/session.c src/session/spy80.c src/session/spy80_ansi.c src/session/spy80_font.c src/session/prompt_cursor.c src/session/prompt_cursor_frame.c
FEATURE_SRCS := src/features/menu.c src/features/menu_tables.c src/features/menu_actions.c src/features/auth.c src/features/newuser.c src/features/bulletin.c src/features/editor.c src/features/mail.c src/features/files.c src/features/xfer.c src/features/chat.c src/features/vote.c src/features/callers.c src/features/sysop.c src/features/doors.c
PUB_HDRS := include/bbs/version.h include/bbs/config.h include/bbs/types.h include/bbs/err.h include/bbs/drives.h include/bbs/net.h include/bbs/io.h include/bbs/term.h include/bbs/rel.h include/bbs/records.h include/bbs/cfg.h include/bbs/users.h include/bbs/boards.h include/bbs/file_areas.h include/bbs/file_entries.h include/bbs/votes.h include/bbs/messages.h include/bbs/usrptr.h include/bbs/sstatus.h include/bbs/syscnt.h include/bbs/usrday.h include/bbs/editor.h include/bbs/menu.h include/bbs/session.h include/bbs/prompt_cursor.h include/bbs/auth.h include/bbs/bulletin.h include/bbs/mail.h include/bbs/files.h include/bbs/xfer.h include/bbs/chat.h include/bbs/vote.h include/bbs/callers.h include/bbs/sysop.h include/bbs/hal/clock.h include/bbs/hal/disk.h include/bbs/hal/reu.h include/bbs/hal/term.h include/bbs/menu_actions.h include/bbs/doors.h include/bbs/devspec.h include/bbs/seq_region.h

.PHONY: all c64 editor door door-example clean release test lint c64-siec editor-siec

SETUP_DATA  := $(OUTDIR)/config
SETUP_SRC   := $(ROOT)data/config

all: c64 editor door-example

# Bundled example door (fortune) — built with the rest so the disk can
# demonstrate the DOORS feature; assemble-d81.sh writes it to the image.
door-example:
	@$(MAKE) door DOOR=fortune

c64: $(BOOT_PRG) $(SETUP_DATA)

editor: $(CONFIGURE_PRG)

disk: all
	bash tools/assemble-d81.sh

disk-with-users: all
	bash tools/assemble-d81.sh --fetch-users

release:
	bash tools/release.sh

$(SETUP_DATA): $(SETUP_SRC)
	@mkdir -p $(OUTDIR)
	@cp $< $@

$(BOOT_PRG): src/main.c $(HAL_SRCS) $(DATA_SRCS) $(SESSION_SRCS) $(FEATURE_SRCS) $(PUB_HDRS)
	@mkdir -p $(OUTDIR)
	$(OSCAR64) $(CFLAGS) $(BOOT_DEFS) -o=$@ -d64=$(OVERLAYS_D64) $< $(HAL_SRCS) $(DATA_SRCS) $(SESSION_SRCS) $(FEATURE_SRCS)
	@n="$$(basename $@ .prg)"; if [ $${#n} -gt 16 ]; then echo "ERROR: CBM name '$$n' is $${#n} chars (C64 limit 16): $@" >&2; exit 1; fi
	@echo "Built: $@"

# ovl_msgs.prg / ovl_boot.prg are produced as side-effects of the BOOT_PRG -d64
# build. These targets just verify they exist after the build.
$(MSGS_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: overlay file not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

$(BOOT_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: overlay file not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

$(DOORS_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: overlay file not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

$(FILES_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: overlay file not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

$(ZMODEM_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: ovl_zmodem not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

$(AUTH_OVL_PRG): $(BOOT_PRG)
	@test -f "$@" || { echo "ERROR: ovl_auth not produced by oscar64"; exit 1; }
	@echo "Overlay: $@"

EDITOR_HAL_SRCS := src/err.c src/hal/disk.c src/hal/rel.c
EDITOR_SRCS := src-editor/setup.c src-editor/ui/util.c src-editor/ui/menu.c src-editor/ui/list.c src-editor/ui/edit.c src-editor/ui/dialog.c src-editor/admin/users.c src-editor/admin/messages.c src-editor/admin/config.c src-editor/admin/files.c src-editor/admin/votes.c src-editor/admin/doors.c
# No %f used in the editor, so strip the float printf path (~1.8 KB) as BOOT does.
EDITOR_DEFS := -dNOFLOAT
# reu_stubs.c satisfies messages.c/users.c's REU-gated calls for the default
# editor build, which never has a real REU. The SIEC editor links the real
# src/hal/reu.c instead (see CONFIGURE_SIEC_PRG below) — it needs a working
# REU because rel_seq.c's rel_open() hard-requires reu_data_available().

$(CONFIGURE_PRG): src-editor/main.c $(EDITOR_SRCS) src-editor/reu_stubs.c $(EDITOR_HAL_SRCS) $(DATA_SRCS) $(PUB_HDRS)
	@mkdir -p $(OUTDIR)
	$(OSCAR64) $(CFLAGS) -i=$(ROOT)src-editor $(CFLAGS) $(EDITOR_DEFS) -o=$@ $< $(EDITOR_SRCS) src-editor/reu_stubs.c $(EDITOR_HAL_SRCS) $(DATA_SRCS)
	@n="$$(basename $@ .prg)"; if [ $${#n} -gt 16 ]; then echo "ERROR: CBM name '$$n' is $${#n} chars (C64 limit 16): $@" >&2; exit 1; fi
	@echo "Built: $@"

# ---------------------------------------------------------------------------
# SoftIEC build (T64_STORE_SEQ): records live in REU-backed flat SEQ files and
# drive_* is a section-folder index rather than a CP<n> partition. Selected
# here by the define and by which rel implementation is linked.
#
# BOOT-SIEC / CONFIGURE-SIEC deliberately do NOT carry $(VERSION) in the
# name, unlike the REL binaries above. CBM filenames top out at 16 chars;
# CONFIGURE-$(VERSION)-SIEC.prg was 20 (CONFIGURE-0.4.0-SIEC) and could
# never load — ?FILE NOT FOUND every time, verified on hardware. Fixed,
# version-independent names close that off for good instead of surviving
# only until the version string grows again. The version isn't lost: it's
# compiled in (include/bbs/version.h) and shown on the boot screen.
# ---------------------------------------------------------------------------
BOOT_SIEC_PRG      := $(OUTDIR_SIEC)/BOOT-SIEC.prg
CONFIGURE_SIEC_PRG := $(OUTDIR_SIEC)/CONFIGURE-SIEC.prg
SIEC_DEFS          := -dT64_STORE_SEQ
SIEC_REL_SRC       := src/hal/rel_seq.c src/hal/seq_region.c

# Same fixed CBM names as the REL build (loaded by krnio_setnam at runtime —
# must not change), but oscar64 writes them beside -o=, so pointing -o= at
# OUTDIR_SIEC is what keeps this set out of the REL build's directory.
OVERLAYS_D64_SIEC  := $(OUTDIR_SIEC)/overlays-siec.d64
MSGS_OVL_PRG_SIEC   := $(OUTDIR_SIEC)/ovl_msgs.prg
BOOT_OVL_PRG_SIEC   := $(OUTDIR_SIEC)/ovl_boot.prg
WFC_OVL_PRG_SIEC    := $(OUTDIR_SIEC)/ovl_wfc.prg
DOORS_OVL_PRG_SIEC  := $(OUTDIR_SIEC)/ovl_doors.prg
FILES_OVL_PRG_SIEC  := $(OUTDIR_SIEC)/ovl_files.prg
ZMODEM_OVL_PRG_SIEC := $(OUTDIR_SIEC)/ovl_zmodem.prg
AUTH_OVL_PRG_SIEC   := $(OUTDIR_SIEC)/ovl_auth.prg

.PHONY: c64-siec editor-siec
c64-siec: $(BOOT_SIEC_PRG)
editor-siec: $(CONFIGURE_SIEC_PRG)

$(BOOT_SIEC_PRG): src/main.c $(HAL_SRCS) $(SIEC_REL_SRC) $(DATA_SRCS) $(SESSION_SRCS) $(FEATURE_SRCS) $(PUB_HDRS)
	@mkdir -p $(OUTDIR_SIEC)
	$(OSCAR64) $(CFLAGS) $(BOOT_DEFS) $(SIEC_DEFS) -o=$@ -d64=$(OVERLAYS_D64_SIEC) \
	  $< $(filter-out src/hal/rel.c,$(HAL_SRCS)) $(SIEC_REL_SRC) \
	  $(DATA_SRCS) $(SESSION_SRCS) $(FEATURE_SRCS)
	@n="$$(basename $@ .prg)"; if [ $${#n} -gt 16 ]; then echo "ERROR: CBM name '$$n' is $${#n} chars (C64 limit 16): $@" >&2; exit 1; fi
	@echo "Built: $@"

$(CONFIGURE_SIEC_PRG): src-editor/main.c $(EDITOR_SRCS) src/hal/reu.c $(EDITOR_HAL_SRCS) $(SIEC_REL_SRC) $(DATA_SRCS) $(PUB_HDRS)
	@mkdir -p $(OUTDIR_SIEC)
	$(OSCAR64) $(CFLAGS) -i=$(ROOT)src-editor $(EDITOR_DEFS) $(SIEC_DEFS) -o=$@ \
	  $< $(EDITOR_SRCS) $(filter-out src/hal/rel.c,$(EDITOR_HAL_SRCS)) $(SIEC_REL_SRC) src/hal/reu.c \
	  $(DATA_SRCS)
	@n="$$(basename $@ .prg)"; if [ $${#n} -gt 16 ]; then echo "ERROR: CBM name '$$n' is $${#n} chars (C64 limit 16): $@" >&2; exit 1; fi
	@echo "Built: $@"


# Storage diagnostics — standalone PRGs that exercise the real HAL against a
# device, for questions only hardware can answer. Each asks for a device number.
#   PTEST   partition round-trip and isolation
#   RELTEST REL file create/position/read
#   CPTEST  what status code CP<n> returns per device
#   DIR     non-destructive directory listing
#   EXISTS  is a named file present (reports the raw DOS code)
#   CLEAN   scratch T/64's system files from a device/partition
#   WIPE    scratch every file on a device/partition (destructive)
#   COPYALL copy the T/64 program set between devices, through the C64
#   SIECPROBE SoftIEC SEQ correctness gates and cost measurements (throwaway)
#   USRREAD   USR LOG raw-open vs rel_seq read, standalone (found the boot data-loss bug)
#   USRSWEEP  reproduces the BOOT-SIEC order (cfg fields, require_storage, sweep) around
#             a USR LOG read, to isolate what breaks it
#   SEQNAME   does a SoftIEC content file need a host .SEQ extension to open
#   CFGREAD   does cfg_init() actually populate the section paths before the USR LOG
#             read — the boot-order difference USRSWEEP couldn't isolate (pinned the
#             oscar64 stack-slot frame-overlay bug fixed in main.c's boot_sequence())
# Usage: make diag        (all four land in build/c64/)
DIAG_HAL := src/err.c src/hal/disk.c
DIAG_FLAGS := $(CFLAGS) -i=$(ROOT)src-diag -dNOFLOAT

.PHONY: diag
diag:
	@mkdir -p $(OUTDIR)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/PTEST.prg   src-diag/ptest.c   $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/RELTEST.prg src-diag/reltest.c $(DIAG_HAL) src/hal/rel.c
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/CPTEST.prg  src-diag/cptest.c  $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/DIR.prg     src-diag/dir.c     $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/EXISTS.prg  src-diag/exists.c  $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/CLEAN.prg   src-diag/clean.c   $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/WIPE.prg    src-diag/wipe.c    $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/COPYALL.prg src-diag/copyall.c $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/SIECPROBE.prg src-diag/siecprobe.c \
	  $(DIAG_HAL) src/hal/reu.c
	$(OSCAR64) $(DIAG_FLAGS) $(SIEC_DEFS) -o=$(OUTDIR)/SEQTEST.prg src-diag/seqtest.c \
	  $(DIAG_HAL) src/hal/rel_seq.c src/hal/seq_region.c src/hal/reu.c
	$(OSCAR64) $(DIAG_FLAGS) $(SIEC_DEFS) -o=$(OUTDIR)/USRREAD.prg src-diag/usrread.c \
	  $(DIAG_HAL) src/hal/rel_seq.c src/hal/seq_region.c src/hal/reu.c
	$(OSCAR64) $(DIAG_FLAGS) $(SIEC_DEFS) -o=$(OUTDIR)/USRSWEEP.prg src-diag/usrsweep.c \
	  $(DIAG_HAL) src/hal/rel_seq.c src/hal/seq_region.c src/hal/reu.c \
	  src/data/cfg.c src/data/devspec.c
	$(OSCAR64) $(DIAG_FLAGS) -o=$(OUTDIR)/SEQNAME.prg src-diag/seqname.c $(DIAG_HAL)
	$(OSCAR64) $(DIAG_FLAGS) $(SIEC_DEFS) -o=$(OUTDIR)/CFGREAD.prg src-diag/cfgread.c \
	  $(DIAG_HAL) src/hal/rel_seq.c src/hal/seq_region.c src/hal/reu.c \
	  src/data/cfg.c src/data/devspec.c \
	  src/platform/cbmdos/net.c src/net/at_response.c src/net/telnet_iac.c
	@echo "Built: PTEST RELTEST CPTEST DIR EXISTS CLEAN WIPE COPYALL SIECPROBE SEQTEST USRREAD USRSWEEP SEQNAME CFGREAD in $(OUTDIR)"

# Build a door PRG at $9700.  Usage: make door DOOR=<name>
# Source: devkit/examples/<name>.c  Output: build/c64/<NAME>.prg
# For a door not under devkit/examples/, pass SRC=<path> override.
# Flags: -n (native; bytecode+overlay crashes oscar64) -O2 (optimize).
# A door is ONE translation unit: the source #includes devkit/door_crt.h (which
# brings in the $9700 overlay/sections + startup) so the author's code AND data
# land in the loaded image.  The $9700 overlay is emitted as DOOR.prg by oscar64;
# -o names the discardable stub.  After the build, move DOOR.prg to <NAME>.prg.
door:
	$(OSCAR64) $(INCLUDES) -i=$(ROOT)devkit -n -O2 \
	  -o=$(OUTDIR)/_door_stub.prg \
	  $(if $(SRC),$(SRC),devkit/examples/$(DOOR).c)
	@mv $(OUTDIR)/DOOR.prg $(OUTDIR)/$(shell echo $(DOOR) | tr a-z A-Z).prg
	@$(RM) $(OUTDIR)/_door_stub.prg $(OUTDIR)/_door_stub.asm \
	        $(OUTDIR)/_door_stub.int $(OUTDIR)/_door_stub.lbl \
	        $(OUTDIR)/_door_stub.map
	@echo "Built: $(OUTDIR)/$(shell echo $(DOOR) | tr a-z A-Z).prg"

test:
	bash tools/test.sh

lint:
	bash tools/lint.sh

clean:
	$(RM) $(OUTDIR)/*.prg $(OUTDIR)/*.asm $(OUTDIR)/*.int $(OUTDIR)/*.lbl $(OUTDIR)/*.map $(OUTDIR)/*.bcs $(OUTDIR)/config $(OVERLAYS_D64) $(MSGS_OVL_PRG) $(BOOT_OVL_PRG) $(DOORS_OVL_PRG) $(FILES_OVL_PRG) $(ZMODEM_OVL_PRG) $(AUTH_OVL_PRG)
	$(RM) -r $(OUTDIR_SIEC)
	$(RM) -r $(ROOT)build/host
	@echo "Clean."
