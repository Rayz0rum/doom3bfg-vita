/*
===========================================================================
Boot-time GPU self-test and on-screen colour sampling for the PS Vita port.

Every rendering bug in this port so far has been diagnosed from photographs
of the handheld, and photographs are a poor instrument: exposure, white
balance and screen angle all change the colours, and "is this green?" has no
numeric answer. This file replaces that with measurements written to
console.txt.

Vita_RunSelfTest() runs once, right after the GL context comes up and before
the engine generates any shader. It does three things:

  1. Uploads small mipmapped textures with a different solid colour in each
     level and reads vitaGL's texture memory back, level by level. This is a
     CPU-only check of the mip-chain addressing in vitaGL's glTexSubImage2D,
     which used to paste every mip over the corner of level 0.

  2. Renders known textures - a DXT5 block, a DXT1 block, RGB565 with and
     without the byte swap, BC4 coverage, GL_ALPHA - through vitaGL's own
     GLSL-to-Cg translator into an offscreen framebuffer, waits for the GPU
     and reads the pixels back. Each one is compared with the value the
     format is supposed to deliver.

  3. Runs the engine's YCoCg-DXT5 decode twice, once exactly as the engine
     generates it (with the compound "YCoCg. xy *= YCoCg. z ;") and once
     with that statement expanded, and compares both against the decode
     computed on the CPU from the texel that step 2 measured. Which of the
     two is correct on this GPU decides how RenderProgs_GLSL.cpp emits the
     interaction shaders.

Each result is printed with the expected and the measured RGBA, so the log
answers the question by itself.

Vita_ScreenSampleAfterSwap() runs after every buffer swap. When a sample is
due it reads a 16x9 grid of pixels back from the display and prints the mean
colour, the share of black pixels, the share of strongly green and strongly
magenta pixels, and the mean green-minus-magenta offset. A correctly lit
d3xpdm3 is dark, desaturated brown metal: near-zero green/magenta offset.
===========================================================================
*/

#include <vitaGL.h>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "vita_public.h"

// The build force-includes idlib's precompiled.h, whose Str.h redefines the C
// printf family to steer engine code towards idStr. This is plain platform
// code with no idStr in it, so use the real ones.
#undef snprintf
#undef vsnprintf

vitaSelfTest_t vitaSelfTest = {
    false,  // ran
    -1, -1, // ycocgCompound, ycocgExpanded
    -1,     // dxt5Raw
    -1,     // dxt1Raw
    -1,     // rgb565Swapped
    -1,     // bc4Coverage
    -1,     // mipLevels
    0, 0,   // passes, failures
};

static void ST_Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Vita_Checkpoint("selftest: %s", buf);
}

static void ST_Verdict(int* slot, bool pass)
{
    if (slot != NULL) {
        *slot = pass ? 1 : 0;
    }
    if (pass) {
        vitaSelfTest.passes++;
    } else {
        vitaSelfTest.failures++;
    }
}

