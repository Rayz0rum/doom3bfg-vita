/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - the benchmark (PS Vita port).

"benchmark [script]" plays a script of map loads, viewpoints, turns, fights
and pauses (the default is built in below; base/benchmark.txt, if present,
replaces it) and logs - exhaustively - where every frame's time went: per
segment of the script, per second, and every frame over 40 ms on its own,
with the scene, the jobs, the loads and the memory of that frame, and a
diagnosis of what held the segment back. Everything goes to the log as it
happens (console.txt on the Vita, through the asynchronous writer) and, at
the end, to benchmark_results.txt in the save folder.

On the Vita: hold L + R while the game starts. On a PC: +benchmark.

"benchScan <map> [views]" (PC builds only) is how the script was made: it
stands the player on every walkable spot of the map's AAS, looks eight ways
from each, scores what the renderer had to do (draws, triangles, shadow
volumes, lights, skinned vertices, extra views such as security camera
screens) and writes the most taxing views - with places to spawn a fight in
front of them - to benchmark/scan_<map>.txt.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Common_local.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif
void TransformVertsAndTangents(idDrawVert* targetVerts, const int numVerts, const idDrawVert* baseVerts, const idJointMat* joints); // Model_md5.cpp (the skinning test)
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>      // sceKernelGetProcessTimeLow (the clock comparison at the start)
#include <psp2/kernel/threadmgr/thread.h> // sceKernelGetSystemTimeWide
#endif

#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
/*
================================================================================
The timing zones on the PC test build (vita_zones.h; the Vita's are in
vita_main.cpp, printed with its perf block): reset when a segment opens,
printed when it closes.
================================================================================
*/
#include "../sys/vita/vita_zones.h"

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

static void Bench_PcZoneReset()
{
    for (vitaZone_t* z = vitaZones; z != NULL; z = z->next) {
        z->us = 0;
        z->calls = 0;
    }
}

static void Bench_PcZoneReport(const char* segment, double frames)
{
    if (frames <= 0.0 || vitaZones == NULL) {
        return;
    }
    idStr line = va("bench   pc zones [ms / calls per frame] for \"%s\":", segment);
    for (int shown = 0; shown < 90; shown++) {
        vitaZone_t* best = NULL;
        for (vitaZone_t* z = vitaZones; z != NULL; z = z->next) {
            if (z->timed && z->us > 0 && (best == NULL || z->us > best->us)) {
                best = z;
            }
        }
        if (best == NULL) {
            break;
        }
        line += va(" %s %.3f/%.1f |", best->name, best->us / 1000.0 / frames, best->calls / frames);
        best->us = -best->us;
        if (line.Length() > 3000) { // the console truncates longer lines
            common->Printf("%s\n", line.c_str());
            line = "bench   pc zones (cont.):";
        }
    }
    common->Printf("%s\n", line.c_str());
    line = va("bench   pc zone counts [per frame] for \"%s\":", segment);
    for (vitaZone_t* z = vitaZones; z != NULL; z = z->next) {
        if (!z->timed && z->calls > 0) {
            line += va(" %s %.1f/frame |", z->name, z->calls / frames);
            if (line.Length() > 3000) {
                common->Printf("%s\n", line.c_str());
                line = "bench   pc zone counts (cont.):";
            }
        }
    }
    Bench_PcZoneReset();
    common->Printf("%s\n", line.c_str());
}
#endif

#include "../sys/vita/vita_zones.h"

#define GD_MAX_NODES 1024
// a segment's sums for the tree, allocated while the segment runs
struct benchGDStats_t {
    float excl[GD_MAX_NODES];
    float incl[GD_MAX_NODES];
    float calls[GD_MAX_NODES];
    float inclMax[GD_MAX_NODES];
};
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
================================================================================

The game/draw thread's tree of passes (vita_zones.h)

Every timed zone and GD_SCOPE entered on the game/draw thread is a node, nested
as the code ran: "game tics" > "game tic: entity thinks" > "idAI" > "ai anim
move" > "clip translation: world (box)"... A node collects its EXCLUSIVE time -
what the thread spent in it and not in a node below it - so the nodes of a
frame add up to the thread's whole frame (idGameThread::Run, from its first
line to its last), and work nobody instrumented shows up as its parent's own
time instead of disappearing. Job lists the thread waits on are nodes too,
with the jobs it ran itself below them: a wait's own time is time spent
waiting for jobs other threads were running.

Only the game/draw thread touches the table (job workers and the main thread
are told apart by their thread register), between VitaGD_FrameBegin and
VitaGD_FrameEnd; the benchmark reads the last frame's figures on the main
thread after waiting for the game/draw thread, so nothing is shared while
both run.

================================================================================
*/
struct gdNode_t {
    const char* name;
    short parent, firstChild, nextSibling, depth;
    uint32 us;    // exclusive, this frame
    uint32 calls; // times entered, this frame
};
static gdNode_t gdNodes[GD_MAX_NODES];
static int gdNumNodes = 0;
static int gdCur = 0;
static uint64 gdMark = 0;
static uintptr_t gdThread = 0;
static bool gdOpen = false;
static int gdUnbalanced = 0;
// the last finished frame: exclusive and inclusive us, calls per node
static uint32 gdLastExcl[GD_MAX_NODES];
static uint32 gdLastIncl[GD_MAX_NODES];
static uint32 gdLastCalls[GD_MAX_NODES];
static int gdLastNumNodes = 0;
static bool gdLastValid = false;

#if defined(__vita__)
extern "C" int sceKernelGetThreadId(void);
static bool gdTokenBySyscall = false; // the thread register did not tell threads apart
static uintptr_t gdMainToken = 0;     // the main thread's, taken by VitaBench_Frame
#endif

static ID_INLINE uintptr_t Bench_ThreadToken()
{
#if defined(__vita__)
    // the user read-only thread register: each thread's own TLS block, read
    // in one instruction (a thread id syscall per zone would cost more than
    // many of the zones it times)
    if (gdTokenBySyscall) {
        return (uintptr_t)sceKernelGetThreadId();
    }
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
#else
    return (uintptr_t)Sys_GetCurrentThreadID();
#endif
}

static void GD_Init()
{
    memset(gdNodes, 0, sizeof(gdNodes));
    gdNodes[0].name = "game/draw thread frame";
    gdNodes[0].parent = -1;
    gdNodes[0].firstChild = gdNodes[0].nextSibling = -1;
    gdNodes[1].name = "(passes past the table's size)";
    gdNodes[1].parent = 0;
    gdNodes[1].firstChild = gdNodes[1].nextSibling = -1;
    gdNodes[1].depth = 1;
    gdNodes[0].firstChild = 1;
    gdNumNodes = 2;
}

static int GD_Child(int parent, const char* name)
{
    // a zone always passes the same string, so the pointer finds it; the
    // names are compared only when no pointer matches (a pass named in two
    // places, or a node not made yet) - a string compare per sibling on every
    // pass was most of a lookup under nodes with many children
    for (int c = gdNodes[parent].firstChild; c >= 0; c = gdNodes[c].nextSibling) {
        if (gdNodes[c].name == name) {
            return c;
        }
    }
    for (int c = gdNodes[parent].firstChild; c >= 0; c = gdNodes[c].nextSibling) {
        if (idStr::Cmp(gdNodes[c].name, name) == 0) {
            return c;
        }
    }
    if (gdNumNodes >= GD_MAX_NODES) {
        return 1;
    }
    const int c = gdNumNodes++;
    gdNode_t& n = gdNodes[c];
    n.name = name;
    n.parent = (short)parent;
    n.firstChild = -1;
    n.nextSibling = gdNodes[parent].firstChild;
    n.depth = gdNodes[parent].depth + 1;
    n.us = n.calls = 0;
    gdNodes[parent].firstChild = (short)c;
    return c;
}

// for other per-thread caches (R_FrameAlloc's chunks): the token, and whether
// tokens are known to tell threads apart - on the Vita once the game/draw
// thread's first frame showed a register value different from the main
// thread's
uintptr_t Vita_ThreadToken()
{
    return Bench_ThreadToken();
}

bool Vita_ThreadTokensPerThread()
{
#if defined(__vita__)
    return !gdTokenBySyscall && gdMainToken != 0 && gdThread != 0 && gdThread != gdMainToken;
#else
    return true;
#endif
}

bool VitaGD_OnThread()
{
    return gdOpen && Bench_ThreadToken() == gdThread;
}

int VitaGD_PushAt(const char* name, uint64 now)
{
    if (!gdOpen || Bench_ThreadToken() != gdThread) {
        return -1;
    }
    gdNodes[gdCur].us += (uint32)(now - gdMark);
    gdMark = now;
    const int parent = gdCur;
    gdCur = GD_Child(parent, name);
    gdNodes[gdCur].calls++;
    return parent;
}

void VitaGD_PopAt(int parent, uint64 now)
{
    if (parent < 0 || !gdOpen) {
        return;
    }
    gdNodes[gdCur].us += (uint32)(now - gdMark);
    gdMark = now;
    gdCur = parent;
}

int VitaGD_Push(const char* name)
{
    if (!gdOpen || Bench_ThreadToken() != gdThread) {
        return -1;
    }
    return VitaGD_PushAt(name, Sys_Microseconds());
}

void VitaGD_Pop(int parent)
{
    if (parent < 0 || !gdOpen) {
        return;
    }
    VitaGD_PopAt(parent, Sys_Microseconds());
}

// idGameThread::Run, first and last thing
void VitaGD_FrameBegin()
{
    if (gdNumNodes == 0) {
        GD_Init();
    }
#if defined(__vita__)
    if (!gdTokenBySyscall && (Bench_ThreadToken() == 0 || Bench_ThreadToken() == gdMainToken)) {
        gdTokenBySyscall = true; // same register value as the main thread: not per thread
    }
#endif
    gdThread = Bench_ThreadToken();
    gdCur = 0;
    gdMark = Sys_Microseconds();
    gdNodes[0].calls = 1;
    gdOpen = true;
}

void VitaGD_FrameEnd()
{
    if (!gdOpen) {
        return;
    }
    const uint64 now = Sys_Microseconds();
    gdNodes[gdCur].us += (uint32)(now - gdMark);
    if (gdCur != 0) {
        gdUnbalanced++; // a pass still open at the end of the frame (should not happen)
    }
    gdOpen = false;
    const int n = gdNumNodes;
    for (int i = 0; i < n; i++) {
        gdLastExcl[i] = gdLastIncl[i] = gdNodes[i].us;
        gdLastCalls[i] = gdNodes[i].calls;
        gdNodes[i].us = 0;
        gdNodes[i].calls = 0;
    }
    // inclusive: children come after their parents in the table
    for (int i = n - 1; i > 0; i--) {
        gdLastIncl[gdNodes[i].parent] += gdLastIncl[i];
    }
    gdLastNumNodes = n;
    gdLastValid = true;
}
#else
static bool gdLastValid = false;
#endif

/*
================================================================================

What reaches the screen (Vita)

The frame times above are the main thread's loop; what the player sees is
when frames are flipped. vitaGL's display queue thread calls back just before
it hands each frame to the display (it shows from the next vblank), and the
vblank counter there says how many vblanks the previous frame stayed on the
screen: 2 is a 30 fps frame on time, 3 a missed vblank (that frame was up for
50 ms), 1 a frame replaced early. The display thread writes a ring, the main
thread reads it - nothing else is shared.

================================================================================
*/
#define BENCH_FLIP_BUCKETS 5 // shown for 1, 2, 3, 4, 5+ vblanks
#if defined(__vita__)
extern "C" void vglSetDisplayCallback(void (*cb)(void* framebuf));
extern "C" int sceDisplayGetVcount(void);
#define BENCH_FLIP_RING 256
static volatile unsigned int benchFlipVcount[BENCH_FLIP_RING];
static volatile unsigned int benchFlipCount = 0; // written by the display queue thread only
static unsigned int benchFlipRead = 0;
static unsigned int benchFlipLastVcount = 0;
static bool benchFlipHooked = false;

void VitaVerify_DisplayFrame(void* framebuf); // VitaVerify.cpp: its screenshots
static void Bench_DisplayFlip(void* framebuf)
{
    VitaVerify_DisplayFrame(framebuf);
    const unsigned int n = benchFlipCount;
    benchFlipVcount[n & (BENCH_FLIP_RING - 1)] = (unsigned int)sceDisplayGetVcount();
    __sync_synchronize();
    benchFlipCount = n + 1;
}

// the flips since the last call, as counts per number of vblanks on screen
static int Bench_TakeFlips(int buckets[BENCH_FLIP_BUCKETS])
{
    memset(buckets, 0, sizeof(int) * BENCH_FLIP_BUCKETS);
    if (!benchFlipHooked) {
        benchFlipHooked = true;
        vglSetDisplayCallback(Bench_DisplayFlip);
        return 0;
    }
    const unsigned int count = benchFlipCount;
    __sync_synchronize();
    if (count - benchFlipRead > BENCH_FLIP_RING) {
        benchFlipRead = count - BENCH_FLIP_RING / 2; // fell behind (a load): start afresh
        benchFlipLastVcount = 0;
    }
    int flips = 0;
    for (; benchFlipRead != count; benchFlipRead++) {
        const unsigned int v = benchFlipVcount[benchFlipRead & (BENCH_FLIP_RING - 1)];
        if (benchFlipLastVcount != 0) {
            const unsigned int d = v - benchFlipLastVcount;
            buckets[d == 0 ? 0 : (d >= BENCH_FLIP_BUCKETS ? BENCH_FLIP_BUCKETS - 1 : d - 1)]++;
            flips++;
        }
        benchFlipLastVcount = v;
    }
    return flips;
}
#endif

// RenderSystem.cpp: the renderer's counters for the last frame
struct vitaBenchScene_t {
    int views, skinnedVerts, shadowVolumes, entities, lights, draws, tris, shadowTris, interactions;
};
extern vitaBenchScene_t vita_benchScene;
// ParallelJobList.cpp
extern int vita_benchJobCount[32];
extern uint64 vita_benchJobUs[32];
extern uint64 vita_benchTailWaitUs[32]; // ParallelJobList.cpp: the waits on a straggler
extern int vita_benchTailWaits[32];
extern vitaBenchSlowJob_t vita_benchSlowJobs[VITA_BENCH_SLOW_JOBS];
extern idSysInterlockedInteger vita_benchSlowJobNext;
extern vitaBenchJobType_t vita_benchJobTypes[VITA_BENCH_JOB_TYPES];
extern volatile int vita_benchNumJobTypes;
// images, sounds, models loaded (Image_load.cpp, AL_SoundSample.cpp, ModelManager.cpp)
int vita_benchLoads[3];
// Game_local.cpp (called while the game/draw thread is idle)
bool Vita_BenchGameReady(idStr& why);
void Vita_BenchPlayerSetup(bool god, bool noclip, bool notarget);
void Vita_BenchGameStats(idStr& out);
int Vita_BenchKill(const char* name);    // 1 killed now, 0 already dead, -1 gone
bool Vita_BenchRemove(const char* name); // false if already gone
#if !defined(__vita__)
int Vita_BenchScanSpots(idList<idVec3>& spots, idStrList& classes, idList<int>& classCounts, float cell);
bool Vita_BenchClearLine(const idVec3& a, const idVec3& b);
#endif

