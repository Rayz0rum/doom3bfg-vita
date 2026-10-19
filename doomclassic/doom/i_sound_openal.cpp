/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Felix Rueegg

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

#include "Precompiled.h"
#include "globaldata.h"

//
// DESCRIPTION:
//	System interface for sound.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <sys/types.h>
#include <fcntl.h>
// Timer stuff. Experimental.
#include <time.h>
#include <signal.h>
#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "d_main.h"
#include "doomdef.h"
#include "libs/timidity/timidity.h"
#include "libs/timidity/controls.h"

#include "sound/snd_local.h"

#pragma warning ( disable : 4244 )

#define SFX_RATE		11050
#define SFX_SAMPLETYPE		AL_FORMAT_MONO8

#define MIDI_CHANNELS		2
#define MIDI_RATE		22050
#if defined(__vita__)
// 16-bit: the music is synthesised (Timidity), not a recording, and at 8 bits
// its quantisation noise was the faint "bitty", crunchy sound under it
#define MIDI_SAMPLETYPE		AL_FORMAT_STEREO16
#define MIDI_FORMAT		AUDIO_S16
#define MIDI_FORMAT_BYTES	2
#else
#define MIDI_SAMPLETYPE		AL_FORMAT_STEREO8
#define MIDI_FORMAT		AUDIO_U8
#define MIDI_FORMAT_BYTES	1
#endif

ALuint		alMusicSourceVoice;
ALuint		alMusicBuffer;

MidiSong*	doomMusic;
byte*		musicBuffer;
int		totalBufferSize;

bool		waitingForMusic;
bool		musicReady;

typedef struct
{
	float x;
	float y;
	float z;
} vec3_t;

typedef struct
{
	vec3_t OrientTop;
	vec3_t OrientFront;
	vec3_t Position;
} doomListener_t;

typedef struct tagActiveSound_t
{
	ALuint alSourceVoice;
	int id;
	int valid;
	int start;
	int player;
	bool localSound;
	mobj_t* originator;
} activeSound_t;

// cheap little struct to hold a sound
typedef struct
{
	int vol;
	int player;
	int pitch;
	int priority;
	mobj_t* originator;
	mobj_t* listener;
} soundEvent_t;

// array of all the possible sounds
// in split screen we only process the loudest sound of each type per frame
soundEvent_t soundEvents[128];
extern int PLAYERCOUNT;

// Source voice settings for all sound effects
const ALfloat		SFX_MAX_DISTANCE = 1200.f;
const ALfloat		SFX_REFERENCE_DISTANCE = 100.f;
const ALfloat		SFX_ROLLOFF_FACTOR = 0.2f;

// Real volumes
const float		GLOBAL_VOLUME_MULTIPLIER = 0.5f;

float			x_SoundVolume = GLOBAL_VOLUME_MULTIPLIER;
float			x_MusicVolume = GLOBAL_VOLUME_MULTIPLIER;

// The actual lengths of all sound effects.
static int 		lengths[NUMSFX];
ALuint			alBuffers[NUMSFX];
activeSound_t		activeSounds[NUM_SOUNDBUFFERS] = {0};

int			S_initialized = 0;
bool			Music_initialized = false;
static bool		soundHardwareInitialized = false;
static int		numOutputChannels = 0;

doomListener_t		doom_Listener;

void			I_InitSoundChannel( int channel, int numOutputChannels_ );

/*
======================
getsfx
======================
*/
// This function loads the sound data from the WAD lump,
// for single sound.
//
void* getsfx( const char* sfxname, int* len )
{
	unsigned char*      sfx;
	unsigned char*	    sfxmem;
	int                 size;
	char                name[20];
	int                 sfxlump;
	//float               scale = 1.0f;

	// Get the sound data from the WAD
	idStr::snPrintf( name, sizeof( name ), "ds%s", sfxname );

	// Scale down the plasma gun, it clips
	//if ( strcmp( sfxname, "plasma" ) == 0 ) {
	//	scale = 0.75f;
	//}
	//if ( strcmp( sfxname, "itemup" ) == 0 ) {
	//	scale = 1.333f;
	//}

	// If sound requested is not found in current WAD, use pistol as default
	if( W_CheckNumForName( name ) == -1 )
	{
		sfxlump = W_GetNumForName( "dspistol" );
	}
	else
	{
		sfxlump = W_GetNumForName( name );
	}

	// Sound lump headers are 8 bytes.
	const int SOUND_LUMP_HEADER_SIZE_IN_BYTES = 8;

	size = W_LumpLength( sfxlump ) - SOUND_LUMP_HEADER_SIZE_IN_BYTES;

	sfx = ( unsigned char* )W_CacheLumpNum( sfxlump, PU_CACHE_SHARED );
	const unsigned char* sfxSampleStart = sfx + SOUND_LUMP_HEADER_SIZE_IN_BYTES;

	// Allocate from zone memory.
	//sfxmem = (float*)DoomLib::Z_Malloc( size*(sizeof(float)), PU_SOUND_SHARED, 0 );
	sfxmem = ( unsigned char* )malloc( size * sizeof( unsigned char ) );

	// Now copy, and convert to Xbox360 native float samples, do initial volume ramp, and scale
	for( int i = 0; i < size; i++ )
	{
		sfxmem[i] = sfxSampleStart[i];// * scale;
	}

	// Remove the cached lump.
	Z_Free( sfx );

	// Set length.
	*len = size;

	// Return allocated padded data.
	return ( void* )( sfxmem );
}

