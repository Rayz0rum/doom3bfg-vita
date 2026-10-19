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

#include "tr_local.h"

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
#endif

#if defined(__vita__)
// Declared rather than pulled in: vitaGL.h drags in the whole GLES2 header set,
// which collides with the engine's own glext.h. Signature from source/vitaGL.h.
extern "C" void* vglGetTexDataPointer(unsigned int target);
#endif

#if defined(__vita__)
// Loading mars_city2 preloads 2181 images and ends with every pool empty - the
// allocator reported RAM 6 KB, VRAM 1223 KB, phycont 43 KB when a 2.7 MB request
// finally failed. BFG dropped the image_downSize cvars that original DOOM 3 had,
// because its images are pre-baked .bimage files, but those files still carry a
// full mip chain. Dropping the top level and treating mip 1 as the base costs
// nothing to compute and quarters the memory for every texture.
//
// 1 = half each dimension (quarter memory), 2 = quarter (one sixteenth).
idCVar vita_imageDownSize("vita_imageDownSize", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE,
    "Vita: skip this many top mip levels when uploading textures");

#endif

/*
================
R_VitaImageCanShrink

Whether a texture may lose top mip levels on the Vita (vita_imageDownSize).
Only textures sampled by normalised UV qualify. The SWF shell packs artwork
into atlases and converts stored pixel rects using the image's uploaded size,
so halving an atlas made every UV twice too large and each cell sampled its
neighbours (controller artwork over the title screen); fonts, GUI art drawn
pixel for pixel, lookup tables, engine-generated images and cube faces are in
the same category.

Diffuse, bump and specular maps always qualify. "Default" textures - particle
strips, decals, blend and glow layers, model effects, about a quarter of a
campaign map's texture memory - qualify when they have mips (so they are
minified anyway) and do not come from the GUI, SWF or font trees.
================
*/
bool R_ImageIsInterfaceArt(const char* name)
{
    if (name == NULL || name[0] == '_') {
        return true; // engine images
    }
    static const char* const ui[] = { "guis/", "gui/", "ui/", "swf/", "generated/swf", "fonts/", "newfonts/", "video/", "gfx/", "textures/bigchars", "lights/" };
    for (int i = 0; i < (int)(sizeof(ui) / sizeof(ui[0])); i++) {
        if (idStr::Icmpn(name, ui[i], strlen(ui[i])) == 0) {
            return true;
        }
    }
    // image programs wrap their source: look at what they load
    return strstr(name, "guis/") != NULL || strstr(name, "swf/") != NULL || strstr(name, "fonts/") != NULL;
}

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
================
R_VitaImageHasText

Textures with text on them - signs, labels, posters, panels, lettering baked
into walls and props - found offline by tools/vita/text_textures.py, which
decodes every texture of the game and measures what halving it destroys (fine,
high-contrast detail laid out in rows, as lettering is) together with name
hints, and checks its picks by eye on contact sheets. 411 texture stems; a
stem covers all of a sign's maps (colour, normal, specular).

The stem of an image name: the first texture path in it (image programs wrap
theirs: "addnormals(models/x/sign_local, heightmap(...))"), lower case,
without an extension and the _d/_s/_local/... suffixes - as the tool makes it.
================
*/
#include "vita_text_textures.inc"

static void R_VitaImageStem(const char* name, char* out, int outSize)
{
    // the first path: skip an image program's name and its parenthesis
    const char* p = name;
    const char* paren = strchr(name, '(');
    if (paren != NULL) {
        p = paren + 1;
        while (*p == ' ' || *p == '(') {
            p++;
        }
        // nested programs: heightmap(addnormals(...)) - the innermost path
        const char* q = p;
        while (*q && *q != ',' && *q != ')' && *q != ' ') {
            if (*q == '(') {
                p = q + 1;
            }
            q++;
        }
    }
    int n = 0;
    while (*p && *p != ',' && *p != ')' && *p != ' ' && n < outSize - 1) {
        char c = *p++;
        if (c == '\\') {
            c = '/';
        }
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = 0;
    // extension
    char* dot = strrchr(out, '.');
    char* slash = strrchr(out, '/');
    if (dot != NULL && (slash == NULL || dot > slash)) {
        *dot = 0;
        n = (int)strlen(out);
    }
    static const char* const suffixes[] = { "_d", "_s", "_h", "_b", "_bmp", "_local", "_norm", "_n", "_diffuse", "_spec", "_specular", "_add", "_glow",
        "_fx", "_df", "_dif", "_hb", "_disp", "_alpha", "_a" };
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < (int)(sizeof(suffixes) / sizeof(suffixes[0])); i++) {
            const int l = (int)strlen(suffixes[i]);
            if (n > l && strcmp(out + n - l, suffixes[i]) == 0) {
                n -= l;
                out[n] = 0;
                break;
            }
        }
    }
}

