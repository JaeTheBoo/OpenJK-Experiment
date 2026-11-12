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

// bg_pmove_momentum.cpp -- momentum based ground movement for the player.
// See bg_pmove_momentum.h for an overview of the movement model.

#include "common_headers.h"

#define GAME_INCLUDE
#include "../qcommon/q_shared.h"
#include "g_shared.h"
#include "g_local.h"
#include "bg_pmove_momentum.h"

#include <math.h>

cvar_t *g_momentumMovement;
cvar_t *g_momentumAccelerationTime;
cvar_t *g_momentumStopTime;
cvar_t *g_momentumTurnTime;
cvar_t *g_momentumOverspeedDecayTime;
cvar_t *g_momentumAirControl;

// The timing cvars are divided by, so they are never allowed to go below this value.
static const float MINIMUM_TIMING_SECONDS = 0.01f;

// Below this speed (in game units per second) the character is considered to be standing still.
static const float STANDING_STILL_SPEED = 1.0f;

void PM_Momentum_RegisterCvars( void )
{
	g_momentumMovement				= gi.cvar( "g_momentumMovement", "1", CVAR_ARCHIVE );
	g_momentumAccelerationTime		= gi.cvar( "g_momentumAccelerationTime", "0.25", CVAR_ARCHIVE );
	g_momentumStopTime				= gi.cvar( "g_momentumStopTime", "0.25", CVAR_ARCHIVE );
	g_momentumTurnTime				= gi.cvar( "g_momentumTurnTime", "0.12", CVAR_ARCHIVE );
	g_momentumOverspeedDecayTime	= gi.cvar( "g_momentumOverspeedDecayTime", "0.6", CVAR_ARCHIVE );
	g_momentumAirControl			= gi.cvar( "g_momentumAirControl", "1.0", CVAR_ARCHIVE );
}

/*
==================
PM_Momentum_ReadTimingCvar

Returns a timing cvar in seconds, clamped so it is always safe to divide by.
==================
*/
static float PM_Momentum_ReadTimingCvar( const cvar_t *timingCvar )
{
	if ( timingCvar->value < MINIMUM_TIMING_SECONDS )
	{
		return MINIMUM_TIMING_SECONDS;
	}
	return timingCvar->value;
}

/*
==================
PM_Momentum_SetSpeed

Rescales a velocity so its length becomes newSpeed while keeping its direction.
currentSpeed must be the velocity's current length.
==================
*/
static void PM_Momentum_SetSpeed( vec3_t velocity, float currentSpeed, float newSpeed )
{
	if ( newSpeed < STANDING_STILL_SPEED || currentSpeed < STANDING_STILL_SPEED )
	{
		VectorClear( velocity );
		return;
	}
	VectorScale( velocity, newSpeed / currentSpeed, velocity );
}

void PM_Momentum_ApplyGroundMovement( vec3_t velocity, const vec3_t wishDirection, float wishSpeed,
									  float runSpeed, float frameTimeSeconds )
{
	if ( frameTimeSeconds <= 0.0f )
	{
		return;
	}

	const float currentSpeed = VectorLength( velocity );

	// The braking and overspeed rates are measured against the running speed. If the running
	// speed is zero (for example the character has been frozen by a script) brake relative to
	// the current speed instead so the character still comes to a stop.
	const float referenceSpeed = ( runSpeed > 0.0f ) ? runSpeed : currentSpeed;

	// No movement keys held: brake at a constant rate until standing still.
	if ( wishSpeed <= 0.0f )
	{
		const float brakingRate = referenceSpeed / PM_Momentum_ReadTimingCvar( g_momentumStopTime );
		const float brakedSpeed = currentSpeed - ( brakingRate * frameTimeSeconds );
		PM_Momentum_SetSpeed( velocity, currentSpeed, brakedSpeed );
		return;
	}

	// Step 1: Steer the current velocity toward the requested direction while keeping its speed.
	// The blend uses an exponential curve so the result does not depend on the frame rate.
	// Blending toward the opposite direction passes through a shorter vector, which is what
	// produces a brief skid when the player reverses direction at full speed.
	const float turnTime = PM_Momentum_ReadTimingCvar( g_momentumTurnTime );
	const float steeringBlend = 1.0f - expf( -frameTimeSeconds / turnTime );

	vec3_t steeredTargetVelocity;
	vec3_t steeringCorrection;
	VectorScale( wishDirection, currentSpeed, steeredTargetVelocity );
	VectorSubtract( steeredTargetVelocity, velocity, steeringCorrection );
	VectorMA( velocity, steeringBlend, steeringCorrection, velocity );

	const float speedAfterSteering = VectorLength( velocity );

	// Step 2a: Slower than requested, so accelerate along the requested direction.
	// The rate is relative to the requested speed so walking and running both take the same time
	// to reach their top speed.
	if ( speedAfterSteering < wishSpeed )
	{
		const float accelerationRate = wishSpeed / PM_Momentum_ReadTimingCvar( g_momentumAccelerationTime );
		VectorMA( velocity, accelerationRate * frameTimeSeconds, wishDirection, velocity );

		const float speedAfterAcceleration = VectorLength( velocity );
		if ( speedAfterAcceleration > wishSpeed )
		{
			PM_Momentum_SetSpeed( velocity, speedAfterAcceleration, wishSpeed );
		}
		return;
	}

	// Step 2b: Faster than requested, so bleed the extra speed off gradually but never drop
	// below the requested speed. This keeps momentum from jumps and slides.
	const float overspeedDecayRate = referenceSpeed / PM_Momentum_ReadTimingCvar( g_momentumOverspeedDecayTime );
	float decayedSpeed = speedAfterSteering - ( overspeedDecayRate * frameTimeSeconds );
	if ( decayedSpeed < wishSpeed )
	{
		decayedSpeed = wishSpeed;
	}
	PM_Momentum_SetSpeed( velocity, speedAfterSteering, decayedSpeed );
}

float PM_Momentum_ScaleAirAcceleration( float originalAirAcceleration )
{
	const float airControlScale = ( g_momentumAirControl->value > 0.0f ) ? g_momentumAirControl->value : 0.0f;
	return originalAirAcceleration * airControlScale;
}
