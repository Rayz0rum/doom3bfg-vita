/*
===========================================================================
PS Vita platform layer for Chocolate DOOM-3-BFG.

Provides the Sys_* entry points that neo/sys/linux/linux_main.cpp supplies on
desktop, plus a Vita-appropriate main() that hands off to a thread with a
stack large enough for the id Tech 5 frontend.
===========================================================================
*/
#include "../../idlib/precompiled.h"
#include "../posix/posix_public.h"
#include "../sys_local.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/ctrl.h>

#include <psp2/io/fcntl.h>

#include <unistd.h>
#include <cstring>
#include <cstdarg>

#include <malloc.h>
#include <vitaGL.h>

#include "vita_public.h"

/*
================================================================================
Memory layout

The allocators are wrapped into vitaGL (see neo/sys/vita/vita_mem.cpp), so every
allocation the engine makes walks vglMalloc's cascade:

    newlib heap -> vitaGL RAM mspace -> phycont -> CDLG budget -> VRAM

Only the first tier is reserved here. The rest is claimed by vitaGL when SDL
brings up the GLES context: SDL's VITA_VGL driver calls

    vglInitExtended( 0, 960, 544, 12 MB, msaa )

which hands vitaGL all remaining main RAM bar 12 MB. So this number is not "how
much memory the game gets" - it is how much is held back from vitaGL, and it
only has to cover what is allocated before the window exists (filesystem tables,
resource containers, cvars, the sound backend). Everything after that point
falls through to the much larger vitaGL pools.

Reserving too much here is the dangerous direction: it starves the mspaces that
vitaGL needs for its own GXM allocations, and those cannot fall back anywhere.

But the first tier turned out to matter far more than "what is allocated before
the window exists". vglMemalign() tries VGL_MEM_EXTERNAL - this heap - before
the RAM mspace, so any allocation small enough to fit here lands here, for the
whole run. With 96 MB the heap peaked at 94 MB used during the shell load and
OpenAL then failed with AL_OUT_OF_MEMORY loading map audio, while the vitaGL RAM
pool still had 109 MB free: the wrong pool was full.

128 MB spends the ~33 MB of user RAM that was measurably sitting idle after GL
init (the "budget after GL free user" line), so vitaGL's own pools are
effectively untouched - it still takes everything bar the 12 MB threshold, and
that cap binds at 200 MB either way.

128 MB still was not enough. Loading mars_city2 ended with the arena pinned at
the cap and 4 KB of headroom, and a 2.7 MB aligned request for decoded audio
failed outright - while the vitaGL RAM pool reported 104 MB free and never
moved. The arena itself had ~27 MB free inside it at the time, so the failure is
contiguity, not total bytes: with no headroom newlib cannot extend the arena to
carve a fresh 2.7 MB block, and vglMemalign's spill into the RAM mspace did not
cover it either.

176 MB. vitaGL takes everything bar the 12 MB threshold, so this comes off the
RAM pool, which drops to roughly 151 MB against a measured peak use of ~101 MB.

200 MB (2026-10-03). The split also decides what the CPU can reach quickly:
vitaGL allocates its RAM pool UNCACHED (vglUseCachedMem is off - nothing in
vitaGL flushes the CPU caches for the GPU), and CDRAM is uncached too, so any
engine allocation that spills out of this heap - which is what happened during
every campaign map load - leaves game state, collision and render models in
memory the CPU reads at a fraction of the speed. Sound no longer takes ~300 MB
(it stays compressed, AL_SoundStream.cpp) and vitaGL's temp vertex pool went
from 32 MB to 6 MB, so the RAM pool can give up 24 MB and still hold the
vertex cache (~58 MB), vitaGL's own buffers and the textures that do not fit
in CDRAM.

208 MB (2026-10-17). mars_city2 still filled the 200 MB arena while loading
(30 MB spilled, 9 MB of it live collision and index data) and its fights ran on
uncached memory. The RAM pool's lowest reading was 18 MB, briefly, between two
maps, so it gives up 8 MB, not more; and the wrappers (vita_mem.cpp) now reuse
the arena's free chunks once it is down to its reserve.
================================================================================
*/
extern "C" {
unsigned int _newlib_heap_size_user = 208 * 1024 * 1024;
}

void GLimp_SwapBuffers();

static const char** cmdargv = NULL;
static int cmdargc = 0;

/*
==============
Vita_Log
==============
*/
static volatile SceUID vitaLogFd = -1;

static void Vita_LogWriteFile(const char* text, size_t len)
{
    // One handle for the whole run: opening and closing the file for every
    // line cost milliseconds each, hundreds of times per map load. What
    // sceIoWrite has handed to the kernel survives the game crashing.
    SceUID fd = vitaLogFd;
    if (fd < 0) {
        fd = sceIoOpen(VITA_DATA_PATH "/console.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (fd < 0) {
            return;
        }
        if (!__sync_bool_compare_and_swap(&vitaLogFd, -1, fd)) {
            sceIoClose(fd); // another thread got there first
            fd = vitaLogFd;
        }
    }
    sceIoWrite(fd, text, len);
}

/*
Lines go into a ring buffer and a low-priority thread writes them out every
50 ms: a write to the memory card can block for tens of milliseconds, and the
game threads used to wait on it for every line - classic DOOM's 2-second
stats, the perf lines, anything a job printed - the 60-70 ms frame in every
perf window. Until the writer runs (early boot), and whenever the buffer is
full, lines are written directly; Vita_LogFlush() drains it on the way out
(errors, exit, the watchdog's dumps). A crash loses at most the last 50 ms.
*/
// (1 MB since run 32: the perf block grew with every counter printed, the
// 128 KB ring filled, and the main thread then waited on the writer's memory
// card write - 84-86 ms, the run's two worst frames)
static char vitaLogRing[1024 * 1024];
volatile int vita_logRingFullWrites = 0; // lines written directly: ring full (perf line)
static volatile unsigned int vitaLogHead = 0, vitaLogTail = 0; // running byte counts
// Threads that log take vitaLogRingLock among themselves; the writer never
// does: it writes [tail, head) straight from the ring and then moves tail on.
// The writer runs on core 3, which the OS takes when it wants - stopped
// there, it must not be holding anything a game thread waits for.
static SceKernelLwMutexWork vitaLogRingLock __attribute__((aligned(8)));
static SceKernelLwMutexWork vitaLogDrainLock __attribute__((aligned(8)));
static volatile int vitaLogAsync = 0;

void Vita_LogFlush()
{
    if (!vitaLogAsync) {
        return;
    }
    sceKernelLockLwMutex(&vitaLogDrainLock, 1, NULL); // one drainer at a time keeps the order
    for (;;) {
        const unsigned int head = vitaLogHead;
        __sync_synchronize(); // the bytes before head are in the ring
        const unsigned int tail = vitaLogTail;
        unsigned int n = head - tail;
        if (n == 0) {
            break;
        }
        const unsigned int at = tail % sizeof(vitaLogRing);
        if (n > sizeof(vitaLogRing) - at) {
            n = sizeof(vitaLogRing) - at; // to the end of the ring; the rest next time round
        }
        Vita_LogWriteFile(vitaLogRing + at, n);
        __sync_synchronize(); // written out before the space is handed back
        vitaLogTail = tail + n;
    }
    sceKernelUnlockLwMutex(&vitaLogDrainLock, 1);
}

static int Vita_LogWriterThread(SceSize, void*)
{
    for (;;) {
        sceKernelDelayThread(20 * 1000);
        Vita_LogFlush();
    }
    return 0;
}

