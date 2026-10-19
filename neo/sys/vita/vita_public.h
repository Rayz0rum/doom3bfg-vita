/*
===========================================================================
PS Vita specific interfaces shared between the platform layer and the
common POSIX backend.
===========================================================================
*/

#ifndef __VITA_PUBLIC_H__
#define __VITA_PUBLIC_H__

// Where the game data, savegames and logs live. vita_main.cpp creates this
// before common->Init() runs; DEFAULT_BASEPATH in framework/Licensee.h has to
// agree with it.
#define VITA_DATA_PATH "ux0:data/doom3bfg"

// Append one line to VITA_DATA_PATH "/console.txt".
//
// There is no stdout on this console, so Sys_Printf()'s vprintf() goes nowhere
// and every diagnostic the engine emits while starting up is lost - which is
// precisely the output needed to work out why a boot failed. Crash dumps are no
// substitute: .psp2dmp files use Sony's own PT_NOTE types and no gdb can read
// register state out of them, so a log the game wrote itself is the only usable
// record of how far it got.
//
// Each call opens, appends and closes, so whatever was written survives a hard
// crash rather than sitting in a buffer that never gets flushed.
void Vita_Log(const char* text);
void Vita_LogFlush(); // write out what the log thread has not yet (errors, exit)

// Same, but timestamped and printf-style. Use for coarse progress markers.
void Vita_Checkpoint(const char* fmt, ...);

// Report whether Sony's Cg compiler module is installed, logging what to do
// about it if not. vitaGL compiles every shader at runtime through vitaShaRK,
// which dlopens libshacccg.suprx; the module ships in the console firmware and
// cannot be redistributed, so it is not part of the VPK. When it is absent
// glCompileShader() fails on its first line and leaves the shader uncompiled,
// and the crash lands somewhere downstream with nothing to connect it back.
bool Vita_CheckShaderCompiler();

// Log free bytes in each of vitaGL's pools. The wrapped allocators spread the
// engine across all of them (see vita_mem.cpp), so a failure that looks like a
// random crash during asset loading is worth checking against these first.
void Vita_LogMemory(const char* tag);
void Vita_LogMemoryBudget(const char* when);
// everything still allocatable (KB), and the part of it in the cached newlib heap
void Vita_FreeMemoryKB(unsigned int* allFreeKB, unsigned int* cachedFreeKB); // O(1): newlib arena tail + vitaGL pool counters
void Vita_LogMemoryAccuracy(const char* when); // fast vs exact (heap-walking, slow) figures, for the log
// the memory an address is in ("cached RAM", "UNCACHED RAM", "CDRAM (UNCACHED)"...): a syscall, logs only
const char* Vita_MemKind(const void* p);
// where Mem_Alloc16's allocations land by memory tag: cached newlib heap or
// the uncached pools (vita_mem.cpp); reset and printed around each map load
#define VITA_MEM_TAGS 160
void Vita_NoteTagAlloc(int tag, size_t size, void* p);
void Vita_TagPlacementReset();
void Vita_TagPlacementReport(const char* when);

// vitaGL must only be called from the thread that brought it up. The first
// sets that thread (GLimp_Init); the second logs - a few times - when a GL
// path runs anywhere else, naming the step and the resource.
void Vita_SetGLThread();
// the game/draw thread registers itself so the watchdog can sample it
void Vita_NoteGameThread();
// per-thread CPU time and preemptions in the perf log (vita_main.cpp)
void Vita_RegisterThread(int sceUid);
void Vita_NameThread(int sceUid, const char* name, int core); // the engine's name and core, for the CPU log
void Vita_PlaceBackgroundThread(const char* name);           // a bare Sys_CreateThread thread: core 3 (or 0+2), lowest priority
bool Vita_Core3Available(); // a plugin (CapUnlocker) lets the game use core 3
void Vita_ScanThreads();
void Vita_LogThreadCpu(double frames);
void Vita_CheckGLThread(const char* what, const char* name);
bool Vita_IsGLThread();

