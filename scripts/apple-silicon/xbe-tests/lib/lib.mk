# xbed lib include snippet — diag XBE Makefiles include this BEFORE
# pulling in $(NXDK_DIR)/Makefile. It populates SRCS and SHADER_OBJS
# with the shared runtime + capture sources and points the C
# preprocessor at the lib dir so xbed_runtime.c finds vs.inl / ps.inl.
#
# Usage example (mirror/Makefile):
#
#   XBE_TITLE       = mirror
#   GEN_XISO        = $(XBE_TITLE).iso
#   SRCS            = $(CURDIR)/main.c
#   XBED_LIB_DIR   ?= $(CURDIR)/../lib
#   include $(XBED_LIB_DIR)/lib.mk
#   NXDK_DIR       ?= /Users/jbbrack03/XEMU_MacOS/nxdk
#   include $(NXDK_DIR)/Makefile
#
# The two `include`s deliberately split: lib.mk runs against the
# caller's $(CURDIR)/main.c values; nxdk/Makefile then handles
# everything once SRCS / SHADER_OBJS are populated.

XBED_LIB_DIR ?= $(CURDIR)/../lib

SRCS += $(XBED_LIB_DIR)/xbed_runtime.c \
        $(XBED_LIB_DIR)/xbed_capture.c \
        $(XBED_LIB_DIR)/xbed_input_synth.c \
        $(XBED_LIB_DIR)/xbed_texture.c

SHADER_OBJS += $(XBED_LIB_DIR)/vs.inl \
               $(XBED_LIB_DIR)/ps.inl

# Preprocessor needs to find the lib's headers from the test's main.c.
# The lib's own .c files use plain `#include "vs.inl"` (resolved by
# the compiler's including-file-directory rule) and `#include
# "xbed_runtime.h"` (likewise; the .c and .h sit together).
CFLAGS  += -I$(XBED_LIB_DIR)
NXDK_CFLAGS += -I$(XBED_LIB_DIR)