#if defined(__vita__)
void Vita_FreeMemoryKB(unsigned int* allFreeKB, unsigned int* cachedFreeKB); // vita_main.cpp
void Vita_LogMemory(const char* tag);                                         // vita_main.cpp
void Vita_BenchEnvironment();                                                 // vita_main.cpp
void Vita_PerfFlush(const char* label);                                       // common_frame.cpp
extern uint64 vita_frontEndUs[6];                                             // tr_frontend_main.cpp
extern volatile int vita_afUs, vita_animUs, vita_decalUs, vita_prtUs, vita_vcCopyUs, vita_soundUs;
extern int vita_dynamicResolutionStep;
extern int vita_textureQualityDrop;
extern int vita_slowShadowJobReports; // DynamicShadowVolume.cpp
void Vita_RenderSizeForStep(int step, int* w, int* h);
#endif

/*
================================================================================

The built-in script: DOOM 3's Alpha Labs Sector 4, EnPro Plant and CPU Complex
(game/alphalabs4, game/enpro, game/cpu). The views and fight positions come
from benchScan on the PC (see above): the most taxing places of each map.

================================================================================
*/
static const char* vitaBenchDefaultScript =
#include "VitaBench_script.inc"
    ;

/*
================================================================================

Measurement

================================================================================
*/

// The main thread's phases (common_frame.cpp's FRAME_PHASE marks): time from
// one mark to the next goes to the first one's name, and VitaBench_Frame
// takes the table at each frame - so the phases of a frame add up to it.
#define BENCH_MAX_PHASES 32
struct benchPhase_t {
    const char* name;
    uint64 us;
};
static benchPhase_t benchPhases[BENCH_MAX_PHASES];
static int benchNumPhases = 0;
static const char* benchPhaseCur = NULL;
static uint64 benchPhaseStart = 0;

static int Bench_PhaseIndex(const char* name)
{
    for (int i = 0; i < benchNumPhases; i++) {
        if (benchPhases[i].name == name) {
            return i;
        }
    }
    if (benchNumPhases < BENCH_MAX_PHASES) {
        benchPhases[benchNumPhases].name = name;
        benchPhases[benchNumPhases].us = 0;
        return benchNumPhases++;
    }
    return -1;
}

void VitaBench_Phase(const char* name)
{
    const uint64 now = Sys_Microseconds();
    if (benchPhaseCur != NULL) {
        const int i = Bench_PhaseIndex(benchPhaseCur);
        if (i >= 0) {
            benchPhases[i].us += now - benchPhaseStart;
        }
    }
    benchPhaseCur = name;
    benchPhaseStart = now;
}

// short labels for the log
static const char* Bench_PhaseLabel(const char* name)
{
    if (idStr::Cmpn(name, "frame: ", 7) == 0) {
        return name + 7;
    }
    return name;
}
static const float BENCH_TARGET_MS = 33.4f; // a 30 fps frame (33.33 ms) plus vsync jitter
static const float BENCH_HITCH_MS = 40.0f;  // logged frame by frame

struct benchPerFrame_t {
    float ms, gameMs, drawMs, backendMs, syncMs, waitMs;
    int tics;
    vitaBenchScene_t scene;
    float fe[6];  // front end: find lights+entities, wait shadow jobs, lights, models, guis+sort, subviews (Vita)
    float gm[6];  // ragdolls, animation, decals, particles, vertex copies, sound update on the game/draw thread (Vita)
    float rb[10]; // the render back end's passes (tr_backend_draw.cpp vita_rbPassUs)
    int alphaEdgeSurfaces; // alpha-tested surfaces in the soft edges' map
    float toksvigMs;       // Toksvig filtering of normal maps loaded during play
    int rbDraws;  // draw calls submitted by the back end
    int loads[3]; // images, sounds, models
    int resStep;
    unsigned int freeKB, cachedKB;
    int numPhases;
    benchPhase_t phases[BENCH_MAX_PHASES]; // the main thread's frame, phase by phase
    // the game/draw thread's frame: its whole length and its costliest passes
    // (exclusive time), from the tree
    float gdTotalMs;
    int flips[BENCH_FLIP_BUCKETS]; // frames flipped since the last one, by vblanks on screen
    int numFlips;
    int gdNumTop;
    short gdTop[12];
    float gdTopMs[12];
};

struct benchStat_t {
    double sum;
    float max;
    int nonZero; // frames with a value
    void Add(float v)
    {
        sum += v;
        if (v != 0.0f) {
            nonZero++;
        }
        if (v > max) {
            max = v;
        }
    }
};

struct benchSegment_t {
    idStr name;
    idStr map;
    int frames;
    double seconds;
    idList<float> frameMs;
    int buckets[7]; // <=33.4, -36, -40, -50, -66.7, -100, more
    int flips[BENCH_FLIP_BUCKETS]; // frames on screen for 1, 2, 3, 4, 5+ vblanks
    benchStat_t ms, gameMs, drawMs, backendMs, syncMs, waitMs, tics;
    benchStat_t fe[6], gm[6], rb[10], rbDraws, alphaEdgeSurfaces, toksvigMs;
    benchStat_t views, skinnedVerts, shadowVolumes, entities, lights, draws, tris, shadowTris, interactions;
    benchStat_t resStep;
    int loads[3];
    unsigned int minFreeKB, minCachedKB, endFreeKB, endCachedKB;
    int hitches, hitchesWithLoads, hitchesWithSlowJobs;
    int settleFrames;
    float settleWorst;
    int slowJobs;
    idStrList slowJobNames;
    idList<int> slowJobCounts;
    idList<int> slowJobWorst;
    int jobCount[32];
    uint64 jobUs[32];
    uint64 tailUs[32];
    int tailCount[32];
    int textureDropAtEnd;
    benchPerFrame_t worst; // the slowest frame, all of it
    // the main thread's phases summed over the segment
    const char* phaseNames[BENCH_MAX_PHASES];
    double phaseUs[BENCH_MAX_PHASES];
    float phaseMax[BENCH_MAX_PHASES];
    int numPhases;
    // the game/draw thread's tree summed over the segment (node index)
    int gdFrames;
    benchGDStats_t* gd; // freed once the segment's summary is written
    // jobs by type: the running totals when the segment opened, then the
    // segment's own figures
    int jobTypeCount[VITA_BENCH_JOB_TYPES];
    uint64 jobTypeUs[VITA_BENCH_JOB_TYPES];
    int jobTypeGdCount[VITA_BENCH_JOB_TYPES];
    uint64 jobTypeGdUs[VITA_BENCH_JOB_TYPES];
};

enum benchState_t {
    BENCH_OFF,
    BENCH_RUN,      // running the script
    BENCH_WAIT_MAP, // a map is loading or settling
    BENCH_TIMED,    // a timed command is running
    BENCH_VERIFY,   // a verification step is running (VitaVerify.cpp polls it)
#if !defined(__vita__)
    BENCH_SCAN,
#endif
};

struct benchInput_t {
    int buttons;
    int forward, right;
    float yawPerMs;
    int impulse; // -1 none
};

static struct {
    benchState_t state;
    idStrList lines;
    int pc;
    idStr scriptName;
    uint64 startUs;
    uint64 lastFrameUs;
    uint64 timedEndUs;
    idStr timedWhat;
    bool measuring;       // the timed command is measured ("hold", "turn", "fire" - not "wait")
    uint64 settleUntilUs; // after a teleport: frames until then are counted apart
    // map loading
    idStr mapName;
    uint64 mapRequestUs;
    uint64 mapReadyUs;
    int mapSettleMs;
    int mapFrames;
    bool mapLoaded;
    idStr mapWaitWhy;
    // segments
    benchSegment_t* seg;
    idList<benchSegment_t*> done;
    // per second
    uint64 secStartUs;
    int secFrames;
    float secWorst;
    double secMs, secGame, secDraw, secBackend, secSync, secWait;
    double secDraws, secTris, secShadowTris, secLights;
    // the last values of the running counters (per frame deltas)
    uint64 lastFe[6];
    int lastGm[6];
    int lastLoads[3];
    int lastSlowJob;
    // spawned monsters
    idStrList spawned;
    int spawnSerial;
    int perfLogWas;
    benchInput_t input;
    // everything logged, for benchmark_results.txt: in 64 KB chunks - one
    // idStr grown a line at a time copied the whole log (1.5 MB by the end)
    // on every line, tens of ms a frame in the late segments, and fragmented
    // the cached heap
    idList<char*> resultChunks;
    int resultLast; // bytes used in the last chunk
    int errors;
    // the full verification (verify): everything logged is streamed to
    // benchmark_results.txt as it goes - a two-hour run would not fit in
    // memory, and what was written survives a crash
    bool verifyMode;
    idFile* resultFile;
    // the view angles the script asks for outright (aiming at a monster)
    bool lookSet;
    float lookYaw, lookPitch;
} bench;

static const int BENCH_CHUNK = 64 * 1024;

static void Bench_Store(const char* text, int len)
{
    while (len > 0) {
        if (bench.resultFile != NULL && bench.resultChunks.Num() == 1 && bench.resultLast == BENCH_CHUNK) {
            // streaming (verify): out to the card, and the chunk starts over
            bench.resultFile->Write(bench.resultChunks[0], BENCH_CHUNK);
            bench.resultFile->Flush();
            bench.resultLast = 0;
        }
        if (bench.resultChunks.Num() == 0 || bench.resultLast == BENCH_CHUNK) {
            char* c = (char*)Mem_Alloc(BENCH_CHUNK, TAG_TEMP);
            if (c == NULL) {
                return;
            }
            bench.resultChunks.Append(c);
            bench.resultLast = 0;
        }
        const int n = Min(len, BENCH_CHUNK - bench.resultLast);
        memcpy(bench.resultChunks[bench.resultChunks.Num() - 1] + bench.resultLast, text, n);
        bench.resultLast += n;
        text += n;
        len -= n;
    }
}

static void Bench_FreeResults()
{
    for (int i = 0; i < bench.resultChunks.Num(); i++) {
        Mem_Free(bench.resultChunks[i]);
    }
    bench.resultChunks.Clear();
    bench.resultLast = 0;
}

static void Bench_Log(VERIFY_FORMAT_STRING const char* fmt, ...)
{
    char text[4096];
    va_list argptr;
    va_start(argptr, fmt);
    int len = idStr::vsnPrintf(text, sizeof(text) - 1, fmt, argptr);
    va_end(argptr);
    text[sizeof(text) - 2] = '\0';
    common->Printf("%s\n", text);
    len = (int)strlen(text);
    text[len++] = '\n';
    Bench_Store(text, len);
}

// for the verification (VitaVerify.cpp)
void VitaBench_Log(VERIFY_FORMAT_STRING const char* fmt, ...)
{
    char text[4096];
    va_list argptr;
    va_start(argptr, fmt);
    idStr::vsnPrintf(text, sizeof(text) - 1, fmt, argptr);
    va_end(argptr);
    text[sizeof(text) - 2] = '\0';
    if (bench.state == BENCH_OFF) {
        common->Printf("%s\n", text); // after the run (the finish screen): the results are written
        return;
    }
    Bench_Log("%s", text);
}

static void Bench_Memory(unsigned int& freeKB, unsigned int& cachedKB)
{
#if defined(__vita__)
    Vita_FreeMemoryKB(&freeKB, &cachedKB);
#elif defined(_WIN32)
    // the PC has no budget to speak of: the process's private bytes, as "free"
    // = 0 and cached = private (only logged, not judged)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    freeKB = 0;
    cachedKB = (unsigned int)(pmc.PrivateUsage / 1024);
#else
    freeKB = cachedKB = 0;
#endif
}

static const char* Bench_ThreadName(int t)
{
    static char names[32][24];
    if (t == 31) {
        return "waiting threads (Game/Draw, main)";
    }
    idStr::snPrintf(names[t], sizeof(names[t]), "JLProc_%d", t);
    return names[t];
}

static float Bench_Percentile(idList<float>& v, float p)
{
    if (v.Num() == 0) {
        return 0.0f;
    }
    v.SortWithTemplate(idSort_Quick<float, idSort_QuickDefault<float> >());
    int i = (int)(p * (v.Num() - 1) + 0.5f);
    return v[idMath::ClampInt(0, v.Num() - 1, i)];
}

static void Bench_OpenSegment(const char* name)
{
#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
    Bench_PcZoneReset();
#endif
    benchSegment_t* s = new benchSegment_t();
    s->name = name;
    s->map = bench.mapName;
    s->frames = 0;
    s->seconds = 0.0;
    memset(s->buckets, 0, sizeof(s->buckets));
    memset(s->flips, 0, sizeof(s->flips));
    benchStat_t zero = { 0.0, 0.0f };
    s->ms = s->gameMs = s->drawMs = s->backendMs = s->syncMs = s->waitMs = s->tics = zero;
    for (int i = 0; i < 6; i++) {
        s->fe[i] = zero;
    }
    for (int i = 0; i < 6; i++) {
        s->gm[i] = zero;
    }
    s->views = s->skinnedVerts = s->shadowVolumes = s->entities = s->lights = s->draws = s->tris = s->shadowTris = s->interactions = zero;
    s->resStep = zero;
    memset(s->loads, 0, sizeof(s->loads));
    s->minFreeKB = s->minCachedKB = 0xFFFFFFFFu;
    s->endFreeKB = s->endCachedKB = 0;
    s->hitches = s->hitchesWithLoads = s->hitchesWithSlowJobs = 0;
    s->settleFrames = 0;
    s->settleWorst = 0.0f;
    s->slowJobs = 0;
    memset(s->jobCount, 0, sizeof(s->jobCount));
    memset(s->jobUs, 0, sizeof(s->jobUs));
    for (int t = 0; t < 32; t++) {
        s->jobCount[t] = -vita_benchJobCount[t]; // the totals so far, taken off at the end
        s->jobUs[t] = (uint64)0 - vita_benchJobUs[t];
        s->tailUs[t] = (uint64)0 - vita_benchTailWaitUs[t];
        s->tailCount[t] = -vita_benchTailWaits[t];
    }
    s->textureDropAtEnd = 0;
    memset(&s->worst, 0, sizeof(s->worst));
    s->numPhases = 0;
    s->gdFrames = 0;
    s->gd = (benchGDStats_t*)Mem_ClearedAlloc(sizeof(benchGDStats_t), TAG_TEMP);
    for (int j = 0; j < VITA_BENCH_JOB_TYPES; j++) {
        vitaBenchJobType_t& jt = vita_benchJobTypes[j];
        s->jobTypeCount[j] = -jt.count;
        s->jobTypeUs[j] = (uint64)0 - jt.us;
        s->jobTypeGdCount[j] = -jt.gdCount;
        s->jobTypeGdUs[j] = (uint64)0 - jt.gdUs;
        jt.maxUs = 0;
    }
    bench.seg = s;
    bench.secStartUs = 0;
    bench.secFrames = 0;
    bench.secWorst = 0.0f;
    bench.secMs = bench.secGame = bench.secDraw = bench.secBackend = bench.secSync = bench.secWait = 0.0;
    bench.secDraws = bench.secTris = bench.secShadowTris = bench.secLights = 0.0;
#if defined(__vita__)
    vita_slowShadowJobReports = 0; // up to 24 slow shadow jobs described per segment
#endif
    Bench_Log("bench ---- segment %d: \"%s\" (%s) at %.1f s ----", bench.done.Num() + 1, name, bench.mapName.c_str(), (Sys_Microseconds() - bench.startUs) / 1000000.0);
}