/*
========================
Vita_LinearMipOffset

Byte offset of mip `level` inside a linear (uncompressed) vitaGL texture, and
its row pitch. This mirrors gpu_get_linear_mip_offset() in vitaGL's
utils/gpu_utils.c and the layout gpu_alloc_mipmaps() builds: level j starts
after sum(MAX(po2w >> i, 8) * (po2h >> i) * bpp) for i < j, rows are
VGL_ALIGN(w >> j, 8) texels apart, and the chain stops when either
power-of-two dimension reaches 1. Returns -1 for a level vitaGL never
allocated.
========================
*/
static unsigned int ST_Po2(unsigned int v)
{
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

int Vita_LinearMipOffset(int level, int baseW, int baseH, int bpp, int* pitch, int* levelCount)
{
    unsigned int pw = ST_Po2((unsigned int)baseW);
    unsigned int ph = ST_Po2((unsigned int)baseH);
    unsigned int size = 0;
    int offset = -1;
    int n = 0;
    while (pw > 1 && ph > 1) {
        if (n == level) {
            offset = (int)size;
        }
        size += (pw > 8 ? pw : 8) * ph * bpp;
        pw /= 2;
        ph /= 2;
        n++;
    }
    if (n == level) {
        offset = (int)size;
    }
    n++;
    if (levelCount != NULL) {
        *levelCount = n;
    }
    if (pitch != NULL) {
        int w = level == 0 ? baseW : ((baseW & ~1) >> level);
        if (w < 1) {
            w = 1;
        }
        *pitch = ((w + 7) & ~7) * bpp;
    }
    return offset;
}

/*
===========================================================================
Step 1: mip-chain addressing, checked in texture memory
===========================================================================
*/
// `settle` true: wait for the transfer queue between allocating the levels and
// uploading them. glTexImage2D(level > 0, NULL) makes vitaGL regenerate the
// mip chain with sceGxmTransferDownscale(), which runs asynchronously; without
// the wait those downscales can land after the CPU upload and replace the
// uploaded levels with a downscale of level 0. That is a property of vitaGL,
// not of the addressing being tested, so the verdict comes from the settled
// run and the unsettled one is reported for information.
static bool ST_MipTest(const char* label, GLenum internalFormat, GLenum format, GLenum type, int bpp,
    void (*fill)(unsigned char* texel, int level), bool settle)
{
    const int base = 16;
    const int levels = 5; // 16, 8, 4, 2, 1
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    for (int l = 0; l < levels; l++) {
        const int d = base >> l;
        glTexImage2D(GL_TEXTURE_2D, l, internalFormat, d, d, 0, format, type, NULL);
    }

    if (settle) {
        sceGxmTransferFinish();
    }

    unsigned char* src = (unsigned char*)malloc(base * base * bpp);
    for (int l = 0; l < levels; l++) {
        const int d = base >> l;
        for (int i = 0; i < d * d; i++) {
            fill(src + i * bpp, l);
        }
        glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, d, d, format, type, src);
    }

    const unsigned char* gpu = (const unsigned char*)vglGetTexDataPointer(GL_TEXTURE_2D);
    bool allOk = (gpu != NULL);
    if (gpu == NULL) {
        ST_Log("mip %s: texture memory pointer is NULL", label);
    }
    for (int l = 0; gpu != NULL && l < levels; l++) {
        const int d = base >> l;
        int pitch = 0;
        int vitaLevels = 0;
        const int off = Vita_LinearMipOffset(l, base, base, bpp, &pitch, &vitaLevels);
        unsigned char want[4] = { 0, 0, 0, 0 };
        fill(want, l);
        if (off < 0) {
            ST_Log("mip %s L%d %dx%d: vitaGL allocated only %d levels, nothing to check", label, l, d, d, vitaLevels);
            continue;
        }
        // First and last texel of the level's first row, and first texel of
        // its last row - a pasted-over level 0 differs at all three.
        const unsigned char* t0 = gpu + off;
        const unsigned char* t1 = gpu + off + (d - 1) * bpp;
        const unsigned char* t2 = gpu + off + (d - 1) * pitch;
        const bool ok = memcmp(t0, want, bpp) == 0 && memcmp(t1, want, bpp) == 0 && memcmp(t2, want, bpp) == 0;
        allOk = allOk && ok;
        char w[16], g[16];
        snprintf(w, sizeof(w), "%02X%02X%02X%02X", want[0], bpp > 1 ? want[1] : 0, bpp > 2 ? want[2] : 0, bpp > 3 ? want[3] : 0);
        snprintf(g, sizeof(g), "%02X%02X%02X%02X", t0[0], bpp > 1 ? t0[1] : 0, bpp > 2 ? t0[2] : 0, bpp > 3 ? t0[3] : 0);
        ST_Log("mip %s %s L%d %dx%d @+%d pitch %d: want %.*s got %.*s/%02X/%02X -> %s",
            label, settle ? "settled  " : "as-engine", l, d, d, off, pitch, bpp * 2, w, bpp * 2, g, t1[0], t2[0],
            ok ? (settle ? "PASS" : "intact") : (settle ? "FAIL" : "overwritten by vitaGL's async downscale"));
    }
    free(src);
    glDeleteTextures(1, &tex);

    if (settle) {
        ST_Log("mip %s: %s", label, allOk ? "PASS - every level lands where vitaGL samples it"
                                          : "FAIL - mip levels are not where vitaGL samples them");
        if (vitaSelfTest.mipLevels != 0) {
            vitaSelfTest.mipLevels = allOk ? 1 : 0;
        }
        ST_Verdict(NULL, allOk);
    } else {
        ST_Log("mip %s: %s", label, allOk ? "uploaded levels survived (no downscale landed late this time)"
                                          : "some uploaded levels were replaced by a downscale of level 0 - harmless for real textures, whose mips are downscales of level 0 anyway");
    }
    return allOk;
}

static void ST_FillRGBA(unsigned char* t, int l)
{
    t[0] = (unsigned char)(10 + l * 50);
    t[1] = (unsigned char)(250 - l * 40);
    t[2] = (unsigned char)(l * 20);
    t[3] = 200;
}

static void ST_FillRGB565(unsigned char* t, int l)
{
    // Already little-endian, i.e. what reaches vitaGL after the engine's
    // byte swap. Distinct R, G and B fields per level.
    const unsigned short c = (unsigned short)(((3 + l * 5) << 11) | ((60 - l * 9) << 5) | (l * 6));
    t[0] = (unsigned char)(c & 0xFF);
    t[1] = (unsigned char)(c >> 8);
}

static void ST_FillI8(unsigned char* t, int l)
{
    t[0] = (unsigned char)(0x20 + l * 0x30);
}

