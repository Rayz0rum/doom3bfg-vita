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

// do this with a pointer, in case we want to make the actual manager
// a private virtual subclass
idImageManager imageManager;
idImageManager* globalImages = &imageManager;

#if defined(__vita__)
// Preloading walks the map's entire manifest and makes every image resident at
// once - 2181 of them for mars_city2, 54 seconds of work, and the single
// largest reason a map load runs the pools dry. Images are loaded lazily by
// idImage::Bind() anyway, so turning this off costs nothing but a first-use
// hitch, and keeps only what is actually drawn in memory.
idCVar preLoad_Images("preLoad_Images", "0", CVAR_SYSTEM | CVAR_BOOL, "preload images during beginlevelload");
#else
idCVar preLoad_Images("preLoad_Images", "1", CVAR_SYSTEM | CVAR_BOOL, "preload images during beginlevelload");
#endif

/*
===============
R_ReloadImages_f

Regenerate all images that came directly from files that have changed, so
any saved changes will show up in place.

New r_texturesize/r_texturedepth variables will take effect on reload

reloadImages <all>
===============
*/
void R_ReloadImages_f(const idCmdArgs& args)
{
    bool all = false;

    if (args.Argc() == 2) {
        if (!idStr::Icmp(args.Argv(1), "all")) {
            all = true;
        } else {
            common->Printf("USAGE: reloadImages <all>\n");
            return;
        }
    }

    globalImages->ReloadImages(all);
}

typedef struct
{
    idImage* image;
    int size;
    int index;
} sortedImage_t;

/*
=======================
R_QsortImageSizes

=======================
*/
static int R_QsortImageSizes(const void* a, const void* b)
{
    const sortedImage_t *ea, *eb;

    ea = (sortedImage_t*)a;
    eb = (sortedImage_t*)b;

    if (ea->size > eb->size) {
        return -1;
    }
    if (ea->size < eb->size) {
        return 1;
    }
    return idStr::Icmp(ea->image->GetName(), eb->image->GetName());
}

/*
=======================
R_QsortImageName

=======================
*/
static int R_QsortImageName(const void* a, const void* b)
{
    const sortedImage_t *ea, *eb;

    ea = (sortedImage_t*)a;
    eb = (sortedImage_t*)b;

    return idStr::Icmp(ea->image->GetName(), eb->image->GetName());
}

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
===============
R_ListTextTextures_f

The loaded textures the Vita keeps at full size for their text
(R_VitaImageHasText), and how many world textures are shrunk around them.
===============
*/
static void R_ListTextTextures_f(const idCmdArgs& args)
{
    int text = 0, world = 0;
    for (int i = 0; i < globalImages->images.Num(); i++) {
        idImage* image = globalImages->images[i];
        if (image == NULL || !R_VitaImageCanShrink(image->GetName(), image->GetUsage(), image->GetOpts().textureType, Max(2, image->GetOpts().numLevels))) {
            continue;
        }
        world++;
        if (R_VitaImageHasText(image->GetName())) {
            text++;
            common->Printf("text texture: %s (%dx%d)\n", image->GetName(), image->GetUploadWidth(), image->GetUploadHeight());
        }
    }
    common->Printf("%d of %d world textures loaded are kept at full size for their text\n", text, world);
}
#endif

