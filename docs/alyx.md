# Half-Life: Alyx

Valve ships a Linux ARM64 build of Half-Life: Alyx (`game/bin/linuxsteamrtarm64`), made for the Steam Frame. Inversa runs it natively on an Apple silicon Mac and sends the frames to your headset through [MoltenVR](https://moltenvr.com).

The game talks OpenVR. [OpenComposite](https://gitlab.com/znixian/OpenOVR) (armax's fork, in `third_party/OpenOVR`) turns that into OpenXR, and MoltenVR's OpenXR runtime takes it from there. Steam calls go to the Mac Steam client.

## What you need

- An Apple silicon Mac, with MoltenVR installed and your headset connecting to it as usual
- Steam for Mac, signed in to an account that owns Half-Life: Alyx
- About 70 GB of free space
- Homebrew, and the tools from the [README](../README.md#building)

## Build

From the top of the repo:

```
tools/setup.sh
make sysroot fex all
make steamrt-libs     # Debian libraries the game expects but doesn't ship
make opencomposite    # build/openvr: OpenVR for the game, on OpenXR
make moltenvk         # the official MoltenVK release
```

ARM64 games don't need FEX, but `make all` builds it along with the app, and it takes a while the first time.

## Download the Linux build

The Mac Steam client won't download another platform's depot, so use steamcmd:

```
brew install --cask steamcmd
steamcmd +@sSteamCmdForcePlatformType linux +force_install_dir ~/Games/alyx \
  +login <your Steam user> +app_update 546560 validate +quit
```
> [!TIP]
> You can download the Linux version using depotdownloader if you prefer it instead of steamcmd:
> ```
> depotdownloader -app 546560 -os linux -dir "~/Games/alyx"
> ```
> Reminder that you need to provide your own login methods (ex. -qr, -no-mobile)! 
> I still recommend running the `fix_depot_links.py` script.

steamcmd on macOS writes the depot's symlinks as empty files. Put them back:

```
python3 tools/fix_depot_links.py ~/Games/alyx/game/bin/linuxsteamrtarm64
```

## Play

1. Start Steam and sign in.
2. Open MoltenVR and connect your headset.
3. Run:

```
tools/alyx.sh ~/Games/alyx
```

Anything after the folder is passed to the game, for example `+vr_fidelity_level_auto 0 +vr_fidelity_level 4` to fix the render resolution.

Quit from the game's menu when you can. Alyx writes its Vulkan pipeline cache on the way out, and the next start stutters less for it.

## What `tools/alyx.sh` sets up

| | |
|---|---|
| `SteamAppId=546560` | Inversa connects the game's `steam_api` to the Mac Steam client |
| `VR_OVERRIDE=build/openvr` | OpenComposite as the OpenVR runtime, without touching `~/.config/openvr` |
| `XR_RUNTIME_JSON=host/openxr-moltenvr.json` | MoltenVR as the OpenXR runtime (the installed app's, or `MVR_OPENXR_RUNTIME`) |
| `OC_SHARED_FRUSTUM=1` | the game renders one frustum covering both eyes, and each eye is cropped out of it on submit |
| `INVERSA_VULKAN_DRIVER=moltenvk` | MoltenVK, from `make moltenvk` |

`OC_SHARED_FRUSTUM` is there because the headset's eyes are asymmetric: Alyx sizes its render target for both eyes' fields together but draws each eye at the eye's own shape, and without the crop part of every frame is never drawn. The approach is the one [VeRantyx](https://github.com/armax9/VeRantyx) uses.

## Known issues

- The renderer doesn't start on KosmicKrisp or on Homebrew's MoltenVK build, only on the official MoltenVK release.
- On an M4 MacBook Air the menu runs at 72 fps for the first seconds, then around 45 to 56. The CPU and GPU are both mostly idle, so the limit is a stall somewhere rather than raw speed; it hasn't been found yet.
- There's a black patch in the lower right of the right eye.
- Only the main menu has been tested, without controllers.

## When it goes wrong

The game's output goes to the terminal. `tools/safe_run.py` (see the README) runs it with a time limit and keeps a log. MoltenVR's runtime logs to `/tmp/oxr_native.log`, including the game's frame rate every two seconds (`perf: app`).

To profile, `kill -USR1 <pid>` writes the guest image map to `/tmp/mvr-images-<pid>.txt`, which names the guest frames in a `sample` report.