static void Vita_StartLogWriter()
{
    if (sceKernelCreateLwMutex(&vitaLogRingLock, "d3bfg_log_ring", 0, 0, NULL) < 0
        || sceKernelCreateLwMutex(&vitaLogDrainLock, "d3bfg_log_drain", 0, 0, NULL) < 0) {
        return;
    }
    // below the game's threads, above the watchdog; on core 3 when the game
    // may use it (nothing waits for the writer - see above)
    const SceUID t = sceKernelCreateThread("d3bfg_log", Vita_LogWriterThread, 0x10000100 + 20, 0x4000, 0,
        Vita_Core3Available() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0, NULL);
    if (t < 0) {
        return;
    }
    vitaLogAsync = 1;
    sceKernelStartThread(t, 0, NULL);
    Vita_RegisterThread(t);
    Vita_NameThread(t, "d3bfg_log (log writer)", Vita_Core3Available() ? 3 : -1);
}

void Vita_Log(const char* text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    const size_t len = strlen(text);
    if (vitaLogAsync) {
        bool queued = false;
        sceKernelLockLwMutex(&vitaLogRingLock, 1, NULL);
        const unsigned int head = vitaLogHead;
        if (head - vitaLogTail + len <= sizeof(vitaLogRing)) {
            const unsigned int at = head % sizeof(vitaLogRing);
            const size_t first = (len < sizeof(vitaLogRing) - at) ? len : sizeof(vitaLogRing) - at;
            memcpy(vitaLogRing + at, text, first);
            memcpy(vitaLogRing, text + first, len - first);
            __sync_synchronize(); // the bytes are in before head says so
            vitaLogHead = head + len;
            queued = true;
        }
        sceKernelUnlockLwMutex(&vitaLogRingLock, 1);
        if (queued) {
            return;
        }
        vita_logRingFullWrites++;
        Vita_LogFlush(); // full: what is queued goes first
    }
    Vita_LogWriteFile(text, len);
}

/*
==============
Vita_Checkpoint
==============
*/
void Vita_Checkpoint(const char* fmt, ...)
{
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    idStr::vsnPrintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    body[sizeof(body) - 1] = '\0';

    char line[1152];
    // Sys_Milliseconds() is not usable this early, and sceKernelGetProcessTimeWide
    // is what it would end up calling anyway.
    idStr::snPrintf(line, sizeof(line), "[%8u ms] %s\n",
        (unsigned int)(sceKernelGetProcessTimeWide() / 1000), body);
    line[sizeof(line) - 1] = '\0';

    Vita_Log(line);
}

/*
==============
Vita_LogMemory
==============
*/
void Vita_LogMemory(const char* tag)
{
    // vgl_mem_get_free_space() returns 0 for VGL_MEM_EXTERNAL - it cannot measure
    // the newlib heap - so ask newlib directly. fordblks is free bytes already in
    // the arena; the heap can still grow past that up to _newlib_heap_size_user.
    struct mallinfo mi = mallinfo();

    // "free" inside the arena is not the number that matters - the arena grows on
    // demand, so what runs out is the distance from the arena to the cap. That is
    // the quantity that hit zero and produced an OpenAL AL_OUT_OF_MEMORY while the
    // vitaGL pools still had 109 MB between them, so report it directly.
    const unsigned int headroomKB = (_newlib_heap_size_user > mi.arena)
        ? (unsigned int)((_newlib_heap_size_user - mi.arena) / 1024)
        : 0;

    // everything still allocatable, across the newlib heap and vitaGL's pools
    const unsigned int freeKB = (unsigned int)((mi.fordblks + (_newlib_heap_size_user > mi.arena ? _newlib_heap_size_user - mi.arena : 0)
                                                   + vglMemFree(VGL_MEM_RAM) + vglMemFree(VGL_MEM_SLOW) + vglMemFree(VGL_MEM_BUDGET) + vglMemFree(VGL_MEM_VRAM))
        / 1024);
    uint32_t circPeak = 0, circOverruns = 0, circSize = 0;
    vglGetCircularPoolStats(&circPeak, &circOverruns, &circSize);
    Vita_Checkpoint("mem %-22s newlib arena %6u used %6u KB headroom %6u KB | RAM %6u KB | phycont %5u KB | CDLG %5u KB | VRAM %6u KB | all free %6u KB | vgl temp pool peak %u/%u KB, %u overruns",
        tag,
        (unsigned int)(mi.arena / 1024),
        (unsigned int)(mi.uordblks / 1024),
        headroomKB,
        (unsigned int)(vglMemFree(VGL_MEM_RAM) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_SLOW) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_BUDGET) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_VRAM) / 1024),
        freeKB, (unsigned int)(circPeak / 1024), (unsigned int)(circSize / 3 / 1024), (unsigned int)circOverruns);
}

/*
==============
Vita_FreeMemoryKB
==============
*/
// newlib's malloc state (mallocr.c): the running mallinfo, whose 'arena' (what
// sbrk has handed out) is kept up to date as the heap grows, and the bins,
// whose entry 2 is the 'top' chunk - the free space at the end of the arena.
extern "C" {
    extern struct mallinfo __malloc_current_mallinfo;
    extern void* __malloc_av_[];
}

// The newlib heap's free space without walking it: never-used heap past the
// arena, plus the top chunk. Free chunks inside the arena are not counted
// (mallinfo() has to visit every one of them, under the malloc lock - with an
// in-game heap ~100 ms, a hitch each time the game checked, every 1.5 s).
// That only errs on the low side.
static unsigned int Vita_NewlibFreeFastKB()
{
    const size_t arena = __malloc_current_mallinfo.arena;
    const size_t untouched = (_newlib_heap_size_user > arena) ? _newlib_heap_size_user - arena : 0;
    const size_t* topChunk = (const size_t*)__malloc_av_[2];
    const size_t topSize = (topChunk != NULL) ? (topChunk[1] & ~(size_t)3) : 0;
    return (unsigned int)((untouched + topSize) / 1024);
}

void Vita_FreeMemoryKB(unsigned int* allFreeKB, unsigned int* cachedFreeKB)
{
    // both parts O(1): vglMemFree reads the pools' running counters (local
    // vitaGL patch; it used to walk them too)
    const unsigned int cached = Vita_NewlibFreeFastKB();
    if (cachedFreeKB != NULL) {
        *cachedFreeKB = cached;
    }
    if (allFreeKB != NULL) {
        *allFreeKB = cached + (unsigned int)((vglMemFree(VGL_MEM_RAM) + vglMemFree(VGL_MEM_SLOW) + vglMemFree(VGL_MEM_BUDGET) + vglMemFree(VGL_MEM_VRAM)) / 1024);
    }
}

/*
==============
Vita_LogMemoryFast

The pools' free space from their running counters only - no heap walk
(mallinfo walks newlib's heap: ~85 ms in-game - run 33's two worst frames
were the "memory drop" log line, which called Vita_LogMemory).
==============
*/
void Vita_LogMemoryFast(const char* tag)
{
    uint32_t circPeak = 0, circOverruns = 0, circSize = 0;
    vglGetCircularPoolStats(&circPeak, &circOverruns, &circSize);
    Vita_Checkpoint("mem %-22s newlib free (fast) %6u KB | RAM %6u KB | phycont %5u KB | CDLG %5u KB | VRAM %6u KB | vgl temp pool peak %u/%u KB, %u overruns",
        tag, Vita_NewlibFreeFastKB(),
        (unsigned int)(vglMemFree(VGL_MEM_RAM) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_SLOW) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_BUDGET) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_VRAM) / 1024),
        (unsigned int)(circPeak / 1024), (unsigned int)(circSize / 3 / 1024), (unsigned int)circOverruns);
}