/*
===============
R_ListImages_f
===============
*/
void R_ListImages_f(const idCmdArgs& args)
{
    int i, partialSize;
    idImage* image;
    int totalSize;
    int count = 0;
    bool uncompressedOnly = false;
    bool unloaded = false;
    bool failed = false;
    bool sorted = false;
    bool duplicated = false;
    bool overSized = false;
    bool sortByName = false;

    if (args.Argc() == 1) {

    } else if (args.Argc() == 2) {
        if (idStr::Icmp(args.Argv(1), "uncompressed") == 0) {
            uncompressedOnly = true;
        } else if (idStr::Icmp(args.Argv(1), "sorted") == 0) {
            sorted = true;
        } else if (idStr::Icmp(args.Argv(1), "namesort") == 0) {
            sortByName = true;
        } else if (idStr::Icmp(args.Argv(1), "unloaded") == 0) {
            unloaded = true;
        } else if (idStr::Icmp(args.Argv(1), "duplicated") == 0) {
            duplicated = true;
        } else if (idStr::Icmp(args.Argv(1), "oversized") == 0) {
            sorted = true;
            overSized = true;
        } else {
            failed = true;
        }
    } else {
        failed = true;
    }

    if (failed) {
        common->Printf("usage: listImages [ sorted | namesort | unloaded | duplicated | showOverSized ]\n");
        return;
    }

    const char* header = "       -w-- -h-- filt -fmt-- wrap  size --name-------\n";
    common->Printf("\n%s", header);

    totalSize = 0;

    sortedImage_t* sortedArray = (sortedImage_t*)alloca(sizeof(sortedImage_t) * globalImages->images.Num());

    for (i = 0; i < globalImages->images.Num(); i++) {
        image = globalImages->images[i];

        if (uncompressedOnly) {
            if (image->IsCompressed()) {
                continue;
            }
        }
        if (unloaded == image->IsLoaded()) {
            continue;
        }

        // only print duplicates (from mismatched wrap / clamp, etc)
        if (duplicated) {
            int j;
            for (j = i + 1; j < globalImages->images.Num(); j++) {
                if (idStr::Icmp(image->GetName(), globalImages->images[j]->GetName()) == 0) {
                    break;
                }
            }
            if (j == globalImages->images.Num()) {
                continue;
            }
        }

        if (sorted || sortByName) {
            sortedArray[count].image = image;
            sortedArray[count].size = image->StorageSize();
            sortedArray[count].index = i;
        } else {
            common->Printf("%4i:", i);
            image->Print();
        }
        totalSize += image->StorageSize();
        count++;
    }

    if (sorted || sortByName) {
        if (sortByName) {
            qsort(sortedArray, count, sizeof(sortedImage_t), R_QsortImageName);
        } else {
            qsort(sortedArray, count, sizeof(sortedImage_t), R_QsortImageSizes);
        }
        partialSize = 0;
        for (i = 0; i < count; i++) {
            common->Printf("%4i:", sortedArray[i].index);
            sortedArray[i].image->Print();
            partialSize += sortedArray[i].image->StorageSize();
            if (((i + 1) % 10) == 0) {
                common->Printf("-------- %5.1f of %5.1f megs --------\n",
                    partialSize / (1024 * 1024.0), totalSize / (1024 * 1024.0));
            }
        }
    }

    common->Printf("%s", header);
    common->Printf(" %i images (%i total)\n", count, globalImages->images.Num());
    common->Printf(" %5.1f total megabytes of images\n\n\n", totalSize / (1024 * 1024.0));
}

/*
==============
AllocImage

Allocates an idImage, adds it to the list,
copies the name, and adds it to the hash chain.
==============
*/
idImage* idImageManager::AllocImage(const char* name)
{
    if (strlen(name) >= MAX_IMAGE_NAME) {
        common->Error("idImageManager::AllocImage: \"%s\" is too long\n", name);
    }

    int hash = idStr(name).FileNameHash();

    idImage* image = new (TAG_IMAGE) idImage(name);

    imageHash.Add(hash, images.Append(image));

    return image;
}

/*
==============
AllocStandaloneImage

Allocates an idImage,does not add it to the list or hash chain

==============
*/
idImage* idImageManager::AllocStandaloneImage(const char* name)
{
    if (strlen(name) >= MAX_IMAGE_NAME) {
        common->Error("idImageManager::AllocImage: \"%s\" is too long\n", name);
    }

    idImage* image = new (TAG_IMAGE) idImage(name);

    return image;
}

/*
==================
ImageFromFunction

Images that are procedurally generated are allways specified
with a callback which must work at any time, allowing the OpenGL
system to be completely regenerated if needed.
==================
*/
idImage* idImageManager::ImageFromFunction(const char* _name, void (*generatorFunction)(idImage* image))
{

    // strip any .tga file extensions from anywhere in the _name
    idStr name = _name;
    name.Replace(".tga", "");
    name.BackSlashesToSlashes();

    // see if the image already exists
    int hash = name.FileNameHash();
    for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i)) {
        idImage* image = images[i];
        if (name.Icmp(image->GetName()) == 0) {
            if (image->generatorFunction != generatorFunction) {
                common->DPrintf("WARNING: reused image %s with mixed generators\n", name.c_str());
            }
            return image;
        }
    }

    // create the image and issue the callback
    idImage* image = AllocImage(name);

    image->generatorFunction = generatorFunction;

    // check for precompressed, load is from the front end
    image->referencedOutsideLevelLoad = true;
    image->ActuallyLoadImage(false);

    return image;
}