/*
===========================================================================
Steps 2 and 3: sample through the real shader pipeline, read back
===========================================================================
*/

// Vertex shader shared by every probe. Written in the shape the engine's
// Cg->GLSL converter emits, and it uses the engine's own attribute and
// varying names so vitaGL's global semantic binding table (keyed by name and
// never cleared) ends up exactly as it would have been without the test.
static const char* const ST_VS =
    "#version 100\n"
    "precision mediump float;\n"
    "attribute vec4 attr_Vertex;\n"
    "varying vec4 vofi_TexCoord0;\n"
    "void main() {\n"
    "\tgl_Position = attr_Vertex ;\n"
    "\tvofi_TexCoord0 = vec4 ( 0.5 , 0.5 , 0.0 , 1.0 ) ;\n"
    "}\n";

static const char* const ST_FS_RAW =
    "#version 100\n"
    "precision mediump float;\n"
    "uniform sampler2D samp0;\n"
    "varying vec4 vofi_TexCoord0;\n"
    "void main() {\n"
    "\tgl_FragColor = tex2D ( samp0 , vofi_TexCoord0 . xy ) ;\n"
    "}\n";

// ConvertYCoCgToRGB exactly as RenderProgs_GLSL.cpp generates it from
// base/renderprogs/global.inc, spacing included. @MUL@ is replaced with the
// compound statement or its expansion.
static const char* const ST_FS_YCOCG =
    "#version 100\n"
    "precision mediump float;\n"
    "float dot4 (vec4 a , vec4 b ) {return dot ( a , b ) ; }\n"
    "const vec4 matrixCoCg1YtoRGB1X = vec4( 1.0, -1.0, 0.0, 1.0 );\n"
    "const vec4 matrixCoCg1YtoRGB1Y = vec4( 0.0, 1.0, -0.50196078, 1.0 );\n"
    "const vec4 matrixCoCg1YtoRGB1Z = vec4( -1.0, -1.0, 1.00392156, 1.0 );\n"
    "vec3 ConvertYCoCgToRGB (vec4 YCoCg ) {\n"
    "\tvec3 rgbColor ;\n"
    "\tYCoCg. z = ( YCoCg. z * 31.875 ) + 1.0 ;\n"
    "\tYCoCg. z = 1.0 / YCoCg. z ;\n"
    "@MUL@\n"
    "\trgbColor. x = dot4 ( YCoCg , matrixCoCg1YtoRGB1X ) ;\n"
    "\trgbColor. y = dot4 ( YCoCg , matrixCoCg1YtoRGB1Y ) ;\n"
    "\trgbColor. z = dot4 ( YCoCg , matrixCoCg1YtoRGB1Z ) ;\n"
    "\treturn rgbColor ;\n"
    "}\n"
    "uniform sampler2D samp0;\n"
    "varying vec4 vofi_TexCoord0;\n"
    "void main() {\n"
    "\tvec4 YCoCG = tex2D ( samp0 , vofi_TexCoord0 . xy ) ;\n"
    "\tvec3 diffuseMap = ConvertYCoCgToRGB ( YCoCG ) ;\n"
    "\tgl_FragColor . xyz = diffuseMap ;\n"
    "\tgl_FragColor . w = 1.0 ;\n"
    "}\n";

static void ST_LogMultiline(const char* prefix, const char* text)
{
    if (text == NULL || text[0] == 0) {
        return;
    }
    char line[256];
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > (int)sizeof(line) - 1) {
            len = sizeof(line) - 1;
        }
        memcpy(line, p, len);
        line[len] = 0;
        if (len > 0) {
            ST_Log("%s%s", prefix, line);
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

static GLuint ST_Compile(GLenum type, const char* src, const char* name)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = GL_FALSE;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    GLint logLen = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 1) {
        char* log = (char*)malloc(logLen + 1);
        glGetShaderInfoLog(s, logLen, NULL, log);
        log[logLen] = 0;
        ST_Log("shader %s: compiler said:", name);
        ST_LogMultiline("    | ", log);
        free(log);
    }
    ST_Log("shader %s (%s): compile %s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", ok ? "ok" : "FAILED");
    if (!ok) {
        ST_LogMultiline("    > ", src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint ST_Program(GLuint vs, const char* fsSrc, const char* name)
{
    GLuint fs = ST_Compile(GL_FRAGMENT_SHADER, fsSrc, name);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "attr_Vertex");
    glLinkProgram(p);
    GLint ok = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    GLint nUnif = 0;
    glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &nUnif);
    const GLint loc = glGetUniformLocation(p, "samp0");
    ST_Log("program %s: link %s, %d active uniform(s), samp0 location %d", name, ok ? "ok" : "FAILED", nUnif, loc);
    if (!ok) {
        return 0;
    }
    glUseProgram(p);
    if (loc != -1) {
        glUniform1i(loc, 0);
    }
    return p;
}

