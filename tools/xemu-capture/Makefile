# xemu-capture — build the Swift CLI, bundle it as a proper .app, and
# ad-hoc code-sign so macOS TCC tracks Camera permission by stable
# bundle ID com.xemu-macos.capture.
#
# Targets:
#   make            build + bundle + sign (default)
#   make build      swift build only
#   make bundle     wrap the swift binary in an .app
#   make sign       ad-hoc codesign with --identifier com.xemu-macos.capture
#   make install    symlink dist/xemu-capture.app/Contents/MacOS/xemu-capture
#                   into ../../scripts/apple-silicon/bin/xemu-capture so it's
#                   on the project's tool path.
#   make clean      remove .build/ and dist/

APP_NAME      := xemu-capture
APP_BUNDLE_ID := com.xemu-macos.capture
APP_DIR       := dist/$(APP_NAME).app
BIN_PATH      := $(APP_DIR)/Contents/MacOS/$(APP_NAME)
INFO_PLIST    := $(APP_DIR)/Contents/Info.plist
WRAPPER_DIR   := ../../scripts/apple-silicon/bin
WRAPPER_PATH  := $(WRAPPER_DIR)/xemu-capture

SWIFT         ?= xcrun swift
CODESIGN      ?= /usr/bin/codesign

.PHONY: all build bundle sign install clean test

all: build bundle sign
	@echo
	@echo "[xemu-capture] Built: $(APP_DIR)"
	@echo "[xemu-capture] Run:   ./$(BIN_PATH) list"
	@echo
	@echo "  First run that hits the camera (snapshot, probe, serve) will"
	@echo "  prompt for Camera permission. Once granted (per bundle ID"
	@echo "  $(APP_BUNDLE_ID)) it persists across sessions."

build:
	$(SWIFT) build -c release

bundle: build
	@mkdir -p $(APP_DIR)/Contents/MacOS
	@mkdir -p $(APP_DIR)/Contents/Resources
	@cp .build/release/$(APP_NAME) $(BIN_PATH)
	@cp Info.plist $(INFO_PLIST)
	@touch $(APP_DIR)/Contents/PkgInfo
	@printf 'APPL????' > $(APP_DIR)/Contents/PkgInfo

sign:
	# Ad-hoc signing without --options runtime. The Hardened Runtime
	# requires explicit resource entitlements (e.g.
	# com.apple.security.device.camera = true) per Apple TN3127; without
	# them macOS may deny Camera access even after a TCC grant. For
	# this local ad-hoc tool we use unhardened ad-hoc signing instead;
	# TCC tracks Camera permission via the bundle ID + cdhash.
	#
	# Caveat per Apple TN3127: ad-hoc signatures use a cdhash-only
	# designated requirement, so rebuilding the binary changes the
	# cdhash and may re-trigger the macOS Camera prompt. The
	# checked-in prebuilt $(APP_DIR) keeps permission while unchanged;
	# rebuilders should expect a one-time regrant.
	$(CODESIGN) --force --deep \
		--sign - \
		--identifier $(APP_BUNDLE_ID) \
		$(APP_DIR)
	@echo "[xemu-capture] Verifying signature..."
	$(CODESIGN) --verify --verbose=2 $(APP_DIR)
	$(CODESIGN) --display --verbose=2 $(APP_DIR) 2>&1 | grep -E '(Identifier|TeamIdentifier|Authority)' | sed 's/^/  /'

install: all
	@mkdir -p $(WRAPPER_DIR)
	@rm -f $(WRAPPER_PATH)
	# Wrapper sits at xemu-fork/scripts/apple-silicon/bin/xemu-capture
	# and the .app at xemu-fork/tools/xemu-capture/dist/.../xemu-capture.
	# So the relative jump is ../../../tools/... (three levels up:
	# bin/ -> apple-silicon/ -> scripts/ -> xemu-fork/).
	@printf '#!/bin/sh\nexec "$$(dirname "$$(realpath "$$0")")"/../../../tools/$(APP_NAME)/$(BIN_PATH) "$$@"\n' > $(WRAPPER_PATH)
	@chmod +x $(WRAPPER_PATH)
	@echo "[xemu-capture] Installed wrapper at $(WRAPPER_PATH)"

test: all
	@echo "[xemu-capture] Smoke: list devices"
	@./$(BIN_PATH) list

clean:
	rm -rf .build dist
