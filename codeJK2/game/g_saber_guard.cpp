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

// g_saber_guard.cpp -- the player's lightsaber guard. See g_saber_guard.h.

#include "g_headers.h"
#include "g_local.h"
#include "g_saber_guard.h"
#include "anims.h"
#include "wp_saber.h"

#include <math.h>

extern cvar_t *g_spskill;
extern cvar_t *g_saberAutoBlocking;
extern cvar_t *g_ragdollDebug;
extern void NPC_SetAnim( gentity_t *ent, int setAnimParts, int anim, int setAnimFlags );
extern qboolean PM_SaberInAttack( int move );
extern qboolean PM_SaberInTransitionAny( int move );
extern qboolean PM_SaberInBounce( int move );
extern qboolean InFront( vec3_t spot, vec3_t from, vec3_t fromAngles, float threshHold );
extern void G_ReflectMissile( gentity_t *ent, gentity_t *missile, vec3_t forward );
extern void G_MissileReflectEffect( gentity_t *ent, vec3_t org, vec3_t dir );
extern void WP_SaberBlockNonRandom( gentity_t *self, vec3_t hitloc, qboolean missileBlock );
extern void WP_ForcePowerDrain( gentity_t *self, forcePowers_t forcePower, int overrideAmt );
extern qboolean WP_SaberLose( gentity_t *self, vec3_t throwDir );

static cvar_t *g_saberGuard;

/*
==================
Tuning
==================
*/

// Force power each block costs, by Saber Defense level (0 to 3; level 0 cannot guard). Blocking a
// saber strike costs far more than a bolt, so it pays to block perfectly (free) or to parry.
static const int BOLT_DEFLECT_COST[FORCE_LEVEL_3 + 1] = { 0, 8, 5, 3 };
static const int SABER_BLOCK_COST[FORCE_LEVEL_3 + 1] = { 0, 30, 22, 15 };

// Parrying: raising the guard at most this long before a strike lands.
static const int PARRY_WINDOW_MILLISECONDS = 300;

// How a saber knocked out of a hand flies (units per second), away from whoever knocked it.
static const float DISARM_THROW_SPEED = 250.0f;
static const float DISARM_THROW_LIFT = 200.0f;

// Aiming the guard by turning.
static const float GUARD_TURN_DEGREES = 2.5f;			// turning this far, lately, moves the guard that way
static const float TURN_MEMORY_SECONDS = 0.12f;			// how quickly older turning is forgotten
static const float LARGEST_TURN_PER_COMMAND = 45.0f;	// a bigger jump is a teleport, not a turn

// Where a strike comes from, measured from the eyes.
static const float HIGH_STRIKE_BELOW_EYES = 6.0f;		// strikes landing above eye height minus this come from above
static const float LOW_STRIKE_BELOW_EYES = 30.0f;		// strikes landing below eye height minus this come from below
static const float SIDE_STRIKE_SHARE = 0.4f;			// a strike at least this far round to one side comes from that side

static const int BLOCK_PAYMENT_DEBOUNCE_MILLISECONDS = 300;	// one strike is paid for once, however many frames it meets the guard
static const int GUARD_POSE_HOLD_MILLISECONDS = 200;	// the parry pose is renewed every frame while guarding

/*
==================
State
==================
*/

// The four ways the guard can face.
enum guardDirection_t
{
	GUARD_UP,
	GUARD_DOWN,
	GUARD_LEFT,
	GUARD_RIGHT
};

// The parry held for each guard direction (the character's own left and right).
static const int GUARD_ANIMATIONS[] = { BOTH_P1_S1_T_, BOTH_P1_S1_BR, BOTH_P1_S1_TL, BOTH_P1_S1_TR };
static const char *const GUARD_DIRECTION_NAMES[] = { "high", "low", "left", "right" };
static const char *const STRIKE_SOURCE_NAMES[] = { "above", "below", "the left", "the right" };