struct stProbe_t {
    const char* name;
    GLuint texture;
    GLuint program;
    int expect[4];   // -1 = don't care
    int tolerance;
    int* verdict;    // vitaSelfTest slot, may be NULL
    int got[4];
    bool drawn;
};

static bool ST_Close(const int* got, const int* want, int tol)
{
    for (int c = 0; c < 4; c++) {
        if (want[c] < 0) {
            continue;
        }
        const int d = got[c] - want[c];
        if (d > tol || d < -tol) {
            return false;
        }
    }
    return true;
}

static GLuint ST_MakeTexture()
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int ST_Expand5(int v)
{
    return (v << 3) | (v >> 2);
}
static int ST_Expand6(int v)
{
    return (v << 2) | (v >> 4);
}

static int ST_ToByte(float f)
{
    if (f < 0.0f) {
        f = 0.0f;
    }
    if (f > 1.0f) {
        f = 1.0f;
    }
    return (int)(f * 255.0f + 0.5f);
}

// What ConvertYCoCgToRGB must produce for raw texel (Co, Cg, scale, Y),
// computed in float on the CPU. `applyScale` false models the multiply
// being dropped, `zeroY` models a dead alpha channel.
static void ST_DecodeYCoCg(const int* raw, bool applyScale, bool zeroY, int* out)
{
    float co = raw[0] / 255.0f;
    float cg = raw[1] / 255.0f;
    float s = raw[2] / 255.0f;
    float y = zeroY ? 0.0f : raw[3] / 255.0f;
    float z = 1.0f / (s * 31.875f + 1.0f);
    if (applyScale) {
        co *= z;
        cg *= z;
    }
    out[0] = ST_ToByte(co - cg + y);
    out[1] = ST_ToByte(cg - 0.50196078f * z + y);
    out[2] = ST_ToByte(-co - cg + 1.00392156f * z + y);
    out[3] = 255;
}

// vitaGL's display depth/stencil surface (source/gxm.c).
extern "C" SceGxmDepthStencilSurface gxm_depth_stencil_surface;