/*
==============
Vita_LogMemoryAccuracy

The fast free-memory figures next to the exact, heap-walking ones, once, at
a point where a hitch does not matter (the end of a level load).
==============
*/
void Vita_LogMemoryAccuracy(const char* when)
{
    const uint64 t0 = Sys_Microseconds();
    struct mallinfo mi = mallinfo();
    const unsigned int newlibExact = (unsigned int)((mi.fordblks + (_newlib_heap_size_user > mi.arena ? _newlib_heap_size_user - mi.arena : 0)) / 1024);
    const uint64 t1 = Sys_Microseconds();
    const unsigned int vglExact = (unsigned int)((vglMemFreeExact(VGL_MEM_RAM) + vglMemFreeExact(VGL_MEM_SLOW) + vglMemFreeExact(VGL_MEM_BUDGET) + vglMemFreeExact(VGL_MEM_VRAM)) / 1024);
    const uint64 t2 = Sys_Microseconds();
    unsigned int fastAll = 0, fastNewlib = 0;
    Vita_FreeMemoryKB(&fastAll, &fastNewlib);
    const uint64 t3 = Sys_Microseconds();
    common->Printf("vita: free memory %s - fast: newlib %u KB + pools %u KB (%.2f ms) | exact: newlib %u KB (%.1f ms) + pools %u KB (%.1f ms)\n",
        when, fastNewlib, fastAll - fastNewlib, (t3 - t2) / 1000.0, newlibExact, (t1 - t0) / 1000.0, vglExact, (t2 - t1) / 1000.0);
}

/*
==============
Vita_SetGLThread / Vita_CheckGLThread
==============
*/
static SceUID vitaGLThread = -1;

void Vita_SetGLThread()
{
    vitaGLThread = sceKernelGetThreadId();
}

bool Vita_IsGLThread()
{
    return vitaGLThread < 0 || sceKernelGetThreadId() == vitaGLThread;
}

void Vita_CheckGLThread(const char* what, const char* name)
{
    if (vitaGLThread < 0) {
        return;
    }
    const SceUID self = sceKernelGetThreadId();
    if (self == vitaGLThread) {
        return;
    }
    static int reports = 0;
    if (reports >= 12) {
        return;
    }
    reports++;
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    const char* threadName = (sceKernelGetThreadInfo(self, &info) == 0) ? info.name : "?";
    Vita_Checkpoint("GL off its thread: %s '%s' ran on thread '%s' (main thread at \"%s\")",
        what, name ? name : "", threadName, (const char*)vita_mainPhase);
}

/*
==============
Vita_LogMemoryBudget

One-shot dump of what the console actually handed this process, so the pool
sizes are a measured fact rather than an assumption. vitaGL claims every byte
that is free when it initialises (minus a 12 MB threshold SDL leaves for
newlib), so what it ends up with is whatever the newlib heap reservation and
the executable did not already take.
==============
*/
void Vita_LogMemoryBudget(const char* when)
{
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) == 0) {
        Vita_Checkpoint("budget %-10s free user %6u KB | cdram %6u KB | phycont %6u KB",
            when,
            (unsigned int)(info.size_user / 1024),
            (unsigned int)(info.size_cdram / 1024),
            (unsigned int)(info.size_phycont / 1024));
    } else {
        Vita_Checkpoint("budget %-10s sceKernelGetFreeMemorySize failed", when);
    }

    // total and free together, at one instant. vgl_mem_get_free_space() reads
    // the mspace stats, and if a pool's memblock allocation failed the mspace
    // was never created - the stats call then reports whatever happened to be
    // on the stack, which looks like a healthy free figure that never moves.
    // Printing both makes that case obvious instead of misleading.
    Vita_Checkpoint("pools  %-10s RAM %6u/%-6u | phycont %6u/%-6u | CDLG %6u/%-6u | VRAM %6u/%-6u KB total/free",
        when,
        (unsigned int)(vglMemTotal(VGL_MEM_RAM) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_RAM) / 1024),
        (unsigned int)(vglMemTotal(VGL_MEM_SLOW) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_SLOW) / 1024),
        (unsigned int)(vglMemTotal(VGL_MEM_BUDGET) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_BUDGET) / 1024),
        (unsigned int)(vglMemTotal(VGL_MEM_VRAM) / 1024),
        (unsigned int)(vglMemFree(VGL_MEM_VRAM) / 1024));
}

/*
==============
Vita_CheckShaderCompiler
==============
*/
bool Vita_CheckShaderCompiler()
{
    // The two paths vitaShaRK and vitaGL try, in order: shark_init( NULL ) uses
    // DEFAULT_SHACCCG_PATH, then vitaGL retries the ScePiglet location.
    static const char* const paths[] = {
        "ur0:data/libshacccg.suprx",
        "ur0:data/external/libshacccg.suprx"
    };

    for (int i = 0; i < 2; i++) {
        SceUID fd = sceIoOpen(paths[i], SCE_O_RDONLY, 0777);
        if (fd >= 0) {
            SceOff len = sceIoLseek(fd, 0, SCE_SEEK_END);
            sceIoClose(fd);
            Vita_Checkpoint("shader compiler: found %s (%lld bytes)", paths[i], (long long)len);
            return true;
        }
    }

    Vita_Checkpoint("shader compiler: libshacccg.suprx NOT FOUND in ur0:data");
    Vita_Checkpoint("shader compiler: vitaGL compiles every shader at runtime and");
    Vita_Checkpoint("shader compiler: cannot do so without it; the renderer will die.");
    return false;
}

/*
==============
Sys_EXEPath
==============
*/
const char* Sys_EXEPath()
{
    static char path[] = "app0:/eboot.bin";
    return path;
}

/*
==============
Sys_GetProcessorId
==============
*/
cpuid_t Sys_GetProcessorId()
{
    return CPUID_GENERIC;
}

/*
==============
Sys_GetProcessorString
==============
*/
const char* Sys_GetProcessorString()
{
    return "ARM Cortex-A9 MPCore";
}

/*
==============
Sys_ClockTicksPerSecond
==============
*/
double Sys_ClockTicksPerSecond()
{
    // This has to agree with Sys_GetClockTicks(), not with the CPU. There is no
    // rdtsc on ARM, so the POSIX Sys_GetClockTicks() falls through to its
    // clock_gettime( CLOCK_MONOTONIC ) path and returns *nanoseconds*. Reporting
    // the real 444 MHz here would make idTimer::Milliseconds(), which computes
    // clockTicks / ( ClockTicksPerSecond() * 0.001 ), read about 2.25x fast.
    return 1000000000.0;
}

/*
==============
Sys_CPUCount
The Vita exposes 4 Cortex-A9 cores; one is reserved for the OS.
==============
*/
void Sys_CPUCount(int& numLogicalCPUCores, int& numPhysicalCPUCores, int& numCPUPackages)
{
    numPhysicalCPUCores = 3;
    numLogicalCPUCores = 3;
    numCPUPackages = 1;
}

/*
==============
Sys_DoStartProcess
==============
*/
void Sys_DoStartProcess(const char* exeName, bool dofork)
{
    // Not supported on the Vita.
}

void Sys_DoPreferences() { }

