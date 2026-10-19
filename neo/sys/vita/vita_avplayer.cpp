/*
===========================================================================
PS Vita hardware video playback for idCinematic.

The engine's own cinematic decoders are no use here. RoQ is a 1993 vector
quantiser decoded in scalar C: loadvideo is 1280x360 at 30 fps, which is
13.8 Mpixel/s on a 444 MHz Cortex-A9, so it cannot hold frame rate. It also
has no sound - idCinematicLocal's chunk dispatch has no ROQ_SOUND case at
all, which is why the intros are silent. Bink is proprietary and the FFmpeg
path is not built (FFmpeg would also add 2-4 MB to an ELF that has ~300 KB
of headroom before vita-elf-create stops being able to package it).

sceAvPlayer is the Vita's own H.264/AAC player. The decoding happens in
dedicated silicon, the library is a stub so it costs the ELF almost nothing,
and - the part that makes this cheap rather than merely possible - GXM's
texture unit can sample 2-plane YUV420 and do the colour conversion in the
sampler. So a decoded frame is handed to the GPU by pointing a texture at
the decoder's own output buffer. The CPU never touches a pixel and no
conversion shader is involved; the frame reaches the renderer as an ordinary
RGB texture as far as it is concerned.

Videos are converted offline to .mp4 (see VITA_PORT.md). If one is missing
the caller falls back to the RoQ path.
===========================================================================
*/

#if defined(__vita__)

#include <psp2/avplayer.h>
#include <psp2/audioout.h>
#include <psp2/gxm.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "vita_public.h"

// Present in libSceAvPlayer_stub.a but missing from psp2/avplayer.h.
extern "C" int sceAvPlayerStreamCount(SceAvPlayerHandle handle);

/*
==============
VitaAv_HandleIsError

SceAvPlayerHandle is typedef'd to int, but sceAvPlayerInit actually hands back
a pointer to its own state - the value it returned was byte for byte the
address of the first block it asked our allocator for. User-space addresses on
this console live at 0x8xxxxxxx, which is negative as a signed int, so testing
the handle for "< 0" rejected a player that had initialised perfectly.

Treat the handle as opaque instead: only NULL, or one of the documented
SceAvPlayerErrorCode values (0x806A0001 invalid param, 0x806A0003 out of
memory), mean failure.
==============
*/
static bool VitaAv_HandleIsError(SceAvPlayerHandle handle)
{
    const uint32_t value = (uint32_t)handle;
    return (value == 0) || ((value & 0xFFFF0000u) == 0x806A0000u);
}

// Number of decoded frames sceAvPlayer keeps in flight. A frame buffer is
// recycled after this many new frames, and because the renderer points a
// texture straight at one rather than copying out of it, that recycle has to
// stay far enough ahead of the GPU. The vitaGL sample calls for at least 4
// with triple buffering; 5 leaves margin.
#define VITA_VIDEO_BUFFERS 5

// sceKernelAllocMemBlock rounds physically contiguous blocks up to 1 MB, so
// ask for the rounded size rather than letting each allocation waste the
// remainder of a megabyte.
#define VITA_PHYCONT_ALIGN (1024 * 1024)
#define VITA_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/*
==============
Frame buffer pool

The decoder frame buffers are never unmapped or freed while the game runs.
They are handed back to a pool and reused instead.

This is not laziness, it is the only safe option. Releasing one means
sceGxmUnmapMemory, and unmapping memory a queued draw still references faults,
so the obvious guard is glFinish() first - but vitaGL implements glFinish() as
sceGxmFinish(), which must not be called inside an active scene. The frame
buffers are released exactly when the stream ends, and the code that notices
that runs from RB_BindVariableStageImage, i.e. between sceGxmBeginScene and
sceGxmEndScene. So the guard deadlocked the GPU with the last decoded frame on
screen: the process never exited, and neither the LiveArea nor sleep could
reclaim the display - the console had to be power cycled. Calling it from the
decoder thread instead (the first attempt) was the same deadlock by a
different route, since that thread does not own the GL context either.

Keeping the blocks mapped removes the problem rather than timing around it:
nothing is ever unmapped, so nothing needs waiting for. The cost is bounded -
this pool held 13 MB for the intro, out of a 26 MB physically contiguous
partition that nothing else in the game touches - and replaying a video reuses
the blocks instead of reallocating them.
==============
*/
/*
==============
Several players, opened and closed in the background

The shell has three video materials live at once (introloop, mars1,
mars_rotation) and a level can show several screens playing at the same time
- mc_underground's room of invasion monitors has seven. One player for the
whole game left every other video black.

A player is now opened when its video is first drawn and closed when it has
not been drawn for a while (idCinematicLocal, Cinematic.cpp), and as many run
at once as the decoder's memory allows: before a player opens, the physically
contiguous memory it will want - what a player of that size took the last
time, or the first time an estimate from the picture size (measured: the
decoder takes ~26 frames' worth, the output buffers 5 frames, plus 1 MB, each
block rounded up to 1 MB - 13 MB at 960x272, 3 MB for a 128x128 screen) -
must be free (or spare in the pool below). A flat 14 MB first guess kept the
512x512 intro loop waiting forever beside the 12 MB menu background. A player handed NULL frame buffers is left inconsistent and
faults on close, so the check is made before, never after.

sceAvPlayerInit / AddSource and the close (sceAvPlayerStop, the audio thread's
join, sceAvPlayerClose: ~70 ms) run on a worker thread - on core 3 when the
game may use it, where nothing waits for them - so the render thread never
stalls on them; it only detaches the texture before handing a player over to
be closed.
==============
*/
/*
One player at a time (2026-10-18). The first two runs of the full
verification on hardware: every player opened while another one was decoding
stalled after its first frame buffer - never asked for its decoder area,
never gave a frame (deployment.mp4 beside the menu's mars_rotation; the
menu's own mars_rotation, reopened beside a stalled one; the mars_city1
intro's tracking screen beside the other cockpit screens, dark blue static)
- while every player opened alone played (loadvideo, mars_rotation at boot,
every PDA disc in game). The hardware decoder serves one player here. A
video that wants a player takes it from another by the rules of
Vita_CinematicEvict (Cinematic.cpp): a video played once over a looping
screen, the newest thing on screen over older ones, anything not drawn for
200 ms or finished gives way - and the old player is closed before the new
one opens (vitaAvActivePlayers counts players still closing).
*/
#define VITA_MAX_CONCURRENT_PLAYERS 1
// A player's frame buffers (allocateTexture), as measured: the output
// buffers (VITA_VIDEO_BUFFERS frames, 128-byte aligned), the decoder's own
// area (~26 frames, 1 MB aligned) and a fixed 1 MB block (1 MB aligned).
#define VITA_MAX_PLAYER_BLOCKS 8
struct vitaAvBlockNeed_t {
    uint32_t size;
    uint32_t align;
};
static int VitaAv_PredictBlocks(int w, int h, vitaAvBlockNeed_t* out)
{
    if (w <= 0 || h <= 0) {
        w = 512;
        h = 512;
    }
    // Measured (frame = w * h * 1.5): the output buffers are exactly
    // VITA_VIDEO_BUFFERS frames; the decoder's area is NOT proportional to the
    // picture - 4096 KB at 128x128, 5120 KB at 256x256, 10240 KB at 960x272 -
    // which ~3.25 MB + 17 frames, rounded up to 1 MB, matches at all three.
    const uint32_t frame = (uint32_t)(w * h * 3 / 2);
    out[0].size = VITA_ALIGN_UP(VITA_VIDEO_BUFFERS * frame, 128);
    out[0].align = 128;
    out[1].size = VITA_ALIGN_UP(3328 * 1024 + 17 * frame, 1024 * 1024);
    out[1].align = 1024 * 1024;
    out[2].size = 1024 * 1024;
    out[2].align = 1024 * 1024;
    return 3;
}

