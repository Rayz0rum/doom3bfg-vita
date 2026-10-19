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
#include "../sys/vita/vita_zones.h" // GD_SCOPE / GD_PASS: the game/draw thread's tree of passes (VitaBench.cpp)

/*
==========================================================================================

FRAME MEMORY ALLOCATION

==========================================================================================
*/

static const unsigned int NUM_FRAME_DATA = 2;
#if defined(__vita__)
// Vita: 32, the Cortex-A9's cache line. BFG rounded every frame allocation to
// 128 bytes and cleared it 128 at a time for the Xbox 360's dcbz128 (which
// clears a line without reading it); on the A9 clearing a line reads it from
// memory first, and the rounding padded a 96-byte draw surface to 128 and a
// shadow volume parm to 256 - thousands of them a frame, on a memory system
// that copies ~330 MB/s. Rounded to 32 and cleared with memset (exactly the
// rounded size: the 128-byte line clear rounds its address down, which would
// clear the allocation before it). Only addresses and padding change.
static const unsigned int FRAME_ALLOC_ALIGNMENT = 32;
#else
static const unsigned int FRAME_ALLOC_ALIGNMENT = 128;
#endif
#if defined(__vita__)
// NUM_FRAME_DATA of these are allocated up front in R_InitFrameData(), so the
// stock 64 MB is 128 MB of frame memory. Mem_Alloc16 goes through __wrap_malloc
// into vglMalloc, which means it comes out of the same vitaGL pool the renderer
// allocates textures and buffers from - measured as a 136 MB drop across
// idRenderSystemLocal::Init(), leaving 174 KB by the time a map tried to load.
//
// The stock size is deliberate headroom for noclipping around a level on PC
// with everything visible at once; a 960x544 view of a normally-clipped scene
// needs far less. R_FrameAlloc() calls idLib::Error() rather than degrading if
// this is too small, so it keeps real headroom over the observed high water
// mark, and R_FrameAlloc() warns once if a frame ever gets within 25% of it.
// 8 MB, not 16: NUM_FRAME_DATA of these are allocated, so this is 16 MB of the
// newlib heap reserved up front. The 75%-usage warning below reports if a frame
// ever comes close, and nothing has tripped it yet at 16 MB.
static const unsigned int MAX_FRAME_MEMORY = 8 * 1024 * 1024;
#else
static const unsigned int MAX_FRAME_MEMORY = 64 * 1024 * 1024; // larger so that we can noclip on PC for dev purposes
#endif

idFrameData smpFrameData[NUM_FRAME_DATA];
idFrameData* frameData;
unsigned int smpFrame;

// #define TRACK_FRAME_ALLOCS

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
================================================================================

Vita: R_FrameAlloc from per-thread chunks.

Every frame allocation used to be an interlocked add on one counter
(frameData->frameMemoryAllocated): a full barrier on each side, and the model
jobs - three or four threads at once - make ~7,000 of them a frame (a light
surface, a shadow surface and a shadow volume parm per surface x light pair),
so that counter's cache line moved from core to core on almost every one.
Now a thread takes an 8 KB chunk with one interlocked add and cuts its small
allocations out of it (rounded and cleared exactly as before), so only the
addresses change. Chunks are dropped when the frame data toggles (a
generation number), and used only once the thread register is known to tell
threads apart (Vita_ThreadTokensPerThread, VitaBench.cpp) - each slot belongs
to one thread, which alone writes it. Allocations over a quarter of a chunk
take the stock path.