static void Bench_SecondLine(uint64 now);

static void Bench_Diagnose(const benchSegment_t& s, idStr& out)
{
    const double n = Max(1, s.frames);
    const float onTarget = 100.0f * s.buckets[0] / (float)n;
    const float game = (float)(s.gameMs.sum / n), draw = (float)(s.drawMs.sum / n), backend = (float)(s.backendMs.sum / n);
    const float sync = (float)(s.syncMs.sum / n), wait = (float)(s.waitMs.sum / n);
    const float sound = (float)(s.gm[5].sum / n); // on the game/draw thread after the front end (vita_soundOnGameThread)
    out.Clear();
    if (s.frames == 0) {
        out = "no frames measured";
        return;
    }
    if (onTarget >= 95.0f && s.ms.max < 50.0f) {
        out = va("holds 30 fps (%.1f%% of frames on target, worst %.0f ms)", onTarget, s.ms.max);
    } else {
        out = va("misses 30 fps (%.1f%% on target, worst %.0f ms):", onTarget, s.ms.max);
    }
    // what bounds the frame: the game/draw thread, the back end or the GPU
    if (game + draw + sound > 30.0f || (wait > 4.0f && game + draw + sound > backend + sync)) {
        out += va(" CPU-BOUND ON THE GAME/DRAW THREAD (game %.1f + front end %.1f + sound %.1f = %.1f ms a frame, the main thread waited %.1f ms for it)", game, draw, sound, game + draw + sound, wait);
        if (game > draw) {
            out += va("; the game logic leads (%.2f tics a frame; ragdolls %.1f, animation %.1f, decals %.1f ms - per class in 'vita thinks', per system in 'vita zones')",
                s.tics.sum / n, s.gm[0].sum / n, s.gm[1].sum / n, s.gm[2].sum / n);
        } else {
            static const char* feNames[6] = { "finding lights+entities", "waiting for shadow jobs", "lights", "models (skinning, interactions, shadow jobs)", "guis+sort", "subviews (mirrors, camera screens)" };
            int worst = 0;
            for (int i = 1; i < 6; i++) {
                if (s.fe[i].sum > s.fe[worst].sum) {
                    worst = i;
                }
            }
            out += va("; the front end leads, mostly %s at %.1f ms a frame (%.0f entities, %.0f lights, %.0f shadow volumes, %.0f skinned verts, %.1f views)",
                feNames[worst], s.fe[worst].sum / n, s.entities.sum / n, s.lights.sum / n, s.shadowVolumes.sum / n, s.skinnedVerts.sum / n, s.views.sum / n);
        }
    }
    // with display pacing (vita_displayPacing) a frame that is early waits
    // for its vblank in the swap too: only frames that miss say the GPU
    if (sync > 4.0f && (s.ms.sum / n) > BENCH_TARGET_MS + 0.5f) {
        out += va(" GPU-BOUND: %.1f ms a frame waiting for the GPU at %.0f draws, %.0fk tris (%.0fk in shadow volumes - fill), %.0f lights, 3D view step %.1f",
            sync, s.draws.sum / n, s.tris.sum / n / 1000.0, s.shadowTris.sum / n / 1000.0, s.lights.sum / n, s.resStep.sum / n);
    }
    if (backend > 14.0f) {
        out += va(" RENDER BACK END %.1f ms a frame (GL command submission for %.0f draws)", backend, s.draws.sum / n);
    }
    {
        // a main-thread phase other than the back end, the waits and the cap
        // taking a real share of the frame
        for (int i = 0; i < s.numPhases; i++) {
            const char* l = Bench_PhaseLabel(s.phaseNames[i]);
            if (!idStr::Cmp(l, "render back end") || !idStr::Cmp(l, "wait for game/draw") || !idStr::Cmp(l, "swap + wait for GPU") || !idStr::Cmp(l, "fps cap")) {
                continue;
            }
            const float ms = (float)(s.phaseUs[i] / 1000.0 / n);
            if (ms >= 3.0f) {
                out += va(" MAIN THREAD BUSY IN '%s': %.1f ms a frame (worst %.0f)", l, ms, s.phaseMax[i]);
            }
        }
    }
    if (s.hitchesWithLoads > 0) {
        out += va(" %d hitches while assets loaded mid-game (images %d, sounds %d, models %d this segment) - streaming stalls", s.hitchesWithLoads, s.loads[0], s.loads[1], s.loads[2]);
    }
    if (s.slowJobs > 0) {
        int worst = 0;
        for (int i = 1; i < s.slowJobNames.Num(); i++) {
            if (s.slowJobWorst[i] > s.slowJobWorst[worst]) {
                worst = i;
            }
        }
        out += va(" %d jobs over 3 ms (worst '%s' %.1f ms) - the front end waits for each%s", s.slowJobs,
            s.slowJobNames.Num() ? s.slowJobNames[worst].c_str() : "?", s.slowJobNames.Num() ? s.slowJobWorst[worst] / 1000.0f : 0.0f,
            s.hitchesWithSlowJobs ? va(", %d hitches had one", s.hitchesWithSlowJobs) : "");
    }
    if (onTarget < 95.0f || s.ms.max >= 50.0f) {
        // the slowest frame, and what took its time
        const benchPerFrame_t& w = s.worst;
        const float parts[4] = { w.gameMs + w.drawMs, w.backendMs, w.syncMs, w.waitMs };
        static const char* partNames[4] = { "the game/draw thread (game + front end)", "the render back end", "waiting for the GPU / the swap", "the main thread waiting on game/draw" };
        int big = 0;
        for (int i = 1; i < 4; i++) {
            if (parts[i] > parts[big]) {
                big = i;
            }
        }
        out += va(" Slowest frame %.1f ms, mostly %s: game %.1f (%d tics) + front end %.1f, back end %.1f, GPU/swap wait %.1f, main waited %.1f; %d draws, %d lights, %d new interactions; loads %d images %d sounds %d models.",
            w.ms, partNames[big], w.gameMs, w.tics, w.drawMs, w.backendMs, w.syncMs, w.waitMs, w.scene.draws, w.scene.lights, w.scene.interactions, w.loads[0], w.loads[1], w.loads[2]);
        if (s.hitches > 0 && s.hitches <= 3 && onTarget >= 95.0f) {
            out += va(" Only %d frame(s) over %.0f ms: isolated spikes, not the scene's steady cost.", s.hitches, BENCH_HITCH_MS);
        }
    }
#if defined(__vita__)
    if (s.minCachedKB < 8 * 1024) {
        out += va(" CACHED HEAP NEARLY FULL (%u MB at the lowest): what is allocated now lands in uncached memory, slow for the CPU", s.minCachedKB / 1024);
    }
    if (s.minFreeKB < 32 * 1024) {
        out += va(" MEMORY TIGHT (%u MB free at the lowest; texture drop %d)", s.minFreeKB / 1024, s.textureDropAtEnd);
    }
#endif
}

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
// "parent > name" for a node (a name alone can sit under several parents)
static const char* Bench_GDPath(int node)
{
    const int parent = gdNodes[node].parent;
    if (parent <= 0) {
        return gdNodes[node].name;
    }
    return va("%s > %s", gdNodes[parent].name, gdNodes[node].name);
}

static void Bench_PrintGDNode(const benchSegment_t& s, int node, double n)
{
    const int depth = gdNodes[node].depth;
    char indent[40];
    const int pad = Min(2 * depth, (int)sizeof(indent) - 1);
    memset(indent, ' ', pad);
    indent[pad] = 0;
    Bench_Log("bench     %s%s %.2f (%.1f) [own %.2f] x%.1f", indent, gdNodes[node].name,
        s.gd->incl[node] / 1000.0 / n, s.gd->inclMax[node], s.gd->excl[node] / 1000.0 / n, s.gd->calls[node] / n);
    // the children, costliest first; the small ones summed on one line
    int kids[GD_MAX_NODES];
    int numKids = 0;
    for (int c = gdNodes[node].firstChild; c >= 0; c = gdNodes[c].nextSibling) {
        if (s.gd->incl[c] > 0.0) {
            kids[numKids++] = c;
        }
    }
    for (int i = 1; i < numKids; i++) {
        const int v = kids[i];
        int j = i - 1;
        while (j >= 0 && s.gd->incl[kids[j]] < s.gd->incl[v]) {
            kids[j + 1] = kids[j];
            j--;
        }
        kids[j + 1] = v;
    }
    int smallCount = 0;
    double smallUs = 0.0;
    for (int i = 0; i < numKids; i++) {
        const int c = kids[i];
        if (s.gd->incl[c] / n >= 50.0 || s.gd->inclMax[c] >= 1.0f) {
            Bench_PrintGDNode(s, c, n);
        } else {
            smallCount++;
            smallUs += s.gd->incl[c];
        }
    }
    if (smallCount > 0) {
        Bench_Log("bench     %s  (+%d smaller passes: %.2f)", indent, smallCount, smallUs / 1000.0 / n);
    }
}

static void Bench_PrintGDTree(const benchSegment_t& s)
{
    if (s.gdFrames == 0 || gdNumNodes == 0 || s.gd == NULL) {
        Bench_Log("bench   game/draw thread: no frames measured");
        return;
    }
    const double n = s.gdFrames;
    Bench_Log("bench   game/draw thread, every pass, per frame: avg ms including the passes below it (worst frame) [own: not in a pass listed below it] x times entered a frame%s:",
        gdUnbalanced ? va(" (%d frames ended with a pass still open)", gdUnbalanced) : "");
#if defined(__vita__)
    if (gdTokenBySyscall) {
        Bench_Log("bench     (threads told apart by a thread id syscall: the thread register was not per thread - each pass costs a little more)");
    }
#endif
    Bench_PrintGDNode(s, 0, n);
    Bench_Log("bench     = %.2f ms of the game/draw thread a frame (game %.1f + front end %.1f + sound %.1f measured separately)",
        s.gd->incl[0] / 1000.0 / n, s.gameMs.sum / Max(1, s.frames), s.drawMs.sum / Max(1, s.frames), s.gm[5].sum / Max(1, s.frames));
}
#endif

static void Bench_PrintJobTypes(const benchSegment_t& s)
{
    const double n = Max(1, s.frames);
    int order[VITA_BENCH_JOB_TYPES];
    int num = 0;
    for (int j = 0; j < vita_benchNumJobTypes && j < VITA_BENCH_JOB_TYPES; j++) {
        if (s.jobTypeCount[j] > 0) {
            order[num++] = j;
        }
    }
    for (int i = 1; i < num; i++) {
        const int v = order[i];
        int k = i - 1;
        while (k >= 0 && s.jobTypeUs[order[k]] < s.jobTypeUs[v]) {
            order[k + 1] = order[k];
            k--;
        }
        order[k + 1] = v;
    }
    char line[2048];
    int len = idStr::snPrintf(line, sizeof(line), "bench   jobs by type, per frame (CPU time on every thread):");
    double total = 0.0;
    for (int i = 0; i < num; i++) {
        const int j = order[i];
        total += (double)s.jobTypeUs[j];
        if (len < (int)sizeof(line) - 160) {
            len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.1f jobs %.2f ms (avg %.0f us, longest %.1f ms; game/draw thread ran %.1f of them, %.2f ms) |",
                vita_benchJobTypes[j].name, s.jobTypeCount[j] / n, s.jobTypeUs[j] / 1000.0 / n,
                (double)s.jobTypeUs[j] / Max(1, s.jobTypeCount[j]), vita_benchJobTypes[j].maxUs / 1000.0,
                s.jobTypeGdCount[j] / n, s.jobTypeGdUs[j] / 1000.0 / n);
        }
    }
    Bench_Log("%s = %.2f ms of job CPU time a frame", line, total / 1000.0 / n);
}

