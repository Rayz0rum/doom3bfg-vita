/*
===========================================================================

Doom 3 BFG Edition GPL Source Code

Compressed, streamed sound playback. See AL_SoundStream.h for the why.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"
#include "../snd_local.h"
#include "../../sys/vita/vita_zones.h" // VITA_ZONE: timing zones for the perf log
#include "AL_ADPCM.h" // MS ADPCM block decoding

#if defined(ID_SOUND_STREAM)

#include <stdio.h>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
bool Vita_Core3Available(); // vita_main.cpp
#endif

idCVar s_streamThreshold("s_streamThreshold", "32", CVAR_INTEGER | CVAR_SOUND,
    "sound samples with more than this many KB of compressed data are streamed from disk while they play instead of being kept in memory", 4, 100000);
idCVar s_streamHead("s_streamHead", "12", CVAR_INTEGER | CVAR_SOUND,
    "KB at the start of a streamed sound kept in memory, so it starts at once while the rest is read", 4, 256);
idCVar s_streamDebug("s_streamDebug", "0", CVAR_INTEGER | CVAR_SOUND, "1 = log stream underruns and reads that fail");
idCVar s_streamSelfTest("s_streamSelfTest", "0", CVAR_INTEGER | CVAR_SOUND, "after a level load, decode up to this many samples through the streaming path and compare them with a whole-file decode");
idCVar s_streamThread("s_streamThread", "1", CVAR_BOOL | CVAR_SOUND | CVAR_INIT, "decode and queue the playing sounds on a worker thread (1) instead of in the game/draw thread's sound update (0)");

extern idCVar s_noSound;
extern idCVar s_debugHardware;

static const uint32 SOUND_MAGIC_IDMSA_STREAM = 0x6D7A7274;

/*
================================================================================================

    Reader thread

    Container files are opened with stdio on whichever thread reads; every
    seek+read pair runs under one lock, so the reader thread and a synchronous
    read on the main thread never interleave on a handle. Requests are a small
    ring of pointers into voice-owned request structs.

================================================================================================
*/

static const int MAX_STREAM_PATHS = 32;
static const int MAX_QUEUED_REQUESTS = 128;

struct streamPath_t {
    char osPath[MAX_OSPATH];
    FILE* fp;
};

static streamPath_t streamPaths[MAX_STREAM_PATHS];
static int numStreamPaths = 0;

static idSysMutex* streamFileLock = NULL;
static idSysMutex* streamQueueLock = NULL;
static idSysSignal* streamWake = NULL;
static soundStreamRequest_t* streamQueue[MAX_QUEUED_REQUESTS];
static int streamQueueHead = 0; // next to take
static int streamQueueTail = 0; // next free slot
static volatile bool streamThreadQuit = false;
static uintptr_t streamThreadHandle = 0;

static int streamReads = 0;
static int64 streamReadBytes = 0;
static int streamReadFailures = 0;

static inline void SoundStream_ReleaseBarrier()
{
    __sync_synchronize();
}

void SoundStream_AcquireBarrier()
{
    __sync_synchronize();
}

static bool SoundStream_ReadLocked(int pathId, int offset, void* dest, int size)
{
    if (pathId < 0 || pathId >= numStreamPaths || size <= 0) {
        return false;
    }
    streamPath_t& p = streamPaths[pathId];
    if (p.fp == NULL) {
        p.fp = fopen(p.osPath, "rb");
        if (p.fp == NULL) {
            return false;
        }
        // whole-request reads; stdio buffering would only add a copy
        setvbuf(p.fp, NULL, _IONBF, 0);
    }
    if (fseek(p.fp, offset, SEEK_SET) != 0) {
        return false;
    }
    const size_t got = fread(dest, 1, size, p.fp);
    streamReads++;
    streamReadBytes += (int64)got;
    return got == (size_t)size;
}

bool SoundStream_ReadSync(int pathId, int offset, void* dest, int size)
{
    if (streamFileLock == NULL) {
        return false;
    }
    idScopedCriticalSection lock(*streamFileLock);
    const bool ok = SoundStream_ReadLocked(pathId, offset, dest, size);
    if (!ok) {
        streamReadFailures++;
    }
    return ok;
}

static unsigned int SoundStream_ThreadMain(void* parm)
{
    while (!streamThreadQuit) {
        soundStreamRequest_t* req = NULL;
        streamQueueLock->Lock();
        if (streamQueueHead != streamQueueTail) {
            req = streamQueue[streamQueueHead];
            streamQueueHead = (streamQueueHead + 1) % MAX_QUEUED_REQUESTS;
        }
        streamQueueLock->Unlock();

        if (req == NULL) {
            // every Submit raises the signal, and a raise before the wait is kept
            streamWake->Wait(idSysSignal::WAIT_INFINITE);
            continue;
        }

        bool ok;
        {
            idScopedCriticalSection lock(*streamFileLock);
            ok = SoundStream_ReadLocked(req->pathId, req->offset, req->dest, req->size);
            if (!ok) {
                streamReadFailures++;
            }
        }
        req->result = ok ? req->size : 0;
        SoundStream_ReleaseBarrier();
        req->state = SSR_DONE;
    }
    return 0;
}

void SoundStream_Init()
{
    if (streamFileLock != NULL) {
        return;
    }
    streamFileLock = new idSysMutex();
    streamQueueLock = new idSysMutex();
    streamWake = new idSysSignal(false);
    streamThreadQuit = false;
    streamThreadHandle = Sys_CreateThread(SoundStream_ThreadMain, NULL, THREAD_ABOVE_NORMAL, "SoundStream", CORE_ANY, 64 * 1024, false);
    common->Printf("sound: compressed playback - samples over %d KB stream from disk (%d KB kept in memory each), the rest stay ADPCM in memory\n",
        s_streamThreshold.GetInteger(), s_streamHead.GetInteger());
}

void SoundStream_Shutdown()
{
    // The process is going away; stop taking requests but leave the thread
    // and handles to the OS rather than racing a read in flight.
    streamThreadQuit = true;
    if (streamWake != NULL) {
        streamWake->Raise();
    }
}

