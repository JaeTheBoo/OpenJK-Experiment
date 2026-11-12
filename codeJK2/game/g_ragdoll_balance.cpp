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

// g_ragdoll_balance.cpp -- balance and staggering for living characters. See g_ragdoll_balance.h.

#include "g_headers.h"
#include "g_local.h"
#include "g_ragdoll_balance.h"

#include <math.h>
#include <string.h>

extern qboolean PM_InKnockDown( playerState_t *ps );
extern qboolean PM_InGetUp( playerState_t *ps );
extern qboolean PM_CrouchAnim( int anim );
extern qboolean PM_SpinningSaberAnim( int anim );
extern qboolean PM_FlippingAnim( int anim );
extern qboolean PM_RollingAnim( int anim );
extern qboolean G_StandardHumanoid( const char *modelName );
extern void NPC_SetAnim( gentity_t *ent, int setAnimParts, int anim, int setAnimFlags );

extern cvar_t *g_ragdollDebug;

namespace ragdoll
{

/*
==================
Console variables and tuning
==================
*/

static cvar_t *g_ragdollStumbles;	// 1 = hits can make characters stagger and fall over
static cvar_t *g_ragdollBalance;	// push a character can take in a short time before it staggers

static const float BALANCE_RECOVERY_HALF_LIFE_SECONDS = 0.5f;	// lost balance halves this often
static const float FALL_OVER_BALANCE_MULTIPLE = 2.2f;		// losing this many times g_ragdollBalance knocks a character over
static const float LEG_HIT_BALANCE_MULTIPLE = 1.75f;		// a hit below the hips unsettles this much more

// A stagger barely over the threshold is short and gentle; one close to falling over is long and
// sends the body moving fast. The physics turns that speed into steps (see StepToKeepBalance in
// g_ragdoll.cpp).
static const float SHORTEST_STUMBLE_SECONDS = 0.7f;	// the body travels while staggering, then the legs plant and stop it
static const float LONGEST_STUMBLE_SECONDS = 1.3f;
static const float SLOWEST_STUMBLE_SPEED = 80.0f;		// units per second the hit adds to the body
static const float FASTEST_STUMBLE_SPEED = 170.0f;
static const float RENEWED_STUMBLE_PUSH_SHARE = 0.5f;	// a hit on a character already staggering adds this share of the speed
static const float SMALLEST_LEAN_DEGREES = 10.0f;		// how far the upper body leans with the push
static const float LARGEST_LEAN_DEGREES = 25.0f;
static const float LEAN_RISE_SECONDS = 0.08f;			// the lean builds up over this long, then eases off

// A character that staggers off a ledge and has been in the air this long falls over.
static const int AIRBORNE_MILLISECONDS_BEFORE_FALLING = 100;

// Pushed this closely along the way it faces (from behind), a character falls forwards.
static const float PUSHED_FROM_BEHIND_ALIGNMENT = 0.2f;

struct BalanceState
{
	float lostBalance;				// how far off balance the character is, in units of push
	int lastRecoveryTime;			// level.time when lostBalance was last recovered

	bool stumbling;
	int stumbleStartTime;			// level.time
	int stumbleMilliseconds;		// how long this stagger lasts
	vec3_t stumbleDirection;		// horizontal unit vector: the way the character is staggering
	float leanRadians;				// the largest lean of the upper body during this stagger
	int airborneSinceTime;			// level.time the character left the ground while staggering, or -1

	bool pushPending;				// the physics has not yet sent the body moving for the latest hit
	vec3_t pushDirection;			// horizontal unit vector: the direction of that hit
	float pushSpeed;				// units per second to add to the body