static void Bench_CloseSegment()
{
    benchSegment_t* sp = bench.seg;
    if (sp == NULL) {
        return;
    }
    bench.seg = NULL;
    benchSegment_t& s = *sp;
    if (bench.secFrames > 0) {
        bench.seg = sp; // the last, partial second
        Bench_SecondLine(Sys_Microseconds());
        bench.seg = NULL;
    }
    for (int t = 0; t < 32; t++) {
        s.jobCount[t] += vita_benchJobCount[t];
        s.jobUs[t] += vita_benchJobUs[t];
        s.tailUs[t] += vita_benchTailWaitUs[t];
        s.tailCount[t] += vita_benchTailWaits[t];
    }
    for (int j = 0; j < VITA_BENCH_JOB_TYPES; j++) {
        const vitaBenchJobType_t& jt = vita_benchJobTypes[j];
        s.jobTypeCount[j] += jt.count;
        s.jobTypeUs[j] += jt.us;
        s.jobTypeGdCount[j] += jt.gdCount;
        s.jobTypeGdUs[j] += jt.gdUs;
    }
    Bench_Memory(s.endFreeKB, s.endCachedKB);
#if defined(__vita__)
    s.textureDropAtEnd = vita_textureQualityDrop;
#endif
    const double n = Max(1, s.frames);
    idList<float> sorted = s.frameMs;
    const float median = Bench_Percentile(sorted, 0.5f);
    const float p95 = Bench_Percentile(sorted, 0.95f);
    const float p99 = Bench_Percentile(sorted, 0.99f);
    Bench_Log("bench segment %d \"%s\" summary:", bench.done.Num() + 1, s.name.c_str());
    Bench_Log("bench   frames %d in %.1f s = %.1f fps | frame avg %.1f ms, median %.1f, 95th %.1f, 99th %.1f, worst %.1f",
        s.frames, s.seconds, s.seconds > 0.0 ? s.frames / s.seconds : 0.0, s.ms.sum / n, median, p95, p99, s.ms.max);
    {
        int shown = 0;
        for (int i = 0; i < BENCH_FLIP_BUCKETS; i++) {
            shown += s.flips[i];
        }
        if (shown > 0) {
            Bench_Log("bench   on screen (vblanks counted at each flip): %d frames shown - for 2 vblanks (30 fps, on time) %.1f%% | 3 (a missed vblank: 50 ms on screen) %.1f%% | 4 %.1f%% | 5 or more %.1f%% | 1 (replaced early) %.1f%%",
                shown, 100.0 * s.flips[1] / shown, 100.0 * s.flips[2] / shown, 100.0 * s.flips[3] / shown, 100.0 * s.flips[4] / shown, 100.0 * s.flips[0] / shown);
        }
    }
    Bench_Log("bench   frame times (main thread loop): <=33.4 ms %.1f%% | 33.4-36 %.1f%% | 36-40 %.1f%% | 40-50 %.1f%% | 50-66.7 %.1f%% | 66.7-100 %.1f%% | over 100 %.1f%%",
        100.0 * s.buckets[0] / n, 100.0 * s.buckets[1] / n, 100.0 * s.buckets[2] / n, 100.0 * s.buckets[3] / n, 100.0 * s.buckets[4] / n, 100.0 * s.buckets[5] / n, 100.0 * s.buckets[6] / n);
    Bench_Log("bench   per frame avg (worst): game %.1f (%.1f) ms in %.2f tics | front end %.1f (%.1f) | back end %.1f (%.1f) | GPU/swap wait %.1f (%.1f) | main thread waiting on game/draw %.1f (%.1f)",
        s.gameMs.sum / n, s.gameMs.max, s.tics.sum / n, s.drawMs.sum / n, s.drawMs.max, s.backendMs.sum / n, s.backendMs.max, s.syncMs.sum / n, s.syncMs.max, s.waitMs.sum / n, s.waitMs.max);
#if defined(__vita__)
    Bench_Log("bench   front end avg (worst) ms: find lights+entities %.1f (%.1f) | wait shadow jobs %.1f (%.1f) | lights %.1f (%.1f) | models %.1f (%.1f) | guis+sort %.1f (%.1f) | subviews %.1f (%.1f)",
        s.fe[0].sum / n, s.fe[0].max, s.fe[1].sum / n, s.fe[1].max, s.fe[2].sum / n, s.fe[2].max, s.fe[3].sum / n, s.fe[3].max, s.fe[4].sum / n, s.fe[4].max, s.fe[5].sum / n, s.fe[5].max);
    Bench_Log("bench   back end passes avg (worst) ms: views %.1f (%.1f) | depth pre-pass %.1f (%.1f) | interactions %.1f (%.1f) - stencil shadows %.1f (%.1f), light passes %.1f (%.1f) | shader passes %.1f (%.1f) | fog + blend lights %.1f (%.1f) | framebuffer copies %.1f (%.1f) | draw calls %.0f (%.0f)",
        s.rb[7].sum / n, s.rb[7].max, s.rb[0].sum / n, s.rb[0].max, s.rb[3].sum / n, s.rb[3].max, s.rb[1].sum / n, s.rb[1].max, s.rb[2].sum / n, s.rb[2].max,
        s.rb[4].sum / n, s.rb[4].max, s.rb[5].sum / n, s.rb[5].max, s.rb[6].sum / n, s.rb[6].max, s.rbDraws.sum / n, s.rbDraws.max);
    // the image quality passes (2x MSAA has no pass of its own: the GPU resolves
    // each tile as it writes it out - its cost is in the GPU wait, compared
    // between runs with vita_msaa 0 / 2)
    Bench_Log("bench   image quality passes avg (worst) ms, back end: 3D view stretch %.2f (%.2f) = soft alpha-tested edges' map %.2f (%.2f, %.1f surfaces a frame, in %.0f%% of the frames) + copy and sharpening pass %.2f | Toksvig during play %.1f ms in all (worst frame %.1f) | GPU/swap wait %.1f (%.1f) - the GPU's share, A/B: the verification's postcheck",
        s.rb[9].sum / n, s.rb[9].max, s.rb[8].sum / n, s.rb[8].max, s.alphaEdgeSurfaces.sum / n, 100.0 * s.alphaEdgeSurfaces.nonZero / n,
        Max(0.0, (s.rb[9].sum - s.rb[8].sum) / n), s.toksvigMs.sum, s.toksvigMs.max, s.syncMs.sum / n, s.syncMs.max);
    Bench_Log("bench   game systems avg (worst) ms: ragdolls %.1f (%.1f) | animation %.1f (%.1f) | decals %.1f (%.1f) | particles %.1f (%.1f) | GPU buffer copies %.1f (%.1f) | sound update (end of the game/draw thread's frame) %.1f (%.1f)",
        s.gm[0].sum / n, s.gm[0].max, s.gm[1].sum / n, s.gm[1].max, s.gm[2].sum / n, s.gm[2].max, s.gm[3].sum / n, s.gm[3].max, s.gm[4].sum / n, s.gm[4].max, s.gm[5].sum / n, s.gm[5].max);
    {
        int w = 0, h = 0;
        Vita_RenderSizeForStep(idMath::Ftoi(s.resStep.max), &w, &h);
        Bench_Log("bench   3D view: step avg %.2f, lowest size reached %dx%d (step %.0f) | texture quality drop at the end %d", s.resStep.sum / n, w, h, s.resStep.max, s.textureDropAtEnd);
    }
#endif
    {
        // the main thread's frame, phase by phase, largest first
        int order[BENCH_MAX_PHASES];
        for (int i = 0; i < s.numPhases; i++) {
            order[i] = i;
        }
        for (int i = 1; i < s.numPhases; i++) {
            const int v = order[i];
            int j = i - 1;
            while (j >= 0 && s.phaseUs[order[j]] < s.phaseUs[v]) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = v;
        }
        char line[1536];
        int len = idStr::snPrintf(line, sizeof(line), "bench   main thread per frame, avg (worst) ms:");
        double sum = 0.0;
        for (int i = 0; i < s.numPhases; i++) {
            const int k = order[i];
            sum += s.phaseUs[k];
            if (s.phaseUs[k] / n >= 50.0 && len < (int)sizeof(line) - 64) {
                len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.2f (%.1f) |", Bench_PhaseLabel(s.phaseNames[k]), s.phaseUs[k] / 1000.0 / n, s.phaseMax[k]);
            }
        }
        Bench_Log("%s = %.1f ms of the %.1f ms frame", line, sum / 1000.0 / n, s.ms.sum / n);
    }
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    Bench_PrintGDTree(s);
    if (s.worst.gdNumTop > 0) {
        char line[1536];
        int len = idStr::snPrintf(line, sizeof(line), "bench   slowest frame (%.1f ms), the game/draw thread's %.1f ms - costliest passes, own time:", s.worst.ms, s.worst.gdTotalMs);
        for (int i = 0; i < s.worst.gdNumTop; i++) {
            if (len < (int)sizeof(line) - 120) {
                len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.1f |", Bench_GDPath(s.worst.gdTop[i]), s.worst.gdTopMs[i]);
            }
        }
        Bench_Log("%s", line);
    }
#endif
    Bench_PrintJobTypes(s);
    if (s.gd != NULL) {
        Mem_Free(s.gd);
        s.gd = NULL;
    }
    Bench_Log("bench   scene avg (max): draws %.0f (%.0f) | tris %.0f (%.0f), in shadow volumes %.0f (%.0f) | lights %.1f (%.0f) | shadow volumes built %.1f (%.0f) | skinned verts %.0f (%.0f) | entities %.1f (%.0f) | views %.2f (%.0f) | new interactions %.1f (%.0f)",
        s.draws.sum / n, s.draws.max, s.tris.sum / n, s.tris.max, s.shadowTris.sum / n, s.shadowTris.max, s.lights.sum / n, s.lights.max,
        s.shadowVolumes.sum / n, s.shadowVolumes.max, s.skinnedVerts.sum / n, s.skinnedVerts.max, s.entities.sum / n, s.entities.max,
        s.views.sum / n, s.views.max, s.interactions.sum / n, s.interactions.max);
    Bench_Log("bench   loaded during the segment: %d images, %d sounds, %d models | frames over 40 ms: %d (%d while loading, %d with a job over 3 ms) | after teleports: %d settle frames, worst %.1f ms (not in the figures above)",
        s.loads[0], s.loads[1], s.loads[2], s.hitches, s.hitchesWithLoads, s.hitchesWithSlowJobs, s.settleFrames, s.settleWorst);
#if defined(__vita__)
    Bench_Log("bench   memory: free at the lowest %u KB (cached heap %u KB) | at the end %u KB (cached %u KB)", s.minFreeKB, s.minCachedKB, s.endFreeKB, s.endCachedKB);
#else
    Bench_Log("bench   memory (PC): process private %u KB at the end", s.endCachedKB);
#endif
    {
        char line[1024];
        int len = idStr::snPrintf(line, sizeof(line), "bench   jobs by thread:");
        for (int t = 0; t < 32; t++) {
            if (s.jobCount[t] > 0 && len < (int)sizeof(line) - 96) {
                len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %d jobs %.1f ms (%.2f ms a frame) |", Bench_ThreadName(t), s.jobCount[t], s.jobUs[t] / 1000.0, s.jobUs[t] / 1000.0 / n);
            }
        }
        Bench_Log("%s none on core 3 by design (only the log writer, the watchdog, savegame threads and the sound threads run there)", line);
    }
    {
        // who the waiting threads idled on once nothing was left to take
        char line[1024];
        int len = idStr::snPrintf(line, sizeof(line), "bench   job list tail waits (nothing left to take, the last jobs still running elsewhere), by the thread holding the oldest:");
        uint64 total = 0;
        for (int t = 0; t < 32; t++) {
            total += s.tailUs[t];
            if (s.tailCount[t] > 0 && len < (int)sizeof(line) - 96) {
                len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.2f ms a frame (%d waits, %.2f ms each) |", Bench_ThreadName(t), s.tailUs[t] / 1000.0 / n, s.tailCount[t],
                    s.tailUs[t] / 1000.0 / s.tailCount[t]);
            }
        }
        Bench_Log("%s total %.2f ms a frame", line, total / 1000.0 / n);
    }
    if (s.slowJobs > 0) {
        char line[1024];
        int len = idStr::snPrintf(line, sizeof(line), "bench   jobs over 3 ms: %d -", s.slowJobs);
        for (int i = 0; i < s.slowJobNames.Num(); i++) {
            if (len < (int)sizeof(line) - 80) {
                len += idStr::snPrintf(line + len, sizeof(line) - len, " '%s' x%d (worst %.1f ms) |", s.slowJobNames[i].c_str(), s.slowJobCounts[i], s.slowJobWorst[i] / 1000.0f);
            }
        }
        Bench_Log("%s (the shadow volume jobs among them are described one by one in the 'shadow volume job' lines)", line);
    }
    {
        idStr gameStats;
        Vita_BenchGameStats(gameStats);
        Bench_Log("bench   game state at the end: %s", gameStats.c_str());
    }
    idStr diag;
    Bench_Diagnose(s, diag);
    Bench_Log("bench   DIAGNOSIS: %s", diag.c_str());
#if defined(__vita__)
    // the perf block (common_frame.cpp) for exactly this segment: per class
    // thinks, zones, physics checks, threads and cores
    Vita_PerfFlush(va("benchmark segment %d \"%s\"", bench.done.Num() + 1, s.name.c_str()));
#elif defined(ID_VITA_PHYSCHECK)
    Bench_PcZoneReport(s.name.c_str(), s.frames);
#endif
    bench.done.Append(sp);
}

static void Bench_SecondLine(uint64 now)
{
    if (bench.secFrames > 0 && bench.seg != NULL) {
        const double n = bench.secFrames;
        unsigned int freeKB = 0, cachedKB = 0;
        Bench_Memory(freeKB, cachedKB);
#if defined(__vita__)
        const int step = vita_dynamicResolutionStep;
#else
        const int step = 0;
#endif
        Bench_Log("bench   t=%5.1fs: %4.1f fps, avg %.1f ms, worst %.1f | game %.1f front end %.1f back end %.1f GPU wait %.1f main waited %.1f | draws %.0f tris %.0fk (shadow %.0fk) lights %.1f | 3D step %d | %s %u MB (%s %u MB)",
            (now - bench.startUs) / 1000000.0, n * 1000000.0 / Max((uint64)1, now - bench.secStartUs), bench.secMs / n, bench.secWorst,
            bench.secGame / n, bench.secDraw / n, bench.secBackend / n, bench.secSync / n, bench.secWait / n,
            bench.secDraws / n, bench.secTris / n / 1000.0, bench.secShadowTris / n / 1000.0, bench.secLights / n, step,
#if defined(__vita__)
            "free", freeKB / 1024, "cached heap free", cachedKB / 1024);
#else
            "PC: n/a", freeKB / 1024, "process private", cachedKB / 1024);
#endif
    }
    bench.secStartUs = now;
    bench.secFrames = 0;
    bench.secWorst = 0.0f;
    bench.secMs = bench.secGame = bench.secDraw = bench.secBackend = bench.secSync = bench.secWait = 0.0;
    bench.secDraws = bench.secTris = bench.secShadowTris = bench.secLights = 0.0;
}

