/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"

/*
================================================================================================
Contains the Image implementation for OpenGL.
================================================================================================
*/

#include "../tr_local.h"

#if defined(__vita__)
#include "../../sys/vita/vita_public.h"
#endif

#if defined(__vita__)
/*
================================================================================================
Vita texture upload fixes and verification

Two formats the engine relies on do not arrive on the GPU the way the desktop
renderer assumes:

  RGB565 (every TD_LIGHT image - all light projection textures). BinaryImage
  writes each texel high byte first, i.e. big-endian, and the desktop path
  undoes that with GL_UNPACK_SWAP_BYTES. GLES2 has no such switch, so on this
  port every light projection texel reached the GPU byte-swapped: a grey ramp
  became a band of scrambled colours, and since the projection multiplies
  every lit pixel, that was the pink/green on every surface. Measured on the
  retail data: lights/squarelight1 is 100% greyscale read big-endian and 3%
  read little-endian, which is how GXM reads it. Swap on upload.

  CFM_GREEN_ALPHA (TD_FONT and TD_COVERAGE - fonts, decals, grates, fences).
  Stored as DXT1 with the coverage in green; the desktop path swizzles the
  texture to (1,1,1,G) with GL_TEXTURE_SWIZZLE_*, which GLES2 also lacks. The
  raw (0,G,0,1) is what put green text on opaque black boxes in the HUD. GXM
  has a native single-channel BC4 format sampled as (1,1,1,value), at the same
  half a byte per texel as DXT1, so each block is transcoded rather than
  expanded - no memory cost. vitaGL learned the format as
  VGL_COMPRESSED_ALPHA_BC4 for this.

Every uncompressed upload is also checked against vitaGL's texture memory at
the address vitaGL samples the level from, because vitaGL used to drop every
mip of an uncompressed texture onto the corner of level 0 and nothing
reported it.
================================================================================================
*/
extern "C" void* vglGetTexDataPointer(unsigned int target);

int vitaRGB565Images = 0;        // distinct RGB565 images swapped
int vitaRGB565Levels = 0;        // RGB565 mip levels swapped
int vitaCoverageImages = 0;      // CFM_GREEN_ALPHA images transcoded to BC4
int vitaCoverageLevels = 0;
int vitaCoverageMaxError = 0;    // worst BC4-vs-DXT1 texel difference, 0-255
int vitaUploadVerifyOk = 0;      // uncompressed levels found intact in GPU memory
int vitaUploadVerifyBad = 0;     // ... found corrupted
int vitaUploadVerifyDropped = 0; // ... that vitaGL never allocated (non-square tails)

static bool VitaUseBC4Coverage(const idImageOpts& o)
{
    // Falls back to the old DXT1 upload only if the boot self-test ran and
    // measured BC4 as broken - then the HUD is wrong but nothing else breaks.
    return o.format == FMT_DXT1 && o.colorFormat == CFM_GREEN_ALPHA && vitaSelfTest.bc4Coverage != 0;
}

// Percentage of RGB565 texels that decode as exact greys (R5 == B5 == G6>>1,
// to within one step) when read with the given byte order. A light
// projection texture is greyscale, so this tells which order the data is in.
static int VitaRGB565GreyPercent(const byte* p, int texels, bool bigEndian)
{
    if (texels <= 0) {
        return 0;
    }
    int grey = 0;
    for (int i = 0; i < texels; i++) {
        const int v = bigEndian ? ((p[i * 2] << 8) | p[i * 2 + 1]) : ((p[i * 2 + 1] << 8) | p[i * 2]);
        const int r = (v >> 11) & 31;
        const int g = (v >> 5) & 63;
        const int b = v & 31;
        const int d = (g >> 1) - r;
        if (r == b && d >= -1 && d <= 1) {
            grey++;
        }
    }
    return (int)((grey * 100LL) / texels);
}

static void VitaBC4Palette(int e0, int e1, int pal[8])
{
    pal[0] = e0;
    pal[1] = e1;
    if (e0 > e1) {
        for (int i = 1; i <= 6; i++) {
            pal[i + 1] = ((7 - i) * e0 + i * e1 + 3) / 7;
        }
    } else {
        for (int i = 1; i <= 4; i++) {
            pal[i + 1] = ((5 - i) * e0 + i * e1 + 2) / 5;
        }
        pal[6] = 0;
        pal[7] = 255;
    }
}

// Encodes 16 values with the given endpoints; returns the sum of squared
// errors and fills `out` (8 bytes) and the worst single-texel error.
static int VitaBC4Try(const int v[16], int e0, int e1, byte out[8], int* worst)
{
    int pal[8];
    VitaBC4Palette(e0, e1, pal);
    unsigned long long bits = 0;
    int err = 0;
    int w = 0;
    for (int t = 0; t < 16; t++) {
        int best = 0;
        int bestD = 1 << 30;
        for (int k = 0; k < 8; k++) {
            const int d = pal[k] > v[t] ? pal[k] - v[t] : v[t] - pal[k];
            if (d < bestD) {
                bestD = d;
                best = k;
            }
        }
        bits |= (unsigned long long)best << (3 * t);
        err += bestD * bestD;
        if (bestD > w) {
            w = bestD;
        }
    }
    out[0] = (byte)e0;
    out[1] = (byte)e1;
    for (int i = 0; i < 6; i++) {
        out[2 + i] = (byte)((bits >> (8 * i)) & 0xFF);
    }
    *worst = w;
    return err;
}