/*
==============
Sys_GetCmdLine
==============
*/
const char* Sys_GetCmdLine()
{
    return "";
}

/*
==============
Sys_ReLaunch
==============
*/
void Sys_ReLaunch()
{
    // Restarting in place isn't supported; just shut down cleanly.
    sceKernelExitProcess(0);
}

/*
===============
Watchdog

VITA_MARK() (sys_defines.h) records where the main thread is. This thread
wakes once a second and, if the mark has not moved for 10 s, logs it - then
again at 30 s and every minute after. A hang that used to leave nothing but a
frozen screen then names the step it is stuck in. Long legitimate steps (the
script compile, a big map load) show up too; they are easy to tell apart.

It deliberately calls nothing but Vita_Checkpoint (stack buffers, sceIo): no
vitaGL, no allocator, no engine state that the stuck thread may hold.
===============
*/
volatile const char* vita_mainPhase = "boot";
volatile unsigned int vita_mainPhaseSeq = 0;
volatile const char* vita_gamePhase = "not started";
static volatile unsigned int vitaFramesDone = 0;
static volatile SceUID vitaMainThreadUid = -1;
static volatile SceUID vitaGameThreadUid = -1;

/*
===============
Vita_Core3Available

Games get cores 0-2; the kernel refuses an affinity mask naming core 3
unless a kernel plugin such as CapUnlocker lifts that check. Probe once by
creating (and never starting) a thread restricted to core 3.
===============
*/
static int ProbeThreadEntry(SceSize, void*)
{
    return 0;
}

bool Vita_Core3Available()
{
    static int state = -1;
    if (state < 0) {
        const SceUID t = sceKernelCreateThread("d3bfg_core3_probe", ProbeThreadEntry, 0x10000100, 0x1000, 0, SCE_KERNEL_CPU_MASK_SYSTEM, NULL);
        state = (t >= 0) ? 1 : 0;
        if (t >= 0) {
            sceKernelDeleteThread(t);
        }
        Vita_Checkpoint("main: core 3 %s (probe 0x%08X)", state ? "available to the game" : "reserved by the system", (unsigned int)t);
    }
    return state == 1;
}

extern "C" void enable_runfast();

/*
==============
Per-thread CPU accounting

Every thread of the process, by UID: CPU time used (runClocks, microseconds)
and preemptions, per frame, once per perf line. A job that takes 10 ms of
wall time for a few hundred triangles is either computing or not running;
these say which, and who runs instead. Threads the engine creates register
themselves; the rest (OpenAL's mixer, vitaGL's collector, the system's
dialog and display workers) are found once by trying the UID range after a
level load - a thread UID is only valid in its own process.
==============
*/
struct vitaThreadRec_t {
    SceUID id;
    char name[28];
    int core; // pinned to (from the engine's placement), -1 unknown
    uint64 lastClocks;
    unsigned int lastPreempt;
    unsigned int lastIntr;
    int lastChangeCpu;
};
static vitaThreadRec_t vitaThreadRecs[40];
static int vitaNumThreadRecs = 0;

void Vita_RegisterThread(SceUID id)
{
    for (int i = 0; i < vitaNumThreadRecs; i++) {
        if (vitaThreadRecs[i].id == id) {
            return;
        }
    }
    if (vitaNumThreadRecs >= (int)(sizeof(vitaThreadRecs) / sizeof(vitaThreadRecs[0]))) {
        return;
    }
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(id, &info) < 0) {
        return;
    }
    vitaThreadRec_t& r = vitaThreadRecs[vitaNumThreadRecs++];
    r.id = id;
    idStr::Copynz(r.name, info.name, sizeof(r.name));
    r.core = -1;
    r.lastClocks = info.runClocks;
    r.lastPreempt = info.threadPreemptCount;
    r.lastIntr = info.intrPreemptCount;
    r.lastChangeCpu = info.changeCpuCount;
    Vita_Checkpoint("thread '%s' 0x%08X: priority 0x%X (initial 0x%X), affinity 0x%X, stack %d KB",
        info.name, (unsigned int)id, (unsigned int)info.currentPriority, (unsigned int)info.initPriority,
        (unsigned int)info.currentCpuAffinityMask, info.stackSize / 1024);
}

void Vita_ScanThreads()
{
    const uint64 t0 = Sys_Microseconds();
    SceKernelThreadInfo info;
    for (unsigned int mid = 0; mid < 0x40; mid++) {
        for (unsigned int low = 1; low < 0x1000; low += 2) {
            const SceUID id = (SceUID)(0x40000000u | (mid << 16) | low);
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(id, &info) == 0) {
                Vita_RegisterThread(id);
            }
        }
    }
    Vita_Checkpoint("thread scan: %d threads known (%.0f ms)", vitaNumThreadRecs, (Sys_Microseconds() - t0) / 1000.0);

    // OpenAL Soft's mixer runs at priority 0x40, above every engine thread,
    // with no affinity: wherever the scheduler put it (5-18 ms of CPU a frame)
    // it took that time from whatever ran there - Game/Draw, or a job worker
    // in the middle of a job the front end was waiting for. Core 0 it is,
    // with the render back end (0x41, just below it: mixing is never late,
    // and the back end waits on Game/Draw for much of the frame anyway), and
    // vitaGL's garbage collector with it - it frees what the back end's
    // thread allocates. Cores 1 and 2 are left to Game/Draw and the job
    // workers.
    // With core 3 available (vita_audioCore3, AL_SoundStream.cpp), the mixer
    // goes there instead: nothing in the frame waits for it, and on core 0 it
    // stopped the job worker there in the middle of the front end's jobs.
    extern idCVar vita_audioCore3;
    const bool mixerOnCore3 = vita_audioCore3.GetBool() && Vita_Core3Available();
    for (int i = 0; i < vitaNumThreadRecs; i++) {
        const bool mixer = strncmp(vitaThreadRecs[i].name, "OpenAL", 6) == 0;
        if (mixer || strcmp(vitaThreadRecs[i].name, "Garbage Collector") == 0) {
            const bool core3 = mixer && mixerOnCore3;
            const int r = sceKernelChangeThreadCpuAffinityMask(vitaThreadRecs[i].id, core3 ? SCE_KERNEL_CPU_MASK_SYSTEM : SCE_KERNEL_CPU_MASK_USER_0);
            vitaThreadRecs[i].core = core3 ? 3 : 0;
            Vita_Checkpoint("thread '%s' pinned to core %d%s", vitaThreadRecs[i].name, core3 ? 3 : 0, r < 0 ? " - refused" : "");
        }
    }
}

// the engine's name for a thread (pthreads are all "pthread" to the kernel)
// and the core it was pinned to
/*
===============
Vita_PlaceBackgroundThread

For threads made with a bare Sys_CreateThread (its priority code is compiled
out, and nothing pins them): called first thing on the thread itself. They
were "pthread[default]" - default priority, cores 1-2 - and the owner badge's
RSA signature (~1 s of CPU, no waits) landed on core 1 beside Game/Draw at the
same priority: the Vita does not share a core between equal priorities, so
Game/Draw stood still for the whole signature (run 7, alphalabs4's fight: a
947 ms frame, "main thread waited 921.5 for game/draw", right after "owner
badge signed in 958 ms"). Now: core 3 when the game may use it (only work
nothing waits for runs there), else cores 0 and 2, never Game/Draw's core 1;
and priority 190, below every game thread (the lowest user priority is 191).
===============
*/
void Vita_PlaceBackgroundThread(const char* name)
{
    const SceUID id = sceKernelGetThreadId();
    Vita_RegisterThread(id);
    const bool core3 = Vita_Core3Available();
    sceKernelChangeThreadCpuAffinityMask(id, core3 ? SCE_KERNEL_CPU_MASK_SYSTEM : (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_2));
    sceKernelChangeThreadPriority(id, 190);
    Vita_NameThread(id, name, core3 ? 3 : -1);
}