int SoundStream_RegisterPath(const char* osPath)
{
    if (streamFileLock == NULL) {
        SoundStream_Init();
    }
    idScopedCriticalSection lock(*streamFileLock);
    for (int i = 0; i < numStreamPaths; i++) {
        if (idStr::Icmp(streamPaths[i].osPath, osPath) == 0) {
            return i;
        }
    }
    if (numStreamPaths >= MAX_STREAM_PATHS) {
        return -1;
    }
    idStr::Copynz(streamPaths[numStreamPaths].osPath, osPath, sizeof(streamPaths[numStreamPaths].osPath));
    streamPaths[numStreamPaths].fp = NULL;
    return numStreamPaths++;
}

void SoundStream_Submit(soundStreamRequest_t* req)
{
    req->result = 0;
    req->state = SSR_PENDING;
    bool queued = false;
    if (streamQueueLock != NULL) {
        streamQueueLock->Lock();
        const int next = (streamQueueTail + 1) % MAX_QUEUED_REQUESTS;
        if (next != streamQueueHead) {
            streamQueue[streamQueueTail] = req;
            streamQueueTail = next;
            queued = true;
        }
        streamQueueLock->Unlock();
    }
    if (queued) {
        streamWake->Raise();
        return;
    }
    // queue full (or no thread): read it here
    const bool ok = SoundStream_ReadSync(req->pathId, req->offset, req->dest, req->size);
    req->result = ok ? req->size : 0;
    SoundStream_ReleaseBarrier();
    req->state = SSR_DONE;
}

/*
================================================================================================

    Accounting

================================================================================================
*/
static int64 accountResidentBytes = 0;
static int accountSamples = 0;
static int accountStreamed = 0;
static int64 accountPeakBytes = 0;

void SoundStream_AccountSample(int residentBytes, bool streamed)
{
    if (residentBytes >= 0) {
        accountResidentBytes += residentBytes;
        accountSamples++;
        if (streamed) {
            accountStreamed++;
        }
    } else {
        accountResidentBytes += residentBytes;
        accountSamples--;
        if (streamed) {
            accountStreamed--;
        }
    }
    if (accountResidentBytes > accountPeakBytes) {
        accountPeakBytes = accountResidentBytes;
    }
}

void SoundStream_PrintStats(const char* when)
{
    common->Printf("sound %s: %d samples, %d KB in memory (peak %d KB), %d of them streamed from disk; %d reads (%d KB) so far, %d failed\n",
        when, accountSamples, (int)(accountResidentBytes / 1024), (int)(accountPeakBytes / 1024), accountStreamed,
        streamReads, (int)(streamReadBytes / 1024), streamReadFailures);
}

/*
================================================================================================

    idSoundSample_OpenAL - compressed in memory, optionally streamed

================================================================================================
*/

/*
========================
idSoundSample_OpenAL::SetupBlocks

Describes buffers[0] (and the streamed rest, if any) as fixed-size blocks the
voices decode one at a time. ADPCM blocks are the format's own; PCM gets
pseudo-blocks of 256 frames.
========================
*/
void idSoundSample_OpenAL::SetupBlocks()
{
    const int ch = Max(1, (int)format.basic.numChannels);
    if (format.basic.formatTag == idWaveFile::FORMAT_ADPCM) {
        blockAlign = format.basic.blockSize;
        framesPerBlock = format.extra.adpcm.samplesPerBlock;
    } else {
        framesPerBlock = 256;
        blockAlign = framesPerBlock * ch * 2;
    }
    if (blockAlign <= 0 || framesPerBlock <= 0) {
        blockAlign = 2 * ch;
        framesPerBlock = 1;
    }
    if (streamPathId < 0) {
        dataBytes = buffers.Num() > 0 ? buffers[0].bufferSize : 0;
        residentBytes = dataBytes;
    }
    numBlocks = (dataBytes + blockAlign - 1) / blockAlign;
    // a streamed head is a whole number of blocks; a resident sample counts its short last block
    residentBlocks = (streamPathId >= 0) ? (residentBytes / blockAlign) : numBlocks;
}

/*
========================
idSoundSample_OpenAL::BlockBytes
========================
*/
int idSoundSample_OpenAL::BlockBytes(int block) const
{
    const int start = block * blockAlign;
    if (block < 0 || start >= dataBytes) {
        return 0;
    }
    return Min(blockAlign, dataBytes - start);
}

/*
========================
idSoundSample_OpenAL::DecodeBlock

One block to interleaved 16-bit PCM; returns frames written. dst must hold
FramesPerBlock() * NumChannels() samples.
========================
*/
int idSoundSample_OpenAL::DecodeBlock(const byte* src, int bytes, int16* dst) const
{
    const int ch = format.basic.numChannels;
    if (format.basic.formatTag != idWaveFile::FORMAT_ADPCM) {
        const int frames = bytes / (2 * ch);
        memcpy(dst, src, frames * 2 * ch);
        return frames;
    }
    // AL_ADPCM.h: the same arithmetic as the loop that was here, laid out for the CPU
    return ADPCM_DecodeBlock(src, bytes, dst, ch, framesPerBlock, &format.extra.adpcm.aCoef[0].coef1);
}

