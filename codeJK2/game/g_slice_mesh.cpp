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

// g_slice_mesh.cpp -- the geometry behind lightsaber slicing. See g_slice_mesh.h.

#include "g_headers.h"
#include "g_local.h"
#include "g_slice_mesh.h"
#include "g_ragdoll_rig.h"
#include "../../code/rd-common/mdx_format.h"

#include <algorithm>
#include <ctype.h>
#include <map>
#include <math.h>

namespace slicing
{

// Cut points closer together than this are treated as the same point when the cut faces are
// traced around (units).
static const float SAME_POINT_DISTANCE = 0.05f;

// The cap texture repeats every this many units across a cut face.
static const float CAP_TEXTURE_SIZE = 8.0f;

/*
==================
Reading a character's mesh
==================
*/

static std::map<std::string, CharacterMesh *> characterMeshCache;

static std::string LowerCase( const std::string &text )
{
	std::string lower = text;
	for ( size_t index = 0; index < lower.size(); index++ )
	{
		lower[index] = (char)tolower( (unsigned char)lower[index] );
	}
	return lower;
}

static std::string Trimmed( const std::string &text )
{
	const size_t first = text.find_first_not_of( " \t" );
	if ( first == std::string::npos )
	{
		return std::string();
	}
	const size_t last = text.find_last_not_of( " \t" );
	return text.substr( first, last - first + 1 );
}

// Reads a skin file: lines of "surface name,shader name". Surface names are stored in lower case.
static void ReadSkin( const char *skinPath, std::map<std::string, std::string> &shaderForSurface )
{
	char *buffer = NULL;
	const long length = gi.FS_ReadFile( skinPath, (void **)&buffer );
	if ( length <= 0 || !buffer )
	{
		return;
	}
	const std::string text( buffer, (size_t)length );
	gi.FS_FreeFile( buffer );

	size_t lineStart = 0;
	while ( lineStart < text.size() )
	{
		size_t lineEnd = text.find_first_of( "\r\n", lineStart );
		if ( lineEnd == std::string::npos )
		{
			lineEnd = text.size();
		}
		const std::string line = text.substr( lineStart, lineEnd - lineStart );
		lineStart = lineEnd + 1;

		const size_t comma = line.find( ',' );
		if ( comma == std::string::npos )
		{
			continue;
		}
		const std::string surfaceName = LowerCase( Trimmed( line.substr( 0, comma ) ) );
		const std::string shaderName = Trimmed( line.substr( comma + 1 ) );
		if ( !surfaceName.empty() && !shaderName.empty() )
		{
			shaderForSurface[surfaceName] = shaderName;
		}
	}
}

// True if size bytes starting at pointer lie inside the file.
static bool IsInsideFile( const byte *fileStart, long fileLength, const void *pointer, long size )
{
	const byte *start = (const byte *)pointer;
	return start >= fileStart && size >= 0 && start + size <= fileStart + fileLength;
}

const CharacterMesh *LoadCharacterMesh( const char *meshPath, const char *skinPath )
{
	const std::string cacheKey = LowerCase( std::string( meshPath ) + "|" + skinPath );
	const std::map<std::string, CharacterMesh *>::const_iterator cached = characterMeshCache.find( cacheKey );
	if ( cached != characterMeshCache.end() )
	{
		return cached->second;
	}

	char *buffer = NULL;
	const long length = gi.FS_ReadFile( meshPath, (void **)&buffer );
	if ( length < (long)sizeof( mdxmHeader_t ) || !buffer )
	{
		if ( buffer )
		{
			gi.FS_FreeFile( buffer );
		}
		return NULL;
	}
	const byte *file = (const byte *)buffer;
	const mdxmHeader_t *header = (const mdxmHeader_t *)file;
	if ( header->ident != MDXM_IDENT || header->version != MDXM_VERSION || header->numLODs < 1 || header->numSurfaces < 1 )
	{
		gi.FS_FreeFile( buffer );
		return NULL;
	}

	std::map<std::string, std::string> shaderForSurface;
	ReadSkin( skinPath, shaderForSurface );

	CharacterMesh *mesh = new CharacterMesh;

	// The skin's shader for its cut-off parts (surfaces named like "torso_cap_head") closes cut faces.
	for ( std::map<std::string, std::string>::const_iterator entry = shaderForSurface.begin(); entry != shaderForSurface.end(); ++entry )
	{
		if ( entry->first.find( "cap" ) != std::string::npos && entry->second.find( "*off" ) == std::string::npos )
		{
			mesh->capShaderName = entry->second;
			break;
		}
	}

	// The surface list (names, flags, default shaders) follows the header; the most detailed level of
	// detail comes first, with a table of where each of its surfaces starts.
	const mdxmHierarchyOffsets_t *hierarchyOffsets = (const mdxmHierarchyOffsets_t *)( file + sizeof( mdxmHeader_t ) );
	const mdxmLOD_t *mostDetailed = (const mdxmLOD_t *)( file + header->ofsLODs );
	const mdxmLODSurfOffset_t *surfaceOffsets = (const mdxmLODSurfOffset_t *)( (const byte *)mostDetailed + sizeof( mdxmLOD_t ) );
	if ( !IsInsideFile( file, length, hierarchyOffsets, (long)sizeof( int ) * header->numSurfaces )
		|| !IsInsideFile( file, length, surfaceOffsets, (long)sizeof( int ) * header->numSurfaces ) )
	{
		gi.FS_FreeFile( buffer );
		characterMeshCache[cacheKey] = mesh;
		return mesh;
	}

	for ( int surfaceIndex = 0; surfaceIndex < header->numSurfaces; surfaceIndex++ )
	{
		const mdxmSurfHierarchy_t *info = (const mdxmSurfHierarchy_t *)( (const byte *)hierarchyOffsets + hierarchyOffsets->offsets[surfaceIndex] );
		const mdxmSurface_t *surface = (const mdxmSurface_t *)( (const byte *)surfaceOffsets + surfaceOffsets->offsets[surfaceIndex] );
		if ( !IsInsideFile( file, length, info, (long)sizeof( mdxmSurfHierarchy_t ) )
			|| !IsInsideFile( file, length, surface, (long)sizeof( mdxmSurface_t ) ) )
		{
			continue;
		}

		// Bolt points ("tags") and parts hidden by default (such as the dismemberment caps) are not drawn.
		if ( ( info->flags & ( G2SURFACEFLAG_ISBOLT | G2SURFACEFLAG_OFF ) ) || info->name[0] == '*' )
		{
			continue;
		}
		std::string shaderName = info->shader;
		const std::map<std::string, std::string>::const_iterator skinEntry = shaderForSurface.find( LowerCase( info->name ) );
		if ( skinEntry != shaderForSurface.end() )
		{
			shaderName = skinEntry->second;
		}
		if ( shaderName.empty() || shaderName.find( "*off" ) != std::string::npos )
		{
			continue;
		}

		const mdxmVertex_t *vertices = (const mdxmVertex_t *)( (const byte *)surface + surface->ofsVerts );
		const mdxmVertexTexCoord_t *texCoords = (const mdxmVertexTexCoord_t *)&vertices[surface->numVerts];
		const mdxmTriangle_t *triangles = (const mdxmTriangle_t *)( (const byte *)surface + surface->ofsTriangles );
		const int *boneReferences = (const int *)( (const byte *)surface + surface->ofsBoneReferences );
		if ( surface->numVerts <= 0 || surface->numTriangles <= 0
			|| !IsInsideFile( file, length, vertices, (long)( sizeof( mdxmVertex_t ) + sizeof( mdxmVertexTexCoord_t ) ) * surface->numVerts )
			|| !IsInsideFile( file, length, triangles, (long)sizeof( mdxmTriangle_t ) * surface->numTriangles )
			|| !IsInsideFile( file, length, boneReferences, (long)sizeof( int ) * surface->numBoneReferences ) )
		{
			continue;
		}

		MeshSurface part;
		part.name = info->name;
		part.shaderName = shaderName;
		part.vertices.resize( surface->numVerts );
		for ( int vertexIndex = 0; vertexIndex < surface->numVerts; vertexIndex++ )
		{
			const mdxmVertex_t &source = vertices[vertexIndex];
			BindVertex &vertex = part.vertices[vertexIndex];
			vertex.position = Vector3::FromArray( source.vertCoords );
			vertex.normal = Vector3::FromArray( source.normal );
			vertex.s = texCoords[vertexIndex].texCoords[0];
			vertex.t = texCoords[vertexIndex].texCoords[1];

			// The weights must be read in order: the last one is whatever the others leave over.
			const int weightCount = G2_GetVertWeights( &source );
			float weightSoFar = 0.0f;
			vertex.boneCount = 0;
			for ( int weightIndex = 0; weightIndex < weightCount && weightIndex < 4; weightIndex++ )
			{
				const int reference = G2_GetVertBoneIndex( &source, weightIndex );
				const float weight = G2_GetVertBoneWeight( &source, weightIndex, weightSoFar, weightCount );
				if ( reference < 0 || reference >= surface->numBoneReferences )
				{
					continue;
				}
				vertex.bones[vertex.boneCount] = boneReferences[reference];
				vertex.weights[vertex.boneCount] = weight;
				vertex.boneCount++;
			}
		}

		part.indexes.reserve( surface->numTriangles * 3 );
		for ( int triangleIndex = 0; triangleIndex < surface->numTriangles; triangleIndex++ )
		{
			const int *corners = triangles[triangleIndex].indexes;
			if ( corners[0] < 0 || corners[0] >= surface->numVerts || corners[1] < 0 || corners[1] >= surface->numVerts
				|| corners[2] < 0 || corners[2] >= surface->numVerts )
			{
				continue;
			}
			part.indexes.push_back( corners[0] );
			part.indexes.push_back( corners[1] );
			part.indexes.push_back( corners[2] );
		}
		mesh->surfaces.push_back( part );
	}
	gi.FS_FreeFile( buffer );

	characterMeshCache[cacheKey] = mesh;
	return mesh;
}

void ClearCharacterMeshCache( void )
{
	for ( std::map<std::string, CharacterMesh *>::iterator entry = characterMeshCache.begin(); entry != characterMeshCache.end(); ++entry )
	{
		delete entry->second;
	}
	characterMeshCache.clear();
}

/*
==================
Posing
==================
*/

void PosedMesh::Clear( void )
{
	vertices.clear();
	indexes.clear();
	triangleShaders.clear();
	shaderNames.clear();
}

int PosedMesh::ShaderSlot( const std::string &shaderName )
{
	for ( size_t slot = 0; slot < shaderNames.size(); slot++ )
	{
		if ( shaderNames[slot] == shaderName )
		{
			return (int)slot;
		}
	}
	shaderNames.push_back( shaderName );
	return (int)shaderNames.size() - 1;
}

void MakeRestMesh( const PosedMesh &restPosed, CharacterMesh &mesh )
{
	mesh.surfaces.clear();
	std::vector<int> copiedIndex( restPosed.vertices.size() );
	for ( size_t shaderSlot = 0; shaderSlot < restPosed.shaderNames.size(); shaderSlot++ )
	{
		MeshSurface surface;
		surface.name = "cut " + restPosed.shaderNames[shaderSlot];
		surface.shaderName = restPosed.shaderNames[shaderSlot];
		std::fill( copiedIndex.begin(), copiedIndex.end(), -1 );
		for ( size_t triangle = 0; triangle < restPosed.triangleShaders.size(); triangle++ )
		{
			if ( restPosed.triangleShaders[triangle] != (int)shaderSlot )
			{
				continue;
			}
			for ( int corner = 0; corner < 3; corner++ )
			{
				const int source = restPosed.indexes[triangle * 3 + corner];
				if ( copiedIndex[source] < 0 )
				{
					const PosedVertex &posedVertex = restPosed.vertices[source];
					BindVertex vertex;
					vertex.position = posedVertex.position;
					vertex.normal = posedVertex.normal;
					vertex.s = posedVertex.s;
					vertex.t = posedVertex.t;
					vertex.boneCount = posedVertex.boneCount;
					for ( int weightIndex = 0; weightIndex < 4; weightIndex++ )
					{
						vertex.bones[weightIndex] = posedVertex.bones[weightIndex];
						vertex.weights[weightIndex] = posedVertex.weights[weightIndex];
					}
					copiedIndex[source] = (int)surface.vertices.size();
					surface.vertices.push_back( vertex );
				}
				surface.indexes.push_back( copiedIndex[source] );
			}
		}
		if ( !surface.indexes.empty() )
		{
			mesh.surfaces.push_back( surface );
		}
	}
}

void PoseCharacterMesh( const CharacterMesh &mesh, const ragdoll::Skeleton &skeleton, const Transform *boneFrames,
						const Transform &modelToWorld, float modelScale, PosedMesh &posed, const std::vector<bool> *surfaceIsShown )
{
	posed.Clear();

	// How each bone has moved from the rest pose: a vertex at rest is carried by it to where it is drawn.
	std::vector<Transform> boneMotion( skeleton.boneCount );
	for ( int bone = 0; bone < skeleton.boneCount; bone++ )
	{
		boneMotion[bone] = ragdoll::Compose( boneFrames[bone], ragdoll::Inverse( skeleton.bones[bone].basePose ) );
	}

	for ( size_t surfaceIndex = 0; surfaceIndex < mesh.surfaces.size(); surfaceIndex++ )
	{
		if ( surfaceIsShown && surfaceIndex < surfaceIsShown->size() && !( *surfaceIsShown )[surfaceIndex] )
		{
			continue;
		}
		const MeshSurface &surface = mesh.surfaces[surfaceIndex];
		const int shaderSlot = posed.ShaderSlot( surface.shaderName );
		const int firstVertex = (int)posed.vertices.size();

		for ( size_t vertexIndex = 0; vertexIndex < surface.vertices.size(); vertexIndex++ )
		{
			const BindVertex &vertex = surface.vertices[vertexIndex];
			Vector3 position( 0.0f, 0.0f, 0.0f );
			Vector3 normal( 0.0f, 0.0f, 0.0f );
			float usedWeight = 0.0f;
			for ( int weightIndex = 0; weightIndex < vertex.boneCount; weightIndex++ )
			{
				const int bone = vertex.bones[weightIndex];
				if ( bone < 0 || bone >= skeleton.boneCount )
				{
					continue;
				}
				const float weight = vertex.weights[weightIndex];
				position += boneMotion[bone].TransformPoint( vertex.position ) * weight;
				normal += ragdoll::Rotate( boneMotion[bone].rotation, vertex.normal ) * weight;
				usedWeight += weight;
			}
			if ( usedWeight <= 0.0f )
			{
				position = vertex.position;		// not moved by any bone: stays in the rest pose
				normal = vertex.normal;
			}

			PosedVertex placed;
			placed.position = modelToWorld.TransformPoint( position * modelScale );
			placed.normal = ragdoll::Normalized( ragdoll::Rotate( modelToWorld.rotation, normal ) );
			placed.s = vertex.s;
			placed.t = vertex.t;
			placed.boneCount = vertex.boneCount;
			for ( int weightIndex = 0; weightIndex < vertex.boneCount; weightIndex++ )
			{
				placed.bones[weightIndex] = vertex.bones[weightIndex];
				placed.weights[weightIndex] = vertex.weights[weightIndex];
			}
			posed.vertices.push_back( placed );
		}

		for ( size_t index = 0; index + 2 < surface.indexes.size(); index += 3 )
		{
			posed.indexes.push_back( firstVertex + surface.indexes[index] );
			posed.indexes.push_back( firstVertex + surface.indexes[index + 1] );
			posed.indexes.push_back( firstVertex + surface.indexes[index + 2] );
			posed.triangleShaders.push_back( shaderSlot );
		}
	}
}

/*
==================
Cutting
==================
*/

namespace
{

// Builds one part of a divided mesh. The original vertices it uses are copied over once each; the
// points where the cut crosses an edge are added as new vertices.
class PartBuilder
{
public:
	PartBuilder( const PosedMesh *source, PosedMesh *output )
		: source( source ), output( output ), copiedIndex( source->vertices.size(), -1 )
	{
	}