void Vita_NameThread(int sceUid, const char* name, int core)
{
    for (int i = 0; i < vitaNumThreadRecs; i++) {
        if (vitaThreadRecs[i].id == sceUid) {
            idStr::Copynz(vitaThreadRecs[i].name, name, sizeof(vitaThreadRecs[i].name));
            vitaThreadRecs[i].core = core;
            return;
        }
    }
}

static const char* Vita_MaskName(int mask)
{
    switch (mask) {
    case 0:
        return "default";
    case SCE_KERNEL_CPU_MASK_USER_0:
        return "c0";
    case SCE_KERNEL_CPU_MASK_USER_1:
        return "c1";
    case SCE_KERNEL_CPU_MASK_USER_2:
        return "c2";
    case SCE_KERNEL_CPU_MASK_SYSTEM:
        return "c3";
    case SCE_KERNEL_CPU_MASK_USER_ALL:
        return "c0-2";
    default:
        return "mixed";
    }
}

void Vita_LogThreadCpu(double frames)
{
    if (vitaNumThreadRecs == 0 || frames <= 0.0) {
        return;
    }
    char line[2048];
    int len = idStr::snPrintf(line, sizeof(line), "vita threads [name[allowed cores, last ran on] CPU ms per frame / thread preemptions / interrupt preemptions / core moves per frame]:");
    double coreMs[4] = { 0.0, 0.0, 0.0, 0.0 }, floatingMs = 0.0;
    for (int i = 0; i < vitaNumThreadRecs; i++) {
        vitaThreadRec_t& r = vitaThreadRecs[i];
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(r.id, &info) < 0) {
            continue; // exited
        }
        const double ms = (double)(info.runClocks - r.lastClocks) / 1000.0 / frames;
        const double pre = (double)(info.threadPreemptCount - r.lastPreempt) / frames;
        const double intr = (double)(info.intrPreemptCount - r.lastIntr) / frames;
        const double moves = (double)(info.changeCpuCount - r.lastChangeCpu) / frames;
        r.lastClocks = info.runClocks;
        r.lastPreempt = info.threadPreemptCount;
        r.lastIntr = info.intrPreemptCount;
        r.lastChangeCpu = info.changeCpuCount;
        const int mask = info.currentCpuAffinityMask;
        if (mask == SCE_KERNEL_CPU_MASK_USER_0) {
            coreMs[0] += ms;
        } else if (mask == SCE_KERNEL_CPU_MASK_USER_1) {
            coreMs[1] += ms;
        } else if (mask == SCE_KERNEL_CPU_MASK_USER_2) {
            coreMs[2] += ms;
        } else if (mask == SCE_KERNEL_CPU_MASK_SYSTEM) {
            coreMs[3] += ms;
        } else {
            floatingMs += ms;
        }
        if (ms < 0.05 && pre < 0.05) {
            continue; // idle
        }
        if (len < (int)sizeof(line) - 96) {
            len += idStr::snPrintf(line + len, sizeof(line) - len, " %s[%s,@%d] %.1f/%.1f/%.1f/%.1f |", r.name, Vita_MaskName(mask), info.lastExecutedCpuId, ms, pre, intr, moves);
        }
    }
    common->Printf("%s\n", line);
    common->Printf("vita cores [CPU ms per frame of the threads pinned to each]: core 0 %.1f | core 1 %.1f | core 2 %.1f | core 3 %.1f (only work nothing waits for: log writer, watchdog, saves, sound mixing and decoding) | not pinned (cores 0-2) %.1f\n",
        coreMs[0], coreMs[1], coreMs[2], coreMs[3], floatingMs);
}

/*
==============
Vita_BenchEnvironment

The benchmark's header (framework/VitaBench.cpp): clocks, cores, threads, memory.
==============
*/
void Vita_BenchEnvironment()
{
    common->Printf("bench Vita: CPU %d MHz (500 needs an overclock plugin: path_oc / PSVshell), GPU %d MHz, bus %d MHz, xbar %d MHz | core 3: %s\n",
        scePowerGetArmClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetBusClockFrequency(), scePowerGetGpuXbarClockFrequency(),
        Vita_Core3Available() ? "available to the game (CapUnlocker or similar) - used only for the log writer, the watchdog, savegame threads and (vita_audioCore3) sound mixing and decoding"
                              : "kept by the system");
    char line[2048];
    int len = idStr::snPrintf(line, sizeof(line), "bench Vita threads:");
    for (int i = 0; i < vitaNumThreadRecs; i++) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(vitaThreadRecs[i].id, &info) < 0) {
            continue;
        }
        if (len < (int)sizeof(line) - 80) {
            len += idStr::snPrintf(line + len, sizeof(line) - len, " %s (priority 0x%X, %s) |", vitaThreadRecs[i].name, (unsigned int)info.currentPriority, Vita_MaskName(info.currentCpuAffinityMask));
        }
    }
    common->Printf("%s\n", line);
    Vita_LogMemory("benchmark start");
}

// The benchmark's trigger: L + R at any moment from power-on until the menus
// are up (main(), doom_main and the watchdog thread every 100 ms look - the
// boot blocks for seconds at a time: the logo video, the loading), or held
// for 1.5 s on the title screen / main menu.
static volatile bool vitaBenchmarkAtBoot = false;
static volatile bool vitaMenusReached = false;

