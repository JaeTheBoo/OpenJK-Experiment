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

// g_slice.h -- lightsaber slicing for Jedi Academy (ported from Jedi Outcast's codeJK2/game, see the
// differences below). Replaces the game's own dismemberment, which can only remove a few
// pre-modelled limbs.
//
// A killing lightsaber blow cuts the character along the plane of the blade's swing, through the
// body parts the blade can reach. The character's mesh is posed exactly as it is drawn and divided
// along that plane; the cut faces are closed with the game's own cauterized cap texture. Whatever the
// cut separates (a hand, an arm, a head, the upper body, a whole half) becomes a limp ragdoll of its
// own, drawn as a run-time mesh (RT_DYNAMIC_MESH) that bends with its ragdoll bodies. See
// g_slice_mesh.h for the geometry.
//
// A blow that does not kill can still take off an arm, or part of one: the character lives on without
// it (Ghoul2 stops drawing its model and what is left of its mesh is drawn instead), and drops its
// weapon if it loses its right hand. Legs, heads and bodies only come off with a killing blow. The
// player is never cut. Severed pieces can be cut again by a passing blade, and the cuts maimed
// characters survived are kept in saved games.
//
//   g_saberSlicing 1   lightsaber blows cut characters (0: the game's own dismemberment instead)
//
// For testing, a console command makes cuts through the whole body (cheats must be on):
//   slice      a vertical cut straight down the middle of the character under the crosshair
//   slice h    a horizontal cut just above the hips
//   slice d    a diagonal cut from shoulder to hip
//   slice arm  the right forearm comes off, and the character lives on
//   slice piece  the severed piece nearest the crosshair is cut down the middle
// Cuts are made the next time the character is drawn, from exactly the pose on screen.
//
// Jedi Academy (this port) differs from Jedi Outcast in a few places:
//   * A character may carry two sabers, or a saber with several blades (a staff). Every lit blade
//     cuts: the blow is cut along the swing of whichever blade passed closest to where it landed, and
//     every blade sweeps through severed pieces. Blades the saber's definition keeps from
//     dismembering (blunt or stabbing weapons) do not cut.
//   * Losing the left hand drops the saber held in it (as a saber pickup, the way a dead character
//     drops it); losing the right hand drops the right saber, a staff or any other weapon.
//   * Skins made of three files (the customizable characters and Jedi) are read like the renderer
//     reads them, and the pieces keep the character's colour tint.
//   * Characters the game carries itself (vehicle riders, characters held by a rancor, a wampa or a
//     sand creature) and vehicles are never cut.
//   * Saved games carry the cuts in as many script variables as they need: one may not hold more
//     than 1023 characters.

#ifndef G_SLICE_H
#define G_SLICE_H

#include "g_local.h"

void G_Slice_RegisterCvars( void );

// Called by G_Damage when a lightsaber blow kills a character (or strikes a dead one). Works out the
// plane of the blade's swing and asks for the character to be cut along it. hitPoint is where the
// blade struck (may be NULL). Returns qtrue if the character will be cut, in which case the game's
// own dismemberment is skipped. A thrust, or a blade that is not moving, does not cut.
qboolean G_Slice_SaberKill( gentity_t *target, gentity_t *attacker, const vec3_t hitPoint );

// Called by G_Damage when a lightsaber blow the game judges able to take off a limb wounds a character
// without killing it. If the blade cut clean through an arm, the arm comes off and the character lives
// on without it.
void G_Slice_SaberWound( gentity_t *target, gentity_t *attacker, const vec3_t hitPoint, int damage, int hitLocation );

// Called every frame for every lightsaber blade (WP_SaberDamageTrace): severed pieces the blade
// sweeps through are cut again.
void G_Slice_SaberSweep( gentity_t *saberUser, int saberNum, int bladeNum );

// Called by CG_Player after it adds a character's model to the scene. Ghoul2 does not draw a maimed
// character's own model (the weapon in its hand still is drawn); this draws what is left of its body,
// where, and in the colour, drawnModel was drawn.
void G_Slice_AddMaimedCharacter( gentity_t *entity, const refEntity_t &drawnModel, int renderTime );

// Saved games: the cuts maimed characters survived are saved with the game's script variables and
// made again after loading.
void G_Slice_WriteSaveGame( void );			// just before the script variables are saved
void G_Slice_FinishWriteSaveGame( void );	// just after they are saved
void G_Slice_ReadSaveGame( void );			// just after they are loaded

// The "slice" console command.
void G_Slice_Command( gentity_t *user, const char *mode );

// Called from G_Ragdoll_UpdateAndPose for every character about to be drawn. If a cut was asked for,
// makes it from the pose on screen, replaces the character with its pieces and returns qtrue.
qboolean G_Slice_CutIfRequested( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3] );

// True once a character has been replaced by its pieces: CG_Player then no longer draws it, and Jedi
// Academy's own ragdoll (G_RagDoll) leaves it alone.
qboolean G_Slice_WasCut( int entityNumber );

// Called when an entity is freed (its slot may be reused by a new character).
void G_Slice_ForgetCharacter( int entityNumber );

// Called once per drawn frame by the cgame: moves the pieces and adds them to the scene.
void G_Slice_AddPiecesToScene( int renderTime );

// Forgets every piece and cached mesh. Called when a level starts or ends.
void G_Slice_Reset( void );

#endif // G_SLICE_H