	bool fallRequested;				// pushed over: fall on the next drawn frame
};

static BalanceState balanceStates[MAX_GENTITIES];

void RegisterBalanceCvars( void )
{
	g_ragdollStumbles	= gi.cvar( "g_ragdollStumbles", "1", CVAR_ARCHIVE );
	// 1300: a single blaster shot is only a flinch, two quick ones or a burst make a character stagger,
	// and it takes sustained fire to push one over, so staggering characters often recover and fight on.
	g_ragdollBalance	= gi.cvar( "g_ragdollBalance", "1300", CVAR_ARCHIVE );
}

void ResetBalance( void )
{
	memset( balanceStates, 0, sizeof( balanceStates ) );
}

void ForgetBalance( int entityNumber )
{
	if ( entityNumber >= 0 && entityNumber < MAX_GENTITIES )
	{
		memset( &balanceStates[entityNumber], 0, sizeof( balanceStates[entityNumber] ) );
	}
}

/*
==================
Helpers
==================
*/

static float StumbleThreshold( void )
{
	return ( g_ragdollBalance->value > 1.0f ) ? g_ragdollBalance->value : 1.0f;
}

static float FallOverThreshold( void )
{
	return StumbleThreshold() * FALL_OVER_BALANCE_MULTIPLE;
}

// Lets the character regain the balance it has recovered since the last time this was called.
static void RecoverBalance( BalanceState &state )
{
	const float elapsedSeconds = ( level.time - state.lastRecoveryTime ) / 1000.0f;
	if ( elapsedSeconds > 0.0f )
	{
		state.lostBalance *= powf( 0.5f, elapsedSeconds / BALANCE_RECOVERY_HALF_LIFE_SECONDS );
	}
	state.lastRecoveryTime = level.time;
}

static void FacingDirections( const gentity_t *character, vec3_t forward, vec3_t right )
{
	const vec3_t facingAngles = { 0.0f, character->client->ps.viewangles[YAW], 0.0f };
	AngleVectors( facingAngles, forward, right, NULL );
}

// True while the character can go on staggering: alive and not taken over by something else.
static bool CanKeepStumbling( gentity_t *character )
{
	if ( !character->inuse || !character->client || character->health <= 0 )
	{
		return false;
	}
	playerState_t &state = character->client->ps;
	return state.pm_type == PM_NORMAL
		&& !( state.eFlags & EF_FORCE_GRIPPED )
		&& !( character->s.eFlags & EF_LOCKED_TO_WEAPON )
		&& !PM_InKnockDown( &state )
		&& !PM_InGetUp( &state );
}

// True if a hit can make this character start staggering right now.
static bool CanStartStumbling( gentity_t *character )
{
	if ( !character->client || character->s.number == 0 )
	{
		return false;	// the player's movement is never taken over
	}
	if ( !CanKeepStumbling( character ) || ( character->flags & FL_NO_KNOCKBACK ) || !G_StandardHumanoid( character->NPC_type ) )
	{
		return false;
	}
	switch ( character->client->NPC_class )
	{
	case CLASS_DESANN:
	case CLASS_TAVION:
	case CLASS_LUKE:
		return false;	// bosses stand their ground
	default:
		break;
	}
	const playerState_t &state = character->client->ps;
	return state.groundEntityNum != ENTITYNUM_NONE
		&& state.weapon != WP_SABER		// saber duels keep their own animations
		&& state.saberLockTime <= level.time
		&& !PM_CrouchAnim( state.legsAnim )
		&& !PM_SpinningSaberAnim( state.legsAnim )
		&& !PM_FlippingAnim( state.legsAnim )
		&& !PM_RollingAnim( state.legsAnim );
}

// Starts (or, if already staggering, renews) a stagger after a hit in the given horizontal
// direction, and leaves a push for the physics to send the body moving with.
static void StartStumble( gentity_t *character, BalanceState &state, const vec3_t hitDirection )
{
	// How close to falling over: 0 just past the stagger threshold, 1 at the fall-over threshold.
	const float severity = Clamp( ( state.lostBalance - StumbleThreshold() ) / ( FallOverThreshold() - StumbleThreshold() ), 0.0f, 1.0f );
	const float speed = SLOWEST_STUMBLE_SPEED + ( FASTEST_STUMBLE_SPEED - SLOWEST_STUMBLE_SPEED ) * severity;

	if ( state.stumbling )
	{
		// Already staggering: the new hit bends the stagger toward its own direction and pushes again,
		// more gently (the body is already moving).
		vec3_t combinedDirection;
		VectorAdd( state.stumbleDirection, hitDirection, combinedDirection );
		combinedDirection[2] = 0.0f;
		if ( VectorNormalize( combinedDirection ) > 0.01f )
		{
			VectorCopy( combinedDirection, state.stumbleDirection );
		}
		state.pushSpeed = speed * RENEWED_STUMBLE_PUSH_SHARE;
	}
	else
	{
		VectorCopy( hitDirection, state.stumbleDirection );
		state.pushSpeed = speed;
	}
	VectorCopy( hitDirection, state.pushDirection );
	state.pushPending = true;

	state.stumbling = true;
	state.stumbleStartTime = level.time;
	state.stumbleMilliseconds = (int)( 1000.0f * ( SHORTEST_STUMBLE_SECONDS + ( LONGEST_STUMBLE_SECONDS - SHORTEST_STUMBLE_SECONDS ) * severity ) );
	state.leanRadians = DegreesToRadians( SMALLEST_LEAN_DEGREES + ( LARGEST_LEAN_DEGREES - SMALLEST_LEAN_DEGREES ) * severity );
	state.airborneSinceTime = -1;

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: staggering (balance lost %.0f of %.0f, pushed at %.0f units per second)\n",
				   character->s.number, state.lostBalance, FallOverThreshold(), state.pushSpeed );
	}
}

// Knocks the character over in the direction it was staggering.
static void KnockOver( gentity_t *character, BalanceState &state )
{
	vec3_t forward, right;
	FacingDirections( character, forward, right );
	const int knockdownAnimation = ( DotProduct( forward, state.stumbleDirection ) > PUSHED_FROM_BEHIND_ALIGNMENT )
		? BOTH_KNOCKDOWN3		// falls forwards
		: BOTH_KNOCKDOWN1;		// falls backwards (pushed from the front or the side)

	state.stumbling = false;
	state.pushPending = false;
	state.fallRequested = false;
	state.lostBalance = 0.0f;
	NPC_SetAnim( character, SETANIM_BOTH, knockdownAnimation, SETANIM_FLAG_OVERRIDE | SETANIM_FLAG_HOLD );

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: pushed over\n", character->s.number );
	}
}