/*
===============
GetImageWithParameters
==============
*/
idImage* idImageManager::GetImageWithParameters(const char* _name, textureFilter_t filter, textureRepeat_t repeat, textureUsage_t usage, cubeFiles_t cubeMap) const
{
    if (!_name || !_name[0] || idStr::Icmp(_name, "default") == 0 || idStr::Icmp(_name, "_default") == 0) {
        declManager->MediaPrint("DEFAULTED\n");
        return globalImages->defaultImage;
    }
    if (idStr::Icmpn(_name, "fonts", 5) == 0 || idStr::Icmpn(_name, "newfonts", 8) == 0) {
        usage = TD_FONT;
    }
    if (idStr::Icmpn(_name, "lights", 6) == 0) {
        usage = TD_LIGHT;
    }
    // strip any .tga file extensions from anywhere in the _name, including image program parameters
    idStrStatic<MAX_OSPATH> name = _name;
    name.Replace(".tga", "");
    name.BackSlashesToSlashes();
    int hash = name.FileNameHash();
    for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i)) {
        idImage* image = images[i];
        if (name.Icmp(image->GetName()) == 0) {
            // the built in's, like _white and _flat always match the other options
            if (name[0] == '_') {
                return image;
            }
            if (image->cubeFiles != cubeMap) {
                common->Error("Image '%s' has been referenced with conflicting cube map states", _name);
            }
            if (image->filter != filter || image->repeat != repeat) {
                // we might want to have the system reset these parameters on every bind and
                // share the image data
                continue;
            }
            if (image->usage != usage) {
                // If an image is used differently then we need 2 copies of it because usage affects the way it's compressed and swizzled
                continue;
            }
            return image;
        }
    }
    return NULL;
}
/*
===============
ImageFromFile

Finds or loads the given image, always returning a valid image pointer.
Loading of the image may be deferred for dynamic loading.
==============
*/
idImage* idImageManager::ImageFromFile(const char* _name, textureFilter_t filter,
    textureRepeat_t repeat, textureUsage_t usage, cubeFiles_t cubeMap)
{

    if (!_name || !_name[0] || idStr::Icmp(_name, "default") == 0 || idStr::Icmp(_name, "_default") == 0) {
        declManager->MediaPrint("DEFAULTED\n");
        return globalImages->defaultImage;
    }
    if (idStr::Icmpn(_name, "fonts", 5) == 0 || idStr::Icmpn(_name, "newfonts", 8) == 0) {
        usage = TD_FONT;
    }
    if (idStr::Icmpn(_name, "lights", 6) == 0) {
        usage = TD_LIGHT;
    }

    // strip any .tga file extensions from anywhere in the _name, including image program parameters
    idStrStatic<MAX_OSPATH> name = _name;
    name.Replace(".tga", "");
    name.BackSlashesToSlashes();

    //
    // see if the image is already loaded, unless we
    // are in a reloadImages call
    //
    int hash = name.FileNameHash();
    for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i)) {
        idImage* image = images[i];
        if (name.Icmp(image->GetName()) == 0) {
            // the built in's, like _white and _flat always match the other options
            if (name[0] == '_') {
                return image;
            }
            if (image->cubeFiles != cubeMap) {
                common->Error("Image '%s' has been referenced with conflicting cube map states", _name);
            }

            if (image->filter != filter || image->repeat != repeat) {
                // we might want to have the system reset these parameters on every bind and
                // share the image data
                continue;
            }
            if (image->usage != usage) {
                // If an image is used differently then we need 2 copies of it because usage affects the way it's compressed and swizzled
                continue;
            }

            image->usage = usage;
            image->levelLoadReferenced = true;

            if ((!insideLevelLoad || preloadingMapImages) && !image->IsLoaded()) {
                image->referencedOutsideLevelLoad = (!insideLevelLoad && !preloadingMapImages);
                image->referencedDuringPlay = image->referencedOutsideLevelLoad && firstLevelLoadStarted;
                if (!R_IsGLThread()) {
                    image->LoadHeaderOnly(); // the GL part happens at the first Bind()
                } else {
                    image->ActuallyLoadImage(false); // load is from front end
                }
                declManager->MediaPrint("%ix%i %s (reload for mixed referneces)\n", image->GetUploadWidth(), image->GetUploadHeight(), image->GetName());
            }
            return image;
        }
    }

    //
    // create a new image
    //
    idImage* image = AllocImage(name);
    image->cubeFiles = cubeMap;
    image->usage = usage;
    image->filter = filter;
    image->repeat = repeat;

    image->levelLoadReferenced = true;

    // load it if we aren't in a level preload
    if (!insideLevelLoad || preloadingMapImages) {
        image->referencedOutsideLevelLoad = (!insideLevelLoad && !preloadingMapImages);
        image->referencedDuringPlay = image->referencedOutsideLevelLoad && firstLevelLoadStarted;
        if (!R_IsGLThread()) {
            image->LoadHeaderOnly(); // the GL part happens at the first Bind()
        } else {
            image->ActuallyLoadImage(false); // load is from front end
        }
        declManager->MediaPrint("%ix%i %s\n", image->GetUploadWidth(), image->GetUploadHeight(), image->GetName());
    } else {
        declManager->MediaPrint("%s\n", image->GetName());
    }

    return image;
}

