# Inversa: Linux ARM64 and x86-64 programs on macOS (Klepton + FEX over Inversa's kernel).
#   make setup     submodules + our patches on FEX and Klepton
#   make sysroot   Debian arm64 sysroot for cross-building FEX
#   make fex       build/lib/libFEXInversa.so
#   make           build/Inversa.app (+ fex)
#   make rootfs    Debian guest roots: build/rootfs/x86_64, and the arm64 libraries FEX itself needs
KLEPTON := third_party/Klepton
FEX     := third_party/FEX
BUILD   := $(abspath build)
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
SRCS    := host/inversa.c host/cpu.c kernel/kernel.c kernel/libc.c kernel/memory.c kernel/files.c kernel/threads.c kernel/linux.c

# Developer ID + the cross-architecture entitlement (memory below 4 GB, 4K pages) need its
# provisioning profile; without them the bundle is signed ad hoc and runs with 16K pages
SIGN_ID ?= $(shell security find-identity -v -p codesigning 2>/dev/null | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')
PROFILE ?= $(HOME)/Downloads/Mac.provisionprofile
BUNDLE_ID ?= dev.moltenvr.xarch-probe

KL_SRCS := $(shell $(MAKE) -s -C $(KLEPTON) -f Makefile -f $(CURDIR)/loader/print.mk print-RUNTIME 2>/dev/null)
KL_OBJS := $(patsubst %,$(BUILD)/klepton/%.o,$(KL_SRCS))
FEXLIB  := $(BUILD)/fex/Source/Tools/FEXInterpreter/libFEXInversa.so

.PHONY: all setup sysroot fex rootfs tests clean
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
$(BUILD)/inversa: $(SRCS) $(LOADER) $(wildcard kernel/*.h loader/*.h loader/*/*.h) $(BUILD)/libklepton.a
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRCS) $(LOADER) $(BUILD)/libklepton.a $(LDLIBS) -Wl,-platform_version,macos,11.0,11.0

$(BUILD)/Inversa.app: $(BUILD)/inversa host/Info.plist host/entitlements.plist
	rm -rf $@ && mkdir -p $@/Contents/MacOS
	cp $(BUILD)/inversa $@/Contents/MacOS/inversa
	sed 's/@ID@/$(BUNDLE_ID)/' host/Info.plist > $@/Contents/Info.plist
	if [ -n "$(SIGN_ID)" ] && [ -f "$(PROFILE)" ]; then \
	  cp "$(PROFILE)" $@/Contents/embedded.provisionprofile; \
	  codesign -f -s "$(SIGN_ID)" --entitlements host/entitlements.plist $@; \
	else \
	  echo "note: no Developer ID + provisioning profile: signing ad hoc (16K pages, no low memory)"; \
	  codesign -f -s - $@; \
	fi

rootfs:
	python3 tools/fetch_rootfs.py amd64 $(BUILD)/rootfs/x86_64 bash coreutils
	python3 tools/fetch_rootfs.py arm64 $(BUILD)/lib/arm64-root libstdc++6 libgcc-s1

LLVM ?= $(shell brew --prefix llvm 2>/dev/null)/bin
tests: tests/build/hello_x64
tests/build/hello_x64: tests/hello_x64.S
	@mkdir -p tests/build
	$(LLVM)/clang --target=x86_64-linux-gnu -nostdlib -static -fuse-ld=$(shell brew --prefix lld)/bin/ld.lld -Wl,-e,_start $< -o $@

clean:
	rm -rf $(BUILD)/inversa $(BUILD)/Inversa.app $(BUILD)/klepton $(BUILD)/libklepton.a
