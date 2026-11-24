# Brother HL-1210W series - native CUPS raster driver for macOS
#
#   make            build the filter (universal arm64 + x86_64) and the PPD
#   make check      run the self-tests
#   make fuzz       round-trip random pages through encoder and decoder
#   make pkg        build an installer package in dist/
#   sudo make install / sudo make uninstall
#
# Optional variables:
#   BROTHER_PKG        Brother's own driver package; its halftone screens and
#                      printer icon are reused when it is available
#   BROTHER_LUT_DIR    folder holding the BRML halftone screens ("Brother LUT")
#   BROTHER_ICON       printer icon (.icns) to install with the driver
#   CODESIGN_IDENTITY  signing identity ("-" = ad hoc, the default)
#   INSTALLER_IDENTITY "Developer ID Installer: ..." to sign the package

VERSION        := 1.0
IDENTIFIER     := org.hl1210w.native-driver
MIN_MACOS      := 11.0
ARCHS          := arm64 x86_64

VENDOR_DIR     := /Library/Printers/HL1210W-Native
FILTER_PATH    := $(VENDOR_DIR)/Filter/rastertohl1210w
ICON_PATH      := $(VENDOR_DIR)/Icons/HL1210W.icns
PPD_DIR        := /Library/Printers/PPDs/Contents/Resources
PPD_NAME       := Brother HL-1210W series native.gz

# Fixed, relative output folders: "override" keeps a stray BUILD= or DIST= on
# the command line from ever pointing the clean-up rules somewhere else.
override BUILD := build
override DIST  := dist
DESTDIR        ?=

CODESIGN_IDENTITY  ?= -
INSTALLER_IDENTITY ?=