/*
========================
idImageManager::ScratchImage
========================
*/
idImage* idImageManager::ScratchImage(const char* _name, idImageOpts* imgOpts, textureFilter_t filter, textureRepeat_t repeat, textureUsage_t usage)
{
    if (!_name || !_name[0]) {
        idLib::FatalError("idImageManager::ScratchImage called with empty name");
    }

    if (imgOpts == NULL) {
        idLib::FatalError("idImageManager::ScratchImage called with NULL imgOpts");
    }

    idStr name = _name;

    //
    // see if the image is already loaded, unless we
    // are in a reloadImages call
    //
    int hash = name.FileNameHash();
    for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i)) {
        idImage* image = images[i];
        if (name.Icmp(image->GetName()) == 0) {
            // the built in's, like _white and _flat always match the other options
            if (name[0] == '_') {
                return image;
            }

            if (image->filter != filter || image->repeat != repeat) {
                // we might want to have the system reset these parameters on every bind and
                // share the image data
                continue;
            }
            if (image->usage != usage) {
                // If an image is used differently then we need 2 copies of it because usage affects the way it's compressed and swizzled
                continue;
            }

            image->usage = usage;
            image->levelLoadReferenced = true;
            image->referencedOutsideLevelLoad = true;
            return image;
        }
    }

    // clamp is the only repeat mode that makes sense for cube maps, but
    // some platforms let them stay in repeat mode and get border seam issues
    if (imgOpts->textureType == TT_CUBIC && repeat != TR_CLAMP) {
        repeat = TR_CLAMP;
    }

    //
    // create a new image
    //
    idImage* newImage = AllocImage(name);
    if (newImage != NULL) {
        newImage->AllocImage(*imgOpts, filter, repeat);
    }
    return newImage;
}

/*
===============
idImageManager::GetImage
===============
*/
idImage* idImageManager::GetImage(const char* _name) const
{

    if (!_name || !_name[0] || idStr::Icmp(_name, "default") == 0 || idStr::Icmp(_name, "_default") == 0) {
        declManager->MediaPrint("DEFAULTED\n");
        return globalImages->defaultImage;
    }

    // strip any .tga file extensions from anywhere in the _name, including image program parameters
    idStr name = _name;
    name.Replace(".tga", "");
    name.BackSlashesToSlashes();

    //
    // look in loaded images
    //
    int hash = name.FileNameHash();
    for (int i = imageHash.First(hash); i != -1; i = imageHash.Next(i)) {
        idImage* image = images[i];
        if (name.Icmp(image->GetName()) == 0) {
            return image;
        }
    }

    return NULL;
}

/*
===============
PurgeAllImages
===============
*/
void idImageManager::PurgeAllImages()
{
    int i;
    idImage* image;

    for (i = 0; i < images.Num(); i++) {
        image = images[i];
        image->PurgeImage();
    }
}

/*
===============
ReloadImages
===============
*/
void idImageManager::ReloadImages(bool all)
{
    for (int i = 0; i < globalImages->images.Num(); i++) {
        globalImages->images[i]->Reload(all);
    }
}

/*
===============
R_CombineCubeImages_f

Used to combine animations of six separate tga files into
a serials of 6x taller tga files, for preparation to roq compress
===============
*/
void R_CombineCubeImages_f(const idCmdArgs& args)
{
    if (args.Argc() != 2) {
        common->Printf("usage: combineCubeImages <baseName>\n");
        common->Printf(" combines basename[1-6][0001-9999].tga to basenameCM[0001-9999].tga\n");
        common->Printf(" 1: forward 2:right 3:back 4:left 5:up 6:down\n");
        return;
    }

    idStr baseName = args.Argv(1);
    common->SetRefreshOnPrint(true);

    for (int frameNum = 1; frameNum < 10000; frameNum++) {
        char filename[MAX_IMAGE_NAME];
        byte* pics[6];
        int width = 0, height = 0;
        int side;
        int orderRemap[6] = { 1, 3, 4, 2, 5, 6 };
        for (side = 0; side < 6; side++) {
            sprintf(filename, "%s%i%04i.tga", baseName.c_str(), orderRemap[side], frameNum);

            common->Printf("reading %s\n", filename);
            R_LoadImage(filename, &pics[side], &width, &height, NULL, true);

            if (!pics[side]) {
                common->Printf("not found.\n");
                break;
            }

            // convert from "camera" images to native cube map images
            switch (side) {
            case 0: // forward
                R_RotatePic(pics[side], width);
                break;
            case 1: // back
                R_RotatePic(pics[side], width);
                R_HorizontalFlip(pics[side], width, height);
                R_VerticalFlip(pics[side], width, height);
                break;
            case 2: // left
                R_VerticalFlip(pics[side], width, height);
                break;
            case 3: // right
                R_HorizontalFlip(pics[side], width, height);
                break;
            case 4: // up
                R_RotatePic(pics[side], width);
                break;
            case 5: // down
                R_RotatePic(pics[side], width);
                break;
            }
        }

        if (side != 6) {
            for (int i = 0; i < side; side++) {
                Mem_Free(pics[side]);
            }
            break;
        }

        idTempArray<byte> buf(width * height * 6 * 4);
        byte* combined = (byte*)buf.Ptr();
        for (side = 0; side < 6; side++) {
            memcpy(combined + width * height * 4 * side, pics[side], width * height * 4);
            Mem_Free(pics[side]);
        }
        sprintf(filename, "%sCM%04i.tga", baseName.c_str(), frameNum);

        common->Printf("writing %s\n", filename);
        R_WriteTGA(filename, combined, width, height * 6);
    }
    common->SetRefreshOnPrint(false);
}