static void Bench_RecordFrame(const benchPerFrame_t& f, uint64 frameStartUs, uint64 now)
{
    benchSegment_t& s = *bench.seg;
    // the jobs over 3 ms that ended during this frame
    char jobsText[512];
    jobsText[0] = '\0';
    int jobsLen = 0, framesSlow = 0;
    const int next = vita_benchSlowJobNext.GetValue();
    for (int i = Max(bench.lastSlowJob, next - VITA_BENCH_SLOW_JOBS); i < next; i++) {
        const vitaBenchSlowJob_t& j = vita_benchSlowJobs[i & (VITA_BENCH_SLOW_JOBS - 1)];
        if (j.name == NULL) {
            continue;
        }
        framesSlow++;
        s.slowJobs++;
        const int k = s.slowJobNames.FindIndex(j.name);
        if (k < 0) {
            s.slowJobNames.Append(j.name);
            s.slowJobCounts.Append(1);
            s.slowJobWorst.Append(j.us);
        } else {
            s.slowJobCounts[k]++;
            s.slowJobWorst[k] = Max(s.slowJobWorst[k], j.us);
        }
        if (jobsLen < (int)sizeof(jobsText) - 96) {
            jobsLen += idStr::snPrintf(jobsText + jobsLen, sizeof(jobsText) - jobsLen, " '%s' %.1f ms on %s,", j.name, j.us / 1000.0f, Bench_ThreadName(j.thread));
        }
    }
    bench.lastSlowJob = next;

    const uint64 settle = bench.settleUntilUs;
    if (now < settle) {
        // just teleported: a burst of new interactions and areas, counted apart
        s.settleFrames++;
        s.settleWorst = Max(s.settleWorst, f.ms);
        return;
    }
    s.frames++;
    s.seconds += f.ms / 1000.0;
    if (f.ms > s.worst.ms) {
        s.worst = f;
    }
    for (int i = 0; i < f.numPhases; i++) {
        int k = 0;
        for (; k < s.numPhases; k++) {
            if (s.phaseNames[k] == f.phases[i].name) {
                break;
            }
        }
        if (k == s.numPhases) {
            if (s.numPhases >= BENCH_MAX_PHASES) {
                continue;
            }
            s.phaseNames[k] = f.phases[i].name;
            s.phaseUs[k] = 0.0;
            s.phaseMax[k] = 0.0f;
            s.numPhases++;
        }
        s.phaseUs[k] += (double)f.phases[i].us;
        s.phaseMax[k] = Max(s.phaseMax[k], f.phases[i].us / 1000.0f);
    }
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (f.gdTotalMs > 0.0f && s.gd != NULL) {
        s.gdFrames++;
        for (int i = 0; i < gdLastNumNodes; i++) {
            s.gd->excl[i] += (float)gdLastExcl[i];
            s.gd->incl[i] += (float)gdLastIncl[i];
            s.gd->calls[i] += (float)gdLastCalls[i];
            s.gd->inclMax[i] = Max(s.gd->inclMax[i], gdLastIncl[i] / 1000.0f);
        }
    }
#endif
    s.frameMs.Append(f.ms);
    const float b[6] = { BENCH_TARGET_MS, 36.0f, 40.0f, 50.0f, 66.7f, 100.0f };
    int bucket = 6;
    for (int i = 0; i < 6; i++) {
        if (f.ms <= b[i]) {
            bucket = i;
            break;
        }
    }
    s.buckets[bucket]++;
    for (int i = 0; i < BENCH_FLIP_BUCKETS; i++) {
        s.flips[i] += f.flips[i];
    }
    s.ms.Add(f.ms);
    s.gameMs.Add(f.gameMs);
    s.drawMs.Add(f.drawMs);
    s.backendMs.Add(f.backendMs);
    s.syncMs.Add(f.syncMs);
    s.waitMs.Add(f.waitMs);
    s.tics.Add((float)f.tics);
    for (int i = 0; i < 6; i++) {
        s.fe[i].Add(f.fe[i]);
    }
    for (int i = 0; i < 6; i++) {
        s.gm[i].Add(f.gm[i]);
    }
    for (int i = 0; i < 10; i++) {
        s.rb[i].Add(f.rb[i]);
    }
    s.alphaEdgeSurfaces.Add((float)f.alphaEdgeSurfaces);
    s.toksvigMs.Add(f.toksvigMs);
    s.rbDraws.Add((float)f.rbDraws);
    s.views.Add((float)f.scene.views);
    s.skinnedVerts.Add((float)f.scene.skinnedVerts);
    s.shadowVolumes.Add((float)f.scene.shadowVolumes);
    s.entities.Add((float)f.scene.entities);
    s.lights.Add((float)f.scene.lights);
    s.draws.Add((float)f.scene.draws);
    s.tris.Add((float)f.scene.tris);
    s.shadowTris.Add((float)f.scene.shadowTris);
    s.interactions.Add((float)f.scene.interactions);
    s.resStep.Add((float)f.resStep);
    for (int i = 0; i < 3; i++) {
        s.loads[i] += f.loads[i];
    }
#if defined(__vita__)
    s.minFreeKB = Min(s.minFreeKB, f.freeKB);
    s.minCachedKB = Min(s.minCachedKB, f.cachedKB);
#endif

    // per second
    if (bench.secStartUs == 0) {
        bench.secStartUs = frameStartUs;
    }
    bench.secFrames++;
    bench.secWorst = Max(bench.secWorst, f.ms);
    bench.secMs += f.ms;
    bench.secGame += f.gameMs;
    bench.secDraw += f.drawMs;
    bench.secBackend += f.backendMs;
    bench.secSync += f.syncMs;
    bench.secWait += f.waitMs;
    bench.secDraws += f.scene.draws;
    bench.secTris += f.scene.tris;
    bench.secShadowTris += f.scene.shadowTris;
    bench.secLights += f.scene.lights;
    if (now - bench.secStartUs >= 1000000) {
        Bench_SecondLine(now);
    }

    // a frame over 40 ms: all of it
    if (f.ms > BENCH_HITCH_MS) {
        s.hitches++;
        const bool loading = (f.loads[0] + f.loads[1] + f.loads[2]) > 0;
        if (loading) {
            s.hitchesWithLoads++;
        }
        if (framesSlow > 0) {
            s.hitchesWithSlowJobs++;
        }
        if (s.hitches <= 60) {
            Bench_Log("bench   HITCH %d at t=%.2fs: frame %.1f ms = game %.1f (%d tics) + front end %.1f on the game/draw thread | back end %.1f | GPU/swap wait %.1f | main thread waited %.1f for game/draw",
                s.hitches, (now - bench.startUs) / 1000000.0, f.ms, f.gameMs, f.tics, f.drawMs, f.backendMs, f.syncMs, f.waitMs);
#if defined(__vita__)
            Bench_Log("bench     front end: lights+entities %.1f | wait shadow jobs %.1f | lights %.1f | models %.1f | guis+sort %.1f | subviews %.1f || game: ragdolls %.1f | animation %.1f | decals %.1f | particles %.1f | GPU buffer copies %.1f || sound update after the front end %.1f",
                f.fe[0], f.fe[1], f.fe[2], f.fe[3], f.fe[4], f.fe[5], f.gm[0], f.gm[1], f.gm[2], f.gm[3], f.gm[4], f.gm[5]);
#endif
            Bench_Log("bench     back end passes: views %.1f of the back end's %.1f = depth pre-pass %.1f | interactions %.1f (stencil shadows %.1f, light passes %.1f, set up %.1f) | shader passes %.1f | fog + blend lights %.1f | framebuffer copies %.1f | rest of the views %.1f | 3D view stretch %.2f (soft edges' map %.2f) || %d draw calls (%.1f us each over the views)",
                f.rb[7], f.backendMs, f.rb[0], f.rb[3], f.rb[1], f.rb[2], Max(0.0f, f.rb[3] - f.rb[1] - f.rb[2]), f.rb[4], f.rb[5], f.rb[6],
                Max(0.0f, f.rb[7] - f.rb[0] - f.rb[3] - f.rb[4] - f.rb[5] - f.rb[6]), f.rb[9], f.rb[8], f.rbDraws, f.rbDraws > 0 ? 1000.0f * f.rb[7] / f.rbDraws : 0.0f);
            Bench_Log("bench     scene: %d draws, %d tris (%d shadow), %d lights, %d shadow volumes, %d skinned verts, %d entities, %d views, %d new interactions | loads: %d images %d sounds %d models | 3D step %d | free %u KB (cached %u KB)",
                f.scene.draws, f.scene.tris, f.scene.shadowTris, f.scene.lights, f.scene.shadowVolumes, f.scene.skinnedVerts, f.scene.entities, f.scene.views,
                f.scene.interactions, f.loads[0], f.loads[1], f.loads[2], f.resStep, f.freeKB, f.cachedKB);
            {
                // where the main thread's frame went, phase by phase (over 0.5 ms)
                char line[1024];
                int len = idStr::snPrintf(line, sizeof(line), "bench     main thread:");
                for (int i = 0; i < f.numPhases; i++) {
                    if (f.phases[i].us >= 500 && len < (int)sizeof(line) - 48) {
                        len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.1f |", Bench_PhaseLabel(f.phases[i].name), f.phases[i].us / 1000.0f);
                    }
                }
                Bench_Log("%s", line);
            }
            if (f.gdNumTop > 0) {
                char line[1536];
                int len = idStr::snPrintf(line, sizeof(line), "bench     game/draw thread %.1f ms, costliest passes (own time):", f.gdTotalMs);
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                for (int i = 0; i < f.gdNumTop; i++) {
                    if (len < (int)sizeof(line) - 120) {
                        len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %.1f |", Bench_GDPath(f.gdTop[i]), f.gdTopMs[i]);
                    }
                }
#endif
                Bench_Log("%s", line);
            }
            if (framesSlow > 0) {
                Bench_Log("bench     jobs over 3 ms that ended in this frame:%s", jobsText);
            }
        } else if (s.hitches == 61) {
            Bench_Log("bench   (more hitches in this segment are counted, not listed)");
        }
    }
}

/*
================================================================================

The script

================================================================================
*/
static void Bench_Finish(bool aborted);
// the finish screen (VitaVerify.cpp)
void VitaVerify_BannerClear();
void VitaVerify_BannerAdd(int color, const char* fmt, ...);
void VitaVerify_BannerShow();

// the verification's steps (VitaVerify.cpp): 0 not one of its commands, 1 done,
// 2 running (VitaVerify_Poll until it returns true)
int VitaVerify_Op(const idCmdArgs& args, int lineNum);
bool VitaVerify_Poll(uint64 now);
void VitaVerify_Report();
void VitaVerify_Begin(bool resume);
int VitaVerify_ResumeLine(int numLines);
void VitaVerify_LineStarted(int lineNum, const char* text);

static void Bench_SetInput(int buttons, int forward, int right, float yawPerMs)
{
    bench.input.buttons = buttons;
    bench.input.forward = forward;
    bench.input.right = right;
    bench.input.yawPerMs = yawPerMs;
}

static void Bench_StartTimed(const char* what, int ms, bool measured)
{
    bench.state = BENCH_TIMED;
    bench.timedEndUs = Sys_Microseconds() + (uint64)Max(0, ms) * 1000;
    bench.timedWhat = what;
    bench.measuring = measured;
}

// runs commands until one takes time
static void Bench_Step()
{
    while (bench.state == BENCH_RUN) {
        if (bench.pc >= bench.lines.Num()) {
            Bench_Finish(false);
            return;
        }
        const int lineNum = bench.pc + 1;
        idStr line = bench.lines[bench.pc++];
        const int comment = line.Find("//");
        if (comment >= 0) {
            line = line.Left(comment);
        }
        line.StripLeading(' ');
        line.StripLeading('\t');
        line.StripTrailingWhitespace();
        if (line.IsEmpty()) {
            continue;
        }
        idCmdArgs args;
        args.TokenizeString(line.c_str(), false);
        if (args.Argc() == 0) {
            continue;
        }
        if (bench.verifyMode) {
            VitaVerify_LineStarted(lineNum, line.c_str());
        }
        const char* op = args.Argv(0);
        if (!idStr::Icmp(op, "title") || !idStr::Icmp(op, "log")) {
            Bench_Log("bench %s", args.Args());
        } else if (!idStr::Icmp(op, "map")) {
            Bench_CloseSegment();
            Bench_SetInput(0, 0, 0, 0.0f);
            bench.spawned.Clear();
            bench.mapName = args.Argv(1);
            bench.mapSettleMs = (args.Argc() > 2) ? atoi(args.Argv(2)) : 4000;
            bench.mapRequestUs = Sys_Microseconds();
            bench.mapReadyUs = 0;
            bench.mapFrames = 0;
            bench.mapLoaded = false;
            bench.mapWaitWhy = "loading";
            Bench_Log("bench ==== map %s: loading (devmap) at %.1f s ====", bench.mapName.c_str(), (bench.mapRequestUs - bench.startUs) / 1000000.0);
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("devmap %s\n", bench.mapName.c_str()));
            bench.state = BENCH_WAIT_MAP;
        } else if (!idStr::Icmp(op, "segment")) {
            Bench_CloseSegment();
            Bench_OpenSegment(args.Args());
        } else if (!idStr::Icmp(op, "view")) {
            const float eye = cvarSystem->GetCVarFloat("pm_normalviewheight") - 0.25f;
            const float x = atof(args.Argv(1)), y = atof(args.Argv(2)), z = atof(args.Argv(3)), yaw = atof(args.Argv(4));
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("setviewpos %.1f %.1f %.1f %.1f\n", x, y, z + eye, yaw));
            bench.settleUntilUs = Sys_Microseconds() + 700 * 1000;
            Bench_Log("bench   view (%.0f %.0f %.0f) yaw %.0f - the next 0.7 s are settle frames", x, y, z, yaw);
        } else if (!idStr::Icmp(op, "hold")) {
            Bench_SetInput(0, 0, 0, 0.0f);
            Bench_StartTimed(va("hold %s ms", args.Argv(1)), atoi(args.Argv(1)), true);
        } else if (!idStr::Icmp(op, "wait")) {
            Bench_SetInput(0, 0, 0, 0.0f);
            Bench_StartTimed(va("wait %s ms (not measured)", args.Argv(1)), atoi(args.Argv(1)), false);
        } else if (!idStr::Icmp(op, "turn")) {
            const float deg = atof(args.Argv(1));
            const int ms = Max(1, atoi(args.Argv(2)));
            Bench_SetInput(0, 0, 0, deg / ms);
            Bench_StartTimed(va("turn %.0f degrees in %d ms", deg, ms), ms, true);
        } else if (!idStr::Icmp(op, "walk")) {
            const int ms = atoi(args.Argv(1));
            Bench_SetInput(0, 127, 0, 0.0f);
            Bench_StartTimed(va("walk %d ms", ms), ms, true);
        } else if (!idStr::Icmp(op, "fire")) {
            const int ms = atoi(args.Argv(1));
            const float yaw = (args.Argc() > 3) ? atof(args.Argv(2)) / Max(1, atoi(args.Argv(3))) : 0.0f;
            Bench_SetInput(BUTTON_ATTACK, 0, 0, yaw);
            Bench_StartTimed(va("fire %d ms", ms), ms, true);
        } else if (!idStr::Icmp(op, "weapon")) {
            bench.input.impulse = atoi(args.Argv(1));
            Bench_Log("bench   weapon %d", bench.input.impulse);
        } else if (!idStr::Icmp(op, "flashlight")) {
            bench.input.impulse = IMPULSE_16;
            Bench_Log("bench   flashlight toggled");
        } else if (!idStr::Icmp(op, "spawn")) {
            const idStr name = va("bench_monster_%d", ++bench.spawnSerial);
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("spawn %s name %s origin \"%s %s %s\" angle %s\n",
                args.Argv(1), name.c_str(), args.Argv(2), args.Argv(3), args.Argv(4), args.Argc() > 5 ? args.Argv(5) : "0"));
            bench.spawned.Append(name);
            Bench_Log("bench   spawn %s '%s' at (%s %s %s)", args.Argv(1), name.c_str(), args.Argv(2), args.Argv(3), args.Argv(4));
        } else if (!idStr::Icmp(op, "kill")) {
            int killed = 0, dead = 0;
            for (int i = 0; i < bench.spawned.Num(); i++) {
                const int r = Vita_BenchKill(bench.spawned[i].c_str());
                killed += (r == 1);
                dead += (r == 0);
            }
            Bench_Log("bench   kill: of %d spawned monsters, %d were still alive (killed now), %d already dead, %d gone (burned away)", bench.spawned.Num(), killed, dead, bench.spawned.Num() - killed - dead);
        } else if (!idStr::Icmp(op, "remove")) {
            int removed = 0;
            for (int i = 0; i < bench.spawned.Num(); i++) {
                removed += Vita_BenchRemove(bench.spawned[i].c_str()) ? 1 : 0;
            }
            Bench_Log("bench   remove: %d corpses removed (%d of the %d spawned had already gone)", removed, bench.spawned.Num() - removed, bench.spawned.Num());
            bench.spawned.Clear();
        } else if (!idStr::Icmp(op, "cmd")) {
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("%s\n", args.Args()));
            Bench_Log("bench   console: %s", args.Args());
        } else if (!idStr::Icmp(op, "end")) {
            Bench_Finish(false);
            return;
        } else if (const int v = VitaVerify_Op(args, lineNum)) {
            if (v == 2) {
                bench.state = BENCH_VERIFY; // VitaVerify_Poll moves it on
            }
        } else {
            bench.errors++;
            Bench_Log("bench   SCRIPT ERROR line %d: unknown command '%s' (skipped)", lineNum, op);
        }
    }
}