/*
==================
Balance
==================
*/

void LoseBalance( gentity_t *character, const vec3_t pushDirection, const vec3_t hitPoint, float push )
{
	if ( !g_ragdollStumbles || !g_ragdollStumbles->integer || !character || !pushDirection || push <= 0.0f )
	{
		return;
	}
	const int entityNumber = character->s.number;
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES )
	{
		return;
	}
	BalanceState &state = balanceStates[entityNumber];
	if ( !( state.stumbling ? CanKeepStumbling( character ) : CanStartStumbling( character ) ) )
	{
		return;
	}

	vec3_t horizontalDirection = { pushDirection[0], pushDirection[1], 0.0f };
	if ( VectorNormalize( horizontalDirection ) < 0.01f )
	{
		return;		// a hit straight down or up does not push anyone sideways
	}
	if ( hitPoint && hitPoint[2] < character->currentOrigin[2] )
	{
		push *= LEG_HIT_BALANCE_MULTIPLE;	// a standing character's origin is at upper thigh height, so this hit is in the legs
	}

	RecoverBalance( state );
	state.lostBalance += push;

	if ( state.lostBalance >= FallOverThreshold() )
	{
		// Pushed over. The fall happens when the character is next drawn, after the game has played
		// its own pain reaction to this hit, so nothing overrides the knockdown animation.
		VectorCopy( horizontalDirection, state.stumbleDirection );
		state.fallRequested = true;
	}
	else if ( state.lostBalance >= StumbleThreshold() )
	{
		StartStumble( character, state, horizontalDirection );
	}
}

balanceOutcome_t UpdateBalance( gentity_t *character )
{
	const int entityNumber = character->s.number;
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES )
	{
		return BALANCE_STEADY;
	}
	BalanceState &state = balanceStates[entityNumber];
	if ( !state.stumbling && !state.fallRequested )
	{
		return BALANCE_STEADY;
	}
	if ( !g_ragdollStumbles->integer || !CanKeepStumbling( character ) )
	{
		state.stumbling = false;
		state.pushPending = false;
		state.fallRequested = false;
		return BALANCE_STEADY;
	}
	if ( state.fallRequested )
	{
		KnockOver( character, state );
		return BALANCE_FALLING;
	}

	if ( level.time - state.stumbleStartTime >= state.stumbleMilliseconds )
	{
		state.stumbling = false;
		return BALANCE_STEADY;
	}

	// Staggered off a ledge: fall over instead of landing on its feet.
	if ( character->client->ps.groundEntityNum == ENTITYNUM_NONE )
	{
		if ( state.airborneSinceTime < 0 )
		{
			state.airborneSinceTime = level.time;
		}
		else if ( level.time - state.airborneSinceTime >= AIRBORNE_MILLISECONDS_BEFORE_FALLING )
		{
			KnockOver( character, state );
			return BALANCE_FALLING;
		}
	}
	else
	{
		state.airborneSinceTime = -1;
	}
	return BALANCE_STUMBLING;
}

void LoseFooting( int entityNumber, const Vector3 &fallDirection )
{
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES )
	{
		return;
	}
	BalanceState &state = balanceStates[entityNumber];
	if ( state.fallRequested )
	{
		return;
	}
	vec3_t horizontalDirection = { fallDirection.x, fallDirection.y, 0.0f };
	if ( VectorNormalize( horizontalDirection ) > 0.01f )
	{
		VectorCopy( horizontalDirection, state.stumbleDirection );
	}
	state.fallRequested = true;

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: misstepped and lost its footing\n", entityNumber );
	}
}

bool TakeStumblePush( int entityNumber, Vector3 &velocityChange )
{
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES || !balanceStates[entityNumber].pushPending )
	{
		return false;
	}
	BalanceState &state = balanceStates[entityNumber];
	state.pushPending = false;
	velocityChange = Vector3( state.pushDirection[0], state.pushDirection[1], 0.0f ) * state.pushSpeed;
	return true;
}

bool StumbleLean( int entityNumber, Vector3 &leanDirection, float &leanRadians )
{
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES || !balanceStates[entityNumber].stumbling )
	{
		return false;
	}
	const BalanceState &state = balanceStates[entityNumber];
	const float elapsedSeconds = ( level.time - state.stumbleStartTime ) / 1000.0f;
	const float progress = Clamp( elapsedSeconds / ( state.stumbleMilliseconds / 1000.0f ), 0.0f, 1.0f );
	const float rise = Clamp( elapsedSeconds / LEAN_RISE_SECONDS, 0.0f, 1.0f );
	const float easeOff = ( 1.0f - progress ) * ( 1.0f - progress );

	leanDirection = Vector3::FromArray( state.stumbleDirection );
	leanRadians = state.leanRadians * rise * easeOff;
	return true;
}

} // namespace ragdoll