/*
===============
UnbindAll
===============
*/
void idImageManager::UnbindAll()
{
    int oldTMU = backEnd.glState.currenttmu;
    for (int i = 0; i < MAX_PROG_TEXTURE_PARMS; ++i) {
        backEnd.glState.currenttmu = i;
        BindNull();
    }
    backEnd.glState.currenttmu = oldTMU;
}

/*
===============
BindNull
===============
*/
void idImageManager::BindNull()
{
    RENDERLOG_PRINTF("BindNull()\n");
}

/*
===============
Init
===============
*/
void idImageManager::Init()
{

    images.Resize(1024, 1024);
    imageHash.ResizeIndex(1024);

    extern void R_ToksvigInit();
    R_ToksvigInit(); // its work area, in cached RAM while the heap has room (Toksvig.cpp)

    CreateIntrinsicImages();

    cmdSystem->AddCommand("reloadImages", R_ReloadImages_f, CMD_FL_RENDERER, "reloads images");
    cmdSystem->AddCommand("listImages", R_ListImages_f, CMD_FL_RENDERER, "lists images");
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    cmdSystem->AddCommand("listTextTextures", R_ListTextTextures_f, CMD_FL_RENDERER, "lists the loaded textures kept at full size on the Vita for their text");
#endif
    cmdSystem->AddCommand("combineCubeImages", R_CombineCubeImages_f, CMD_FL_RENDERER, "combines six images for roq compression");

    // should forceLoadImages be here?
}

/*
===============
Shutdown
===============
*/
void idImageManager::Shutdown()
{
    images.DeleteContents(true);
    imageHash.Clear();
}

/*
====================
idImageManager::BeginLevelLoad
Frees all images used by the previous level
====================
*/
idCVar image_debugPurgeAll("image_debugPurgeAll", "0", CVAR_RENDERER | CVAR_BOOL, "debug: at each level load also purge every image first used outside a level load, boot-time ones included");

void idImageManager::BeginLevelLoad()
{
    insideLevelLoad = true;

    int transientPurged = 0;
    int64 transientBytes = 0;
    for (int i = 0; i < images.Num(); i++) {
        idImage* image = images[i];

        // generator function images are always kept around
        if (image->generatorFunction) {
            continue;
        }

        if (!image->referencedOutsideLevelLoad && image->IsLoaded()) {
            image->PurgeImage();
            // idLib::Printf( "purging %s\n", image->GetName() );
        } else if ((image->referencedDuringPlay || image_debugPurgeAll.GetBool()) && image->IsLoaded() && !R_ImageIsInterfaceArt(image->GetName())) {
            // First used during play (an effect, a GUI, a video) rather than
            // at boot. Stock BFG keeps these for the rest of the session, so
            // over a campaign they pile up; drop them with the level they
            // came from. Bind() reloads one on demand if it is used again.
            transientBytes += image->StorageSize();
            transientPurged++;
            image->PurgeImage();
        } else {
            // idLib::Printf( "not purging %s\n", image->GetName() );
        }

        image->levelLoadReferenced = false;
    }
    if (transientPurged > 0) {
        common->Printf("images: purged %d (%d KB) that were first used during the last level's play\n", transientPurged, (int)(transientBytes / 1024));
    }
}

/*
====================
idImageManager::ExcludePreloadImage
====================
*/
bool idImageManager::ExcludePreloadImage(const char* name)
{
    idStrStatic<MAX_OSPATH> imgName = name;
    imgName.ToLower();
    if (imgName.Find("newfonts/", false) >= 0) {
        return true;
    }
    if (imgName.Find("generated/swf/", false) >= 0) {
        return true;
    }
    if (imgName.Find("/loadscreens/", false) >= 0) {
        return true;
    }
    return false;
}

