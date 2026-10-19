# Performance

**Do not expect a fully locked 30 fps.** Expect a good experience on the go: most of DOOM 3 plays at or near 30 fps,
heavy scenes drop toward 20 fps for a while, and a few places dip lower.

The port targets 30 fps (the BFG Edition's console frame rate): a frame every 33 ms. When the Vita can't keep up,
the frame rate holds at 20 fps rather than wobbling, and dynamic resolution lowers the 3D view (down to 544x312) to
get back to 30.

## Measured on hardware (0.9 development build, CPU 500 MHz, GPU 222 MHz, 2x MSAA)

From the built-in L + R benchmark and verification (a 3-hour automated play-through of every part of the game):

| Part | Average fps | Frames on time (30 fps) | Notes |
|---|---|---|---|
| DOOM 3 | 26.7 | 77% | most levels 28-30 fps; mars_city1, alphalabs2, delta2a and delta3 are the heaviest |
| The Lost Mission | 23.8 | 72% | |
| Resurrection of Evil | 13.9 | 62% | the erebus levels carry long loading hitches in this build |
| Multiplayer (with bots) | 23.9 | 66% | |
| Benchmark fights | 26.0 | 79% | |
| Classic DOOM / DOOM II | - | - | see below |

Per level (DOOM 3): admin 30.4, alphalabs1 30.1, alphalabs3 30.0, alphalabs4 29.8, commoutside 30.2, recycling2 29.4,
delta4 29.3, hell1 30.0, cpuboss 30.0, hellhole 30.9, mc_underground 28.1, recycling1 28.7, monorail 28.4, caverns2 28.4,
delta1 27.8, delta2b 27.5, cpu 27.8, delta5 27.0, comm1 27.2, mars_city1 26.1, mars_city2 26.1, enpro 26.0, site3 25.0,
caverns1 24.5, alphalabs2 24.0, delta3 22.3, delta2a 14.9.

**Classic DOOM and DOOM II** ran slowly in that run because, after three hours of DOOM 3 levels, the classic games'
memory landed in the Vita's uncached memory. 0.9 builds the classic games' memory from several cached pieces instead,
which is what runs them at full speed.

## Why the Vita finds it hard

- **CPU**: 4 ARM Cortex-A9 cores at 444 MHz (500 MHz with the plugin) - the BFG Edition was written for consoles with
  several times that. Game logic, the renderer's front end, shadow volumes and animation are spread over the cores
  and vectorised with NEON.
- **GPU**: a PowerVR SGX543MP4+ at 222 MHz. DOOM 3's per-light rendering and stencil shadows are heavy; the port draws
  the lighter interaction shader, culls lights and shadows hard, and scales the 3D view's resolution.
- **Memory**: 512 MB in separate pools (cached RAM, uncached RAM, video RAM), where the game expects one heap.

The 500 MHz plugin ([path_oc](PATH_OC.md)) is recommended.
