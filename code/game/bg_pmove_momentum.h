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

// bg_pmove_momentum.h -- momentum based ground movement for the player (single player only)
//
// The original Quake 3 derived movement reaches full running speed in roughly 0.1 seconds
// and turns almost instantly, which makes characters feel weightless. This module replaces
// the friction + acceleration step for the player when walking on ordinary ground with a
// velocity steering model similar to the one used by modern third person action games:
//
//   * Speed builds up over a configurable time instead of instantly.
//   * Releasing the movement keys brakes at a constant rate, so the character comes to a
//     smooth, weighted stop instead of a sudden one.
//   * Changing direction steers the existing velocity toward the new direction over time.
//     Sharp reversals therefore cause a short skid instead of an instant about-face.
//   * Speed above the normal running speed (landing from a jump, leaving a crouch slide,
//     force speed ending) bleeds off gradually instead of being removed by friction at once.
//
// Every value is exposed as a console variable so it can be tuned while playing.
// Setting g_momentumMovement to 0 restores the original movement exactly.

#ifndef BG_PMOVE_MOMENTUM_H
#define BG_PMOVE_MOMENTUM_H

#include "../qcommon/q_shared.h"

// Master switch. 1 = momentum movement for the player, 0 = original movement.
extern cvar_t *g_momentumMovement;

// Seconds needed to go from standing still to full running speed.
extern cvar_t *g_momentumAccelerationTime;

// Seconds needed to brake from full running speed to a standstill after releasing the keys.
extern cvar_t *g_momentumStopTime;

// Seconds for the velocity to swing most of the way (about 63 percent) toward a new input
// direction. Smaller values turn more sharply, larger values feel heavier and skid more.
extern cvar_t *g_momentumTurnTime;

// Seconds needed to bleed off one full running speed worth of excess speed while still
// holding a direction (for example right after landing from a fast jump).
extern cvar_t *g_momentumOverspeedDecayTime;

// Multiplier for steering control while airborne. 1 = original air control, 0 = none.
extern cvar_t *g_momentumAirControl;

/*
==================
PM_Momentum_RegisterCvars

Creates the console variables listed above. Called once from InitGame.
==================
*/
void PM_Momentum_RegisterCvars( void );

/*
==================
PM_Momentum_ApplyGroundMovement

Updates a ground velocity for one movement frame. Replaces both PM_Friction and
PM_Accelerate while the player is walking on ordinary ground.

velocity          The current velocity, already lying in the ground plane. Modified in place.
wishDirection     Unit vector of the direction the player is asking to move in, projected onto
                  the ground plane. Ignored when wishSpeed is zero.
wishSpeed         The speed the player is asking for (zero when no movement keys are held).
runSpeed          The character's normal running speed. All of the timing cvars are measured
                  relative to this speed so the feel stays the same when the speed changes.
frameTimeSeconds  Duration of this movement frame in seconds.
==================
*/
void PM_Momentum_ApplyGroundMovement( vec3_t velocity, const vec3_t wishDirection, float wishSpeed,
									  float runSpeed, float frameTimeSeconds );

/*
==================
PM_Momentum_ScaleAirAcceleration

Returns the air acceleration to use for the player after applying g_momentumAirControl.
==================
*/
float PM_Momentum_ScaleAirAcceleration( float originalAirAcceleration );

#endif // BG_PMOVE_MOMENTUM_H
