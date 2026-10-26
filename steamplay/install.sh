#!/bin/sh
# Adds Inversa to Steam as a Steam Play compatibility tool, so Steam can download a game's Linux
# build and start it through Inversa:  steamplay/install.sh [--yes] | --uninstall
#
# Inversa doesn't change Steam. Mac Steam ships Steam Play switched off, so the tool only shows up
# once something else (NotProton) has switched it on; installing that is up to you.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
steam="$HOME/Library/Application Support/Steam"
dest="$steam/compatibilitytools.d/inversa"

if [ "$1" = "--uninstall" ]; then
  rm -rf "$dest"
  echo "Removed the Inversa tool from Steam. Restart Steam to see it gone."
  exit 0
fi

[ -d "$steam" ] || { echo "No Steam for Mac at $steam." >&2; exit 1; }
# the Inversa folder this sits in: a release (Inversa.app beside steamplay/), or a checkout's build/
home="$(dirname "$here")"
[ -x "$home/Inversa.app/Contents/MacOS/inversa" ] || home="$home/build"
[ -x "$home/Inversa.app/Contents/MacOS/inversa" ] || { echo "No Inversa.app next to $here." >&2; exit 1; }

if [ "$1" != "--yes" ]; then
  echo "This adds \"Inversa (Linux build)\" to Steam's compatibility tools:"
  echo "  $dest"
  echo "It runs games with the Inversa in $home."
  echo "Steam only lists it while Steam Play is switched on, which Mac Steam doesn't do by itself."
  printf "Continue? [y/N] "
  read answer
  case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 1 ;; esac
fi

mkdir -p "$dest"
cp "$here/compatibilitytool.vdf" "$here/toolmanifest.vdf" "$dest/"
rm -f "$dest/openxr.json" "$dest/inversa-home"
# a stub: the tool itself stays in the Inversa folder, so it updates whenever Inversa does. If that
# folder is moved or deleted (an unzipped download), MoltenVR's copy of Inversa stands in.
cat > "$dest/run" <<STUB
#!/bin/sh
home="$home" tool="$here/run"
if [ ! -x "\$tool" ]; then
  home="\$HOME/Library/Application Support/MoltenVR/inversa-runtime" tool="\$home/steamplay/run"
fi
if [ ! -x "\$tool" ]; then
  log="\$HOME/Library/Logs/Inversa/steamplay.log"
  mkdir -p "\$(dirname "\$log")"
  echo "=== \$(date): no Inversa at '$here' or in MoltenVR: run steamplay/install.sh again from an Inversa folder" >> "\$log"
  exit 1
fi
export INVERSA_HOME="\$home"
exec "\$tool" "\$@"
STUB
chmod +x "$dest/run"
echo "Installed. Restart Steam, then pick \"Inversa (Linux build)\" in a game's Properties > Compatibility."