/*
======================
I_SetChannels
======================
*/
void I_SetChannels()
{
	// Original Doom set up lookup tables here
}

/*
======================
I_SetSfxVolume
======================
*/
void I_SetSfxVolume( int volume )
{
	x_SoundVolume = ( ( float )volume / 15.f ) * GLOBAL_VOLUME_MULTIPLIER;
}

/*
======================
I_GetSfxLumpNum
======================
*/
//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum( sfxinfo_t* sfx )
{
	char namebuf[9];
	idStr::snPrintf( namebuf, sizeof( namebuf ), "ds%s", sfx->name );
	return W_GetNumForName( namebuf );
}

/*
======================
I_StartSound2
======================
*/
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback) is set
//
int I_StartSound2( int id, int player, mobj_t* origin, mobj_t* listener_origin, int pitch, int priority )
{
	if( !soundHardwareInitialized || id == 0 )
	{
		return id;
	}

	int i;
	activeSound_t* sound = 0;
	int oldest = 0, oldestnum = -1;

	// these id's should not overlap
	if( id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit || id == sfx_stnmov )
	{
		// Loop all channels, check.
		for( i = 0; i < NUM_SOUNDBUFFERS; i++ )
		{
			sound = &activeSounds[i];

			if( sound->valid && ( sound->id == id && sound->player == player ) )
			{
				I_StopSound( sound->id, player );
				break;
			}
		}
	}

	// find a valid channel, or one that has finished playing
	for( i = 0; i < NUM_SOUNDBUFFERS; i++ )
	{
		sound = &activeSounds[i];

		if( !sound->valid )
		{
			break;
		}

		if( !oldest || oldest > sound->start )
		{
			oldestnum = i;
			oldest = sound->start;
		}

		ALint sourceState;
		alGetSourcei( sound->alSourceVoice, AL_SOURCE_STATE, &sourceState );
		if( sourceState == AL_STOPPED )
		{
			break;
		}
	}

	// none found, so use the oldest one
	if( i == NUM_SOUNDBUFFERS )
	{
		i = oldestnum;
		sound = &activeSounds[i];
	}

	alSourceStop( sound->alSourceVoice );

	// Attach the source voice to the correct buffer
	if( sound->id != id )
	{
		alSourcei( sound->alSourceVoice, AL_BUFFER, 0 );
		alSourcei( sound->alSourceVoice, AL_BUFFER, alBuffers[id] );
	}

	// Set the source voice volume
	alSourcef( sound->alSourceVoice, AL_GAIN, x_SoundVolume );

	// Set the source voice pitch
	alSourcef( sound->alSourceVoice, AL_PITCH, 1 + ( ( float )pitch - 128.f ) / 95.f );

	// Set the source voice position
	ALfloat x = 0.f;
	ALfloat y = 0.f;
	ALfloat z = 0.f;
	if( origin )
	{
		if( origin == listener_origin )
		{
			sound->localSound = true;
		}
		else
		{
			sound->localSound = false;
			x = ( ALfloat )( origin->x >> FRACBITS );
			z = ( ALfloat )( origin->y >> FRACBITS );
		}
	}
	else
	{
		sound->localSound = true;
	}
	if( sound->localSound )
	{
		x = doom_Listener.Position.x;
		z = doom_Listener.Position.z;
	}
	alSource3f( sound->alSourceVoice, AL_POSITION, x, y, z );

	alSourcePlay( sound->alSourceVoice );

	// Set id, and start time
	sound->id = id;
	sound->start = ::g->gametic;
	sound->valid = 1;
	sound->player = player;
	sound->originator = origin;

	return id;
}

/*
======================
I_ProcessSoundEvents
======================
*/
void I_ProcessSoundEvents( void )
{
	for( int i = 0; i < 128; i++ )
	{
		if( soundEvents[i].pitch )
		{
			I_StartSound2( i, soundEvents[i].player, soundEvents[i].originator, soundEvents[i].listener,
						   soundEvents[i].pitch, soundEvents[i].priority );
		}
	}
	memset( soundEvents, 0, sizeof( soundEvents ) );
}