/*
========================
idSoundSample_OpenAL::LoadGeneratedSampleStreaming

Reads only the header and as much data as is kept in memory. Small samples
keep all of their compressed data; big ones keep s_streamHead KB and remember
where the rest is.
========================
*/
bool idSoundSample_OpenAL::LoadGeneratedSampleStreaming(const idStr& filename)
{
    idStr osPath;
    int offset = 0;
    int length = 0;
    if (!fileSystem->GetResourceLocation(filename, osPath, offset, length) || length <= 0) {
        // a loose generated file outside the containers: all of it, in memory
        return LoadGeneratedSample(filename);
    }
    const int pathId = SoundStream_RegisterPath(osPath);
    if (pathId < 0) {
        return LoadGeneratedSample(filename);
    }

    const int firstBytes = Min(length, 64 * 1024);
    byte* first = (byte*)Mem_Alloc(firstBytes, TAG_TEMP);
    if (first == NULL) {
        return false;
    }
    if (!SoundStream_ReadSync(pathId, offset, first, firstBytes)) {
        Mem_Free(first);
        return false;
    }

    idFile_Memory hdr(filename, (const char*)first, firstBytes);
    uint32 magic = 0;
    hdr.ReadBig(magic);
    if (magic != SOUND_MAGIC_IDMSA_STREAM) {
        Mem_Free(first);
        return false;
    }
    hdr.ReadBig(timestamp);
    hdr.ReadBig(loaded);
    hdr.ReadBig(playBegin);
    hdr.ReadBig(playLength);
    if (!idWaveFile::ReadWaveFormatDirect(format, &hdr)) {
        Mem_Free(first);
        return LoadGeneratedSample(filename);
    }
    int num = 0;
    hdr.ReadBig(num);
    if (num < 0 || hdr.Tell() + num + 16 > firstBytes) {
        Mem_Free(first);
        return LoadGeneratedSample(filename);
    }
    amplitude.Clear();
    amplitude.SetNum(num);
    hdr.Read(amplitude.Ptr(), num);
    hdr.ReadBig(totalBufferSize);
    int numBuffers = 0;
    hdr.ReadBig(numBuffers);
    if (numBuffers != 1) {
        Mem_Free(first);
        return LoadGeneratedSample(filename);
    }
    buffers.SetNum(1);
    hdr.ReadBig(buffers[0].numSamples);
    hdr.ReadBig(buffers[0].bufferSize);
    const int dataStart = hdr.Tell();
    const int dataSize = buffers[0].bufferSize;
    if (dataSize < 0 || dataStart + dataSize > length) {
        buffers.Clear();
        Mem_Free(first);
        return false;
    }

    const bool knownFormat = (format.basic.formatTag == idWaveFile::FORMAT_ADPCM || format.basic.formatTag == idWaveFile::FORMAT_PCM)
        && format.basic.numChannels >= 1 && format.basic.numChannels <= 2;
    dataBytes = dataSize;
    streamPathId = -1;
    SetupBlocks(); // blockAlign for the head size

    bool stream = knownFormat && dataSize > s_streamThreshold.GetInteger() * 1024;
    int keep = dataSize;
    if (stream) {
        // at least half a second of sound, so the first read has time to land
        // (12 KB is a full second of 22 kHz mono, a quarter of 44 kHz stereo)
        const int headBytes = Max(s_streamHead.GetInteger() * 1024, (int)format.basic.avgBytesPerSec / 2);
        keep = (headBytes / blockAlign) * blockAlign;
        keep = Max(keep, blockAlign);
        if (keep >= dataSize) {
            stream = false;
            keep = dataSize;
        }
    }

    byte* data = (byte*)Mem_Alloc(Max(keep, 16), TAG_AUDIO);
    if (data == NULL) {
        buffers.Clear();
        Mem_Free(first);
        return false;
    }
    const int inFirst = Max(0, Min(keep, firstBytes - dataStart));
    if (inFirst > 0) {
        memcpy(data, first + dataStart, inFirst);
    }
    if (inFirst < keep) {
        if (!SoundStream_ReadSync(pathId, offset + dataStart + inFirst, data + inFirst, keep - inFirst)) {
            Mem_Free(data);
            buffers.Clear();
            Mem_Free(first);
            return false;
        }
    }
    Mem_Free(first);

    buffers[0].buffer = data;
    buffers[0].bufferSize = keep;
    streamPathId = stream ? pathId : -1;
    streamDataOffset = offset + dataStart;
    dataBytes = dataSize;
    residentBytes = keep;
    SetupBlocks();
    return true;
}

/*
================================================================================================

    idSoundVoice_OpenAL - decodes into a queue of small OpenAL buffers

================================================================================================
*/

// s_streamDebug: what the voices did, summed and printed every 5 s
static int dbgChunks = 0, dbgUnderruns = 0, dbgALErrors = 0, dbgStarts = 0;
static int dbgLastPrint = 0;

static void StreamDebugTick()
{
    if (!s_streamDebug.GetBool()) {
        return;
    }
    const int now = Sys_Milliseconds();
    if (now - dbgLastPrint < 5000) {
        return;
    }
    dbgLastPrint = now;
    common->Printf("sound stream: last 5 s - %d starts, %d buffers decoded and queued, %d underruns, %d OpenAL errors; %d reads (%d KB) in total\n",
        dbgStarts, dbgChunks, dbgUnderruns, dbgALErrors, streamReads, (int)(streamReadBytes / 1024));
    dbgChunks = dbgUnderruns = dbgALErrors = dbgStarts = 0;
}

static const int STREAM_RING_HALF = 16 * 1024;
static const int MAX_CHUNK_FRAMES = 8192;
static int16 streamChunkPCM[MAX_CHUNK_FRAMES * 2];
static int16 streamBlockPCM[MAX_CHUNK_FRAMES * 2];
// the two buffers above serve one decoder at a time: the worker thread, or
// the game/draw thread when the worker is off, or the self test
static idSysMutex* streamDecodeLock = NULL;

/*
================================================================================================

    Sound worker thread

    Decoding the playing sounds (MS ADPCM, ~105 ms of PCM per buffer, four
    buffers queued per voice) and feeding OpenAL used to be the tail of the
    game/draw thread's frame: 3-10 ms of it in fights, the "sound update" of
    the benchmark, all of it on the thread that sets the frame rate. None of
    it has to be there - OpenAL has 300-400 ms of sound queued per voice, so
    whether a buffer is refilled now or 10 ms from now changes nothing you can
    hear. The worker does it all: every 10 ms (sooner when a sound starts) it
    hands finished buffers back, decodes the next ones, keeps the disk reads
    going and restarts a source that ran dry. The game/draw thread keeps what
    has to be in step with the game: starting (it sets the voice up and the
    worker queues the first buffer and plays it, ~1 ms later), stopping,
    pausing, and position/volume/pitch, which are plain OpenAL calls.

    Cores 0 and 2: core 1 is Game/Draw's, core 3 the system's. Priority 0x90,
    above the game's threads (0x9F), so a start does not wait behind a job
    worker; each turn is short (a few hundred microseconds at most).

================================================================================================
*/
static const int SOUND_WORKER_PERIOD_MS = 10;
#if defined(__vita__)
static const int VITA_SOUND_WORKER_PRIORITY = 0x90;
static const int VITA_SOUND_WORKER_CORES = 8; // Thread.cpp: cores 0 and 2
static const int VITA_SOUND_WORKER_CORE3 = 6; // Thread.cpp: core 3

