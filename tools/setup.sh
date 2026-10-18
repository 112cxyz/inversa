#!/bin/sh
# Check out FEX, Klepton and OpenComposite at the pinned commits and apply Inversa's patches
# (idempotent).
set -e
cd "$(dirname "$0")/.."
git submodule update --init third_party/FEX third_party/Klepton third_party/OpenOVR
( cd third_party/FEX && git submodule update --init --depth 1 External/fmt External/range-v3 External/rpmalloc \
    External/unordered_dense External/vixl External/xxhash External/zydis External/drm-headers \
    External/Vulkan-Headers External/tracy Source/Common/cpp-optparse )
( cd third_party/OpenOVR && git submodule update --init --depth 1 libs/openxr-sdk libs/glm )
for p in FEX:fex-inversa Klepton:klepton OpenOVR:opencomposite; do
  dir=third_party/${p%%:*} patch=$PWD/patches/${p#*:}.patch
  if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then echo "$dir: patch already applied"
  else git -C "$dir" apply "$patch" && echo "$dir: patched"; fi
done
