# Inversa: Linux ARM64 and x86-64 programs on macOS (Klepton + FEX over Inversa's kernel).
#   make setup     submodules + our patches on FEX and Klepton
#   make sysroot   Debian arm64 sysroot for cross-building FEX
#   make fex       build/lib/libFEXInversa.so
#   make           build/Inversa.app (+ fex)
#   make rootfs    Debian guest roots: build/rootfs/x86_64, and the arm64 libraries FEX itself needs
#   make steamrt-libs    the Debian libraries Steam Linux Runtime games expect
#   make opencomposite   build/openvr: OpenVR on OpenXR for Linux ARM64 VR games (see docs/alyx.md)
#   make moltenvk        the official MoltenVK release, for games that don't run on KosmicKrisp yet
#   make notarize        Apple's notary check on build/Inversa.app, ticket stapled
#   make dist            build/dist/Inversa-$(VERSION).zip: the app, lib/, the x86-64 root and OpenComposite
KLEPTON := third_party/Klepton
FEX     := third_party/FEX
OPENOVR := third_party/OpenOVR
BUILD   := $(abspath build)
VERSION := 0.1.2
CC      := clang
VK_INC  ?= $(shell brew --prefix vulkan-headers 2>/dev/null)/include
INC     := -I$(KLEPTON)/runtime -I$(KLEPTON)/runtime/jni -I$(KLEPTON)/runtime/libc -I$(KLEPTON)/runtime/gfx \
           -I$(KLEPTON)/runtime/xr -I$(KLEPTON)/runtime/media -I$(KLEPTON)/runtime/guest -I$(KLEPTON)/runtime/diag \
           -I$(KLEPTON) -I$(VK_INC) -Iloader
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -arch arm64
LDLIBS  := -lz -framework AudioToolbox -framework CoreAudio -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
           -framework IOSurface -framework Security -framework CoreFoundation -framework AVFoundation -framework Foundation
LOADER  := $(addprefix loader/,crash.c deps.c steam.c jit.c glibc/glibc.c glibc/glibc_ext.c glibc/glibc_va.S \
           tls/mvr_tls.c tls/mvr_tls.S tls/mvr_enter.S)
SRCS    := host/inversa.c host/cpu.c kernel/kernel.c kernel/libc.c kernel/memory.c kernel/files.c kernel/threads.c kernel/linux.c kernel/ioctl.c kernel/exec.c

# Developer ID + the cross-architecture entitlement (memory below 4 GB, 4K pages) need its
# provisioning profile; without them the bundle is signed ad hoc and runs with 16K pages
SIGN_ID ?= $(shell security find-identity -v -p codesigning 2>/dev/null | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')
PROFILE ?= $(HOME)/Downloads/Mac.provisionprofile
BUNDLE_ID ?= dev.moltenvr.xarch-probe
NOTARY_PROFILE ?= moltenvr

KL_SRCS := $(shell $(MAKE) -s -C $(KLEPTON) -f Makefile -f $(CURDIR)/loader/print.mk print-RUNTIME 2>/dev/null)
KL_OBJS := $(patsubst %,$(BUILD)/klepton/%.o,$(KL_SRCS))
FEXLIB  := $(BUILD)/fex/Source/Tools/FEXInterpreter/libFEXInversa.so

.PHONY: all setup sysroot fex rootfs steamrt-libs opencomposite moltenvk notarize dist tests clean
all: $(BUILD)/Inversa.app $(BUILD)/lib/libFEXInversa.so

setup:
	tools/setup.sh

sysroot: $(BUILD)/sysroot/usr/include/stdio.h
$(BUILD)/sysroot/usr/include/stdio.h:
	python3 tools/fetch_rootfs.py arm64 $(BUILD)/sysroot libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev

$(BUILD)/fex/build.ninja: $(BUILD)/sysroot/usr/include/stdio.h
	cmake -S $(FEX) -B $(BUILD)/fex -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/tools/aarch64-linux.cmake \
	  -DCMAKE_BUILD_TYPE=Release -DTUNE_CPU=apple-m1 -DBUILD_FEXCONFIG=OFF -DENABLE_LTO=OFF \
	  -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF -DENABLE_CCACHE=OFF -DENABLE_OFFLINE_TELEMETRY=OFF -DBUILD_TESTING=OFF \
	  -DCMAKE_CXX_FLAGS=-DFEX_INVERSA=1 -DCMAKE_C_FLAGS=-DFEX_INVERSA=1

fex: $(BUILD)/fex/build.ninja
	ninja -C $(BUILD)/fex FEXInversa

$(FEXLIB): fex
$(BUILD)/lib/libFEXInversa.so: $(FEXLIB)
	@mkdir -p $(BUILD)/lib
	cp $(FEXLIB) $@

$(BUILD)/libklepton.a: $(KL_OBJS)
	@rm -f $@ && ar rcs $@ $^

$(BUILD)/klepton/%.o: $(KLEPTON)/%
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# guest code treats x18 as a scratch register; XNU only preserves it for binaries built for macOS 11
$(BUILD)/inversa: $(SRCS) $(LOADER) $(wildcard host/*.h kernel/*.h loader/*.h loader/*/*.h) $(BUILD)/libklepton.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRCS) $(LOADER) $(BUILD)/libklepton.a $(LDLIBS) -Wl,-platform_version,macos,11.0,11.0