static volatile int vitaAvActivePlayers = 0;

/*
Frame buffers: one physically contiguous memory block each, GPU-mapped, kept
in a pool and reused (2026-10-17). The 2026-10-13 rounds put every player's
buffers in one shared 16-18 MB block ("the arena"), and no video ever played
again: each player got its first two buffers and then stalled - whatever
margin of free memory was left beside it (loadvideo, the decom screen,
mars_rotation, 5 MB free to spare). The decoder hardware wants every buffer
in a memory block of its own (it maps the block that holds an address); a
second buffer inside the same block fails. The builds before, with a block
per buffer, played. Nothing else takes this memory meanwhile: vitaGL's own
pool is sized once at start-up (PHYCONT_ON_DEMAND is off in this build).

A block given back is kept, mapped, for the next player (unmapping one the
GPU may still sample faults, see above); free blocks idle for a while are
released when a new one does not fit. A pooled block is only reused for a
buffer of exactly its (1 MB rounded) size: the mars city livestream screen
(dropship.mp4) was handed a free 10 MB block for its 5 MB decoder area and
never decoded a frame, and closing that stuck player at the map's end
crashed sceAvPlayer's demux thread; every player given its exact size played.

And only for a buffer of the same kind - asked for with the same alignment
(2026-10-18): the full verification's first PDA video (deployment.mp4,
256x256) was handed loadvideo's free 1 MB block - the 1 MB-aligned block a
player with sound asks for last - for its 128-byte aligned picture buffers,
and never asked for its second buffer. Reused for its own kind (the decoder
area of the menu background, which had been loadvideo's) a block plays.
*/
// keep at least this much free beside the blocks (the old working builds
// left ~3 MB; sceAvPlayer allocates some of its own)
#define VITA_DECODER_MIN_FREE_KB 1024
#define VITA_POOL_MAX_BLOCKS 32
struct vitaAvBlock_t {
    SceUID       uid;
    uint8_t*     base;
    uint32_t     size;    // rounded up to 1 MB
    uint32_t     align;   // the alignment it was first asked for with (its kind of buffer)
    vitaVideo_t* owner;   // NULL: free
    uint32_t     freedMs; // when it was given back
};
static vitaAvBlock_t vitaAvBlocks[VITA_POOL_MAX_BLOCKS];
static int           vitaAvNumBlocks = 0;
static SceUID        vitaAvGpuMutex = -1;

// what a player of a given size took (peak physically contiguous bytes, KB)
#define VITA_MAX_LEARNED 16
struct vitaAvLearned_t {
    int width, height, kb;
    int numBlocks;
    vitaAvBlockNeed_t blocks[VITA_MAX_PLAYER_BLOCKS];
};
static vitaAvLearned_t vitaAvLearned[VITA_MAX_LEARNED];
static int             vitaAvNumLearned = 0;

static uint32_t VitaAv_NowMs()
{
    return (uint32_t)(sceKernelGetProcessTimeWide() / 1000);
}

static void VitaAv_LockPool()
{
    if (vitaAvGpuMutex >= 0) {
        sceKernelLockMutex(vitaAvGpuMutex, 1, NULL);
    }
}

static void VitaAv_UnlockPool()
{
    if (vitaAvGpuMutex >= 0) {
        sceKernelUnlockMutex(vitaAvGpuMutex, 1);
    }
}

enum vitaAvState_t {
    VITA_AV_OPENING, // queued for / inside sceAvPlayerInit + AddSource
    VITA_AV_OPEN,
    VITA_AV_FAILED,
};

struct vitaVideo_t {
    SceAvPlayerHandle handle;
    volatile int      state;          // vitaAvState_t
    bool              sawFirstFrame;  // distinguishes "starting up" from "ended"
    bool              rewinding;      // VitaVideo_Rewind: frames from before the jump still to drop
    uint32_t          rewindMs;       // when it was rewound
    uint64_t          rewindFromTs;   // the timestamp shown when it was rewound
    uint64_t          lastFrameTs;    // the timestamp of the frame on screen
    int               staleDropped;   // frames dropped since the rewind
    bool              finished;
    bool              hijackedTexture;
    bool              looping;
    int               width;
    int               height;
    int               streamWidth;    // from the file's header (memory estimate / learning)
    int               streamHeight;
    int               lengthMs;
    unsigned int      texnum;
    uint32_t          openMs;         // when it was asked for (startup time-out)
    volatile int      phyBytes;       // physically contiguous bytes it holds now
    volatile int      phyPeak;
    int               numReq;         // its frame buffer requests, in order (learned at the first frame)
    vitaAvBlockNeed_t req[VITA_MAX_PLAYER_BLOCKS];
    bool              learned;
    bool              refused;        // a frame buffer did not fit (the player is unusable)
    bool              loggedFail;
    int               sysFreeAtOpenKB; // physically contiguous memory free at open
    SceUID            audioThread;
    volatile int      audioRunning;
    volatile int      audioExited;
    volatile int      countReleased;  // taken off vitaAvActivePlayers (once)
    volatile int      openReturned;   // the worker came back from sceAvPlayerInit / AddSource
    int               framesShown;
    int               cpuAllocs;      // sceAvPlayer's own allocations (allocate), for the stall reports
    int               cpuKB;
    char              path[256];
};

// a player's place among the active ones, given back once (the worker's
// close, or the watchdog for a close that never came back)
static void VitaAv_ReleaseCount(vitaVideo_t* v);

/*
==============
Memory replacement callbacks

sceAvPlayer splits its allocations in two. Everything only the CPU and the
decoder firmware touch goes through allocate/deallocate and can live in the
ordinary heap (which vita_mem.cpp routes into vitaGL's pools). Frame buffers
go through allocateTexture/deallocateTexture and must be physically
contiguous and GPU-mapped, because the video decoder is separate hardware
writing directly into them.

Nothing else in the game uses physically contiguous memory: at peak, with a
map loaded, that pool measured 26,623 KB free, because vitaGL's allocation
cascade tries newlib and then the RAM pool and never gets that far. SDL is
built to leave a slice of it unclaimed for exactly this.
==============
*/
// Diagnostics are capped so a decoder that allocates per frame cannot turn
// console.txt into the bottleneck.
static int vitaAvLogBudget = 64;

static void* VitaAv_AllocCpu(void* arg, uint32_t alignment, uint32_t size)
{
    // memalign returns NULL for an alignment that is zero or not a power of
    // two, and this port routes it through vglMemalign, which is not the plain
    // newlib one the vitaGL sample was written against. Normalise rather than
    // trust the caller.
    uint32_t align = alignment;
    if (align < sizeof(void*) || (align & (align - 1)) != 0) {
        align = sizeof(void*);
    }

    void* ret = memalign(align, size);
    if (ret == NULL) {
        Vita_Checkpoint("avplayer: cpu alloc %u KB align %u FAILED", (unsigned int)(size / 1024), (unsigned int)alignment);
    } else if (arg != NULL) {
        vitaVideo_t* v = (vitaVideo_t*)arg;
        v->cpuAllocs++;
        v->cpuKB += (int)((size + 1023) / 1024);
    }
    return ret;
}

static void VitaAv_FreeCpu(void* arg, void* ptr)
{
    free(ptr);
}

static int VitaAv_FreePhycontKB()
{
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0) {
        return 0;
    }
    return info.size_phycont / 1024;
}

// (the arena is gone: blocks are allocated per buffer - see above)
static bool VitaAv_ArenaInit()
{
    return true;
}

// a pooled block's release: only when free and given back over a second ago
// (no frame of it can still be on the GPU)
static void VitaAv_ReleaseBlockLocked(int i)
{
    vitaAvBlock_t& b = vitaAvBlocks[i];
    sceGxmUnmapMemory(b.base);
    sceKernelFreeMemBlock(b.uid);
    Vita_Checkpoint("avplayer: pooled block %u KB released (idle %u ms)", (unsigned int)(b.size / 1024), (unsigned int)(VitaAv_NowMs() - b.freedMs));
    vitaAvBlocks[i] = vitaAvBlocks[--vitaAvNumBlocks];
}

