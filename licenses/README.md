# Licences

Inversa is free software under the GNU General Public License, version 3 (`Inversa-LICENSE`).
Its source, including every release tag, is at <https://github.com/112cxyz/inversa>.

A release also carries these, each under its own licence:

| Part | Where | Licence | Source |
|---|---|---|---|
| FEX | `lib/libFEXInversa.so` | MIT (`FEX-LICENSE`) | <https://github.com/FEX-Emu/FEX> |
| Klepton | linked into `Inversa.app` | MIT (`Klepton-LICENSE`) | <https://github.com/shinyquagsire23/Klepton> |
| OpenComposite | `openvr/` | GPL-3.0 (`OpenComposite-LICENSE.txt`, with `OpenComposite-LICENSE_OPENVR.txt` and `OpenComposite-LICENSE_INIH.txt` for the parts it includes) | <https://gitlab.com/armax9/OpenOVR> |
| MoltenVK 1.4.2 | `Inversa.app/Contents/Frameworks` | Apache-2.0 (`MoltenVK-LICENSE`) | <https://github.com/KhronosGroup/MoltenVK> |
| SPIRV-Cross, SPIRV-Tools (in MoltenVK) | | Apache-2.0 (`MoltenVK-LICENSE`) | <https://github.com/KhronosGroup/SPIRV-Cross>, <https://github.com/KhronosGroup/SPIRV-Tools> |
| glslang (in MoltenVK) | | BSD-3-Clause and others (`glslang-LICENSE.txt`) | <https://github.com/KhronosGroup/glslang> |
| cereal (in MoltenVK) | | BSD-3-Clause (`cereal-LICENSE`) | <https://github.com/USCiLab/cereal> |
| Debian packages | `lib/arm64-root`, `rootfs/x86_64` | each package's own; see `usr/share/doc/<package>/copyright` in that root | <https://sources.debian.org> (trixie, and bullseye for libicu67) |