static bool Vita_BenchmarkButtonsHeld()
{
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    return sceCtrlPeekBufferPositive(0, &pad, 1) >= 0
        && (pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) == (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
}

static void Vita_CheckBenchmarkButtons()
{
    if (!vitaMenusReached && !vitaBenchmarkAtBoot && Vita_BenchmarkButtonsHeld()) {
        vitaBenchmarkAtBoot = true;
        Vita_Checkpoint("benchmark: L + R held during the boot - it starts once the menus are up");
    }
}

// once a frame on the menus (common_frame.cpp): true once when it should start
bool Vita_BenchmarkTrigger()
{
    static uint64 heldSince = 0;
    vitaMenusReached = true;
    if (vitaBenchmarkAtBoot) {
        vitaBenchmarkAtBoot = false;
        heldSince = 0;
        return true;
    }
    // (no longer also "held for 1.5 s on the menus": L + R pressed after the
    // boot opens the console)
    if (true) {
        heldSince = 0;
        return false;
    }
    const uint64 now = Sys_Microseconds();
    if (heldSince == 0) {
        heldSince = now;
    } else if (now - heldSince >= 1500 * 1000) {
        heldSince = 0;
        return true;
    }
    return false;
}

/*
==============
Timing zones (vita_zones.h)
==============
*/
#include "vita_zones.h"

static vitaZone_t* vitaZones = NULL;

void Vita_ZoneRegister(vitaZone_t* z)
{
    // the first thread to get here links the zone in; the others go on
    // timing into it (its counters are atomic) while that happens
    if (__sync_lock_test_and_set(&z->registered, 1) != 0) {
        return;
    }
    vitaZone_t* head;
    do {
        head = vitaZones;
        z->next = head;
    } while (!__sync_bool_compare_and_swap(&vitaZones, head, z));
}

void Vita_ZoneReport(double frames)
{
    if (frames <= 0.0 || vitaZones == NULL) {
        return;
    }
    // every zone, nothing dropped: the timed ones costliest first (all with
    // 0.1 ms or more in the window), then every counter; a line that fills
    // up is printed and continued on the next ("(cont.)")
    char line[2048];
    int len = idStr::snPrintf(line, sizeof(line), "vita zones [ms / calls per frame]:");
    int timedShown = 0, timedSmall = 0, counted = 0;
    for (;;) {
        vitaZone_t* best = NULL;
        for (vitaZone_t* z = vitaZones; z != NULL; z = z->next) {
            if (z->timed && z->us > 0 && (best == NULL || z->us > best->us)) {
                best = z;
            }
        }
        if (best == NULL) {
            break;
        }
        if (best->us < 100) {
            timedSmall++;
        } else {
            char item[256];
            const int n = idStr::snPrintf(item, sizeof(item), " %s %.2f/%.1f |", best->name, best->us / 1000.0 / frames, best->calls / frames);
            if (len + n >= (int)sizeof(line) - 1) {
                common->Printf("%s" "\n", line);
                len = idStr::snPrintf(line, sizeof(line), "vita zones (cont.):");
            }
            len += idStr::snPrintf(line + len, sizeof(line) - len, "%s", item);
            timedShown++;
        }
        best->us = -best->us; // shown (all are cleared below)
    }
    common->Printf("%s" "\n", line);
    len = idStr::snPrintf(line, sizeof(line), "vita counts [per frame]:");
    for (vitaZone_t* z = vitaZones; z != NULL; z = z->next) {
        if (!z->timed && z->calls > 0) {
            char item[256];
            const int n = idStr::snPrintf(item, sizeof(item), " %s %.1f |", z->name, z->calls / frames);
            if (len + n >= (int)sizeof(line) - 1) {
                common->Printf("%s" "\n", line);
                len = idStr::snPrintf(line, sizeof(line), "vita counts (cont.):");
            }
            len += idStr::snPrintf(line + len, sizeof(line) - len, "%s", item);
            counted++;
        }
        z->us = 0;
        z->calls = 0;
    }
    common->Printf("%s" "\n", line);
    common->Printf("vita zones: %d timed zones shown, %d timed under 0.1 ms in the window not shown, %d counters" "\n", timedShown, timedSmall, counted);
}

void Vita_NoteGameThread()
{
    if (vitaGameThreadUid < 0) {
        vitaGameThreadUid = sceKernelGetThreadId();
    }
}

/*
===============
Vita_MemKind

What kind of memory an address is in, for the logs. The CPU reads uncached
memory (vitaGL's RAM pool, CDRAM) several times slower than cached RAM (the
newlib heap), so a hot loop over data that spilled out of the newlib heap
runs slow for no visible reason. A syscall: diagnostics only.
===============
*/
extern volatile uint64_t vita_tagCachedBytes[VITA_MEM_TAGS]; // vita_mem.cpp
extern volatile uint64_t vita_tagSpilledBytes[VITA_MEM_TAGS];
static const char* vitaMemTagNames[] = {
#define MEM_TAG(x) #x,
#include "../../idlib/sys/sys_alloc_tags.h"
};

void Vita_TagPlacementReset()
{
    for (int i = 0; i < VITA_MEM_TAGS; i++) {
        vita_tagCachedBytes[i] = 0;
        vita_tagSpilledBytes[i] = 0;
    }
}

void Vita_TagPlacementReport(const char* when)
{
    const int numTags = Min((int)(sizeof(vitaMemTagNames) / sizeof(vitaMemTagNames[0])), VITA_MEM_TAGS);
    uint64_t cached = 0, spilled = 0;
    for (int i = 0; i < numTags; i++) {
        cached += vita_tagCachedBytes[i];
        spilled += vita_tagSpilledBytes[i];
    }
    // the tags that spilled most, then the ones that stayed cached
    for (int pass = 0; pass < 2; pass++) {
        volatile uint64_t* bytes = pass == 0 ? vita_tagSpilledBytes : vita_tagCachedBytes;
        bool shown[VITA_MEM_TAGS];
        memset(shown, 0, sizeof(shown));
        char line[2048];
        int len = idStr::snPrintf(line, sizeof(line), pass == 0
                ? "vita memory placement (%s): %u KB allocated into the cached heap, %u KB SPILLED to uncached pools - spilled most, KB uncached / cached:"
                : "vita memory placement (%s): largest in the cached heap, KB cached / uncached:",
            when, (unsigned int)(cached / 1024), (unsigned int)(spilled / 1024));
        for (int n = 0; n < 16; n++) {
            int best = -1;
            for (int i = 0; i < numTags; i++) {
                if (!shown[i] && bytes[i] >= 64 * 1024 && (best < 0 || bytes[i] > bytes[best])) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            shown[best] = true;
            if (len < (int)sizeof(line) - 80) {
                const uint64_t other = pass == 0 ? vita_tagCachedBytes[best] : vita_tagSpilledBytes[best];
                len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %u / %u |", vitaMemTagNames[best],
                    (unsigned int)(bytes[best] / 1024), (unsigned int)(other / 1024));
            }
        }
        common->Printf("%s\n", line);
    }
}

// CPU-cached RAM (the newlib heap, cached memblocks): what the CPU's hot data needs
bool Vita_IsCachedMemory(const void* p)
{
    if (p == NULL) {
        return false;
    }
    SceKernelMemBlockInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr((void*)p, &info) < 0) {
        return false;
    }
    return info.memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL
        && (info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW || info.type == SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW);
}

const char* Vita_MemKind(const void* p)
{
    if (p == NULL) {
        return "null";
    }
    SceKernelMemBlockInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetMemBlockInfoByAddr((void*)p, &info) < 0) {
        return "not a memory block (a thread stack?)";
    }
    const bool cached = (info.memoryType == SCE_KERNEL_MEMORY_TYPE_NORMAL);
    switch ((unsigned int)info.type) {
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW:
        return cached ? "cached RAM" : "RAM, UNCACHED";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE:
        return "UNCACHED RAM";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW:
        return "CDRAM (UNCACHED)";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW:
        return cached ? "phycont, cached" : "phycont, UNCACHED";
    case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW:
        return "phycont, UNCACHED";
    default:
        return cached ? "other, cached" : "other, UNCACHED";
    }
}

/*
===============
Vita_WatchdogDumpThread

A stuck thread cannot be asked where it is, and a core dump needs the user to
fish a file off the card. Instead: its kernel status says whether it is
spinning (RUNNING/READY, run clocks climbing) or blocked (WAITING, and on what
kind of object), and its stack still holds the return addresses of every live
call. Every word that points just past a BL/BLX in this module's code is
logged top-down, as an ELF address (0x81000000 + offset into the code) that
addr2line on build-vita/Doom3BFG turns into a function. The first entries
are the live call chain from the thread's entry point inward; older, stale
frames below the stack pointer follow and are easy to tell apart (they don't
call each other).
===============
*/
static bool Vita_IsReturnAddress(uint32_t value, uint32_t textStart, uint32_t textEnd)
{
    if ((value & 1) == 0) {
        return false; // Thumb code: return addresses carry bit 0
    }
    const uint32_t addr = value & ~1u;
    if (addr < textStart + 4 || addr >= textEnd) {
        return false;
    }
    const uint16_t hw1 = *(const uint16_t*)(addr - 4);
    const uint16_t hw2 = *(const uint16_t*)(addr - 2);
    const bool bl = (hw1 & 0xF800) == 0xF000 && (hw2 & 0xD000) == 0xD000; // BL / BLX imm (32-bit)
    const bool blxReg = (hw2 & 0xFF87) == 0x4780;                          // BLX Rm (16-bit)
    return bl || blxReg;
}