// One DXT1 block whose green channel carries coverage -> one BC4 block with
// the same 16 values. The DXT1 palette is rebuilt exactly as a decoder would,
// then BC4 endpoints are chosen so the DXT1 thirds land on BC4 sevenths
// (stretching the range by 7/6) whenever that fits in 0..255, which makes
// the usual four-level DXT1 block exact rather than off by a twelfth.
// Returns the worst texel error of the chosen encoding.
static int VitaTranscodeCoverageBlock(const byte* src, byte* dst)
{
    const int c0 = src[0] | (src[1] << 8);
    const int c1 = src[2] | (src[3] << 8);
    const unsigned int idx = src[4] | (src[5] << 8) | (src[6] << 16) | ((unsigned int)src[7] << 24);
    const int g0 = (((c0 >> 5) & 63) << 2) | (((c0 >> 5) & 63) >> 4);
    const int g1 = (((c1 >> 5) & 63) << 2) | (((c1 >> 5) & 63) >> 4);
    int pal[4];
    pal[0] = g0;
    pal[1] = g1;
    if (c0 > c1) {
        pal[2] = (2 * g0 + g1 + 1) / 3;
        pal[3] = (g0 + 2 * g1 + 1) / 3;
    } else {
        pal[2] = (g0 + g1 + 1) / 2;
        pal[3] = 0; // transparent black: no coverage
    }
    int v[16];
    int lo = 255, hi = 0;
    for (int t = 0; t < 16; t++) {
        v[t] = pal[(idx >> (2 * t)) & 3];
        lo = v[t] < lo ? v[t] : lo;
        hi = v[t] > hi ? v[t] : hi;
    }

    byte cand[8];
    int worst = 0;
    if (lo == hi) {
        VitaBC4Try(v, lo, lo, dst, &worst); // six-value mode, every index 0
        return worst;
    }
    int bestErr = VitaBC4Try(v, hi, lo, dst, &worst); // plain eight-value
    int bestWorst = worst;
    const int stretch = (7 * (hi - lo) + 3) / 6;
    const int e0 = lo + stretch;
    if (e0 <= 255 && e0 > lo) {
        const int err = VitaBC4Try(v, e0, lo, cand, &worst);
        if (err < bestErr) {
            bestErr = err;
            bestWorst = worst;
            memcpy(dst, cand, 8);
        }
    }
    const int e1 = hi - stretch;
    if (e1 >= 0 && hi > e1) {
        const int err = VitaBC4Try(v, hi, e1, cand, &worst);
        if (err < bestErr) {
            bestErr = err;
            bestWorst = worst;
            memcpy(dst, cand, 8);
        }
    }
    // Six-value mode has explicit 0 and 255, which suits three-colour DXT1
    // blocks with their transparent index; the 5/4 stretch puts the DXT1
    // midpoint exactly on a BC4 fifth.
    int lo6 = 255, hi6 = 0;
    for (int t = 0; t < 16; t++) {
        if (v[t] != 0 && v[t] != 255) {
            lo6 = v[t] < lo6 ? v[t] : lo6;
            hi6 = v[t] > hi6 ? v[t] : hi6;
        }
    }
    if (lo6 <= hi6) {
        int err = VitaBC4Try(v, lo6, hi6, cand, &worst);
        if (err < bestErr) {
            bestErr = err;
            bestWorst = worst;
            memcpy(dst, cand, 8);
        }
        const int top = lo6 + ((hi6 - lo6) * 5 + 2) / 4;
        if (hi6 > lo6 && top <= 255) {
            err = VitaBC4Try(v, lo6, top, cand, &worst);
            if (err < bestErr) {
                bestErr = err;
                bestWorst = worst;
                memcpy(dst, cand, 8);
            }
        }
    }
    return bestWorst;
}

// Compares what was just uploaded with vitaGL's copy at the address it will
// sample the level from. Only for formats vitaGL stores byte-for-byte.
static void VitaVerifyLinearUpload(const idImage* img, const idImageOpts& opts, int level, int w, int h,
    const byte* src, int bpp, bool chatty)
{
    const byte* gpu = (const byte*)vglGetTexDataPointer(GL_TEXTURE_2D);
    int pitch = 0, vitaLevels = 0;
    const int off = Vita_LinearMipOffset(level, opts.width, opts.height, bpp, &pitch, &vitaLevels);
    if (gpu == NULL) {
        vitaUploadVerifyBad++;
        Vita_Checkpoint("upload-verify '%s' L%d: vitaGL returned no texture memory", img->GetName(), level);
        return;
    }
    if (off < 0) {
        vitaUploadVerifyDropped++;
        if (chatty) {
            Vita_Checkpoint("upload-verify '%s' L%d %dx%d: vitaGL allocated only %d of %d levels for %dx%d, so this level has no memory and was dropped",
                img->GetName(), level, w, h, vitaLevels, opts.numLevels, opts.width, opts.height);
        }
        return;
    }
    const int rowBytes = w * bpp;
    const int n = rowBytes < 64 ? rowBytes : 64;
    int firstBad = -1;
    for (int i = 0; i < n && firstBad < 0; i++) {
        if (gpu[off + i] != src[i]) {
            firstBad = i;
        }
    }
    const int lastRow = h - 1;
    for (int i = 0; i < n && firstBad < 0; i++) {
        if (gpu[off + lastRow * pitch + i] != src[lastRow * rowBytes + i]) {
            firstBad = lastRow * rowBytes + i;
        }
    }
    if (firstBad < 0) {
        vitaUploadVerifyOk++;
        if (chatty) {
            Vita_Checkpoint("upload-verify '%s' L%d %dx%d fmt %d: GPU copy at +%d (pitch %d) matches the upload",
                img->GetName(), level, w, h, (int)opts.format, off, pitch);
        }
    } else {
        vitaUploadVerifyBad++;
        Vita_Checkpoint("upload-verify '%s' L%d %dx%d fmt %d: GPU copy at +%d (pitch %d) DIFFERS at byte %d (upload %02X, GPU %02X)",
            img->GetName(), level, w, h, (int)opts.format, off, pitch, firstBad,
            src[firstBad], gpu[off + (firstBad / rowBytes) * pitch + (firstBad % rowBytes)]);
    }
}

