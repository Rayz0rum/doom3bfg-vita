# Graphics on the Vita: anti-aliasing and image quality

The Vita's GPU is a PowerVR SGX543MP4+ (run at 222 MHz in this port) drawing to a 960x544 screen. The 3D view renders
at 720x408 (dynamic resolution takes it down to 544x312 when a scene is heavy) and is stretched to the full screen;
the HUD and menus are always drawn at 960x544. Image quality had to come from techniques that suit a **tile-based**
GPU and cost next to nothing.

## What the port uses

| Technique | What it fixes | Cost |
|---|---|---|
| **2x MSAA** | jagged geometry edges | resolved by the GPU as it writes each tile out - no extra pass |
| **Toksvig normal map filtering** | specular "sparkle" on distant bumpy surfaces | done once when a texture loads; a few multiplies per lit pixel |
| **Soft edges on alpha-tested surfaces** | jagged grates, fences, cut-outs (what MSAA can't reach) | a small alpha-only pass over those surfaces + part of the stretch |
| **Sharpening** | the softness of the stretched 3D view | part of the stretch the view needs anyway |

### 2x MSAA

The SGX resolves multisampling on-chip, tile by tile, so 2x MSAA costs very little memory bandwidth - it is the one
anti-aliasing method that is "free" on this kind of GPU. 4x was rejected for its cost at this resolution.

### Why FXAA was dropped

FXAA (NVIDIA FXAA 3.11, console variant) was implemented and built, then **scrapped**: post-process anti-aliasing
needs the finished frame read back from memory and every pixel processed with many dependent texture reads and
branches. That is cheap on desktop GPUs and a poor fit for a tile-based GPU like the SGX - it is exactly the
bandwidth-heavy, latency-bound work tile renderers avoid. Combined with the stretch it also blurred the already-upscaled
image. The simpler set above does the job for a fraction of the cost.

### Toksvig normal map filtering

Far away, a normal map's small mip levels average many bumps into one texel, and the highlight stays as sharp as on
a flat surface - so it flickers from pixel to pixel as the view moves. Toksvig (2005) measures how much the averaged
normals disagreed (the length of the unnormalised average) and widens and dims the highlight there.

The port computes it **when each normal map loads**, writes it into a colour channel the BFG's normal maps leave
unused (no extra memory), and leaves the normals themselves bit-for-bit unchanged. The shader then needs no `pow()`:
the highlight is `max(1 - k(1 - h), 0)^8` with `k` read from the texture - a multiply-add and three squarings,
within 2/255 of the exact curve. On the Vita the load-time pass runs on the job threads with its work buffers in
cached memory.

### Soft edges on alpha-tested surfaces

Alpha-tested surfaces (grates, fences, foliage) are cut out pixel by pixel, which MSAA cannot smooth. The GPU has no
alpha-to-coverage switch available, so the port fakes it: right before the 3D view is stretched, the alpha-tested
surfaces are drawn again (alpha only, depth-equal) into the view's alpha channel, and the stretch blurs exactly
where that map changes - the cut-out edges, on both sides.

### Sharpening

The stretched 3D view is soft on the Vita's screen. The stretch pass adds **LumaSharpen's diagonal 3-tap** pattern
(CeeJay.dk): two bilinear taps on the diagonal make a small blur, and the brightness difference from it is added back
- luma only (no colour fringes), clamped to ±9/255 (no halos). All three texture reads use coordinates computed in
the vertex shader, so the GPU fetches them before the pixel shader runs (tile-friendly). The strength grows a little
as dynamic resolution lowers the 3D view. It is applied on the Vita's screen only (not on a PlayStation TV, which
scales the picture again).

Settings for the curious (console variables): `vita_msaa`, `r_toksvig`, `vita_toksvigShader`, `vita_postFilter`,
`vita_sharpen`, `vita_sharpenClamp`, `vita_softAlphaEdges`, `vita_softAlphaEdgesShow` (paints the softened pixels red).