/*
====================
idImageManager::Preload
====================
*/
void idImageManager::Preload(const idPreloadManifest& manifest, const bool& mapPreload)
{
    if (preLoad_Images.GetBool() && manifest.NumResources() > 0) {
        // preload this levels images
        common->Printf("Preloading images...\n");
        preloadingMapImages = mapPreload;
        int start = Sys_Milliseconds();
        int numLoaded = 0;

        // fileSystem->StartPreload( preloadImageFiles );
        for (int i = 0; i < manifest.NumResources(); i++) {
            const preloadEntry_s& p = manifest.GetPreloadByIndex(i);
            if (p.resType == PRELOAD_IMAGE && !ExcludePreloadImage(p.resourceName)) {
                globalImages->ImageFromFile(p.resourceName, (textureFilter_t)p.imgData.filter, (textureRepeat_t)p.imgData.repeat, (textureUsage_t)p.imgData.usage, (cubeFiles_t)p.imgData.cubeMap);
                numLoaded++;
            }
        }
        // fileSystem->StopPreload();
        int end = Sys_Milliseconds();
        common->Printf("%05d images preloaded ( or were already loaded ) in %5.1f seconds\n", numLoaded, (end - start) * 0.001);
        common->Printf("----------------------------------------\n");
        preloadingMapImages = false;
    }
}

/*
===============
idImageManager::LoadLevelImages
===============
*/
#if defined(__vita__)
// What loading textures actually costs vitaGL's pools, against what the images
// say they hold. A big gap means texture storage is being duplicated or
// padded somewhere between the engine and the GPU.
static int64 vitaImageExpectedBytes = 0;
static int64 vitaImageMeasuredBytes = 0;
static int vitaImageInflatedLogged = 0;
static int64 Vita_PoolsFree()
{
    return (int64)vglMemFree(VGL_MEM_RAM) + (int64)vglMemFree(VGL_MEM_VRAM) + (int64)vglMemFree(VGL_MEM_SLOW);
}

/*
===============
Memory pressure while a level's textures load

Textures are the one big allocation that can shrink. If memory runs low part
way through, the rest of the level's world textures load a mip level
smaller (vita_textureQualityDrop), and common_frame.cpp shrinks the ones
already loaded once play starts. Below 32 MB free one level is dropped, below
20 MB two. (It was 48 / 32: mc_underground and mars_city2 end their loads at
47 MB free, so every world texture of both played at a quarter of its size -
blurry signs and walls - with 43 MB still free in play. Shrinking in play
reacts to what is really used, common_frame.cpp.)
===============
*/
void Vita_FreeMemoryKB(unsigned int* allFreeKB, unsigned int* cachedFreeKB); // vita_main.cpp

static void Vita_TextureMemoryGuard(int loaded)
{
    unsigned int freeKB = 0;
    Vita_FreeMemoryKB(&freeKB, NULL);
    const int want = (freeKB < 20 * 1024) ? 2 : ((freeKB < 32 * 1024) ? 1 : 0);
    if (want > vita_textureQualityDrop) {
        vita_textureQualityDrop = want;
        common->Printf("vita: only %u MB free after %d textures - world textures now load %d mip level(s) smaller\n",
            freeKB / 1024, loaded, vita_textureQualityDrop);
    }
}

/*
===============
idImageManager::VitaShrinkLoadedTextures
===============
*/
int idImageManager::VitaShrinkLoadedTextures(int budgetUs, int* shrunk, int64* savedBytes)
{
    // A slice of the list per call from a cursor that goes round, not the
    // whole list: run 7 spent 1.2 ms a frame on the main thread (every fight)
    // walking ~2000 images every third frame, with a clock syscall for each
    // one still waiting. It returns 0 only once a whole round of the list has
    // gone by with nothing left to shrink.
    static int cursor = 0;
    static int cleanRun = 0; // images seen in a row that want nothing
    const int64 start = Sys_Microseconds();
    const int num = images.Num();
    if (num == 0) {
        return 0;
    }
    const int slice = Min(num, 256);
    int waiting = 0;
    bool overBudget = false;
    for (int n = 0; n < slice; n++) {
        cursor = (cursor + 1) % num;
        idImage* image = images[cursor];
        if (!image->VitaWantsSmaller()) {
            cleanRun++;
            continue;
        }
        cleanRun = 0;
        if (overBudget || Sys_Microseconds() - start > budgetUs) {
            overBudget = true;
            waiting++;
            break; // the rest of the round next time
        }
        const int before = image->StorageSize();
        image->PurgeImage();
        image->ActuallyLoadImage(false);
        if (shrunk != NULL) {
            (*shrunk)++;
        }
        if (savedBytes != NULL) {
            *savedBytes += before - image->StorageSize();
        }
    }
    return (waiting > 0 || cleanRun < num) ? 1 : 0;
}
#endif

