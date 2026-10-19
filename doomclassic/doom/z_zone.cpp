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

#include "Precompiled.h"

#include "z_zone.h"
#include "i_system.h"
#include "doomdef.h"
#include "globaldata.h"
#if defined(__vita__) || defined(ID_TEST_CLASSIC_MT)
void R_VitaDeferFlushIfActive(); // r_draw.cpp
#endif


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
//

#define ZONEID	0x1d4a11


//
// Z_ClearZone
//
void Z_ClearZone( memzone_t* zone )
{
	memblock_t*		block;

	// set the entire zone to one free block
	zone->blocklist.next =
		zone->blocklist.prev =
			block = ( memblock_t* )( ( byte* )zone + sizeof( memzone_t ) );

	zone->blocklist.user = ( void** )zone;
	zone->blocklist.tag = PU_STATIC;
	zone->rover = block;

	block->prev = block->next = &zone->blocklist;

	// NULL indicates a free block.
	block->user = NULL;

	block->size = zone->size - sizeof( memzone_t );
}

// The zone is up to ZONE_MAX_PIECES pieces of memory, each its own memzone_t
// list (blocks never cross a piece): Z_Malloc tries the piece it allocated
// from last, then the others; Z_Free and the rest find a block's piece by
// its address. ::g->mainzone is the first piece.
//
// Why pieces (Vita, 2026-10-19 full run r55): classic DOOM starts after hours
// of DOOM 3 maps, when the newlib heap - the only CACHED memory there is - is
// fragmented: ~100 MB free inside it, no 8 MB of it in one piece. The zone
// fell back to vitaGL's UNCACHED pool and the classics crawled (15 tics a
// second, a demo never played in NRFTL). 2-6 MB holes are still there.
#define ZONE_MAX_PIECES 8
static memzone_t* zonePieces[ZONE_MAX_PIECES];
static int zoneNumPieces = 0;
static int zoneLastPiece = 0;

static memzone_t* Z_PieceOf( const void* p )
{
	for( int i = 0; i < zoneNumPieces; i++ )
	{
		const byte* base = ( const byte* )zonePieces[i];
		if( ( const byte* )p >= base && ( const byte* )p < base + zonePieces[i]->size )
		{
			return zonePieces[i];
		}
	}
	return NULL;
}

#if defined(__vita__)
bool Vita_IsCachedMemory( const void* p ); // vita_main.cpp
const char* Vita_MemKind( const void* p );
#endif
static int zoneTotalMB = 0;
static int zoneCachedMB = 0;
// for the verification: the zone's size and whether all of it is cached RAM
void Vita_ClassicZone( int* sizeMB, bool* cached )
{
	*sizeMB = zoneTotalMB;
	*cached = zoneTotalMB > 0 && zoneCachedMB == zoneTotalMB;
}
void Vita_ClassicZonePieces( int* pieces, int* cachedMB )
{
	*pieces = zoneNumPieces;
	*cachedMB = zoneCachedMB;
}

static void Z_AddPiece( void* base, int bytes, bool cached )
{
	memzone_t* zone = ( memzone_t* )base;
	memset( zone, 0, sizeof( memzone_t ) );
	zone->size = bytes;
	Z_ClearZone( zone );
	zonePieces[zoneNumPieces++] = zone;
	zoneTotalMB += bytes >> 20;
	if( cached )
	{
		zoneCachedMB += bytes >> 20;
	}
}

