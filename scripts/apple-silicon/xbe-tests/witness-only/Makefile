XBE_TITLE       = witness-only
GEN_XISO        = $(XBE_TITLE).iso
SRCS            = $(CURDIR)/main.c

XBED_LIB_DIR   ?= $(CURDIR)/../lib
include $(XBED_LIB_DIR)/lib.mk

# Cycle-29 option (c) self-allocated witness shim. Opted in here
# (NOT via lib.mk) so the rest of the diag-XBE corpus does not get
# linked with the new code. See lib.mk for rationale.
SRCS           += $(XBED_LIB_DIR)/xbed_self_witness.c

# Cycle-42B candidate B (calling-context discriminator, 2026-05-24):
# custom PE entry-point thunk that fires the cycle-29 self-witness
# shim BEFORE nxdk's standard `WinMainCRTStartup` runs. See the
# in-file header in witness_only_crt0.c for the full rationale +
# discriminator semantics + risk surface. Confined to this XBE; no
# nxdk source edits; no lib/* edits. The `-entry:` link flag below
# selects our thunk as the PE entry point; nxdk's
# `WinMainCRTStartup` is still linked in and is tail-called from the
# thunk so the rest of the runtime is bit-identical to a non-cycle-
# 42B nxdk XBE.
SRCS           += $(CURDIR)/witness_only_crt0.c
NXDK_LDFLAGS   += -entry:witness_only_pre_winmain_crt_startup

NXDK_DIR       ?= /Users/jbbrack03/XEMU_MacOS/nxdk
include $(NXDK_DIR)/Makefile