static int VitaAv_ReleaseIdleLocked(uint32_t minIdleMs)
{
    int released = 0;
    const uint32_t now = VitaAv_NowMs();
    for (int i = vitaAvNumBlocks - 1; i >= 0; i--) {
        if (vitaAvBlocks[i].owner == NULL && now - vitaAvBlocks[i].freedMs >= minIdleMs) {
            VitaAv_ReleaseBlockLocked(i);
            released++;
        }
    }
    return released;
}

/*
A pooled block given back is released (unmapped) only once it has been free
for VITA_BLOCK_SETTLE_MS: the last frame of the player that had it may still
be on the GPU for a frame or two after the texture was detached (unmapping
memory a queued draw reads faults). It was a second; with one player at a time
the menu background and the video taking its place swapped players every few
seconds, every block stayed "fresh", and a 512x512 video was let open and then
refused its 11 MB decoder area - a stuck player, abandoned, its memory lost;
two of those and no video fitted any more (2026-10-18, the third verification
run). Now every free pooled block counts towards a fit, and a frame buffer that
needs a block freed a moment ago waits for it to settle (the decoder's own
thread waits, not the game) instead of being refused.
*/
#define VITA_BLOCK_SETTLE_MS 250

// would these blocks fit - free pooled blocks first, then new blocks from
// the free memory, keeping VITA_DECODER_MIN_FREE_KB?
static bool VitaAv_TrialFit(const vitaAvBlockNeed_t* blocks, int n)
{
    bool taken[VITA_POOL_MAX_BLOCKS];
    memset(taken, 0, sizeof(taken));
    VitaAv_LockPool();
    int sysKB = VitaAv_FreePhycontKB();
    // every free pooled block can be released for it (one player at a time:
    // a block given back a moment ago is waited for, VitaAv_AllocGpu)
    int idleKB = 0;
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        if (vitaAvBlocks[b].owner == NULL) {
            idleKB += (int)(vitaAvBlocks[b].size / 1024);
        }
    }
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        int best = -1;
        for (int b = 0; b < vitaAvNumBlocks; b++) {
            if (!taken[b] && vitaAvBlocks[b].owner == NULL && vitaAvBlocks[b].size == VITA_ALIGN_UP(blocks[i].size, VITA_PHYCONT_ALIGN)
                && vitaAvBlocks[b].align == blocks[i].align) {
                best = b;
                break;
            }
        }
        if (best >= 0) {
            taken[best] = true;
            idleKB -= (int)(vitaAvBlocks[best].size / 1024);
            continue;
        }
        const int kb = (int)(VITA_ALIGN_UP(blocks[i].size, VITA_PHYCONT_ALIGN) / 1024);
        if (sysKB - kb < VITA_DECODER_MIN_FREE_KB) {
            // the free pooled blocks not used here would be released for it
            if (sysKB + idleKB - kb < VITA_DECODER_MIN_FREE_KB) {
                ok = false;
            }
        }
        sysKB -= kb;
    }
    VitaAv_UnlockPool();
    return ok;
}

// free pooled memory plus what can be allocated (KB), for the logs
static int VitaAv_ArenaFreeKB()
{
    int kb = 0;
    VitaAv_LockPool();
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        if (vitaAvBlocks[b].owner == NULL) {
            kb += (int)(vitaAvBlocks[b].size / 1024);
        }
    }
    VitaAv_UnlockPool();
    return kb;
}

static void* VitaAv_AllocGpu(void* arg, uint32_t alignment, uint32_t size)
{
    vitaVideo_t* v = (vitaVideo_t*)arg;
    if (v != NULL && v->numReq < VITA_MAX_PLAYER_BLOCKS) {
        v->req[v->numReq].size = size;
        v->req[v->numReq].align = alignment;
        v->numReq++;
    }
    VitaAv_LockPool();
    // a free pooled block big enough (the smallest); blocks start on 1 MB
    int best = -1;
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        if (vitaAvBlocks[b].owner == NULL && vitaAvBlocks[b].size == VITA_ALIGN_UP(size, VITA_PHYCONT_ALIGN)
            && vitaAvBlocks[b].align == alignment && ((uintptr_t)vitaAvBlocks[b].base & (alignment - 1)) == 0) {
            best = b;
            break;
        }
    }
    const char* how = "reused from the pool";
    if (best < 0 && vitaAvNumBlocks < VITA_POOL_MAX_BLOCKS) {
        const uint32_t rounded = VITA_ALIGN_UP(size, VITA_PHYCONT_ALIGN);
        SceUID uid = sceKernelAllocMemBlock("d3bfg_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, rounded, NULL);
        // no room: the free pooled blocks go back to the system - waiting
        // for those given back a moment ago to settle (see above)
        for (int tries = 0; uid < 0 && tries < 40; tries++) {
            if (VitaAv_ReleaseIdleLocked(VITA_BLOCK_SETTLE_MS) > 0) {
                uid = sceKernelAllocMemBlock("d3bfg_video", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, rounded, NULL);
                continue;
            }
            bool young = false;
            const uint32_t now = VitaAv_NowMs();
            for (int b = 0; b < vitaAvNumBlocks && !young; b++) {
                young = (vitaAvBlocks[b].owner == NULL && now - vitaAvBlocks[b].freedMs < VITA_BLOCK_SETTLE_MS);
            }
            if (!young) {
                break;
            }
            VitaAv_UnlockPool();
            sceKernelDelayThread(10 * 1000);
            VitaAv_LockPool();
        }
        void* base = NULL;
        if (uid >= 0 && sceKernelGetMemBlockBase(uid, &base) >= 0 && base != NULL && ((uintptr_t)base & (alignment - 1)) == 0) {
            sceGxmMapMemory(base, rounded, SCE_GXM_MEMORY_ATTRIB_RW);
            vitaAvBlock_t& nb = vitaAvBlocks[vitaAvNumBlocks];
            nb.uid = uid;
            nb.base = (uint8_t*)base;
            nb.size = rounded;
            nb.align = alignment;
            nb.owner = NULL;
            nb.freedMs = 0;
            best = vitaAvNumBlocks++;
            how = "new block";
        } else if (uid >= 0) {
            sceKernelFreeMemBlock(uid);
            uid = -1;
        }
    }
    if (best < 0) {
        VitaAv_UnlockPool();
        Vita_Checkpoint("avplayer: FRAME BUFFER REFUSED - %u KB align %u: no block (%d KB free, %d pooled blocks) for %s",
            (unsigned int)(size / 1024), (unsigned int)alignment, VitaAv_FreePhycontKB(), vitaAvNumBlocks, v != NULL ? v->path : "?");
        if (v != NULL) {
            v->refused = true;
        }
        return NULL;
    }
    vitaAvBlocks[best].owner = (v != NULL) ? v : (vitaVideo_t*)1;
    if (v != NULL) {
        v->phyBytes += (int)vitaAvBlocks[best].size;
        v->phyPeak = (v->phyBytes > v->phyPeak) ? v->phyBytes : v->phyPeak;
    }
    void* addr = vitaAvBlocks[best].base;
    const uint32_t blockKB = vitaAvBlocks[best].size / 1024;
    VitaAv_UnlockPool();
    if (vitaAvLogBudget > 0) {
        vitaAvLogBudget--;
        Vita_Checkpoint("avplayer: frame buffer #%d %u KB align %u -> %p (%s, %u KB; %d KB free beside) (%s)", v != NULL ? v->numReq : 0,
            (unsigned int)(size / 1024), (unsigned int)alignment, addr, how, blockKB, VitaAv_FreePhycontKB(), v != NULL ? v->path : "?");
    }
    return addr;
}