bool R_VitaImageHasText(const char* name)
{
    if (name == NULL || name[0] == 0) {
        return false;
    }
    char stem[MAX_OSPATH];
    R_VitaImageStem(name, stem, sizeof(stem));
    int lo = 0, hi = (int)(sizeof(vitaTextTextureOffsets) / sizeof(vitaTextTextureOffsets[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = strcmp(stem, vitaTextTextureNames + vitaTextTextureOffsets[mid]);
        if (c == 0) {
            return true;
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return false;
}
#endif

#if defined(__vita__)
int vita_textureQualityDrop = 0;

/*
================
R_VitaSkipLevels

How many top mip levels a texture loses (request = vita_imageDownSize plus
the memory-pressure drop, or 8 for "the most it may ever lose"):
  - text (R_VitaImageHasText): two fewer - full size normally, and only the
    second memory-pressure step and beyond shrink it, so lettering stays
    readable;
  - specular maps without text: one more - shine is soft and a quarter-size
    specular map is not told apart, and that pays for the text kept sharp
    (measured over all the game's data: +18 MB for text, -15 MB specular);
  - never below 64 pixels a side (it was 32: small labels went to mush).
================
*/
int R_VitaSkipLevels(const char* name, textureUsage_t usage, textureType_t type, int width, int height, int numLevels, int request)
{
    if (request <= 0 || !R_VitaImageCanShrink(name, usage, type, numLevels)) {
        return 0;
    }
    int skip = request;
    if (R_VitaImageHasText(name)) {
        skip = Max(0, request - 2);
    } else if (usage == TD_SPECULAR) {
        skip = request + 1;
    }
    while (skip > 0 && ((numLevels - skip) < 1 || (width >> skip) < 64 || (height >> skip) < 64)) {
        skip--;
    }
    return skip;
}
#endif

bool R_VitaImageCanShrink(const char* name, textureUsage_t usage, textureType_t type, int numLevels)
{
    if (type != TT_2D) {
        return false;
    }
    if (usage == TD_DIFFUSE || usage == TD_BUMP || usage == TD_SPECULAR) {
        return true;
    }
    if (usage != TD_DEFAULT || numLevels <= 1) {
        return false;
    }
    return !R_ImageIsInterfaceArt(name);
}

/*
================
BitsForFormat
================
*/
int BitsForFormat(textureFormat_t format)
{
    switch (format) {
    case FMT_NONE:
        return 0;
    case FMT_RGBA8:
        return 32;
    case FMT_XRGB8:
        return 32;
    case FMT_RGB565:
        return 16;
    case FMT_L8A8:
        return 16;
    case FMT_ALPHA:
        return 8;
    case FMT_LUM8:
        return 8;
    case FMT_INT8:
        return 8;
    case FMT_DXT1:
        return 4;
    case FMT_DXT5:
        return 8;
    case FMT_DEPTH:
        return 32;
    case FMT_X16:
        return 16;
    case FMT_Y16_X16:
        return 32;
    default:
        assert(0);
        return 0;
    }
}

/*
========================
idImage::DeriveOpts
========================
*/
#if defined(__vita__)
int vitaImagesFromBimage = 0;
int vitaImagesRecompressed = 0;
// How many images ended up as YCoCg-DXT5 diffuse.
int vitaImagesYCoCg = 0;
#endif

ID_INLINE void idImage::DeriveOpts()
{

    if (opts.format == FMT_NONE) {
        opts.colorFormat = CFM_DEFAULT;

        switch (usage) {
        case TD_COVERAGE:
            opts.format = FMT_DXT1;
            opts.colorFormat = CFM_GREEN_ALPHA;
            break;
        case TD_DEPTH:
            opts.format = FMT_DEPTH;
            break;
        case TD_DIFFUSE:
            // TD_DIFFUSE gets only set to when its a diffuse texture for an interaction
            opts.gammaMips = true;
            opts.format = FMT_DXT5;
            opts.colorFormat = CFM_YCOCG_DXT5;
            break;
        case TD_SPECULAR:
            opts.gammaMips = true;
            opts.format = FMT_DXT1;
            opts.colorFormat = CFM_DEFAULT;
            break;
        case TD_DEFAULT:
            opts.gammaMips = true;
            opts.format = FMT_DXT5;
            opts.colorFormat = CFM_DEFAULT;
            break;
        case TD_BUMP:
            opts.format = FMT_DXT5;
            opts.colorFormat = CFM_NORMAL_DXT5;
            break;
        case TD_FONT:
            opts.format = FMT_DXT1;
            opts.colorFormat = CFM_GREEN_ALPHA;
            opts.numLevels = 4; // We only support 4 levels because we align to 16 in the exporter
            opts.gammaMips = true;
            break;
        case TD_LIGHT:
            opts.format = FMT_RGB565;
            opts.gammaMips = true;
            break;
        case TD_LOOKUP_TABLE_MONO:
            opts.format = FMT_INT8;
            break;
        case TD_LOOKUP_TABLE_ALPHA:
            opts.format = FMT_ALPHA;
            break;
        case TD_LOOKUP_TABLE_RGB1:
        case TD_LOOKUP_TABLE_RGBA:
            opts.format = FMT_RGBA8;
            break;
        default:
            assert(false);
            opts.format = FMT_RGBA8;
        }
    }

    if (opts.numLevels == 0) {
        opts.numLevels = 1;

        if (filter == TF_LINEAR || filter == TF_NEAREST) {
            // don't create mip maps if we aren't going to be using them
        } else {
            int temp_width = opts.width;
            int temp_height = opts.height;
            while (temp_width > 1 || temp_height > 1) {
                temp_width >>= 1;
                temp_height >>= 1;
                if ((opts.format == FMT_DXT1 || opts.format == FMT_DXT5) && ((temp_width & 0x3) != 0 || (temp_height & 0x3) != 0)) {
                    break;
                }
                opts.numLevels++;
            }
        }
    }
}

/*
========================
idImage::AllocImage
========================
*/
void idImage::AllocImage(const idImageOpts& imgOpts, textureFilter_t tf, textureRepeat_t tr)
{
    filter = tf;
    repeat = tr;
    opts = imgOpts;
    DeriveOpts();
    AllocImage();
}

/*
================
GenerateImage
================
*/
void idImage::GenerateImage(const byte* pic, int width, int height, textureFilter_t filterParm, textureRepeat_t repeatParm, textureUsage_t usageParm)
{
    PurgeImage();

    filter = filterParm;
    repeat = repeatParm;
    usage = usageParm;
    cubeFiles = CF_2D;

    opts.textureType = TT_2D;
    opts.width = width;
    opts.height = height;
    opts.numLevels = 0;
    DeriveOpts();

    // if we don't have a rendering context, just return after we
    // have filled in the parms.  We must have the values set, or
    // an image match from a shader before the render starts would miss
    // the generated texture
    if (!R_IsInitialized()) {
        return;
    }

    idBinaryImage im(GetName());
    im.Load2DFromMemory(width, height, pic, opts.numLevels, opts.format, opts.colorFormat, opts.gammaMips);

    AllocImage();

    for (int i = 0; i < im.NumImages(); i++) {
        const bimageImage_t& img = im.GetImageHeader(i);
        const byte* data = im.GetImageData(i);
        SubImageUpload(img.level, 0, 0, img.destZ, img.width, img.height, data);
    }
}

/*
====================
GenerateCubeImage

Non-square cube sides are not allowed
====================
*/
void idImage::GenerateCubeImage(const byte* pic[6], int size, textureFilter_t filterParm, textureUsage_t usageParm)
{
    PurgeImage();

    filter = filterParm;
    repeat = TR_CLAMP;
    usage = usageParm;
    cubeFiles = CF_NATIVE;

    opts.textureType = TT_CUBIC;
    opts.width = size;
    opts.height = size;
    opts.numLevels = 0;
    DeriveOpts();

    // if we don't have a rendering context, just return after we
    // have filled in the parms.  We must have the values set, or
    // an image match from a shader before the render starts would miss
    // the generated texture
    if (!R_IsInitialized()) {
        return;
    }

    idBinaryImage im(GetName());
    im.LoadCubeFromMemory(size, pic, opts.numLevels, opts.format, opts.gammaMips);

    AllocImage();

    for (int i = 0; i < im.NumImages(); i++) {
        const bimageImage_t& img = im.GetImageHeader(i);
        const byte* data = im.GetImageData(i);
        SubImageUpload(img.level, 0, 0, img.destZ, img.width, img.height, data);
    }
}

/*
===============
GetGeneratedName

name contains GetName() upon entry
===============
*/
void idImage::GetGeneratedName(idStr& _name, const textureUsage_t& _usage, const cubeFiles_t& _cube)
{
    idStrStatic<64> extension;

    _name.ExtractFileExtension(extension);
    _name.StripFileExtension();

    _name += va("#__%02d%02d", (int)_usage, (int)_cube);
    if (extension.Length() > 0) {
        _name.SetFileExtension(extension);
    }
}

/*
===============
ActuallyLoadImage

Absolutely every image goes through this path
On exit, the idImage will have a valid OpenGL texture number that can be bound
===============
*/
void idImage::ActuallyLoadImage(bool fromBackEnd)
{

    // if we don't have a rendering context yet, just return
    if (!R_IsInitialized()) {
        return;
    }

    // this is the ONLY place generatorFunction will ever be called
    if (generatorFunction) {
        generatorFunction(this);
        return;
    }

    extern int vita_benchLoads[3]; // VitaBench.cpp: images loaded, per frame in the benchmark log
    vita_benchLoads[0]++;

    if (com_productionMode.GetInteger() != 0) {
        sourceFileTime = FILE_NOT_FOUND_TIMESTAMP;
        if (cubeFiles != CF_2D) {
            opts.textureType = TT_CUBIC;
            repeat = TR_CLAMP;
        }
    } else {
        if (cubeFiles != CF_2D) {
            opts.textureType = TT_CUBIC;
            repeat = TR_CLAMP;
            R_LoadCubeImages(GetName(), cubeFiles, NULL, NULL, &sourceFileTime);
        } else {
            opts.textureType = TT_2D;
            R_LoadImageProgram(GetName(), NULL, NULL, NULL, &sourceFileTime, &usage);
        }
    }

    // Figure out opts.colorFormat and opts.format so we can make sure the binary image is up to date
    DeriveOpts();

    idStrStatic<MAX_OSPATH> generatedName = GetName();
    GetGeneratedName(generatedName, usage, cubeFiles);

    idBinaryImage im(generatedName);
#if defined(__vita__)
    // top levels that will not be uploaded are skipped over, not read
    im.vitaImageName = GetName();
    im.vitaUsage = usage;
    im.vitaSkipRequest = vita_imageDownSize.GetInteger() + vita_textureQualityDrop;
#endif
    binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);

    // BFHACK, do not want to tweak on buildgame so catch these images here
    if (binaryFileTime == FILE_NOT_FOUND_TIMESTAMP && fileSystem->UsingResourceFiles()) {
        int c = 1;
        while (c-- > 0) {
            if (generatedName.Find("guis/assets/white#__0000", false) >= 0) {
                generatedName.Replace("white#__0000", "white#__0200");
                im.SetName(generatedName);
                binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);
                break;
            }
            if (generatedName.Find("guis/assets/white#__0100", false) >= 0) {
                generatedName.Replace("white#__0100", "white#__0200");
                im.SetName(generatedName);
                binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);
                break;
            }
            if (generatedName.Find("textures/black#__0100", false) >= 0) {
                generatedName.Replace("black#__0100", "black#__0200");
                im.SetName(generatedName);
                binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);
                break;
            }
            if (generatedName.Find("textures/decals/bulletglass1_d#__0100", false) >= 0) {
                generatedName.Replace("bulletglass1_d#__0100", "bulletglass1_d#__0200");
                im.SetName(generatedName);
                binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);
                break;
            }
            if (generatedName.Find("models/monsters/skeleton/skeleton01_d#__1000", false) >= 0) {
                generatedName.Replace("skeleton01_d#__1000", "skeleton01_d#__0100");
                im.SetName(generatedName);
                binaryFileTime = im.LoadFromGeneratedFile(sourceFileTime);
                break;
            }
        }
    }
    const bimageFile_t& header = im.GetFileHeader();