void Vita_RunSelfTest()
{
    Vita_Checkpoint("selftest: ===== begin =====");
    vitaSelfTest.ran = true;

    // Framebuffer copies split the display scene in two (see
    // VitaBlitFramebufferToImage). Depth and stencil only survive that if the
    // display surface loads and stores them; otherwise everything drawn after
    // a copy ignores the geometry drawn before it.
    {
        const bool load = sceGxmDepthStencilSurfaceGetForceLoadMode(&gxm_depth_stencil_surface) == SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED;
        const bool store = sceGxmDepthStencilSurfaceGetForceStoreMode(&gxm_depth_stencil_surface) == SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED;
        ST_Log("display depth/stencil: force-load %s, force-store %s -> %s", load ? "ENABLED" : "disabled", store ? "ENABLED" : "disabled",
            (load && store) ? "PASS - depth survives a mid-frame scene split" : "FAIL - surfaces drawn after a framebuffer copy will show through walls");
        ST_Verdict(NULL, load && store);
    }

    vitaSelfTest.mipLevels = -1;

    /* ---- step 1 + 2 setup ------------------------------------------ */
    Vita_Checkpoint("selftest: step 1/3 building offscreen target and probe shaders");

    enum { PROBES = 10, CELL = 8 };
    const int fbW = PROBES * CELL;
    const int fbH = CELL;

    GLuint fbTex = 0;
    glGenTextures(1, &fbTex);
    glBindTexture(GL_TEXTURE_2D, fbTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbW, fbH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbTex, 0);
    const GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    ST_Log("offscreen target %dx%d: status 0x%04X%s", fbW, fbH, fbStatus,
        fbStatus == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (NOT complete)");

    GLuint vs = ST_Compile(GL_VERTEX_SHADER, ST_VS, "probe.vs");
    GLuint progRaw = ST_Program(vs, ST_FS_RAW, "raw");

    char ycocgSrc[2048];
    const char* compound = "\tYCoCg. xy *= YCoCg. z ;";
    const char* expanded = "\tYCoCg. xy = YCoCg. xy * ( YCoCg. z ) ;";
    {
        const char* at = strstr(ST_FS_YCOCG, "@MUL@");
        const int head = (int)(at - ST_FS_YCOCG);
        snprintf(ycocgSrc, sizeof(ycocgSrc), "%.*s%s%s", head, ST_FS_YCOCG, compound, at + 5);
    }
    GLuint progCompound = ST_Program(vs, ycocgSrc, "ycocg-compound");
    {
        const char* at = strstr(ST_FS_YCOCG, "@MUL@");
        const int head = (int)(at - ST_FS_YCOCG);
        snprintf(ycocgSrc, sizeof(ycocgSrc), "%.*s%s%s", head, ST_FS_YCOCG, expanded, at + 5);
    }
    GLuint progExpanded = ST_Program(vs, ycocgSrc, "ycocg-expanded");

    /* ---- test textures -------------------------------------------- */
    // DXT5 (BC3): alpha 80 in the alpha block, colour RGB565 (23,30,1) =
    // Co 189, Cg 121, scale 8 once expanded - a brown texel at scale 2,
    // which is what a YCoCg encoder produces for grey-brown metal.
    static const unsigned char dxt5Block[16] = {
        80, 80, 0, 0, 0, 0, 0, 0,
        0xC1, 0xBB, 0xC1, 0xBB, 0, 0, 0, 0 };
    GLuint texDXT5 = ST_MakeTexture();
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 4, 4, 0, 16, dxt5Block);

    // DXT1 (BC1): RGB565 (31,0,15) = 255,0,123 opaque.
    static const unsigned char dxt1Block[8] = { 0x0F, 0xF8, 0x0F, 0xF8, 0, 0, 0, 0 };
    GLuint texDXT1 = ST_MakeTexture();
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 4, 4, 0, 8, dxt1Block);

    // RGB565 grey 0x8410 = (16,32,16), stored big-endian the way
    // BinaryImage.cpp writes every RGB565 .bimage (high byte first).
    unsigned char rgb565BE[32], rgb565LE[32];
    for (int i = 0; i < 16; i++) {
        rgb565BE[i * 2 + 0] = 0x84;
        rgb565BE[i * 2 + 1] = 0x10;
        rgb565LE[i * 2 + 0] = 0x10;
        rgb565LE[i * 2 + 1] = 0x84;
    }
    GLuint tex565Swapped = ST_MakeTexture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565LE);
    GLuint tex565Raw = ST_MakeTexture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565BE);

    // BC4, both block modes: e0 <= e1 (six-value, index 0 = e0 = 200) and
    // e0 > e1 (eight-value, index 2 = (6*240 + 16) / 7 = 208).
    static const unsigned char bc4Flat[8] = { 200, 200, 0, 0, 0, 0, 0, 0 };
    // index 2 for all 16 texels: 3-bit pattern 010 repeated -> 48 bits
    // 0x492492492492 little-endian.
    static const unsigned char bc4Ramp[8] = { 240, 16, 0x92, 0x24, 0x49, 0x92, 0x24, 0x49 };
    GLuint texBC4Flat = ST_MakeTexture();
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, VGL_COMPRESSED_ALPHA_BC4, 4, 4, 0, 8, bc4Flat);
    GLuint texBC4Ramp = ST_MakeTexture();
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, VGL_COMPRESSED_ALPHA_BC4, 4, 4, 0, 8, bc4Ramp);

    unsigned char a8[16];
    memset(a8, 0x5A, sizeof(a8));
    GLuint texA8 = ST_MakeTexture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, 4, 4, 0, GL_ALPHA, GL_UNSIGNED_BYTE, a8);

    unsigned char i8[16];
    memset(i8, 0x77, sizeof(i8));
    GLuint texI8 = ST_MakeTexture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_INTENSITY, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, i8);

    const int dxt5Want[4] = { ST_Expand5(23), ST_Expand6(30), ST_Expand5(1), 80 };
    stProbe_t probes[PROBES] = {
        { "DXT5 raw (Co,Cg,scale,Y)", texDXT5, progRaw, { dxt5Want[0], dxt5Want[1], dxt5Want[2], dxt5Want[3] }, 3, &vitaSelfTest.dxt5Raw },
        { "DXT1 raw", texDXT1, progRaw, { 255, 0, ST_Expand5(15), 255 }, 3, &vitaSelfTest.dxt1Raw },
        { "RGB565 swapped (engine path)", tex565Swapped, progRaw, { ST_Expand5(16), ST_Expand6(32), ST_Expand5(16), 255 }, 3, &vitaSelfTest.rgb565Swapped },
        { "RGB565 unswapped (old path)", tex565Raw, progRaw, { -1, -1, -1, -1 }, 0, NULL },
        { "BC4 coverage, 6-value mode", texBC4Flat, progRaw, { 255, 255, 255, 200 }, 3, NULL },
        { "BC4 coverage, 8-value mode", texBC4Ramp, progRaw, { 255, 255, 255, 208 }, 3, NULL },
        { "GL_ALPHA", texA8, progRaw, { 255, 255, 255, 0x5A }, 3, NULL },
        { "GL_INTENSITY", texI8, progRaw, { 0x77, 0x77, 0x77, 0x77 }, 3, NULL },
        { "YCoCg decode, compound *=", texDXT5, progCompound, { -1, -1, -1, -1 }, 4, &vitaSelfTest.ycocgCompound },
        { "YCoCg decode, expanded", texDXT5, progExpanded, { -1, -1, -1, -1 }, 4, &vitaSelfTest.ycocgExpanded },
    };

    /* ---- draw ------------------------------------------------------ */
    const bool canDraw = (fbStatus == GL_FRAMEBUFFER_COMPLETE) && vs != 0;
    if (canDraw) {
        Vita_Checkpoint("selftest: drawing %d probes", PROBES);
        static const float quad[16] = {
            -1.0f, -1.0f, 0.0f, 1.0f,
            1.0f, -1.0f, 0.0f, 1.0f,
            -1.0f, 1.0f, 0.0f, 1.0f,
            1.0f, 1.0f, 0.0f, 1.0f };
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);

        for (int i = 0; i < PROBES; i++) {
            if (probes[i].program == 0) {
                continue;
            }
            glViewport(i * CELL, 0, CELL, CELL);
            glUseProgram(probes[i].program);
            glBindTexture(GL_TEXTURE_2D, probes[i].texture);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            probes[i].drawn = true;
        }

        // vglSwapBuffers() with an FBO bound ends the GXM scene without
        // queueing anything for display; only after that is sceGxmFinish()
        // (glFinish) legal. Calling it inside a scene wedges the GPU - that is
        // what hung the video player earlier in this port.
        Vita_Checkpoint("selftest: ending offscreen scene");
        vglSwapBuffers(GL_FALSE);
        Vita_Checkpoint("selftest: waiting for the GPU");
        glFinish();
        Vita_Checkpoint("selftest: reading back");

        unsigned char* px = (unsigned char*)malloc(fbW * fbH * 4);
        memset(px, 0xEE, fbW * fbH * 4);
        glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (int i = 0; i < PROBES; i++) {
            const unsigned char* c = px + ((CELL / 2) * fbW + i * CELL + CELL / 2) * 4;
            for (int k = 0; k < 4; k++) {
                probes[i].got[k] = c[k];
            }
        }
        free(px);

        glDisableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteBuffers(1, &vbo);
    } else {
        ST_Log("cannot draw probes (framebuffer status 0x%04X, vertex shader %u)", fbStatus, vs);
    }

    /* ---- verdicts -------------------------------------------------- */
    // The YCoCg expectations come from the texel step 2 actually measured, so
    // a sampler that expands RGB565 slightly differently cannot fake a
    // shader failure.
    const int* raw = probes[0].got;
    int good[4], dropped[4], deadY[4];
    ST_DecodeYCoCg(raw, true, false, good);
    ST_DecodeYCoCg(raw, false, false, dropped);
    ST_DecodeYCoCg(raw, true, true, deadY);
    for (int k = 0; k < 4; k++) {
        probes[8].expect[k] = good[k];
        probes[9].expect[k] = good[k];
    }

    Vita_Checkpoint("selftest: step 2/3 results (expected vs measured RGBA, 0-255)");
    for (int i = 0; i < PROBES; i++) {
        stProbe_t& p = probes[i];
        if (!p.drawn) {
            ST_Log("%-30s NOT RUN", p.name);
            if (p.verdict != NULL) {
                *p.verdict = -1;
            }
            continue;
        }
        if (p.expect[0] < 0 && p.expect[3] < 0) {
            ST_Log("%-30s got (%3d,%3d,%3d,%3d)  [reference only]", p.name, p.got[0], p.got[1], p.got[2], p.got[3]);
            continue;
        }
        const bool pass = ST_Close(p.got, p.expect, p.tolerance);
        ST_Log("%-30s want (%3d,%3d,%3d,%3d) got (%3d,%3d,%3d,%3d) -> %s", p.name,
            p.expect[0], p.expect[1], p.expect[2], p.expect[3],
            p.got[0], p.got[1], p.got[2], p.got[3], pass ? "PASS" : "FAIL");
        ST_Verdict(p.verdict, pass);
        if (i == 4 || i == 5) {
            // Both BC4 modes must decode for the coverage transcoder to be safe.
            vitaSelfTest.bc4Coverage = (vitaSelfTest.bc4Coverage != 0 && pass) ? 1 : 0;
        }
    }

    // Name the hypothesis each YCoCg result matches, so nobody has to do the
    // arithmetic from the log.
    for (int i = 8; i <= 9; i++) {
        stProbe_t& p = probes[i];
        if (!p.drawn) {
            continue;
        }
        const char* match = "none of the known models";
        if (ST_Close(p.got, good, 4)) {
            match = "a CORRECT decode";
        } else if (ST_Close(p.got, dropped, 4)) {
            match = "the chroma scale being DROPPED";
        } else if (ST_Close(p.got, deadY, 4)) {
            match = "the luma (DXT5 alpha) reading ZERO";
        }
        ST_Log("%s matches %s  [correct (%d,%d,%d) | scale dropped (%d,%d,%d) | luma zero (%d,%d,%d)]",
            p.name, match, good[0], good[1], good[2], dropped[0], dropped[1], dropped[2], deadY[0], deadY[1], deadY[2]);
    }
    if (probes[3].drawn) {
        ST_Log("RGB565 without the swap samples as (%d,%d,%d) instead of grey (%d,%d,%d): this is what every light projection texture looked like before",
            probes[3].got[0], probes[3].got[1], probes[3].got[2], ST_Expand5(16), ST_Expand6(32), ST_Expand5(16));
    }

    /* ---- step 3: mip-chain addressing --------------------------------
       Here rather than first: the offscreen scene has been ended and the GPU
       drained, so waiting on the transfer queue cannot deadlock against an
       open scene. */
    Vita_Checkpoint("selftest: step 3/3 mip-chain addressing (CPU readback of texture memory)");
    if (canDraw) {
        ST_MipTest("RGBA8 ", GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, 4, ST_FillRGBA, true);
        ST_MipTest("RGB565", GL_RGB, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, ST_FillRGB565, true);
        ST_MipTest("I8    ", GL_INTENSITY, GL_LUMINANCE, GL_UNSIGNED_BYTE, 1, ST_FillI8, true);
        ST_MipTest("RGBA8 ", GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, 4, ST_FillRGBA, false);
    } else {
        ST_Log("mip tests skipped: the offscreen scene never ran, so it is not known to be safe to wait on the GPU");
    }

    /* ---- clean up -------------------------------------------------- */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLuint texs[] = { texDXT5, texDXT1, tex565Swapped, tex565Raw, texBC4Flat, texBC4Ramp, texA8, texI8, fbTex };
    glDeleteTextures(sizeof(texs) / sizeof(texs[0]), texs);
    glDeleteFramebuffers(1, &fbo);
    if (progRaw) {
        glDeleteProgram(progRaw);
    }
    if (progCompound) {
        glDeleteProgram(progCompound);
    }
    if (progExpanded) {
        glDeleteProgram(progExpanded);
    }

    Vita_Checkpoint("selftest: ===== end: %d passed, %d failed | ycocg compound %d expanded %d | dxt5 %d dxt1 %d rgb565 %d bc4 %d mips %d (1 pass, 0 fail, -1 not run) =====",
        vitaSelfTest.passes, vitaSelfTest.failures,
        vitaSelfTest.ycocgCompound, vitaSelfTest.ycocgExpanded,
        vitaSelfTest.dxt5Raw, vitaSelfTest.dxt1Raw, vitaSelfTest.rgb565Swapped,
        vitaSelfTest.bc4Coverage, vitaSelfTest.mipLevels);
}