/*
======================
I_StartSound
======================
*/
int I_StartSound( int id, mobj_t* origin, mobj_t* listener_origin, int vol, int pitch, int priority )
{
	// only allow player 0s sounds in intermission and finale screens
	if( ::g->gamestate != GS_LEVEL && DoomLib::GetPlayer() != 0 )
	{
		return 0;
	}

	// if we're only one player or we're trying to play the chainsaw sound, do it normal
	// otherwise only allow one sound of each type per frame
	if( PLAYERCOUNT == 1 || id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit )
	{
		return I_StartSound2( id, ::g->consoleplayer, origin, listener_origin, pitch, priority );
	}
	else
	{
		if( soundEvents[ id ].vol < vol )
		{
			soundEvents[ id ].player = DoomLib::GetPlayer();
			soundEvents[ id ].pitch = pitch;
			soundEvents[ id ].priority = priority;
			soundEvents[ id ].vol = vol;
			soundEvents[ id ].originator = origin;
			soundEvents[ id ].listener = listener_origin;
		}
		return id;
	}
}

/*
======================
I_StopSound
======================
*/
void I_StopSound( int handle, int player )
{
	// You need the handle returned by StartSound.
	// Would be looping all channels,
	// tracking down the handle,
	// and setting the channel to zero.
	int i;
	activeSound_t* sound = 0;

	for( i = 0; i < NUM_SOUNDBUFFERS; ++i )
	{
		sound = &activeSounds[i];
		if( !sound->valid || sound->id != handle || ( player >= 0 && sound->player != player ) )
		{
			continue;
		}
		break;
	}

	if( i == NUM_SOUNDBUFFERS )
	{
		return;
	}

	// Stop the sound
	alSourceStop( sound->alSourceVoice );

	sound->valid = 0;
	sound->player = -1;
}

/*
======================
I_SoundIsPlaying
======================
*/
int I_SoundIsPlaying( int handle )
{
	if( !soundHardwareInitialized )
	{
		return 0;
	}

	int i;
	activeSound_t* sound;

	for( i = 0; i < NUM_SOUNDBUFFERS; ++i )
	{
		sound = &activeSounds[i];
		if( !sound->valid || sound->id != handle )
		{
			continue;
		}

		ALint sourceState;
		alGetSourcei( sound->alSourceVoice, AL_SOURCE_STATE, &sourceState );
		if( sourceState == AL_PLAYING )
		{
			return 1;
		}
	}

	return 0;
}

/*
======================
I_UpdateSound
======================
*/
// Update listener position and go through all the
// channels and update sound positions.
void I_UpdateSound( void )
{
	if( !soundHardwareInitialized )
	{
		return;
	}

	// Update listener orientation and position
	mobj_t* playerObj = ::g->players[0].mo;
	if( playerObj )
	{
		angle_t	pAngle = playerObj->angle;
		fixed_t fx, fz;

		pAngle >>= ANGLETOFINESHIFT;

		fx = finecosine[pAngle];
		fz = finesine[pAngle];

		doom_Listener.OrientFront.x = ( float )( fx ) / 65535.f;
		doom_Listener.OrientFront.y = 0.f;
		doom_Listener.OrientFront.z = ( float )( fz ) / 65535.f;
		doom_Listener.Position.x = ( float )( playerObj->x >> FRACBITS );
		doom_Listener.Position.y = 0.f;
		doom_Listener.Position.z = ( float )( playerObj->y >> FRACBITS );
	}
	else
	{
		doom_Listener.OrientFront.x = 0.f;
		doom_Listener.OrientFront.y = 0.f;
		doom_Listener.OrientFront.z = 1.f;

		doom_Listener.Position.x = 0.f;
		doom_Listener.Position.y = 0.f;
		doom_Listener.Position.z = 0.f;
	}

	ALfloat listenerOrientation[] = { doom_Listener.OrientFront.x, doom_Listener.OrientFront.y,
									  doom_Listener.OrientFront.z, doom_Listener.OrientTop.x, doom_Listener.OrientTop.y,
									  doom_Listener.OrientTop.z
									};
	alListenerfv( AL_ORIENTATION, listenerOrientation );
	alListener3f( AL_POSITION, doom_Listener.Position.x, doom_Listener.Position.y, doom_Listener.Position.z );

	// Update playing source voice positions
	int i;
	activeSound_t* sound;
	for( i = 0; i < NUM_SOUNDBUFFERS; i++ )
	{
		sound = &activeSounds[i];

		if( !sound->valid )
		{
			continue;
		}

		ALint sourceState;
		alGetSourcei( sound->alSourceVoice, AL_SOURCE_STATE, &sourceState );
		if( sourceState == AL_PLAYING )
		{
			if( sound->localSound )
			{
				alSource3f( sound->alSourceVoice, AL_POSITION, doom_Listener.Position.x,
							doom_Listener.Position.y, doom_Listener.Position.z );
			}
			else
			{
				ALfloat x = ( ALfloat )( sound->originator->x >> FRACBITS );
				ALfloat y = 0.f;
				ALfloat z = ( ALfloat )( sound->originator->y >> FRACBITS );

				alSource3f( sound->alSourceVoice, AL_POSITION, x, y, z );
			}
		}
	}
}