// fills zonePieces
static void Z_AllocPieces()
{
	zoneNumPieces = 0;
	zoneLastPiece = 0;
	zoneTotalMB = 0;
	zoneCachedMB = 0;
#if defined(__vita__)
	// One piece first, the biggest the cached heap has (room to keep every
	// lump of a level cached: a full zone purges PU_CACHE textures and patches
	// and W_ReadLump goes back to the memory card for them mid-frame; DOOM 3's
	// level is not loaded while the classics run).
	static const int wholeMB[] = { 32, 24, 16, 12 };
	for( int i = 0; i < ( int )( sizeof( wholeMB ) / sizeof( wholeMB[0] ) ); i++ )
	{
		void* base = malloc( wholeMB[i] * 1024 * 1024 );
		if( base == NULL )
		{
			continue;
		}
		if( !Vita_IsCachedMemory( base ) )
		{
			free( base ); // a smaller piece may still be found in the cached heap
			continue;
		}
		Z_AddPiece( base, wholeMB[i] * 1024 * 1024, true );
		idLib::Printf( "Z_Init: the zone is one %d MB piece of cached RAM\n", wholeMB[i] );
		return;
	}
	// Then cached pieces of 8 MB down to 2 MB until 16 MB (8 at the very
	// least). Every block DOOM allocates must fit one piece: its biggest are
	// level lumps of a few hundred KB.
	static const int pieceMB[] = { 8, 6, 4, 3, 2 };
	int totalMB = 0;
	for( int i = 0; i < ( int )( sizeof( pieceMB ) / sizeof( pieceMB[0] ) ) && totalMB < 16 && zoneNumPieces < ZONE_MAX_PIECES; )
	{
		void* base = malloc( pieceMB[i] * 1024 * 1024 );
		if( base == NULL || !Vita_IsCachedMemory( base ) )
		{
			if( base != NULL )
			{
				free( base );
			}
			i++; // none this size left: smaller
			continue;
		}
		Z_AddPiece( base, pieceMB[i] * 1024 * 1024, true );
		totalMB += pieceMB[i];
	}
	if( totalMB < 8 && zoneNumPieces < ZONE_MAX_PIECES )
	{
		// what is missing comes from wherever malloc finds it (vitaGL's
		// uncached pool): slow, but the classics still run
		const int missingMB = Max( 2, 8 - totalMB );
		void* base = malloc( missingMB * 1024 * 1024 );
		if( base == NULL )
		{
			I_Error( "Z_Init: no memory for the zone (%d MB cached, %d MB more not found)", totalMB, missingMB );
		}
		const bool cached = Vita_IsCachedMemory( base );
		Z_AddPiece( base, missingMB * 1024 * 1024, cached );
		idLib::Printf( "Z_Init: WARNING - only %d MB of cached RAM found in pieces; %d MB of %s added - what lands there draws slowly\n",
					   totalMB, missingMB, Vita_MemKind( base ) );
	}
	idStr list;
	for( int i = 0; i < zoneNumPieces; i++ )
	{
		list += va( "%s%d MB (%s)", i ? ", " : "", zonePieces[i]->size >> 20, Vita_MemKind( zonePieces[i] ) );
	}
	idLib::Printf( "Z_Init: no 12 MB piece of the cached heap was free; the zone is %d MB in %d pieces, %d MB of them cached: %s\n",
				   zoneTotalMB, zoneNumPieces, zoneCachedMB, list.c_str() );
#else
	const int bytes = 15 * 1024 * 1024; // SMF - was 10 * 1024 * 1024
	void* base = malloc( bytes );
	if( base == NULL )
	{
		I_Error( "Z_Init: no memory for the zone" );
	}
	Z_AddPiece( base, bytes, true );
#endif
}

//
// Z_Init
//
void Z_Init( void )
{
	Z_AllocPieces();
	::g->mainzone = zonePieces[0];
}

// the pieces back to the system (DoomLib::Shutdown)
void Z_FreeZone( void )
{
	for( int i = 0; i < zoneNumPieces; i++ )
	{
		free( zonePieces[i] );
		zonePieces[i] = NULL;
	}
	zoneNumPieces = 0;
	zoneTotalMB = 0;
	zoneCachedMB = 0;
	::g->mainzone = NULL;
}

int NumAlloc = 0;