static void Vita_WatchdogDumpThread(const char* label, SceUID uid)
{
    if (uid < 0) {
        Vita_Checkpoint("watchdog: %s thread id not known", label);
        return;
    }
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(uid, &info) < 0) {
        Vita_Checkpoint("watchdog: %s thread: sceKernelGetThreadInfo failed", label);
        return;
    }
    const unsigned long long clocks1 = (unsigned long long)info.runClocks;
    sceKernelDelayThread(200 * 1000);
    SceKernelThreadInfo info2;
    memset(&info2, 0, sizeof(info2));
    info2.size = sizeof(info2);
    sceKernelGetThreadInfo(uid, &info2);
    const unsigned long long clocks2 = (unsigned long long)info2.runClocks;
    // status bits: 1 running, 2 ready, 4 standby, 8 waiting, 16 dormant
    Vita_Checkpoint("watchdog: %s thread '%s' status 0x%X (1 running, 2 ready, 8 waiting) wait type 0x%X on object 0x%08X, "
                    "ran %llu us of the last 200 ms, stack %p + %d KB",
        label, info2.name, (unsigned int)info2.status, (unsigned int)info2.waitType, (unsigned int)info2.waitId,
        clocks2 - clocks1, info2.stack, info2.stackSize / 1024);

    SceKernelModuleInfo mod;
    memset(&mod, 0, sizeof(mod));
    mod.size = sizeof(mod);
    const SceUID modid = sceKernelGetModuleIdByAddr((void*)&Vita_WatchdogDumpThread);
    if (modid < 0 || sceKernelGetModuleInfo(modid, &mod) < 0) {
        Vita_Checkpoint("watchdog: module info unavailable, no stack scan");
        return;
    }
    const uint32_t textStart = (uint32_t)mod.segments[0].vaddr;
    const uint32_t textEnd = textStart + mod.segments[0].memsz;
    const uint32_t* lo = (const uint32_t*)info2.stack;
    const uint32_t* hi = (const uint32_t*)((const char*)info2.stack + info2.stackSize);
    char line[512];
    int len = 0;
    int found = 0;
    for (const uint32_t* p = hi - 1; p >= lo && found < 96; p--) {
        const uint32_t v = *p;
        if (!Vita_IsReturnAddress(v, textStart, textEnd)) {
            continue;
        }
        found++;
        len += idStr::snPrintf(line + len, sizeof(line) - len, " %08X", (unsigned int)(0x81000000u + ((v & ~1u) - textStart)));
        if (len > 400) {
            Vita_Checkpoint("watchdog: %s stack (top down):%s", label, line);
            len = 0;
        }
    }
    if (len > 0) {
        Vita_Checkpoint("watchdog: %s stack (top down):%s", label, line);
    }
    Vita_Checkpoint("watchdog: %s stack scan: %d return addresses (code at %08X, ELF base 0x81000000)", label, found, (unsigned int)textStart);
}

/*
===============
Vita_WatchdogSampleLongFrame

A game/draw frame running past 90 ms: what that thread is doing right now -
running or blocked (and on which kernel object), what it was last marked as
doing, and the return addresses on its stack (decode with addr2line against
the ELF, base 0x81000000). The benchmark's fights had 150-180 ms frames in a
script's sys.spawn that the PC runs in under 1 ms.
===============
*/
extern volatile uint64 vita_gdFrameStartUs; // common_frame.cpp

static void Vita_WatchdogSampleLongFrame(SceUID uid, int sampleNum, uint64 frameUs)
{
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (uid < 0 || sceKernelGetThreadInfo(uid, &info) < 0) {
        return;
    }
    // status bits: 1 running, 2 ready, 8 waiting
    Vita_Checkpoint("watchdog: long game/draw frame, %.0f ms so far, sample %d: status 0x%X (1 running, 2 ready, 8 waiting), wait type 0x%X on 0x%08X, marked \"%s\"",
        frameUs / 1000.0, sampleNum, (unsigned int)info.status, (unsigned int)info.waitType, (unsigned int)info.waitId, (const char*)vita_gamePhase);
    SceKernelModuleInfo mod;
    memset(&mod, 0, sizeof(mod));
    mod.size = sizeof(mod);
    const SceUID modid = sceKernelGetModuleIdByAddr((void*)&Vita_WatchdogSampleLongFrame);
    if (modid < 0 || sceKernelGetModuleInfo(modid, &mod) < 0) {
        return;
    }
    const uint32_t textStart = (uint32_t)mod.segments[0].vaddr;
    const uint32_t textEnd = textStart + mod.segments[0].memsz;
    const uint32_t* lo = (const uint32_t*)info.stack;
    const uint32_t* hi = (const uint32_t*)((const char*)info.stack + info.stackSize);
    // top down, outermost call first, as the hang dump does: below the live
    // stack pointer the stack holds stale return addresses from older calls,
    // so the real chain is the first stretch of the list
    char line[512];
    int len = 0;
    int found = 0;
    for (const uint32_t* p2 = hi - 1; p2 >= lo && found < 80; p2--) {
        const uint32_t v = *p2;
        if (!Vita_IsReturnAddress(v, textStart, textEnd)) {
            continue;
        }
        found++;
        len += idStr::snPrintf(line + len, sizeof(line) - len, " %08X", (unsigned int)(0x81000000u + ((v & ~1u) - textStart)));
        if (len > 400) {
            Vita_Checkpoint("watchdog: long frame stack (top down):%s", line);
            len = 0;
        }
    }
    if (len > 0) {
        Vita_Checkpoint("watchdog: long frame stack (top down):%s", line);
    }
}

static int Vita_WatchdogThread(SceSize argSize, void* argp)
{
    unsigned int lastSeq = vita_mainPhaseSeq;
    int still = 0;
    int reports = 0;
    int dumps = 0;
    for (;;) {
        // a second, looking for the benchmark's L + R every 100 ms until the
        // menus are up - and every 20 ms at the game/draw thread's frame, to
        // sample one that runs long (at most 3 samples a frame, 24 a run)
        static uint64 sampledFrame = 0;
        static int samplesThisFrame = 0;
        static int longSamples = 0;
        for (int i = 0; i < 50; i++) {
            sceKernelDelayThread(20 * 1000);
            if ((i % 5) == 0) {
                Vita_CheckBenchmarkButtons();
            }
            const uint64 start = vita_gdFrameStartUs;
            if (start != 0 && longSamples < 24) {
                const uint64 now = Sys_Microseconds();
                if (now > start && now - start > 90 * 1000) {
                    if (start != sampledFrame) {
                        sampledFrame = start;
                        samplesThisFrame = 0;
                    }
                    if (samplesThisFrame < 3) {
                        samplesThisFrame++;
                        longSamples++;
                        Vita_WatchdogSampleLongFrame(vitaGameThreadUid, samplesThisFrame, now - start);
                    }
                }
            }
        }
        const unsigned int seq = vita_mainPhaseSeq;
        if (seq != lastSeq) {
            lastSeq = seq;
            still = 0;
            continue;
        }
        still++;
        if ((still == 10 || still == 30 || (still % 60) == 0) && reports < 40) {
            reports++;
            Vita_Checkpoint("watchdog: main thread unchanged for %d s at \"%s\" (game/draw thread at \"%s\", %u frames done so far)",
                still, (const char*)vita_mainPhase, (const char*)vita_gamePhase, vitaFramesDone);
        }
        // once per stall (and at most three stalls a run): where exactly
        if (still == 15 && dumps < 3) {
            dumps++;
            Vita_WatchdogDumpThread("main", vitaMainThreadUid);
            Vita_WatchdogDumpThread("game/draw", vitaGameThreadUid);
            Vita_Checkpoint("watchdog: second sample, to tell live frames from a spinning loop:");
            Vita_WatchdogDumpThread("game/draw", vitaGameThreadUid);
            Vita_LogFlush(); // a hang may never let the writer run again
        }
    }
    return 0;
}