/*
======================
I_UpdateSoundParams
======================
*/
void I_UpdateSoundParams( int handle, int vol, int sep, int pitch )
{
}

/*
======================
I_ShutdownSound
======================
*/
void I_ShutdownSound( void )
{
	int done = 0;
	int i;

	if( S_initialized )
	{
		// Stop all sounds
		for( i = 0; i < NUM_SOUNDBUFFERS; i++ )
		{
			activeSound_t* sound = &activeSounds[i];

			if( !sound )
			{
				continue;
			}

			I_StopSound( sound->id, 0 );

			// SRS - Detach buffers before freeing memory to allow reallocation in I_InitSound()
			alSourcei( sound->alSourceVoice, AL_BUFFER, 0 );
		}

		// Free allocated sound memory
		for( i = 1; i < NUMSFX; i++ )
		{
			if( S_sfx[i].data && !( S_sfx[i].link ) )
			{
				free( S_sfx[i].data );
			}
		}
	}

	I_StopSong( 0 );

	S_initialized = 0;
}

/*
======================
I_InitSoundHardware

Called from the tech4x initialization code. Sets up Doom classic's
sound channels.
======================
*/
void I_InitSoundHardware( int numOutputChannels_, int channelMask )
{
	::numOutputChannels = numOutputChannels_;

	// Initialize source voices
	for( int i = 0; i < NUM_SOUNDBUFFERS; i++ )
	{
		I_InitSoundChannel( i, numOutputChannels );
	}

	// Create OpenAL buffers for all sounds
	for( int i = 1; i < NUMSFX; i++ )
	{
		alGenBuffers( ( ALuint )1, &alBuffers[i] );
	}

	I_InitMusic();

	soundHardwareInitialized = true;
}

/*
======================
I_ShutdownSoundHardware

Called from the tech4x shutdown code. Tears down Doom classic's
sound channels.
======================
*/
void I_ShutdownSoundHardware()
{
	soundHardwareInitialized = false;

	I_ShutdownMusic();

	// Delete all source voices
	for( int i = 0; i < NUM_SOUNDBUFFERS; ++i )
	{
		activeSound_t* sound = &activeSounds[i];

		if( !sound )
		{
			continue;
		}

		if( alIsSource( sound->alSourceVoice ) )
		{
			alSourceStop( sound->alSourceVoice );
			alSourcei( sound->alSourceVoice, AL_BUFFER, 0 );
			alDeleteSources( 1, &sound->alSourceVoice );
		}
	}

	// Delete OpenAL buffers for all sounds
	for( int i = 0; i < NUMSFX; i++ )
	{
		if( alIsBuffer( alBuffers[i] ) )
		{
			alDeleteBuffers( 1, &alBuffers[i] );
		}
	}
}

/*
======================
I_InitSoundChannel
======================
*/
#if defined(__vita__)
/*
======================
I_VitaSmoothResampler

The effects are DOOM's own 8-bit 11 kHz samples, played at the 48 kHz output
rate. With OpenAL Soft's point or linear resampling the images of the 11 kHz
spectrum stay in - a gritty edge on every sound; cubic interpolation filters
them, for a handful of voices at most. Returns -1 to leave the default.
======================
*/
static int I_VitaSmoothResampler()
{
	static int chosen = -2;
	if( chosen != -2 )
	{
		return chosen;
	}
	chosen = -1;
	if( !alIsExtensionPresent( "AL_SOFT_source_resampler" ) )
	{
		idLib::Printf( "vita: classic sound - no AL_SOFT_source_resampler, default resampling\n" );
		return chosen;
	}
	LPALGETSTRINGISOFT getStringi = ( LPALGETSTRINGISOFT )alGetProcAddress( "alGetStringiSOFT" );
	const ALint count = alGetInteger( AL_NUM_RESAMPLERS_SOFT );
	const ALint def = alGetInteger( AL_DEFAULT_RESAMPLER_SOFT );
	for( ALint i = 0; getStringi != NULL && i < count; i++ )
	{
		const char* name = getStringi( AL_RESAMPLER_NAME_SOFT, i );
		if( name != NULL && ( idStr::FindText( name, "ubic", false ) >= 0 ) )
		{
			chosen = i;
		}
	}
	idLib::Printf( "vita: classic sound effects resampled with '%s' (default '%s')\n",
				   ( chosen >= 0 && getStringi ) ? getStringi( AL_RESAMPLER_NAME_SOFT, chosen ) : "default",
				   ( def >= 0 && getStringi ) ? getStringi( AL_RESAMPLER_NAME_SOFT, def ) : "?" );
	return chosen;
}
#endif