================================================================================
*/
uintptr_t Vita_ThreadToken();          // VitaBench.cpp
bool Vita_ThreadTokensPerThread();     // VitaBench.cpp
int vita_frameMemoryPeak = 0;          // the highest frame memory use since the last perf line
extern const int vita_frameMemoryMax = MAX_FRAME_MEMORY; // extern: a namespace-scope const is file-local otherwise
idCVar vita_frameAllocChunks("vita_frameAllocChunks", "1", CVAR_RENDERER | CVAR_BOOL, "Vita: front end frame allocations from per-thread 8 KB chunks instead of one shared counter each");
static const int VITA_FRAME_CHUNK = 8192;
static const int VITA_FRAME_CHUNK_SLOTS = 24;
struct vitaFrameChunk_t {
    uintptr_t token; // the owning thread's (0 = slot not filled in yet)
    unsigned int generation;
    byte* cur;
    byte* end;
    int served; // allocations cut from this thread's chunks since it last took one
};
static vitaFrameChunk_t vitaFrameChunks[VITA_FRAME_CHUNK_SLOTS];
static volatile int vitaNumFrameChunkSlots = 0;
static volatile unsigned int vitaFrameChunkGeneration = 1;

static vitaFrameChunk_t* Vita_FrameChunkSlot()
{
    if (!vita_frameAllocChunks.GetBool() || !Vita_ThreadTokensPerThread()) {
        return NULL;
    }
    const uintptr_t token = Vita_ThreadToken();
    if (token == 0) {
        return NULL;
    }
    const int n = vitaNumFrameChunkSlots;
    for (int i = 0; i < n && i < VITA_FRAME_CHUNK_SLOTS; i++) {
        if (vitaFrameChunks[i].token == token) {
            return &vitaFrameChunks[i];
        }
    }
    // a thread's first frame allocation: claim a slot (once per thread)
    const int i = __sync_fetch_and_add(&vitaNumFrameChunkSlots, 1);
    if (i >= VITA_FRAME_CHUNK_SLOTS) {
        return NULL; // more threads than slots: the stock path for the rest
    }
    vitaFrameChunk_t& c = vitaFrameChunks[i];
    c.generation = 0;
    c.cur = c.end = NULL;
    c.served = 0;
    __sync_synchronize();
    c.token = token;
    return &c;
}
#endif

#if defined(TRACK_FRAME_ALLOCS)
idSysInterlockedInteger frameAllocTypeCount[FRAME_ALLOC_MAX];
int frameHighWaterTypeCount[FRAME_ALLOC_MAX];
#endif

/*
====================
R_ToggleSmpFrame
====================
*/
void R_ToggleSmpFrame()
{
    // update the highwater mark
    if (frameData->frameMemoryAllocated.GetValue() > frameData->highWaterAllocated) {
        frameData->highWaterAllocated = frameData->frameMemoryAllocated.GetValue();
#if defined(TRACK_FRAME_ALLOCS)
        frameData->highWaterUsed = frameData->frameMemoryUsed.GetValue();
        for (int i = 0; i < FRAME_ALLOC_MAX; i++) {
            frameHighWaterTypeCount[i] = frameAllocTypeCount[i].GetValue();
        }
#endif
    }

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    // the frame memory peak for the perf log (common_frame.cpp), and every
    // thread's chunk of the frame ending is dropped (see R_FrameAlloc)
    if (frameData->frameMemoryAllocated.GetValue() > vita_frameMemoryPeak) {
        vita_frameMemoryPeak = frameData->frameMemoryAllocated.GetValue();
    }
    vitaFrameChunkGeneration++;
#endif

    // switch to the next frame
    smpFrame++;
    frameData = &smpFrameData[smpFrame % NUM_FRAME_DATA];

    // reset the memory allocation

    // RB: 64 bit fixes, changed unsigned int to uintptr_t
    const uintptr_t bytesNeededForAlignment = FRAME_ALLOC_ALIGNMENT - ((uintptr_t)frameData->frameMemory & (FRAME_ALLOC_ALIGNMENT - 1));
    // RB end

    frameData->frameMemoryAllocated.SetValue(bytesNeededForAlignment);
    frameData->frameMemoryUsed.SetValue(0);

#if defined(TRACK_FRAME_ALLOCS)
    for (int i = 0; i < FRAME_ALLOC_MAX; i++) {
        frameAllocTypeCount[i].SetValue(0);
    }
#endif

    // clear the command chain and make a RC_NOP command the only thing on the list
    frameData->cmdHead = frameData->cmdTail = (emptyCommand_t*)R_FrameAlloc(sizeof(*frameData->cmdHead), FRAME_ALLOC_DRAW_COMMAND);
    frameData->cmdHead->commandId = RC_NOP;
    frameData->cmdHead->next = NULL;
}