/*
========================
VitaBlitFramebufferToImage

idImage::CopyFramebuffer() grabs the frame rendered so far into a texture -
_currentRender for heat haze and glass, and the RC_COPY_RENDER captures used
by mirrors and remote cameras. Its glCopyTexImage2D is, in vitaGL, a CPU
glReadPixels of display memory followed by a texture upload. GXM is a tiled,
deferred renderer: nothing drawn in the current scene exists in memory until
the scene ends, so that read returns an older, finished frame - pause menu,
HUD and all - which the heat haze then drew back into the world. It also
cost a 2 MB uncached CPU read and a 2 MB re-upload every time.

Blit on the GPU instead. vitaGL's glBlitFramebuffer() ends the display scene
(so the frame so far is written out), draws it into an FBO wrapping the
image's texture, and the next draw opens a fresh display scene; the display's
colour comes back through its background texture and depth/stencil through
force-load, both of which vitaGL sets up for exactly this. The result is in
OpenGL's bottom-up row order, the same as the old copy.
========================
*/
struct vitaCopyTarget_t {
    const idImage* image;
    GLuint texnum;
    const void* texData;
    GLuint fbo;
    int width;
    int height;
};
static vitaCopyTarget_t vitaCopyTargets[8];
static int vitaCopyCount = 0;

bool VitaBlitFramebufferToImage(const idImage* image, GLuint texnum, int x, int y, int w, int h)
{
    vitaCopyTarget_t* t = NULL;
    for (int i = 0; i < 8 && t == NULL; i++) {
        if (vitaCopyTargets[i].image == image) {
            t = &vitaCopyTargets[i];
        }
    }
    for (int i = 0; i < 8 && t == NULL; i++) {
        if (vitaCopyTargets[i].image == NULL) {
            t = &vitaCopyTargets[i];
            t->image = image;
        }
    }
    if (t == NULL) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Vita_Checkpoint("framebuffer copy -> '%s': more than 8 copy targets, falling back to the CPU read (stale frame)", image->GetName());
        }
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, texnum);
    const void* data = vglGetTexDataPointer(GL_TEXTURE_2D);
    if (t->fbo == 0 || t->texnum != texnum || t->texData != data || t->width != w || t->height != h) {
        if (t->fbo != 0) {
            glDeleteFramebuffers(1, &t->fbo);
            t->fbo = 0;
        }
        // Storage first: an FBO points at the texture's memory, so it is
        // created after the last time that memory can move.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &t->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texnum, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        t->texnum = texnum;
        t->texData = vglGetTexDataPointer(GL_TEXTURE_2D);
        t->width = w;
        t->height = h;
        static int created = 0;
        if (created++ < 6 || status != GL_FRAMEBUFFER_COMPLETE) {
            Vita_Checkpoint("framebuffer copy -> '%s' %dx%d: GPU blit target created, framebuffer status 0x%04X%s",
                image->GetName(), w, h, status, status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (NOT complete - falling back)");
        }
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            glDeleteFramebuffers(1, &t->fbo);
            t->fbo = 0;
            t->image = NULL;
            return false;
        }
    }

    // vitaGL's blit honours the scissor test, and re-applies the engine's
    // last scissor rectangle to the copy target when the blit scene starts.
    // With the 3D view cropped (720x408 inside 960x544) that rectangle cut the
    // copy down to a band and the rest of the texture stayed black - the
    // stretched view came out as a thin "panoramic" strip. Copy unclipped.
    const GLboolean scissorWasOn = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorWasOn) {
        glDisable(GL_SCISSOR_TEST);
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    // vitaGL samples the display with the source rectangle as given, from the
    // top row down (srcY / height straight into texture space), while GL
    // passes rows counted from the bottom. Whole-screen copies never noticed;
    // a crop does - the 3D view is drawn in the top-left corner of the screen
    // when it renders below the display resolution (and so are the in-world
    // camera screens), and without this the copy grabbed the bottom instead.
    const int displayH = renderSystem->GetHeight();
    const int srcTop = displayH - (y + h);
    glBlitFramebuffer(x, srcTop, x + w, srcTop + h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissorWasOn) {
        glEnable(GL_SCISSOR_TEST); // back on the display, with the engine's rectangle
    }

    // The blit ran in a scene of its own; make the engine re-issue
    // glUseProgram so vitaGL re-uploads uniforms into the next display scene
    // instead of trusting state from before the split.
    renderProgManager.Unbind();

    vitaCopyCount++;
    if (vitaCopyCount <= 3 || (vitaCopyCount % 600) == 0) {
        Vita_Checkpoint("framebuffer copy #%d -> '%s' from (%d,%d) %dx%d: GPU blit (scene split), not a CPU read",
            vitaCopyCount, image->GetName(), x, y, w, h);
    }
    return true;
}

static int VitaBytesPerTexelIfStoredVerbatim(textureFormat_t f)
{
    switch (f) {
    case FMT_RGBA8:
        return 4;
    case FMT_RGB565:
    case FMT_L8A8:
        return 2;
    case FMT_INT8:
    case FMT_LUM8:
    case FMT_ALPHA:
        return 1;
    default:
        return 0; // converted on upload (XRGB8) or compressed: not comparable
    }
}
#endif