# ---- Brother assets (optional) ------------------------------------------
# The halftone screens and the printer icon are taken from a Brother driver
# you already have: the installed one if present, else the package next to
# this folder.  Without either, generic screens and the system icon are used.
BROTHER_PKG     ?= $(firstword $(wildcard ../Brother_PrinterDrivers_MonochromeLaser/*.pkg))
INSTALLED_LUT   := /Library/Printers/Brother/Brother LUT
INSTALLED_ICON  := /Library/Printers/Brother/Icons/BRLaser1210W.icns
PKG_PAYLOAD     := $(BUILD)/brother-pkg/Payload/Library/Printers/Brother

ifeq ($(origin BROTHER_LUT_DIR),undefined)
  ifeq ($(shell test -f "$(INSTALLED_LUT)/0600-G12.bin" && echo yes),yes)
    BROTHER_LUT_DIR := $(INSTALLED_LUT)
  else ifneq ($(strip $(BROTHER_PKG)),)
    BROTHER_LUT_DIR := $(PKG_PAYLOAD)/Brother LUT
    NEED_PKG_EXPAND := 1
  else
    BROTHER_LUT_DIR :=
  endif
endif

ifeq ($(origin BROTHER_ICON),undefined)
  ifeq ($(shell test -f "$(INSTALLED_ICON)" && echo yes),yes)
    BROTHER_ICON := $(INSTALLED_ICON)
  else ifneq ($(strip $(BROTHER_PKG)),)
    BROTHER_ICON := $(PKG_PAYLOAD)/Icons/BRLaser1210W.icns
    NEED_PKG_EXPAND := 1
  else
    BROTHER_ICON :=
  endif
endif

# ---- Toolchain -----------------------------------------------------------
CC        := xcrun clang
ARCHFLAGS := $(foreach a,$(ARCHS),-arch $(a))
CFLAGS    := -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
             -Wno-sign-conversion -fstack-protector-strong \
             -mmacosx-version-min=$(MIN_MACOS) \
             -DHL1210W_VERSION='"$(VERSION)"' -I$(BUILD) -Isrc
LDFLAGS   := -mmacosx-version-min=$(MIN_MACOS) -Wl,-dead_strip

CORE_SRCS := src/brcomp.c src/halftone.c src/smooth.c src/page.c src/job.c
HEADERS   := $(wildcard src/*.h) $(BUILD)/halftone_tables.h

FILTER    := $(BUILD)/rastertohl1210w
TESTTOOL  := $(BUILD)/hl1210w-testtool
PPD       := $(BUILD)/HL1210W.ppd
PKG       := $(DIST)/HL1210W-Native-Driver-$(VERSION).pkg

.PHONY: all check fuzz pkg install uninstall clean

all: $(FILTER) $(PPD) $(TESTTOOL)

$(BUILD):
	@mkdir -p $(BUILD)

# Expand Brother's package once, only when its assets are not installed.
$(BUILD)/brother-pkg/.expanded: | $(BUILD)
ifdef NEED_PKG_EXPAND
	rm -rf $(BUILD)/brother-pkg
	pkgutil --expand-full "$(BROTHER_PKG)" $(BUILD)/brother-pkg-tmp
	mv $(BUILD)/brother-pkg-tmp/*.pkg $(BUILD)/brother-pkg
	rm -rf $(BUILD)/brother-pkg-tmp
else
	@mkdir -p $(BUILD)/brother-pkg
endif
	@touch $@

$(BUILD)/halftone_tables.h: tools/gen_halftone.py $(BUILD)/brother-pkg/.expanded
	python3 tools/gen_halftone.py "$(BROTHER_LUT_DIR)" > $@.tmp
	mv $@.tmp $@

$(BUILD)/Info.plist: packaging/Info.plist.in Makefile | $(BUILD)
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@IDENTIFIER@|$(IDENTIFIER)|g' \
	    -e 's|@MIN_MACOS@|$(MIN_MACOS)|g' $< > $@

$(FILTER): src/rastertohl1210w.c $(CORE_SRCS) $(HEADERS) $(BUILD)/Info.plist
	$(CC) $(ARCHFLAGS) $(CFLAGS) $(LDFLAGS) \
	    -Wl,-sectcreate,__TEXT,__info_plist,$(BUILD)/Info.plist \
	    -o $@ src/rastertohl1210w.c $(CORE_SRCS) -lcups
	codesign --force --options runtime \
	    $(if $(filter-out -,$(CODESIGN_IDENTITY)),--timestamp) \
	    --identifier $(IDENTIFIER).rastertohl1210w \
	    --sign "$(CODESIGN_IDENTITY)" $@

$(TESTTOOL): tests/testtool.c $(CORE_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ tests/testtool.c $(CORE_SRCS)

$(BUILD)/HL1210W.icns: $(BUILD)/brother-pkg/.expanded
	@if [ -n "$(BROTHER_ICON)" ] && [ -f "$(BROTHER_ICON)" ]; then \
	    cp "$(BROTHER_ICON)" $@; \
	else \
	    rm -f $@; echo "note: no printer icon found; macOS will show its generic one"; \
	fi

$(PPD): ppd/HL1210W.ppd.in Makefile $(BUILD)/HL1210W.icns
	@if [ -f $(BUILD)/HL1210W.icns ]; then \
	    icon='*APPrinterIconPath: "$(ICON_PATH)"'; \
	else \
	    icon='*% (no printer icon installed)'; \
	fi; \
	sed -e 's|@VERSION@|$(VERSION)|g' -e 's|@FILTER_PATH@|$(FILTER_PATH)|g' \
	    -e "s|@ICON_LINE@|$$icon|" $< > $@

check: all
	python3 tests/run_tests.py $(BUILD)

# Random pages through the encoder and back through the independent decoder.
fuzz: all
	python3 tests/fuzz_roundtrip.py $(BUILD)

# ---- Staging, packaging and installation ---------------------------------
ROOT := $(BUILD)/root

$(ROOT)/.staged: $(FILTER) $(PPD) packaging/uninstall.sh
	rm -rf $(ROOT)
	mkdir -p "$(ROOT)$(VENDOR_DIR)/Filter" "$(ROOT)$(PPD_DIR)"
	install -m 0755 $(FILTER) "$(ROOT)$(FILTER_PATH)"
	install -m 0755 packaging/uninstall.sh "$(ROOT)$(VENDOR_DIR)/uninstall.sh"
	@if [ -f $(BUILD)/HL1210W.icns ]; then \
	    mkdir -p "$(ROOT)$(VENDOR_DIR)/Icons"; \
	    install -m 0644 $(BUILD)/HL1210W.icns "$(ROOT)$(ICON_PATH)"; \
	fi
	gzip -9 -n -c $(PPD) > "$(ROOT)$(PPD_DIR)/$(PPD_NAME)"
	chmod 0644 "$(ROOT)$(PPD_DIR)/$(PPD_NAME)"
	@touch $@

pkg: $(PKG)

# pkgbuild writes the package; its payload and bill of materials are then
# rebuilt from the same staged files without the "._*" AppleDouble entries
# that appear when the build environment tags files with extended attributes.
# Each run works in a fresh folder under build/.
$(PKG): $(ROOT)/.staged packaging/PackageInfo
	@mkdir -p $(DIST)
	@rm -f $(ROOT)/.staged
	@set -e; work=$$(mktemp -d "$(BUILD)/pkg.XXXXXX"); \
	pkgbuild --root $(ROOT) --identifier $(IDENTIFIER) --version $(VERSION) \
	    --ownership recommended --install-location / \
	    --info packaging/PackageInfo "$$work/component.pkg"; \
	pkgutil --expand "$$work/component.pkg" "$$work/expanded"; \
	(cd $(ROOT) && find . -print | sort | \
	    COPYFILE_DISABLE=1 cpio -o --quiet --format odc -R 0:0 | gzip -9 -n) \
	    > "$$work/expanded/Payload"; \
	lsbom "$$work/expanded/Bom" | grep -v '/\._' > "$$work/files.txt"; \
	mkbom -i "$$work/files.txt" "$$work/expanded/Bom"; \
	count=$$(wc -l < "$$work/files.txt" | tr -d ' '); \
	sed -i '' -e "s/numberOfFiles=\"[0-9]*\"/numberOfFiles=\"$$count\"/" \
	    "$$work/expanded/PackageInfo"; \
	pkgutil --flatten "$$work/expanded" "$$work/unsigned.pkg"; \
	if [ -n "$(INSTALLER_IDENTITY)" ]; then \
	    productsign --sign "$(INSTALLER_IDENTITY)" "$$work/unsigned.pkg" "$@"; \
	else \
	    cp "$$work/unsigned.pkg" "$@"; \
	fi
	@touch $(ROOT)/.staged
	@echo "Built $@"

install: $(ROOT)/.staged
	install -d -m 0755 "$(DESTDIR)$(VENDOR_DIR)/Filter" "$(DESTDIR)$(PPD_DIR)"
	install -m 0755 "$(ROOT)$(FILTER_PATH)" "$(DESTDIR)$(FILTER_PATH)"
	install -m 0755 "$(ROOT)$(VENDOR_DIR)/uninstall.sh" "$(DESTDIR)$(VENDOR_DIR)/uninstall.sh"
	@if [ -f "$(ROOT)$(ICON_PATH)" ]; then \
	    install -d -m 0755 "$(DESTDIR)$(VENDOR_DIR)/Icons"; \
	    install -m 0644 "$(ROOT)$(ICON_PATH)" "$(DESTDIR)$(ICON_PATH)"; \
	fi
	install -m 0644 "$(ROOT)$(PPD_DIR)/$(PPD_NAME)" "$(DESTDIR)$(PPD_DIR)/$(PPD_NAME)"
	@echo "Installed. Add the printer in System Settings > Printers & Scanners,"
	@echo "or run: packaging/add-printer.sh"

uninstall:
	sh packaging/uninstall.sh

clean:
	rm -rf $(BUILD) $(DIST)