static void VitaAv_FreeGpu(void* arg, void* addr)
{
    // Runs on a sceAvPlayer thread: the block goes back to the pool, mapped
    vitaVideo_t* v = (vitaVideo_t*)arg;
    if (addr == NULL) {
        return;
    }
    VitaAv_LockPool();
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        if (vitaAvBlocks[b].base == (uint8_t*)addr && vitaAvBlocks[b].owner != NULL) {
            if (v != NULL) {
                v->phyBytes -= (int)vitaAvBlocks[b].size;
            }
            vitaAvBlocks[b].owner = NULL;
            vitaAvBlocks[b].freedMs = VitaAv_NowMs();
            break;
        }
    }
    VitaAv_UnlockPool();
}

// a closed player's blocks it did not give back: free again (returns how many)
static int VitaAv_ReclaimLocked(vitaVideo_t* v, int& kb)
{
    int n = 0;
    kb = 0;
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        if (vitaAvBlocks[b].owner == v) {
            vitaAvBlocks[b].owner = NULL;
            vitaAvBlocks[b].freedMs = VitaAv_NowMs();
            kb += (int)(vitaAvBlocks[b].size / 1024);
            n++;
        }
    }
    return n;
}

static int VitaAv_LearnedKB(int w, int h)
{
    for (int i = 0; i < vitaAvNumLearned; i++) {
        if (vitaAvLearned[i].width == w && vitaAvLearned[i].height == h) {
            return vitaAvLearned[i].kb;
        }
    }
    return 0;
}

static void VitaAv_Learn(vitaVideo_t* v)
{
    if (v->learned || v->streamWidth <= 0 || v->phyPeak <= 0) {
        return;
    }
    v->learned = true;
    const int kb = v->phyPeak / 1024;
    char blocks[256];
    blocks[0] = 0;
    for (int b = 0; b < v->numReq; b++) {
        const size_t used = strlen(blocks);
        idStr::snPrintf(blocks + used, (int)(sizeof(blocks) - used), "%s%u KB/%u", b ? ", " : "", (unsigned int)(v->req[b].size / 1024), (unsigned int)v->req[b].align);
    }
    for (int i = 0; i < vitaAvNumLearned; i++) {
        if (vitaAvLearned[i].width == v->streamWidth && vitaAvLearned[i].height == v->streamHeight) {
            if (kb > vitaAvLearned[i].kb) {
                vitaAvLearned[i].kb = kb;
                vitaAvLearned[i].numBlocks = v->numReq;
                memcpy(vitaAvLearned[i].blocks, v->req, sizeof(v->req[0]) * v->numReq);
            }
            return;
        }
    }
    if (vitaAvNumLearned < VITA_MAX_LEARNED) {
        vitaAvLearned_t& l = vitaAvLearned[vitaAvNumLearned];
        l.width = v->streamWidth;
        l.height = v->streamHeight;
        l.kb = kb;
        l.numBlocks = v->numReq;
        memcpy(l.blocks, v->req, sizeof(v->req[0]) * v->numReq);
        vitaAvNumLearned++;
    }
    Vita_Checkpoint("avplayer: a %dx%d player takes %d KB of physically contiguous memory in %d frame buffers (%s; size/alignment) - %s",
        v->streamWidth, v->streamHeight, kb, v->numReq, blocks, v->path);
}

/*
==============
VitaAv_AudioThread

sceAvPlayerGetAudioData hands back already-decoded PCM, so this only has to
move it to an output port. sceAudioOutOutput blocks until the previous buffer
has been consumed, which is what paces the thread - there is no sleeping in
the normal path.

The port is opened lazily from the first frame's format rather than assumed,
so a video with no audio track never opens one at all.
==============
*/
static int VitaAv_AudioThread(SceSize argSize, void* argp)
{
    vitaVideo_t* v = *(vitaVideo_t**)argp;
    int port = -1;
    int portChannels = 0;
    int portRate = 0;

    while (v->audioRunning) {
        SceAvPlayerFrameInfo frame;

        if (!sceAvPlayerIsActive(v->handle) || !sceAvPlayerGetAudioData(v->handle, &frame)) {
            sceKernelDelayThread(1000);
            continue;
        }

        const int channels = (frame.details.audio.channelCount == 1) ? 1 : 2;
        const SceAudioOutMode mode = (channels == 1) ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
        const int rate = (int)frame.details.audio.sampleRate;

        if (port < 0) {
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, 1024, rate, mode);
            if (port < 0) {
                Vita_Checkpoint("avplayer: sceAudioOutOpenPort failed (0x%08X), video plays silent",
                    (unsigned int)port);
                break;
            }
            portChannels = channels;
            portRate = rate;
        } else if (channels != portChannels || rate != portRate) {
            sceAudioOutSetConfig(port, 1024, rate, mode);
            portChannels = channels;
            portRate = rate;
        }

        sceAudioOutOutput(port, frame.pData);
    }

    if (port >= 0) {
        sceAudioOutReleasePort(port);
    }

    v->audioExited = 1;
    return sceKernelExitDeleteThread(0);
}

/*
==============
The worker: opens and closes players, in order, off the render thread
==============
*/
enum { VITA_JOB_OPEN, VITA_JOB_CLOSE };
struct vitaAvJob_t {
    int          kind;
    vitaVideo_t* v;
};
#define VITA_MAX_JOBS 32
static vitaAvJob_t vitaAvJobs[VITA_MAX_JOBS];
static volatile unsigned int vitaAvJobHead = 0, vitaAvJobTail = 0;
static SceUID vitaAvJobSema = -1;
static SceUID vitaAvJobMutex = -1;

static void VitaAv_WorkerOpen(vitaVideo_t* v)
{
    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.objectPointer = v;
    init.memoryReplacement.allocate = VitaAv_AllocCpu;
    init.memoryReplacement.deallocate = VitaAv_FreeCpu;
    init.memoryReplacement.allocateTexture = VitaAv_AllocGpu;
    init.memoryReplacement.deallocateTexture = VitaAv_FreeGpu;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = VITA_VIDEO_BUFFERS;
    init.autoStart = SCE_TRUE;
    init.debugLevel = 0;

    const uint32_t t0 = VitaAv_NowMs();
    v->handle = sceAvPlayerInit(&init);
    if (VitaAv_HandleIsError(v->handle)) {
        Vita_Checkpoint("avplayer: sceAvPlayerInit failed (0x%08X) for %s", (unsigned int)v->handle, v->path);
        v->handle = 0;
        __sync_synchronize();
        v->state = VITA_AV_FAILED;
        v->openReturned = 1;
        return;
    }

    const int addErr = sceAvPlayerAddSource(v->handle, v->path);
    if (addErr < 0) {
        Vita_Checkpoint("avplayer: sceAvPlayerAddSource(%s) -> 0x%08X", v->path, (unsigned int)addErr);
        sceAvPlayerClose(v->handle);
        v->handle = 0;
        __sync_synchronize();
        v->state = VITA_AV_FAILED;
        v->openReturned = 1;
        return;
    }

    sceAvPlayerSetLooping(v->handle, v->looping ? SCE_TRUE : SCE_FALSE);

    v->audioRunning = 1;
    v->audioThread = sceKernelCreateThread("d3bfg_video_audio", VitaAv_AudioThread,
        0x10000100 - 10, 0x4000, 0, 0, NULL);
    if (v->audioThread >= 0) {
        vitaVideo_t* arg = v;
        sceKernelStartThread(v->audioThread, sizeof(arg), &arg);
    } else {
        v->audioRunning = 0;
    }

    __sync_synchronize(); // everything above is visible before the state says so
    v->state = VITA_AV_OPEN;
    v->openReturned = 1;
    Vita_Checkpoint("avplayer: open %s (%s, %u ms to open; %d players; pool %d KB free; %d KB free beside it before the open, %d KB now)", v->path, v->looping ? "looping" : "once",
        (unsigned int)(VitaAv_NowMs() - t0), (int)vitaAvActivePlayers, VitaAv_ArenaFreeKB(), v->sysFreeAtOpenKB, VitaAv_FreePhycontKB());
}