int idImageManager::LoadLevelImages(bool pacifier)
{
    int loadCount = 0;
#if defined(__vita__)
    // One pool measurement for the whole pass: the query walks every chunk
    // header of vitaGL's heaps, in uncached memory, so per image it cost
    // seconds over a level's two thousand textures.
    const int64 passFreeBefore = Vita_PoolsFree();
    int64 passExpected = 0;
#endif
    for (int i = 0; i < images.Num(); i++) {
        if (pacifier) {
            common->UpdateLevelLoadPacifier();
        }

        idImage* image = images[i];
        if (image->generatorFunction) {
            continue;
        }
        if (image->levelLoadReferenced && !image->IsLoaded()) {
            loadCount++;
#if defined(__vita__)
            if ((loadCount & 63) == 1) {
                Vita_TextureMemoryGuard(loadCount);
            }
            image->ActuallyLoadImage(false);
            passExpected += (int64)image->StorageSize() * (image->opts.textureType == TT_CUBIC ? 6 : 1);
            // vitaGL pads compressed textures to power-of-two sizes: name the
            // big ones that are not (SWF atlases, some GUI art)
            if (image->IsCompressed() && ((image->opts.width & (image->opts.width - 1)) != 0 || (image->opts.height & (image->opts.height - 1)) != 0)
                && image->StorageSize() > 256 * 1024 && vitaImageInflatedLogged < 12) {
                vitaImageInflatedLogged++;
                Vita_Checkpoint("image '%s' %dx%d fmt %d levels %d: %d KB, padded to a power of two by vitaGL",
                    image->GetName(), image->opts.width, image->opts.height, (int)image->opts.format, image->opts.numLevels,
                    image->StorageSize() / 1024);
            }
#else
            image->ActuallyLoadImage(false);
#endif
        }
    }
#if defined(__vita__)
    if (loadCount > 0) {
        vitaImageExpectedBytes += passExpected;
        vitaImageMeasuredBytes += passFreeBefore - Vita_PoolsFree();
        Vita_Checkpoint("images: %d loaded this pass; running totals: they hold %d KB, the pools lost %d KB",
            loadCount, (int)(vitaImageExpectedBytes / 1024), (int)(vitaImageMeasuredBytes / 1024));
    }
#endif
    return loadCount;
}

/*
===============
idImageManager::EndLevelLoad
===============
*/
void idImageManager::EndLevelLoad()
{
    insideLevelLoad = false;

    common->Printf("----- idImageManager::EndLevelLoad -----\n");
    int start = Sys_Milliseconds();
    int loadCount = LoadLevelImages(true);

    int end = Sys_Milliseconds();
    common->Printf("%5i images loaded in %5.1f seconds\n", loadCount, (end - start) * 0.001);
    PrintMemorySummary("after level load");

#if defined(__vita__)
    // Totals for the two load paths - see the note in idImage::ActuallyLoadImage.
    // Anything non-zero in the second number means textures are being DXT
    // compressed on the console instead of using the shipped .bimage files.
    extern int vitaImagesFromBimage;
    extern int vitaImagesRecompressed;
    extern int vitaImagesYCoCg;
    extern int vitaRGB565Images, vitaRGB565Levels;
    extern int vitaCoverageImages, vitaCoverageLevels, vitaCoverageMaxError;
    extern int vitaUploadVerifyOk, vitaUploadVerifyBad, vitaUploadVerifyDropped;
    Vita_Checkpoint("images: %d from .bimage, %d recompressed on the Vita, %d are YCoCg-DXT5 diffuse",
        vitaImagesFromBimage, vitaImagesRecompressed, vitaImagesYCoCg);
    Vita_Checkpoint("images: %d RGB565 light images (%d levels) byte-swapped; %d coverage/font images (%d levels) transcoded DXT1->BC4, worst error %d/255",
        vitaRGB565Images, vitaRGB565Levels, vitaCoverageImages, vitaCoverageLevels, vitaCoverageMaxError);
    Vita_Checkpoint("images: uncompressed upload check: %d levels intact in GPU memory, %d CORRUPTED, %d not allocated by vitaGL (running totals)",
        vitaUploadVerifyOk, vitaUploadVerifyBad, vitaUploadVerifyDropped);
#endif
    common->Printf("----------------------------------------\n");
    // R_ListImages_f( idCmdArgs( "sorted sorted", false ) );
}

/*
===============
idImageManager::PrintMemorySummary

Texture memory by usage. On the Vita the sizes are what was allocated (after
vita_imageDownSize); elsewhere a second figure estimates what the Vita would
hold with the same rule (one top mip level dropped from 2D diffuse, bump and
specular maps of 64 px and up).
===============
*/
idCVar r_imageSummaryTop("r_imageSummaryTop", "0", CVAR_RENDERER | CVAR_INTEGER, "also list this many of the biggest loaded images after a level load");