// The player's guard. There is only one player.
struct PlayerGuard
{
	bool buttonHeld;				// the guard button is down (and attack is not)
	guardDirection_t direction;
	float recentYawTurn;			// degrees turned lately (left positive), fading
	float recentPitchTurn;			// degrees turned lately (down positive), fading
	bool haveLastCommand;
	int lastCommandAngles[2];		// pitch and yaw of the last command, as sent
	int lastCommandTime;
	int lastPaidAttacker;			// the strike dealt with last: who struck, and when
	int lastPaidTime;
	int heldAnimation;				// the parry being held (-1: none)
	int raisedTime;					// when the guard button was last pressed
	bool parryAttempted;			// a strike has already met this raise of the guard within the parry window
};

static PlayerGuard playerGuard = { false, GUARD_UP, 0.0f, 0.0f, false, { 0, 0 }, 0, ENTITYNUM_NONE, 0, -1, 0, true };

void G_SaberGuard_RegisterCvars( void )
{
	g_saberGuard = gi.cvar( "g_saberGuard", "1", CVAR_ARCHIVE );
}

qboolean G_SaberGuard_RulesApply( void )
{
	return ( g_saberGuard && g_saberGuard->integer ) ? qtrue : qfalse;
}

qboolean G_SaberGuard_OwnsDisarms( const gentity_t *defender )
{
	return ( G_SaberGuard_RulesApply() && defender && defender->s.number == 0 && defender->client ) ? qtrue : qfalse;
}

qboolean G_PlayerBlocksAutomatically( void )
{
	return ( g_saberAutoBlocking && g_saberAutoBlocking->integer && !G_SaberGuard_RulesApply() ) ? qtrue : qfalse;
}

static bool ReportsBlocks( void )
{
	return ( g_saberGuard && g_saberGuard->integer >= 2 ) || ( g_ragdollDebug && g_ragdollDebug->integer );
}

static bool IsThePlayer( const gentity_t *entity )
{
	return entity && entity->s.number == 0 && entity->client;
}

static int SaberDefenseLevel( const gentity_t *player )
{
	const int level = player->client->ps.forcePowerLevel[FP_SABER_DEFENSE];
	return ( level < FORCE_LEVEL_0 ) ? FORCE_LEVEL_0 : ( ( level > FORCE_LEVEL_3 ) ? FORCE_LEVEL_3 : level );
}

// True while the player's guard is up: the button held, the saber lit in hand, the Saber Defense to
// guard with and some Force left.
static bool GuardIsUp( const gentity_t *player )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( player ) || !playerGuard.buttonHeld || player->health <= 0 )
	{
		return false;
	}
	const playerState_t &state = player->client->ps;
	return state.weapon == WP_SABER && state.saberActive && !state.saberInFlight
		&& SaberDefenseLevel( player ) > FORCE_LEVEL_0 && state.forcePower > 0;
}

// Takes cost Force power for a block. Returns false if there is not enough.
static bool PayForBlock( gentity_t *player, int cost )
{
	if ( player->client->ps.forcePower < cost )
	{
		return false;
	}
	if ( cost > 0 )
	{
		WP_ForcePowerDrain( player, FP_SABER_DEFENSE, cost );
	}
	return true;
}

// The side a strike landing at strikePoint comes from, as the player sees it.
static guardDirection_t StrikeDirection( const gentity_t *player, const vec3_t strikePoint )
{
	const vec3_t &eyes = player->client->renderInfo.eyePoint;
	const float height = strikePoint[2] - eyes[2];
	if ( height < -LOW_STRIKE_BELOW_EYES )
	{
		return GUARD_DOWN;
	}
	vec3_t toStrike = { strikePoint[0] - eyes[0], strikePoint[1] - eyes[1], 0.0f };
	VectorNormalize( toStrike );
	vec3_t facing = { 0.0f, player->client->ps.viewangles[YAW], 0.0f };
	vec3_t right;
	AngleVectors( facing, NULL, right, NULL );
	const float side = DotProduct( right, toStrike );
	if ( height > -HIGH_STRIKE_BELOW_EYES && fabsf( side ) < SIDE_STRIKE_SHARE )
	{
		return GUARD_UP;
	}
	return ( side >= 0.0f ) ? GUARD_RIGHT : GUARD_LEFT;
}

