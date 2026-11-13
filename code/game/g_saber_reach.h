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

// g_saber_reach.h -- a lightsaber deflecting a blaster bolt moves to meet it.
//
// The game deflects bolts wherever they strike: Jedi play one of a few canned parries chosen by
// where the bolt is, and the player's guard (see g_saber_guard.h) simply holds the stance the view
// aims. On top of those animations the saber arm now reaches for each bolt that is about to be
// deflected, so the blade crosses the bolt's path just as it arrives:
//   1. Shortly before a bolt that will be deflected arrives (for the player: the guard is up, the
//      bolt is in front and there is Force to pay; for NPCs: their blade is blocking and the bolt
//      comes from in front), the point where the blade can best meet the bolt's path is chosen in
//      front of the body: blade up for high bolts, down for low ones, leaning toward the bolt's side,
//      held across the bolt's path, with the arm, the hilt and the blade clear of the torso and head.
//   2. The shoulder, elbow and wrist blend toward that pose (two-bone inverse kinematics plus a
//      wrist turn) over the time left before the bolt arrives, never quicker than about 90 ms.
//   3. The pose holds briefly after the deflection, then blends back to the animation over a fifth of
//      a second. A bolt following close behind takes the blade straight on from where it is, so a
//      volley flows from one bolt to the next.
//   4. The deflection spark is shown where the blade meets the bolt.
// The torso and legs keep their animation. If the animation holds the hilt with both hands, the
// left hand stays on it while it can reach, and otherwise lets go and hangs at the side until the
// blade is back. Single sabers and the right-hand saber of a pair reach; staffs (and other sabers
// with more than one blade) keep their animation. Saber attacks, saber locks, knockdowns, rolls and
// Force powers that busy the hands are never interrupted.
//
// It is only a matter of looks: which bolts are deflected, what they cost and the guard's rules do
// not change. Nothing is kept in saved games.
//
//   g_saberReach 1   the saber arm reaches for the bolts it deflects (0: the animation alone)
//   g_saberReach 2   also prints every reach to the console (and, for reachbolt's target, why it
//                    is not reaching)
//   g_saberReach 3   (testing) also keeps each reached pose until the next bolt, for screenshots
//
// For testing, a console command fires a blaster bolt at the character under the crosshair (or the
// nearest NPC) from a point placed relative to that character (cheats must be on):
//   reachbolt <right> <up> [distance] [speed] [me]
//      right, up  where the bolt comes from: units to the target's right, and the height above its
//                 chest it arrives at
//      distance   how far in front of the target it starts (default 300)
//      speed      units per second (default 1150, a stormtrooper's bolt on normal difficulty)
//      me         fire it at the player instead
// The bolt belongs to the player, or to the nearest NPC when it is fired at the player.

#ifndef G_SABER_REACH_H
#define G_SABER_REACH_H

#include "g_local.h"

void G_SaberReach_RegisterCvars( void );

// Forgets every reach. Called when a level starts or ends (including loading a saved game).
void G_SaberReach_Reset( void );

// Called from CG_Player every frame for every drawn character, after the ragdoll and slicing code
// (G_Ragdoll_UpdateAndPose) and before the saber and hand positions are read. Looks for a bolt the
// character's saber is about to deflect and poses the saber arm for it. renderOrigin and renderAxis are
// where and how the model is drawn this frame.
void G_SaberReach_UpdateAndPose( gentity_t *entity, int renderTime, const vec3_t renderOrigin, vec3_t renderAxis[3] );

// Called when a deflection effect is shown for a missile. If a blade was reaching for this missile,
// point becomes where the blade meets it; otherwise point is left as it is.
void G_SaberReach_DeflectionPoint( const gentity_t *missile, vec3_t point );

// The "reachbolt" console command (see above).
void G_SaberReach_Command( gentity_t *user );

#endif // G_SABER_REACH_H
