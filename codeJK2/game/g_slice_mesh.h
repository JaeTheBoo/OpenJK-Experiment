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

// g_slice_mesh.h -- the geometry behind lightsaber slicing (Jedi Outcast).
//
//   1. LoadCharacterMesh reads a character's mesh (its .glm file, most detailed level of detail) and
//      the skin that says which shader each part is drawn with.
//   2. PoseCharacterMesh moves every vertex by its bones, exactly as the model is drawn right now,
//      and places the result in the world.
//   3. SplitMesh divides the posed mesh into parts along a plane: triangles the plane divides between
//      two parts are split, and the faces the cut opens are closed with "cap" triangles using the
//      skin's own cap shader (the cauterized texture the game uses for its built-in dismemberment),
//      so no new art is needed. Which part each vertex belongs to is decided by the caller (from the
//      ragdoll bodies), so a cut can stop where the blade could not reach.

#ifndef G_SLICE_MESH_H
#define G_SLICE_MESH_H

#include "g_ragdoll_math.h"

#include <string>
#include <vector>

namespace ragdoll
{
struct Skeleton;
}

namespace slicing
{

using ragdoll::Vector3;
using ragdoll::Transform;

// One vertex of a character's mesh in its rest pose, with the bones (up to four) that move it.
struct BindVertex
{
	Vector3 position;		// model space, rest pose
	Vector3 normal;
	float s, t;				// texture coordinates
	int boneCount;
	int bones[4];			// skeleton bone indices
	float weights[4];		// how much each bone moves the vertex (they add up to one)
};

// One drawable part of a character's mesh (for example the torso or the helmet).
struct MeshSurface
{
	std::string name;
	std::string shaderName;			// from the character's skin
	std::vector<BindVertex> vertices;
	std::vector<int> indexes;		// three per triangle
};

// A character's whole mesh, as read from its .glm file and skin.
struct CharacterMesh
{
	std::vector<MeshSurface> surfaces;
	std::string capShaderName;		// the skin's shader for its cut-off ("cap") surfaces, used for cut faces
};

// Reads a character's mesh and skin, remembering the result for next time. Returns NULL if the
// mesh cannot be read.
const CharacterMesh *LoadCharacterMesh( const char *meshPath, const char *skinPath );
void ClearCharacterMeshCache( void );

// A vertex of a posed mesh (world space). The bones that moved it are kept, so the pieces of a cut
// can bend with the ragdoll bodies those bones belong to.
struct PosedVertex
{
	Vector3 position;
	Vector3 normal;
	float s, t;
	int boneCount;
	int bones[4];			// skeleton bone indices
	float weights[4];
};

// A posed triangle mesh. Each triangle is drawn with one of shaderNames.
struct PosedMesh
{
	std::vector<PosedVertex> vertices;
	std::vector<int> indexes;				// three per triangle
	std::vector<int> triangleShaders;		// one per triangle: an index into shaderNames
	std::vector<std::string> shaderNames;

	void Clear( void );
	int ShaderSlot( const std::string &shaderName );	// the index of a shader name, added if new
};

// Poses the mesh exactly as the model is drawn. boneFrames are the bones' current frames in model
// space (as ragdoll::ReadCurrentBoneFrames reads them); each vertex is moved by its bones from the
// rest pose, then placed in the world by modelToWorld at the model's scale. If surfaceIsShown is
// given, surfaces it marks false (switched off on the model) are left out.
void PoseCharacterMesh( const CharacterMesh &mesh, const ragdoll::Skeleton &skeleton, const Transform *boneFrames,
						const Transform &modelToWorld, float modelScale, PosedMesh &posed,
						const std::vector<bool> *surfaceIsShown = NULL );

// Turns a mesh posed in the rest pose (at scale 1, in model space) back into a mesh that can be posed:
// one surface per shader. Used for what is left of a maimed character after its cuts.
void MakeRestMesh( const PosedMesh &restPosed, CharacterMesh &mesh );

// Divides a posed mesh into partCount parts. vertexParts gives the part of every vertex.
//   - A triangle whose corners are all in one part goes to that part.
//   - A triangle the plane divides between two parts (its corners on each side of the plane all
//     belong to one part, a different one on each side) is cut along the plane, and the outlines of
//     these cuts are closed with cap triangles drawn with capShaderName on both parts.
//   - Any other triangle goes whole to the part most of its corners belong to.
void SplitMesh( const PosedMesh &mesh, const std::vector<int> &vertexParts, int partCount, const Vector3 &planePoint,
				const Vector3 &planeNormal, const std::string &capShaderName, std::vector<PosedMesh> &parts );

} // namespace slicing

#endif // G_SLICE_MESH_H
