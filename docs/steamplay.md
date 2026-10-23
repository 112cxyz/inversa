# Steam Play

Inversa can sit in Steam as a Steam Play compatibility tool. Steam then downloads a game's Linux build itself, keeps it updated, and starts it through Inversa when you press Play. VR games go to MoltenVR when it's installed.

## Before you start

Mac Steam ships Steam Play switched off, and Inversa doesn't change Steam. The tool only appears once something else has switched Steam Play on, such as [NotProton](https://github.com/NotProtonNot/NotProton). That's your call: it patches the Steam client, and Inversa has no part in it.

## Install

From an Inversa release (or a checkout after `make all`):

```
steamplay/install.sh
```

It says what it will add and asks first. Restart Steam, then in a game's Properties > Compatibility, force "Inversa (Linux build)". Steam downloads the Linux build the next time you install or update the game.

`steamplay/install.sh --uninstall` takes it out again. Each launch is logged to `~/Library/Logs/Inversa/steamplay.log`.

## Games with their own settings

The tool applies what a game needs beyond `inversa run`, as `tools/<game>.sh` does. So far that's Half-Life: Alyx (MoltenVK, its library folders and the shared OpenComposite frustum).