/*
========================
idImage::SubImageUpload
========================
*/
#if defined(__vita__)
/*
========================
idImage::VitaReplaceRGBA

A whole-texture update (classic Doom frames, cinematics). glTexSubImage2D on
a texture the GPU used in the last few frames makes vitaGL allocate a new
buffer and copy the old contents into it before the update overwrites them;
re-specifying the level allocates the new buffer and copies only the new
data. The old buffer is freed once the GPU is done with it.
========================
*/
void idImage::VitaReplaceRGBA(const byte* pic)
{
    Bind();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, opts.width, opts.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pic);
    // re-specifying may reset the sampler on the new storage; restore it
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const GLint wrap = (repeat == TR_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
}
#endif

#if defined(__vita__)
/*
========================
idImage::VitaUploadScaled

Classic Doom: the 320x200 frame goes into a small texture, and the GPU blits
it 'scale' times bigger into this image with nearest filtering. The engine
then draws this image with linear filtering, so the Doom pixels come out
crisp and even ("sharp bilinear") for the price of one small blit.

Inside framebuffer objects vitaGL renders flipped (GL convention), so an
FBO-to-FBO blit turns the picture upside down; the source rows are passed
inverted to cancel that.
========================
*/
void idImage::VitaUploadScaled(const byte* pic, int w, int h, int scale)
{
    static idImage* small = NULL;
    static GLuint fbos[2] = { 0, 0 };
    static GLuint attachedBig = 0;
    if (small == NULL) {
        small = globalImages->AllocImage("_doomClassicSmall");
        idImageOpts so;
        so.format = FMT_RGBA8;
        so.colorFormat = CFM_DEFAULT;
        so.width = w;
        so.height = h;
        so.numLevels = 1;
        small->AllocImage(so, TF_NEAREST, TR_CLAMP);
        glGenFramebuffers(2, fbos);
        common->Printf("vita: classic doom frames are %dx%d, blown up %dx on the GPU into %dx%d\n", w, h, scale, opts.width, opts.height);
    }
    small->VitaReplaceRGBA(pic);

    // (re)attach: the small texture's storage moves on every re-specify
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[0]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, small->texnum, 0);
    if (attachedBig != texnum) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[1]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texnum, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        common->Printf("vita: classic doom upscale target %dx%d attached, framebuffer status 0x%04X%s\n",
            opts.width, opts.height, status, status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " - INCOMPLETE");
        attachedBig = texnum;
    }
    // vitaGL's blit honours the scissor test, re-applied to the target: with
    // the engine's last rectangle it rewrote only a band of the 960x600 frame,
    // and the rest kept whatever that memory held before (the "panoramic"
    // strip, old overlay and menu pixels in the black areas)
    const GLboolean scissorWasOn = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorWasOn) {
        glDisable(GL_SCISSOR_TEST);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[1]);
    glBlitFramebuffer(0, h, w, 0, 0, 0, w * scale, h * scale, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (scissorWasOn) {
        glEnable(GL_SCISSOR_TEST);
    }
    // the blit ran its own programs; make the engine re-issue glUseProgram
    renderProgManager.Unbind();
}
#endif

#if defined(__vita__)
/*
========================
idImage::VitaClassicBeginFrameP8 / VitaClassicEndFrameP8

Classic DOOM's frames go to the GPU as they are: 8-bit palette indices in a
paletted (P8) texture plus its 256-colour palette. The GPU's texture unit does
the palette lookup for free; the CPU copies 576 KB of indices a frame instead
of converting 576,000 pixels to RGBA and writing 2.3 MB into video memory (22
ms a frame on the hardware - twice Doom's own tic and render).

The texture is this image's own, respecified as P8 (again if anything
recreates it). Its pixels and palette live in three rotating buffer pairs in
vitaGL's uncached main RAM pool - the CPU's burst writes are fast there, unlike
CDRAM, and the GPU reads it - and each frame points the texture at the next
pair. The texture descriptor is copied into the GPU's command stream when a
draw is issued, so frames already queued keep the pair they were drawn with.
Rows are the frame's own (row 0 at the top, as the engine draws the image).
========================
*/
extern "C" void* vgl_memalign(size_t alignment, size_t size, vglMemType type);

static byte* vitaClassicPix[3] = { NULL, NULL, NULL };
static uint32* vitaClassicPal[3] = { NULL, NULL, NULL };
static int vitaClassicPixW = 0, vitaClassicPixH = 0;
static int vitaClassicCur = 0;

static void* Vita_ClassicAlloc(size_t alignment, size_t size)
{
    void* p = vgl_memalign(alignment, size, VGL_MEM_RAM);
    if (p == NULL) {
        p = vgl_memalign(alignment, size, VGL_MEM_VRAM);
    }
    return p;
}

