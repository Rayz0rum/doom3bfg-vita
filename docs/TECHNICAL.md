# Technical notes

A short tour of what the port changes, for the curious. The source comments explain each piece in detail.

## Platform

- **vitaGL** (Rinnegatamante) provides OpenGL ES on the Vita's GXM; the engine's GLSL shaders are converted to Cg at
  run time and cached. A boot self-test checks the shader translator's behaviour before the shaders are built.
- **Memory**: the Vita's memory is split into the newlib heap (cached), vitaGL's RAM pool (uncached), physically
  contiguous RAM and video RAM. All C/C++ allocations go through wrappers that spill from one pool to the next, keep
  CPU-heavy data in cached memory where possible, and keep a reserve for the C library's own allocations.
- **Threads and cores**: the game and the renderer's front end run on their own thread; job workers spread over cores
  0-2; core 3 (when the system grants it) takes only background work - the log writer, the watchdog, saving.
- **Clocks**: the game asks for a 500 MHz CPU (granted with the `path_oc` plugin), GPU and bus at 222 MHz.

## Rendering

- Dynamic resolution for the 3D view (720x408 to 544x312), the HUD and menus at 960x544.
- Shadow volumes, skinning, culling and the render matrices in NEON (checked against the scalar code on PC:
  `tools/vita/neontest`).
- A lighter interaction shader (the same lighting model, fewer and cheaper instructions).
- 2x MSAA, Toksvig filtering, soft alpha-tested edges and sharpening ([graphics](GRAPHICS.md)).
- Texture budgets: diffuse, bump and specular maps lose a mip level, text stays sharp; memory pressure lowers them
  further during play.

## Sound and video

- Sounds are compressed (ADPCM) and streamed from the memory card instead of being decoded into memory.
- Videos play on the Vita's hardware H.264 decoder (`sceAvPlayer`), with RoQ software video for screens that play
  together.
- Classic DOOM's music is synthesised (TiMidity) and streamed as it plays.

## Loading

- Big text files (the maps, up to 8 MB) are read through a 1 MB window instead of one buffer - late in a session a
  single large block may not exist even with plenty of free memory.
- Missing-file lookups and other hidden stalls were removed from play; everything that still loads during play is
  logged with its time.

## Multiplayer

- The BFG Edition's lobby and networking, with the PSN parts replaced: local network discovery, friends kept by the
  game, and (official builds) online play without servers ([multiplayer](MULTIPLAYER.md)).
- Bots: Quake III Arena's bot AI on a waypoint graph the host builds and learns per map (`neo/d3xp/VitaBots.cpp`).

## Testing

Holding **L + R** at the title screen runs the benchmark and the full verification: every campaign level, the
cutscenes, saving and loading, fights, multiplayer with bots, the classic games, the menus and languages - each check
with a verdict, every frame timed, screenshots taken. On PC the same suite runs headless.