/*
=====================
R_ShutdownFrameData
=====================
*/
void R_ShutdownFrameData()
{
    frameData = NULL;
    for (int i = 0; i < NUM_FRAME_DATA; i++) {
        Mem_Free16(smpFrameData[i].frameMemory);
        smpFrameData[i].frameMemory = NULL;
    }
}

/*
=====================
R_InitFrameData
=====================
*/
void R_InitFrameData()
{
    R_ShutdownFrameData();

    for (int i = 0; i < NUM_FRAME_DATA; i++) {
        smpFrameData[i].frameMemory = (byte*)Mem_Alloc16(MAX_FRAME_MEMORY, TAG_RENDER);
    }

    // must be set before calling R_ToggleSmpFrame()
    frameData = &smpFrameData[0];

    R_ToggleSmpFrame();
}

/*
================
R_FrameAlloc

This data will be automatically freed when the
current frame's back end completes.

This should only be called by the front end.  The
back end shouldn't need to allocate memory.

All temporary data, like dynamic tesselations
and local spaces are allocated here.

All memory is cache-line-cleared for the best performance.
================
*/
void* R_FrameAlloc(int bytes, frameAllocType_t type)
{
#if defined(TRACK_FRAME_ALLOCS)
    frameData->frameMemoryUsed.Add(bytes);
    frameAllocTypeCount[type].Add(bytes);
#endif

    bytes = (bytes + FRAME_ALLOC_ALIGNMENT - 1) & ~(FRAME_ALLOC_ALIGNMENT - 1);

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (bytes <= VITA_FRAME_CHUNK / 4) {
        vitaFrameChunk_t* c = Vita_FrameChunkSlot();
        if (c != NULL) {
            if (c->generation != vitaFrameChunkGeneration || c->cur + bytes > c->end) {
                const int chunkEnd = frameData->frameMemoryAllocated.Add(VITA_FRAME_CHUNK);
                if (chunkEnd > MAX_FRAME_MEMORY) {
                    idLib::Error("R_FrameAlloc ran out of memory taking a chunk. end = %d, highWaterAllocated = %d\n", chunkEnd, frameData->highWaterAllocated);
                }
                VITA_ZONE_COUNT("frame memory: 8 KB chunks taken by threads");
                VITA_ZONE_ADD("frame memory: allocations cut from a thread's chunk", c->served);
                c->served = 0;
                c->cur = frameData->frameMemory + chunkEnd - VITA_FRAME_CHUNK;
                c->end = c->cur + VITA_FRAME_CHUNK;
                c->generation = vitaFrameChunkGeneration;
            }
            byte* ptr = c->cur;
            c->cur += bytes;
            c->served++;
            // the next allocations' lines asked for now (for writing): clearing
            // them reads each 32-byte line from memory first on the A9, and
            // asked for early those reads overlap the work between allocations
            __builtin_prefetch(c->cur, 1);
            __builtin_prefetch(c->cur + 32, 1);
            __builtin_prefetch(c->cur + 64, 1);
            __builtin_prefetch(c->cur + 96, 1);
            __builtin_prefetch(c->cur + 128, 1);
            __builtin_prefetch(c->cur + 160, 1);
            __builtin_prefetch(c->cur + 192, 1);
            __builtin_prefetch(c->cur + 224, 1);
#if defined(__vita__)
            memset(ptr, 0, bytes);
#else
            // cache line clear the memory
            for (int offset = 0; offset < bytes; offset += CACHE_LINE_SIZE) {
                ZeroCacheLine(ptr, offset);
            }
#endif
            return ptr;
        }
    }
#endif

    // thread safe add
    int end = frameData->frameMemoryAllocated.Add(bytes);
    if (end > MAX_FRAME_MEMORY) {
        idLib::Error("R_FrameAlloc ran out of memory. bytes = %d, end = %d, highWaterAllocated = %d\n", bytes, end, frameData->highWaterAllocated);
    }
#if defined(__vita__)
    // MAX_FRAME_MEMORY is cut to a quarter of stock here, and overflowing it is
    // a hard idLib::Error rather than a degraded frame. Say so once while there
    // is still headroom, so the limit can be raised before it becomes a crash.
    {
        static bool warned = false;
        if (!warned && end > (int)(MAX_FRAME_MEMORY - MAX_FRAME_MEMORY / 4)) {
            warned = true;
            idLib::Warning("R_FrameAlloc within 25%% of the Vita frame memory limit: %d of %d bytes used",
                end, (int)MAX_FRAME_MEMORY);
        }
    }
#endif

    byte* ptr = frameData->frameMemory + end - bytes;

#if defined(__vita__)
    memset(ptr, 0, bytes); // see FRAME_ALLOC_ALIGNMENT
#else
    // cache line clear the memory
    for (int offset = 0; offset < bytes; offset += CACHE_LINE_SIZE) {
        ZeroCacheLine(ptr, offset);
    }
#endif

    return ptr;
}