byte* idImage::VitaClassicBeginFrameP8(int w, int h, uint32** palette)
{
    Bind();
    SceGxmTexture* gxm = vglGetGxmTexture(GL_TEXTURE_2D);
    if (gxm == NULL) {
        return NULL;
    }
    if (vitaClassicPix[0] == NULL || w != vitaClassicPixW || h != vitaClassicPixH) {
        // (only ever once; on a size change the old set is left alone, as a
        // queued frame may still read it)
        const int stride = (w + 7) & ~7; // vitaGL's linear row pitch
        for (int i = 0; i < 3; i++) {
            vitaClassicPix[i] = (byte*)Vita_ClassicAlloc(256, stride * h);
            vitaClassicPal[i] = (uint32*)Vita_ClassicAlloc(64, 256 * sizeof(uint32));
            if (vitaClassicPix[i] == NULL || vitaClassicPal[i] == NULL) {
                common->Warning("vita: no GPU memory for the classic doom frames");
                vitaClassicPix[0] = NULL;
                return NULL;
            }
            memset(vitaClassicPix[i], 0, stride * h);
            memset(vitaClassicPal[i], 0, 256 * sizeof(uint32));
        }
        vitaClassicPixW = w;
        vitaClassicPixH = h;
    }
    if (sceGxmTextureGetFormat(gxm) != SCE_GXM_TEXTURE_FORMAT_P8_ABGR
        || (int)sceGxmTextureGetWidth(gxm) != w || (int)sceGxmTextureGetHeight(gxm) != h) {
        // the palette argument only gives the new texture a palette pointer;
        // EndFrame points it at ours
        glColorTable(GL_COLOR_TABLE, GL_RGBA, 256, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX8_EXT, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
        gxm = vglGetGxmTexture(GL_TEXTURE_2D);
        common->Printf("vita: classic doom frames %dx%d as 8-bit paletted textures - the GPU does the palette lookup (format 0x%08X)\n",
            w, h, (unsigned int)sceGxmTextureGetFormat(gxm));
    }
    vitaClassicCur = (vitaClassicCur + 1) % 3;
    *palette = vitaClassicPal[vitaClassicCur];
    return vitaClassicPix[vitaClassicCur];
}

void idImage::VitaClassicEndFrameP8()
{
    if (vitaClassicPix[vitaClassicCur] == NULL) {
        return;
    }
    Bind();
    SceGxmTexture* gxm = vglGetGxmTexture(GL_TEXTURE_2D);
    if (gxm != NULL) {
        sceGxmTextureSetData(gxm, vitaClassicPix[vitaClassicCur]);
        sceGxmTextureSetPalette(gxm, vitaClassicPal[vitaClassicCur]);
    }
}
#endif

void idImage::SubImageUpload(int mipLevel, int x, int y, int z, int width, int height, const void* pic, int pixelPitch) const
{
#if defined(__vita__)
    Vita_CheckGLThread("SubImageUpload", GetName());
#endif
    assert(x >= 0 && y >= 0 && mipLevel >= 0 && width >= 0 && height >= 0 && mipLevel < opts.numLevels);

    const bool isGLES = (glConfig.driverType == GLDRV_OPENGL_ES2) || (glConfig.driverType == GLDRV_OPENGL_ES3);


    int compressedSize = 0;

    if (IsCompressed()) {
        assert(!(x & 3) && !(y & 3));

        // compressed size may be larger than the dimensions due to padding to quads
        int quadW = (width + 3) & ~3;
        int quadH = (height + 3) & ~3;
        compressedSize = quadW * quadH * BitsForFormat(opts.format) / 8;

        int padW = (opts.width + 3) & ~3;
        int padH = (opts.height + 3) & ~3;
        (void)padH;
        (void)padW;
        assert(x + width <= padW && y + height <= padH);
        // upload the non-aligned value, OpenGL understands that there
        // will be padding
        if (x + width > opts.width) {
            width = opts.width - x;
        }
        if (y + height > opts.height) {
            height = opts.height - y;
        }
    } else {
        assert(x + width <= opts.width && y + height <= opts.height);
    }

    int target;
    int uploadTarget;
    if (opts.textureType == TT_2D) {
        target = GL_TEXTURE_2D;
        uploadTarget = GL_TEXTURE_2D;
    } else if (opts.textureType == TT_CUBIC) {
        target = GL_TEXTURE_CUBE_MAP_EXT;
        uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT + z;
    } else {
        assert(!"invalid opts.textureType");
        target = GL_TEXTURE_2D;
        uploadTarget = GL_TEXTURE_2D;
    }

    qglBindTexture(target, texnum);

#if defined(__vita__)
    // See "Vita texture upload fixes" above.
    const void* uploadPic = pic;
    byte* vitaScratch = NULL;
    if (pic != NULL && opts.format == FMT_RGB565) {
        const int texels = (pixelPitch != 0 ? pixelPitch : width) * height;
        const byte* s = (const byte*)pic;
        vitaScratch = (byte*)Mem_Alloc(texels * 2, TAG_TEMP);
        for (int i = 0; i < texels; i++) {
            vitaScratch[i * 2 + 0] = s[i * 2 + 1];
            vitaScratch[i * 2 + 1] = s[i * 2 + 0];
        }
        uploadPic = vitaScratch;
        vitaRGB565Levels++;
        if (mipLevel == 0 && ++vitaRGB565Images <= 4) {
            Vita_Checkpoint("rgb565 '%s' %dx%d, %d levels: source %d%% greyscale read big-endian vs %d%% read little-endian -> byte-swapped for GXM (first texel bytes %02X %02X -> %02X %02X)",
                GetName(), width, height, opts.numLevels,
                VitaRGB565GreyPercent(s, texels, true), VitaRGB565GreyPercent(s, texels, false),
                s[0], s[1], vitaScratch[0], vitaScratch[1]);
        }
    } else if (pic != NULL && VitaUseBC4Coverage(opts)) {
        const int blocks = compressedSize / 8;
        vitaScratch = (byte*)Mem_Alloc(blocks * 8, TAG_TEMP);
        int worst = 0;
        int covered = 0;
        for (int b = 0; b < blocks; b++) {
            const int e = VitaTranscodeCoverageBlock((const byte*)pic + b * 8, vitaScratch + b * 8);
            worst = e > worst ? e : worst;
            if (vitaScratch[b * 8] != 0 || vitaScratch[b * 8 + 1] != 0) {
                covered++;
            }
        }
        uploadPic = vitaScratch;
        vitaCoverageLevels++;
        vitaCoverageMaxError = worst > vitaCoverageMaxError ? worst : vitaCoverageMaxError;
        if (mipLevel == 0) {
            vitaCoverageImages++;
            Vita_Checkpoint("coverage '%s' %dx%d, %d levels: DXT1 green -> BC4 sampled (1,1,1,a); %d blocks, %d with coverage, worst texel error %d/255",
                GetName(), width, height, opts.numLevels, blocks, covered, worst);
        }
    }
#else
    const void* uploadPic = pic;
#endif

    if (pixelPitch != 0) {
        qglPixelStorei(GL_UNPACK_ROW_LENGTH, pixelPitch);
    }
    if (opts.format == FMT_RGB565 && !isGLES) {
        // FIXME: GLES doesn't support this, but probably it is necessary..
        qglPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
    }
#ifdef DEBUG
    GL_CheckErrors();
#endif
    if (IsCompressed()) {
        qglCompressedTexSubImage2DARB(uploadTarget, mipLevel, x, y, width, height, internalFormat, compressedSize, uploadPic);
    } else {

        // make sure the pixel store alignment is correct so that lower mips get created
        // properly for odd shaped textures - this fixes the mip mapping issues with
        // fonts
#if !defined(__vita__)
        int unpackAlignment = width * BitsForFormat((textureFormat_t)opts.format) / 8;
        if ((unpackAlignment & 3) == 0) {
            qglPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        } else {
            qglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
#else
        // vitaGL's glPixelStorei only implements GL_UNPACK_ROW_LENGTH and
        // rejects GL_UNPACK_ALIGNMENT outright. It reads source rows tightly
        // packed through its per-format read callback, so there is no row
        // padding for an alignment to describe and nothing to set here.
#endif

        qglTexSubImage2D(uploadTarget, mipLevel, x, y, width, height, dataFormat, dataType, uploadPic);
    }
#if defined(__vita__)
    if (uploadPic != NULL && !IsCompressed() && opts.textureType == TT_2D && x == 0 && y == 0 && pixelPitch == 0) {
        const int bpp = VitaBytesPerTexelIfStoredVerbatim((textureFormat_t)opts.format);
        if (bpp > 0) {
            // The first few levels of each format, then only failures (every
            // light projection level used to be reported: a memory card write
            // per level, some 200 of them in a campaign map's load).
            static int chattyBudget[32];
            const bool chatty = (chattyBudget[opts.format & 31]++ < 8);
            VitaVerifyLinearUpload(this, opts, mipLevel, width, height, (const byte*)uploadPic, bpp, chatty);
        }
    }
    if (vitaScratch != NULL) {
        Mem_Free(vitaScratch);
    }
#endif
#ifdef DEBUG
    GL_CheckErrors();
#endif
    if (opts.format == FMT_RGB565 && !isGLES) {
        // FIXME: GLES doesn't support this
        qglPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
    }
    if (pixelPitch != 0) {
        qglPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

/*
========================
idImage::SetPixel
========================
*/
void idImage::SetPixel(int mipLevel, int x, int y, const void* data, int dataSize)
{
    SubImageUpload(mipLevel, x, y, 0, 1, 1, data);
}

/*
========================
idImage::SetTexParameters
========================
*/
void idImage::SetTexParameters()
{
    int target = GL_TEXTURE_2D;
    switch (opts.textureType) {
    case TT_2D:
        target = GL_TEXTURE_2D;
        break;
    case TT_CUBIC:
        target = GL_TEXTURE_CUBE_MAP_EXT;
        break;
    default:
        idLib::FatalError("%s: bad texture type %d", GetName(), opts.textureType);
        return;
    }

    // ALPHA, LUMINANCE, LUMINANCE_ALPHA, and INTENSITY have been removed
    // in OpenGL 3.2. In order to mimic those modes, we use the swizzle operators
#if defined(__vita__)
    // GL_TEXTURE_SWIZZLE_* is GLES3/desktop only and vitaGL rejects it with
    // GL_INVALID_ENUM. The channel layout has to come from the texture format
    // instead - see AllocImage(), where the single-channel formats use the
    // unsized GL names so GXM picks a matching swizzle natively.
#elif defined(USE_CORE_PROFILE)
    if (opts.colorFormat == CFM_GREEN_ALPHA) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
    } else if (opts.format == FMT_LUM8) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    } else if (opts.format == FMT_L8A8) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
    } else if (opts.format == FMT_ALPHA) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    } else if (opts.format == FMT_INT8) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    } else {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_RED);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    }
