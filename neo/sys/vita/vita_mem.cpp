/*
===========================================================================

Doom 3 BFG Edition GPL Source Code — PS Vita allocator wrappers.

vitaGL is built here with HAVE_WRAPPED_ALLOCATORS, which means it expects the
application to link with -Wl,--wrap for the C allocators and route them into
vglMalloc & friends. vglMalloc cascades across every pool the console has:

    newlib heap -> vitaGL RAM mspace -> phycont RAM -> CDLG budget -> VRAM

That matters a lot here. DOOM 3 BFG was written against a 512 MB console with a
unified heap, and the Vita only reaches a comparable figure by pooling all of
its separate memory regions. Wiring the wrappers up means every idHeap, every
STL container and every C++ new in the engine can spill into VRAM as a last
resort instead of failing outright, without the engine knowing anything about it.

The split is tuned in neo/sys/vita/vita_main.cpp: the newlib heap is kept small
enough that vitaGL can claim the bulk of main RAM when SDL brings up the GLES
context, and large enough to cover everything allocated before that point.

===========================================================================
*/

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include <vitaGL.h>
#include <psp2/kernel/sysmem.h>
#include <reent.h>

#include "vita_public.h"

extern "C" {

void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);
void* __real_memalign(size_t alignment, size_t size);

// vitaGL's per-pool allocators (utils/mem_utils.c)
void* vgl_malloc(size_t size, vglMemType type);
void* vgl_memalign(size_t alignment, size_t size, vglMemType type);
size_t vgl_malloc_usable_size(void* ptr);

extern unsigned int _newlib_heap_size_user;
void __malloc_lock(struct _reent* r);
void __malloc_unlock(struct _reent* r);
extern struct mallinfo __malloc_current_mallinfo;
extern void* __malloc_av_[];

/*
================
The newlib reserve

newlib's own allocations - _malloc_r from inside the C library: printf's
float conversion (dtoa's Bigints), stdio buffers, per-thread state - do not
go through these wrappers, so they cannot fall back to vitaGL's pools. Once
the game's allocations had used the newlib heap up, a "%.1f" log line on a job
thread asserted in dtoa ("Balloc succeeded") and aborted: firing a weapon in
mars_city2 crashed the game. So the wrappers stop taking newlib memory while
its never-used space plus top chunk is under VITA_NEWLIB_RESERVE and go
straight to vitaGL's pools; newlib keeps that much for itself. (Free chunks
inside the arena are not counted - reading them means walking the heap - so
this errs towards the pools.)
================
*/
static const size_t VITA_NEWLIB_RESERVE = 4 * 1024 * 1024;

static inline bool Vita_NewlibHasRoom(size_t size)
{
    const size_t arena = __malloc_current_mallinfo.arena;
    const size_t untouched = (_newlib_heap_size_user > arena) ? _newlib_heap_size_user - arena : 0;
    const size_t* topChunk = (const size_t*)__malloc_av_[2];
    const size_t top = (topChunk != NULL) ? (topChunk[1] & ~(size_t)3) : 0;
    return untouched + top > VITA_NEWLIB_RESERVE + size;
}

/*
================
Vita_NewlibFreeChunk

Once the newlib heap is down to its reserve, the wrappers below send every
allocation to vitaGL's uncached pools - yet the heap still holds free chunks
inside it (8 MB of them after mars_city2's load, and every monster, projectile
and gib freed during play adds more). Everything spawned in a fight landed in
uncached memory the CPU reads at a fraction of the speed while those chunks sat
unused. So try newlib first anyway, under its own (recursive) lock: an
allocation that newlib served from a free chunk is kept; one it had to carve
from the top chunk - the reserve - or grow the arena for is handed straight
back (it merges into the top again) and the caller goes to the pools.
================
*/
static void* Vita_NewlibFreeChunk(size_t alignment, size_t size)
{
    __malloc_lock(_REENT);
    const void* topBefore = __malloc_av_[2];
    const size_t arenaBefore = __malloc_current_mallinfo.arena;
    void* p = (alignment > 8) ? __real_memalign(alignment, size) : __real_malloc(size);
    if (p != NULL && (__malloc_av_[2] != topBefore || __malloc_current_mallinfo.arena != arenaBefore)) {
        __real_free(p);
        p = NULL;
    }
    __malloc_unlock(_REENT);
    return p;
}

// vglMalloc's cascade without its first step (newlib)
static void* Vita_PoolMemalign(size_t alignment, size_t size)
{
    static const vglMemType order[4] = { VGL_MEM_RAM, VGL_MEM_SLOW, VGL_MEM_BUDGET, VGL_MEM_VRAM };
    for (int i = 0; i < 4; i++) {
        void* p = (alignment > 8) ? vgl_memalign(alignment, size, order[i]) : vgl_malloc(size, order[i]);
        if (p != NULL) {
            return p;
        }
    }
    return NULL;
}

static inline bool Vita_IsNewlibPointer(void* ptr)
{
    // the newlib heap is the one region vitaGL calls "external"
    static uintptr_t lo = 0, hi = 0;
    if (lo == 0) {
        SceKernelMemBlockInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        void* probe = __real_malloc(1);
        if (probe != NULL && sceKernelGetMemBlockInfoByAddr(probe, &info) == 0) {
            lo = (uintptr_t)info.mappedBase;
            hi = lo + info.mappedSize;
        }
        __real_free(probe);
    }
    return (uintptr_t)ptr >= lo && (uintptr_t)ptr < hi;
}

void* __wrap_malloc(size_t size)
{
    if (!Vita_NewlibHasRoom(size)) {
        void* p = Vita_NewlibFreeChunk(8, size);
        if (p != NULL) {
            return p;
        }
        p = Vita_PoolMemalign(8, size);
        if (p != NULL) {
            return p;
        }
    }
    return vglMalloc(size);
}