#if defined(__vita__)
    // Which path an image takes matters a lot here. If the prebuilt .bimage is
    // missing, or its format does not match what DeriveOpts() derived, the
    // engine discards it and re-compresses the texture on the console with
    // idDxtEncoder. For a TD_DIFFUSE that means CompressYCoCgDXT5Fast running
    // on ARM, and a fault in that would corrupt chroma on some textures and
    // not others - which is what the screen shows. Count both paths so it is
    // a measurement rather than an assumption.
    {
        static bool loggedMode = false;
        if (!loggedMode) {
            loggedMode = true;
            Vita_Checkpoint("images: productionMode %d inProduction %d usingResources %d",
                com_productionMode.GetInteger(), fileSystem->InProductionMode() ? 1 : 0,
                fileSystem->UsingResourceFiles() ? 1 : 0);
        }
    }
#endif

#if defined(__vita__)
    int vitaPreSkipped = 0; // levels the binary loader never read
#endif
    if ((fileSystem->InProductionMode() && binaryFileTime != FILE_NOT_FOUND_TIMESTAMP) || ((binaryFileTime != FILE_NOT_FOUND_TIMESTAMP) && (header.colorFormat == opts.colorFormat) && (header.format == opts.format) && (header.textureType == opts.textureType))) {
#if defined(__vita__)
        vitaImagesFromBimage++;
        vitaPreSkipped = im.vitaSkipped;
#endif
        opts.width = header.width;
        opts.height = header.height;
        opts.numLevels = header.numLevels;
        opts.colorFormat = (textureColor_t)header.colorFormat;
        opts.format = (textureFormat_t)header.format;
        opts.textureType = (textureType_t)header.textureType;
        if (cvarSystem->GetCVarBool("fs_buildresources")) {
            // for resource gathering write this image to the preload file for this map
            fileSystem->AddImagePreload(GetName(), filter, repeat, usage, cubeFiles);
        }
    } else {
#if defined(__vita__)
        vitaImagesRecompressed++;
        if (vitaImagesRecompressed <= 24) {
            Vita_Checkpoint("recompress #%d '%s' usage %d want fmt %d cfm %d | header fmt %d cfm %d bimageTime %d",
                vitaImagesRecompressed, GetName(), (int)usage, (int)opts.format, (int)opts.colorFormat,
                (int)header.format, (int)header.colorFormat,
                (binaryFileTime == FILE_NOT_FOUND_TIMESTAMP) ? 0 : 1);
        }
#endif
        if (cubeFiles != CF_2D) {
            int size;
            byte* pics[6];

            if (!R_LoadCubeImages(GetName(), cubeFiles, pics, &size, &sourceFileTime) || size == 0) {
                idLib::Warning("Couldn't load cube image: %s", GetName());
                return;
            }

            opts.textureType = TT_CUBIC;
            repeat = TR_CLAMP;
            opts.width = size;
            opts.height = size;
            opts.numLevels = 0;
            DeriveOpts();
            im.LoadCubeFromMemory(size, (const byte**)pics, opts.numLevels, opts.format, opts.gammaMips);
            repeat = TR_CLAMP;

            for (int i = 0; i < 6; i++) {
                if (pics[i]) {
                    Mem_Free(pics[i]);
                }
            }
        } else {
            int width, height;
            byte* pic;

            // load the full specification, and perform any image program calculations
            R_LoadImageProgram(GetName(), &pic, &width, &height, &sourceFileTime, &usage);

            if (pic == NULL) {
                idLib::Warning("Couldn't load image: %s : %s", GetName(), generatedName.c_str());
                // create a default so it doesn't get continuously reloaded
                opts.width = 8;
                opts.height = 8;
                opts.numLevels = 1;
                DeriveOpts();
                AllocImage();

                // clear the data so it's not left uninitialized
                idTempArray<byte> clear(opts.width * opts.height * 4);
                memset(clear.Ptr(), 0, clear.Size());
                for (int level = 0; level < opts.numLevels; level++) {
                    SubImageUpload(level, 0, 0, 0, opts.width >> level, opts.height >> level, clear.Ptr());
                }

                return;
            }

            opts.width = width;
            opts.height = height;
            opts.numLevels = 0;
            DeriveOpts();
            im.Load2DFromMemory(opts.width, opts.height, pic, opts.numLevels, opts.format, opts.colorFormat, opts.gammaMips);

            Mem_Free(pic);
        }
        binaryFileTime = im.WriteGeneratedFile(sourceFileTime);
    }