static void VitaAv_ReleaseCount(vitaVideo_t* v)
{
    if (__sync_bool_compare_and_swap(&v->countReleased, 0, 1)) {
        __sync_fetch_and_sub(&vitaAvActivePlayers, 1);
    }
}

/*
A player closed before its first frame (2026-10-18). The full verification
closed deployment.mp4 four seconds after opening it, while it was still
waiting to ask for its second frame buffer: the worker went into
sceAvPlayerStop / sceAvPlayerClose and never came back, and every video for
the rest of the session (85 minutes: PDA discs, wall screens) stayed black.
An earlier round saw the same close crash sceAvPlayer's demux thread.

So a player that has not given a frame yet is not closed on the worker: a
thread of its own waits for it to get going - until 8 s after it was opened,
asking it for a frame - and closes it once it has (an ordinary close). One
that never does is left alone instead: no Stop, no Close, its frame buffers
stay its own (the decoder may still write into them), and its place among
the players is given back. That leaks its memory, logged, rather than the
whole video system.
*/
#define VITA_LATE_CLOSE_MS 8000
// when the last player finished closing: the next opens 300 ms later at the
// soonest (both players that stalled alone in the fourth verification run
// were opened 135-190 ms after a close - a guess at a cause, a cheap one)
static volatile uint32_t vitaAvLastCloseMs = 0;
#define VITA_REOPEN_GAP_MS 300
static void VitaAv_WorkerCloseStarted(vitaVideo_t* v);

static int VitaAv_LateCloseThread(SceSize, void* argp)
{
    vitaVideo_t* v = *(vitaVideo_t**)argp;
    const uint32_t t0 = VitaAv_NowMs();
    bool started = false;
    while (VitaAv_NowMs() - v->openMs < VITA_LATE_CLOSE_MS) {
        if (v->openReturned && v->state == VITA_AV_OPEN && sceAvPlayerIsActive(v->handle)) {
            SceAvPlayerFrameInfo frame;
            if (sceAvPlayerGetVideoData(v->handle, &frame)) {
                started = true;
                break;
            }
        } else if (v->openReturned && v->state == VITA_AV_FAILED) {
            break;
        }
        sceKernelDelayThread(10000);
    }
    if (started) {
        Vita_Checkpoint("avplayer: %s gave its first frame %u ms after it was asked to close (%u ms after its open) - closing it now", v->path,
            (unsigned int)(VitaAv_NowMs() - t0), (unsigned int)(VitaAv_NowMs() - v->openMs));
        v->sawFirstFrame = true;
        VitaAv_WorkerCloseStarted(v);
    } else if (v->openReturned && v->state == VITA_AV_FAILED) {
        // the open itself failed: nothing to stop
        VitaAv_ReleaseCount(v);
        Vita_Checkpoint("avplayer: closed %s (its open had failed)", v->path);
    } else {
        // left alone: stop its sound thread only
        if (v->audioRunning) {
            v->audioRunning = 0;
            for (int i = 0; i < 200 && !v->audioExited; i++) {
                sceKernelDelayThread(5000);
            }
        }
        VitaAv_ReleaseCount(v);
        Vita_Checkpoint("avplayer: %s ABANDONED - no frame %u ms after its open (%s, %d frame buffer(s) asked for%s, %d KB of them; its own allocations %d, %d KB); not stopped or closed (that hangs "
                        "sceAvPlayer), its memory stays taken; %d players left, %d KB free beside the pool",
            v->path, (unsigned int)(VitaAv_NowMs() - v->openMs), !v->openReturned ? "still inside sceAvPlayerInit" : "opened", v->numReq,
            v->refused ? ", one refused" : "", v->phyBytes / 1024, v->cpuAllocs, v->cpuKB, (int)vitaAvActivePlayers, VitaAv_FreePhycontKB());
    }
    return sceKernelExitDeleteThread(0);
}

static void VitaAv_WorkerClose(vitaVideo_t* v)
{
    if (!v->sawFirstFrame) {
        vitaVideo_t* arg = v;
        const SceUID t = sceKernelCreateThread("d3bfg_video_late_close", VitaAv_LateCloseThread, 0x10000100 + 10, 0x4000, 0,
            Vita_Core3Available() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0, NULL);
        if (t >= 0 && sceKernelStartThread(t, sizeof(arg), &arg) >= 0) {
            Vita_Checkpoint("avplayer: %s asked to close before its first frame (%u ms after its open, %d frame buffer(s) so far) - waiting for it to get going first",
                v->path, (unsigned int)(VitaAv_NowMs() - v->openMs), v->numReq);
            return;
        }
        Vita_Checkpoint("avplayer: no thread to wait for %s to get going (0x%08X) - closing it as it is", v->path, (unsigned int)t);
    }
    VitaAv_WorkerCloseStarted(v);
}

static void VitaAv_WorkerCloseStarted(vitaVideo_t* v)
{
    const uint32_t t0 = VitaAv_NowMs();
    // Stop the player before joining the audio thread. Stopping first makes
    // sceAvPlayerGetAudioData return false immediately, so the thread cannot
    // still be inside it when the player is closed; the only other thing it
    // blocks on is sceAudioOutOutput, which returns once the hardware has
    // consumed the buffer it was handed - about 21 ms at worst.
    if (v->handle != 0) {
        sceAvPlayerStop(v->handle);
    }
    if (v->audioRunning) {
        // Not sceKernelWaitThreadEnd: the thread ends with
        // sceKernelExitDeleteThread, so by the time the wait starts the UID can
        // already be gone. Poll a flag the thread sets on its way out instead,
        // and give up rather than block if it never does.
        v->audioRunning = 0;
        for (int i = 0; i < 200 && !v->audioExited; i++) {
            sceKernelDelayThread(5000);
        }
        if (!v->audioExited) {
            Vita_Checkpoint("avplayer: close: audio thread TIMED OUT (%s)", v->path);
        }
    }
    if (v->handle != 0) {
        sceAvPlayerClose(v->handle);
    }
    // A player that never got going (no frame, a refused buffer) did not hand
    // all of its frame buffers back through deallocateTexture: loadvideo's
    // 10 MB decoder area stayed taken after its close, and every video after
    // it found the arena full. The player is gone, so whatever it still owns
    // is reclaimed here.
    int reclaimedKB = 0;
    VitaAv_LockPool();
    const int reclaimed = VitaAv_ReclaimLocked(v, reclaimedKB);
    VitaAv_UnlockPool();
    vitaAvLastCloseMs = VitaAv_NowMs() | 1;
    VitaAv_ReleaseCount(v);
    Vita_Checkpoint("avplayer: closed %s (%u ms, in the background; %d players left; %s%s; pool %d KB free, %d KB free beside it)", v->path,
        (unsigned int)(VitaAv_NowMs() - t0), (int)vitaAvActivePlayers, v->sawFirstFrame ? "it played" : "it never gave a frame",
        v->refused ? ", a frame buffer was refused" : "", VitaAv_ArenaFreeKB(), VitaAv_FreePhycontKB());
    if (reclaimed > 0) {
        Vita_Checkpoint("avplayer: %d frame buffer(s), %d KB, not given back by the player on close - reclaimed (%s)", reclaimed, reclaimedKB, v->path);
    }
    // a player that never decoded may still have threads winding down that
    // call back with this record: it is kept (a few hundred bytes) rather
    // than freed under them
    if (v->sawFirstFrame) {
        free(v);
    }
}