void idImageManager::PrintMemorySummary(const char* when)
{
    static const char* names[] = { "specular", "diffuse", "default", "bump", "font", "light", "lut", "lut", "lut", "lut", "coverage", "depth" };
    int64 bytes[12] = { 0 };
    int counts[12] = { 0 };
    int64 total = 0, vitaTotal = 0, outsideBytes = 0;
    int loaded = 0, outside = 0;
    for (int i = 0; i < images.Num(); i++) {
        const idImage* img = images[i];
        if (img == NULL || !img->IsLoaded()) {
            continue;
        }
        int64 sz = img->StorageSize();
        if (img->opts.textureType == TT_CUBIC) {
            sz *= 6;
        }
        const int u = idMath::ClampInt(0, 11, (int)img->usage);
        bytes[u] += sz;
        counts[u]++;
        total += sz;
        loaded++;
        if (img->referencedOutsideLevelLoad) {
            outside++;
            outsideBytes += sz;
        }
#if defined(__vita__)
        vitaTotal += sz;
#else
        const bool shrinks = R_VitaImageCanShrink(img->GetName(), img->usage, img->opts.textureType, img->opts.numLevels)
            && img->opts.numLevels > 1 && img->opts.width >= 64 && img->opts.height >= 64;
        vitaTotal += shrinks ? sz / 4 : sz;
#endif
    }
    common->Printf("images %s: %d loaded, %d KB (Vita: %d KB) - diffuse %d (%d KB), bump %d (%d KB), specular %d (%d KB), default %d (%d KB), light %d (%d KB), other %d KB\n",
        when, loaded, (int)(total / 1024), (int)(vitaTotal / 1024),
        counts[TD_DIFFUSE], (int)(bytes[TD_DIFFUSE] / 1024), counts[TD_BUMP], (int)(bytes[TD_BUMP] / 1024),
        counts[TD_SPECULAR], (int)(bytes[TD_SPECULAR] / 1024), counts[TD_DEFAULT], (int)(bytes[TD_DEFAULT] / 1024),
        counts[TD_LIGHT], (int)(bytes[TD_LIGHT] / 1024),
        (int)((total - bytes[TD_DIFFUSE] - bytes[TD_BUMP] - bytes[TD_SPECULAR] - bytes[TD_DEFAULT] - bytes[TD_LIGHT]) / 1024));
    common->Printf("images %s: %d of them (%d KB) were first used outside a level load and are kept across levels\n",
        when, outside, (int)(outsideBytes / 1024));
    extern void R_ToksvigReport(const char* when);
    R_ToksvigReport(when); // the normal maps filtered since the last report (Toksvig.cpp)

    const int top = r_imageSummaryTop.GetInteger();
    if (top > 0) {
        idList<int> order;
        for (int i = 0; i < images.Num(); i++) {
            if (images[i] != NULL && images[i]->IsLoaded()) {
                order.Append(i);
            }
        }
        // selection of the biggest few; the list is short enough
        for (int n = 0; n < top && n < order.Num(); n++) {
            int best = n;
            for (int k = n + 1; k < order.Num(); k++) {
                if (images[order[k]]->StorageSize() > images[order[best]]->StorageSize()) {
                    best = k;
                }
            }
            SwapValues(order[n], order[best]);
            const idImage* img = images[order[n]];
            common->Printf("  %6d KB %4dx%-4d %s usage %d fmt %d levels %d %s\n", img->StorageSize() / 1024, img->opts.width, img->opts.height,
                img->opts.textureType == TT_CUBIC ? "cube" : "2d  ", (int)img->usage, (int)img->opts.format, img->opts.numLevels, img->GetName());
        }
    }
}

/*
===============
idImageManager::StartBuild
===============
*/
void idImageManager::StartBuild()
{
}

/*
===============
idImageManager::FinishBuild
===============
*/
void idImageManager::FinishBuild(bool removeDups)
{
}

/*
===============
idImageManager::PrintMemInfo
===============
*/
void idImageManager::PrintMemInfo(MemInfo_t* mi)
{
    int i, j, total = 0;
    int* sortIndex;
    idFile* f;

    f = fileSystem->OpenFileWrite(mi->filebase + "_images.txt");
    if (!f) {
        return;
    }

    // sort first
    sortIndex = new (TAG_IMAGE) int[images.Num()];

    for (i = 0; i < images.Num(); i++) {
        sortIndex[i] = i;
    }

    for (i = 0; i < images.Num() - 1; i++) {
        for (j = i + 1; j < images.Num(); j++) {
            if (images[sortIndex[i]]->StorageSize() < images[sortIndex[j]]->StorageSize()) {
                int temp = sortIndex[i];
                sortIndex[i] = sortIndex[j];
                sortIndex[j] = temp;
            }
        }
    }

    // print next
    for (i = 0; i < images.Num(); i++) {
        idImage* im = images[sortIndex[i]];
        int size;

        size = im->StorageSize();
        total += size;

        f->Printf("%s %3i %s\n", idStr::FormatNumber(size).c_str(), im->refCount, im->GetName());
    }

    delete[] sortIndex;
    mi->imageAssetsTotal = total;

    f->Printf("\nTotal image bytes allocated: %s\n", idStr::FormatNumber(total).c_str());
    fileSystem->CloseFile(f);
}