#if defined(__vita__)
    // Decide how many top mip levels to drop, then shrink opts to match so
    // AllocImage() reserves the smaller texture.
    const int vitaSkip = Max(VitaSkipLevels(), vitaPreSkipped);
    vitaAppliedSkip = vitaSkip;
    vitaSkipCap = R_VitaSkipLevels(GetName(), usage, opts.textureType, opts.width, opts.height, opts.numLevels, 8);

    if (vitaSkip > 0) {
        opts.width >>= vitaSkip;
        opts.height >>= vitaSkip;
        opts.numLevels -= vitaSkip;
    }
#endif

#if defined(__vita__)
    if (opts.colorFormat == CFM_YCOCG_DXT5) {
        vitaImagesYCoCg++;
    }
    if (usage == TD_DIFFUSE) {
        static int diffuseLogBudget = 16;
        if (diffuseLogBudget > 0) {
            diffuseLogBudget--;
            Vita_Checkpoint("diffuse '%s' fmt %d cfm %d %dx%d levels %d skip %d",
                GetName(), (int)opts.format, (int)opts.colorFormat,
                opts.width, opts.height, opts.numLevels, vitaSkip);
        }
    }
#endif

    AllocImage();

    // Toksvig (Toksvig.cpp): a normal map's lower mip levels get the length
    // of their unnormalised mean normal in blue, from the top level uploaded
    if (opts.colorFormat == CFM_NORMAL_DXT5 && opts.format == FMT_DXT5 && opts.textureType == TT_2D) {
        extern int R_ToksvigDXT5Normals(idBinaryImage & im, int baseLevel, const char* name, bool duringPlay);
#if defined(__vita__)
        R_ToksvigDXT5Normals(im, vitaSkip, GetName(), referencedDuringPlay);
#else
        R_ToksvigDXT5Normals(im, 0, GetName(), referencedDuringPlay);
#endif
    }

    for (int i = 0; i < im.NumImages(); i++) {
        const bimageImage_t& img = im.GetImageHeader(i);
        const byte* data = im.GetImageData(i);
#if defined(__vita__)
        if (img.level < vitaSkip) {
            continue; // this level is no longer part of the texture
        }

        SubImageUpload(img.level - vitaSkip, 0, 0, img.destZ, img.width, img.height, data);

        // YCoCg-DXT5 diffuse: does the block reach GPU memory intact, and what
        // colour does it stand for? Luma lives in the DXT5 alpha block, Co in
        // red, Cg in green and the chroma scale in blue, so a block that is
        // damaged in transit, or alpha endpoints that are flat across a whole
        // texture, show up here before anything is drawn. Block (0,0) is
        // Morton index 0, so it sits at offset 0 in the swizzled GPU copy and
        // the two can be compared byte for byte. The decoded colour is what
        // the interaction shader must produce for that block's first texel.
        if ((img.level - vitaSkip) == 0 && opts.format == FMT_DXT5
            && opts.colorFormat == CFM_YCOCG_DXT5 && img.dataSize >= 16) {
            static int verifyBudget = 4;
            if (verifyBudget > 0) {
                verifyBudget--;

                int aMin = 255;
                int aMax = 0;
                for (int b = 0; b + 16 <= img.dataSize; b += 16) {
                    for (int e = 0; e < 2; e++) {
                        const int v = data[b + e];
                        if (v < aMin) {
                            aMin = v;
                        }
                        if (v > aMax) {
                            aMax = v;
                        }
                    }
                }

                Vita_Checkpoint("texverify '%s' %dx%d src %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | alpha (luma) endpoints span %d..%d over %d blocks",
                    GetName(), img.width, img.height,
                    data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
                    data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
                    aMin, aMax, img.dataSize / 16);

                // First texel of block 0, decoded on the CPU exactly as
                // ConvertYCoCgToRGB does it.
                {
                    const int a0 = data[0], a1 = data[1];
                    const int ai = data[2] & 7;
                    int luma;
                    if (ai == 0) {
                        luma = a0;
                    } else if (ai == 1) {
                        luma = a1;
                    } else if (a0 > a1) {
                        luma = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
                    } else if (ai < 6) {
                        luma = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
                    } else {
                        luma = (ai == 6) ? 0 : 255;
                    }
                    const int c0 = data[8] | (data[9] << 8);
                    const int c1 = data[10] | (data[11] << 8);
                    const int ci = data[12] & 3;
                    int rgb0[3], rgb1[3], col[3];
                    rgb0[0] = (((c0 >> 11) & 31) << 3) | (((c0 >> 11) & 31) >> 2);
                    rgb0[1] = (((c0 >> 5) & 63) << 2) | (((c0 >> 5) & 63) >> 4);
                    rgb0[2] = ((c0 & 31) << 3) | ((c0 & 31) >> 2);
                    rgb1[0] = (((c1 >> 11) & 31) << 3) | (((c1 >> 11) & 31) >> 2);
                    rgb1[1] = (((c1 >> 5) & 63) << 2) | (((c1 >> 5) & 63) >> 4);
                    rgb1[2] = ((c1 & 31) << 3) | ((c1 & 31) >> 2);
                    for (int k = 0; k < 3; k++) {
                        col[k] = ci == 0 ? rgb0[k] : ci == 1 ? rgb1[k] : ci == 2 ? (2 * rgb0[k] + rgb1[k]) / 3 : (rgb0[k] + 2 * rgb1[k]) / 3;
                    }
                    const float co = col[0] / 255.0f, cg = col[1] / 255.0f, sc = col[2] / 255.0f, y = luma / 255.0f;
                    const float z = 1.0f / (sc * 31.875f + 1.0f);
                    const float r = co * z - cg * z + y;
                    const float g = cg * z - 0.50196078f * z + y;
                    const float b = -co * z - cg * z + 1.00392156f * z + y;
                    Vita_Checkpoint("texverify '%s' texel 0: Co %d Cg %d scale %d Y %d -> RGB %.0f %.0f %.0f (what the interaction shader must output for it)",
                        GetName(), col[0], col[1], col[2], luma,
                        idMath::ClampFloat(0.0f, 1.0f, r) * 255.0f, idMath::ClampFloat(0.0f, 1.0f, g) * 255.0f, idMath::ClampFloat(0.0f, 1.0f, b) * 255.0f);
                }

                const byte* gpu = (const byte*)vglGetTexDataPointer(0x0DE1 /* GL_TEXTURE_2D */);
                if (gpu != NULL) {
                    Vita_Checkpoint("texverify '%s' gpu %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X | block 0 %s",
                        GetName(),
                        gpu[0], gpu[1], gpu[2], gpu[3], gpu[4], gpu[5], gpu[6], gpu[7],
                        gpu[8], gpu[9], gpu[10], gpu[11], gpu[12], gpu[13], gpu[14], gpu[15],
                        (memcmp(gpu, data, 16) == 0) ? "matches" : "DIFFERS");
                } else {
                    Vita_Checkpoint("texverify '%s' gpu pointer is NULL", GetName());
                }
            }
        }

#else
        SubImageUpload(img.level, 0, 0, img.destZ, img.width, img.height, data);
#endif
    }
}

