/*
===========================================================================
Copyright (C) 2026, OpenJK-Experiment contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// tr_outcast_skeleton.cpp -- Jedi Outcast's animations on Jedi Academy's humanoid skeleton.
//
// When Jedi Outcast's campaign is played on this engine (fs_outcastpath set), Jedi Outcast's levels ask
// for animations Jedi Academy no longer has (sitting in a cockpit, the bartender, prisoners...). Both
// games animate humans with a skeleton of the same name, models/players/_humanoid/_humanoid.gla, and
// Jedi Academy's file hides Jedi Outcast's. So when Jedi Academy's skeleton is read from disk, every
// frame of Jedi Outcast's is converted and appended to it: Jedi Outcast's frame N becomes frame
// (Jedi Academy's frame count + N), which is where the game code's animation list puts them.
//
// The two skeletons differ: Jedi Outcast's has 72 bones, Jedi Academy's 53 (Jedi Outcast's extra
// finger and toe bones are gone, and Jedi Academy added two coat tails and a left hand tag). Bones of
// the same name have exactly the same rest pose, but they hang from different parents (Jedi Outcast
// hangs the legs straight from the pelvis, Jedi Academy from the thighs). A frame stores each bone
// relative to its parent, so frames cannot be copied as they are. Instead each Jedi Outcast frame is
// evaluated to where every bone ends up on the whole model; since the rest poses agree, that is also
// where Jedi Academy's bone of that name must end up, and it is stored again relative to Jedi
// Academy's parent. Jedi Academy's own bones simply follow their parent.
//
// (Jedi Outcast's models already work on Jedi Academy's skeleton: R_LoadMDXM remaps their bones.)

#ifndef JK2_MODE

#include "../server/exe_headers.h"

#include "tr_common.h"
#include "tr_local.h"
#include "qcommon/matcomp.h"

#include <math.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

const char HUMANOID_SKELETON_NAME[] = "models/players/_humanoid/_humanoid.gla";

// A bone pose: rotation and translation, as Ghoul2 multiplies them (3x4, the last column moves).
struct BonePose
{
	float m[3][4];
};

const BonePose IDENTITY_POSE = { { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };

// result = a * b, as Multiply_3x4Matrix
void MultiplyPoses( const BonePose &a, const BonePose &b, BonePose &result )
{
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 4; column++ )
		{
			result.m[row][column] = a.m[row][0] * b.m[0][column] + a.m[row][1] * b.m[1][column] + a.m[row][2] * b.m[2][column];
		}
		result.m[row][3] += a.m[row][3];
	}
}

// The inverse of a rigid pose: the rotation transposed, and the translation undone.
void InvertPose( const BonePose &pose, BonePose &inverse )
{
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			inverse.m[row][column] = pose.m[column][row];
		}
		inverse.m[row][3] = -( pose.m[0][row] * pose.m[0][3] + pose.m[1][row] * pose.m[1][3] + pose.m[2][row] * pose.m[2][3] );
	}
}

unsigned short QuantizeShort( float value )
{
	const float rounded = floorf( value + 0.5f );
	return (unsigned short)( ( rounded < 0.0f ) ? 0.0f : ( ( rounded > 65535.0f ) ? 65535.0f : rounded ) );
}

// The inverse of MC_UnCompressQuat: the rotation as a unit quaternion (w, x, y, z), each stored as
// (value + 2) * 16383, then the translation, each stored as (value + 512) * 64.
void CompressPose( const BonePose &pose, unsigned char compressed[sizeof( mdxaCompQuatBone_t )] )
{
	const float ( &m )[3][4] = pose.m;
	float w, x, y, z;
	const float trace = m[0][0] + m[1][1] + m[2][2];
	if ( trace > 0.0f )
	{
		const float s = sqrtf( trace + 1.0f ) * 2.0f;
		w = 0.25f * s;
		x = ( m[2][1] - m[1][2] ) / s;
		y = ( m[0][2] - m[2][0] ) / s;
		z = ( m[1][0] - m[0][1] ) / s;
	}
	else if ( m[0][0] > m[1][1] && m[0][0] > m[2][2] )
	{
		const float s = sqrtf( 1.0f + m[0][0] - m[1][1] - m[2][2] ) * 2.0f;
		w = ( m[2][1] - m[1][2] ) / s;
		x = 0.25f * s;
		y = ( m[0][1] + m[1][0] ) / s;
		z = ( m[0][2] + m[2][0] ) / s;
	}
	else if ( m[1][1] > m[2][2] )
	{
		const float s = sqrtf( 1.0f + m[1][1] - m[0][0] - m[2][2] ) * 2.0f;
		w = ( m[0][2] - m[2][0] ) / s;
		x = ( m[0][1] + m[1][0] ) / s;
		y = 0.25f * s;
		z = ( m[1][2] + m[2][1] ) / s;
	}
	else
	{
		const float s = sqrtf( 1.0f + m[2][2] - m[0][0] - m[1][1] ) * 2.0f;
		w = ( m[1][0] - m[0][1] ) / s;
		x = ( m[0][2] + m[2][0] ) / s;
		y = ( m[1][2] + m[2][1] ) / s;
		z = 0.25f * s;
	}
	const float length = sqrtf( w * w + x * x + y * y + z * z );
	if ( length > 0.0f )
	{
		w /= length; x /= length; y /= length; z /= length;
	}

	const unsigned short values[7] =
	{
		QuantizeShort( ( w + 2.0f ) * 16383.0f ),
		QuantizeShort( ( x + 2.0f ) * 16383.0f ),
		QuantizeShort( ( y + 2.0f ) * 16383.0f ),
		QuantizeShort( ( z + 2.0f ) * 16383.0f ),
		QuantizeShort( ( m[0][3] + 512.0f ) * 64.0f ),
		QuantizeShort( ( m[1][3] + 512.0f ) * 64.0f ),
		QuantizeShort( ( m[2][3] + 512.0f ) * 64.0f ),
	};
	memcpy( compressed, values, sizeof( values ) );
}

// A skeleton file (.gla) read from disk, checked and indexed.
class Skeleton
{
public:
	bool Read( const void *file, long fileLength, const char *description )
	{
		header = (const mdxaHeader_t *)file;
		if ( fileLength < (long)sizeof( mdxaHeader_t ) || header->ident != MDXA_IDENT || header->version != MDXA_VERSION
			|| header->numBones <= 0 || header->numFrames <= 0 || header->ofsEnd > fileLength
			|| header->ofsFrames + (long)header->numFrames * header->numBones * 3 > fileLength )
		{
			ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: %s is not a skeleton this engine reads\n", description );
			return false;
		}

		const mdxaSkelOffsets_t *offsets = (const mdxaSkelOffsets_t *)( (const byte *)header + sizeof( mdxaHeader_t ) );
		bones.resize( header->numBones );
		for ( int bone = 0; bone < header->numBones; bone++ )
		{
			bones[bone] = (const mdxaSkel_t *)( (const byte *)header + sizeof( mdxaHeader_t ) + offsets->offsets[bone] );
		}

		// Parents before children, whatever order the file lists them in.
		std::vector<bool> placed( header->numBones, false );
		while ( (int)evaluationOrder.size() < header->numBones )
		{
			const size_t placedBefore = evaluationOrder.size();
			for ( int bone = 0; bone < header->numBones; bone++ )
			{
				const int parent = bones[bone]->parent;
				if ( !placed[bone] && ( parent < 0 || ( parent < header->numBones && placed[parent] ) ) )
				{
					placed[bone] = true;
					evaluationOrder.push_back( bone );
				}
			}
			if ( evaluationOrder.size() == placedBefore )
			{
				ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: %s has a broken bone hierarchy\n", description );
				return false;
			}
		}

		// The compressed bone pool need not end the file, so count its entries from the frames.
		numPoolEntries = 0;
		for ( int frame = 0; frame < header->numFrames; frame++ )
		{
			for ( int bone = 0; bone < header->numBones; bone++ )
			{
				numPoolEntries = Q_max( numPoolEntries, PoolIndex( frame, bone ) + 1 );
			}
		}
		if ( header->ofsCompBonePool + (long)numPoolEntries * (long)sizeof( mdxaCompQuatBone_t ) > fileLength )
		{
			ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: %s is cut short\n", description );
			return false;
		}
		return true;
	}

	int FindBone( const char *name ) const
	{
		for ( int bone = 0; bone < header->numBones; bone++ )
		{
			if ( !Q_stricmp( bones[bone]->name, name ) )
			{
				return bone;
			}
		}
		return -1;
	}

	int PoolIndex( int frame, int bone ) const
	{
		const byte *index = (const byte *)header + header->ofsFrames + ( frame * header->numBones + bone ) * 3;
		return index[0] | ( index[1] << 8 ) | ( index[2] << 16 );
	}

	void FramePose( int frame, int bone, BonePose &pose ) const
	{
		const mdxaCompQuatBone_t *pool = (const mdxaCompQuatBone_t *)( (const byte *)header + header->ofsCompBonePool );
		MC_UnCompressQuat( pose.m, pool[PoolIndex( frame, bone )].Comp );
	}

	const mdxaHeader_t				*header = NULL;
	std::vector<const mdxaSkel_t *>	bones;
	std::vector<int>				evaluationOrder;
	int								numPoolEntries = 0;
};

} // namespace

/*
=================
R_Outcast_AddAnimationsToSkeleton

Called with every model file just read from disk. For Jedi Academy's humanoid skeleton while Jedi
Outcast's campaign is played, returns a new file holding Jedi Academy's frames followed by Jedi
Outcast's, allocated like a file read from disk (the old one is freed). Any other file comes back as
it was.
=================
*/
void *R_Outcast_AddAnimationsToSkeleton( const char *modelName, void *file, long fileLength )
{
#ifdef Q3_BIG_ENDIAN
	return file;	// the files would need byte swapping first
#else
	if ( !file || Q_stricmp( modelName, HUMANOID_SKELETON_NAME ) || !ri.Cvar_VariableString( "fs_outcastpath" )[0] )
	{
		return file;
	}

	void *outcastFile = NULL;
	const long outcastFileLength = ri.FS_ReadOutcastFile( HUMANOID_SKELETON_NAME, &outcastFile );
	if ( outcastFileLength <= 0 )
	{
		ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: its %s was not found\n", HUMANOID_SKELETON_NAME );
		return file;
	}

	Skeleton academy, outcast;
	if ( !academy.Read( file, fileLength, "Jedi Academy's skeleton" ) || !outcast.Read( outcastFile, outcastFileLength, "Jedi Outcast's skeleton" ) )
	{
		ri.FS_FreeFile( outcastFile );
		return file;
	}

	const int numBones = academy.header->numBones;
	const int numAcademyFrames = academy.header->numFrames;
	const int numOutcastFrames = outcast.header->numFrames;

	// Each Jedi Academy bone's Jedi Outcast twin (same name), or -1 for Jedi Academy's own bones.
	std::vector<int> outcastTwin( numBones );
	int numTwins = 0;
	for ( int bone = 0; bone < numBones; bone++ )
	{
		outcastTwin[bone] = outcast.FindBone( academy.bones[bone]->name );
		numTwins += ( outcastTwin[bone] >= 0 ) ? 1 : 0;
	}
	if ( outcastTwin[0] < 0 )
	{
		ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: the skeletons have different roots\n" );
		ri.FS_FreeFile( outcastFile );
		return file;
	}

	// Convert every Jedi Outcast frame, sharing identical compressed poses as the files do.
	std::vector<byte> outcastFrames( (size_t)numOutcastFrames * numBones * 3 );
	std::vector<byte> addedPool;
	std::unordered_map<std::string, int> addedPoolIndex;
	std::vector<BonePose> outcastModelPose( outcast.header->numBones );
	std::vector<BonePose> academyModelPose( numBones );

	for ( int frame = 0; frame < numOutcastFrames; frame++ )
	{
		// Where each Jedi Outcast bone ends up on the model in this frame
		for ( const int bone : outcast.evaluationOrder )
		{
			BonePose framePose;
			outcast.FramePose( frame, bone, framePose );
			const int parent = outcast.bones[bone]->parent;
			if ( parent < 0 )
			{
				outcastModelPose[bone] = framePose;
			}
			else
			{
				MultiplyPoses( outcastModelPose[parent], framePose, outcastModelPose[bone] );
			}
		}

		// ...is where Jedi Academy's bone of that name goes; its own bones follow their parent
		for ( const int bone : academy.evaluationOrder )
		{
			const int parent = academy.bones[bone]->parent;
			if ( outcastTwin[bone] >= 0 )
			{
				academyModelPose[bone] = outcastModelPose[outcastTwin[bone]];
			}
			else
			{
				academyModelPose[bone] = ( parent >= 0 ) ? academyModelPose[parent] : IDENTITY_POSE;
			}

			// stored relative to Jedi Academy's parent
			BonePose framePose;
			if ( parent < 0 )
			{
				framePose = academyModelPose[bone];
			}
			else
			{
				BonePose parentInverse;
				InvertPose( academyModelPose[parent], parentInverse );
				MultiplyPoses( parentInverse, academyModelPose[bone], framePose );
			}

			unsigned char compressed[sizeof( mdxaCompQuatBone_t )];
			CompressPose( framePose, compressed );
			const std::string key( (const char *)compressed, sizeof( compressed ) );
			auto found = addedPoolIndex.find( key );
			int poolIndex;
			if ( found != addedPoolIndex.end() )
			{
				poolIndex = found->second;
			}
			else
			{
				poolIndex = academy.numPoolEntries + (int)addedPoolIndex.size();
				addedPoolIndex.emplace( key, poolIndex );
				addedPool.insert( addedPool.end(), compressed, compressed + sizeof( compressed ) );
			}

			byte *index = &outcastFrames[( (size_t)frame * numBones + bone ) * 3];
			index[0] = (byte)( poolIndex & 0xff );
			index[1] = (byte)( ( poolIndex >> 8 ) & 0xff );
			index[2] = (byte)( ( poolIndex >> 16 ) & 0xff );
		}
	}

	const int numPoolEntries = academy.numPoolEntries + (int)addedPoolIndex.size();
	if ( numPoolEntries >= ( 1 << 24 ) )
	{
		ri.Printf( PRINT_WARNING, "Can't add Jedi Outcast's animations: too many bone poses\n" );
		ri.FS_FreeFile( outcastFile );
		return file;
	}

	// The new file: Jedi Academy's header and bones, its frames then Jedi Outcast's, then the pool.
	const mdxaHeader_t *academyHeader = academy.header;
	const int academyFramesSize = numAcademyFrames * numBones * 3;
	const int framesEnd = academyHeader->ofsFrames + academyFramesSize + (int)outcastFrames.size();
	const int ofsPool = ( framesEnd + 3 ) & ~3;
	const int academyPoolSize = academy.numPoolEntries * (int)sizeof( mdxaCompQuatBone_t );
	const int mergedLength = ofsPool + academyPoolSize + (int)addedPool.size();

	byte *merged = (byte *)R_Malloc( mergedLength + 1, TAG_FILESYS, qtrue );
	memcpy( merged, academyHeader, academyHeader->ofsFrames );
	memcpy( merged + academyHeader->ofsFrames, (const byte *)academyHeader + academyHeader->ofsFrames, academyFramesSize );
	memcpy( merged + academyHeader->ofsFrames + academyFramesSize, outcastFrames.data(), outcastFrames.size() );
	memcpy( merged + ofsPool, (const byte *)academyHeader + academyHeader->ofsCompBonePool, academyPoolSize );
	memcpy( merged + ofsPool + academyPoolSize, addedPool.data(), addedPool.size() );

	mdxaHeader_t *mergedHeader = (mdxaHeader_t *)merged;
	mergedHeader->numFrames = numAcademyFrames + numOutcastFrames;
	mergedHeader->ofsCompBonePool = ofsPool;
	mergedHeader->ofsEnd = mergedLength;

	ri.Printf( PRINT_ALL, "Added Jedi Outcast's %d animation frames to %s from frame %d (%d of %d bones shared, %d new bone poses)\n",
		numOutcastFrames, HUMANOID_SKELETON_NAME, numAcademyFrames, numTwins, numBones, (int)addedPoolIndex.size() );

	// Tells the game code where Jedi Outcast's frames start (G_Outcast_AddAnimations); it adds none
	// of Jedi Outcast's animations unless this is set.
	char firstOutcastFrame[16];
	Com_sprintf( firstOutcastFrame, sizeof( firstOutcastFrame ), "%d", numAcademyFrames );
	ri.Cvar_Get( "r_outcastAnimationsStart", "0", CVAR_ROM );
	ri.Cvar_Set( "r_outcastAnimationsStart", firstOutcastFrame );

	ri.FS_FreeFile( outcastFile );
	ri.FS_FreeFile( file );
	return merged;
#endif
}

#endif // !JK2_MODE