// Core 3, when a plugin lets the game have it, is for work nothing in the
// frame waits for - and nothing does wait for the sound: the worker keeps
// 300-400 ms queued per voice, OpenAL's mixer has its own buffer. On cores
// 0-2 both took time from the job workers (the mixer alone ~5 ms a frame at
// priority 0x40, above them): the benchmark's slow front end jobs - the ones
// the frame then waits for - were nearly all on core 0's worker. vita_main.cpp
// moves the mixer.
idCVar vita_audioCore3("vita_audioCore3", "1", CVAR_BOOL | CVAR_INIT, "Vita: run OpenAL's mixer and the sound worker on core 3 when a plugin (CapUnlocker) makes it available");
#endif

class idSoundWorkerThread : public idSysThread {
public:
    virtual int Run();
};

static idSoundWorkerThread* soundWorker = NULL;
static idSysSignal* soundWorkerWake = NULL;
static volatile bool soundWorkerRunning = false;

int idSoundWorkerThread::Run()
{
#if defined(__vita__)
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), VITA_SOUND_WORKER_PRIORITY);
#endif
    int lastFullRound = 0;
    while (!IsTerminating()) {
        soundWorkerWake->Wait(SOUND_WORKER_PERIOD_MS);
        if (IsTerminating()) {
            break;
        }
        // a wake for a start serves only the voices waiting to start, unless a
        // full round is due anyway
        const int now = Sys_Milliseconds();
        const bool full = (now - lastFullRound >= SOUND_WORKER_PERIOD_MS);
        if (full) {
            lastFullRound = now;
        }
        VITA_ZONE("sound worker: round (decode + queue, off the game/draw thread)");
        soundSystemLocal.hardware.ServiceStreamVoices(full);
    }
    return 0;
}

void SoundWorker_Start()
{
    if (streamDecodeLock == NULL) {
        streamDecodeLock = new idSysMutex();
    }
    if (soundWorker != NULL || !s_streamThread.GetBool()) {
        return;
    }
    soundWorkerWake = new idSysSignal(false);
    soundWorker = new idSoundWorkerThread();
#if defined(__vita__)
    const core_t core = (core_t)((vita_audioCore3.GetBool() && Vita_Core3Available()) ? VITA_SOUND_WORKER_CORE3 : VITA_SOUND_WORKER_CORES);
#else
    const core_t core = CORE_ANY;
#endif
    soundWorker->StartThread("SoundWorker", core, THREAD_ABOVE_NORMAL, 64 * 1024);
    soundWorkerRunning = true;
    common->Printf("sound: playing sounds are decoded and queued on the SoundWorker thread\n");
}

void SoundWorker_Stop()
{
    if (soundWorker == NULL) {
        return;
    }
    // from here on the game/draw thread decodes again (Update), and the worker
    // finishes the round it is in before StopThread returns
    soundWorkerRunning = false;
    soundWorker->StopThread(false);
    soundWorkerWake->Raise();
    soundWorker->StopThread(true);
    delete soundWorker;
    soundWorker = NULL;
    delete soundWorkerWake;
    soundWorkerWake = NULL;
}

bool SoundWorker_Running()
{
    return soundWorkerRunning;
}

void SoundWorker_Wake()
{
    if (soundWorkerWake != NULL) {
        soundWorkerWake->Raise();
    }
}

static int StreamChunkFrames(const idSoundSample_OpenAL* s)
{
    // ~105 ms per buffer; four queued buffers ride out a frame hitch of ~0.4 s
    const int fpb = Max(1, s->FramesPerBlock());
    int blocks = (int)(s->SampleRate() * 0.105f) / fpb;
    blocks = Max(1, blocks);
    return Min(blocks * fpb, (MAX_CHUNK_FRAMES / fpb) * fpb);
}

/*
========================
idSoundVoice_OpenAL::StreamClearQueue

Detaches every queued buffer and forgets the disk reads in flight (they finish
into the ring and are dropped).
========================
*/
void idSoundVoice_OpenAL::StreamClearQueue()
{
    StreamClearAL();
    StreamClearState();
}

/*
========================
idSoundVoice_OpenAL::StreamClearAL

alSourceStop waits for OpenAL's mixer to finish the period it is mixing (up
to 6 ms on the Vita), so with the worker running only the worker calls it
(Start and Stop leave it a streamStopPending).
========================
*/
void idSoundVoice_OpenAL::StreamClearAL()
{
    if (alIsSource(openalSource)) {
        alSourceStop(openalSource);
        alSourcei(openalSource, AL_BUFFER, 0);
    }
    numFreeStreamBuffers = 0;
    if (streamBuffersCreated) {
        for (int i = 0; i < NUM_STREAM_BUFFERS; i++) {
            freeStreamBuffers[numFreeStreamBuffers++] = streamBuffers[i];
        }
    }
    streamStopPending = false;
}

/*
========================
idSoundVoice_OpenAL::StreamClearState
========================
*/
void idSoundVoice_OpenAL::StreamClearState()
{
    for (int h = 0; h < 2; h++) {
        streamHalfValid[h] = false;
        if (streamReq[h].state == SSR_DONE) {
            streamReq[h].state = SSR_IDLE;
        }
    }
    streamSample = NULL;
    streamEnded = true;
    fetchSample = NULL;
    fetchDone = true;
    streamActive = false;
    streamStartPending = false;
}

/*
========================
idSoundVoice_OpenAL::StreamNextSample

What plays after s runs out: the looping sample after the leadin, the looping
sample again after itself, or nothing.
========================
*/
idSoundSample_OpenAL* idSoundVoice_OpenAL::StreamNextSample(idSoundSample_OpenAL* s) const
{
    if (s == NULL) {
        return NULL;
    }
    if (loopingSample != NULL && (s == leadinSample || s == loopingSample)) {
        return loopingSample;
    }
    return NULL;
}

/*
========================
idSoundVoice_OpenAL::StreamSetFetch

Points the disk reader at block 'block' of s, or at the next streamed data that
will play after it.
========================
*/
void idSoundVoice_OpenAL::StreamSetFetch(idSoundSample_OpenAL* s, int block)
{
    fetchDone = true;
    fetchSample = NULL;
    for (int guard = 0; guard < 3 && s != NULL; guard++) {
        if (s->IsStreamed()) {
            const int b = Max(block, s->ResidentBlocks());
            if (b < s->NumBlocks()) {
                fetchSample = s;
                fetchBlock = b;
                fetchDone = false;
                return;
            }
        }
        idSoundSample_OpenAL* next = StreamNextSample(s);
        if (next == s && !s->IsStreamed()) {
            return; // a resident loop never needs the disk
        }
        s = next;
        block = 0;
    }
}