#if defined(__vita__)
/*
==============
idImage::VitaSkipLevels

Only world-surface textures shrink (R_VitaImageCanShrink). Never drop the only
level, and never shrink below 32 - past that the saving is negligible and DXT
blocks start to dominate.
==============
*/
int idImage::VitaSkipLevels() const
{
    return R_VitaSkipLevels(GetName(), usage, opts.textureType, opts.width, opts.height, opts.numLevels,
        vita_imageDownSize.GetInteger() + vita_textureQualityDrop);
}

bool idImage::VitaWantsSmaller() const
{
    if (!IsLoaded() || generatorFunction != NULL || vitaSkipCap <= vitaAppliedSkip) {
        return false;
    }
    // what it would get now, by the same rules (a text texture's request is
    // not what it gets: comparing with the request reloaded it forever)
    const int want = Min(vitaSkipCap, VitaSkipLevels());
    return want > vitaAppliedSkip;
}

#endif

/*
==============
idImage::LoadHeaderOnly

vitaGL may only be called from the thread that brought it up, but BFG loads
an image the moment it is first referenced outside a level load - which during
play is the game/draw thread (a new effect, a GUI, a PDA video). Its GL calls
then ran concurrently with the render back end on the main thread. Here the
image gets its size from the .bimage header and nothing else; the first Bind()
on the GL thread does the real load.
==============
*/
void idImage::LoadHeaderOnly()
{
    if (generatorFunction) {
        return; // these are made on the GL thread at start-up
    }
    if (cubeFiles != CF_2D) {
        opts.textureType = TT_CUBIC;
        repeat = TR_CLAMP;
    } else {
        opts.textureType = TT_2D;
        if (com_productionMode.GetInteger() == 0) {
            R_LoadImageProgram(GetName(), NULL, NULL, NULL, &sourceFileTime, &usage);
        }
    }
    DeriveOpts();

    idStrStatic<MAX_OSPATH> generatedName = GetName();
    GetGeneratedName(generatedName, usage, cubeFiles);
    idFileLocal f(fileSystem->OpenFileRead(generatedName));
    if (f == NULL) {
        return;
    }
    bimageFile_t header;
    if (f->Read(&header, sizeof(header)) != sizeof(header)) {
        return;
    }
    idSwapClass<bimageFile_t> swap;
    swap.Big(header.headerMagic);
    swap.Big(header.textureType);
    swap.Big(header.format);
    swap.Big(header.colorFormat);
    swap.Big(header.width);
    swap.Big(header.height);
    swap.Big(header.numLevels);
    if (header.headerMagic != BIMAGE_MAGIC) {
        return;
    }
    opts.width = header.width;
    opts.height = header.height;
    opts.numLevels = header.numLevels;
    opts.colorFormat = (textureColor_t)header.colorFormat;
    opts.format = (textureFormat_t)header.format;
    opts.textureType = (textureType_t)header.textureType;
#if defined(__vita__)
    const int skip = VitaSkipLevels();
    if (skip > 0) {
        opts.width >>= skip;
        opts.height >>= skip;
        opts.numLevels -= skip;
    }
#endif
    static int logged = 0;
    if (logged < 8) {
        logged++;
        common->Printf("image '%s' first used off the GL thread: %dx%d from its header, loaded at its first draw\n", GetName(), opts.width, opts.height);
    }
}