// Knocks the lightsaber out of a character's hand (if it is holding one), flying away from whoever
// knocked it.
static void KnockSaberAway( gentity_t *holder, const gentity_t *knocker )
{
	if ( !holder || !holder->client || holder->client->ps.weapon != WP_SABER || holder->client->ps.saberInFlight || !knocker )
	{
		return;
	}
	vec3_t throwVelocity;
	VectorSubtract( holder->currentOrigin, knocker->currentOrigin, throwVelocity );
	throwVelocity[2] = 0.0f;
	VectorNormalize( throwVelocity );
	VectorScale( throwVelocity, DISARM_THROW_SPEED, throwVelocity );
	throwVelocity[2] = DISARM_THROW_LIFT;
	WP_SaberLose( holder, throwVelocity );
}

// Bolts the saber can turn back (the game's own rules: nothing that explodes, and on harder settings
// none of the heavier weapons' shots).
static bool MissileCanBeDeflected( const gentity_t *missile )
{
	if ( missile->splashDamage && missile->splashRadius )
	{
		return false;
	}
	const int weapon = missile->s.weapon;
	if ( g_spskill->integer == 1 )
	{
		return weapon != WP_FLECHETTE && weapon != WP_DEMP2;
	}
	if ( g_spskill->integer >= 2 )
	{
		return weapon != WP_FLECHETTE && weapon != WP_DEMP2 && weapon != WP_BOWCASTER && weapon != WP_REPEATER;
	}
	return true;
}

/*
==================
G_SaberGuard_ReadCommand
==================
*/
void G_SaberGuard_ReadCommand( gentity_t *player, usercmd_t *command )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( player ) || !command )
	{
		return;
	}

	// Aim the guard by turning: the way the view has turned lately, once it has turned far enough.
	if ( playerGuard.haveLastCommand )
	{
		const float yawTurn = AngleNormalize180( SHORT2ANGLE( command->angles[YAW] - playerGuard.lastCommandAngles[1] ) );
		const float pitchTurn = AngleNormalize180( SHORT2ANGLE( command->angles[PITCH] - playerGuard.lastCommandAngles[0] ) );
		const float seconds = ( command->serverTime - playerGuard.lastCommandTime ) / 1000.0f;
		const float keep = ( seconds > 0.0f ) ? expf( -seconds / TURN_MEMORY_SECONDS ) : 1.0f;
		if ( fabsf( yawTurn ) < LARGEST_TURN_PER_COMMAND && fabsf( pitchTurn ) < LARGEST_TURN_PER_COMMAND )
		{
			playerGuard.recentYawTurn = playerGuard.recentYawTurn * keep + yawTurn;
			playerGuard.recentPitchTurn = playerGuard.recentPitchTurn * keep + pitchTurn;
		}
		if ( fabsf( playerGuard.recentYawTurn ) >= GUARD_TURN_DEGREES || fabsf( playerGuard.recentPitchTurn ) >= GUARD_TURN_DEGREES )
		{
			if ( fabsf( playerGuard.recentYawTurn ) >= fabsf( playerGuard.recentPitchTurn ) )
			{
				playerGuard.direction = ( playerGuard.recentYawTurn > 0.0f ) ? GUARD_LEFT : GUARD_RIGHT;
			}
			else
			{
				playerGuard.direction = ( playerGuard.recentPitchTurn < 0.0f ) ? GUARD_UP : GUARD_DOWN;
			}
			playerGuard.recentYawTurn = 0.0f;		// it takes a new turn to move it again
			playerGuard.recentPitchTurn = 0.0f;
		}
	}
	playerGuard.lastCommandAngles[0] = command->angles[PITCH];
	playerGuard.lastCommandAngles[1] = command->angles[YAW];
	playerGuard.lastCommandTime = command->serverTime;
	playerGuard.haveLastCommand = true;

	// With the saber, alt-attack guards and the throw button throws (as alt-attack used to). Every
	// other weapon keeps its alt-fire.
	if ( player->client->ps.weapon == WP_SABER )
	{
		const bool held = ( command->buttons & BUTTON_ALT_ATTACK ) && !( command->buttons & BUTTON_ATTACK );
		if ( held && !playerGuard.buttonHeld )
		{//just raised: a strike landing in the next moments is a parry attempt
			playerGuard.raisedTime = level.time;
			playerGuard.parryAttempted = false;
		}
		playerGuard.buttonHeld = held;
		command->buttons &= ~BUTTON_ALT_ATTACK;
		if ( command->buttons & BUTTON_SABER_THROW )
		{
			command->buttons |= BUTTON_ALT_ATTACK;
		}
	}
	else
	{
		playerGuard.buttonHeld = false;
	}
	command->buttons &= ~BUTTON_SABER_THROW;

	// The game's own blocking window stays open while the guard is up.
	if ( GuardIsUp( player ) )
	{
		player->client->ps.saberBlockingTime = level.time + FRAMETIME;
	}
}