/*
========================
idSoundVoice_OpenAL::StreamFetch

Keeps both halves of the ring busy reading what will be decoded next.
========================
*/
void idSoundVoice_OpenAL::StreamFetch()
{
    for (int h = 0; h < 2; h++) {
        if (streamReq[h].state == SSR_DONE) {
            SoundStream_AcquireBarrier();
            if (!streamHalfValid[h] || streamHalfConsumed[h]) {
                streamReq[h].state = SSR_IDLE;
                streamHalfValid[h] = false;
            }
        }
    }
    if (fetchDone || fetchSample == NULL) {
        return;
    }
    if (streamRing == NULL) {
        streamRing = (byte*)Mem_Alloc(STREAM_RING_HALF * 2, TAG_AUDIO);
        if (streamRing == NULL) {
            fetchDone = true;
            return;
        }
    }
    for (int h = 0; h < 2 && !fetchDone; h++) {
        if (streamReq[h].state != SSR_IDLE) {
            continue;
        }
        idSoundSample_OpenAL* s = fetchSample;
        const int halfBlocks = Max(1, STREAM_RING_HALF / s->BlockAlign());
        const int count = Min(halfBlocks, s->NumBlocks() - fetchBlock);
        const int startByte = fetchBlock * s->BlockAlign();
        const int bytes = Min(count * s->BlockAlign(), s->dataBytes - startByte);
        if (count <= 0 || bytes <= 0) {
            StreamSetFetch(StreamNextSample(s), 0);
            continue;
        }
        streamHalfSample[h] = s;
        streamHalfBlock[h] = fetchBlock;
        streamHalfBlocks[h] = count;
        streamHalfValid[h] = true;
        streamHalfConsumed[h] = false;
        streamReq[h].pathId = s->streamPathId;
        streamReq[h].offset = s->streamDataOffset + startByte;
        streamReq[h].size = bytes;
        streamReq[h].dest = streamRing + h * STREAM_RING_HALF;
        SoundStream_Submit(&streamReq[h]);

        fetchBlock += count;
        if (fetchBlock >= s->NumBlocks()) {
            idSoundSample_OpenAL* next = StreamNextSample(s);
            StreamSetFetch(next, 0);
        }
    }
}

/*
========================
idSoundVoice_OpenAL::StreamBlockData

Where block 'block' of s is right now: in memory, in a finished ring half, or
not here yet (NULL). A block nobody is reading gets the reader pointed at it.
========================
*/
const byte* idSoundVoice_OpenAL::StreamBlockData(idSoundSample_OpenAL* s, int block, int& half)
{
    half = -1;
    if (!s->IsStreamed() || block < s->ResidentBlocks()) {
        return (const byte*)s->buffers[0].buffer + block * s->BlockAlign();
    }
    bool inFlight = false;
    for (int h = 0; h < 2; h++) {
        if (!streamHalfValid[h] || streamHalfConsumed[h] || streamHalfSample[h] != s) {
            continue;
        }
        if (block < streamHalfBlock[h] || block >= streamHalfBlock[h] + streamHalfBlocks[h]) {
            continue;
        }
        if (streamReq[h].state != SSR_DONE) {
            inFlight = true;
            continue;
        }
        SoundStream_AcquireBarrier();
        if (streamReq[h].result <= 0) {
            // the read failed: give up on this sound rather than wait forever
            if (s_streamDebug.GetBool()) {
                common->Printf("sound stream: read failed for '%s' block %d\n", s->GetName(), block);
            }
            streamEnded = true;
            return NULL;
        }
        half = h;
        return streamRing + h * STREAM_RING_HALF + (block - streamHalfBlock[h]) * s->BlockAlign();
    }
    if (!inFlight) {
        // nothing is bringing this block in (a seek, or the reader fell behind
        // a loop point): restart the reader here
        if (s_streamDebug.GetBool()) {
            common->Printf("sound stream: refetching '%s' at block %d\n", s->GetName(), block);
        }
        for (int h = 0; h < 2; h++) {
            streamHalfValid[h] = false;
        }
        StreamSetFetch(s, block);
        StreamFetch();
    }
    return NULL;
}

/*
========================
idSoundVoice_OpenAL::StreamDecodeChunk

Up to maxFrames frames of the current sample (never across a change of sample,
whose rate may differ). Returns 0 at the end of the sound or while waiting for
the disk.
========================
*/
int idSoundVoice_OpenAL::StreamDecodeChunk(int16* dst, int maxFrames)
{
    int written = 0;
    int emptyWraps = 0;     // sample changes with nothing decoded in between
    int writtenAtWrap = -1;
    while (written < maxFrames && !streamEnded && streamSample != NULL) {
        idSoundSample_OpenAL* s = streamSample;
        const int fpb = s->FramesPerBlock();
        const int ch = s->NumChannels();
        if (streamBlock >= s->NumBlocks() || streamBlock * fpb >= s->playLength) {
            idSoundSample_OpenAL* next = StreamNextSample(s);
            if (next == NULL) {
                streamEnded = true;
                break;
            }
            if (next != s && written > 0) {
                break; // start the next sample in a buffer of its own
            }
            emptyWraps = (written == writtenAtWrap) ? emptyWraps + 1 : 0;
            writtenAtWrap = written;
            if (emptyWraps > 2) {
                streamEnded = true; // a loop with nothing in it
                break;
            }
            streamSample = next;
            streamBlock = 0;
            streamSkip = 0;
            continue;
        }
        int half = -1;
        const byte* p = StreamBlockData(s, streamBlock, half);
        if (p == NULL) {
            if (s_streamDebug.GetInteger() > 1) {
                common->Printf("sound stream: waiting for '%s' block %d\n", s->GetName(), streamBlock);
            }
            break;
        }
        int frames = s->DecodeBlock(p, s->BlockBytes(streamBlock), streamBlockPCM);
        frames = Min(frames, s->playLength - streamBlock * fpb);
        const int n = Min(frames - streamSkip, maxFrames - written);
        if (n > 0) {
            memcpy(dst + written * ch, streamBlockPCM + streamSkip * ch, n * ch * sizeof(int16));
            written += n;
        }
        if (n > 0 && streamSkip + n < frames) {
            streamSkip += n; // the chunk filled up inside this block
        } else {
            if (half >= 0 && streamBlock == streamHalfBlock[half] + streamHalfBlocks[half] - 1) {
                streamHalfConsumed[half] = true;
            }
            streamSkip = 0;
            streamBlock++;
        }
    }
    return written;
}

