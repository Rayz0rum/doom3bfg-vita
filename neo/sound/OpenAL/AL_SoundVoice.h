/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

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
#ifndef __AL_SOUNDVOICE_H__
#define __AL_SOUNDVOICE_H__

static const int MAX_QUEUED_BUFFERS = 3;

/*
================================================
idSoundVoice_OpenAL
================================================
*/
class idSoundVoice_OpenAL : public idSoundVoice_Base {
public:
    idSoundVoice_OpenAL();
    ~idSoundVoice_OpenAL();

    // Every voice gets all three every frame; each OpenAL call takes OpenAL
    // Soft's context and source-list locks, so a value OpenAL already has is
    // not sent again (alPosition/alGain/alPitch: what it was last told).
    void SetPosition(const idVec3& p)
    {
        idSoundVoice_Base::SetPosition(p);

        const idVec3 al(-p.y, p.z, -p.x);
        if (al != alPosition) {
            alPosition = al;
            alSource3f(openalSource, AL_POSITION, al.x, al.y, al.z);
        }
    }

    void SetGain(float gain)
    {
        idSoundVoice_Base::SetGain(gain);

        const float al = (gain) < (1.0f) ? (gain) : (1.0f);
        if (al != alGain) {
            alGain = al;
            alSourcef(openalSource, AL_GAIN, al);
        }
    }

    void SetPitch(float p)
    {
        idSoundVoice_Base::SetPitch(p);

        if (p != alPitch) {
            alPitch = p;
            alSourcef(openalSource, AL_PITCH, p);
        }
    }

    void Create(const idSoundSample* leadinSample, const idSoundSample* loopingSample);

    // Start playing at a particular point in the buffer.  Does an Update() too
    void Start(int offsetMS, int ssFlags);

    // Stop playing.
    void Stop();

    // Stop consuming buffers
    void Pause();

    // Start consuming buffers again
    void UnPause();

    // Sends new position/volume/pitch information to the hardware
    bool Update();

    // returns the RMS levels of the most recently processed block of audio, SSF_FLICKER must have been passed to Start
    float GetAmplitude();

    // returns true if we can re-use this voice
    bool CompatibleFormat(idSoundSample_OpenAL* s);

    uint32 GetSampleRate() const
    {
        return sampleRate;
    }

    // callback function
    void OnBufferStart(idSoundSample_OpenAL* sample, int bufferNumber);

private:
    friend class idSoundHardware_OpenAL;

    // Returns true when all the buffers are finished processing
    bool IsPlaying();

    // Called after the voice has been stopped
    void FlushSourceBuffers();

    // Destroy the internal hardware resource
    void DestroyInternal();

    // Helper function used by the initial start as well as for looping a streamed buffer
    int RestartAt(int offsetSamples);

    // Helper function to submit a buffer
    int SubmitBuffer(idSoundSample_OpenAL* sample, int bufferNumber, int offset);

    // Adjust the voice frequency based on the new sample rate for the buffer
    void SetSampleRate(uint32 newSampleRate, uint32 operationSet);

    // what the source was last set to (see SetPosition); Create() sets
    // position and gain, and makes the next pitch go through
    idVec3 alPosition;
    float alGain;
    float alPitch;
    void ResetALCache()
    {
        alPosition.Zero();
        alGain = 1.0f;
        alPitch = -1.0f; // no pitch is negative: the next SetPitch is sent
    }

#if defined(ID_SOUND_STREAM)
    // decode-as-you-play (AL_SoundStream.cpp)
    static const int NUM_STREAM_BUFFERS = 4;
    void StreamClearQueue(); // both of these
    void StreamClearAL();    // stops the source and takes every buffer back
    void StreamClearState(); // forgets what was being decoded and read
    idSoundSample_OpenAL* StreamNextSample(idSoundSample_OpenAL* s) const;
    void StreamSetFetch(idSoundSample_OpenAL* s, int block);
    void StreamFetch();
    const byte* StreamBlockData(idSoundSample_OpenAL* s, int block, int& half);
    int StreamDecodeChunk(int16* dst, int maxFrames);
    void StreamFillQueue(int maxBuffers = NUM_STREAM_BUFFERS);
    void StreamReleaseProcessed();
    void StreamKeepPlaying();

    // The sound worker thread (AL_SoundStream.cpp) keeps a playing voice's
    // queue full - the decoding, the disk reads, the OpenAL buffers - while
    // the game/draw thread only starts, stops, pauses and steers it. The lock
    // guards the stream state (everything in this block) and the source's
    // queue and play state between the two.
    idSysMutex streamLock;
    volatile bool streamActive;       // there is something to keep queued
    volatile bool streamStartPending; // Start() set it up: the worker queues the first buffers and plays
    volatile bool streamStopPending;  // the worker stops the source and takes its buffers back (StreamClearAL)

public:
    // the worker's turn at this voice, under streamLock
    void StreamService();

private:

    ALuint streamBuffers[NUM_STREAM_BUFFERS];
    ALuint freeStreamBuffers[NUM_STREAM_BUFFERS];
    int numFreeStreamBuffers;
    bool streamBuffersCreated;

    idSoundSample_OpenAL* streamSample; // being decoded
    int streamQueueRate;                // format of what is queued in OpenAL
    int streamQueueChannels;
    int streamBlock;
    int streamSkip; // frames of streamBlock already used
    bool streamEnded;

    // two halves of a ring that the reader thread fills for streamed samples
    byte* streamRing;
    soundStreamRequest_t streamReq[2];
    idSoundSample_OpenAL* streamHalfSample[2];
    int streamHalfBlock[2];
    int streamHalfBlocks[2];
    bool streamHalfValid[2];
    bool streamHalfConsumed[2];
    idSoundSample_OpenAL* fetchSample; // what the reader brings in next
    int fetchBlock;
    bool fetchDone;

public:
    // decodes every loaded sample through the streaming path and compares it
    // with a straight decode of the whole file (s_streamSelfTest)
    static void StreamSelfTest();
#endif

    // IXAudio2SourceVoice* 	pSourceVoice;
    bool triggered;
    ALuint openalSource;
    ALuint openalStreamingOffset;
    ALuint openalStreamingBuffer[3];
    ALuint lastopenalStreamingBuffer[3];

    idSoundSample_OpenAL* leadinSample;
    idSoundSample_OpenAL* loopingSample;

    // These are the fields from the sample format that matter to us for voice reuse
    uint16 formatTag;
    uint16 numChannels;

    uint32 sourceVoiceRate;
    uint32 sampleRate;

    bool hasVUMeter;
    bool paused;
};

/*
================================================
idSoundVoice
================================================
*/
class idSoundVoice : public idSoundVoice_OpenAL {
};

#endif