#else
    if (opts.colorFormat == CFM_GREEN_ALPHA) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
    } else if (opts.format == FMT_ALPHA) {
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_R, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_ONE);
        qglTexParameteri(target, GL_TEXTURE_SWIZZLE_A, GL_RED);
    }
#endif

    switch (filter) {
    case TF_DEFAULT:
        if (r_useTrilinearFiltering.GetBool()) {
            qglTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        } else {
            qglTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        }
        qglTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        break;
    case TF_LINEAR:
        qglTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        qglTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        break;
    case TF_NEAREST:
        qglTexParameterf(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        qglTexParameterf(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        break;
    default:
        common->FatalError("%s: bad texture filter %d", GetName(), filter);
    }

    if (glConfig.anisotropicFilterAvailable) {
        // only do aniso filtering on mip mapped images
        if (filter == TF_DEFAULT) {
            int aniso = r_maxAnisotropicFiltering.GetInteger();
            if (aniso > glConfig.maxTextureAnisotropy) {
                aniso = glConfig.maxTextureAnisotropy;
            }
            if (aniso < 0) {
                aniso = 0;
            }
            qglTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
        } else {
            qglTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1);
        }
    }
    if (glConfig.textureLODBiasAvailable && (usage != TD_FONT)) {
        // use a blurring LOD bias in combination with high anisotropy to fix our aliasing grate textures...
        qglTexParameterf(target, GL_TEXTURE_LOD_BIAS_EXT, r_lodBias.GetFloat());
    }

    // set the wrap/clamp modes
    switch (repeat) {
    case TR_REPEAT:
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_REPEAT);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_REPEAT);
        break;
    case TR_CLAMP_TO_ZERO: {
#if defined(__vita__)
        // Neither GL_TEXTURE_BORDER_COLOR nor GL_CLAMP_TO_BORDER exists in GLES2,
        // and vitaGL's wrap modes are limited to CLAMP_TO_EDGE / REPEAT /
        // MIRRORED_REPEAT / MIRROR_CLAMP. Edge clamp is the closest available:
        // sampling past the edge repeats the edge texel rather than returning
        // the border colour, so light projection and falloff textures smear at
        // their edges instead of cutting off.
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
        float color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        qglTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, color);
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#endif
    } break;
    case TR_CLAMP_TO_ZERO_ALPHA: {
#if defined(__vita__)
        // Neither GL_TEXTURE_BORDER_COLOR nor GL_CLAMP_TO_BORDER exists in GLES2,
        // and vitaGL's wrap modes are limited to CLAMP_TO_EDGE / REPEAT /
        // MIRRORED_REPEAT / MIRROR_CLAMP. Edge clamp is the closest available:
        // sampling past the edge repeats the edge texel rather than returning
        // the border colour, so light projection and falloff textures smear at
        // their edges instead of cutting off.
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
        float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        qglTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, color);
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#endif
    } break;
    case TR_CLAMP:
        qglTexParameterf(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTexParameterf(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        break;
    default:
        common->FatalError("%s: bad texture repeat %d", GetName(), repeat);
    }
}