/*
========================
idSoundVoice_OpenAL::StreamFillQueue
========================
*/
void idSoundVoice_OpenAL::StreamFillQueue(int maxBuffers)
{
    while (numFreeStreamBuffers > 0 && maxBuffers-- > 0 && !streamEnded && streamSample != NULL) {
        // the sample the next chunk will come from
        idSoundSample_OpenAL* s = streamSample;
        if (streamBlock >= s->NumBlocks() || streamBlock * s->FramesPerBlock() >= s->playLength) {
            s = StreamNextSample(s);
            if (s == NULL) {
                streamEnded = true;
                break;
            }
        }
        // Every buffer in an OpenAL queue must share one format, rate included
        // (OpenAL Soft 1.19 refuses the rest). Leadin and loop samples differ by
        // a few Hz (22049 vs 22055): those just play at the queue's rate. A
        // real change waits for the queue to drain.
        const int queuedNow = NUM_STREAM_BUFFERS - numFreeStreamBuffers;
        int rate = s->SampleRate();
        if (queuedNow > 0) {
            if (s->NumChannels() != streamQueueChannels || abs(rate - streamQueueRate) * 20 > streamQueueRate) {
                break;
            }
            rate = streamQueueRate;
        }
        int frames;
        ALenum err = AL_NO_ERROR;
        ALuint buf = 0;
        if (streamDecodeLock != NULL) {
            streamDecodeLock->Lock();
        }
        {
            VITA_ZONE("sound: decode");
            frames = StreamDecodeChunk(streamChunkPCM, StreamChunkFrames(s));
        }
        if (frames > 0) {
            s = streamSample; // what was actually decoded
            buf = freeStreamBuffers[--numFreeStreamBuffers];
            const ALenum fmt = (s->NumChannels() == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
            VITA_ZONE("sound: OpenAL buffer queue");
            alGetError();
            alBufferData(buf, fmt, streamChunkPCM, frames * s->NumChannels() * sizeof(int16), rate);
            alSourceQueueBuffers(openalSource, 1, &buf);
            err = alGetError();
        }
        if (streamDecodeLock != NULL) {
            streamDecodeLock->Unlock();
        }
        if (frames <= 0) {
            break;
        }
        if (err != AL_NO_ERROR) {
            VITA_ZONE_COUNT("sound: OpenAL queue errors");
            freeStreamBuffers[numFreeStreamBuffers++] = buf;
            if (s_streamDebug.GetBool() && dbgALErrors++ < 3) {
                common->Printf("sound stream: OpenAL error 0x%x queueing '%s' (%d frames, %d ch, %d Hz)\n",
                    err, s->GetName(), frames, s->NumChannels(), rate);
            }
            break;
        }
        if (queuedNow == 0) {
            streamQueueRate = rate;
            streamQueueChannels = s->NumChannels();
        }
        dbgChunks++;
    }
    StreamFetch();
    StreamDebugTick();
}

/*
========================
idSoundVoice_OpenAL::StreamReleaseProcessed
========================
*/
void idSoundVoice_OpenAL::StreamReleaseProcessed()
{
    ALint processed = 0;
    alGetSourcei(openalSource, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0 && numFreeStreamBuffers < NUM_STREAM_BUFFERS) {
        ALuint buf = 0;
        alSourceUnqueueBuffers(openalSource, 1, &buf);
        freeStreamBuffers[numFreeStreamBuffers++] = buf;
    }
}

/*
========================
idSoundVoice_OpenAL::Create
========================
*/
void idSoundVoice_OpenAL::Create(const idSoundSample* leadinSample_, const idSoundSample* loopingSample_)
{
    if (IsPlaying()) {
        // This should never hit
        Stop();
        return;
    }
    // (only now: Stop takes the lock itself, and it is not recursive)
    idScopedCriticalSection lock(streamLock);

    triggered = true;
    leadinSample = (idSoundSample_OpenAL*)leadinSample_;
    loopingSample = (idSoundSample_OpenAL*)loopingSample_;

    formatTag = leadinSample->format.basic.formatTag;
    numChannels = leadinSample->format.basic.numChannels;
    sampleRate = leadinSample->format.basic.samplesPerSec;

    if (!alIsSource(openalSource)) {
        CheckALErrors();
        alGenSources(1, &openalSource);
        if (CheckALErrors() != AL_NO_ERROR) {
            openalSource = 0;
            return;
        }
        alSourcef(openalSource, AL_ROLLOFF_FACTOR, 0.0f);
    }
    if (!streamBuffersCreated) {
        CheckALErrors();
        alGenBuffers(NUM_STREAM_BUFFERS, streamBuffers);
        if (CheckALErrors() != AL_NO_ERROR) {
            return;
        }
        streamBuffersCreated = true;
    }

    if (SoundWorker_Running()) {
        StreamClearState();
        streamStopPending = true;
    } else {
        StreamClearQueue();
    }

    alSourcei(openalSource, AL_LOOPING, AL_FALSE);
    alSourcei(openalSource, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(openalSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(openalSource, AL_GAIN, 1.0f);
    ResetALCache();
    sourceVoiceRate = sampleRate;
}

/*
========================
idSoundVoice_OpenAL::DestroyInternal
========================
*/
void idSoundVoice_OpenAL::DestroyInternal()
{
    idScopedCriticalSection lock(streamLock);
    streamActive = false;
    streamStartPending = false;
    streamStopPending = false;
    if (alIsSource(openalSource)) {
        alSourceStop(openalSource);
        alSourcei(openalSource, AL_BUFFER, 0);
        alDeleteSources(1, &openalSource);
    }
    openalSource = 0;
    if (streamBuffersCreated) {
        alDeleteBuffers(NUM_STREAM_BUFFERS, streamBuffers);
        streamBuffersCreated = false;
    }
    numFreeStreamBuffers = 0;
    // a read may still be landing in the ring
    for (int h = 0; h < 2; h++) {
        for (int wait = 0; streamReq[h].state == SSR_PENDING && wait < 2000; wait++) {
            Sys_Sleep(1);
        }
    }
    if (streamRing != NULL && streamReq[0].state != SSR_PENDING && streamReq[1].state != SSR_PENDING) {
        Mem_Free(streamRing);
        streamRing = NULL;
    }
    hasVUMeter = false;
}

/*
========================
idSoundVoice_OpenAL::Start
========================
*/
void idSoundVoice_OpenAL::Start(int offsetMS, int ssFlags)
{
    if (s_debugHardware.GetBool()) {
        idLib::Printf("%dms: %i starting %s @ %dms\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>", offsetMS);
    }
    if (leadinSample == NULL || !alIsSource(openalSource) || !streamBuffersCreated) {
        return;
    }
    if (leadinSample->IsDefault()) {
        idLib::Warning("Starting defaulted sound sample %s", leadinSample->GetName());
    }
    hasVUMeter = (ssFlags & SSF_NO_FLICKER) == 0;

    assert(offsetMS >= 0);
    int offsetSamples = MsecToSamples(offsetMS, leadinSample->SampleRate());
    if (loopingSample == NULL && offsetSamples >= leadinSample->playLength) {
        return;
    }

    idScopedCriticalSection lock(streamLock);
    const bool worker = SoundWorker_Running();
    if (worker) {
        // the worker clears the queue before it queues anything new
        StreamClearState();
        streamStopPending = true;
    } else {
        VITA_ZONE("sound: OpenAL clear queue");
        StreamClearQueue();
    }

    idSoundSample_OpenAL* s = leadinSample;
    if (offsetSamples >= leadinSample->playLength && loopingSample != NULL) {
        s = loopingSample;
        offsetSamples = (loopingSample->playLength > 0) ? (offsetSamples % loopingSample->playLength) : 0;
    }
    if (s->NumBlocks() <= 0 || s->playLength <= 0) {
        return;
    }
    const int fpb = Max(1, s->FramesPerBlock());
    streamSample = s;
    streamBlock = offsetSamples / fpb;
    streamSkip = offsetSamples % fpb;
    streamEnded = false;

    // Starting past the part kept in memory (a looping sound coming back into
    // earshot) queues nothing until the first read lands; the source is then
    // started when it ran dry with buffers queued (StreamKeepPlaying). A frame
    // of silence on a sound fading in beats stalling on the memory card.
    StreamSetFetch(s, streamBlock);
    paused = false;
    streamActive = true;
    dbgStarts++;

    if (worker) {
        // the worker queues the first buffer and plays it (StreamService)
        streamStartPending = true;
        SoundWorker_Wake();
        return;
    }

    StreamFetch();
    // two ~105 ms buffers to start: the next update (a frame later) queues
    // the other two. Decoding all four here put 420 ms of decoding into the
    // frame a sound started - the benchmark's 10+ ms "voice start" spikes.
    StreamFillQueue(2);

    {
        VITA_ZONE("sound: alSourcePlay");
        alSourcePlay(openalSource);
    }
}

/*
========================
idSoundVoice_OpenAL::StreamKeepPlaying

A source that ran dry (a long frame, a slow disk read) stops by itself; once
buffers are queued again it is started again, unless the game paused it.
========================
*/
void idSoundVoice_OpenAL::StreamKeepPlaying()
{
    if (paused) {
        return;
    }
    VITA_ZONE("sound: OpenAL state check");
    ALint state = AL_INITIAL;
    alGetSourcei(openalSource, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING && state != AL_PAUSED) {
        ALint queued = 0;
        alGetSourcei(openalSource, AL_BUFFERS_QUEUED, &queued);
        if (queued > 0) {
            VITA_ZONE_COUNT("sound: sources restarted (queue ran dry)");
            if (s_streamDebug.GetBool()) {
                dbgUnderruns++;
                if (s_streamDebug.GetInteger() > 1) {
                    common->Printf("sound stream: '%s' ran dry, restarting\n", streamSample ? streamSample->GetName() : leadinSample->GetName());
                }
            }
            alSourcePlay(openalSource);
        }
    }
}

/*
========================
idSoundVoice_OpenAL::StreamService

The sound worker's turn at a playing voice (streamLock held): hand finished
buffers back, start a voice Start() set up, decode the next chunks, keep the
source playing. A sound that has queued its last buffer and played it out
needs nothing more until it is started again.
========================
*/
void idSoundVoice_OpenAL::StreamService()
{
    if (streamStopPending) {
        VITA_ZONE("sound: OpenAL clear queue");
        StreamClearAL();
    }
    if (!streamActive) {
        return;
    }
    if (!alIsSource(openalSource) || leadinSample == NULL || !streamBuffersCreated) {
        streamActive = false;
        streamStartPending = false;
        return;
    }
    StreamFetch();
    {
        VITA_ZONE("sound: OpenAL release buffers");
        StreamReleaseProcessed();
    }
    if (streamStartPending) {
        // one buffer, then play: the rest are decoded while it plays
        StreamFillQueue(1);
        if (!paused) {
            VITA_ZONE("sound: alSourcePlay");
            alSourcePlay(openalSource);
        }
        streamStartPending = false;
    }
    StreamFillQueue();
    StreamKeepPlaying();
    if (streamEnded && numFreeStreamBuffers == NUM_STREAM_BUFFERS) {
        streamActive = false;
    }
}

/*
========================
idSoundVoice_OpenAL::Update

Called every sound frame for a voice that is playing: hand finished buffers
back, decode the next chunks into them, and restart the source if the queue
ran dry (a long frame, or a slow disk read).
========================
*/
bool idSoundVoice_OpenAL::Update()
{
    if (SoundWorker_Running()) {
        return true; // the worker keeps the queue full
    }
    if (!alIsSource(openalSource) || leadinSample == NULL || !streamBuffersCreated) {
        return true;
    }
    idScopedCriticalSection lock(streamLock);
    StreamFetch();
    {
        VITA_ZONE("sound: OpenAL release buffers");
        StreamReleaseProcessed();
    }
    StreamFillQueue();
    StreamKeepPlaying();
    return true;
}

/*
========================
idSoundVoice_OpenAL::Stop
========================
*/
void idSoundVoice_OpenAL::Stop()
{
    if (!alIsSource(openalSource)) {
        return;
    }
    if (s_debugHardware.GetBool() && !paused) {
        idLib::Printf("%dms: %i stopping %s\n", Sys_Milliseconds(), openalSource, leadinSample ? leadinSample->GetName() : "<null>");
    }
    idScopedCriticalSection lock(streamLock);
    if (SoundWorker_Running()) {
        // the source plays on until the worker stops it, a few ms from now;
        // until then the voice waits on the zombie list (FreeVoice), which is
        // what that list is for
        StreamClearState();
        streamStopPending = true;
        SoundWorker_Wake();
    } else {
        StreamClearQueue();
    }
    paused = true;
}

/*
========================
idSoundVoice_OpenAL::StreamSelfTest

For each sample: the whole compressed data read in one go and decoded with the
stock MS_ADPCM_decode (the reference), against what a voice produces pulling
small chunks through the head, the disk ring and the reader thread - straight,
looping two and a half times, and starting from an offset.
========================
*/
void idSoundVoice_OpenAL::StreamSelfTest()
{
    const int maxSamples = s_streamSelfTest.GetInteger();
    if (maxSamples <= 0) {
        return;
    }
    int tested = 0, failed = 0, streamedTested = 0;
    int64 framesCompared = 0;
    const int startMs = Sys_Milliseconds();
    for (int i = 0; i < soundSystemLocal.samples.Num() && tested < maxSamples; i++) {
        idSoundSample_OpenAL* s = soundSystemLocal.samples[i];
        if (s == NULL || !s->IsLoaded() || s->IsDefault() || s->NumBlocks() <= 0 || s->playLength <= 0) {
            continue;
        }
        const int ch = s->NumChannels();
        const int total = s->playLength;

        // ---- reference
        byte* all = (byte*)Mem_Alloc(s->dataBytes + 16, TAG_TEMP);
        if (s->IsStreamed()) {
            if (!SoundStream_ReadSync(s->streamPathId, s->streamDataOffset, all, s->dataBytes)) {
                common->Printf("stream selftest: '%s' could not be read\n", s->GetName());
                Mem_Free(all);
                failed++;
                continue;
            }
        } else {
            memcpy(all, s->buffers[0].buffer, s->dataBytes);
        }
        int16* ref = NULL;
        int refFrames = 0;
        if (s->format.basic.formatTag == idWaveFile::FORMAT_ADPCM) {
            uint8* buf = all;
            uint32 len = s->dataBytes;
            if (s->MS_ADPCM_decode(&buf, &len) < 0) { // frees 'all'
                common->Printf("stream selftest: '%s' MS_ADPCM_decode failed\n", s->GetName());
                failed++;
                continue;
            }
            ref = (int16*)buf;
            refFrames = Min((int)(len / (2 * ch)), total);
        } else {
            ref = (int16*)all;
            refFrames = Min(s->dataBytes / (2 * ch), total);
        }

        // ---- streaming decode, three ways
        const int cases = 3;
        bool ok = true;
        for (int c = 0; c < cases && ok; c++) {
            idSoundVoice_OpenAL v;
            v.leadinSample = s;
            v.loopingSample = (c == 1) ? s : NULL;
            const int startFrame = (c == 2) ? (total / 3) : 0;
            const int want = (c == 1) ? (refFrames * 5 / 2) : (refFrames - startFrame);
            v.streamSample = s;
            v.streamBlock = startFrame / s->FramesPerBlock();
            v.streamSkip = startFrame % s->FramesPerBlock();
            v.streamEnded = false;
            v.StreamSetFetch(s, v.streamBlock);
            int got = 0;
            int stalls = 0;
            static int16 out[MAX_CHUNK_FRAMES * 2];
            while (got < want && !v.streamEnded && stalls < 5000) {
                v.StreamFetch();
                if (streamDecodeLock != NULL) {
                    streamDecodeLock->Lock();
                }
                const int n = v.StreamDecodeChunk(out, Min(StreamChunkFrames(s), want - got));
                if (streamDecodeLock != NULL) {
                    streamDecodeLock->Unlock();
                }
                if (n <= 0) {
                    stalls++;
                    Sys_Sleep(1);
                    continue;
                }
                for (int f = 0; f < n && ok; f++) {
                    const int rf = (startFrame + got + f) % refFrames;
                    for (int k = 0; k < ch; k++) {
                        if (out[f * ch + k] != ref[rf * ch + k]) {
                            common->Printf("stream selftest: '%s' case %d differs at frame %d ch %d: %d vs %d (%s, %d blocks, %d resident)\n",
                                s->GetName(), c, startFrame + got + f, k, out[f * ch + k], ref[rf * ch + k],
                                s->IsStreamed() ? "streamed" : "in memory", s->NumBlocks(), s->ResidentBlocks());
                            ok = false;
                            break;
                        }
                    }
                }
                got += n;
                framesCompared += n;
            }
            if (ok && got < want && c != 1) {
                common->Printf("stream selftest: '%s' case %d stopped after %d of %d frames (stalls %d)\n", s->GetName(), c, got, want, stalls);
                ok = false;
            }
            if (ok && c == 1 && got < want) {
                common->Printf("stream selftest: '%s' loop case stopped after %d of %d frames (stalls %d)\n", s->GetName(), got, want, stalls);
                ok = false;
            }
            // let reads in flight land before the voice (and its ring) goes away
            for (int h = 0; h < 2; h++) {
                while (v.streamReq[h].state == SSR_PENDING) {
                    Sys_Sleep(1);
                }
            }
            if (v.streamRing != NULL) {
                Mem_Free(v.streamRing);
                v.streamRing = NULL;
            }
        }
        Mem_Free(ref);
        tested++;
        if (s->IsStreamed()) {
            streamedTested++;
        }
        if (!ok) {
            failed++;
        }
    }
    common->Printf("stream selftest: %d samples (%d streamed), %d failed, %lld frames compared in %d ms\n",
        tested, streamedTested, failed, (long long)framesCompared, Sys_Milliseconds() - startMs);
}

#endif // ID_SOUND_STREAM