/*
===========================================================================
Screen sampling
===========================================================================
*/
static int vitaScreenPending = 0;
static int vitaScreenFrame = 0;
static int vitaScreenPeriodic = 0;
static char vitaScreenReason[64] = "";

static int vitaCensusAmbientLights = 0;
static int vitaCensusLights = 0;
static int vitaCensusAmbientDraws = 0;
static int vitaCensusDraws = 0;
// screen area the frame's light passes cover (their scissor rectangles),
// i.e. roughly how many full-screen interaction passes the GPU paid for
static double vitaCensusPixels = 0.0;
static int vitaCensusBiggestPixels = 0;
static char vitaCensusBiggestName[64] = "";
// frame timing between screen samples
static unsigned int vitaFrameLastMs = 0;
static unsigned int vitaFrameSumMs = 0;
static unsigned int vitaFrameWorstMs = 0;
static int vitaFrameCount = 0;

void Vita_CensusLight(bool ambient, int draws, int scissorPixels, const char* name)
{
    if (ambient) {
        vitaCensusAmbientLights++;
        vitaCensusAmbientDraws += draws;
    } else {
        vitaCensusLights++;
        vitaCensusDraws += draws;
    }
    vitaCensusPixels += scissorPixels;
    if (scissorPixels > vitaCensusBiggestPixels) {
        vitaCensusBiggestPixels = scissorPixels;
        snprintf(vitaCensusBiggestName, sizeof(vitaCensusBiggestName), "%s", name ? name : "?");
    }
}

