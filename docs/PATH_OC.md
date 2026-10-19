# path_oc - the 500 MHz plugin

The Vita's CPU normally runs games at most at **444 MHz**. DOOM 3 BFG is CPU-hungry, and runs noticeably better at
**500 MHz**. `path_oc.skprx` is a small kernel plugin that lets the game ask for 500 MHz.

## Installing

1. Copy `path_oc.skprx` (from the [Releases](../../releases)) to **`ur0:tai/path_oc.skprx`**.
2. In **`ur0:tai/config.txt`**, add it under `*KERNEL`:
   ```
   *KERNEL
   ur0:tai/path_oc.skprx
   ```
   (If a `ux0:tai/config.txt` exists, taiHEN reads that one instead - put the line there too, or delete it.)
3. Reboot.

The game's log (`console.txt`) shows the clocks at start-up, e.g. `main: clocks CPU 500 MHz (500 requested)`.

## What it does - and doesn't

- The game asks the system for 500 MHz when it starts. Without the plugin the system refuses anything above
  444 MHz; with it, a request between 445 and 500 MHz is granted (the method is the one PSVshell by Electry uses).
- **It only breaks the "no more than 444 MHz" limit.** Every other request goes through to the system untouched, and
  only software written to ask for more than 444 MHz is affected.
- **It does not override your PSVshell saved profile for this game.** If you saved a PSVshell profile for DOOM 3 BFG,
  that profile still applies; the plugin only changes the clocks the game gets **by default**, when you haven't set
  anything for it in PSVshell.
- It works alongside PSVshell.

Running the CPU at 500 MHz uses a little more battery and makes the Vita a little warmer - the same as overclocking
with PSVshell.

## Building

`tools/vita/path_oc` (VitaSDK + taiHEN):

```
cd tools/vita/path_oc && mkdir build && cd build
cmake .. && make
```
