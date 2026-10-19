/*
===========================================================================

Doom 3 BFG Edition GPL Source Code

Compressed, streamed sound playback (used on the PS Vita).

The stock OpenAL backend decodes every MS-ADPCM sample to 16-bit PCM when it is
loaded, keeps that PCM, and hands OpenAL a second copy - about 7.3 bytes of
memory for every byte of sound data. A campaign map references some 800
samples (mars_city2: 41 MB of ADPCM), which came to ~300 MB and could not fit.

With ID_SOUND_STREAM, samples stay compressed:
  - small samples keep their ADPCM data in memory;
  - big ones (ambience beds, voice-overs, audio logs) keep only their first few
    KB in memory and the rest is read from the .resources container on a
    background thread while they play;
  - every voice decodes its sample a block at a time into a short queue of
    OpenAL buffers, refilled each sound frame.
Memory for sound becomes roughly the size of the small samples plus a few KB
per big one - about 9 MB for mars_city2 instead of 300.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/
#ifndef __AL_SOUNDSTREAM_H__
#define __AL_SOUNDSTREAM_H__

#if defined(ID_SOUND_STREAM)

/*
================================================
Background reads from .resources containers
================================================
*/
enum {
    SSR_IDLE = 0,
    SSR_PENDING = 1,
    SSR_DONE = 2
};

struct soundStreamRequest_t {
    volatile int state; // SSR_*
    int pathId;
    int offset; // absolute, in the container file
    int size;
    byte* dest;
    volatile int result; // bytes read; 0 on failure
};

void SoundStream_Init();
void SoundStream_Shutdown();

// Container files are referred to by a small integer. Thread safe.
int SoundStream_RegisterPath(const char* osPath);

// Blocking read on the calling thread. Thread safe.
bool SoundStream_ReadSync(int pathId, int offset, void* dest, int size);

// Queues req for the reader thread; req->state goes PENDING, then DONE. The
// caller must not touch req or its dest buffer until it sees DONE.
void SoundStream_Submit(soundStreamRequest_t* req);

// After seeing req->state == SSR_DONE, before reading req->dest.
void SoundStream_AcquireBarrier();

/*
================================================
The sound worker thread: decodes and queues for the playing voices
================================================
*/
void SoundWorker_Start(); // idSoundHardware_OpenAL::Init, once the voices exist
void SoundWorker_Stop();  // idSoundHardware_OpenAL::Shutdown, before they go
bool SoundWorker_Running();
void SoundWorker_Wake(); // a voice is waiting to start

/*
================================================
Memory accounting, for the logs
================================================
*/
void SoundStream_AccountSample(int residentBytes, bool streamed);
void SoundStream_PrintStats(const char* when);

#endif // ID_SOUND_STREAM

#endif // !__AL_SOUNDSTREAM_H__
