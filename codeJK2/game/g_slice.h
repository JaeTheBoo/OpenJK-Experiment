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

// g_slice.h -- lightsaber slicing, prototype (Jedi Outcast).
//
// A character can be cut in two along any plane: its mesh is posed exactly as it is drawn, cut
// along the plane, the cut faces are closed with the game's own cauterized cap texture, and the
// two pieces replace the character. They are drawn as run-time meshes (RT_DYNAMIC_MESH) and fall
// as simple rigid bodies. See g_slice_mesh.h for the geometry.
//
// For now a console command makes the cuts (cheats must be on); the lightsaber is not hooked up yet:
//   slice      a vertical cut straight down the middle of the character under the crosshair
//   slice h    a horizontal cut just above the hips
//   slice d    a diagonal cut from shoulder to hip
// The cut is made the next time the character is drawn, from exactly the pose on screen.

#ifndef G_SLICE_H
#define G_SLICE_H

#include "g_local.h"

// The "slice" console command.
void G_Slice_Command( gentity_t *user, const char *mode );

// Called from G_Ragdoll_UpdateAndPose for every character about to be drawn. If a cut was asked for,
// makes it from the pose on screen, replaces the character with its pieces and returns qtrue.
qboolean G_Slice_CutIfRequested( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3] );

// True once a character has been replaced by its pieces: CG_Player then no longer draws it.
qboolean G_Slice_WasCut( int entityNumber );

// Called when an entity is freed (its slot may be reused by a new character).
void G_Slice_ForgetCharacter( int entityNumber );

// Called once per drawn frame by the cgame: moves the pieces and adds them to the scene.
void G_Slice_AddPiecesToScene( int renderTime );

// Forgets every piece and cached mesh. Called when a level starts or ends.
void G_Slice_Reset( void );

#endif // G_SLICE_H