/*
==============
Bind

Automatically enables 2D mapping or cube mapping if needed
==============
*/
void idImage::Bind()
{

    RENDERLOG_PRINTF("idImage::Bind( %s )\n", GetName());

    // load the image if necessary (FIXME: not SMP safe!)
    if (!IsLoaded()) {
        // load the image on demand here, which isn't our normal game operating mode
        ActuallyLoadImage(true);
    }

    const int texUnit = backEnd.glState.currenttmu;

    tmu_t* tmu = &backEnd.glState.tmu[texUnit];
    // bind the texture
    if (opts.textureType == TT_2D) {
        if (tmu->current2DMap != texnum) {
            tmu->current2DMap = texnum;
            qglBindMultiTextureEXT(GL_TEXTURE0_ARB + texUnit, GL_TEXTURE_2D, texnum);
        }
    } else if (opts.textureType == TT_CUBIC) {
        if (tmu->currentCubeMap != texnum) {
            tmu->currentCubeMap = texnum;
            qglBindMultiTextureEXT(GL_TEXTURE0_ARB + texUnit, GL_TEXTURE_CUBE_MAP_EXT, texnum);
        }
    }
}

/*
================
MakePowerOfTwo
================
*/
int MakePowerOfTwo(int num)
{
    int pot;
    for (pot = 1; pot < num; pot <<= 1) {
    }
    return pot;
}