//
// Z_Free
//
void Z_Free( void* ptr )
{
	memblock_t*		block;
	memblock_t*		other;

	block = ( memblock_t* )( ( byte* )ptr - sizeof( memblock_t ) );

	NumAlloc -= block->size;

	if( block->id != ZONEID )
	{
		I_Error( "Z_Free: freed a pointer without ZONEID" );
	}
	memzone_t* zone = Z_PieceOf( block );
	if( zone == NULL )
	{
		I_Error( "Z_Free: %p is in no piece of the zone", ptr );
	}

	if( block->user > ( void** )0x100 )
	{
		// smaller values are not pointers
		// Note: OS-dependend?

		// clear the user's mark
		*block->user = 0;
	}

	// mark as free
	block->user = NULL;
	block->tag = 0;
	block->id = 0;

	other = block->prev;

	if( !other->user )
	{
		// merge with previous free block
		other->size += block->size;
		other->next = block->next;
		other->next->prev = other;

		if( block == zone->rover )
		{
			zone->rover = other;
		}

		block = other;
	}

	other = block->next;
	if( !other->user )
	{
		// merge the next free block onto the end
		block->size += other->size;
		block->next = other->next;
		block->next->prev = block;

		if( other == zone->rover )
		{
			zone->rover = block;
		}
	}
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64

// one piece; NULL when it has no room even after purging what it can
static void* Z_MallocIn( memzone_t* zone, int size, int tag, void* user )
{
	int		extra;
	memblock_t*	start;
	memblock_t* rover;
	memblock_t* newblock;
	memblock_t*	base;

	// scan through the block list,
	// looking for the first free block
	// of sufficient size,
	// throwing out any purgable blocks along the way.

	// if there is a free block behind the rover,
	//  back up over them
	base = zone->rover;

	if( !base->prev->user )
	{
		base = base->prev;
	}

	rover = base;
	start = base->prev;

	do
	{
		if( rover == start )
		{
			// scanned all the way around the list
			return NULL;
		}

		if( rover->user )
		{
			if( rover->tag < PU_PURGELEVEL )
			{
				// hit a block that can't be purged,
				//  so move base past it
				base = rover = rover->next;
			}
			else
			{
				// free the rover block (adding the size to base)
#if defined(__vita__) || defined(ID_TEST_CLASSIC_MT)
				// deferred draws may still point into it (r_draw.cpp)
				R_VitaDeferFlushIfActive();
#endif

				// the rover can be the base block
				base = base->prev;
				Z_Free( ( byte* )rover + sizeof( memblock_t ) );
				base = base->next;
				rover = base->next;
			}
		}
		else
		{
			rover = rover->next;
		}
	}
	while( base->user || base->size < size );


	// found a block big enough
	extra = base->size - size;

	if( extra >  MINFRAGMENT )
	{
		// there will be a free fragment after the allocated block
		newblock = ( memblock_t* )( ( byte* )base + size );
		newblock->size = extra;

		// NULL indicates free block.
		newblock->user = NULL;
		newblock->tag = 0;
		newblock->prev = base;
		newblock->next = base->next;
		newblock->next->prev = newblock;

		base->next = newblock;
		base->size = size;
	}

	if( user )
	{
		// mark as an in use block
		base->user = ( void** )user;
		*( void** )user = ( void* )( ( byte* )base + sizeof( memblock_t ) );
	}
	else
	{
		if( tag >= PU_PURGELEVEL )
		{
			I_Error( "Z_Malloc: an owner is required for purgable blocks" );
		}

		// mark as in use, but unowned
		base->user = ( void** )2;
	}
	base->tag = tag;

	// next allocation will start looking here
	zone->rover = base->next;

	base->id = ZONEID;

	return ( void* )( ( byte* )base + sizeof( memblock_t ) );
}

void*
Z_Malloc
( int		size,
  int		tag,
  void*		user )
{
	NumAlloc += size;

	size = ( size + 3 ) & ~3;

	// account for size of block header
	size += sizeof( memblock_t );

	// the piece used last, then the others in turn
	for( int n = 0; n < zoneNumPieces; n++ )
	{
		const int i = ( zoneLastPiece + n ) % zoneNumPieces;
		void* p = Z_MallocIn( zonePieces[i], size, tag, user );
		if( p != NULL )
		{
			zoneLastPiece = i;
			return p;
		}
	}
	I_Error( "Z_Malloc: failed on allocation of %i bytes (zone: %d MB in %d pieces)", size, zoneTotalMB, zoneNumPieces );
	return NULL;
}



//
// Z_FreeTags
//
void
Z_FreeTags
( int		lowtag,
  int		hightag )
{
	memblock_t*	block;
	memblock_t*	next;

	for( int i = 0; i < zoneNumPieces; i++ )
	{
		memzone_t* zone = zonePieces[i];
		for( block = zone->blocklist.next ;
				block != &zone->blocklist ;
				block = next )
		{
			// get link before freeing
			next = block->next;

			// free block?
			if( !block->user )
			{
				continue;
			}

			if( block->tag >= lowtag && block->tag <= hightag )
			{
				Z_Free( ( byte* )block + sizeof( memblock_t ) );
			}
		}
	}
}



//
// Z_DumpHeap
// Note: TFileDumpHeap( stdout ) ?
//
void
Z_DumpHeap
( int		lowtag,
  int		hightag )
{
	memblock_t*	block;

	for( int i = 0; i < zoneNumPieces; i++ )
	{
		memzone_t* zone = zonePieces[i];
		I_Printf( "zone piece %d size: %i  location: %p\n", i, zone->size, zone );

		I_Printf( "tag range: %i to %i\n",
				  lowtag, hightag );

		for( block = zone->blocklist.next ; ; block = block->next )
		{
			if( block->tag >= lowtag && block->tag <= hightag )
				I_Printf( "block:%p    size:%7i    user:%p    tag:%3i\n",
						  block, block->size, block->user, block->tag );

			if( block->next == &zone->blocklist )
			{
				// all blocks have been hit
				break;
			}

			if( ( byte* )block + block->size != ( byte* )block->next )
			{
				I_Printf( "ERROR: block size does not touch the next block\n" );
			}

			if( block->next->prev != block )
			{
				I_Printf( "ERROR: next block doesn't have proper back link\n" );
			}

			if( !block->user && !block->next->user )
			{
				I_Printf( "ERROR: two consecutive free blocks\n" );
			}
		}
	}
}


//
// Z_FileDumpHeap
//
void Z_FileDumpHeap( FILE* f )
{
	memblock_t*	block;

	for( int i = 0; i < zoneNumPieces; i++ )
	{
		memzone_t* zone = zonePieces[i];
		fprintf( f, "zone piece %d size: %i  location: %p\n", i, zone->size, zone );

		for( block = zone->blocklist.next ; ; block = block->next )
		{
			fprintf( f, "block:%p    size:%7i    user:%p    tag:%3i\n",
					 block, block->size, block->user, block->tag );

			if( block->next == &zone->blocklist )
			{
				// all blocks have been hit
				break;
			}

			if( ( byte* )block + block->size != ( byte* )block->next )
			{
				fprintf( f, "ERROR: block size does not touch the next block\n" );
			}

			if( block->next->prev != block )
			{
				fprintf( f, "ERROR: next block doesn't have proper back link\n" );
			}

			if( !block->user && !block->next->user )
			{
				fprintf( f, "ERROR: two consecutive free blocks\n" );
			}
		}
	}
}



//
// Z_CheckHeap
//
void Z_CheckHeap( void )
{
	memblock_t*	block;

	for( int i = 0; i < zoneNumPieces; i++ )
	{
		memzone_t* zone = zonePieces[i];
		for( block = zone->blocklist.next ; ; block = block->next )
		{
			if( block->next == &zone->blocklist )
			{
				// all blocks have been hit
				break;
			}

			if( ( byte* )block + block->size != ( byte* )block->next )
			{
				I_Error( "Z_CheckHeap: block size does not touch the next block\n" );
			}

			if( block->next->prev != block )
			{
				I_Error( "Z_CheckHeap: next block doesn't have proper back link\n" );
			}

			if( !block->user && !block->next->user )
			{
				I_Error( "Z_CheckHeap: two consecutive free blocks\n" );
			}
		}
	}
}




//
// Z_ChangeTag
//
void
Z_ChangeTag2
( void*		ptr,
  int		tag )
{
	memblock_t*	block;

	block = ( memblock_t* )( ( byte* )ptr - sizeof( memblock_t ) );

	if( block->id != ZONEID )
	{
		I_Error( "Z_ChangeTag: freed a pointer without ZONEID" );
	}

	if( tag >= PU_PURGELEVEL && ( uintptr_t )block->user < 0x100 )
	{
		I_Error( "Z_ChangeTag: an owner is required for purgable blocks" );
	}

	block->tag = tag;
}

void Z_ChangeTag2( void** pp, int tag )
{
	Z_ChangeTag2( *pp, tag );
}


//
// Z_FreeMemory
//
int Z_FreeMemory( void )
{
	memblock_t*		block;
	int			free;

	free = 0;

	for( int i = 0; i < zoneNumPieces; i++ )
	{
		memzone_t* zone = zonePieces[i];
		for( block = zone->blocklist.next ;
				block != &zone->blocklist;
				block = block->next )
		{
			if( !block->user || block->tag >= PU_PURGELEVEL )
			{
				free += block->size;
			}
		}
	}
	return free;
}

/*
bool MallocForLump( int lump, unsigned int size, void **data, int tag )
{
	*data = Z_Malloc( size, tag, 0 );

	return true;
}
*/