static void Vita_CensusReset()
{
    vitaCensusAmbientLights = vitaCensusLights = vitaCensusAmbientDraws = vitaCensusDraws = 0;
    vitaCensusPixels = 0.0;
    vitaCensusBiggestPixels = 0;
    vitaCensusBiggestName[0] = '\0';
}

void Vita_ScreenSampleRequest(int frames, const char* reason, int periodicFrames)
{
    vitaScreenPending = frames;
    vitaScreenPeriodic = periodicFrames;
    snprintf(vitaScreenReason, sizeof(vitaScreenReason), "%s", reason ? reason : "");
}

void Vita_ScreenSampleAfterSwap()
{
    vitaScreenFrame++;
    {
        const unsigned int now = sceKernelGetProcessTimeLow() / 1000;
        if (vitaFrameLastMs != 0) {
            const unsigned int dt = now - vitaFrameLastMs;
            vitaFrameSumMs += dt;
            vitaFrameCount++;
            if (dt > vitaFrameWorstMs) {
                vitaFrameWorstMs = dt;
            }
        }
        vitaFrameLastMs = now;
    }
    bool due = vitaScreenPending > 0;
    if (!due && vitaScreenPeriodic > 0 && (vitaScreenFrame % vitaScreenPeriodic) == 0) {
        due = true;
    }
    if (!due) {
        Vita_CensusReset();
        return;
    }
    if (vitaScreenPending > 0) {
        vitaScreenPending--;
    }

    // Default framebuffer: vitaGL reads the back buffer, which right after a
    // swap holds an already-completed earlier frame and is not being drawn to.
    enum { GX = 16, GY = 9 };
    int W = 960, H = 544;
    Vita_GetRenderResolution(&W, &H);
    int sum[3] = { 0, 0, 0 };
    int black = 0, green = 0, magenta = 0;
    float gm = 0.0f;
    unsigned char grid[GY][GX][4];
    for (int j = 0; j < GY; j++) {
        for (int i = 0; i < GX; i++) {
            unsigned char* c = grid[j][i];
            c[0] = c[1] = c[2] = c[3] = 0;
            glReadPixels((i * W + W / 2) / GX, (j * H + H / 2) / GY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            const int r = c[0], g = c[1], b = c[2];
            sum[0] += r;
            sum[1] += g;
            sum[2] += b;
            if (r < 8 && g < 8 && b < 8) {
                black++;
            }
            if (g > r + 24 && g > b + 24) {
                green++;
            }
            if (r > g + 24 && b > g + 24) {
                magenta++;
            }
            gm += g - (r + b) * 0.5f;
        }
    }
    const int n = GX * GY;
    const float gmMean = gm / n;
    const char* verdict = "neutral";
    if (black * 100 / n > 90) {
        verdict = "BLACK";
    } else if (gmMean > 12.0f) {
        verdict = "GREEN-TINTED";
    } else if (gmMean < -12.0f) {
        verdict = "MAGENTA-TINTED";
    }
    Vita_Checkpoint("screen [%s] frame %d: mean RGB (%d,%d,%d), black %d%%, green %d%%, magenta %d%%, green-magenta offset %+.1f -> %s | lights %d (%d draws), ambient lights %d (%d draws) since last sample",
        vitaScreenReason, vitaScreenFrame, sum[0] / n, sum[1] / n, sum[2] / n,
        black * 100 / n, green * 100 / n, magenta * 100 / n, gmMean, verdict,
        vitaCensusLights, vitaCensusDraws, vitaCensusAmbientLights, vitaCensusAmbientDraws);
    // 3x3 excerpt, top row first, as RRGGBB.
    char line[256];
    int len = 0;
    for (int jj = 0; jj < 3; jj++) {
        const int j = GY - 1 - (jj * 4); // glReadPixels y is bottom-up
        len += snprintf(line + len, sizeof(line) - len, "%s", jj ? " / " : "");
        for (int ii = 0; ii < 3; ii++) {
            const int i = 2 + ii * 6;
            const unsigned char* c = grid[j < 0 ? 0 : j][i];
            len += snprintf(line + len, sizeof(line) - len, "%s%02X%02X%02X", ii ? " " : "", c[0], c[1], c[2]);
        }
    }
    Vita_Checkpoint("screen [%s] frame %d: 3x3 grid (top row first): %s", vitaScreenReason, vitaScreenFrame, line);
    // what the GPU was asked to do in this frame, and how long frames took since the last sample
    const double screenPx = (double)W * H;
    Vita_Checkpoint("screen [%s] frame %d: %dx%d, light passes cover %.2f screens (biggest '%s' %.0f%% of the screen); "
                    "frames since last sample: avg %.1f ms (%.1f fps), worst %u ms over %d frames",
        vitaScreenReason, vitaScreenFrame, W, H, vitaCensusPixels / screenPx,
        vitaCensusBiggestName[0] ? vitaCensusBiggestName : "-", 100.0 * vitaCensusBiggestPixels / screenPx,
        vitaFrameCount ? (double)vitaFrameSumMs / vitaFrameCount : 0.0,
        vitaFrameSumMs ? 1000.0 * vitaFrameCount / vitaFrameSumMs : 0.0, vitaFrameWorstMs, vitaFrameCount);
    vitaFrameSumMs = vitaFrameWorstMs = 0;
    vitaFrameCount = 0;
    Vita_CensusReset();
}
