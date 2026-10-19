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
#ifndef __VERTEXCACHE2_H__
#define __VERTEXCACHE2_H__

#if defined(__vita__)

// idVertexCache::Init() allocates all of these up front. At the stock sizes that
// is 2 * (31 + 31 + 0.25) + 31 + 31 = ~186 MB of GPU-visible buffers, which was
// sized for a 512 MB unified-memory console. The Vita has to fit them alongside
// everything else in main RAM, phycont and 128 MB of CDRAM, so they are scaled
// to what a 960x544 target actually consumes.
//
// Undersizing is safe to iterate on: ActuallyAlloc() returns 0 when a set is
// full and the surface is dropped with a warning, so a too-small cache shows up
// as missing geometry rather than a crash.
// Measured: these land in CDRAM, and at 10 MB each they were 70 MB of the 96 MB
// VRAM pool, leaving 13 MB for every texture a map loads. The per-frame sets are
// the safe ones to cut - see the note above - so they take the reduction and the
// static sets, whose overflow is a FatalError, keep their headroom.
const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 4 * 1024 * 1024;  // ~2M indices/frame
const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 4 * 1024 * 1024; // ~131k idDrawVert/frame
const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

// Four, not two: the Vita lets the GPU run up to two frames behind the CPU
// (no glFinish at the swap - see GL_BlockingSwapBuffers; vitaGL's display
// queue holds two pending frames with its three display buffers). While the
// front end fills frame N+1's set, the back end submits frame N and the GPU
// may still be drawing N-1 and N-2: four sets keep them all apart. The sets
// live in vitaGL's RAM pool (dynamic buffers), 8.25 MB each.
const int VERTCACHE_NUM_FRAMES = 4;

// there are a lot more static indexes than vertexes, because interactions are just new
// index lists that reference existing vertexes
// Sized from measurement rather than guessed. 8 MB of static vertex memory was
// too mean: d3xpdm3 died on "AllocStaticVertex failed", and of the maps that did
// load, d3dm5 reached 80% and delta4 82% - no margin at all. Vertex demand
// tracks geometry density, not map file size, which is why a 33 MB deathmatch
// map overflowed while a 55 MB campaign map fit.
//
// Index memory pays for it: the same three maps peaked at 17-19% of 8 MB, so
// halving it to 4 MB still leaves better than 2x headroom and offsets half the
// vertex increase. Net cost to the RAM pool is 4 MB.
//
// The campaign needs more than those deathmatch maps. Measured with the same
// data structures on a PC build ("Used N kb of static vertex memory" after
// each map load, all 47 single-player maps), the peaks are phobos3 (RoE) at
// 26.5 MB of vertices and 9.6 MB of indexes, then le_enpro2 (26.3 / 8.3) and
// erebus5 (25.4); the original campaign tops out at enpro (24.7 / 7.2), and
// mars_city2 alone (17.9 / 5.8) overflowed both old limits. 30 + 11 MB covers
// every map with 13% to spare.
const int STATIC_INDEX_MEMORY = 11 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY = 30 * 1024 * 1024; // must fit in VERTCACHE_OFFSET_MASK

#else

const int VERTCACHE_INDEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
const int VERTCACHE_VERTEX_MEMORY_PER_FRAME = 31 * 1024 * 1024;
const int VERTCACHE_JOINT_MEMORY_PER_FRAME = 256 * 1024;

const int VERTCACHE_NUM_FRAMES = 2;

// there are a lot more static indexes than vertexes, because interactions are just new
// index lists that reference existing vertexes
const int STATIC_INDEX_MEMORY = 31 * 1024 * 1024;
const int STATIC_VERTEX_MEMORY = 31 * 1024 * 1024; // make sure it fits in VERTCACHE_OFFSET_MASK!

#endif

// vertCacheHandle_t packs size, offset, and frame number into 64 bits
typedef uint64 vertCacheHandle_t;
const int VERTCACHE_STATIC = 1; // in the static set, not the per-frame set
const int VERTCACHE_SIZE_SHIFT = 1;
const int VERTCACHE_SIZE_MASK = 0x7fffff; // 8 megs
const int VERTCACHE_OFFSET_SHIFT = 24;
const int VERTCACHE_OFFSET_MASK = 0x1ffffff; // 32 megs
const int VERTCACHE_FRAME_SHIFT = 49;
const int VERTCACHE_FRAME_MASK = 0x7fff; // 15 bits = 32k frames to wrap around

const int VERTEX_CACHE_ALIGN = 32;
const int INDEX_CACHE_ALIGN = 16;
const int JOINT_CACHE_ALIGN = 16;

enum cacheType_t {
    CACHE_VERTEX,
    CACHE_INDEX,
    CACHE_JOINT
};