# the bundle's executable is a small launcher that starts inversa-core in a 4K-page process
$(BUILD)/inversa-launch: host/launch.c host/entitlement.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -arch arm64 -o $@ $< -framework Security -framework CoreFoundation

$(BUILD)/Inversa.icns: tools/make_icon.swift
	@mkdir -p $(BUILD)
	rm -rf $(BUILD)/Inversa.iconset
	swift tools/make_icon.swift $(BUILD)/Inversa.iconset
	iconutil -c icns -o $@ $(BUILD)/Inversa.iconset

# both executables carry the entitlement: the launcher to ask for 4K pages, the core to run in them
# the official MoltenVK release goes in Frameworks: games whose renderer needs it (Alyx) get it
# without a build of their own (make moltenvk)
MVK_DYLIB := $(KLEPTON)/vendor-moltenvk/out/macos/libMoltenVK.dylib

$(BUILD)/Inversa.app: $(BUILD)/inversa $(BUILD)/inversa-launch $(BUILD)/Inversa.icns host/Info.plist host/entitlements.plist Makefile
	rm -rf $@ && mkdir -p $@/Contents/MacOS $@/Contents/Resources
	cp $(BUILD)/inversa-launch $@/Contents/MacOS/inversa
	cp $(BUILD)/inversa $@/Contents/MacOS/inversa-core
	cp $(BUILD)/Inversa.icns $@/Contents/Resources/
	if [ -f $(MVK_DYLIB) ]; then mkdir -p $@/Contents/Frameworks && cp $(MVK_DYLIB) $@/Contents/Frameworks/ \
	  && cp $(KLEPTON)/vendor-moltenvk/dist/MoltenVK/LICENSE $@/Contents/Resources/MoltenVK-LICENSE; fi
	sed -e 's/@ID@/$(BUNDLE_ID)/' -e 's/@VERSION@/$(VERSION)/' host/Info.plist > $@/Contents/Info.plist
	if [ -n "$(SIGN_ID)" ] && [ -f "$(PROFILE)" ]; then \
	  cp "$(PROFILE)" $@/Contents/embedded.provisionprofile; \
	  if [ -f $@/Contents/Frameworks/libMoltenVK.dylib ]; then codesign -f -s "$(SIGN_ID)" -o runtime --timestamp $@/Contents/Frameworks/libMoltenVK.dylib; fi; \
	  codesign -f -s "$(SIGN_ID)" -o runtime --timestamp -i $(BUNDLE_ID) --entitlements host/entitlements.plist $@/Contents/MacOS/inversa-core; \
	  codesign -f -s "$(SIGN_ID)" -o runtime --timestamp --entitlements host/entitlements.plist $@; \
	else \
	  echo "note: no Developer ID + provisioning profile: signing ad hoc (16K pages, no low memory)"; \
	  if [ -f $@/Contents/Frameworks/libMoltenVK.dylib ]; then codesign -f -s - $@/Contents/Frameworks/libMoltenVK.dylib; fi; \
	  codesign -f -s - $@/Contents/MacOS/inversa-core; \
	  codesign -f -s - $@; \
	fi

# OpenComposite: the game's OpenVR calls turned into OpenXR for MoltenVR's runtime. Its own
# sysroot, with the Vulkan and GL headers it builds against
VRSYSROOT := $(BUILD)/sysroot-vr
$(VRSYSROOT)/usr/include/vulkan/vulkan.h:
	python3 tools/fetch_rootfs.py arm64 $(VRSYSROOT) libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev \
	  libvulkan-dev libgl-dev libglx-dev libopengl-dev libegl-dev libgles-dev libglvnd-dev

$(BUILD)/opencomposite/build.ninja: $(VRSYSROOT)/usr/include/vulkan/vulkan.h
	cmake -S $(OPENOVR) -B $(BUILD)/opencomposite -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/tools/aarch64-linux.cmake \
	  -DCMAKE_SYSROOT=$(VRSYSROOT) -DCMAKE_BUILD_TYPE=Release