/*
The worker's watchdog (2026-10-18): a job inside sceAvPlayer that has not
come back in 5 s (an open takes ~5 ms, a close ~70 ms) leaves that worker to
it - a new one takes the queue, and the old one ends if it ever comes back -
so one wedged player cannot keep every later video black. Checked from the
render thread whenever a video is opened or closed.
*/
static volatile int      vitaAvWorkerGen = 0;     // the current worker's generation
static volatile uint32_t vitaAvBusySinceMs = 0;   // the current worker's job: since when (0: idle)
static volatile int      vitaAvBusyKind = 0;
static vitaVideo_t* volatile vitaAvBusyVideo = NULL;
static int               vitaAvWorkersReplaced = 0;
#define VITA_WORKER_STUCK_MS 5000

static bool VitaAv_SpawnWorker();

static int VitaAv_Worker(SceSize, void* argp)
{
    const int gen = *(int*)argp;
    for (;;) {
        sceKernelWaitSema(vitaAvJobSema, 1, NULL);
        sceKernelLockMutex(vitaAvJobMutex, 1, NULL);
        if (gen != vitaAvWorkerGen) {
            // replaced while it was stuck: the job is the new worker's
            sceKernelUnlockMutex(vitaAvJobMutex, 1);
            sceKernelSignalSema(vitaAvJobSema, 1);
            break;
        }
        const vitaAvJob_t job = vitaAvJobs[vitaAvJobTail % VITA_MAX_JOBS];
        vitaAvJobTail++;
        vitaAvBusyKind = job.kind;
        vitaAvBusyVideo = job.v;
        __sync_synchronize();
        vitaAvBusySinceMs = VitaAv_NowMs() | 1;
        sceKernelUnlockMutex(vitaAvJobMutex, 1);
        if (job.kind == VITA_JOB_OPEN) {
            VitaAv_WorkerOpen(job.v);
        } else {
            VitaAv_WorkerClose(job.v);
        }
        sceKernelLockMutex(vitaAvJobMutex, 1, NULL);
        const bool stale = (gen != vitaAvWorkerGen);
        if (!stale) {
            vitaAvBusySinceMs = 0;
            vitaAvBusyVideo = NULL;
        }
        sceKernelUnlockMutex(vitaAvJobMutex, 1);
        if (stale) {
            Vita_Checkpoint("avplayer: the replaced video worker (generation %d) came back from its %s of %s - it ends", gen,
                job.kind == VITA_JOB_OPEN ? "open" : "close", job.v != NULL ? job.v->path : "?");
            break;
        }
    }
    return sceKernelExitDeleteThread(0);
}

static bool VitaAv_SpawnWorker()
{
    static int genArg[64];
    const int gen = vitaAvWorkerGen;
    genArg[gen & 63] = gen;
    // core 3 when the game may use it: nothing waits for this thread
    const SceUID t = sceKernelCreateThread("d3bfg_video_worker", VitaAv_Worker, 0x10000100 + 10, 0x4000, 0,
        Vita_Core3Available() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0, NULL);
    if (t < 0) {
        return false;
    }
    int* arg = &genArg[gen & 63];
    sceKernelStartThread(t, sizeof(arg), arg);
    Vita_RegisterThread(t);
    Vita_NameThread(t, "d3bfg_video_worker (video open/close)", Vita_Core3Available() ? 3 : -1);
    return true;
}

static bool VitaAv_StartWorker()
{
    if (vitaAvJobSema >= 0) {
        return true;
    }
    vitaAvJobSema = sceKernelCreateSema("d3bfg_video_jobs", 0, 0, VITA_MAX_JOBS, NULL);
    vitaAvJobMutex = sceKernelCreateMutex("d3bfg_video_jobq", 0, 0, NULL);
    if (vitaAvJobSema < 0 || vitaAvJobMutex < 0) {
        return false;
    }
    return VitaAv_SpawnWorker();
}

// on the render thread: a worker stuck inside sceAvPlayer is replaced
static void VitaAv_CheckWorker()
{
    if (vitaAvJobMutex < 0) {
        return;
    }
    sceKernelLockMutex(vitaAvJobMutex, 1, NULL);
    const uint32_t since = vitaAvBusySinceMs;
    const uint32_t now = VitaAv_NowMs();
    // (signed: the job's start is stamped odd - "| 1" - so it can read 1 ms
    // ahead of now, which unsigned was 4294967295 ms, "stuck")
    if (since == 0 || (int32_t)(now - since) < VITA_WORKER_STUCK_MS) {
        sceKernelUnlockMutex(vitaAvJobMutex, 1);
        return;
    }
    vitaVideo_t* v = vitaAvBusyVideo;
    const int kind = vitaAvBusyKind;
    vitaAvWorkerGen++;
    vitaAvBusySinceMs = 0;
    vitaAvBusyVideo = NULL;
    vitaAvWorkersReplaced++;
    const int pending = (int)(vitaAvJobHead - vitaAvJobTail);
    sceKernelUnlockMutex(vitaAvJobMutex, 1);
    if (v != NULL && kind == VITA_JOB_CLOSE) {
        VitaAv_ReleaseCount(v); // its close never came back: its place is free
    }
    const bool ok = VitaAv_SpawnWorker();
    Vita_Checkpoint("avplayer: VIDEO WORKER STUCK for %u ms in the %s of %s - a new worker (generation %d, %d replaced so far) takes the %d job(s) waiting%s",
        (unsigned int)(now - since), kind == VITA_JOB_OPEN ? "open" : "close", v != NULL ? v->path : "?", (int)vitaAvWorkerGen,
        vitaAvWorkersReplaced, pending, ok ? "" : " - FAILED to start it");
}

static bool VitaAv_Queue(int kind, vitaVideo_t* v)
{
    sceKernelLockMutex(vitaAvJobMutex, 1, NULL);
    if (vitaAvJobHead - vitaAvJobTail >= VITA_MAX_JOBS) {
        sceKernelUnlockMutex(vitaAvJobMutex, 1);
        return false;
    }
    vitaAvJobs[vitaAvJobHead % VITA_MAX_JOBS].kind = kind;
    vitaAvJobs[vitaAvJobHead % VITA_MAX_JOBS].v = v;
    vitaAvJobHead++;
    sceKernelUnlockMutex(vitaAvJobMutex, 1);
    sceKernelSignalSema(vitaAvJobSema, 1);
    return true;
}

int VitaVideo_ActivePlayers()
{
    return vitaAvActivePlayers;
}

/*
==============
VitaVideo_Open
==============
*/
void VitaVideo_ReserveArena()
{
    if (vitaAvGpuMutex < 0) {
        vitaAvGpuMutex = sceKernelCreateMutex("d3bfg_video_pool", 0, 0, NULL);
    }
    VitaAv_ArenaInit();
    Vita_Checkpoint("avplayer: %d KB physically contiguous memory free for video frame buffers (a block each, pooled)", VitaAv_FreePhycontKB());
}