/*
==================
G_SaberGuard_UpdatePose
==================
*/
void G_SaberGuard_UpdatePose( gentity_t *player )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( player ) )
	{
		return;
	}
	playerState_t &state = player->client->ps;
	const bool guarding = GuardIsUp( player );

	// The blade in hand stops other blades and bolts only while it guards or attacks.
	if ( !state.saberInFlight && state.saberEntityNum > 0 && state.saberEntityNum < ENTITYNUM_WORLD )
	{
		const bool bladeIsSolid = guarding || PM_SaberInAttack( state.saberMove ) || PM_SaberInTransitionAny( state.saberMove )
			|| PM_SaberInBounce( state.saberMove );
		g_entities[state.saberEntityNum].contents = bladeIsSolid ? CONTENTS_LIGHTSABER : 0;
	}

	if ( !guarding )
	{
		if ( playerGuard.heldAnimation >= 0 && state.torsoAnim == playerGuard.heldAnimation )
		{
			state.torsoAnimTimer = 0;		// let the saber go back to its usual stance
		}
		playerGuard.heldAnimation = -1;
		return;
	}

	// Hold the parry the guard faces, unless the saber is busy (a block being played, say).
	if ( state.saberMove != LS_READY && state.saberMove != LS_NONE )
	{
		return;
	}
	const int animation = GUARD_ANIMATIONS[playerGuard.direction];
	if ( state.torsoAnim != animation )
	{
		NPC_SetAnim( player, SETANIM_TORSO, animation, SETANIM_FLAG_OVERRIDE | SETANIM_FLAG_HOLD );
	}
	state.torsoAnimTimer = GUARD_POSE_HOLD_MILLISECONDS;
	playerGuard.heldAnimation = animation;
}