static void Bench_WaitMap(uint64 now)
{
    bench.mapFrames++;
    idStr why;
    const bool spawned = commonLocal.GetSpawnedMapName() != NULL && bench.mapName.Icmp(commonLocal.GetSpawnedMapName()) == 0;
    const bool ready = spawned && bench.mapFrames > 2 && Vita_BenchGameReady(why);
    if (!spawned) {
        why = "loading";
    }
    if (why != bench.mapWaitWhy) {
        Bench_Log("bench   map %s: %s (%.1f s after the request)", bench.mapName.c_str(), why.IsEmpty() ? "ready" : why.c_str(), (now - bench.mapRequestUs) / 1000000.0);
        bench.mapWaitWhy = why;
    }
    if (!ready) {
        if (now - bench.mapRequestUs > 240u * 1000000u) {
            bench.errors++;
            Bench_Log("bench   ERROR: map %s not playable after 240 s (%s) - skipping to the next map", bench.mapName.c_str(), why.c_str());
            // skip this map's commands
            while (bench.pc < bench.lines.Num()) {
                idStr l = bench.lines[bench.pc];
                l.StripLeading(' ');
                if (!idStr::Icmpn(l, "map ", 4) || !idStr::Icmpn(l, "end", 3)) {
                    break;
                }
                bench.pc++;
            }
            bench.state = BENCH_RUN;
        }
        return;
    }
    if (!bench.mapLoaded) {
        bench.mapLoaded = true;
        bench.mapReadyUs = now;
        Vita_BenchPlayerSetup(true, false, false);
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "give all\n");
        unsigned int freeKB = 0, cachedKB = 0;
        Bench_Memory(freeKB, cachedKB);
        Bench_Log("bench   map %s playable %.1f s after the request; god mode, all weapons; free %u KB (cached %u KB); settling %d ms before measuring",
            bench.mapName.c_str(), (now - bench.mapRequestUs) / 1000000.0, freeKB, cachedKB, bench.mapSettleMs);
#if defined(__vita__)
        Vita_LogMemory(va("benchmark: %s loaded", bench.mapName.c_str()));
#endif
    }
    if (now - bench.mapReadyUs >= (uint64)bench.mapSettleMs * 1000) {
        bench.state = BENCH_RUN;
    }
}

void VitaBench_ApplyInput(usercmd_t& cmd, idVec3& viewangles, int msec)
{
    if (bench.state == BENCH_OFF) {
        return;
    }
    if (bench.input.buttons) {
        cmd.buttons |= bench.input.buttons;
    }
    if (bench.input.forward) {
        cmd.forwardmove = (signed char)bench.input.forward;
    }
    if (bench.input.right) {
        cmd.rightmove = (signed char)bench.input.right;
    }
    if (bench.lookSet) {
        viewangles[YAW] = bench.lookYaw;
        viewangles[PITCH] = bench.lookPitch;
    }
    if (bench.input.yawPerMs != 0.0f) {
        viewangles[YAW] += bench.input.yawPerMs * msec;
    }
    if (bench.input.impulse >= 0) {
        cmd.impulse = (byte)bench.input.impulse;
        cmd.impulseSequence++;
        bench.input.impulse = -1;
    }
}

// the verification's handles on the script (VitaVerify.cpp)
void VitaBench_Memory(unsigned int& freeKB, unsigned int& cachedKB)
{
    Bench_Memory(freeKB, cachedKB);
}

const char* VitaBench_Line(int i)
{
    return (i >= 0 && i < bench.lines.Num()) ? bench.lines[i].c_str() : "";
}

void VitaBench_SetInput(int buttons, int forward, int right, float yawPerMs)
{
    Bench_SetInput(buttons, forward, right, yawPerMs);
}

void VitaBench_SetImpulse(int impulse)
{
    bench.input.impulse = impulse;
}

void VitaBench_SetLook(bool on, float yaw, float pitch)
{
    bench.lookSet = on;
    bench.lookYaw = yaw;
    bench.lookPitch = pitch;
}

void VitaBench_OpenSegment(const char* name)
{
    Bench_CloseSegment();
    Bench_OpenSegment(name);
}

void VitaBench_CloseSegment()
{
    Bench_CloseSegment();
}

void VitaBench_SetMeasuring(bool on)
{
    bench.measuring = on;
}

// the verification's own work (a screenshot encoded and written, ~300 ms)
// is in the next frame's time: that frame is left out of the figures
static bool benchSkipNextFrame = false;
static int benchSkippedFrames = 0;
void VitaBench_SkipFrame()
{
    benchSkipNextFrame = true;
}

void VitaBench_SetMapName(const char* name)
{
    bench.mapName = name;
    bench.spawned.Clear();
}

void VitaBench_AddSpawned(const char* name)
{
    bench.spawned.Append(name);
}

bool VitaBench_VerifyMode()
{
    return bench.state != BENCH_OFF && bench.verifyMode;
}

double VitaBench_Seconds()
{
    return (Sys_Microseconds() - bench.startUs) / 1000000.0;
}

bool VitaBench_Active()
{
    return bench.state != BENCH_OFF;
}

#if !defined(__vita__)
static void Bench_ScanFrame(uint64 now);
#endif

/*
================
VitaBench_Frame

Once a frame, on the main thread after the game/draw thread finished
(common_frame.cpp): measures the frame, then moves the script on.
================
*/
// the last frame's front end (draw) and back end, for the verification's
// shadow check (its precise inside test on / off)
static uint64 benchLastDrawUs = 0, benchLastBackendUs = 0;
void VitaBench_LastFrame(uint64* drawUs, uint64* backendUs)
{
    *drawUs = benchLastDrawUs;
    *backendUs = benchLastBackendUs;
}

// the last frame's wait for the GPU (the verification's GPU cost checks)
static uint64 benchLastSyncUs = 0;
uint64 VitaBench_LastFrameGpuWait()
{
    return benchLastSyncUs;
}

void VitaBench_Frame(uint64 gameUs, uint64 drawUs, uint64 backendUs, uint64 syncUs, uint64 waitUs, int tics)
{
    benchLastDrawUs = drawUs;
    benchLastBackendUs = backendUs;
    benchLastSyncUs = syncUs;
    extern void VitaVerify_Idle();
    VitaVerify_Idle(); // the finish screen's screenshot
#if defined(__vita__)
    if (gdMainToken == 0 && !gdTokenBySyscall) {
        gdMainToken = Bench_ThreadToken(); // the main thread (game/draw runs elsewhere)
    }
#endif
    if (bench.state == BENCH_OFF) {
        return;
    }
#if !defined(__vita__)
    extern void Vita_ShadowCalibPrint();
    Vita_ShadowCalibPrint();
#endif
    const uint64 now = Sys_Microseconds();
    const uint64 frameStart = bench.lastFrameUs;
    bench.lastFrameUs = now;
#if !defined(__vita__)
    if (bench.state == BENCH_SCAN) {
        Bench_ScanFrame(now);
        return;
    }
#endif
    // this frame's figures (the running counters as deltas)
    benchPerFrame_t f;
    memset(&f, 0, sizeof(f));
    f.ms = frameStart ? (now - frameStart) / 1000.0f : 0.0f;
    f.gameMs = gameUs / 1000.0f;
    f.drawMs = drawUs / 1000.0f;
    f.backendMs = backendUs / 1000.0f;
    f.syncMs = syncUs / 1000.0f;
    f.waitMs = waitUs / 1000.0f;
    f.tics = tics;
    f.scene = vita_benchScene;
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    {
        extern uint64 vita_rbPassUs[10]; // tr_backend_draw.cpp
        extern int vita_rbDrawCalls;
        extern int r_vitaAlphaEdgeSurfaces;
        extern uint64 R_ToksvigTakePlayUs();
        f.alphaEdgeSurfaces = r_vitaAlphaEdgeSurfaces;
        f.toksvigMs = R_ToksvigTakePlayUs() / 1000.0f;
        for (int i = 0; i < 10; i++) {
            f.rb[i] = vita_rbPassUs[i] / 1000.0f;
            vita_rbPassUs[i] = 0;
        }
        f.rbDraws = vita_rbDrawCalls;
        vita_rbDrawCalls = 0;
    }
#endif
#if defined(__vita__)
    for (int i = 0; i < 6; i++) {
        const uint64 v = vita_frontEndUs[i];
        f.fe[i] = (v >= bench.lastFe[i] ? v - bench.lastFe[i] : v) / 1000.0f;
        bench.lastFe[i] = v;
    }
    const int gm[6] = { vita_afUs, vita_animUs, vita_decalUs, vita_prtUs, vita_vcCopyUs, vita_soundUs };
    for (int i = 0; i < 6; i++) {
        f.gm[i] = (gm[i] >= bench.lastGm[i] ? gm[i] - bench.lastGm[i] : gm[i]) / 1000.0f;
        bench.lastGm[i] = gm[i];
    }
    f.resStep = vita_dynamicResolutionStep;
#endif
    for (int i = 0; i < 3; i++) {
        f.loads[i] = vita_benchLoads[i] - bench.lastLoads[i];
        bench.lastLoads[i] = vita_benchLoads[i];
    }
    Bench_Memory(f.freeKB, f.cachedKB);
#if defined(__vita__)
    f.numFlips = Bench_TakeFlips(f.flips);
#endif
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    // the game/draw thread's frame that just finished (it is idle now)
    if (gdLastValid) {
        gdLastValid = false;
        f.gdTotalMs = gdLastIncl[0] / 1000.0f;
        for (int i = 0; i < gdLastNumNodes; i++) {
            const float ms = gdLastExcl[i] / 1000.0f;
            if (ms < 0.3f) {
                continue;
            }
            int k = f.gdNumTop;
            if (k < 12) {
                f.gdNumTop++;
            } else if (ms <= f.gdTopMs[11]) {
                continue;
            } else {
                k = 11;
            }
            while (k > 0 && f.gdTopMs[k - 1] < ms) {
                f.gdTop[k] = f.gdTop[k - 1];
                f.gdTopMs[k] = f.gdTopMs[k - 1];
                k--;
            }
            f.gdTop[k] = (short)i;
            f.gdTopMs[k] = ms;
        }
    }
#endif
    // the main thread's phases since the last frame (closing the current one here)
    VitaBench_Phase(benchPhaseCur != NULL ? benchPhaseCur : "frame: benchmark");
    f.numPhases = benchNumPhases;
    for (int i = 0; i < benchNumPhases; i++) {
        f.phases[i] = benchPhases[i];
        benchPhases[i].us = 0;
    }

    if (benchSkipNextFrame) {
        benchSkipNextFrame = false;
        benchSkippedFrames++;
        bench.lastSlowJob = vita_benchSlowJobNext.GetValue();
    } else if ((bench.state == BENCH_TIMED || bench.state == BENCH_VERIFY) && bench.measuring && bench.seg != NULL && frameStart != 0) {
        Bench_RecordFrame(f, frameStart, now);
    } else {
        bench.lastSlowJob = vita_benchSlowJobNext.GetValue();
    }

    if (bench.state == BENCH_WAIT_MAP) {
        Bench_WaitMap(now);
    }
    if (bench.state == BENCH_TIMED && now >= bench.timedEndUs) {
        Bench_SetInput(0, 0, 0, 0.0f);
        bench.state = BENCH_RUN;
    }
    // the test's own work, as phases of their own in the main thread's line
    // (run 7: "benchmark" took 1.5 ms a frame on average, 7 ms in fights,
    // inside the frame times - now split: frame statistics, the
    // verification's step, the script)
    static const char* phaseVerify = "frame: benchmark: verification step";
    static const char* phaseScript = "frame: benchmark: script";
    static const char* phaseStats = "frame: benchmark: rest"; // (a literal of its own: phases match by pointer)
    if (bench.state == BENCH_VERIFY) {
        VitaBench_Phase(phaseVerify);
        if (VitaVerify_Poll(now)) {
            bench.measuring = false;
            bench.state = BENCH_RUN;
        }
    }
    if (bench.state == BENCH_RUN) {
        VitaBench_Phase(phaseScript);
        Bench_Step();
    }
    VitaBench_Phase(phaseStats);
}