/*
====================
CopyFramebuffer
====================
*/
#if defined(__vita__)
bool VitaBlitFramebufferToImage(const idImage* image, GLuint texnum, int x, int y, int w, int h);
#endif

void idImage::CopyFramebuffer(int x, int y, int imageWidth, int imageHeight)
{
#if defined(__vita__)
    // See VitaBlitFramebufferToImage in gl_Image.cpp: the stock path reads an
    // older, finished frame on a tiled GPU.
    if (opts.textureType == TT_2D && VitaBlitFramebufferToImage(this, texnum, x, y, imageWidth, imageHeight)) {
        opts.width = imageWidth;
        opts.height = imageHeight;
        backEnd.pc.c_copyFrameBuffer++;
        return;
    }
#endif

    qglBindTexture((opts.textureType == TT_CUBIC) ? GL_TEXTURE_CUBE_MAP_EXT : GL_TEXTURE_2D, texnum);

    qglReadBuffer(GL_BACK);

    opts.width = imageWidth;
    opts.height = imageHeight;
    qglCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, x, y, imageWidth, imageHeight, 0);

    // these shouldn't be necessary if the image was initialized properly
    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    backEnd.pc.c_copyFrameBuffer++;
}

/*
====================
CopyDepthbuffer
====================
*/
void idImage::CopyDepthbuffer(int x, int y, int imageWidth, int imageHeight)
{
    qglBindTexture((opts.textureType == TT_CUBIC) ? GL_TEXTURE_CUBE_MAP_EXT : GL_TEXTURE_2D, texnum);

    opts.width = imageWidth;
    opts.height = imageHeight;
    qglCopyTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, x, y, imageWidth, imageHeight, 0);

    backEnd.pc.c_copyFrameBuffer++;
}

