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

#include "g_local.h"
#include "../cgame/cg_local.h"
#include "g_saber_guard.h"
#include "anims.h"
#include "wp_saber.h"
#include "g_vehicles.h"

#include <math.h>

extern cvar_t *g_spskill;
extern cvar_t *g_saberAutoBlocking;
extern cvar_t *g_ragdollDebug;
extern qboolean PM_SaberInAttack( int move );
extern qboolean PM_SaberInTransitionAny( int move );
extern qboolean PM_SaberInBounce( int move );
extern qboolean PM_SaberInSpecialAttack( int anim );
extern qboolean PM_SpinningSaberAnim( int anim );
extern qboolean PM_InKnockDown( playerState_t *ps );
extern qboolean PM_SuperBreakLoseAnim( int anim );
extern qboolean PM_SuperBreakWinAnim( int anim );
extern qboolean InFront( vec3_t spot, vec3_t from, vec3_t fromAngles, float threshHold );
extern void G_ReflectMissile( gentity_t *ent, gentity_t *missile, vec3_t forward );
extern void G_MissileReflectEffect( gentity_t *ent, vec3_t org, vec3_t dir );
extern void WP_SaberBlockNonRandom( gentity_t *self, vec3_t hitloc, qboolean missileBlock );
extern void WP_ForcePowerDrain( gentity_t *self, forcePowers_t forcePower, int overrideAmt );
extern qboolean WP_SaberLose( gentity_t *self, vec3_t throwDir );
extern bool WP_DoingMoronicForcedAnimationForForcePowers( gentity_t *ent );
extern Vehicle_t *G_IsRidingVehicle( gentity_t *ent );

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
// It starts this far beyond the edge of its holder's box: a saber lying inside the box would be caught
// straight back (WP_SaberThrow picks up a dropped saber its holder touches).
static const float DISARM_CLEARANCE = 24.0f;

// Aiming the guard by turning.
static const float GUARD_TURN_DEGREES = 2.5f;			// turning this far, lately, moves the guard that way
static const float TURN_MEMORY_SECONDS = 0.12f;			// how quickly older turning is forgotten
static const float LARGEST_TURN_PER_COMMAND = 45.0f;	// a bigger jump is a teleport, not a turn

// Where a strike comes from, measured from the eyes.
static const float HIGH_STRIKE_BELOW_EYES = 6.0f;		// strikes landing above eye height minus this come from above
static const float LOW_STRIKE_BELOW_EYES = 30.0f;		// strikes landing below eye height minus this come from below
static const float SIDE_STRIKE_SHARE = 0.4f;			// a strike at least this far round to one side comes from that side

static const int BLOCK_PAYMENT_DEBOUNCE_MILLISECONDS = 300;	// one strike is paid for once, however many frames it meets the guard
static const int FORCE_METER_FLASH_MILLISECONDS = 1000;	// the Force meter flashes when a block cannot be paid (as for special moves)

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

// The parry held for each guard direction (the character's own left and right), from the single
// saber's set. Dual sabers and staffs have their own sets in the same order (see G_SaberGuard_StanceAnimation).
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
	bool lastPaidWasParry;			// ... and whether it was parried (rather than blocked)
	int gaveWayAttacker;			// the strike the guard last gave way to: who struck, and when
	int gaveWayTime;
	int raisedTime;					// when the guard button was last pressed
	bool parryAttempted;			// a strike has already met this raise of the guard within the parry window
	bool keepsBlockingWindowOpen;	// the game's blocking window (saberBlockingTime) is open for the guard
};

static PlayerGuard playerGuard = { false, GUARD_UP, 0.0f, 0.0f, false, { 0, 0 }, 0, ENTITYNUM_NONE, 0, false, ENTITYNUM_NONE, 0, 0, true, false };

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

// While riding a vehicle alt-attack drives it (a swoop's boost, say), and through binoculars it zooms:
// the guard leaves it alone then.
static bool AltAttackKeepsItsOwnJob( gentity_t *player )
{
	return cg.zoomMode != 0 || G_IsRidingVehicle( player ) != NULL;
}

// The game's own blocking gives up while a Force power keeps the saber hand busy, or while a Force
// power's set animation plays; so does the guard.
static bool ForcePowerBusiesSaberHand( gentity_t *player )
{
	const int handPowers = ( 1 << FP_GRIP ) | ( 1 << FP_LIGHTNING ) | ( 1 << FP_DRAIN );
	return ( player->client->ps.forcePowersActive & handPowers ) != 0 || WP_DoingMoronicForcedAnimationForForcePowers( player );
}