static void Bench_Header()
{
    Bench_Log("bench ================================================================");
    Bench_Log("bench DOOM 3 BFG benchmark - script '%s', %d lines", bench.scriptName.c_str(), bench.lines.Num());
    Bench_Log("bench build %s %s, %s", __DATE__, __TIME__,
#if defined(__vita__)
        "PS Vita"
#else
        "PC test build (timings are the PC's: compare the scene counts and the script, not the milliseconds)"
#endif
    );
    Bench_Log("bench target: every frame within %.1f ms (30 fps); frames over %.0f ms are listed one by one; figures per segment of the script, per second, and per hitch", BENCH_TARGET_MS, BENCH_HITCH_MS);
    {
        // what the measuring itself costs: every timed zone reads the clock
        // twice, and the game/draw thread's tree adds a lookup per pass
        const uint64 t0 = Sys_Microseconds();
        uint64 sink = 0;
        for (int i = 0; i < 2000; i++) {
            sink += Sys_Microseconds();
        }
        const uint64 t1 = Sys_Microseconds();
        Bench_Log("bench instrumentation: %.2f us per clock read (each timed zone or pass costs about two)%s", (t1 - t0) / 2000.0, sink == 0 ? " " : "");
#if defined(__vita__)
        // the other clocks a zone could read, and what a whole zone costs as
        // the game uses them (on this thread: no game/draw tree node)
        uint32 sink32 = 0;
        const uint64 c0 = Sys_Microseconds();
        for (int i = 0; i < 2000; i++) {
            sink32 += sceKernelGetProcessTimeLow();
        }
        const uint64 c1 = Sys_Microseconds();
        for (int i = 0; i < 2000; i++) {
            sink += (uint64)sceKernelGetSystemTimeWide();
        }
        const uint64 c2 = Sys_Microseconds();
        for (int i = 0; i < 2000; i++) {
            VITA_ZONE("bench: an empty timed zone (instrumentation test, 2000 at the start)");
        }
        const uint64 c3 = Sys_Microseconds();
        for (int i = 0; i < 2000; i++) {
            VITA_ZONE_COUNT("bench: an empty counted zone (instrumentation test, 2000 at the start)");
        }
        const uint64 c4 = Sys_Microseconds();
        Bench_Log("bench instrumentation, other clocks: sceKernelGetProcessTimeLow %.2f us, sceKernelGetSystemTimeWide %.2f us per read | an empty timed zone %.2f us, an empty counted zone %.3f us%s",
            (c1 - c0) / 2000.0, (c2 - c1) / 2000.0, (c3 - c2) / 2000.0, (c4 - c3) / 2000.0, (sink32 == 0 && sink == 0) ? " " : "");

        // what limits the CPU skinning (~0.7 us a vertex in the fights): the
        // same NEON skin over 256 vertices that stay in the cache, run 128
        // times, then over 32768 (1 MB in, 1 MB out) that do not; and a 1 MB
        // memcpy for the memory's own speed
        {
            const int bigVerts = 32768, smallVerts = 256, numJoints = 64;
            idDrawVert* base = (idDrawVert*)Mem_Alloc16(bigVerts * sizeof(idDrawVert), TAG_TEMP);
            idDrawVert* out = (idDrawVert*)Mem_Alloc16(bigVerts * sizeof(idDrawVert), TAG_TEMP);
            idJointMat* jm = (idJointMat*)Mem_Alloc16(numJoints * sizeof(idJointMat), TAG_TEMP);
            if (base != NULL && out != NULL && jm != NULL) {
                for (int j = 0; j < numJoints; j++) {
                    jm[j].SetRotation(idAngles(j * 5.0f, j * 3.0f, j * 7.0f).ToMat3());
                    jm[j].SetTranslation(idVec3(j * 1.5f, j * -0.5f, j * 2.0f));
                }
                for (int i = 0; i < bigVerts; i++) {
                    base[i].Clear();
                    base[i].xyz.Set((float)(i & 255), (float)((i >> 8) & 127), (float)(i % 97));
                    base[i].SetNormal(0.0f, 0.6f, 0.8f);
                    base[i].SetTangent(0.8f, -0.6f, 0.0f);
                    const dword j0 = i % numJoints, j1 = (i * 7) % numJoints, j2 = (i * 13) % numJoints, j3 = (i * 29) % numJoints;
                    base[i].SetColor(j0 | (j1 << 8) | (j2 << 16) | (j3 << 24));
                    base[i].SetColor2(128 | (64 << 8) | (40 << 16) | (23 << 24));
                }
                memcpy(out, base, bigVerts * sizeof(idDrawVert));
                const uint64 s0 = Sys_Microseconds();
                for (int r = 0; r < 128; r++) {
                    TransformVertsAndTangents(out, smallVerts, base, jm);
                }
                const uint64 s1 = Sys_Microseconds();
                TransformVertsAndTangents(out, bigVerts, base + 0, jm);
                const uint64 s2 = Sys_Microseconds();
                memcpy(out, base, bigVerts * sizeof(idDrawVert));
                const uint64 s3 = Sys_Microseconds();
                Bench_Log("bench skinning test: %.0f ns a vertex in the cache, %.0f ns out of it (1 MB in, 1 MB out) | a 1 MB memcpy: %.0f MB/s",
                    (s1 - s0) * 1000.0 / (128.0 * smallVerts), (s2 - s1) * 1000.0 / bigVerts,
                    (s3 > s2) ? (bigVerts * sizeof(idDrawVert)) / (double)(s3 - s2) : 0.0);
            }
            Mem_Free16(base);
            Mem_Free16(out);
            Mem_Free16(jm);
        }
#endif
    }
    static const char* cvars[] = { "vita_gameHz", "vita_maxFPS", "vita_displayPacing", "r_swapInterval", "vita_gpuSync", "vita_soundOnGameThread", "r_useStencilShadowPreload", "vita_prelightInsideTest", "vita_dynamicResolution", "vita_renderResolution", "vita_afMaxMoveTime",
        "r_useShadowPreciseInsideTest", "r_cullDynamicLightTriangles", "r_cullDynamicShadowTriangles", "r_useLightDepthBounds", "r_useShadowDepthBounds",
        "r_skipDynamicShadows", "r_skipStaticShadows", "s_maxEmitterChannels", "jobs_numThreads", "com_smp", "com_engineHz", "com_deltaTimeClamp",
        "g_decals", "g_bloodEffects", "r_flareSize", "vita_toggleCrouch",
        "vita_maxTicsPerFrame", "vita_animLOD", "vita_animNearDistance", "vita_animFocusAngle", "vita_guiLOD", "vita_hudHalfRate", "vita_animatedShadowLights", "vita_afFastTree", "vita_frameAllocChunks",
        "vita_tuningVersion", "vita_guiNearDistance", "vita_guiFocusAngle", "vita_aiFarDistance", "vita_ikDistance", "vita_soundHalfRate", "vita_animatedShadowDistance", "vita_animatedLightLimit", "vita_aiScriptHalfRateDistance", "vita_lightMinScreenArea", "vita_skinUnitNormals", "vita_aiHalfRateThinkDistance", "vita_afHalfRate", "vita_debrisClipBox", "vita_particleThin", "vita_lightMinScreenAreaPerLoadStep", "vita_guiFarDistance", "vita_aasNearestGoalMaxTests", "vita_aiMoveSearchRetryMs", "vita_renderResolution", "vita_dynamicResolution", "vita_lightCapAuto", "vita_debrisHalfRate", "vita_debrisMaxMoveMs", "vita_debrisTranslationSweep", "vita_afMaxPerTic", "vita_overlaysPerFrame", "vita_afContactsPerBody", "vita_screenCopyReuse2", "vita_jobWorkerCores", "vita_fxaa", "vita_msaa", "vita_postFilter", "vita_postFilterPlain", "vita_sharpen", "vita_sharpenClamp", "vita_softAlphaEdges", "r_toksvig", "r_toksvigJobs", "vita_toksvigShader", NULL };
    char line[4096]; // (every setting: dropped ones were cut off at 1 KB)
    int len = idStr::snPrintf(line, sizeof(line), "bench settings:");
    for (int i = 0; cvars[i] != NULL; i++) {
        idCVar* cv = cvarSystem->Find(cvars[i]);
        if (cv != NULL && len < (int)sizeof(line) - 64) {
            len += idStr::snPrintf(line + len, sizeof(line) - len, " %s %s |", cvars[i], cv->GetString());
        }
    }
    Bench_Log("%s", line);
    {
        // the image quality features in one line (their costs: the segments'
        // "image quality passes" lines, the verification's postcheck)
        extern bool r_vitaPostFilterBuilt;
        idCVar* tk = cvarSystem->Find("vita_toksvigShader");
        Bench_Log("bench image quality: anti-aliasing %dx MSAA (vita_msaa) | Toksvig: normal maps filtered at load %s, highlight shader %s | 3D view stretch: %s - sharpening %.2f (clamp %.3f), soft alpha-tested edges %.2f",
            cvarSystem->GetCVarInteger("vita_msaa"), cvarSystem->GetCVarBool("r_toksvig") ? "on" : "OFF",
            tk == NULL ? "on" : (tk->GetBool() ? "on" : "OFF (fixed exponent)"),
            !r_vitaPostFilterBuilt ? "plain (filter not built)" : (cvarSystem->GetCVarBool("vita_postFilterPlain") ? "plain (vita_postFilterPlain)" : "filtered"),
            cvarSystem->GetCVarFloat("vita_sharpen"), cvarSystem->GetCVarFloat("vita_sharpenClamp"), cvarSystem->GetCVarFloat("vita_softAlphaEdges"));
    }
#if defined(__vita__)
    Vita_BenchEnvironment();
#endif
    Bench_Log("bench ================================================================");
}

static void Bench_Finish(bool aborted)
{
    Bench_CloseSegment();
    Bench_SetInput(0, 0, 0, 0.0f);
    bench.input.impulse = -1;
    const uint64 now = Sys_Microseconds();
    Bench_Log("bench ================================================================");
    Bench_Log("bench %s after %.1f s, %d segments, %d script errors", aborted ? "ABORTED" : "FINISHED", (now - bench.startUs) / 1000000.0, bench.done.Num(), bench.errors);
    Bench_Log("bench results by segment (fps | %% of frames on target | 99th percentile ms | worst ms | game + front end ms | back end | GPU wait | min free MB / cached MB | hitches):");
    int totalFrames = 0, totalOn = 0, totalHitches = 0;
    float worst = 0.0f;
    unsigned int minFree = 0xFFFFFFFFu, minCached = 0xFFFFFFFFu;
    idStr lastMap;
    for (int i = 0; i < bench.done.Num(); i++) {
        benchSegment_t& s = *bench.done[i];
        const double n = Max(1, s.frames);
        idList<float> sorted = s.frameMs;
        const float p99 = Bench_Percentile(sorted, 0.99f);
        if (s.map != lastMap) {
            Bench_Log("bench   [%s]", s.map.c_str());
            lastMap = s.map;
        }
        Bench_Log("bench   %2d %-64s %5.1f fps | %5.1f%% | %5.1f | %5.1f | %4.1f + %4.1f | %4.1f | %4.1f | %u / %u | %d",
            i + 1, s.name.c_str(), s.seconds > 0.0 ? s.frames / s.seconds : 0.0, 100.0 * s.buckets[0] / n, p99, s.ms.max,
            s.gameMs.sum / n, s.drawMs.sum / n, s.backendMs.sum / n, s.syncMs.sum / n,
            s.minFreeKB == 0xFFFFFFFFu ? 0 : s.minFreeKB / 1024, s.minCachedKB == 0xFFFFFFFFu ? 0 : s.minCachedKB / 1024, s.hitches);
        totalFrames += s.frames;
        totalOn += s.buckets[0];
        totalHitches += s.hitches;
        worst = Max(worst, s.ms.max);
        minFree = Min(minFree, s.minFreeKB);
        minCached = Min(minCached, s.minCachedKB);
    }
    const float onPct = totalFrames ? 100.0f * totalOn / totalFrames : 0.0f;
    {
        int shown = 0, onTime = 0, missed = 0;
        for (int i = 0; i < bench.done.Num(); i++) {
            const benchSegment_t& s = *bench.done[i];
            for (int k = 0; k < BENCH_FLIP_BUCKETS; k++) {
                shown += s.flips[k];
            }
            onTime += s.flips[1];
            missed += s.flips[2] + s.flips[3] + s.flips[4];
        }
        if (shown > 0) {
            Bench_Log("bench overall on screen: %d frames shown, %.1f%% for exactly 2 vblanks (30 fps on time), %.1f%% held for 3 or more", shown, 100.0 * onTime / shown, 100.0 * missed / shown);
            Bench_Log("bench VERDICT presentation: %s (pass = 95%% of frames on screen for exactly 2 vblanks)", onTime * 100 >= shown * 95 ? "PASS" : "FAIL");
        }
    }
    Bench_Log("bench overall: %d frames measured, %.1f%% within %.1f ms, %d over %.0f ms, worst %.1f ms", totalFrames, onPct, BENCH_TARGET_MS, totalHitches, BENCH_HITCH_MS, worst);
    Bench_Log("bench VERDICT frame time: %s (pass = 95%% of frames within %.1f ms)", onPct >= 95.0f ? "PASS" : "FAIL", BENCH_TARGET_MS);
    // the finish screen over the main menu (VitaVerify.cpp draws it)
    VitaVerify_BannerClear();
    VitaVerify_BannerAdd(aborted ? 2 : 1, "%s %s", bench.verifyMode ? "VERIFICATION" : "BENCHMARK", aborted ? "ABORTED" : "FINISHED");
    VitaVerify_BannerAdd(4, "%.0f minutes, %d segments, %d script errors", (now - bench.startUs) / 60000000.0, bench.done.Num(), bench.errors);
    VitaVerify_BannerAdd(onPct >= 95.0f ? 1 : 3, "Frame time: %s - %.1f%% of %d frames within %.1f ms, worst %.0f ms", onPct >= 95.0f ? "PASS" : "FAIL", onPct, totalFrames, BENCH_TARGET_MS, worst);
#if defined(__vita__)
    Bench_Log("bench VERDICT memory: %s (lowest free %u MB, lowest cached heap %u MB; pass = 32 MB free and 8 MB cached)",
        (minFree >= 32 * 1024 && minCached >= 8 * 1024) ? "PASS" : "FAIL", minFree / 1024, minCached / 1024);
    VitaVerify_BannerAdd((minFree >= 32 * 1024 && minCached >= 8 * 1024) ? 1 : 3, "Memory: %s - lowest free %u MB, lowest cached heap %u MB",
        (minFree >= 32 * 1024 && minCached >= 8 * 1024) ? "PASS" : "FAIL", minFree / 1024, minCached / 1024);
#endif
    for (int i = 0; i < bench.done.Num(); i++) {
        idStr diag;
        Bench_Diagnose(*bench.done[i], diag);
        Bench_Log("bench   %2d %s: %s", i + 1, bench.done[i]->name.c_str(), diag.c_str());
    }
    if (bench.verifyMode) {
        VitaVerify_Report();
    }
    Bench_Log("bench ================================================================");
    idFile* f = bench.resultFile != NULL ? bench.resultFile : fileSystem->OpenFileWrite("benchmark_results.txt");
    bench.resultFile = NULL;
    if (f != NULL) {
        for (int i = 0; i < bench.resultChunks.Num(); i++) {
            f->Write(bench.resultChunks[i], (i == bench.resultChunks.Num() - 1) ? bench.resultLast : BENCH_CHUNK);
        }
        delete f;
        common->Printf("bench: results written to benchmark_results.txt in the save folder\n");
    }
#if defined(__vita__)
    VitaVerify_BannerAdd(0, "Results in ux0:data/doom3bfg/ (benchmark_results.txt)");
    VitaVerify_BannerAdd(4, "Press SELECT to close");
#else
    VitaVerify_BannerAdd(0, "Results: benchmark_results.txt in the save folder");
#endif
    VitaVerify_BannerShow();
    bench.verifyMode = false;
    bench.lookSet = false;
    bench.done.DeleteContents(true);
    Bench_FreeResults();
    bench.state = BENCH_OFF;
#if defined(__vita__)
    extern void Vita_TrophyDryRun(bool on);
    Vita_TrophyDryRun(false); // trophies unlock again (vita_trophies.cpp)
#endif
#if defined(__vita__)
    cvarSystem->SetCVarInteger("vita_perfLog", bench.perfLogWas);
#endif
    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
}


static void Bench_Start(const idStr& text, const char* scriptName, bool verify, bool resume)
{
    VitaVerify_BannerClear(); // the last run's finish screen
    bench.scriptName = scriptName;
    bench.lines.Clear();
    int start = 0;
    for (int i = 0; i <= text.Length(); i++) {
        if (i == text.Length() || text[i] == '\n') {
            idStr l = text.Mid(start, i - start);
            l.StripTrailing('\r');
            bench.lines.Append(l);
            start = i + 1;
        }
    }
    bench.pc = 0;
    bench.errors = 0;
    bench.seg = NULL;
    bench.done.Clear();
    Bench_FreeResults();
    bench.spawned.Clear();
    bench.spawnSerial = 0;
    bench.mapName = "(none)";
    bench.startUs = Sys_Microseconds();
    bench.lastFrameUs = 0;
    bench.settleUntilUs = 0;
    bench.input.impulse = -1;
    bench.lookSet = false;
    Bench_SetInput(0, 0, 0, 0.0f);
    for (int i = 0; i < 3; i++) {
        bench.lastLoads[i] = vita_benchLoads[i];
    }
    bench.lastSlowJob = vita_benchSlowJobNext.GetValue();
#if defined(__vita__)
    // the perf block prints once per segment (Vita_PerfFlush), not every 300 frames
    bench.perfLogWas = cvarSystem->GetCVarInteger("vita_perfLog");
    cvarSystem->SetCVarInteger("vita_perfLog", 1000000);
    for (int i = 0; i < 6; i++) {
        bench.lastFe[i] = vita_frontEndUs[i];
    }
#endif
    bench.verifyMode = verify;
    bench.resultFile = NULL;
    if (verify) {
        // a resumed run adds to the log of the run it continues
        if (resume) {
            bench.resultFile = fileSystem->OpenFileAppend("benchmark_results.txt", false, "fs_savepath");
        }
        if (bench.resultFile == NULL) {
            bench.resultFile = fileSystem->OpenFileWrite("benchmark_results.txt");
        }
        VitaVerify_Begin(resume);
    }
    Bench_Header();
    Bench_Log("bench   frames skipped for the test's screenshots: their JPEG encoding is not counted as game frames");
    for (int i = 0; i < benchNumPhases; i++) {
        benchPhases[i].us = 0; // the phases from here on
    }
    if (verify && resume) {
        bench.pc = VitaVerify_ResumeLine(bench.lines.Num());
    }
#if defined(__vita__)
    // no trophy is earned by the benchmark or the verification: requests are
    // recorded instead (the Vita cannot lock a trophy again)
    extern void Vita_TrophyDryRun(bool on);
    Vita_TrophyDryRun(true);
#endif
    bench.state = BENCH_RUN;
    Bench_Step();
}