/*
==================
R_ClearedFrameAlloc
==================
*/
void* R_ClearedFrameAlloc(int bytes, frameAllocType_t type)
{
    // NOTE: every allocation is cache line cleared
    return R_FrameAlloc(bytes, type);
}

/*
==========================================================================================

FONT-END STATIC MEMORY ALLOCATION

==========================================================================================
*/

/*
=================
R_StaticAlloc
=================
*/
void* R_StaticAlloc(int bytes, const memTag_t tag)
{
    tr.pc.c_alloc++;

    void* buf = Mem_Alloc(bytes, tag);

    // don't exit on failure on zero length allocations since the old code didn't
    if (buf == NULL && bytes != 0) {
        common->FatalError("R_StaticAlloc failed on %i bytes", bytes);
    }
    return buf;
}

/*
=================
R_ClearedStaticAlloc
=================
*/
void* R_ClearedStaticAlloc(int bytes)
{
    void* buf = R_StaticAlloc(bytes);
    memset(buf, 0, bytes);
    return buf;
}

/*
=================
R_StaticFree
=================
*/
void R_StaticFree(void* data)
{
    tr.pc.c_free++;
    Mem_Free(data);
}

/*
==========================================================================================

FONT-END RENDERING

==========================================================================================
*/

/*
=================
R_SortDrawSurfs
=================
*/
static void R_SortDrawSurfs(drawSurf_t** drawSurfs, const int numDrawSurfs)
{
#if 1

    uint64* indices = (uint64*)_alloca16(numDrawSurfs * sizeof(indices[0]));

    // sort the draw surfs based on:
    // 1. sort value (largest first)
    // 2. depth (smallest first)
    // 3. index (largest first)
    assert(numDrawSurfs <= 0xFFFF);
    for (int i = 0; i < numDrawSurfs; i++) {
        float sort = SS_POST_PROCESS - drawSurfs[i]->sort;
        assert(sort >= 0.0f);

        uint64 dist = 0;
        if (drawSurfs[i]->frontEndGeo != NULL) {
            float min = 0.0f;
            float max = 1.0f;
            idRenderMatrix::DepthBoundsForBounds(min, max, drawSurfs[i]->space->mvp, drawSurfs[i]->frontEndGeo->bounds);
            dist = idMath::Ftoui16(min * 0xFFFF);
        }

        indices[i] = ((numDrawSurfs - i) & 0xFFFF) | (dist << 16) | ((uint64)(*(uint32*)&sort) << 32);
    }

    const int64 MAX_LEVELS = 128;
    int64 lo[MAX_LEVELS];
    int64 hi[MAX_LEVELS];

    // Keep the top of the stack in registers to avoid load-hit-stores.
    register int64 st_lo = 0;
    register int64 st_hi = numDrawSurfs - 1;
    register int64 level = 0;

    for (;;) {
        register int64 i = st_lo;
        register int64 j = st_hi;
        if (j - i >= 4 && level < MAX_LEVELS - 1) {
            register uint64 pivot = indices[(i + j) / 2];
            do {
                while (indices[i] > pivot)
                    i++;
                while (indices[j] < pivot)
                    j--;
                if (i > j)
                    break;
                uint64 h = indices[i];
                indices[i] = indices[j];
                indices[j] = h;
            } while (++i <= --j);

            // No need for these iterations because we are always sorting unique values.
            // while ( indices[j] == pivot && st_lo < j ) j--;
            // while ( indices[i] == pivot && i < st_hi ) i++;

            assert(level < MAX_LEVELS - 1);
            lo[level] = i;
            hi[level] = st_hi;
            st_hi = j;
            level++;
        } else {
            for (; i < j; j--) {
                register int64 m = i;
                for (int64 k = i + 1; k <= j; k++) {
                    if (indices[k] < indices[m]) {
                        m = k;
                    }
                }
                uint64 h = indices[m];
                indices[m] = indices[j];
                indices[j] = h;
            }
            if (--level < 0) {
                break;
            }
            st_lo = lo[level];
            st_hi = hi[level];
        }
    }

    drawSurf_t** newDrawSurfs = (drawSurf_t**)indices;
    for (int i = 0; i < numDrawSurfs; i++) {
        newDrawSurfs[i] = drawSurfs[numDrawSurfs - (indices[i] & 0xFFFF)];
    }
    memcpy(drawSurfs, newDrawSurfs, numDrawSurfs * sizeof(drawSurfs[0]));

#else

    struct local_t {
        static int R_QsortSurfaces(const void* a, const void* b)
        {
            const drawSurf_t* ea = *(drawSurf_t**)a;
            const drawSurf_t* eb = *(drawSurf_t**)b;
            if (ea->sort < eb->sort) {
                return -1;
            }
            if (ea->sort > eb->sort) {
                return 1;
            }
            return 0;
        }
    };

    // Add a sort offset so surfaces with equal sort orders still deterministically
    // draw in the order they were added, at least within a given model.
    float sorfOffset = 0.0f;
    for (int i = 0; i < numDrawSurfs; i++) {
        drawSurf[i]->sort += sorfOffset;
        sorfOffset += 0.000001f;
    }

    // sort the drawsurfs
    qsort(drawSurfs, numDrawSurfs, sizeof(drawSurfs[0]), local_t::R_QsortSurfaces);

#endif
}

