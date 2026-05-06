XBE_TITLE = oracle-agent
GEN_XISO = $(XBE_TITLE).iso
SRCS = $(CURDIR)/main.c
NXDK_DIR ?= /Users/jbbrack03/XEMU_MacOS/nxdk

# Bring in lwIP (TCP listener) — this is what the httpd sample also pulls in
NXDK_NET = y

include $(NXDK_DIR)/Makefile