vitaVideo_t* VitaVideo_Open(const char* osPath, bool looping, int width, int height)
{
    static bool moduleLoaded = false;
    if (!moduleLoaded) {
        const int err = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        Vita_Checkpoint("avplayer: sceSysmoduleLoadModule(AVPLAYER) -> 0x%08X", (unsigned int)err);
        if (err < 0) {
            return NULL;
        }
        moduleLoaded = true;
    }

    if (vitaAvGpuMutex < 0) {
        vitaAvGpuMutex = sceKernelCreateMutex("d3bfg_video_pool", 0, 0, NULL);
    }
    if (!VitaAv_StartWorker()) {
        return NULL;
    }
    VitaAv_CheckWorker();

    if (vitaAvActivePlayers >= VITA_MAX_CONCURRENT_PLAYERS) {
        return NULL;
    }
    if (vitaAvLastCloseMs != 0 && (int32_t)(VitaAv_NowMs() - vitaAvLastCloseMs) < VITA_REOPEN_GAP_MS) {
        return NULL; // asked again in a moment
    }

    // the memory it will want must be there before it starts (see above)
    if (!VitaAv_ArenaInit()) {
        return NULL;
    }
    vitaAvBlockNeed_t need[VITA_MAX_PLAYER_BLOCKS];
    int numNeed = 0;
    const int learnedKB = VitaAv_LearnedKB(width, height);
    for (int i = 0; i < vitaAvNumLearned; i++) {
        if (vitaAvLearned[i].width == width && vitaAvLearned[i].height == height && vitaAvLearned[i].numBlocks > 0) {
            numNeed = vitaAvLearned[i].numBlocks;
            memcpy(need, vitaAvLearned[i].blocks, sizeof(need[0]) * numNeed);
        }
    }
    if (numNeed == 0) {
        numNeed = VitaAv_PredictBlocks(width, height, need);
        // (rounded up to whole megabytes already: no extra margin needed)
    }
    int needKB = 0;
    for (int i = 0; i < numNeed; i++) {
        needKB += (int)(need[i].size / 1024);
    }
    const int haveKB = VitaAv_ArenaFreeKB();
    if (!VitaAv_TrialFit(need, numNeed)) {
        static uint32_t lastLog = 0;
        const uint32_t now = VitaAv_NowMs();
        if (now - lastLog > 2000) {
            lastLog = now;
            Vita_Checkpoint("avplayer: no room for %s (%dx%d) yet - needs %d KB%s, %d KB pooled free; %d players running",
                osPath, width, height, needKB, learnedKB > 0 ? " (measured for this size)" : " (estimated from the picture size)", haveKB, (int)vitaAvActivePlayers);
        }
        return NULL;
    }

    // Room is made now: free pooled blocks of sizes this player will not
    // use (idle over a second: no frame of theirs is still on the GPU) go
    // back to the system, so its own exact-size blocks can be allocated.
    // (2026-10-17: a check of the free memory here refused every player once
    // the pool held all of it - the livestream, the PDA, the UAC message, the
    // invasion screens all stayed black - although the pool was idle.)
    {
        VitaAv_LockPool();
        const uint32_t now = VitaAv_NowMs();
        int releasedKB = 0;
        bool kept[VITA_MAX_PLAYER_BLOCKS];
        memset(kept, 0, sizeof(kept));
        for (int b = vitaAvNumBlocks - 1; b >= 0; b--) {
            if (vitaAvBlocks[b].owner != NULL || now - vitaAvBlocks[b].freedMs < VITA_BLOCK_SETTLE_MS) {
                continue;
            }
            bool wanted = false;
            for (int i = 0; i < numNeed && !wanted; i++) {
                if (!kept[i] && VITA_ALIGN_UP(need[i].size, VITA_PHYCONT_ALIGN) == vitaAvBlocks[b].size && need[i].align == vitaAvBlocks[b].align) {
                    kept[i] = true;
                    wanted = true;
                }
            }
            if (!wanted) {
                releasedKB += (int)(vitaAvBlocks[b].size / 1024);
                VitaAv_ReleaseBlockLocked(b);
            }
        }
        VitaAv_UnlockPool();
        if (releasedKB > 0) {
            Vita_Checkpoint("avplayer: %d KB of pooled blocks of other sizes released for %s (%d KB free now)", releasedKB, osPath, VitaAv_FreePhycontKB());
        }
    }
    const int sysFreeKB = VitaAv_FreePhycontKB();

    vitaVideo_t* v = (vitaVideo_t*)calloc(1, sizeof(vitaVideo_t));
    if (v == NULL) {
        return NULL;
    }
    v->sysFreeAtOpenKB = sysFreeKB;
    strncpy(v->path, osPath, sizeof(v->path) - 1);
    v->looping = looping;
    v->streamWidth = width;
    v->streamHeight = height;
    v->openMs = VitaAv_NowMs();
    v->state = VITA_AV_OPENING;

    __sync_fetch_and_add(&vitaAvActivePlayers, 1);
    if (!VitaAv_Queue(VITA_JOB_OPEN, v)) {
        __sync_fetch_and_sub(&vitaAvActivePlayers, 1);
        free(v);
        return NULL;
    }
    return v;
}

/*
==============
VitaVideo_Close

On the GL thread: the texture goes back to memory of its own here; the
player itself is closed by the worker.
==============
*/
void VitaVideo_Close(vitaVideo_t* v)
{
    if (v == NULL) {
        return;
    }

    // The texture is still pointing at a decoder frame buffer. Clear it before
    // the player goes: vitaGL frees whatever a texture points at when the
    // texture is deleted, and this did not come from a vitaGL pool.
    if (v->hijackedTexture) {
        GLint prevUnit = GL_TEXTURE0;
        GLint prevTex = 0;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

        glBindTexture(GL_TEXTURE_2D, v->texnum);
        vglOverloadTexDataPointer(GL_TEXTURE_2D, NULL);
        // Give it a real allocation again, so anything that samples the image
        // between here and its deletion reads valid memory.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
        glActiveTexture((GLenum)prevUnit);
        v->hijackedTexture = false;
    }

    VitaAv_CheckWorker();
    if (!VitaAv_Queue(VITA_JOB_CLOSE, v)) {
        // the queue is full (32 jobs): close here rather than leak a player
        VitaAv_WorkerClose(v);
    }
}

/*
==============
VitaVideo_UpdateFrame

Polls for a newly decoded frame and, if there is one, repoints the GL texture
at it. Returns true once at least one frame has been decoded, which is the
point from which the caller has something worth drawing.

Timing is sceAvPlayer's job, not ours: it decodes against its own clock and
returns false while the frame already on screen is still current, so this is
safe to call once per rendered frame at any frame rate.

The GL binding is saved and restored because the renderer tracks its own
texture state, and leaving a different texture bound behind its back would
desynchronise that.
==============
*/
// frames handed to the screen, all players together
volatile int vita_avFramesShown = 0;

// the verification's video test: the frames of the one video it watches
// (its OS path ends with this), and when the first came
static char              vitaAvWatch[160];
static volatile int      vitaAvWatchFrames = 0;
static volatile uint32_t vitaAvWatchFirstMs = 0;

void VitaVideo_Watch(const char* pathEnd)
{
    vitaAvWatchFrames = 0;
    vitaAvWatchFirstMs = 0;
    strncpy(vitaAvWatch, pathEnd != NULL ? pathEnd : "", sizeof(vitaAvWatch) - 1);
    vitaAvWatch[sizeof(vitaAvWatch) - 1] = 0;
}

int VitaVideo_WatchedFrames(unsigned int* firstFrameMs)
{
    if (firstFrameMs != NULL) {
        *firstFrameMs = vitaAvWatchFirstMs;
    }
    return vitaAvWatchFrames;
}

static bool VitaAv_IsWatched(const vitaVideo_t* v)
{
    const int wl = (int)strlen(vitaAvWatch);
    const int pl = (int)strlen(v->path);
    return wl > 0 && pl >= wl && idStr::Icmp(v->path + pl - wl, vitaAvWatch) == 0;
}

// the video system's state in one line (the verification's reports)
void VitaVideo_Status(char* out, int outLen)
{
    int pooled = 0, pooledFreeKB = 0;
    VitaAv_LockPool();
    for (int b = 0; b < vitaAvNumBlocks; b++) {
        pooled++;
        if (vitaAvBlocks[b].owner == NULL) {
            pooledFreeKB += (int)(vitaAvBlocks[b].size / 1024);
        }
    }
    VitaAv_UnlockPool();
    const uint32_t since = vitaAvBusySinceMs;
    vitaVideo_t* busy = vitaAvBusyVideo;
    idStr::snPrintf(out, outLen, "%d players, %d job(s) queued, worker %s%s%s (generation %d, %d replaced), %d pooled blocks (%d KB free), %d KB free beside",
        (int)vitaAvActivePlayers, (int)(vitaAvJobHead - vitaAvJobTail),
        since == 0 ? "idle" : (vitaAvBusyKind == VITA_JOB_OPEN ? "opening " : "closing "), since == 0 ? "" : (busy != NULL ? busy->path : "?"),
        since == 0 ? "" : va(" for %u ms", (unsigned int)(VitaAv_NowMs() - since)), (int)vitaAvWorkerGen, vitaAvWorkersReplaced, pooled, pooledFreeKB,
        VitaAv_FreePhycontKB());
}