/*
==================
G_SaberGuard_PayForParry
==================
*/
qboolean G_SaberGuard_PayForParry( gentity_t *defender, gentity_t *attacker, const vec3_t strikePoint )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( defender ) )
	{
		return qtrue;
	}
	if ( !GuardIsUp( defender ) )
	{
		return qfalse;
	}
	const int attackerNumber = attacker ? attacker->s.number : ENTITYNUM_NONE;
	if ( attackerNumber == playerGuard.lastPaidAttacker && level.time - playerGuard.lastPaidTime < BLOCK_PAYMENT_DEBOUNCE_MILLISECONDS )
	{
		return qtrue;		// the same strike, already paid for
	}

	const guardDirection_t strikeFrom = StrikeDirection( defender, strikePoint );
	const bool facesStrike = ( strikeFrom == playerGuard.direction );

	// The guard raised just before the strike lands: a parry. Facing the strike, it knocks the
	// attacker's saber away; facing the wrong way, the player's own saber is knocked away. Either way
	// the strike itself is stopped.
	if ( !playerGuard.parryAttempted && level.time - playerGuard.raisedTime <= PARRY_WINDOW_MILLISECONDS )
	{
		playerGuard.parryAttempted = true;
		playerGuard.lastPaidAttacker = attackerNumber;
		playerGuard.lastPaidTime = level.time;
		if ( facesStrike )
		{
			KnockSaberAway( attacker, defender );
		}
		else
		{
			KnockSaberAway( defender, attacker );
		}
		if ( ReportsBlocks() )
		{
			if ( facesStrike )
			{
				gi.Printf( "saber guard: parried a strike from %s, the attacker is disarmed\n", STRIKE_SOURCE_NAMES[strikeFrom] );
			}
			else
			{
				gi.Printf( "saber guard: failed parry (guard %s, strike from %s), the player is disarmed\n",
						   GUARD_DIRECTION_NAMES[playerGuard.direction], STRIKE_SOURCE_NAMES[strikeFrom] );
			}
		}
		return qtrue;
	}

	// Otherwise a block: free from the side the guard faces, dear from any other.
	const bool perfect = facesStrike;
	const int cost = perfect ? 0 : SABER_BLOCK_COST[SaberDefenseLevel( defender )];
	if ( !PayForBlock( defender, cost ) )
	{
		if ( ReportsBlocks() )
		{
			gi.Printf( "saber guard: broken by a strike from %s (needed %d Force)\n", STRIKE_SOURCE_NAMES[strikeFrom], cost );
		}
		return qfalse;
	}
	playerGuard.lastPaidAttacker = attackerNumber;
	playerGuard.lastPaidTime = level.time;
	if ( ReportsBlocks() )
	{
		gi.Printf( "saber guard: %s a strike from %s (guard %s, %d Force, %d left)\n", perfect ? "perfectly blocked" : "blocked",
				   STRIKE_SOURCE_NAMES[strikeFrom], GUARD_DIRECTION_NAMES[playerGuard.direction], cost, defender->client->ps.forcePower );
	}
	return qtrue;
}

/*
==================
G_SaberGuard_DeflectMissile
==================
*/
qboolean G_SaberGuard_DeflectMissile( gentity_t *missile, gentity_t **hitEntity, trace_t *trace )
{
	if ( !G_SaberGuard_RulesApply() || !missile || !hitEntity || !*hitEntity )
	{
		return qfalse;
	}

	// Is it the player, or the blade in the player's hand?
	gentity_t *struck = *hitEntity;
	gentity_t *player = NULL;
	if ( IsThePlayer( struck ) )
	{
		player = struck;
	}
	else if ( ( struck->contents & CONTENTS_LIGHTSABER ) && IsThePlayer( struck->owner ) && !struck->owner->client->ps.saberInFlight )
	{
		player = struck->owner;
	}
	if ( !player || missile->owner == player )
	{
		return qfalse;
	}

	if ( GuardIsUp( player ) && MissileCanBeDeflected( missile )
		&& InFront( missile->currentOrigin, player->currentOrigin, player->client->ps.viewangles, SABER_REFLECT_MISSILE_CONE ) )
	{
		const int cost = BOLT_DEFLECT_COST[SaberDefenseLevel( player )];
		if ( PayForBlock( player, cost ) )
		{
			gentity_t *saber = &g_entities[player->client->ps.saberEntityNum];
			vec3_t awayFromPlayer;
			VectorSubtract( missile->currentOrigin, player->currentOrigin, awayFromPlayer );
			VectorNormalize( awayFromPlayer );
			G_ReflectMissile( saber, missile, awayFromPlayer );
			G_MissileReflectEffect( missile, trace->endpos, trace->plane.normal );
			player->client->ps.saberEventFlags |= SEF_DEFLECTED;
			player->client->sess.missionStats.saberBlocksCnt++;
			WP_SaberBlockNonRandom( player, missile->currentOrigin, qtrue );		// a quick parry toward the bolt
			if ( ReportsBlocks() )
			{
				gi.Printf( "saber guard: deflected a bolt (%d Force, %d left)\n", cost, player->client->ps.forcePower );
			}
			return qtrue;
		}
		if ( ReportsBlocks() )
		{
			gi.Printf( "saber guard: too little Force to deflect a bolt (needed %d)\n", cost );
		}
	}

	// Not deflected: a bolt striking the lowered blade goes on into the player.
	*hitEntity = player;
	return qfalse;
}