static void Vita_StartWatchdog()
{
    // lowest user priority: it only ever sleeps and occasionally writes a line
    SceUID t = sceKernelCreateThread("d3bfg_watchdog", Vita_WatchdogThread, 0x10000100 + 30, 0x4000, 0,
        Vita_Core3Available() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0, NULL);
    Vita_RegisterThread(t);
    Vita_NameThread(t, "d3bfg_watchdog", Vita_Core3Available() ? 3 : -1);
    if (t >= 0) {
        sceKernelStartThread(t, 0, NULL);
    }
}

/*
===============
doom_main
Runs the engine on a thread with a large stack.
===============
*/
static int doom_main(unsigned int argc, void* argv)
{
    Vita_Checkpoint("doom_main: entered (code loaded with doom_main at %p - subtract its address in the ELF to decode logged addresses)", (void*)doom_main);
#if defined(ID_VITA_NEON_MATH)
    enable_runfast(); // math-neon: VFP flush-to-zero + default NaN (per thread; see idSysThread)
#endif
    vitaMainThreadUid = sceKernelGetThreadId();
    Vita_StartLogWriter();
    Vita_StartWatchdog();
    Vita_CheckBenchmarkButtons();

    VITA_MARK("Posix_EarlyInit");
    Posix_EarlyInit();
    Vita_Checkpoint("doom_main: Posix_EarlyInit done");

    if (cmdargc > 1) {
        common->Init(cmdargc - 1, &cmdargv[1], NULL);
    } else {
        common->Init(0, NULL, NULL);
    }
    Vita_Checkpoint("doom_main: common->Init done");

    // the trophies (vita_trophies.cpp): the renderer is up for the setup dialog
    extern void Vita_TrophyStart();
    Vita_TrophyStart();

    Posix_LateInit();
    Vita_Checkpoint("doom_main: entering frame loop");

    while (1) {
        common->Frame();
        vitaFramesDone++;
        // the first few frames, then a heartbeat every ~30 s at 30 fps
        if (vitaFramesDone <= 3 || (vitaFramesDone % 900) == 0) {
            Vita_Checkpoint("doom_main: frame %u completed", vitaFramesDone);
        }
    }

    return 0;
}

/*
===============
main
===============
*/
int main(int argc, const char** argv)
{
    cmdargc = argc;
    cmdargv = argv;

    // Make sure the writable data directory exists before anything touches it.
    sceIoMkdir(VITA_DATA_PATH, 0777);

    // Start each run with an empty log so the file always describes this boot.
    SceUID logFd = sceIoOpen(VITA_DATA_PATH "/console.txt",
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (logFd >= 0) {
        sceIoClose(logFd);
    }
    // vitaGL appends to ux0:data/vitaGL.log and never clears it: lines from an
    // earlier run (a DOOM I session, an older build) read as if this run had
    // them. Cleared here, before vitaGL starts, with a header naming the run.
    {
        SceUID glLog = sceIoOpen("ux0:/data/vitaGL.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (glLog >= 0) {
            char header[160];
            idStr::snPrintf(header, sizeof(header), "vitaGL.log - cleared at the start of this run (" ENGINE_VERSION ", built " __DATE__ " " __TIME__ ")\n");
            sceIoWrite(glLog, header, strlen(header));
            sceIoClose(glLog);
        }
    }
    Vita_Checkpoint("main: " ENGINE_VERSION " starting (built " __DATE__ " " __TIME__ "; vitaGL.log cleared)");
    Vita_LogMemoryBudget("at boot");

    // Checked up front so the log says so plainly, rather than the absence
    // showing up later as an unexplained crash in shader loading.
    Vita_CheckShaderCompiler();

    // Clocks fixed by request: CPU 500 MHz, GPU 222 MHz. The stock user API
    // stops at 444; 500 needs an overclock plugin hooking this call - path_oc
    // (tools/vita/path_oc), PSVshell and the like - so fall back to 444 when
    // the firmware refuses.
    int armResult = scePowerSetArmClockFrequency(500);
    if (armResult < 0 || scePowerGetArmClockFrequency() < 500) {
        scePowerSetArmClockFrequency(444);
    }
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    Vita_Checkpoint("main: clocks CPU %d MHz (500 requested, 0x%08X), GPU %d MHz, bus %d MHz, xbar %d MHz",
        scePowerGetArmClockFrequency(), (unsigned int)armResult, scePowerGetGpuClockFrequency(),
        scePowerGetBusClockFrequency(), scePowerGetGpuXbarClockFrequency());

    // GXM's buffers, set before vitaGL starts (SDL starts it with the window).
    // cpu fight 3 left the main thread blocked ~12 ms a frame inside GL calls
    // (31 ms of back end wall time for 19 ms of CPU) at 1,300 draws and 110k
    // triangles plus a pass per light: a full ring buffer makes a draw wait
    // for the GPU to drain it, and a full parameter buffer makes the GPU
    // render the scene in parts (partial renders: every tile's color, depth
    // and stencil written out and read back). Sony's defaults are 16 MB of
    // parameter buffer, 128 KB of VDM ring, 2 MB of vertex ring and 512 KB of
    // fragment ring; the maps leave 27+ MB of VRAM free at their lowest.
    vglSetParamBufferSize(28 * 1024 * 1024);
    vglSetVDMBufferSize(512 * 1024);
    vglSetVertexBufferSize(4 * 1024 * 1024);
    vglSetFragmentBufferSize(2 * 1024 * 1024);
    Vita_Checkpoint("main: GXM buffers - parameter 28 MB, VDM ring 512 KB, vertex ring 4 MB, fragment ring 2 MB (Sony's defaults 16 MB / 128 KB / 2 MB / 512 KB)");

    Vita_Core3Available();
    Vita_CheckBenchmarkButtons();

    // The engine recurses deeply during map load, so give it a big stack.
    // Core 0 to itself: it runs the render back end, while Game/Draw has core 1
    // and the first job thread core 2 (idSysThread::ThreadProc).
    // 0x41: one step below OpenAL's mixer, which shares core 0 (see
    // Vita_ScanThreads) - at the same priority it would wait for the whole
    // render back end to finish
    SceUID mainThread = sceKernelCreateThread(
        "doom3bfg", doom_main, 0x41, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (mainThread < 0) {
        Vita_Checkpoint("main: sceKernelCreateThread failed (0x%08X)", (unsigned int)mainThread);
        return -1;
    }
    Vita_RegisterThread(mainThread);
    Vita_NameThread(mainThread, "doom3bfg (main: back end)", 0);
    sceKernelStartThread(mainThread, 0, NULL);

    return sceKernelExitDeleteThread(0);
}