CONSOLE_COMMAND_SHIP(benchmark, "runs the benchmark: benchmark [script] (base/<script>.txt; without one, base/benchmark.txt or the built-in script)", 0)
{
    if (bench.state != BENCH_OFF) {
        common->Printf("bench: already running - 'benchmarkStop' ends it\n");
        return;
    }
    idStr text;
    idStr name = (args.Argc() > 1) ? va("%s.txt", args.Argv(1)) : "benchmark.txt";
    void* buffer = NULL;
    const int len = fileSystem->ReadFile(name, &buffer);
    idStr scriptName;
    if (len > 0 && buffer != NULL) {
        text = idStr((const char*)buffer).Left(len);
        fileSystem->FreeFile(buffer);
        scriptName = name;
    } else {
        if (args.Argc() > 1) {
            common->Printf("bench: %s not found\n", name.c_str());
            return;
        }
        text = vitaBenchDefaultScript;
        scriptName = "built-in (alphalabs4, enpro, cpu)";
    }
    Bench_Start(text, scriptName.c_str(), false, false);
}

/*
================
verify

The full verification (VitaVerify.cpp): every part of the game - the three
campaigns, their cutscenes and videos, saving and loading, the menus, every
language, DOOM, DOOM II and No Rest for the Living, multiplayer with bots,
the online services, the benchmark's own heavy scenes - with a PASS / FAIL
verdict for each check. L + R at boot starts it. A run that died (a crash, a
hang the user had to quit) is picked up where it stopped the next time it is
started, with the step that died counted as a failure; "verify fresh" starts
over.
================
*/
CONSOLE_COMMAND_SHIP(verify, "runs the full verification: verify [fresh] [script] (base/<script>.txt, else the built-in one; picks up a run that died unless 'fresh')", 0)
{
    if (bench.state != BENCH_OFF) {
        common->Printf("bench: already running - 'benchmarkStop' ends it\n");
        return;
    }
    bool fresh = false;
    idStr scriptArg;
    for (int i = 1; i < args.Argc(); i++) {
        if (!idStr::Icmp(args.Argv(i), "fresh")) {
            fresh = true;
        } else {
            scriptArg = args.Argv(i);
        }
    }
    idStr text;
    idStr scriptName;
    if (scriptArg.Length() > 0) {
        void* buffer = NULL;
        const int len = fileSystem->ReadFile(va("%s.txt", scriptArg.c_str()), &buffer);
        if (len <= 0 || buffer == NULL) {
            common->Printf("bench: %s.txt not found\n", scriptArg.c_str());
            return;
        }
        text = idStr((const char*)buffer).Left(len);
        fileSystem->FreeFile(buffer);
        scriptName = va("%s.txt", scriptArg.c_str());
    } else {
        extern const char* VitaVerify_BuiltInScript();
        text = VitaVerify_BuiltInScript();
        scriptName = "built-in full verification";
    }
    Bench_Start(text, scriptName.c_str(), true, !fresh);
}

CONSOLE_COMMAND_SHIP(benchmarkStop, "ends the benchmark early (the results so far are logged and written)", 0)
{
    if (bench.state != BENCH_OFF) {
        Bench_Finish(true);
    }
}

#if !defined(__vita__)
// job threads cannot print on the PC: a ring the main thread empties
struct benchCalib_t {
    const char* model;
    int us, tris, verts, skinned, precise;
};
static benchCalib_t benchCalib[256];
static idSysInterlockedInteger benchCalibNext;
static int benchCalibPrinted = 0;
void Vita_ShadowCalibLog(const char* model, int us, int tris, int verts, int skinned, int precise)
{
    const int i = benchCalibNext.Increment() - 1;
    if (i < 256) {
        benchCalib_t c = { model, us, tris, verts, skinned, precise };
        benchCalib[i] = c;
    }
}
void Vita_ShadowCalibPrint()
{
    const int n = Min(256, benchCalibNext.GetValue());
    for (; benchCalibPrinted < n; benchCalibPrinted++) {
        const benchCalib_t& c = benchCalib[benchCalibPrinted];
        common->Printf("pcshadow: %d us for '%s' (%d tris, %d verts, %s, precise inside test %d)\n", c.us, c.model, c.tris, c.verts, c.skinned ? "GPU-skinned" : "CPU verts", c.precise);
    }
}

/*
================================================================================

benchScan (PC): find the most taxing views of a map

================================================================================
*/
struct benchScanSample_t {
    int spot;
    int yaw;
    float score;
    vitaBenchScene_t scene;
};

static struct {
    idStr map;
    int wantViews;
    idList<idVec3> spots;
    idStrList classes;
    idList<int> classCounts;
    idList<benchScanSample_t> samples;
    int cur;      // spot * 8 + direction
    int frame;    // frames at the current view
    int phase;    // 0 loading, 1 scanning
} scan;

static float Bench_ScanScore(const vitaBenchScene_t& s)
{
    // a rough cost on the Vita: the GPU's draws and fill, the front end's
    // lights, shadow volumes and skinning, and whole extra views
    return s.draws + s.tris / 100.0f + s.shadowTris / 60.0f + s.lights * 12.0f + s.shadowVolumes * 6.0f + s.skinnedVerts / 150.0f + Max(0, s.views - 1) * 100.0f;
}

static void Bench_ScanWrite()
{
    // views: the best scores, at least 600 units apart
    idList<int> order;
    for (int i = 0; i < scan.samples.Num(); i++) {
        order.Append(i);
    }
    for (int i = 1; i < order.Num(); i++) { // insertion sort, best first (a few thousand)
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && scan.samples[order[j]].score < scan.samples[v].score) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    idList<int> picked;
    for (int i = 0; i < order.Num() && picked.Num() < scan.wantViews; i++) {
        const benchScanSample_t& s = scan.samples[order[i]];
        bool tooNear = false;
        for (int k = 0; k < picked.Num(); k++) {
            if ((scan.spots[scan.samples[picked[k]].spot] - scan.spots[s.spot]).LengthFast() < 600.0f) {
                tooNear = true;
                break;
            }
        }
        if (!tooNear) {
            picked.Append(order[i]);
        }
    }
    // monsters to fight with: the map's own, the most common ones, no bosses
    idStrList fightClasses;
    {
        idList<int> idx;
        for (int i = 0; i < scan.classes.Num(); i++) {
            const idStr& c = scan.classes[i];
            if (c.Find("boss") >= 0 || c.Find("cin") >= 0 || c.Find("sentry") >= 0 || c.Find("turret") >= 0 || c.Find("char_") >= 0 || c.Find("npc") >= 0
                || c.Find("vagary") >= 0 || c.Find("guardian") >= 0 || c.Find("sabaoth") >= 0 || c.Find("cyberdemon") >= 0) {
                continue;
            }
            idx.Append(i);
        }
        // the most common first, but the small fry (trites, ticks, cherubs,
        // lost souls) after the rest: imps' fireballs and soldiers' muzzle
        // flashes are what a fight costs
        auto weight = [](const idStr& c, int count) -> int {
            const bool small = c.Find("trite") >= 0 || c.Find("tick") >= 0 || c.Find("cherub") >= 0 || c.Find("lostsoul") >= 0 || c.Find("skeleton") >= 0;
            return small ? count : count + 1000;
        };
        for (int a = 0; a < idx.Num(); a++) {
            for (int b = a + 1; b < idx.Num(); b++) {
                if (weight(scan.classes[idx[b]], scan.classCounts[idx[b]]) > weight(scan.classes[idx[a]], scan.classCounts[idx[a]])) {
                    const int swap = idx[a];
                    idx[a] = idx[b];
                    idx[b] = swap;
                }
            }
        }
        for (int i = 0; i < idx.Num() && i < 3; i++) {
            fightClasses.Append(scan.classes[idx[i]]);
        }
    }
    idStr base = scan.map;
    base.StripPath();
    idStr out;
    out += va("// benchScan of %s: %d walkable spots x 8 directions = %d views, scored by draws + tris/100 + shadow tris/60 + 12 x lights\n", scan.map.c_str(), scan.spots.Num(), scan.samples.Num());
    out += "// + 6 x shadow volumes + skinned verts/150 + 100 x extra views; the best views at least 600 units apart.\n";
    out += va("// monsters in the map:");
    for (int i = 0; i < scan.classes.Num(); i++) {
        out += va(" %s x%d", scan.classes[i].c_str(), scan.classCounts[i]);
    }
    out += "\n";
    out += va("map %s\n", scan.map.c_str());
    for (int v = 0; v < picked.Num(); v++) {
        const benchScanSample_t& s = scan.samples[picked[v]];
        const idVec3& p = scan.spots[s.spot];
        const float yaw = s.yaw * 45.0f;
        out += va("segment \"%s %d: score %.0f - %d draws, %dk tris (%dk shadow), %d lights, %d shadow volumes, %d skinned verts, %d views\"\n",
            base.c_str(), v + 1, s.score, s.scene.draws, s.scene.tris / 1000, s.scene.shadowTris / 1000, s.scene.lights, s.scene.shadowVolumes, s.scene.skinnedVerts, s.scene.views);
        out += va("view %.0f %.0f %.0f %.0f\n", p.x, p.y, p.z, yaw);
        out += "hold 3000\n";
        out += "turn 360 8000\n";
    }
    // fights at the three most taxing views: monsters on walkable spots 200-700
    // units in front, in sight
    int fights = 0;
    for (int v = 0; v < picked.Num() && fights < 3 && fightClasses.Num() > 0; v++) {
        const benchScanSample_t& s = scan.samples[picked[v]];
        const idVec3& p = scan.spots[s.spot];
        const float yaw = s.yaw * 45.0f;
        const idVec3 fwd = idAngles(0.0f, yaw, 0.0f).ToForward();
        const idVec3 eye = p + idVec3(0.0f, 0.0f, 64.0f);
        idList<idVec3> at;
        for (int i = 0; i < scan.spots.Num() && at.Num() < 5; i++) {
            const idVec3 d = scan.spots[i] - p;
            const float dist = d.LengthFast();
            if (dist < 200.0f || dist > 700.0f || (d * fwd) < dist * 0.82f) {
                continue; // too near, too far, or more than ~35 degrees off
            }
            bool crowded = false;
            for (int k = 0; k < at.Num(); k++) {
                if ((at[k] - scan.spots[i]).LengthFast() < 80.0f) {
                    crowded = true;
                }
            }
            if (crowded || !Vita_BenchClearLine(eye, scan.spots[i] + idVec3(0.0f, 0.0f, 40.0f))) {
                continue;
            }
            at.Append(scan.spots[i]);
        }
        if (at.Num() < 3) {
            continue;
        }
        fights++;
        out += va("segment \"%s fight %d: %d monsters (%s) at view %d, flashlight on\"\n", base.c_str(), fights, at.Num(), fightClasses[0].c_str(), v + 1);
        out += va("view %.0f %.0f %.0f %.0f\n", p.x, p.y, p.z, yaw);
        out += "weapon 6\n";
        out += "flashlight\n";
        for (int k = 0; k < at.Num(); k++) {
            out += va("spawn %s %.0f %.0f %.0f %.0f\n", fightClasses[k % fightClasses.Num()].c_str(), at[k].x, at[k].y, at[k].z, yaw + 180.0f);
        }
        out += "hold 2500\n";
        out += "fire 6000 60 6000\n";
        out += "kill\n";
        out += "hold 4000\n";
        out += "remove\n";
        out += "flashlight\n";
    }
    const idStr path = va("benchmark/scan_%s.txt", base.c_str());
    idFile* f = fileSystem->OpenFileWrite(path);
    if (f != NULL) {
        f->Write(out.c_str(), out.Length());
        delete f;
    }
    common->Printf("benchScan: %d views and %d fights written to %s (save folder)\n", picked.Num(), fights, path.c_str());
    common->Printf("%s", out.c_str());
}

static void Bench_ScanFrame(uint64 now)
{
    if (scan.phase == 0) {
        idStr why;
        const bool spawned = commonLocal.GetSpawnedMapName() != NULL && scan.map.Icmp(commonLocal.GetSpawnedMapName()) == 0;
        if (!spawned || !Vita_BenchGameReady(why)) {
            return;
        }
        if (scan.frame++ < 90) {
            return; // let it settle
        }
        Vita_BenchPlayerSetup(true, true, true);
        scan.spots.Clear();
        scan.classes.Clear();
        scan.classCounts.Clear();
        scan.samples.Clear();
        Vita_BenchScanSpots(scan.spots, scan.classes, scan.classCounts, 160.0f);
        common->Printf("benchScan: %s - %d walkable spots, %d monster classes\n", scan.map.c_str(), scan.spots.Num(), scan.classes.Num());
        scan.phase = 1;
        scan.cur = -1;
        scan.frame = 0;
    }
    if (scan.cur >= 0 && ++scan.frame < 4) {
        return;
    }
    if (scan.cur >= 0) {
        benchScanSample_t s;
        s.spot = scan.cur / 8;
        s.yaw = scan.cur % 8;
        s.scene = vita_benchScene;
        s.score = Bench_ScanScore(s.scene);
        scan.samples.Append(s);
    }
    scan.cur++;
    scan.frame = 0;
    if (scan.cur >= scan.spots.Num() * 8) {
        Bench_ScanWrite();
        bench.state = BENCH_OFF;
        return;
    }
    if ((scan.cur % 400) == 0) {
        common->Printf("benchScan: %d of %d views\n", scan.cur, scan.spots.Num() * 8);
    }
    const idVec3& p = scan.spots[scan.cur / 8];
    const float eye = cvarSystem->GetCVarFloat("pm_normalviewheight") - 0.25f;
    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("setviewpos %.1f %.1f %.1f %.1f\n", p.x, p.y, p.z + eye, (scan.cur % 8) * 45.0f));
}

CONSOLE_COMMAND_SHIP(benchScan, "PC: finds the most taxing views of a map for the benchmark: benchScan <map> [views]", 0)
{
    if (args.Argc() < 2) {
        common->Printf("usage: benchScan <map> [views]\n");
        return;
    }
    scan.map = args.Argv(1);
    scan.wantViews = (args.Argc() > 2) ? atoi(args.Argv(2)) : 6;
    scan.phase = 0;
    scan.frame = 0;
    bench.state = BENCH_SCAN;
    bench.lastFrameUs = 0;
    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("devmap %s\n", scan.map.c_str()));
}
#endif