// True while the player's guard is up: the button held, a saber lit in hand, the Saber Defense to
// guard with, some Force left, and the player on its feet with the saber hand free.
static bool GuardIsUp( gentity_t *player )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( player ) || !playerGuard.buttonHeld || player->health <= 0 )
	{
		return false;
	}
	playerState_t &state = player->client->ps;
	return state.weapon == WP_SABER && state.SaberActive() && !state.saberInFlight
		&& SaberDefenseLevel( player ) > FORCE_LEVEL_0 && state.forcePower > 0
		&& !PM_InKnockDown( &state )
		&& !PM_SuperBreakLoseAnim( state.torsoAnim ) && !PM_SuperBreakWinAnim( state.torsoAnim )
		&& !ForcePowerBusiesSaberHand( player );
}

// Takes cost Force power for a block. Returns false if there is not enough (and flashes the Force
// meter, as the game does for a special move the player cannot afford).
static bool PayForBlock( gentity_t *player, int cost )
{
	if ( player->client->ps.forcePower < cost )
	{
		cg.forceHUDTotalFlashTime = level.time + FORCE_METER_FLASH_MILLISECONDS;
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

// Whether a character holds a lightsaber that can be knocked out of its hand. Jedi Academy only ever
// knocks away the right-hand saber, and some sabers are made never to be dropped.
static bool CanBeDisarmed( const gentity_t *holder )
{
	return holder && holder->client && holder->client->ps.weapon == WP_SABER && !holder->client->ps.saberInFlight
		&& holder->client->ps.saberEntityNum > 0 && holder->client->ps.saberEntityNum < ENTITYNUM_WORLD
		&& !( holder->client->ps.saber[0].saberFlags & SFL_NOT_DISARMABLE );
}

// Knocks the lightsaber out of a character's hand (if it can be), flying away from whoever knocked it.
// Returns true if it left the hand.
static bool KnockSaberAway( gentity_t *holder, const gentity_t *knocker )
{
	if ( !CanBeDisarmed( holder ) || !knocker )
	{
		return false;
	}
	vec3_t awayFromKnocker;
	VectorSubtract( holder->currentOrigin, knocker->currentOrigin, awayFromKnocker );
	awayFromKnocker[2] = 0.0f;
	if ( VectorNormalize( awayFromKnocker ) < 1.0f )
	{//right on top of each other: behind the holder
		vec3_t facing = { 0.0f, holder->client->ps.viewangles[YAW], 0.0f };
		AngleVectors( facing, awayFromKnocker, NULL, NULL );
		VectorScale( awayFromKnocker, -1.0f, awayFromKnocker );
	}
	vec3_t throwVelocity;
	VectorScale( awayFromKnocker, DISARM_THROW_SPEED, throwVelocity );
	throwVelocity[2] = DISARM_THROW_LIFT;
	if ( !WP_SaberLose( holder, throwVelocity ) || !holder->client->ps.saberInFlight )
	{
		return false;
	}

	// Start it clear of the holder (as far as walls allow), on the side away from the knocker.
	gentity_t *saber = &g_entities[holder->client->ps.saberEntityNum];
	vec3_t clearPoint;
	VectorMA( holder->currentOrigin, holder->maxs[0] * 1.5f + DISARM_CLEARANCE, awayFromKnocker, clearPoint );
	clearPoint[2] = saber->currentOrigin[2];
	trace_t trace;
	gi.trace( &trace, saber->currentOrigin, saber->mins, saber->maxs, clearPoint, holder->s.number, MASK_SOLID, (EG2_Collision)0, 0 );
	if ( !trace.startsolid && !trace.allsolid )
	{
		G_SetOrigin( saber, trace.endpos );
		saber->s.pos.trType = TR_GRAVITY;
		saber->s.pos.trTime = level.time;
		VectorCopy( throwVelocity, saber->s.pos.trDelta );
		gi.linkentity( saber );
	}
	return true;
}

// Bolts the saber can turn back (the game's own rules: nothing that explodes, no gas bombs, and on
// harder settings none of the heavier weapons' shots).
static bool MissileCanBeDeflected( const gentity_t *missile )
{
	if ( missile->splashDamage && missile->splashRadius )
	{
		return false;
	}
	const int weapon = missile->s.weapon;
	if ( weapon == WP_NOGHRI_STICK )
	{
		return false;
	}
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

// Raising the guard with the saber switched off lights it, as the game's own manual blocking does.
static void LightSaberForGuard( gentity_t *player )
{
	playerState_t &state = player->client->ps;
	if ( state.weapon == WP_SABER && !state.saberInFlight && !state.SaberActive()
		&& !( state.saberEventFlags & SEF_INWATER )
		&& state.weaponstate != WEAPON_DROPPING && state.weaponstate != WEAPON_RAISING
		&& SaberDefenseLevel( player ) > FORCE_LEVEL_0 )
	{
		state.SaberActivate();
	}
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

	// A new level (or a loaded game) starts the command clock again: forget the old view.
	if ( playerGuard.haveLastCommand && command->serverTime < playerGuard.lastCommandTime )
	{
		playerGuard.haveLastCommand = false;
		playerGuard.recentYawTurn = 0.0f;
		playerGuard.recentPitchTurn = 0.0f;
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

	// With the saber, alt-attack guards and the throw button does what alt-attack used to (throw,
	// kata with attack, kick with a staff). Every other weapon keeps its alt-fire.
	if ( player->client->ps.weapon == WP_SABER && !AltAttackKeepsItsOwnJob( player ) )
	{
		const bool held = ( command->buttons & BUTTON_ALT_ATTACK ) && !( command->buttons & BUTTON_ATTACK );
		if ( held && !playerGuard.buttonHeld )
		{//just raised: a strike landing in the next moments is a parry attempt
			playerGuard.raisedTime = level.time;
			playerGuard.parryAttempted = false;
			LightSaberForGuard( player );
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

	// The game's own blocking window stays open while the guard is up, and shuts as soon as it drops
	// (so the guard's stance and blocks end with the button).
	playerState_t &state = player->client->ps;
	if ( GuardIsUp( player ) )
	{
		state.saberBlockingTime = level.time + FRAMETIME;
		playerGuard.keepsBlockingWindowOpen = true;
	}
	else if ( playerGuard.keepsBlockingWindowOpen )
	{
		state.saberBlockingTime = 0;
		playerGuard.keepsBlockingWindowOpen = false;
	}
}

/*
==================
G_SaberGuard_UpdateBlade
==================
*/
void G_SaberGuard_UpdateBlade( gentity_t *player )
{
	if ( !G_SaberGuard_RulesApply() || !IsThePlayer( player ) )
	{
		return;
	}
	const playerState_t &state = player->client->ps;
	if ( state.saberInFlight || state.saberEntityNum <= 0 || state.saberEntityNum >= ENTITYNUM_WORLD )
	{
		return;
	}

	// The blade in hand stops other blades and bolts only while it guards or attacks (including
	// special moves, spins and saber locks). WP_SaberUpdate makes it solid whenever it could block;
	// this takes that back otherwise.
	const bool attacking = PM_SaberInAttack( state.saberMove ) || PM_SaberInTransitionAny( state.saberMove )
		|| PM_SaberInBounce( state.saberMove ) || PM_SaberInSpecialAttack( state.torsoAnim )
		|| PM_SpinningSaberAnim( state.torsoAnim ) || state.saberLockTime > level.time;
	if ( !attacking && !GuardIsUp( player ) )
	{
		g_entities[state.saberEntityNum].contents = 0;
	}
}

/*
==================
G_SaberGuard_StanceAnimation
==================
*/
int G_SaberGuard_StanceAnimation( gentity_t *character, int defaultAnimation )
{
	if ( !IsThePlayer( character ) || !GuardIsUp( character ) )
	{
		return defaultAnimation;
	}
	playerState_t &state = character->client->ps;
	if ( state.saber[0].type == SABER_CLAW )
	{//claws have no parries of their own
		return defaultAnimation;
	}
	// The parry set the game itself plays for this saber style (see PM_SetSaberMove).
	int firstParry = BOTH_P1_S1_T_;
	if ( state.saberAnimLevel == SS_STAFF )
	{
		firstParry = BOTH_P7_S7_T_;
	}
	else if ( state.saberAnimLevel == SS_DUAL || ( state.dualSabers && state.saber[1].Active() ) )
	{
		firstParry = BOTH_P6_S6_T_;
	}
	return firstParry + ( GUARD_ANIMATIONS[playerGuard.direction] - BOTH_P1_S1_T_ );
}

/*
==================
Settling strikes
==================
*/

// Remembers that the guard gave way to a strike, so the strike carries through.
static void RememberGivingWay( const gentity_t *attacker )
{
	playerGuard.gaveWayAttacker = attacker ? attacker->s.number : ENTITYNUM_NONE;
	playerGuard.gaveWayTime = level.time;
}

// Remembers the strike just dealt with, so it is paid for once.
static void RememberPayment( int attackerNumber, bool parried )
{
	playerGuard.lastPaidAttacker = attackerNumber;
	playerGuard.lastPaidTime = level.time;
	playerGuard.lastPaidWasParry = parried;
}

qboolean G_SaberGuard_GaveWay( const gentity_t *defender, const gentity_t *attacker )
{
	return ( G_SaberGuard_RulesApply() && IsThePlayer( defender ) && attacker
		&& playerGuard.gaveWayAttacker == attacker->s.number && playerGuard.gaveWayTime == level.time ) ? qtrue : qfalse;
}

qboolean G_SaberGuard_Parried( const gentity_t *defender, const gentity_t *attacker )
{
	return ( G_SaberGuard_RulesApply() && IsThePlayer( defender ) && attacker && playerGuard.lastPaidWasParry
		&& playerGuard.lastPaidAttacker == attacker->s.number
		&& level.time - playerGuard.lastPaidTime < BLOCK_PAYMENT_DEBOUNCE_MILLISECONDS ) ? qtrue : qfalse;
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
		RememberGivingWay( attacker );
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
	// the strike itself is stopped. A saber that cannot be knocked away makes a failed parry an
	// ordinary block, paid for below.
	if ( !playerGuard.parryAttempted && level.time - playerGuard.raisedTime <= PARRY_WINDOW_MILLISECONDS )
	{
		playerGuard.parryAttempted = true;
		if ( facesStrike || CanBeDisarmed( defender ) )
		{
			RememberPayment( attackerNumber, true );
			if ( facesStrike )
			{
				const bool disarmed = KnockSaberAway( attacker, defender );
				if ( ReportsBlocks() )
				{
					gi.Printf( "saber guard: parried a strike from %s, %s\n", STRIKE_SOURCE_NAMES[strikeFrom],
							   disarmed ? "the attacker is disarmed" : "the attacker keeps its saber" );
				}
			}
			else
			{
				const bool disarmed = KnockSaberAway( defender, attacker );
				if ( ReportsBlocks() )
				{
					gi.Printf( "saber guard: failed parry (guard %s, strike from %s), %s\n",
							   GUARD_DIRECTION_NAMES[playerGuard.direction], STRIKE_SOURCE_NAMES[strikeFrom],
							   disarmed ? "the player is disarmed" : "the player keeps the saber" );
				}
			}
			return qtrue;
		}
	}

	// Otherwise a block: free from the side the guard faces, dear from any other.
	const bool perfect = facesStrike;
	const int cost = perfect ? 0 : SABER_BLOCK_COST[SaberDefenseLevel( defender )];
	if ( !PayForBlock( defender, cost ) )
	{
		RememberGivingWay( attacker );
		if ( ReportsBlocks() )
		{
			gi.Printf( "saber guard: broken by a strike from %s (needed %d Force)\n", STRIKE_SOURCE_NAMES[strikeFrom], cost );
		}
		return qfalse;
	}
	RememberPayment( attackerNumber, false );
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

/*
==================
G_SaberGuard_WillDeflect
==================
*/
qboolean G_SaberGuard_WillDeflect( gentity_t *player, gentity_t *missile )
{
	if ( !GuardIsUp( player ) )
	{
		return qfalse;
	}
	if ( !missile )
	{
		return qtrue;
	}
	if ( missile->owner == player || !MissileCanBeDeflected( missile )
		|| !InFront( missile->currentOrigin, player->currentOrigin, player->client->ps.viewangles, SABER_REFLECT_MISSILE_CONE ) )
	{
		return qfalse;
	}
	return ( player->client->ps.forcePower >= BOLT_DEFLECT_COST[SaberDefenseLevel( player )] ) ? qtrue : qfalse;
}
