XBE_TITLE = oracle-agent
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c $(CURDIR)/protocol.c $(CURDIR)/commands.c \
       $(CURDIR)/controller.c $(CURDIR)/tier2.c
NXDK_DIR ?= /Users/jbbrack03/XEMU_MacOS/nxdk

# Bring in lwIP (TCP listener) — same flag the httpd sample uses.
NXDK_NET = y

include $(NXDK_DIR)/Makefile
