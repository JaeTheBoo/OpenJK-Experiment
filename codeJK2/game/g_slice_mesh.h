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
//   3. SliceMesh cuts the posed mesh in two along a plane: triangles crossing the plane are split,
//      and the faces the cut opens are closed with "cap" triangles using the skin's own cap shader
//      (the cauterized texture the game uses for its built-in dismemberment), so no new art is needed.

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

// A vertex of a posed mesh (world space).
struct PosedVertex
{
	Vector3 position;
	Vector3 normal;
	float s, t;
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
// rest pose, then placed in the world by modelToWorld at the model's scale.
void PoseCharacterMesh( const CharacterMesh &mesh, const ragdoll::Skeleton &skeleton, const Transform *boneFrames,
						const Transform &modelToWorld, float modelScale, PosedMesh &posed );

// Cuts a posed mesh in two along a plane. front receives everything on the side the plane's normal
// points to, back everything on the other side; triangles crossing the plane are split between
// them, and the faces the cut opens are closed with cap triangles drawn with capShaderName.
// Returns false (leaving both empty) if the plane does not pass through the mesh.
bool SliceMesh( const PosedMesh &mesh, const Vector3 &planePoint, const Vector3 &planeNormal,
				const std::string &capShaderName, PosedMesh &front, PosedMesh &back );

} // namespace slicing

#endif // G_SLICE_MESH_H
