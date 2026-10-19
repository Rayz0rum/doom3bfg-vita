# Trophies

The port has the full DOOM 3 BFG Edition trophy list, unlocked as you play (DOOM 3, Resurrection of Evil, The Lost
Mission and the classic games' trophies).

The Vita only accepts trophies from homebrew with the **NoTrpDrm** plugin by Rinnegatamante, which turns off the
system's check of the trophy pack's signature.

## Setting it up

1. Download **NoTrpDrm** (`NoTrpDrm.suprx`) from https://github.com/Rinnegatamante/NoTrpDrm/releases.
2. Copy it to **`ur0:tai/NoTrpDrm.suprx`**.
3. In **`ur0:tai/config.txt`**, add the line under the `*main` section (it patches the Vita's shell, SceShell):
   ```
   *main
   ur0:tai/NoTrpDrm.suprx
   ```
4. Reboot the Vita.
5. Start the game. The first time, the Vita sets the trophy list up (a short "installing trophies" step).

**Use `ur0:tai`.** Note that if a `ux0:tai/config.txt` exists, taiHEN reads it **instead of** `ur0:tai/config.txt` -
either delete it or put the same line in it.

NoTrpDrm supports the Vita's **firmware 3.60, 3.65, 3.67 and 3.68** (it recognises the system's shell itself, so a
spoofed version number does not matter).

## If trophies don't unlock

The game logs exactly what happened at start-up in `ux0:data/doom3bfg/base/console.txt` - search for `trophy:`.
It says whether the trophy system started, and if not, what it found in `ur0:tai/config.txt` and `ux0:tai/config.txt`
(is NoTrpDrm listed, under which section, is the file where the line says), and whether `NoTrpDrm.suprx` is in
`ur0:tai` or `ux0:tai`. Most problems are a path in `config.txt` that doesn't match where the file is.

## Notes

- Trophies can't be taken back once unlocked; the L + R benchmark / verification never unlocks any (it only logs
  which ones it would have earned).
- Homebrew trophies are not official PSN trophies. Syncing a trophy list made by homebrew with PSN is at your own
  risk - many people keep their Vita off PSN, or don't sync.