void I_InitSoundChannel( int channel, int numOutputChannels_ )
{
	activeSound_t* soundchannel = &activeSounds[ channel ];

	alGenSources( ( ALuint )1, &soundchannel->alSourceVoice );
#if defined(__vita__)
	if( I_VitaSmoothResampler() >= 0 )
	{
		alSourcei( soundchannel->alSourceVoice, AL_SOURCE_RESAMPLER_SOFT, I_VitaSmoothResampler() );
	}
#endif

	alSource3f( soundchannel->alSourceVoice, AL_VELOCITY, 0.f, 0.f, 0.f );
	alSourcei( soundchannel->alSourceVoice, AL_LOOPING, AL_FALSE );
	alSourcef( soundchannel->alSourceVoice, AL_MAX_DISTANCE, SFX_MAX_DISTANCE );
	alSourcef( soundchannel->alSourceVoice, AL_REFERENCE_DISTANCE, SFX_REFERENCE_DISTANCE );
	alSourcef( soundchannel->alSourceVoice, AL_ROLLOFF_FACTOR, SFX_ROLLOFF_FACTOR );
}

/*
======================
I_InitSound
======================
*/
void I_InitSound()
{
	if( S_initialized == 0 )
	{
		// Set up listener parameters
		doom_Listener.OrientFront.x	= 0.f;
		doom_Listener.OrientFront.y	= 0.f;
		doom_Listener.OrientFront.z	= 1.f;

		doom_Listener.OrientTop.x	= 0.f;
		doom_Listener.OrientTop.y	= -1.f;
		doom_Listener.OrientTop.z	= 0.f;

		doom_Listener.Position.x	= 0.f;
		doom_Listener.Position.y	= 0.f;
		doom_Listener.Position.z	= 0.f;

		for( int i = 1; i < NUMSFX; i++ )
		{
			// Alias? Example is the chaingun sound linked to pistol.
			if( !S_sfx[i].link )
			{
				// Load data from WAD file.
				S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
			}
			else
			{
				// Previously loaded already?
				S_sfx[i].data = S_sfx[i].link->data;
				lengths[i] = lengths[( S_sfx[i].link - S_sfx ) / sizeof( sfxinfo_t ) ];
			}
			if( S_sfx[i].data )
			{
				alBufferData( alBuffers[i], SFX_SAMPLETYPE, ( byte* )S_sfx[i].data, lengths[i], SFX_RATE );
			}
		}

		S_initialized = 1;
	}
}

/*
======================
I_SubmitSound
======================
*/
void I_SubmitSound( void )
{
	// Only do this for player 0, it will still handle positioning
	//		for other players, but it can't be outside the game
	//		frame like the soundEvents are.
	if( DoomLib::GetPlayer() == 0 )
	{
		// Do 3D positioning of sounds
		I_UpdateSound();

		// Change music if required
		I_UpdateMusic();
	}
}


// =========================================================
// =========================================================
// Background Music
// =========================================================
// =========================================================

/*
======================
I_SetMusicVolume
======================
*/
void I_SetMusicVolume( int volume )
{
	x_MusicVolume = ( float )volume / 15.f;
}

/*
================================================================================

The music, streamed

Stock BFG rendered a whole song with Timidity into one buffer (22 kHz 16-bit
stereo: ~16 MB for a three-minute track), then alBufferData copied it again.
On the Vita, after hours of DOOM 3, that malloc returned NULL and Timidity
wrote the song to address 0 (the full verification's crash, 2026-10-19). Now
the song is synthesised as it plays: MUSIC_BUFFERS small buffers queued on
the music source, refilled by a worker thread (core 3 on the Vita - nothing
waits on it, the queue holds MUSIC_BUFFERS * MUSIC_BUFFER_MS of sound). The
song loops by restarting Timidity at its end, as the stock AL_LOOPING did.
Stats per song (synthesis cost, underruns) go to the log and to
I_MusicStats() for the verification.

================================================================================
*/
uint64 Sys_Microseconds();
void Sys_Sleep( int msec );

#define MUSIC_BUFFERS		6
#define MUSIC_BUFFER_MS		350
#define MUSIC_BUFFER_SAMPLES	( MIDI_RATE * MUSIC_BUFFER_MS / 1000 )
#define MUSIC_BUFFER_BYTES	( MUSIC_BUFFER_SAMPLES * MIDI_CHANNELS * MIDI_FORMAT_BYTES )

static ALuint		musicStreamBuffers[MUSIC_BUFFERS];
// Timidity's block is MUSIC_BUFFER_SAMPLES (Timidity_Init's last size) and
// one call may finish a block and flush the song's end: room for two
static byte		musicChunk[MUSIC_BUFFER_BYTES * 2];
static idSysMutex	musicLock;		// Timidity and the music source's queue
static uintptr_t	musicThread = 0;
static volatile bool	musicThreadQuit = false;
static bool		musicStreaming = false;	// a song is loaded and being fed
static bool		musicPaused = false;
static bool		musicLooping = true;
static bool		musicEnded = false;		// not looping: the song's end was fed
static idStr		musicName;
// this song's figures
static int			musicChunks = 0;
static int			musicUnderruns = 0;
static uint64		musicStartUs = 0;		// when the song was queued
static uint64		musicFirstUnderrunUs = 0;	// the first gap, from the song's start (0 none)
static int			musicUnderrunQueued = 0;	// buffers still queued when it was found
static int			musicLoops = 0;
static uint64		musicSynthUs = 0;
static uint64		musicWorstChunkUs = 0;
static int			musicPeak = 0;		// the loudest sample (0..32767), to tell sound from silence or garbage
static double		musicSumSquares = 0.0;
static int64		musicSamplesMeasured = 0;
// the session's, for the verification
static int			musicSongsTotal = 0;
static int			musicUnderrunsTotal = 0;
static int			musicChunksTotal = 0;
static uint64		musicSynthUsTotal = 0;

