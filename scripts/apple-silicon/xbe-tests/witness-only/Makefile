XBE_TITLE       = witness-only
GEN_XISO        = $(XBE_TITLE).iso
SRCS            = $(CURDIR)/main.c

XBED_LIB_DIR   ?= $(CURDIR)/../lib
include $(XBED_LIB_DIR)/lib.mk

# Cycle-29 option (c) self-allocated witness shim. Opted in here
# (NOT via lib.mk) so the rest of the diag-XBE corpus does not get
# linked with the new code. See lib.mk for rationale.
SRCS           += $(XBED_LIB_DIR)/xbed_self_witness.c

NXDK_DIR       ?= /Users/jbbrack03/XEMU_MacOS/nxdk
include $(NXDK_DIR)/Makefile
