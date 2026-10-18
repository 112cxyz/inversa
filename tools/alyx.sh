#!/bin/sh
# Half-Life: Alyx's Linux ARM64 build under Inversa, in VR through MoltenVR (see docs/alyx.md).
#   tools/alyx.sh <Alyx install dir> [game args...]
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
alyx="$(cd "$1" && pwd)"; shift
bin="$alyx/game/bin/linuxsteamrtarm64"
[ -x "$bin/hlvr" ] || { echo "no $bin/hlvr: is this the Linux depot?" >&2; exit 1; }
[ -f "$root/build/openvr/bin/linuxarm64/vrclient.so" ] || { echo "run make opencomposite first" >&2; exit 1; }

export SteamAppId=546560
export VR_OVERRIDE="$root/build/openvr"                    # OpenComposite as the OpenVR runtime
export XR_RUNTIME_JSON="$root/host/openxr-moltenvr.json"   # and MoltenVR as the OpenXR one
export OC_SHARED_FRUSTUM=1                                  # one frustum for both eyes, cropped per eye
export MVR_LIBPATH="$bin:$alyx/game/hlvr/bin/linuxsteamrtarm64:$root/build/lib/arm64-root/usr/lib/aarch64-linux-gnu"
# Alyx's renderer doesn't start on KosmicKrisp yet, or on Homebrew's MoltenVK; the official
# release (make moltenvk) works
export INVERSA_VULKAN_DRIVER="${INVERSA_VULKAN_DRIVER:-moltenvk}"
export KL_MVK_DIR="${KL_MVK_DIR:-$root/third_party/Klepton/vendor-moltenvk/out/macos}"
[ -f "$KL_MVK_DIR/libMoltenVK.dylib" ] || { echo "run make moltenvk first" >&2; exit 1; }

cd "$alyx/game"
exec "$root/build/Inversa.app/Contents/MacOS/inversa" run "$bin/hlvr" -vr -vulkan -nowindow -novid "$@"