bool VitaVideo_UpdateFrame(vitaVideo_t* v, unsigned int texnum, int* width, int* height)
{
    if (v == NULL || v->state != VITA_AV_OPEN) {
        return false;
    }
    __sync_synchronize(); // the worker's writes, before the state, are visible

    if (sceAvPlayerIsActive(v->handle)) {
        SceAvPlayerFrameInfo frame;

        bool show = sceAvPlayerGetVideoData(v->handle, &frame);
        if (show && v->rewinding) {
            // After sceAvPlayerJumpToTime the player still hands back the
            // frames it had decoded ahead of the old position: shown, they
            // flashed the end of the last play before the start of the new one
            // (the PDA's video discs, stopped and played again). Frames from at
            // or after the timestamp on screen at the rewind are those; the
            // first earlier one is the new start. Until it comes the caller
            // draws black, as when a video starts. 1 s at most.
            const uint32_t waited = VitaAv_NowMs() - v->rewindMs;
            if (frame.timeStamp >= v->rewindFromTs && waited < 1000) {
                v->staleDropped++;
                show = false;
            } else {
                v->rewinding = false;
                Vita_Checkpoint("avplayer: %s rewound - %d frame(s) from before the jump dropped, playing from %u ms after %u ms%s", v->path, v->staleDropped,
                    (unsigned int)frame.timeStamp, (unsigned int)waited, (waited >= 1000) ? " (gave up waiting for an earlier frame)" : "");
            }
        }
        if (show) {
            vita_avFramesShown++;
            v->framesShown++;
            if (VitaAv_IsWatched(v)) {
                if (vitaAvWatchFrames == 0) {
                    vitaAvWatchFirstMs = VitaAv_NowMs() - v->openMs + 1;
                }
                vitaAvWatchFrames++;
            }
            v->lastFrameTs = frame.timeStamp;
            GLint prevUnit = GL_TEXTURE0;
            GLint prevTex = 0;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

            glBindTexture(GL_TEXTURE_2D, texnum);

            if (!v->hijackedTexture) {
                // The image arrived with an ordinary vitaGL allocation behind
                // it that nothing will read again. Release it once, here,
                // rather than leaking it for the length of the video.
                vglFree(vglGetTexDataPointer(GL_TEXTURE_2D));
                v->hijackedTexture = true;
                v->texnum = texnum;
            }

            SceGxmTexture* gxmTex = vglGetGxmTexture(GL_TEXTURE_2D);
            if (gxmTex != NULL) {
                v->width = (int)frame.details.video.width;
                v->height = (int)frame.details.video.height;

                // YVU420P2_CSC1: two planes, luma then interleaved chroma,
                // with the colour conversion applied by the texture unit.
                sceGxmTextureInitLinear(gxmTex, frame.pData,
                    SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1, v->width, v->height, 0);
                sceGxmTextureSetMinFilter(gxmTex, SCE_GXM_TEXTURE_FILTER_LINEAR);
                sceGxmTextureSetMagFilter(gxmTex, SCE_GXM_TEXTURE_FILTER_LINEAR);
                sceGxmTextureSetUAddrMode(gxmTex, SCE_GXM_TEXTURE_ADDR_CLAMP);
                sceGxmTextureSetVAddrMode(gxmTex, SCE_GXM_TEXTURE_ADDR_CLAMP);

                vglOverloadTexDataPointer(GL_TEXTURE_2D, frame.pData);
                if (!v->sawFirstFrame) {
                    v->sawFirstFrame = true;
                    const int sysNow = VitaAv_FreePhycontKB();
                    Vita_Checkpoint("avplayer: first frame of %s after %u ms (%dx%d; the player took %d KB of physically contiguous memory beside its frame buffers: %d KB free before, %d KB now)", v->path,
                        (unsigned int)(VitaAv_NowMs() - v->openMs), v->width, v->height, v->sysFreeAtOpenKB - sysNow, v->sysFreeAtOpenKB, sysNow);
                    // its frame buffers are all allocated by now
                    VitaAv_Learn(v);
                }
            }

            glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
            glActiveTexture((GLenum)prevUnit);
        }
    } else if (v->sawFirstFrame) {
        // sceAvPlayer takes a moment to spin up, so "not active" only means the
        // video ended once it has actually produced something.
        v->finished = true;
    }

    // sceAvPlayerAddSource is asynchronous: at Open() time the container had
    // not been parsed yet and sceAvPlayerStreamCount reported zero, so the
    // duration came back as 0 ms. Ask again once frames are arriving.
    if (v->lengthMs == 0 && v->sawFirstFrame) {
        const int streams = sceAvPlayerStreamCount(v->handle);
        for (int i = 0; i < streams; i++) {
            SceAvPlayerStreamInfo info;
            memset(&info, 0, sizeof(info));
            if (sceAvPlayerGetStreamInfo(v->handle, (uint32_t)i, &info) == 0) {
                if ((int)info.duration > v->lengthMs) {
                    v->lengthMs = (int)info.duration;
                }
            }
        }
    }

    if (width != NULL) {
        *width = v->width;
    }
    if (height != NULL) {
        *height = v->height;
    }

    return v->sawFirstFrame && !v->rewinding;
}

bool VitaVideo_IsPlaying(vitaVideo_t* v)
{
    return (v != NULL) && v->state != VITA_AV_FAILED && !v->finished;
}

bool VitaVideo_Failed(vitaVideo_t* v)
{
    if (v == NULL || v->state == VITA_AV_FAILED) {
        return true;
    }
    // opened (or still opening) and no frame in 6 s: the decoder will not
    // give it one (no free decoder instance, a file it cannot read). 3 s was
    // too short while a level was still loading around it.
    if (!v->sawFirstFrame && VitaAv_NowMs() - v->openMs > 6000) {
        if (!v->loggedFail) {
            v->loggedFail = true;
            Vita_Checkpoint("avplayer: %s gave no frame in 6 s: state %d, %d frame buffers asked for%s, free beside the pool %d KB at open and %d KB now, pool %d KB free; its own allocations: %d (%d KB)",
                v->path, (int)v->state, v->numReq, v->refused ? " (one refused)" : "", v->sysFreeAtOpenKB, VitaAv_FreePhycontKB(), VitaAv_ArenaFreeKB(), v->cpuAllocs, v->cpuKB);
        }
        return true;
    }
    return v->refused;
}

// ms since it was opened while it has given no frame yet, -1 once it has
int VitaVideo_StartupMs(vitaVideo_t* v)
{
    if (v == NULL || v->sawFirstFrame) {
        return -1;
    }
    return (int)(VitaAv_NowMs() - v->openMs);
}

int VitaVideo_LengthMs(vitaVideo_t* v)
{
    return (v != NULL) ? v->lengthMs : 0;
}

void VitaVideo_Rewind(vitaVideo_t* v)
{
    if (v == NULL || v->state != VITA_AV_OPEN) {
        return;
    }

    // Seeking a decoder that is still starting up achieves nothing and risks
    // stalling it, so a rewind only means something after the first frame.
    if (!v->sawFirstFrame) {
        return;
    }

    sceAvPlayerJumpToTime(v->handle, 0);
    v->finished = false;
    // a rewind within the first 300 ms has nothing worth hiding (and its new
    // frames could not be told from the old ones by timestamp)
    if (v->lastFrameTs >= 300) {
        v->rewinding = true;
        v->rewindMs = VitaAv_NowMs();
        v->rewindFromTs = v->lastFrameTs;
        v->staleDropped = 0;
    }
}

#endif // __vita__