/*
=============
RB_UploadScratchImage

if rows = cols * 6, assume it is a cube map animation
=============
*/
void idImage::UploadScratch(const byte* data, int cols, int rows)
{
#if defined(__vita__)
    Vita_CheckGLThread("UploadScratch", GetName());
#endif

    // if rows = cols * 6, assume it is a cube map animation
    if (rows == cols * 6) {
        rows /= 6;
        const byte* pic[6];
        for (int i = 0; i < 6; i++) {
            pic[i] = data + cols * rows * 4 * i;
        }

        if (opts.textureType != TT_CUBIC || usage != TD_LOOKUP_TABLE_RGBA) {
            GenerateCubeImage(pic, cols, TF_LINEAR, TD_LOOKUP_TABLE_RGBA);
            return;
        }
        if (opts.width != cols || opts.height != rows) {
            opts.width = cols;
            opts.height = rows;
            AllocImage();
        }
        SetSamplerState(TF_LINEAR, TR_CLAMP);
        for (int i = 0; i < 6; i++) {
            SubImageUpload(0, 0, 0, i, opts.width, opts.height, pic[i]);
        }

    } else {
        if (opts.textureType != TT_2D || usage != TD_LOOKUP_TABLE_RGBA) {
            GenerateImage(data, cols, rows, TF_LINEAR, TR_REPEAT, TD_LOOKUP_TABLE_RGBA);
            return;
        }
        if (opts.width != cols || opts.height != rows) {
            opts.width = cols;
            opts.height = rows;
            AllocImage();
        }
        SetSamplerState(TF_LINEAR, TR_REPEAT);
#if defined(__vita__)
        if (opts.format == FMT_RGBA8 && opts.numLevels == 1) {
            VitaReplaceRGBA(data);
            return;
        }
#endif
        SubImageUpload(0, 0, 0, 0, opts.width, opts.height, data);
    }
}

/*
==================
StorageSize
==================
*/
int idImage::StorageSize() const
{

    if (!IsLoaded()) {
        return 0;
    }
    int baseSize = opts.width * opts.height;
    if (opts.numLevels > 1) {
        baseSize *= 4;
        baseSize /= 3;
    }
    baseSize *= BitsForFormat(opts.format);
    baseSize /= 8;
    return baseSize;
}

/*
==================
Print
==================
*/
void idImage::Print() const
{
    if (generatorFunction) {
        common->Printf("F");
    } else {
        common->Printf(" ");
    }

    switch (opts.textureType) {
    case TT_2D:
        common->Printf(" ");
        break;
    case TT_CUBIC:
        common->Printf("C");
        break;
    default:
        common->Printf("<BAD TYPE:%i>", opts.textureType);
        break;
    }

    common->Printf("%4i %4i ", opts.width, opts.height);

    switch (opts.format) {
#define NAME_FORMAT(x)               \
    case FMT_##x:                    \
        common->Printf("%-6s ", #x); \
        break;
        NAME_FORMAT(NONE);
        NAME_FORMAT(RGBA8);
        NAME_FORMAT(XRGB8);
        NAME_FORMAT(RGB565);
        NAME_FORMAT(L8A8);
        NAME_FORMAT(ALPHA);
        NAME_FORMAT(LUM8);
        NAME_FORMAT(INT8);
        NAME_FORMAT(DXT1);
        NAME_FORMAT(DXT5);
        NAME_FORMAT(DEPTH);
        NAME_FORMAT(X16);
        NAME_FORMAT(Y16_X16);
    default:
        common->Printf("<%3i>", opts.format);
        break;
    }

    switch (filter) {
    case TF_DEFAULT:
        common->Printf("mip  ");
        break;
    case TF_LINEAR:
        common->Printf("linr ");
        break;
    case TF_NEAREST:
        common->Printf("nrst ");
        break;
    default:
        common->Printf("<BAD FILTER:%i>", filter);
        break;
    }

    switch (repeat) {
    case TR_REPEAT:
        common->Printf("rept ");
        break;
    case TR_CLAMP_TO_ZERO:
        common->Printf("zero ");
        break;
    case TR_CLAMP_TO_ZERO_ALPHA:
        common->Printf("azro ");
        break;
    case TR_CLAMP:
        common->Printf("clmp ");
        break;
    default:
        common->Printf("<BAD REPEAT:%i>", repeat);
        break;
    }

    common->Printf("%4ik ", StorageSize() / 1024);

    common->Printf(" %s\n", GetName());
}

/*
===============
idImage::Reload
===============
*/
void idImage::Reload(bool force)
{
    // always regenerate functional images
    if (generatorFunction) {
        common->DPrintf("regenerating %s.\n", GetName());
        generatorFunction(this);
        return;
    }

    // check file times
    if (!force) {
        ID_TIME_T current;
        if (cubeFiles != CF_2D) {
            R_LoadCubeImages(imgName, cubeFiles, NULL, NULL, &current);
        } else {
            // get the current values
            R_LoadImageProgram(imgName, NULL, NULL, NULL, &current);
        }
        if (current <= sourceFileTime) {
            return;
        }
    }

    common->DPrintf("reloading %s.\n", GetName());

    PurgeImage();

    // Load is from the front end, so the back end must be synced
    ActuallyLoadImage(false);
}

/*
========================
idImage::SetSamplerState
========================
*/
void idImage::SetSamplerState(textureFilter_t tf, textureRepeat_t tr)
{
    if (tf == filter && tr == repeat) {
        return;
    }
    filter = tf;
    repeat = tr;
    qglBindTexture((opts.textureType == TT_CUBIC) ? GL_TEXTURE_CUBE_MAP_EXT : GL_TEXTURE_2D, texnum);
    SetTexParameters();
}