// one buffer's worth of the song (musicLock held); false once a song that
// does not loop has ended
static bool I_MusicFillChunk( ALuint buffer )
{
	if( musicEnded )
	{
		return false;
	}
	const uint64 t0 = Sys_Microseconds();
	// One block per call, exactly Timidity's block size: a call asked for
	// less still writes a whole block once its buffer fills (that overran
	// this buffer in the first version - the PC test caught it), and one
	// asked for exactly a block writes at most a block plus the song's end.
	// A call that only fills Timidity's own buffer writes nothing: again.
	int filled = 0;
	int guard = 0;
	while( filled == 0 && guard++ < 8 )
	{
		int written = 0;
		const int rc = Timidity_PlaySome( musicChunk, MUSIC_BUFFER_SAMPLES, &written );
		filled = written;
		if( filled > ( int )sizeof( musicChunk ) )
		{
			I_Error( "classic music: Timidity wrote %d bytes into a %d byte buffer", filled, ( int )sizeof( musicChunk ) );
		}
		if( rc == RC_TUNE_END || ( written == 0 && !Timidity_Active() ) )
		{
			if( !musicLooping )
			{
				musicEnded = true;
				break;
			}
			// the stock source looped the whole buffer: start the song over
			Timidity_Start( doomMusic );
			musicLoops++;
		}
	}
	if( filled <= 0 )
	{
		return false;
	}
	// its level: one sample in 8 (the log's peak and RMS per song)
#if MIDI_FORMAT_BYTES == 2
	const short* pcm = ( const short* )musicChunk;
	const int count = filled / 2;
	for( int i = 0; i < count; i += 8 )
	{
		const int v = pcm[i];
#else
	const int count = filled;
	for( int i = 0; i < count; i += 8 )
	{
		const int v = ( ( int )musicChunk[i] - 128 ) * 256;
#endif
		musicPeak = Max( musicPeak, abs( v ) );
		musicSumSquares += ( double )v * v;
		musicSamplesMeasured++;
	}
	alBufferData( buffer, MIDI_SAMPLETYPE, musicChunk, filled, MIDI_RATE );
	const uint64 us = Sys_Microseconds() - t0;
	musicSynthUs += us;
	musicWorstChunkUs = Max( musicWorstChunkUs, us );
	musicChunks++;
	return true;
}

// the log line for the song that ends (musicLock held)
static void I_MusicReport( const char* why )
{
	if( musicName.IsEmpty() || musicChunks == 0 )
	{
		return;
	}
	const double audioMs = ( double )musicChunks * MUSIC_BUFFER_MS;
	idLib::Printf( "classic music: %s %s after %.1f s - %d chunks of %d ms synthesised, %.1f%% of a core (worst chunk %.1f ms), %d loops, %d underruns (the queue ran dry), level peak %.0f%% RMS %.1f%% of full scale\n",
			  musicName.c_str(), why, audioMs / 1000.0, musicChunks, MUSIC_BUFFER_MS, 100.0 * ( musicSynthUs / 1000.0 ) / audioMs, musicWorstChunkUs / 1000.0, musicLoops, musicUnderruns,
			  100.0 * musicPeak / 32768.0, musicSamplesMeasured ? 100.0 * sqrt( musicSumSquares / musicSamplesMeasured ) / 32768.0 : 0.0 );
	if( musicFirstUnderrunUs != 0 )
	{
		idLib::Printf( "classic music: %s - its first underrun %.2f s after the song was queued (%d buffers queued then, %s)\n", musicName.c_str(), musicFirstUnderrunUs / 1000000.0,
					   musicUnderrunQueued, musicPaused ? "paused" : "playing" );
	}
	if( musicSamplesMeasured > 0 && musicPeak < 64 )
	{
		idLib::Warning( "classic music: %s was silent (peak %d)", musicName.c_str(), musicPeak );
	}
	musicPeak = 0;
	musicSumSquares = 0.0;
	musicSamplesMeasured = 0;
	musicChunksTotal += musicChunks;
	musicUnderrunsTotal += musicUnderruns;
	musicSynthUsTotal += musicSynthUs;
	musicChunks = 0;
}

// stop feeding and empty the queue (musicLock held)
static void I_MusicStopLocked()
{
	alSourceStop( alMusicSourceVoice );
	alSourcei( alMusicSourceVoice, AL_BUFFER, 0 ); // unqueues every buffer
	alSourceRewind( alMusicSourceVoice ); // back to AL_INITIAL: a stopped source with the next song queued read as a queue that ran dry
	if( musicStreaming )
	{
		Timidity_Stop();
		Timidity_FreeSong( doomMusic );
		doomMusic = NULL;
		musicStreaming = false;
	}
	musicEnded = false;
}

// the worker: refills the buffers the source has played
static unsigned int I_MusicThread( void* )
{
	while( !musicThreadQuit )
	{
		Sys_Sleep( 30 );
		idScopedCriticalSection lock( musicLock );
		if( !musicStreaming || musicThreadQuit )
		{
			continue;
		}
		ALint processed = 0;
		alGetSourcei( alMusicSourceVoice, AL_BUFFERS_PROCESSED, &processed );
		while( processed-- > 0 )
		{
			ALuint buffer = 0;
			alSourceUnqueueBuffers( alMusicSourceVoice, 1, &buffer );
			if( buffer != 0 && I_MusicFillChunk( buffer ) )
			{
				alSourceQueueBuffers( alMusicSourceVoice, 1, &buffer );
			}
		}
		ALint state = 0, queued = 0;
		alGetSourcei( alMusicSourceVoice, AL_SOURCE_STATE, &state );
		alGetSourcei( alMusicSourceVoice, AL_BUFFERS_QUEUED, &queued );
		if( state == AL_STOPPED && !musicPaused && queued > 0 ) // (AL_INITIAL: queued, I_UpdateMusic has not started it yet)
		{
			// it played everything before the next chunk came: start it again
			musicUnderruns++;
			if( musicFirstUnderrunUs == 0 )
			{
				musicFirstUnderrunUs = Max( ( uint64 )1, Sys_Microseconds() - musicStartUs );
				musicUnderrunQueued = queued;
			}
			alSourcePlay( alMusicSourceVoice );
		}
	}
	return 0;
}

/*
======================
I_InitMusic
======================
*/
void I_InitMusic( void )
{
	if( !Music_initialized )
	{
		// Initialize Timidity
		// its output block = one streamed chunk (I_MusicFillChunk)
		Timidity_Init( MIDI_RATE, MIDI_FORMAT, MIDI_CHANNELS, MUSIC_BUFFER_SAMPLES, "classicmusic/gravis.cfg" );

		musicBuffer = NULL;
		totalBufferSize = 0;
		waitingForMusic = false;
		musicReady = false;

		alGenSources( ( ALuint )1, &alMusicSourceVoice );

		alSourcef( alMusicSourceVoice, AL_PITCH, 1.f );
		alSourcei( alMusicSourceVoice, AL_LOOPING, AL_FALSE ); // a queue: the song loops in I_MusicFillChunk

		alGenBuffers( ( ALuint )1, &alMusicBuffer );
		alGenBuffers( MUSIC_BUFFERS, musicStreamBuffers );

		musicThreadQuit = false;
		// core 3 on the Vita when it can be had (idlib/Thread.cpp: core 6)
		musicThread = Sys_CreateThread( I_MusicThread, NULL, THREAD_NORMAL, "ClassicMusic", ( core_t )6, 64 * 1024 );
		idLib::Printf( "classic music: streamed - %d buffers of %d ms (%d KB of sound in flight instead of the whole song)\n",
				  MUSIC_BUFFERS, MUSIC_BUFFER_MS, MUSIC_BUFFERS * MUSIC_BUFFER_BYTES / 1024 );

		Music_initialized = true;
	}
}

/*
======================
I_ShutdownMusic
======================
*/
void I_ShutdownMusic( void )
{
	if( Music_initialized )
	{
		musicThreadQuit = true;
		if( musicThread != 0 )
		{
			Sys_DestroyThread( musicThread ); // joined
			musicThread = 0;
		}
		{
			idScopedCriticalSection lock( musicLock );
			I_MusicReport( "shut down" );
			if( alIsSource( alMusicSourceVoice ) )
			{
				I_MusicStopLocked();
				alDeleteSources( 1, &alMusicSourceVoice );
			}
		}

		alDeleteBuffers( MUSIC_BUFFERS, musicStreamBuffers );
		if( alIsBuffer( alMusicBuffer ) )
		{
			alDeleteBuffers( 1, &alMusicBuffer );
		}

		if( musicBuffer )
		{
			free( musicBuffer );
			musicBuffer = NULL;
		}

		Timidity_Shutdown();
	}

	totalBufferSize = 0;
	waitingForMusic = false;
	musicReady = false;

	Music_initialized = false;
}

int Mus2Midi( unsigned char* bytes, unsigned char* out, int* len );

namespace
{
const int MaxMidiConversionSize = 1024 * 1024;
unsigned char midiConversionBuffer[MaxMidiConversionSize];
}

/*
======================
I_LoadSong

The song is converted and started; the first buffers are synthesised here
and queued (musicLock held).
======================
*/
void I_LoadSong( const char* songname )
{
	idStr lumpName = "d_";
	lumpName += static_cast< const char* >( songname );

	unsigned char* musFile = static_cast< unsigned char* >( W_CacheLumpName( lumpName.c_str(), PU_STATIC_SHARED ) );

	int length = 0;
	Mus2Midi( musFile, midiConversionBuffer, &length );

	const uint64 t0 = Sys_Microseconds();
	doomMusic = Timidity_LoadSongMem( midiConversionBuffer, length );
	musicName = lumpName;
	musicChunks = musicUnderruns = musicLoops = 0;
	musicStartUs = Sys_Microseconds();
	musicFirstUnderrunUs = 0;
	musicSynthUs = musicWorstChunkUs = 0;

	if( doomMusic )
	{
		Timidity_Start( doomMusic );
		musicStreaming = true;
		musicEnded = false;
		int queued = 0;
		for( int i = 0; i < MUSIC_BUFFERS; i++ )
		{
			if( I_MusicFillChunk( musicStreamBuffers[i] ) )
			{
				alSourceQueueBuffers( alMusicSourceVoice, 1, &musicStreamBuffers[i] );
				queued++;
			}
		}
		musicSongsTotal++;
		idLib::Printf( "classic music: %s started (%.1f s long, %d buffers queued, %.1f ms to load and synthesise the first %d ms)\n",
				  lumpName.c_str(), doomMusic->samples / ( double )MIDI_RATE, queued, ( Sys_Microseconds() - t0 ) / 1000.0, queued * MUSIC_BUFFER_MS );
	}
	else
	{
		idLib::Printf( "classic music: %s could not be loaded by Timidity\n", lumpName.c_str() );
	}

	musicReady = true;
}

/*
======================
I_PlaySong
======================
*/
void I_PlaySong( const char* songname, int looping )
{
	if( !Music_initialized )
	{
		return;
	}

	{
		idScopedCriticalSection lock( musicLock );
		I_MusicReport( "replaced" );
		I_MusicStopLocked();

		musicReady = false;
		musicPaused = false;
		// the stock source always looped (AL_LOOPING); the game asks for it
		// on everything but the finale's and intermission's one-shots
		musicLooping = looping != 0;
		I_LoadSong( songname );
	}
	waitingForMusic = true;

	if( DoomLib::GetPlayer() >= 0 )
	{
		::g->mus_looping = looping;
	}
}

/*
======================
I_UpdateMusic
======================
*/
void I_UpdateMusic( void )
{
	if( !Music_initialized )
	{
		return;
	}

	if( alMusicSourceVoice )
	{
		// Set the volume
		alSourcef( alMusicSourceVoice, AL_GAIN, x_MusicVolume * GLOBAL_VOLUME_MULTIPLIER );
	}

	if( waitingForMusic )
	{
		if( musicReady && alMusicSourceVoice )
		{
			idScopedCriticalSection lock( musicLock );
			if( musicStreaming && !musicPaused )
			{
				alSourcePlay( alMusicSourceVoice );
			}
			waitingForMusic = false;
		}
	}
}

/*
======================
I_PauseSong
======================
*/
void I_PauseSong( int handle )
{
	if( !Music_initialized || !alMusicSourceVoice )
	{
		return;
	}

	idScopedCriticalSection lock( musicLock );
	musicPaused = true;
	alSourcePause( alMusicSourceVoice );
}

/*
======================
I_ResumeSong
======================
*/
void I_ResumeSong( int handle )
{
	if( !Music_initialized || !alMusicSourceVoice )
	{
		return;
	}

	idScopedCriticalSection lock( musicLock );
	musicPaused = false;
	if( musicStreaming )
	{
		alSourcePlay( alMusicSourceVoice );
	}
}

/*
======================
I_StopSong
======================
*/
void I_StopSong( int handle )
{
	if( !Music_initialized || !alMusicSourceVoice )
	{
		return;
	}

	idScopedCriticalSection lock( musicLock );
	I_MusicReport( "stopped" );
	I_MusicStopLocked();
}

/*
======================
I_MusicStats

The verification (vita_verify_classic.cpp): songs started, underruns, the
synthesis cost - the current song included.
======================
*/
void I_MusicStats( int* songs, int* underruns, int* chunks, double* synthMsPerSecond )
{
	idScopedCriticalSection lock( musicLock );
	*songs = musicSongsTotal;
	*underruns = musicUnderrunsTotal + musicUnderruns;
	*chunks = musicChunksTotal + musicChunks;
	const double audioS = ( double )( *chunks ) * MUSIC_BUFFER_MS / 1000.0;
	*synthMsPerSecond = audioS > 0.0 ? ( ( musicSynthUsTotal + musicSynthUs ) / 1000.0 ) / audioS : 0.0;
}

/*
======================
I_UnRegisterSong
======================
*/
void I_UnRegisterSong( int handle )
{
	// does nothing
}

/*
======================
I_RegisterSong
======================
*/
int I_RegisterSong( void* data, int length )
{
	// does nothing
	return 0;
}