/*
========================
idImage::AllocImage

Every image will pass through this function. Allocates all the necessary MipMap levels for the
Image, but doesn't put anything in them.

This should not be done during normal game-play, if you can avoid it.
========================
*/
void idImage::AllocImage()
{
#if defined(__vita__)
    Vita_CheckGLThread("AllocImage", GetName());
#endif
    GL_CheckErrors();
    PurgeImage();

    switch (opts.format) {
    case FMT_RGBA8:
        internalFormat = GL_RGBA8;
        dataFormat = GL_RGBA;
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_XRGB8:
        internalFormat = GL_RGB;
        dataFormat = GL_RGBA;
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_RGB565:
        internalFormat = GL_RGB;
        dataFormat = GL_RGB;
        dataType = GL_UNSIGNED_SHORT_5_6_5;
        break;
    case FMT_ALPHA:
#if defined(__vita__)
        // vitaGL maps GL_ALPHA to GXM's U8_R111. GXM writes its swizzle letters
        // in A,B,G,R order, so U8_R111 samples as (1,1,1,A) - which is exactly
        // the swizzle the desktop path sets by hand below (R,G,B = ONE,
        // A = RED), and exactly what "alpha lookup table with a white color
        // channel" wants. One byte a texel, and no GL_TEXTURE_SWIZZLE_* needed,
        // which matters because GLES2 does not have it.
        internalFormat = GL_ALPHA;
        dataFormat = GL_ALPHA;
#elif defined(USE_CORE_PROFILE)
        internalFormat = GL_R8;
        dataFormat = GL_RED;
#else
        // Deliberately left sized on the Vita, unlike the three formats below.
        // vitaGL maps GL_ALPHA to GXM's U8_R111, which samples as (A,1,1,1)
        // rather than GL's (0,0,0,A), so switching would trade four bytes a
        // texel for a different wrong swizzle. Falling through vitaGL's switch
        // to the RGBA8 default at least leaves the byte readable in .r, which
        // is where the unsized form would put it too. Worth revisiting once a
        // frame can actually be looked at.
        internalFormat = GL_ALPHA8;
        dataFormat = GL_ALPHA;
#endif
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_L8A8:
#if defined(__vita__)
        // GLES2 takes unsized internal formats, and vitaGL only recognises the
        // unsized names: GL_LUMINANCE8_ALPHA8 falls through its switch to the
        // RGBA8 default, which costs four bytes a texel instead of two and
        // loses the (L,L,L,A) swizzle. GL_LUMINANCE_ALPHA maps to GXM's A8L8
        // == U8U8_GRRR, which samples as (L,L,L,A).
        internalFormat = GL_LUMINANCE_ALPHA;
        dataFormat = GL_LUMINANCE_ALPHA;
#elif defined(USE_CORE_PROFILE)
        internalFormat = GL_RG8;
        dataFormat = GL_RG;
#else
        internalFormat = GL_LUMINANCE8_ALPHA8;
        dataFormat = GL_LUMINANCE_ALPHA;
#endif
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_LUM8:
#if defined(__vita__)
        // GXM L8 == U8_1RRR, sampled as (L,L,L,1). See FMT_L8A8 above.
        internalFormat = GL_LUMINANCE;
        dataFormat = GL_LUMINANCE;
#elif defined(USE_CORE_PROFILE)
        internalFormat = GL_R8;
        dataFormat = GL_RED;
#else
        internalFormat = GL_LUMINANCE8;
        dataFormat = GL_LUMINANCE;
#endif
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_INT8:
#if defined(__vita__)
        // GXM U8_RRRR, sampled as (I,I,I,I), which is exactly what GL_INTENSITY
        // means. See FMT_L8A8 above.
        internalFormat = GL_INTENSITY;
        dataFormat = GL_LUMINANCE;
#elif defined(USE_CORE_PROFILE)
        internalFormat = GL_R8;
        dataFormat = GL_RED;
#else
        internalFormat = GL_INTENSITY8;
        dataFormat = GL_LUMINANCE;
#endif
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_DXT1:
#if defined(__vita__)
        // CFM_GREEN_ALPHA images are transcoded to BC4 in SubImageUpload; see
        // "Vita texture upload fixes" at the top of this file.
        internalFormat = VitaUseBC4Coverage(opts) ? VGL_COMPRESSED_ALPHA_BC4 : GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
#else
        internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
#endif
        dataFormat = GL_RGBA;
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_DXT5:
        internalFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        dataFormat = GL_RGBA;
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_DEPTH:
        internalFormat = GL_DEPTH_COMPONENT;
        dataFormat = GL_DEPTH_COMPONENT;
        dataType = GL_UNSIGNED_BYTE;
        break;
    case FMT_X16:
        internalFormat = GL_INTENSITY16;
        dataFormat = GL_LUMINANCE;
        dataType = GL_UNSIGNED_SHORT;
        break;
    case FMT_Y16_X16:
        internalFormat = GL_LUMINANCE16_ALPHA16;
        dataFormat = GL_LUMINANCE_ALPHA;
        dataType = GL_UNSIGNED_SHORT;
        break;
    default:
        idLib::Error("Unhandled image format %d in %s\n", opts.format, GetName());
    }

    // if we don't have a rendering context, just return after we
    // have filled in the parms.  We must have the values set, or
    // an image match from a shader before OpenGL starts would miss
    // the generated texture
    if (!R_IsInitialized()) {
        return;
    }

    // generate the texture number
    qglGenTextures(1, (GLuint*)&texnum);
    assert(texnum != TEXTURE_NOT_LOADED);

    //----------------------------------------------------
    // allocate all the mip levels with NULL data
    //----------------------------------------------------

    int numSides;
    int target;
    int uploadTarget;
    if (opts.textureType == TT_2D) {
        target = uploadTarget = GL_TEXTURE_2D;
        numSides = 1;
    } else if (opts.textureType == TT_CUBIC) {
        target = GL_TEXTURE_CUBE_MAP_EXT;
        uploadTarget = GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT;
        numSides = 6;
    } else {
        assert(!"opts.textureType");
        target = uploadTarget = GL_TEXTURE_2D;
        numSides = 1;
    }

    qglBindTexture(target, texnum);

    for (int side = 0; side < numSides; side++) {
        int w = opts.width;
        int h = opts.height;
        if (opts.textureType == TT_CUBIC) {
            h = w;
        }
        for (int level = 0; level < opts.numLevels; level++) {

            // clear out any previous error
            GL_CheckErrors();

            if (IsCompressed()) {
                int compressedSize = (((w + 3) / 4) * ((h + 3) / 4) * int64(16) * BitsForFormat(opts.format)) / 8;

                // Even though the OpenGL specification allows the 'data' pointer to be NULL, for some
                // drivers we actually need to upload data to get it to allocate the texture.
                // However, on 32-bit systems we may fail to allocate a large block of memory for large
                // textures. We handle this case by using HeapAlloc directly and allowing the allocation
                // to fail in which case we simply pass down NULL to glCompressedTexImage2D and hope for the best.
                // As of 2011-10-6 using NVIDIA hardware and drivers we have to allocate the memory with HeapAlloc
                // with the exact size otherwise large image allocation (for instance for physical page textures)
                // may fail on Vista 32-bit.

                // RB begin
#if defined(_WIN32)
                void* data = HeapAlloc(GetProcessHeap(), 0, compressedSize);
                qglCompressedTexImage2DARB(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, data);
                if (data != NULL) {
                    HeapFree(GetProcessHeap(), 0, data);
                }
#elif defined(__vita__)
                // vitaGL allocates (and zeroes) the level for NULL; a scratch
                // buffer only made it swizzle garbage into every level of every
                // texture before the real upload swizzled the data again
                qglCompressedTexImage2DARB(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, NULL);
#else
                byte* data = (byte*)Mem_Alloc(compressedSize, TAG_TEMP);
                qglCompressedTexImage2DARB(uploadTarget + side, level, internalFormat, w, h, 0, compressedSize, data);
                if (data != NULL) {
                    Mem_Free(data);
                }
#endif
                // RB end
            } else {
                qglTexImage2D(uploadTarget + side, level, internalFormat, w, h, 0, dataFormat, dataType, NULL);
            }

            GL_CheckErrors();

            w = Max(1, w >> 1);
            h = Max(1, h >> 1);
        }
    }

#if !defined(__vita__)
    // GL_TEXTURE_MAX_LEVEL does not exist in GLES2 and vitaGL rejects it with
    // GL_INVALID_ENUM. The mip chain is fully specified by the levels uploaded.
    qglTexParameteri(target, GL_TEXTURE_MAX_LEVEL, opts.numLevels - 1);
#endif

    // see if we messed anything up
    GL_CheckErrors();

    SetTexParameters();

    GL_CheckErrors();
}

/*
========================
idImage::PurgeImage
========================
*/
void idImage::PurgeImage()
{
    if (texnum != TEXTURE_NOT_LOADED) {
        qglDeleteTextures(1, (GLuint*)&texnum); // this should be the ONLY place it is ever called!
        texnum = TEXTURE_NOT_LOADED;
    }
    // clear all the current binding caches, so the next bind will do a real one
    for (int i = 0; i < MAX_MULTITEXTURE_UNITS; i++) {
        backEnd.glState.tmu[i].current2DMap = TEXTURE_NOT_LOADED;
        backEnd.glState.tmu[i].currentCubeMap = TEXTURE_NOT_LOADED;
    }
}

/*
========================
idImage::Resize
========================
*/
void idImage::Resize(int width, int height)
{
    if (opts.width == width && opts.height == height) {
        return;
    }
    opts.width = width;
    opts.height = height;
    AllocImage();
}