struct geoBufferSet_t {
    idIndexBuffer indexBuffer;
    idVertexBuffer vertexBuffer;
    idJointBuffer jointBuffer;
    byte* mappedVertexBase;
    byte* mappedIndexBase;
    byte* mappedJointBase;
    idSysInterlockedInteger indexMemUsed;
    idSysInterlockedInteger vertexMemUsed;
    idSysInterlockedInteger jointMemUsed;
    int allocations; // number of index and vertex allocations combined
};

class idVertexCache {
public:
    void Init(bool restart = false);
    void Shutdown();
    void PurgeAll();

    // call on loading a new map
    void FreeStaticData();

    // this data is only valid for one frame of rendering
    vertCacheHandle_t AllocVertex(const void* data, int bytes)
    {
        return ActuallyAlloc(frameData[listNum], data, bytes, CACHE_VERTEX);
    }
    vertCacheHandle_t AllocIndex(const void* data, int bytes)
    {
        return ActuallyAlloc(frameData[listNum], data, bytes, CACHE_INDEX);
    }
    vertCacheHandle_t AllocJoint(const void* data, int bytes)
    {
        return ActuallyAlloc(frameData[listNum], data, bytes, CACHE_JOINT);
    }

    // this data is valid until the next map load
    vertCacheHandle_t AllocStaticVertex(const void* data, int bytes)
    {
        if (staticData.vertexMemUsed.GetValue() + bytes > STATIC_VERTEX_MEMORY) {
            idLib::FatalError("AllocStaticVertex failed, increase STATIC_VERTEX_MEMORY");
        }
        return ActuallyAlloc(staticData, data, bytes, CACHE_VERTEX);
    }
    vertCacheHandle_t AllocStaticIndex(const void* data, int bytes)
    {
        if (staticData.indexMemUsed.GetValue() + bytes > STATIC_INDEX_MEMORY) {
            idLib::FatalError("AllocStaticIndex failed, increase STATIC_INDEX_MEMORY");
        }
        return ActuallyAlloc(staticData, data, bytes, CACHE_INDEX);
    }

    byte* MappedVertexBuffer(vertCacheHandle_t handle)
    {
        release_assert(!CacheIsStatic(handle));
        const uint64 offset = (int)(handle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;
        const uint64 frameNum = (int)(handle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
        release_assert(frameNum == (currentFrame & VERTCACHE_FRAME_MASK));
        return frameData[listNum].mappedVertexBase + offset;
    }

    byte* MappedIndexBuffer(vertCacheHandle_t handle)
    {
        release_assert(!CacheIsStatic(handle));
        const uint64 offset = (int)(handle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;
        const uint64 frameNum = (int)(handle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
        release_assert(frameNum == (currentFrame & VERTCACHE_FRAME_MASK));
        return frameData[listNum].mappedIndexBase + offset;
    }

    // Returns false if it's been purged
    // This can only be called by the front end, the back end should only be looking at
    // vertCacheHandle_t that are already validated.
    bool CacheIsCurrent(const vertCacheHandle_t handle)
    {
        const int isStatic = handle & VERTCACHE_STATIC;
        if (isStatic) {
            return true;
        }
        const uint64 frameNum = (int)(handle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
        if (frameNum != (currentFrame & VERTCACHE_FRAME_MASK)) {
            return false;
        }
        return true;
    }

    static bool CacheIsStatic(const vertCacheHandle_t handle)
    {
        return (handle & VERTCACHE_STATIC) != 0;
    }

    // vb/ib is a temporary reference -- don't store it
    bool GetVertexBuffer(vertCacheHandle_t handle, idVertexBuffer* vb);
    bool GetIndexBuffer(vertCacheHandle_t handle, idIndexBuffer* ib);
    bool GetJointBuffer(vertCacheHandle_t handle, idJointBuffer* jb);

    void BeginBackEnd();

public:
    int currentFrame; // for determining the active buffers
    int listNum;      // currentFrame % VERTCACHE_NUM_FRAMES
    int drawListNum;  // (currentFrame-1) % VERTCACHE_NUM_FRAMES

    geoBufferSet_t staticData;
    geoBufferSet_t frameData[VERTCACHE_NUM_FRAMES];

    // High water marks for the per-frame buffers
    int mostUsedVertex;
    int mostUsedIndex;
    int mostUsedJoint;

    // Try to make room for <bytes> bytes
    vertCacheHandle_t ActuallyAlloc(geoBufferSet_t& vcs, const void* data, int bytes, cacheType_t type);
};

// platform specific code to memcpy into vertex buffers efficiently
// 16 byte alignment is guaranteed
void CopyBuffer(byte* dst, const byte* src, int numBytes);

extern idVertexCache vertexCache;

#endif // __VERTEXCACHE2_H__