	int Original( int sourceIndex )
	{
		if ( copiedIndex[sourceIndex] < 0 )
		{
			copiedIndex[sourceIndex] = (int)output->vertices.size();
			output->vertices.push_back( source->vertices[sourceIndex] );
		}
		return copiedIndex[sourceIndex];
	}

	int New( const PosedVertex &vertex )
	{
		output->vertices.push_back( vertex );
		return (int)output->vertices.size() - 1;
	}

	// Adds a convex polygon, given in the original triangle's winding, as a fan of triangles.
	void Polygon( const int *corners, int cornerCount, int sourceShaderSlot )
	{
		const int shaderSlot = output->ShaderSlot( source->shaderNames[sourceShaderSlot] );
		for ( int corner = 1; corner + 1 < cornerCount; corner++ )
		{
			output->indexes.push_back( corners[0] );
			output->indexes.push_back( corners[corner] );
			output->indexes.push_back( corners[corner + 1] );
			output->triangleShaders.push_back( shaderSlot );
		}
	}

	// Adds one of the source mesh's triangles unchanged.
	void WholeTriangle( const int *sourceCorners, int sourceShaderSlot )
	{
		const int copied[3] = { Original( sourceCorners[0] ), Original( sourceCorners[1] ), Original( sourceCorners[2] ) };
		Polygon( copied, 3, sourceShaderSlot );
	}

private:
	const PosedMesh *source;
	PosedMesh *output;
	std::vector<int> copiedIndex;
};

// A piece of the cut's outline: where it crosses one triangle.
struct CutSegment
{
	PosedVertex start;
	PosedVertex end;
};

// A point part of the way along an edge. It is moved by the bones of whichever end it is nearer.
PosedVertex Interpolated( const PosedVertex &from, const PosedVertex &to, float fraction )
{
	PosedVertex vertex = ( fraction < 0.5f ) ? from : to;
	vertex.position = from.position + ( to.position - from.position ) * fraction;
	vertex.normal = ragdoll::Normalized( from.normal + ( to.normal - from.normal ) * fraction );
	vertex.s = from.s + ( to.s - from.s ) * fraction;
	vertex.t = from.t + ( to.t - from.t ) * fraction;
	return vertex;
}

// A key for a point, the same for points closer together than SAME_POINT_DISTANCE (nearly always).
long long PointKey( const Vector3 &point )
{
	const float cellsPerUnit = 1.0f / SAME_POINT_DISTANCE;
	const long long cellOffset = 1LL << 20;
	const long long cellMask = ( 1LL << 21 ) - 1;
	const long long x = ( (long long)floorf( point.x * cellsPerUnit ) + cellOffset ) & cellMask;
	const long long y = ( (long long)floorf( point.y * cellsPerUnit ) + cellOffset ) & cellMask;
	const long long z = ( (long long)floorf( point.z * cellsPerUnit ) + cellOffset ) & cellMask;
	return ( x << 42 ) | ( y << 21 ) | z;
}

// Joins the cut's segments into outlines. Segments that meet end to end form an outline; an outline
// that does not close up (the mesh has a gap there) is closed by its first and last points.
void TraceOutlines( const std::vector<CutSegment> &segments, std::vector< std::vector<PosedVertex> > &outlines )
{
	std::multimap<long long, int> segmentsAtPoint;
	for ( size_t segmentIndex = 0; segmentIndex < segments.size(); segmentIndex++ )
	{
		segmentsAtPoint.insert( std::make_pair( PointKey( segments[segmentIndex].start.position ), (int)segmentIndex ) );
		segmentsAtPoint.insert( std::make_pair( PointKey( segments[segmentIndex].end.position ), (int)segmentIndex ) );
	}

	std::vector<bool> used( segments.size(), false );
	for ( size_t firstSegment = 0; firstSegment < segments.size(); firstSegment++ )
	{
		if ( used[firstSegment] )
		{
			continue;
		}
		used[firstSegment] = true;
		std::vector<PosedVertex> outline;
		outline.push_back( segments[firstSegment].start );
		outline.push_back( segments[firstSegment].end );
		const long long startKey = PointKey( segments[firstSegment].start.position );
		PosedVertex current = segments[firstSegment].end;

		for ( ;; )
		{
			const long long currentKey = PointKey( current.position );
			if ( currentKey == startKey )
			{
				outline.pop_back();		// back where it started: the last point repeats the first
				break;
			}
			int nextSegment = -1;
			const std::pair<std::multimap<long long, int>::iterator, std::multimap<long long, int>::iterator> candidates =
				segmentsAtPoint.equal_range( currentKey );
			for ( std::multimap<long long, int>::iterator candidate = candidates.first; candidate != candidates.second; ++candidate )
			{
				if ( !used[candidate->second] )
				{
					nextSegment = candidate->second;
					break;
				}
			}
			if ( nextSegment < 0 )
			{
				break;		// an open outline
			}
			used[nextSegment] = true;
			const CutSegment &segment = segments[nextSegment];
			current = ( PointKey( segment.start.position ) == currentKey ) ? segment.end : segment.start;
			outline.push_back( current );
		}

		if ( outline.size() >= 3 )
		{
			outlines.push_back( outline );
		}
	}
}

// True if point p is inside the triangle a, b, c (counter-clockwise), or on its edge.
bool IsInsideTriangle( float px, float py, float ax, float ay, float bx, float by, float cx, float cy )
{
	const float side1 = ( bx - ax ) * ( py - ay ) - ( by - ay ) * ( px - ax );
	const float side2 = ( cx - bx ) * ( py - by ) - ( cy - by ) * ( px - bx );
	const float side3 = ( ax - cx ) * ( py - cy ) - ( ay - cy ) * ( px - cx );
	return side1 >= 0.0f && side2 >= 0.0f && side3 >= 0.0f;
}

// Splits a flat outline (2D points) into triangles by cutting off "ears" one at a time. The
// triangles come out counter-clockwise. A badly tangled outline that runs out of ears is finished
// as a fan.
void TriangulateOutline( const std::vector<float> &x, const std::vector<float> &y, std::vector<int> &triangles )
{
	const int pointCount = (int)x.size();
	std::vector<int> remaining;
	for ( int point = 0; point < pointCount; point++ )
	{
		remaining.push_back( point );
	}
	float doubleArea = 0.0f;
	for ( int point = 0; point < pointCount; point++ )
	{
		const int next = ( point + 1 ) % pointCount;
		doubleArea += x[point] * y[next] - x[next] * y[point];
	}
	if ( doubleArea < 0.0f )
	{
		std::vector<int> reversed( remaining.rbegin(), remaining.rend() );
		remaining.swap( reversed );
	}

	while ( remaining.size() > 3 )
	{
		bool clippedAnEar = false;
		const int count = (int)remaining.size();
		for ( int corner = 0; corner < count && !clippedAnEar; corner++ )
		{
			const int a = remaining[( corner + count - 1 ) % count];
			const int b = remaining[corner];
			const int c = remaining[( corner + 1 ) % count];
			const float turn = ( x[b] - x[a] ) * ( y[c] - y[a] ) - ( y[b] - y[a] ) * ( x[c] - x[a] );
			if ( turn <= 1.0e-6f )
			{
				continue;		// a dent in the outline (or a straight run): not an ear
			}
			bool containsAnotherPoint = false;
			for ( int other = 0; other < count && !containsAnotherPoint; other++ )
			{
				const int point = remaining[other];
				if ( point != a && point != b && point != c && IsInsideTriangle( x[point], y[point], x[a], y[a], x[b], y[b], x[c], y[c] ) )
				{
					containsAnotherPoint = true;
				}
			}
			if ( containsAnotherPoint )
			{
				continue;
			}
			triangles.push_back( a );
			triangles.push_back( b );
			triangles.push_back( c );
			remaining.erase( remaining.begin() + corner );
			clippedAnEar = true;
		}
		if ( !clippedAnEar )
		{
			break;
		}
	}
	for ( size_t corner = 1; corner + 1 < remaining.size(); corner++ )
	{
		triangles.push_back( remaining[0] );
		triangles.push_back( remaining[corner] );
		triangles.push_back( remaining[corner + 1] );
	}
}

// Adds the cap closing one cut face to a side. The triangles are added both ways round, so the cap
// shows whichever way the renderer culls; the normals point out of the piece.
void AddCap( PosedMesh &side, const std::vector<PosedVertex> &outline, const std::vector<int> &triangles, const Vector3 &outward,
			 const Vector3 &planePoint, const Vector3 &axisU, const Vector3 &axisV, const std::string &capShaderName )
{
	const int shaderSlot = side.ShaderSlot( capShaderName );
	const int firstVertex = (int)side.vertices.size();
	for ( size_t point = 0; point < outline.size(); point++ )
	{
		PosedVertex vertex = outline[point];	// keeps the bones that move it
		vertex.normal = outward;
		vertex.s = ragdoll::Dot( vertex.position - planePoint, axisU ) / CAP_TEXTURE_SIZE;
		vertex.t = ragdoll::Dot( vertex.position - planePoint, axisV ) / CAP_TEXTURE_SIZE;
		side.vertices.push_back( vertex );
	}
	for ( size_t index = 0; index + 2 < triangles.size(); index += 3 )
	{
		side.indexes.push_back( firstVertex + triangles[index] );
		side.indexes.push_back( firstVertex + triangles[index + 1] );
		side.indexes.push_back( firstVertex + triangles[index + 2] );
		side.triangleShaders.push_back( shaderSlot );
		side.indexes.push_back( firstVertex + triangles[index] );
		side.indexes.push_back( firstVertex + triangles[index + 2] );
		side.indexes.push_back( firstVertex + triangles[index + 1] );
		side.triangleShaders.push_back( shaderSlot );
	}
}

} // namespace

void SplitMesh( const PosedMesh &mesh, const std::vector<int> &vertexParts, int partCount, const Vector3 &planePoint,
				const Vector3 &planeNormal, const std::string &capShaderName, std::vector<PosedMesh> &parts )
{
	parts.assign( partCount > 0 ? partCount : 0, PosedMesh() );
	if ( partCount <= 0 || vertexParts.size() != mesh.vertices.size() )
	{
		return;
	}

	const int vertexCount = (int)mesh.vertices.size();
	std::vector<float> distance( vertexCount );
	for ( int vertex = 0; vertex < vertexCount; vertex++ )
	{
		distance[vertex] = ragdoll::Dot( mesh.vertices[vertex].position - planePoint, planeNormal );
	}
	std::vector<PartBuilder> builders;
	builders.reserve( partCount );
	for ( int part = 0; part < partCount; part++ )
	{
		builders.push_back( PartBuilder( &mesh, &parts[part] ) );
	}

	// Where the cut crosses each edge, worked out once per edge (from its lower-numbered end), so the
	// triangles on both sides of an edge agree exactly on the point.
	std::map<long long, PosedVertex> crossingOfEdge;
	// The cut's segments, kept apart for every pair of parts they divide (front part, back part).
	std::map< std::pair<int, int>, std::vector<CutSegment> > segmentsBetweenParts;

	for ( size_t index = 0; index + 2 < mesh.indexes.size(); index += 3 )
	{
		const int corners[3] = { mesh.indexes[index], mesh.indexes[index + 1], mesh.indexes[index + 2] };
		const int shaderSlot = mesh.triangleShaders[index / 3];
		const int cornerParts[3] = { vertexParts[corners[0]], vertexParts[corners[1]], vertexParts[corners[2]] };
		if ( cornerParts[0] == cornerParts[1] && cornerParts[1] == cornerParts[2] )
		{
			builders[cornerParts[0]].WholeTriangle( corners, shaderSlot );
			continue;
		}

		// Does the plane divide this triangle between two parts?
		const bool inFront[3] = { distance[corners[0]] >= 0.0f, distance[corners[1]] >= 0.0f, distance[corners[2]] >= 0.0f };
		int frontPart = -1;
		int backPart = -1;
		bool dividedByPlane = true;
		for ( int corner = 0; corner < 3; corner++ )
		{
			int &sidePart = inFront[corner] ? frontPart : backPart;
			if ( sidePart < 0 )
			{
				sidePart = cornerParts[corner];
			}
			else if ( sidePart != cornerParts[corner] )
			{
				dividedByPlane = false;
			}
		}
		if ( !dividedByPlane || frontPart < 0 || backPart < 0 || frontPart == backPart )
		{
			// Goes whole to the part most of its corners belong to.
			const int majorityPart = ( cornerParts[1] == cornerParts[2] ) ? cornerParts[1] : cornerParts[0];
			builders[majorityPart].WholeTriangle( corners, shaderSlot );
			continue;
		}

		// Cut it: walk around it, keeping each corner in its own part and adding the crossing point
		// of every edge that changes side to both.
		PartBuilder &frontBuilder = builders[frontPart];
		PartBuilder &backBuilder = builders[backPart];
		int frontCorners[4];
		int backCorners[4];
		int frontCount = 0;
		int backCount = 0;
		PosedVertex crossings[2];
		int crossingCount = 0;
		for ( int edge = 0; edge < 3; edge++ )
		{
			const int from = corners[edge];
			const int to = corners[( edge + 1 ) % 3];
			if ( inFront[edge] )
			{
				frontCorners[frontCount++] = frontBuilder.Original( from );
			}
			else
			{
				backCorners[backCount++] = backBuilder.Original( from );
			}
			if ( inFront[edge] == inFront[( edge + 1 ) % 3] )
			{
				continue;
			}

			const int lower = ( from < to ) ? from : to;
			const int higher = ( from < to ) ? to : from;
			const long long edgeKey = (long long)lower * vertexCount + higher;
			std::map<long long, PosedVertex>::iterator known = crossingOfEdge.find( edgeKey );
			if ( known == crossingOfEdge.end() )
			{
				const float fraction = distance[lower] / ( distance[lower] - distance[higher] );
				known = crossingOfEdge.insert( std::make_pair( edgeKey, Interpolated( mesh.vertices[lower], mesh.vertices[higher], fraction ) ) ).first;
			}
			frontCorners[frontCount++] = frontBuilder.New( known->second );
			backCorners[backCount++] = backBuilder.New( known->second );
			if ( crossingCount < 2 )
			{
				crossings[crossingCount++] = known->second;
			}
		}
		frontBuilder.Polygon( frontCorners, frontCount, shaderSlot );
		backBuilder.Polygon( backCorners, backCount, shaderSlot );
		if ( crossingCount == 2 )
		{
			CutSegment segment = { crossings[0], crossings[1] };
			segmentsBetweenParts[std::make_pair( frontPart, backPart )].push_back( segment );
		}
	}

	// Close the cut faces. Each outline is flattened onto the plane, split into triangles and added
	// to the two parts it divides, facing out of each.
	if ( capShaderName.empty() )
	{
		return;
	}
	const Vector3 axisU = ragdoll::Normalized( ragdoll::AnyPerpendicular( planeNormal ) );
	const Vector3 axisV = ragdoll::Cross( planeNormal, axisU );
	for ( std::map< std::pair<int, int>, std::vector<CutSegment> >::const_iterator between = segmentsBetweenParts.begin();
		  between != segmentsBetweenParts.end(); ++between )
	{
		std::vector< std::vector<PosedVertex> > outlines;
		TraceOutlines( between->second, outlines );
		for ( size_t outlineIndex = 0; outlineIndex < outlines.size(); outlineIndex++ )
		{
			const std::vector<PosedVertex> &outline = outlines[outlineIndex];
			std::vector<float> x( outline.size() );
			std::vector<float> y( outline.size() );
			for ( size_t point = 0; point < outline.size(); point++ )
			{
				x[point] = ragdoll::Dot( outline[point].position - planePoint, axisU );
				y[point] = ragdoll::Dot( outline[point].position - planePoint, axisV );
			}
			std::vector<int> triangles;
			TriangulateOutline( x, y, triangles );
			AddCap( parts[between->first.first], outline, triangles, planeNormal * -1.0f, planePoint, axisU, axisV, capShaderName );
			AddCap( parts[between->first.second], outline, triangles, planeNormal, planePoint, axisU, axisV, capShaderName );
		}
	}
}

} // namespace slicing