/*
================
R_RenderView

A view may be either the actual camera view,
a mirror / remote location, or a 3D view on a gui surface.

Parms will typically be allocated with R_FrameAlloc
================
*/
#if defined(__vita__)
// Front end time by stage for the top-level views (common_frame.cpp sums and
// prints them with the perf line): 0 find visible lights/entities, 1 wait for
// shadow jobs, 2 lights, 3 models, 4 GUIs + light list + sort, 5 subviews.
uint64 vita_frontEndUs[6];
static int vitaRenderViewDepth = 0;
#define VITA_FE_STAGE(n) do { if (vitaRenderViewDepth == 1) { const uint64 t_ = Sys_Microseconds(); vita_frontEndUs[(n)] += t_ - vitaStageStart; vitaStageStart = t_; } } while (0)
#else
#define VITA_FE_STAGE(n) do { } while (0)
#endif

void R_RenderView(viewDef_t* parms)
{
#if defined(__vita__)
    vitaRenderViewDepth++;
    uint64 vitaStageStart = Sys_Microseconds();
#endif
    GD_PASS_BEGIN("view setup (matrices, frustum)");
    // save view in case we are a subview
    viewDef_t* oldView = tr.viewDef;

    tr.viewDef = parms;

    // setup the matrix for world space to eye space
    R_SetupViewMatrix(tr.viewDef);

    // we need to set the projection matrix before doing
    // portal-to-screen scissor calculations
    R_SetupProjectionMatrix(tr.viewDef);

    // setup render matrices for faster culling
    idRenderMatrix::Transpose(*(idRenderMatrix*)tr.viewDef->projectionMatrix, tr.viewDef->projectionRenderMatrix);
    idRenderMatrix viewRenderMatrix;
    idRenderMatrix::Transpose(*(idRenderMatrix*)tr.viewDef->worldSpace.modelViewMatrix, viewRenderMatrix);
    idRenderMatrix::Multiply(tr.viewDef->projectionRenderMatrix, viewRenderMatrix, tr.viewDef->worldSpace.mvp);

    // the planes of the view frustum are needed for portal visibility culling
    idRenderMatrix::GetFrustumPlanes(tr.viewDef->frustum, tr.viewDef->worldSpace.mvp, false, true);

    // the DOOM 3 frustum planes point outside the frustum
    for (int i = 0; i < 6; i++) {
        tr.viewDef->frustum[i] = -tr.viewDef->frustum[i];
    }
    // remove the Z-near to avoid portals from being near clipped
    tr.viewDef->frustum[4][3] -= r_znear.GetFloat();

    // identify all the visible portal areas, and create view lights and view entities
    // for all the the entityDefs and lightDefs that are in the visible portal areas
    GD_PASS("find visible lights + entities (portal flood)");
    static_cast<idRenderWorldLocal*>(parms->renderWorld)->FindViewLightsAndEntities();
    VITA_FE_STAGE(0);

    // wait for any shadow volume jobs from the previous frame to finish
    GD_PASS("wait for the previous view's jobs");
    tr.frontEndJobList->Wait();
    VITA_FE_STAGE(1);

    // make sure that interactions exist for all light / entity combinations that are visible
    // add any pre-generated light shadows, and calculate the light shader values
    GD_PASS("lights (R_AddLights)");
    R_AddLights();
    VITA_FE_STAGE(2);

    // adds ambient surfaces and create any necessary interaction surfaces to add to the light lists
    GD_PASS("models (R_AddModels)");
    R_AddModels();
    VITA_FE_STAGE(3);

    // build up the GUIs on world surfaces
    GD_PASS("GUIs on world surfaces (R_AddInGameGuis)");
    R_AddInGameGuis(tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs);

    // any viewLight that didn't have visible surfaces can have it's shadows removed
    GD_PASS("optimize the light list");
    R_OptimizeViewLightsList();

    // sort all the ambient surfaces for translucency ordering
    GD_PASS("sort draw surfaces");
    R_SortDrawSurfs(tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs);
    VITA_FE_STAGE(4);

    // generate any subviews (mirrors, cameras, etc) before adding this view
    GD_PASS("subviews (mirrors, cameras, remote screens)");
    if (R_GenerateSubViews(tr.viewDef->drawSurfs, tr.viewDef->numDrawSurfs)) {
        // if we are debugging subviews, allow the skipping of the main view draw
        if (r_subviewOnly.GetBool()) {
#if defined(__vita__)
            vitaRenderViewDepth--;
#endif
            GD_PASS_END();
            return;
        }
    }
    VITA_FE_STAGE(5);

    // write everything needed to the demo file
    if (common->WriteDemo()) {
        static_cast<idRenderWorldLocal*>(parms->renderWorld)->WriteVisibleDefs(tr.viewDef);
    }

    // add the rendering commands for this viewDef
    GD_PASS("add the draw-view command");
    R_AddDrawViewCmd(parms, false);
    GD_PASS_END();

    // restore view in case we are a subview
    tr.viewDef = oldView;
#if defined(__vita__)
    vitaRenderViewDepth--;
#endif
}

/*
================
R_RenderPostProcess

Because R_RenderView may be called by subviews we have to make sure the post process
pass happens after the active view and its subviews is done rendering.
================
*/
void R_RenderPostProcess(viewDef_t* parms)
{
    viewDef_t* oldView = tr.viewDef;

    R_AddDrawPostProcess(parms);

    tr.viewDef = oldView;
}