/*
===========================================================================
Hardware video playback (vita_avplayer.cpp).

idCinematic uses this in place of its RoQ decoder when a .mp4 exists beside
the video it was asked for. See the comment at the top of vita_avplayer.cpp
for why, and VITA_PORT.md for how the files are produced.
===========================================================================
*/
struct vitaVideo_t;

// Starts a player for the file (width x height: the stream's size, from the
// file's header, for the memory estimate; 0 if unknown). Returns NULL when
// there is no room right now - too many players, or not enough of the memory
// the decoder needs - in which case the caller tries again later or plays its
// own decoder. The player opens on a background thread: until it has, the
// update below returns false. Call on the GL thread.
vitaVideo_t* VitaVideo_Open(const char* osPath, bool looping, int width, int height);
// claims the video arena (physically contiguous memory for the decoder's
// frame buffers) at renderer start, before a map can take it; logs the result
void VitaVideo_ReserveArena();
// Detaches the texture (GL thread) and closes the player in the background.
// The pointer is dead on return.
void VitaVideo_Close(vitaVideo_t* v);

// Polls for a decoded frame and points GL texture `texnum` at it. Returns
// true once at least one frame has arrived, i.e. once there is something to
// draw. Call once per rendered frame; sceAvPlayer keeps its own clock.
bool VitaVideo_UpdateFrame(vitaVideo_t* v, unsigned int texnum, int* width, int* height);

bool VitaVideo_IsPlaying(vitaVideo_t* v); // starting up counts as playing
bool VitaVideo_Failed(vitaVideo_t* v);    // the open failed, or no frame came in 3 s
int VitaVideo_LengthMs(vitaVideo_t* v);
int VitaVideo_StartupMs(vitaVideo_t* v);  // ms since its open while no frame has come, -1 once one has
void VitaVideo_Rewind(vitaVideo_t* v);
int VitaVideo_ActivePlayers();            // players open, opening or closing
// the verification's video test: count the frames of the video whose OS path
// ends with pathEnd ("" stops); frames so far, and ms from open to the first
void VitaVideo_Watch(const char* pathEnd);
int VitaVideo_WatchedFrames(unsigned int* firstFrameMs);
// players, job queue, worker, pool - one line for a report
void VitaVideo_Status(char* out, int outLen);

/*
===========================================================================
Boot self-test and screen sampling (vita_selftest.cpp).
===========================================================================
*/
// Verdicts: 1 = pass, 0 = fail, -1 = not run.
struct vitaSelfTest_t {
    bool ran;
    int ycocgCompound;  // engine's ConvertYCoCgToRGB, "YCoCg. xy *= YCoCg. z ;" kept
    int ycocgExpanded;  // same, with the compound statement expanded
    int dxt5Raw;        // DXT5 texel sampled with all four channels intact
    int dxt1Raw;
    int rgb565Swapped;  // RGB565 after the engine's big-endian -> little-endian swap
    int bc4Coverage;    // BC4 sampled as (1,1,1,value), used for CFM_GREEN_ALPHA
    int mipLevels;      // vitaGL places each uncompressed mip where it samples it
    int passes;
    int failures;
};
extern vitaSelfTest_t vitaSelfTest;

// Runs once, after the GL context is up and before any engine shader is
// generated - the YCoCg verdict decides how the interaction shaders are
// emitted. Logs every step to console.txt.
void Vita_RunSelfTest();

// Where vitaGL keeps mip `level` of a linear (uncompressed) texture: byte
// offset from the texture's data pointer, row pitch, and how many levels
// vitaGL actually allocated. Returns -1 for a level it never allocated.
int Vita_LinearMipOffset(int level, int baseW, int baseH, int bpp, int* pitch, int* levelCount);

// Screen colour statistics, read back after a buffer swap. `frames` samples
// are taken on the next swaps, then one every `periodicFrames` (0 = never).
void Vita_ScreenSampleRequest(int frames, const char* reason, int periodicFrames);
void Vita_ScreenSampleAfterSwap();

// Per-light counters reported alongside each screen sample.
void Vita_CensusLight(bool ambient, int draws, int scissorPixels, const char* name);

// Render resolution chosen at start (vita_resolution, sdl_glimp.cpp).
void Vita_GetRenderResolution(int* w, int* h);

#endif // __VITA_PUBLIC_H__