opencomposite: $(BUILD)/opencomposite/build.ninja
	ninja -C $(BUILD)/opencomposite
	@mkdir -p $(BUILD)/openvr/bin/linuxarm64
	cp $(BUILD)/opencomposite/bin/linux64/vrclient.so $(BUILD)/openvr/bin/linuxarm64/

# Klepton fetches it, checks its sha256 and stages it in vendor-moltenvk/out/macos
moltenvk:
	$(MAKE) -C $(KLEPTON) mvk

rootfs:
	python3 tools/fetch_rootfs.py amd64 $(BUILD)/rootfs/x86_64 bash coreutils
	python3 tools/fetch_rootfs.py arm64 $(BUILD)/lib/arm64-root libstdc++6 libgcc-s1

# what a Steam Linux Runtime game expects but doesn't ship (ICU 67 for Alyx's V8 only exists in bullseye)
steamrt-libs:
	python3 tools/fetch_rootfs.py arm64 $(BUILD)/lib/arm64-root libstdc++6 libgcc-s1 libssl3t64 libpng16-16t64 \
	  zlib1g libzstd1 libbz2-1.0 libbrotli1 libexpat1 libglib2.0-0t64 libfribidi0 libogg0 libuuid1 libpcre2-8-0 \
	  libffi8 libmount1 libblkid1 libselinux1 libspirv-tools2025.1 libatomic1
	python3 tools/fetch_rootfs.py --suite bullseye --nodeps arm64 $(BUILD)/lib/arm64-root libicu67

notarize:
	ditto -c -k --keepParent $(BUILD)/Inversa.app $(BUILD)/notarize.zip
	xcrun notarytool submit $(BUILD)/notarize.zip --keychain-profile $(NOTARY_PROFILE) --wait
	rm -f $(BUILD)/notarize.zip
	xcrun stapler staple $(BUILD)/Inversa.app

# Inversa finds lib/ beside its app and the x86-64 root at lib/../rootfs, so this folder runs from
# anywhere. It's the release download, and what MoltenVR bundles
DIST := $(BUILD)/dist/Inversa
dist:
	@test -f $(BUILD)/lib/libFEXInversa.so -a -d $(BUILD)/rootfs/x86_64/usr -a -d $(BUILD)/lib/arm64-root/usr \
	  || { echo "dist needs make fex rootfs steamrt-libs first"; exit 1; }
	@xcrun stapler validate -q $(BUILD)/Inversa.app || echo "warning: Inversa.app isn't notarized (make notarize)"
	rm -rf $(BUILD)/dist && mkdir -p $(DIST)/lib $(DIST)/rootfs
	ditto $(BUILD)/Inversa.app $(DIST)/Inversa.app
	ditto $(BUILD)/lib/libFEXInversa.so $(DIST)/lib/libFEXInversa.so
	ditto $(BUILD)/lib/arm64-root $(DIST)/lib/arm64-root
	ditto $(BUILD)/rootfs/x86_64 $(DIST)/rootfs/x86_64
	if [ -d $(BUILD)/openvr ]; then ditto $(BUILD)/openvr $(DIST)/openvr; fi
	mkdir -p $(DIST)/licenses
	cp licenses/README.md licenses/cereal-LICENSE licenses/glslang-LICENSE.txt $(DIST)/licenses/
	cp LICENSE $(DIST)/licenses/Inversa-LICENSE
	cp third_party/FEX/LICENSE $(DIST)/licenses/FEX-LICENSE
	cp $(KLEPTON)/LICENSE $(DIST)/licenses/Klepton-LICENSE
	for f in LICENSE.txt LICENSE_OPENVR.txt LICENSE_INIH.txt; do cp third_party/OpenOVR/$$f $(DIST)/licenses/OpenComposite-$$f; done
	cp $(KLEPTON)/vendor-moltenvk/dist/MoltenVK/LICENSE $(DIST)/licenses/MoltenVK-LICENSE
	cd $(BUILD)/dist && ditto -c -k --keepParent Inversa Inversa-$(VERSION).zip
	shasum -a 256 $(BUILD)/dist/Inversa-$(VERSION).zip

LLVM ?= $(shell brew --prefix llvm 2>/dev/null)/bin
tests: tests/build/hello_x64
tests/build/hello_x64: tests/hello_x64.S
	@mkdir -p tests/build
	$(LLVM)/clang --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=$(shell brew --prefix lld)/bin/ld.lld -Wl,-e,_start $< -o $@

clean:
	rm -rf $(BUILD)/inversa $(BUILD)/Inversa.app $(BUILD)/klepton $(BUILD)/libklepton.a