void __wrap_free(void* ptr)
{
    if (ptr == NULL) {
        return;
    }
    vglFree(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size)
{
    if (!Vita_NewlibHasRoom(nmemb * size)) {
        void* p = Vita_NewlibFreeChunk(8, nmemb * size);
        if (p == NULL) {
            p = Vita_PoolMemalign(8, nmemb * size);
        }
        if (p != NULL) {
            memset(p, 0, nmemb * size);
            return p;
        }
    }
    return vglCalloc(nmemb, size);
}

void* __wrap_realloc(void* ptr, size_t size)
{
    // vgl_realloc identifies the source pool from the pointer, so it cannot
    // classify NULL. Both degenerate cases are handled here instead.
    if (ptr == NULL) {
        return __wrap_malloc(size);
    }
    if (size == 0) {
        vglFree(ptr);
        return NULL;
    }
    // growing a newlib block would take from the reserve (and vitaGL's realloc
    // keeps a newlib block in newlib): move it to the pools instead
    if (Vita_IsNewlibPointer(ptr) && !Vita_NewlibHasRoom(size)) {
        const size_t old = vgl_malloc_usable_size(ptr);
        if (old >= size) {
            return ptr;
        }
        void* p = Vita_PoolMemalign(8, size);
        if (p != NULL) {
            memcpy(p, ptr, old);
            vglFree(ptr);
            return p;
        }
    }
    return vglRealloc(ptr, size);
}

void* __wrap_memalign(size_t alignment, size_t size)
{
    if (!Vita_NewlibHasRoom(size + alignment)) {
        void* p = Vita_NewlibFreeChunk(alignment, size);
        if (p != NULL) {
            return p;
        }
        p = Vita_PoolMemalign(alignment, size);
        if (p != NULL) {
            return p;
        }
    }
    return vglMemalign(alignment, size);
}

/*
================
__wrap_aligned_alloc

The C11 spelling of memalign, and the one OpenAL Soft actually uses for its
buffer storage - libopenal.a references malloc, calloc, realloc and
aligned_alloc, and only the first three were wrapped. That left alBufferData()
allocating straight out of the newlib heap with no access to vglMalloc's
fallback chain, so it returned AL_OUT_OF_MEMORY loading map audio while the
vitaGL RAM pool still had 104 MB free. libbox2d.a uses it too.

C11 requires size to be a multiple of alignment; vglMemalign does not care, and
neither do the callers, so the stricter contract is not worth enforcing here.
================
*/
void* __wrap_aligned_alloc(size_t alignment, size_t size)
{
    return __wrap_memalign(alignment, size);
}

void* __wrap_valloc(size_t size)
{
    return __wrap_memalign(4096, size);
}

size_t __wrap_malloc_usable_size(void* ptr)
{
    if (ptr == NULL) {
        return 0;
    }
    return vglMallocUsableSize(ptr);
}

/*
================
posix_memalign

Not implemented by the VitaSDK newlib. idHeap's Mem_Alloc16() is the only caller.
================
*/
int posix_memalign(void** memptr, size_t alignment, size_t size)
{
    // POSIX: alignment must be a power of two and a multiple of sizeof(void*).
    if (memptr == NULL) {
        return 22; // EINVAL
    }
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
        *memptr = NULL;
        return 22; // EINVAL
    }

    void* p = __wrap_memalign(alignment, size);
    if (p == NULL) {
        // vglMemalign walks newlib -> RAM -> phycont -> CDLG -> VRAM, so getting
        // NULL back means every one of them refused. That is worth a line,
        // because the interesting case is the one seen loading mars_city2: a
        // 2.7 MB request failed while the RAM pool still reported 104 MB free,
        // which says the failure is contiguity rather than capacity. Print what
        // each pool claimed so the two can be told apart. Capped, and
        // Vita_Checkpoint formats into stack buffers, so this cannot recurse
        // back into the allocator.
        static int logged = 0;
        if (logged < 8) {
            logged++;
            Vita_Checkpoint("vglMemalign FAILED align=%u size=%u | RAM %u KB | phycont %u KB | CDLG %u KB | VRAM %u KB",
                (unsigned int)alignment, (unsigned int)size,
                (unsigned int)(vglMemFree(VGL_MEM_RAM) / 1024),
                (unsigned int)(vglMemFree(VGL_MEM_SLOW) / 1024),
                (unsigned int)(vglMemFree(VGL_MEM_BUDGET) / 1024),
                (unsigned int)(vglMemFree(VGL_MEM_VRAM) / 1024));
        }
        *memptr = NULL;
        return 12; // ENOMEM
    }

    *memptr = p;
    return 0;
}

} // extern "C"

/*
================
Where the engine's allocations land, by memory tag (Mem_Alloc16, Heap.cpp):
bytes that went into the cached newlib heap and bytes that spilled into
vitaGL's uncached pools, since the last reset (each map load). vita_main.cpp
prints them when the map is loaded - the tags that spill are the data the CPU
then reads slowly for the whole map.
================
*/
volatile uint64_t vita_tagCachedBytes[VITA_MEM_TAGS];
volatile uint64_t vita_tagSpilledBytes[VITA_MEM_TAGS];

void Vita_NoteTagAlloc(int tag, size_t size, void* p)
{
    if (p == NULL || tag < 0 || tag >= VITA_MEM_TAGS) {
        return;
    }
    if (Vita_IsNewlibPointer(p)) {
        __sync_fetch_and_add(&vita_tagCachedBytes[tag], (uint64_t)size);
    } else {
        __sync_fetch_and_add(&vita_tagSpilledBytes[tag], (uint64_t)size);
    }
}
