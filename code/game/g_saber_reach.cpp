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

// g_saber_reach.cpp -- the saber arm reaching for the blaster bolts it deflects. See g_saber_reach.h.
//
// Everything is worked out each drawn frame in the character's model space (the space Ghoul2's bone
// frames are read in, x forward, y left, z up, unscaled): the animated arm is read, the blade's
// target is chosen, the arm is solved toward it and blended with the animation, and the result is
// written back as bone overrides on the arm only. The game's own code then reads the hand and blade
// positions from the posed model, so the drawn blade and the blade the game knows (its collision box,
// its trail) follow the arm.

#include "g_local.h"
#include "../cgame/cg_local.h"
#include "g_saber_reach.h"
#include "g_saber_guard.h"
#include "g_ragdoll.h"
#include "g_ragdoll_math.h"
#include "g_ragdoll_rig.h"
#include "wp_saber.h"

#include <math.h>

extern cvar_t *g_spskill;
extern SavedGameJustLoaded_e g_eSavedGameJustLoaded;
extern qboolean InFront( vec3_t spot, vec3_t from, vec3_t fromAngles, float threshHold );
extern bool WP_DoingMoronicForcedAnimationForForcePowers( gentity_t *ent );
extern Vehicle_t *G_IsRidingVehicle( gentity_t *ent );
extern gentity_t *CreateMissile( vec3_t org, vec3_t dir, float vel, int life, gentity_t *owner, qboolean altFire );
extern qboolean PM_InKnockDown( playerState_t *ps );
extern qboolean PM_InRoll( playerState_t *ps );
extern qboolean PM_SaberInAttack( int move );
extern qboolean PM_SaberInStart( int move );
extern qboolean PM_SaberInBrokenParry( int move );
extern qboolean PM_SaberInSpecialAttack( int anim );
extern qboolean PM_SpinningSaberAnim( int anim );
extern qboolean PM_InSaberLock( int anim );
extern qboolean PM_SuperBreakLoseAnim( int anim );
extern qboolean PM_SuperBreakWinAnim( int anim );

namespace
{

using ragdoll::Vector3;
using ragdoll::Quaternion;
using ragdoll::Transform;
using ragdoll::Skeleton;
using ragdoll::Clamp;
using ragdoll::Dot;
using ragdoll::Cross;
using ragdoll::Length;
using ragdoll::Normalized;
using ragdoll::RemoveComponent;
using ragdoll::Rotate;
using ragdoll::InverseRotate;
using ragdoll::ShortestArc;
using ragdoll::Slerp;

cvar_t *g_saberReach;

// Timing.
const float LOOKAHEAD_SECONDS = 0.3f;			// a bolt this close to being deflected is reached for
const float SHORTEST_REACH_SECONDS = 0.09f;		// however late a bolt is seen, the arm takes this long to get there
const float HOLD_SECONDS = 0.08f;				// the blade stays where it met the bolt
const float RETURN_SECONDS = 0.2f;				// then blends back to the animation
const float BUSY_RETURN_SECONDS = 0.1f;			// quicker when an attack (or the like) takes the arm back
const int FORGET_AFTER_MILLISECONDS = 250;		// a character not drawn for this long starts afresh
const int DEFLECTION_POINT_MILLISECONDS = 200;	// how recent a reach must be to place a deflection effect
const float SCAN_RADIUS = 720.0f;				// a player's bolt flies 690 units in LOOKAHEAD_SECONDS
const float TWO_HAND_SHARE_PER_SECOND = 6.0f;	// how quickly the free hand lets go of the hilt, or takes it again

// Where the blade meets the bolt, in model units (the humanoid's arm is about 20 long, a blade 40).
const float NEAREST_MEETING_ON_BLADE = 0.25f;	// share of the blade's length from its base
const float FARTHEST_MEETING_ON_BLADE = 0.8f;
const float MEETING_STRETCH = 48.0f;			// the blade meets the bolt this far at most before the body
const float BODY_CLEARANCE = -6.0f;				// and at most this far inside the body's box (the spark is shown at the blade)

// The body the arm and blade keep clear of, in model units.
const float TORSO_RADIUS = 7.0f;
const float HEAD_RADIUS = 5.5f;
const float ARM_CLEARANCE = 1.5f;				// extra room for the forearm and hilt
const float MEETING_CLEARANCE = 6.0f;			// and for where the blade meets the bolt

const float LONGEST_REACH_SHARE = 0.985f;		// of the arm's full length
const float MAXIMUM_WRIST_TURN_DEGREES = 75.0f;
const float TWO_HANDED_WRIST_DISTANCE = 9.0f;	// animated wrists this close together hold the hilt together

// How the blade's pose is chosen: the lowest score wins (see ScoreBladeDirection).
const float MISS_SCORE_PER_UNIT = 3.0f;
const float ALONG_PATH_SCORE = 3.0f;
const float ALONG_PATH_LIMIT = 0.35f;			// cosine between blade and path beyond which it is heavily penalised
const float ALONG_PATH_EXCESS_SCORE = 15.0f;
const float STYLE_SCORE_PER_DEGREE = 0.05f;
const float WRIST_COMFORT_DEGREES = 35.0f;
const float WRIST_SCORE_PER_DEGREE = 0.08f;
const float COMFORT_SCORE_PER_UNIT = 0.05f;
const float BODY_SCORE_PER_UNIT = 3.0f;
const float BEHIND_SCORE_PER_UNIT = 0.5f;

const int CANDIDATE_DIRECTION_COUNT = 48;

enum
{
	RIGHT_ARM,
	LEFT_ARM,
	ARM_COUNT
};

/*
==================
Skeleton layout
==================
*/

enum armSegment_t
{
	SEGMENT_UPPER_ARM,
	SEGMENT_FOREARM,
	SEGMENT_HAND
};

const int MAXIMUM_ARM_BONES = 32;
const int MAXIMUM_READ_BONES = 2 * MAXIMUM_ARM_BONES + 8;

// One arm: its three joints and every bone it carries (twist helpers, fingers, the weapon tag), each
// following the segment it hangs from. In Jedi Academy's skeleton the fingers and the weapon tag are
// children of the forearm, not of the hand, so they are moved with the hand here.
struct ArmLayout
{
	int shoulderBone;	// rhumerus / lhumerus
	int elbowBone;		// rradius / lradius
	int wristBone;		// rhand / lhand
	int boneCount;
	int bones[MAXIMUM_ARM_BONES];
	armSegment_t segments[MAXIMUM_ARM_BONES];
};

struct SkeletonLayout
{
	const Skeleton *skeleton;
	bool usable;
	ArmLayout arms[ARM_COUNT];
	int chestBone;		// thoracic
	int pelvisBone;
	int neckBone;		// cervical
	int headBone;		// cranium
	int readCount;
	int readBones[MAXIMUM_READ_BONES];
};

const int MAXIMUM_LAYOUTS = 4;
SkeletonLayout layouts[MAXIMUM_LAYOUTS];
int layoutCount = 0;

bool IsInSubtree( const Skeleton &skeleton, int boneIndex, int ancestorIndex )
{
	for ( int guard = 0; boneIndex >= 0 && guard < ragdoll::MAXIMUM_SKELETON_BONES; guard++ )
	{
		if ( boneIndex == ancestorIndex )
		{
			return true;
		}
		boneIndex = skeleton.bones[boneIndex].parentIndex;
	}
	return false;
}

bool BuildArmLayout( const Skeleton &skeleton, const char *shoulderName, const char *elbowName, const char *wristName, ArmLayout &arm )
{
	arm.shoulderBone = skeleton.FindBone( shoulderName );
	arm.elbowBone = skeleton.FindBone( elbowName );
	arm.wristBone = skeleton.FindBone( wristName );
	arm.boneCount = 0;
	if ( arm.shoulderBone < 0 || arm.elbowBone < 0 || arm.wristBone < 0
		|| skeleton.bones[arm.shoulderBone].parentIndex < 0
		|| !IsInSubtree( skeleton, arm.elbowBone, arm.shoulderBone )
		|| !IsInSubtree( skeleton, arm.wristBone, arm.elbowBone ) )
	{
		return false;
	}

	const size_t elbowNameLength = strlen( elbowName );
	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		if ( !IsInSubtree( skeleton, boneIndex, arm.shoulderBone ) )
		{
			continue;
		}
		if ( arm.boneCount >= MAXIMUM_ARM_BONES )
		{
			return false;
		}
		armSegment_t segment = SEGMENT_UPPER_ARM;
		if ( IsInSubtree( skeleton, boneIndex, arm.wristBone ) )
		{
			segment = SEGMENT_HAND;
		}
		else if ( IsInSubtree( skeleton, boneIndex, arm.elbowBone ) )
		{
			// The forearm and its twist helper (rradiusX); the fingers and the weapon tag hang from the
			// forearm but move with the hand.
			segment = !Q_stricmpn( skeleton.bones[boneIndex].name, elbowName, elbowNameLength ) ? SEGMENT_FOREARM : SEGMENT_HAND;
		}
		arm.bones[arm.boneCount] = boneIndex;
		arm.segments[arm.boneCount] = segment;
		arm.boneCount++;
	}
	return true;
}

void AddReadBone( SkeletonLayout &layout, int boneIndex )
{
	for ( int listIndex = 0; listIndex < layout.readCount; listIndex++ )
	{
		if ( layout.readBones[listIndex] == boneIndex )
		{
			return;
		}
	}
	if ( boneIndex >= 0 && layout.readCount < MAXIMUM_READ_BONES )
	{
		layout.readBones[layout.readCount++] = boneIndex;
	}
}

const SkeletonLayout *LayoutForSkeleton( const Skeleton *skeleton )
{
	for ( int layoutIndex = 0; layoutIndex < layoutCount; layoutIndex++ )
	{
		if ( layouts[layoutIndex].skeleton == skeleton )
		{
			return layouts[layoutIndex].usable ? &layouts[layoutIndex] : NULL;
		}
	}
	if ( layoutCount >= MAXIMUM_LAYOUTS )
	{
		return NULL;
	}

	SkeletonLayout &layout = layouts[layoutCount++];
	memset( &layout, 0, sizeof( layout ) );
	layout.skeleton = skeleton;
	layout.chestBone = skeleton->FindBone( "thoracic" );
	layout.pelvisBone = skeleton->FindBone( "pelvis" );
	layout.neckBone = skeleton->FindBone( "cervical" );
	layout.headBone = skeleton->FindBone( "cranium" );
	layout.usable = layout.chestBone >= 0 && layout.pelvisBone >= 0 && layout.neckBone >= 0 && layout.headBone >= 0
		&& BuildArmLayout( *skeleton, "rhumerus", "rradius", "rhand", layout.arms[RIGHT_ARM] )
		&& BuildArmLayout( *skeleton, "lhumerus", "lradius", "lhand", layout.arms[LEFT_ARM] );
	if ( !layout.usable )
	{
		return NULL;
	}

	AddReadBone( layout, layout.chestBone );
	AddReadBone( layout, layout.pelvisBone );
	AddReadBone( layout, layout.neckBone );
	AddReadBone( layout, layout.headBone );
	for ( int armIndex = 0; armIndex < ARM_COUNT; armIndex++ )
	{
		const ArmLayout &arm = layout.arms[armIndex];
		AddReadBone( layout, skeleton->bones[arm.shoulderBone].parentIndex );
		for ( int listIndex = 0; listIndex < arm.boneCount; listIndex++ )
		{
			AddReadBone( layout, arm.bones[listIndex] );
		}
	}
	return &layout;
}

/*
==================
Reach state (one per character, never saved)
==================
*/

// Where the blade is to be: its base (the hilt's emitter) and the way it points, in model space.
struct BladePose
{
	Vector3 base;
	Vector3 direction;
};

struct Reach
{
	bool active;
	int lastDrawTime;

	int missileNumber;		// the bolt being reached for; ENTITYNUM_NONE once deflected or gone
	BladePose from;			// the blade's target when this bolt was taken on (the last bolt's, in a volley)
	float fromWeight;
	BladePose to;			// the blade's target for this bolt
	float progress;			// how far the arm has got from 'from' to 'to', in proportion to time

	int releaseTime;		// when the bolt was deflected or lost: the hold, then the return, start
	float releaseWeight;
	float releaseSeconds;

	BladePose current;		// what was drawn last frame
	float currentWeight;

	Vector3 meetingPoint;	// where the blade meets the bolt, in the world (for the deflection effect)
	int meetingTime;

	bool armPosed[ARM_COUNT];	// overrides written on this arm last frame
	float twoHandShare;			// 1: the free hand holds the hilt, 0: it has let go
	bool checkedAfterLoad;
};

Reach reaches[MAX_GENTITIES];

// The character the reachbolt test command last fired at, for a while.
int reachTestTarget = ENTITYNUM_NONE;
int reachTestUntil = 0;
const char *reachTestLastReason = NULL;
int reachingDefender[MAX_GENTITIES];	// for each missile: the character reaching for it, plus one

bool ShowsDebugText()
{
	return g_saberReach && g_saberReach->integer >= 2;
}

bool HoldsReachedPose()
{
	return g_saberReach && g_saberReach->integer >= 3;
}

int EntityIndex( const gentity_t *entity )
{
	return (int)( entity - g_entities );
}

float ModelScaleOf( const gentity_t *entity )
{
	const float averageScale = ( entity->s.modelScale[0] + entity->s.modelScale[1] + entity->s.modelScale[2] ) / 3.0f;
	return ( averageScale > 0.0f ) ? averageScale : 1.0f;
}

Quaternion QuaternionFromAxis( vec3_t axis[3] )
{
	// axis[c] is the direction the model's c-th basis vector points, so it is column c.
	float rows[3][3];
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			rows[row][column] = axis[column][row];
		}
	}
	return ragdoll::QuaternionFromRotationMatrix( rows );
}

float Smooth( float fraction )
{
	const float clamped = Clamp( fraction, 0.0f, 1.0f );
	return clamped * clamped * ( 3.0f - 2.0f * clamped );
}

float AngleBetweenDegrees( const Vector3 &first, const Vector3 &second )
{
	return ragdoll::RadiansToDegrees( acosf( Clamp( Dot( Normalized( first ), Normalized( second ) ), -1.0f, 1.0f ) ) );
}

Vector3 Lerp( const Vector3 &from, const Vector3 &to, float fraction )
{
	return from + ( to - from ) * fraction;
}

Vector3 SlerpDirection( const Vector3 &from, const Vector3 &to, float fraction )
{
	return Normalized( Rotate( Slerp( Quaternion(), ShortestArc( from, to ), fraction ), from ) );
}

BladePose BlendBladePoses( const BladePose &from, const BladePose &to, float fraction )
{
	BladePose blended;
	blended.base = Lerp( from.base, to.base, fraction );
	blended.direction = SlerpDirection( from.direction, to.direction, fraction );
	return blended;
}

/*
==================
Geometry helpers
==================
*/

Vector3 ClosestPointOnSegment( const Vector3 &point, const Vector3 &start, const Vector3 &end )
{
	const Vector3 segment = end - start;
	const float lengthSquared = ragdoll::LengthSquared( segment );
	if ( lengthSquared < ragdoll::SMALL_NUMBER )
	{
		return start;
	}
	return start + segment * Clamp( Dot( point - start, segment ) / lengthSquared, 0.0f, 1.0f );
}

float DistanceBetweenSegments( const Vector3 &firstStart, const Vector3 &firstEnd, const Vector3 &secondStart, const Vector3 &secondEnd )
{
	// Closest points of two segments (Real-Time Collision Detection, 5.1.9).
	const Vector3 firstDirection = firstEnd - firstStart;
	const Vector3 secondDirection = secondEnd - secondStart;
	const Vector3 between = firstStart - secondStart;
	const float firstLengthSquared = Dot( firstDirection, firstDirection );
	const float secondLengthSquared = Dot( secondDirection, secondDirection );
	const float secondDotBetween = Dot( secondDirection, between );
	float firstFraction = 0.0f;
	float secondFraction = 0.0f;
	if ( firstLengthSquared < ragdoll::SMALL_NUMBER && secondLengthSquared < ragdoll::SMALL_NUMBER )
	{
		return Length( between );
	}
	if ( firstLengthSquared < ragdoll::SMALL_NUMBER )
	{
		secondFraction = Clamp( secondDotBetween / secondLengthSquared, 0.0f, 1.0f );
	}
	else
	{
		const float firstDotBetween = Dot( firstDirection, between );
		if ( secondLengthSquared < ragdoll::SMALL_NUMBER )
		{
			firstFraction = Clamp( -firstDotBetween / firstLengthSquared, 0.0f, 1.0f );
		}
		else
		{
			const float directionsDot = Dot( firstDirection, secondDirection );
			const float denominator = firstLengthSquared * secondLengthSquared - directionsDot * directionsDot;
			firstFraction = ( denominator > ragdoll::SMALL_NUMBER )
				? Clamp( ( directionsDot * secondDotBetween - firstDotBetween * secondLengthSquared ) / denominator, 0.0f, 1.0f )
				: 0.0f;
			secondFraction = ( directionsDot * firstFraction + secondDotBetween ) / secondLengthSquared;
			if ( secondFraction < 0.0f )
			{
				secondFraction = 0.0f;
				firstFraction = Clamp( -firstDotBetween / firstLengthSquared, 0.0f, 1.0f );
			}
			else if ( secondFraction > 1.0f )
			{
				secondFraction = 1.0f;
				firstFraction = Clamp( ( directionsDot - firstDotBetween ) / firstLengthSquared, 0.0f, 1.0f );
			}
		}
	}
	return Length( ( firstStart + firstDirection * firstFraction ) - ( secondStart + secondDirection * secondFraction ) );
}

// How far into a capsule (or a sphere, when its ends are the same point) a segment reaches.
float Penetration( const Vector3 &start, const Vector3 &end, const Vector3 &capsuleStart, const Vector3 &capsuleEnd, float radius )
{
	const float distance = DistanceBetweenSegments( start, end, capsuleStart, capsuleEnd );
	return ( distance < radius ) ? radius - distance : 0.0f;
}

// Where a ray enters a box, as a distance along the ray (0 if it starts inside). False if it misses.
bool RayEntersBox( const vec3_t start, const vec3_t unitDirection, const vec3_t mins, const vec3_t maxs, float &entryDistance )
{
	float nearest = 0.0f;
	float farthest = 1.0e9f;
	for ( int axis = 0; axis < 3; axis++ )
	{
		if ( fabsf( unitDirection[axis] ) < 1.0e-6f )
		{
			if ( start[axis] < mins[axis] || start[axis] > maxs[axis] )
			{
				return false;
			}
			continue;
		}
		const float inverse = 1.0f / unitDirection[axis];
		float first = ( mins[axis] - start[axis] ) * inverse;
		float second = ( maxs[axis] - start[axis] ) * inverse;
		if ( first > second )
		{
			const float swap = first;
			first = second;
			second = swap;
		}
		if ( first > nearest )
		{
			nearest = first;
		}
		if ( second < farthest )
		{
			farthest = second;
		}
		if ( nearest > farthest )
		{
			return false;
		}
	}
	entryDistance = nearest;
	return true;
}

/*
==================
Who reaches, and for which bolt
==================
*/

// The same rules G_MissileImpact applies to a bolt meeting a blade (except its random failures).
bool MissileCanBeReflected( const gentity_t *missile )
{
	if ( ( missile->splashDamage && missile->splashRadius ) || missile->s.weapon == WP_NOGHRI_STICK || missile->s.weapon == WP_SABER )
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

bool PlayerGuardDecides( const gentity_t *entity )
{
	return entity->s.number == 0 && G_SaberGuard_RulesApply();
}

gentity_t *SaberEntityOf( const gentity_t *entity )
{
	const int saberNumber = entity->client->ps.saberEntityNum;
	if ( saberNumber <= 0 || saberNumber >= ENTITYNUM_WORLD || !g_entities[saberNumber].inuse )
	{
		return NULL;
	}
	return &g_entities[saberNumber];
}

// The blade is blocking: its collision box is solid to missiles.
bool BladeIsBlocking( const gentity_t *entity )
{
	const gentity_t *saber = SaberEntityOf( entity );
	return saber && ( saber->contents & CONTENTS_LIGHTSABER );
}

// Whether the character can reach with its saber arm at all this frame. When it cannot,
// reachRefusal says why (for the reachbolt test command).
const char *reachRefusal = "";

bool CanReach( gentity_t *entity )
{
	reachRefusal = "not a living character";
	if ( !g_saberReach || !g_saberReach->integer || !entity->inuse || !entity->client || entity->health <= 0
		|| ( entity->s.eFlags & EF_NODRAW ) )
	{
		return false;
	}
	if ( entity->playerModel < 0 || entity->playerModel >= entity->ghoul2.size()
		|| entity->weaponModel[0] < 0 || entity->weaponModel[0] >= entity->ghoul2.size() )
	{
		return false;
	}
	playerState_t &state = entity->client->ps;
	if ( state.weapon != WP_SABER || state.saberInFlight || state.saber[0].numBlades != 1
		|| state.saber[0].blade[0].length < state.saber[0].blade[0].lengthMax * 0.5f || state.saber[0].blade[0].length <= 1.0f )
	{
		reachRefusal = "no single lit saber in hand";
		return false;
	}
	const int heldFlags = EF_HELD_BY_RANCOR | EF_HELD_BY_WAMPA | EF_HELD_BY_SAND_CREATURE;
	if ( ( state.eFlags & heldFlags ) || state.heldByClient < ENTITYNUM_WORLD || G_IsRidingVehicle( entity ) )
	{
		reachRefusal = "held or riding";
		return false;
	}
	if ( PM_InKnockDown( &state ) || PM_InRoll( &state ) )
	{
		reachRefusal = "knocked down or rolling";
		return false;
	}
	if ( PM_SaberInAttack( state.saberMove ) || PM_SaberInStart( state.saberMove ) || PM_SaberInBrokenParry( state.saberMove )
		|| PM_SaberInSpecialAttack( state.torsoAnim ) || PM_SpinningSaberAnim( state.torsoAnim ) )
	{
		reachRefusal = "attacking";
		return false;
	}
	if ( PM_InSaberLock( state.torsoAnim ) || PM_SuperBreakLoseAnim( state.torsoAnim ) || PM_SuperBreakWinAnim( state.torsoAnim )
		|| state.saberLockTime > level.time )
	{
		reachRefusal = "in a saber lock";
		return false;
	}
	const int handPowers = ( 1 << FP_GRIP ) | ( 1 << FP_LIGHTNING ) | ( 1 << FP_DRAIN );
	if ( ( state.forcePowersActive & handPowers ) || WP_DoingMoronicForcedAnimationForForcePowers( entity ) )
	{
		reachRefusal = "using the Force";
		return false;
	}
	if ( PlayerGuardDecides( entity ) )
	{
		reachRefusal = "guard down";
		return G_SaberGuard_WillDeflect( entity, NULL ) ? true : false;
	}
	reachRefusal = "blade not blocking";
	return state.forcePowerLevel[FP_SABER_DEFENSE] >= FORCE_LEVEL_1 && BladeIsBlocking( entity );
}

// Whether this missile would be deflected by the character's blade (or guard) were it to strike now.
bool WillBeDeflected( gentity_t *entity, gentity_t *missile )
{
	if ( missile->owner == entity )
	{
		return false;
	}
	if ( PlayerGuardDecides( entity ) )
	{
		return G_SaberGuard_WillDeflect( entity, missile ) ? true : false;
	}
	return MissileCanBeReflected( missile )
		&& InFront( missile->currentOrigin, entity->currentOrigin, entity->client->ps.viewangles, SABER_REFLECT_MISSILE_CONE );
}

// An incoming bolt, in the world.
struct IncomingBolt
{
	int missileNumber;
	vec3_t position;			// where it is drawn now
	vec3_t direction;			// unit
	float speed;
	float deflectionDistance;	// how far it flies before it strikes the blade's box or the body
	float bodyDistance;			// how far before it reaches the body's box (-1: it passes by)
};

// The bolt the character's blade will deflect soonest, if one is due within LOOKAHEAD_SECONDS. The
// bolt already reached for keeps the arm unless another is clearly sooner (the blade's box moves with
// the blade, which could otherwise make two close bolts trade places every frame).
const float KEEP_CURRENT_BOLT_SECONDS = 0.03f;

bool FindIncomingBolt( gentity_t *entity, int renderTime, const vec3_t renderOrigin, int currentMissile, IncomingBolt &bolt )
{
	// Boxes are where the game has them, moved to where the character is drawn.
	vec3_t shift, bodyMins, bodyMaxs, saberMins, saberMaxs, centre;
	VectorSubtract( renderOrigin, entity->currentOrigin, shift );
	VectorAdd( entity->absmin, shift, bodyMins );
	VectorAdd( entity->absmax, shift, bodyMaxs );
	bool hasSaberBox = false;
	const gentity_t *saber = SaberEntityOf( entity );
	if ( saber && ( saber->contents & CONTENTS_LIGHTSABER ) && !VectorCompare( saber->mins, saber->maxs ) )
	{
		VectorAdd( saber->currentOrigin, saber->mins, saberMins );
		VectorAdd( saber->currentOrigin, saber->maxs, saberMaxs );
		VectorAdd( saberMins, shift, saberMins );
		VectorAdd( saberMaxs, shift, saberMaxs );
		hasSaberBox = true;
	}
	VectorAdd( bodyMins, bodyMaxs, centre );
	VectorScale( centre, 0.5f, centre );

	vec3_t mins, maxs;
	for ( int axis = 0; axis < 3; axis++ )
	{
		mins[axis] = renderOrigin[axis] - SCAN_RADIUS;
		maxs[axis] = renderOrigin[axis] + SCAN_RADIUS;
	}
	gentity_t *nearby[MAX_GENTITIES];
	const int nearbyCount = gi.EntitiesInBox( mins, maxs, nearby, MAX_GENTITIES );

	bool found = false;
	float soonest = LOOKAHEAD_SECONDS;
	for ( int listIndex = 0; listIndex < nearbyCount; listIndex++ )
	{
		gentity_t *missile = nearby[listIndex];
		if ( !missile || !missile->inuse || missile->s.eType != ET_MISSILE || missile->s.pos.trType != TR_LINEAR
			|| missile->s.weapon == WP_SABER )
		{
			continue;
		}
		const float speed = VectorLength( missile->s.pos.trDelta );
		if ( speed < 100.0f || !WillBeDeflected( entity, missile ) )
		{
			continue;
		}

		vec3_t position, direction, toCentre;
		EvaluateTrajectory( &missile->s.pos, renderTime, position );
		VectorScale( missile->s.pos.trDelta, 1.0f / speed, direction );
		VectorSubtract( centre, position, toCentre );
		if ( DotProduct( toCentre, direction ) <= 0.0f )
		{
			continue;	// flying away (already deflected, perhaps)
		}

		float bodyDistance = -1.0f;
		float saberDistance = -1.0f;
		float entry;
		if ( RayEntersBox( position, direction, bodyMins, bodyMaxs, entry ) )
		{
			bodyDistance = entry;
		}
		if ( hasSaberBox && RayEntersBox( position, direction, saberMins, saberMaxs, entry ) )
		{
			saberDistance = entry;
		}
		if ( bodyDistance < 0.0f && saberDistance < 0.0f )
		{
			continue;	// it will miss
		}
		float deflectionDistance = bodyDistance;
		if ( saberDistance >= 0.0f && ( deflectionDistance < 0.0f || saberDistance < deflectionDistance ) )
		{
			deflectionDistance = saberDistance;
		}
		float seconds = deflectionDistance / speed;
		if ( missile->s.number == currentMissile )
		{
			seconds -= KEEP_CURRENT_BOLT_SECONDS;
		}
		if ( seconds > soonest )
		{
			continue;
		}

		// A bolt the floor or a wall stops first never arrives.
		vec3_t arrival;
		VectorMA( position, deflectionDistance, direction, arrival );
		trace_t trace;
		gi.trace( &trace, position, NULL, NULL, arrival, missile->s.number, MASK_SOLID, G2_NOCOLLIDE, 0 );
		if ( trace.fraction < 0.99f && trace.entityNum != entity->s.number )
		{
			continue;
		}

		soonest = seconds;
		found = true;
		bolt.missileNumber = missile->s.number;
		VectorCopy( position, bolt.position );
		VectorCopy( direction, bolt.direction );
		bolt.speed = speed;
		bolt.deflectionDistance = deflectionDistance;
		bolt.bodyDistance = bodyDistance;
	}
	return found;
}

/*
==================
The pose, read from the animation
==================
*/

struct ArmJoints
{
	Vector3 shoulder;
	Vector3 elbow;
	Vector3 wrist;
	float upperLength;
	float foreLength;
};

struct CharacterPose
{
	const SkeletonLayout *layout;
	Transform frames[ragdoll::MAXIMUM_SKELETON_BONES];
	bool frameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ArmJoints arms[ARM_COUNT];

	Vector3 bladeBase;			// the blade as animated
	Vector3 bladeDirection;
	float bladeLength;

	Vector3 chest;
	Vector3 pelvis;
	Vector3 neck;
	Vector3 headCentre;
	Vector3 forward;			// the torso's facing, which saber stances often turn from the model's
	Vector3 left;
	Vector3 up;

	bool twoHanded;
};

bool ReadPose( gentity_t *entity, const Skeleton &skeleton, const SkeletonLayout &layout, int renderTime, float modelScale, CharacterPose &pose )
{
	pose.layout = &layout;

	// While the renderer smooths animation (r_ghoul2animsmooth), Ghoul2 answers bone bolts from the
	// skeleton as it was last drawn, which still holds last frame's reach. With the smoothing off for
	// the moment, the skeleton rebuilt for these reads is the current animation (blade and hand tags
	// are always read from the current skeleton).
	cvar_t *animationSmoothing = gi.cvar( "r_ghoul2animsmooth", "0.25", 0 );
	const float smoothing = animationSmoothing ? animationSmoothing->value : 0.0f;
	if ( animationSmoothing )
	{
		animationSmoothing->value = 0.0f;
	}
	entity->ghoul2[entity->playerModel].mSkelFrameNum = 0;
	ragdoll::ReadSomeBoneFrames( entity->ghoul2, entity->playerModel, skeleton, renderTime, layout.readBones, layout.readCount,
								 pose.frames, pose.frameIsValid );
	if ( animationSmoothing )
	{
		animationSmoothing->value = smoothing;
	}
	for ( int listIndex = 0; listIndex < layout.readCount; listIndex++ )
	{
		if ( !pose.frameIsValid[layout.readBones[listIndex]] )
		{
			return false;
		}
	}

	for ( int armIndex = 0; armIndex < ARM_COUNT; armIndex++ )
	{
		const ArmLayout &arm = layout.arms[armIndex];
		ArmJoints &joints = pose.arms[armIndex];
		joints.shoulder = pose.frames[arm.shoulderBone].position;
		joints.elbow = pose.frames[arm.elbowBone].position;
		joints.wrist = pose.frames[arm.wristBone].position;
		joints.upperLength = Length( joints.elbow - joints.shoulder );
		joints.foreLength = Length( joints.wrist - joints.elbow );
		if ( joints.upperLength < 1.0f || joints.foreLength < 1.0f )
		{
			return false;
		}
	}

	// The blade, from the saber model's blade tag (as CG_AddSaberBladeGo finds it), in model space.
	CGhoul2Info &saberModel = entity->ghoul2[entity->weaponModel[0]];
	int bladeBolt = gi.G2API_AddBolt( &saberModel, "*blade1" );
	if ( bladeBolt < 0 )
	{
		bladeBolt = gi.G2API_AddBolt( &saberModel, "*flash" );
	}
	if ( bladeBolt < 0 )
	{
		return false;
	}
	mdxaBone_t boltMatrix;
	if ( !gi.G2API_GetBoltMatrix( entity->ghoul2, entity->weaponModel[0], bladeBolt, &boltMatrix, vec3_origin, vec3_origin,
								  renderTime, NULL, vec3_origin ) )
	{
		return false;
	}
	vec3_t boltOrigin, boltForward;
	gi.G2API_GiveMeVectorFromMatrix( boltMatrix, ORIGIN, boltOrigin );
	gi.G2API_GiveMeVectorFromMatrix( boltMatrix, NEGATIVE_X, boltForward );
	pose.bladeBase = Vector3::FromArray( boltOrigin );
	pose.bladeDirection = Normalized( Vector3::FromArray( boltForward ) );
	pose.bladeLength = entity->client->ps.saber[0].blade[0].lengthMax / modelScale;
	if ( Length( pose.bladeDirection ) < 0.5f || pose.bladeLength < 4.0f )
	{
		return false;
	}

	pose.chest = pose.frames[layout.chestBone].position;
	pose.pelvis = pose.frames[layout.pelvisBone].position;
	pose.neck = pose.frames[layout.neckBone].position;
	const Vector3 neckToHead = pose.frames[layout.headBone].position - pose.neck;
	pose.headCentre = pose.frames[layout.headBone].position + Normalized( neckToHead ) * 3.0f;

	pose.left = Normalized( pose.arms[LEFT_ARM].shoulder - pose.arms[RIGHT_ARM].shoulder );
	pose.up = Normalized( RemoveComponent( pose.neck - pose.pelvis, pose.left ) );
	pose.forward = Normalized( Cross( pose.left, pose.up ) );
	if ( Length( pose.left ) < 0.5f || Length( pose.up ) < 0.5f || Length( pose.forward ) < 0.5f )
	{
		return false;
	}

	pose.twoHanded = !entity->client->ps.dualSabers
		&& Length( pose.arms[LEFT_ARM].wrist - pose.arms[RIGHT_ARM].wrist ) < TWO_HANDED_WRIST_DISTANCE;
	return true;
}

/*
==================
Solving the arm
==================
*/

// The rotations that carry an arm from its animated pose to a new one, each about its own joint.
struct ArmTurn
{
	Quaternion upper;		// the upper arm, about the shoulder
	Quaternion fore;		// the forearm, about the elbow (it includes the upper arm's turn)
	Quaternion hand;		// the hand, about the wrist (it includes both)
	Vector3 elbow;			// where the joints end up
	Vector3 wrist;
};

Vector3 DefaultPole( const CharacterPose &pose, int armIndex )
{
	// Elbows hang outward, down and a little back.
	const float outward = ( armIndex == RIGHT_ARM ) ? -0.55f : 0.55f;
	return Normalized( pose.forward * -0.15f + pose.left * outward + pose.up * -0.8f );
}

// Two-bone inverse kinematics: shoulder fixed, wrist to the target (or as near as the arm reaches),
// the elbow toward the pole side. The upper arm twists so the elbow bends in the arm's own plane and
// the forearm only hinges.
void SolveTwoBone( const ArmJoints &joints, const Vector3 &target, const Vector3 &defaultPole, ArmTurn &turn )
{
	const float upper = joints.upperLength;
	const float fore = joints.foreLength;
	Vector3 toTarget = target - joints.shoulder;
	float distance = Length( toTarget );
	const Vector3 reachDirection = ( distance > 0.01f ) ? toTarget / distance : Normalized( joints.wrist - joints.shoulder );
	distance = Clamp( distance, fabsf( upper - fore ) + 0.5f, ( upper + fore ) * 0.999f );

	// The animated elbow's side of the arm, turned with the arm, keeps the animation's style; the
	// default pole keeps it sensible when the arm moves far (a target at the animated wrist gives
	// back the animated arm exactly).
	const Vector3 animatedReach = Normalized( joints.wrist - joints.shoulder );
	const Vector3 animatedSide = Normalized( RemoveComponent( joints.elbow - joints.shoulder, animatedReach ) );
	const Vector3 turnedSide = Rotate( ShortestArc( animatedReach, reachDirection ), animatedSide );
	const float defaultShare = 0.4f * Clamp( Length( target - joints.wrist ) / 12.0f, 0.0f, 1.0f );
	Vector3 pole = Normalized( RemoveComponent( turnedSide * ( 1.0f - defaultShare ) + defaultPole * defaultShare, reachDirection ) );
	if ( Length( pole ) < 0.5f )
	{
		pole = ragdoll::AnyPerpendicular( reachDirection );
	}

	const float shoulderCosine = Clamp( ( upper * upper + distance * distance - fore * fore ) / ( 2.0f * upper * distance ), -1.0f, 1.0f );
	const float shoulderSine = sqrtf( 1.0f - shoulderCosine * shoulderCosine );
	turn.elbow = joints.shoulder + ( reachDirection * shoulderCosine + pole * shoulderSine ) * upper;
	turn.wrist = joints.shoulder + reachDirection * distance;

	// Upper arm: point it at the new elbow, then twist it about itself so the forearm's plane matches.
	const Quaternion swing = ShortestArc( Normalized( joints.elbow - joints.shoulder ), Normalized( turn.elbow - joints.shoulder ) );
	const Vector3 upperAxis = Normalized( turn.elbow - joints.shoulder );
	const Vector3 swungForearm = RemoveComponent( Rotate( swing, joints.wrist - joints.elbow ), upperAxis );
	const Vector3 wantedForearm = RemoveComponent( turn.wrist - turn.elbow, upperAxis );
	Quaternion twist;
	if ( Length( swungForearm ) > 0.05f && Length( wantedForearm ) > 0.05f )
	{
		const Vector3 from = Normalized( swungForearm );
		const Vector3 to = Normalized( wantedForearm );
		twist = ragdoll::QuaternionFromAxisAngle( upperAxis, atan2f( Dot( Cross( from, to ), upperAxis ), Dot( from, to ) ) );
	}
	turn.upper = ragdoll::NormalizedQuaternion( twist * swing );

	// Forearm: the hinge that is left.
	const Vector3 carriedForearm = Rotate( turn.upper, joints.wrist - joints.elbow );
	turn.fore = ragdoll::NormalizedQuaternion( ShortestArc( Normalized( carriedForearm ), Normalized( turn.wrist - turn.elbow ) ) * turn.upper );
	turn.hand = turn.fore;
}

// The saber arm posed so its blade starts at bladeBase and points along bladeDirection, as nearly
// as the arm and the wrist allow.
struct SaberArmSolution
{
	ArmTurn turn;
	Vector3 bladeBase;		// where the blade then is
	Vector3 bladeDirection;
	float wristTurnDegrees;
};

void SolveSaberArm( const CharacterPose &pose, const Vector3 &bladeBase, const Vector3 &bladeDirection, SaberArmSolution &solution )
{
	const ArmJoints &joints = pose.arms[RIGHT_ARM];
	const Vector3 hiltOffset = pose.bladeBase - joints.wrist;	// from the wrist to the blade's base, as animated
	const Vector3 defaultPole = DefaultPole( pose, RIGHT_ARM );

	Quaternion handGuess = ShortestArc( pose.bladeDirection, bladeDirection );
	for ( int iteration = 0; iteration < 2; iteration++ )
	{
		SolveTwoBone( joints, bladeBase - Rotate( handGuess, hiltOffset ), defaultPole, solution.turn );

		// The hand rides the forearm, and the wrist turns the blade the rest of the way (up to a limit).
		const Vector3 carriedBlade = Rotate( solution.turn.fore, pose.bladeDirection );
		Quaternion wristTurn = ShortestArc( carriedBlade, bladeDirection );
		solution.wristTurnDegrees = AngleBetweenDegrees( carriedBlade, bladeDirection );
		if ( solution.wristTurnDegrees > MAXIMUM_WRIST_TURN_DEGREES )
		{
			wristTurn = Slerp( Quaternion(), wristTurn, MAXIMUM_WRIST_TURN_DEGREES / solution.wristTurnDegrees );
		}
		solution.turn.hand = ragdoll::NormalizedQuaternion( wristTurn * solution.turn.fore );
		handGuess = solution.turn.hand;
	}
	solution.bladeBase = solution.turn.wrist + Rotate( solution.turn.hand, hiltOffset );
	solution.bladeDirection = Normalized( Rotate( solution.turn.hand, pose.bladeDirection ) );
}

/*
==================
Choosing where the blade meets the bolt
==================
*/

// The bolt's path in model space, and the stretch of it the blade may meet it on.
struct BoltPath
{
	Vector3 start;
	Vector3 direction;
	float nearest;
	float farthest;
};

Vector3 ComfortableBladeBase( const CharacterPose &pose )
{
	// In front of the stomach, a little to the saber's side: where a ready stance holds the hilt.
	return pose.arms[RIGHT_ARM].shoulder + pose.forward * 11.0f + pose.left * 4.0f - pose.up * 12.0f;
}

// Places a blade pointing along direction so it crosses the path at the point nearest the comfortable
// hilt position. Returns the blade's base, and the meeting point.
void PlaceBlade( const CharacterPose &pose, const BoltPath &path, const Vector3 &direction, Vector3 &bladeBase, Vector3 &meetingPoint )
{
	const Vector3 comfort = ComfortableBladeBase( pose );
	const Vector3 offset = comfort - path.start;
	const float cosine = Dot( direction, path.direction );
	const float nearestOnBlade = pose.bladeLength * NEAREST_MEETING_ON_BLADE;
	const float farthestOnBlade = pose.bladeLength * FARTHEST_MEETING_ON_BLADE;

	// base = start + along * pathDirection - onBlade * direction, as near the comfortable spot as can be.
	const float determinant = 1.0f - cosine * cosine;
	float along = ( determinant > 0.01f ) ? ( Dot( path.direction, offset ) - cosine * Dot( direction, offset ) ) / determinant : Dot( path.direction, offset );
	float onBlade = Clamp( along * cosine - Dot( direction, offset ), nearestOnBlade, farthestOnBlade );
	along = Clamp( Dot( path.direction, offset ) + onBlade * cosine, path.nearest, path.farthest );
	onBlade = Clamp( along * cosine - Dot( direction, offset ), nearestOnBlade, farthestOnBlade );

	meetingPoint = path.start + path.direction * along;
	bladeBase = meetingPoint - direction * onBlade;
}

// The way the blade would rather point to meet a bolt at this point: up for high bolts, down for low
// ones, leaning out toward the bolt's side.
Vector3 PreferredBladeDirection( const CharacterPose &pose, const Vector3 &meetingPoint )
{
	const float height = Dot( meetingPoint - pose.chest, pose.up );
	const float side = Clamp( Dot( meetingPoint - pose.chest, pose.left ) / 10.0f, -1.0f, 1.0f );
	const Vector3 middle = pose.forward * 0.15f + pose.left * ( 0.6f * side ) + pose.up * 0.8f;
	const Vector3 low = pose.forward * 0.25f + pose.left * ( 0.35f * side ) - pose.up * 0.9f;
	const Vector3 high = pose.forward * 0.25f + pose.left * ( 0.35f * side ) + pose.up * 0.9f;
	const float lowShare = Smooth( ( -height - 6.0f ) / 12.0f );
	const float highShare = Smooth( ( height - 4.0f ) / 10.0f );
	return Normalized( middle * ( 1.0f - lowShare - highShare ) + low * lowShare + high * highShare );
}

float ScoreBladeDirection( const CharacterPose &pose, const BoltPath &path, const Vector3 &direction, const Vector3 &preferred )
{
	const float alongPath = fabsf( Dot( direction, path.direction ) );
	if ( alongPath > 0.85f )
	{
		return 1.0e9f;
	}

	Vector3 bladeBase, meetingPoint;
	PlaceBlade( pose, path, direction, bladeBase, meetingPoint );
	SaberArmSolution solution;
	SolveSaberArm( pose, bladeBase, direction, solution );
	const Vector3 bladeTip = solution.bladeBase + solution.bladeDirection * pose.bladeLength;
	const ArmJoints &joints = pose.arms[RIGHT_ARM];

	float score = MISS_SCORE_PER_UNIT * Length( meetingPoint - ClosestPointOnSegment( meetingPoint, solution.bladeBase, bladeTip ) );
	score += ALONG_PATH_SCORE * alongPath;
	if ( alongPath > ALONG_PATH_LIMIT )
	{
		score += ALONG_PATH_EXCESS_SCORE * ( alongPath - ALONG_PATH_LIMIT );
	}
	score += STYLE_SCORE_PER_DEGREE * AngleBetweenDegrees( direction, preferred );
	if ( solution.wristTurnDegrees > WRIST_COMFORT_DEGREES )
	{
		score += WRIST_SCORE_PER_DEGREE * ( solution.wristTurnDegrees - WRIST_COMFORT_DEGREES );
	}
	score += COMFORT_SCORE_PER_UNIT * Length( bladeBase - ComfortableBladeBase( pose ) );

	// Keep the forearm, the hilt and the blade out of the torso and the head, and meet the bolt clear of
	// the body.
	float intrusion = Penetration( meetingPoint, meetingPoint, pose.pelvis, pose.neck, TORSO_RADIUS + MEETING_CLEARANCE );
	intrusion += Penetration( solution.turn.elbow, solution.turn.wrist, pose.pelvis, pose.neck, TORSO_RADIUS + ARM_CLEARANCE );
	intrusion += Penetration( solution.turn.wrist, solution.bladeBase, pose.pelvis, pose.neck, TORSO_RADIUS + ARM_CLEARANCE );
	intrusion += Penetration( solution.bladeBase, bladeTip, pose.pelvis, pose.neck, TORSO_RADIUS );
	intrusion += Penetration( solution.turn.elbow, solution.bladeBase, pose.headCentre, pose.headCentre, HEAD_RADIUS + ARM_CLEARANCE );
	intrusion += Penetration( solution.bladeBase, bladeTip, pose.headCentre, pose.headCentre, HEAD_RADIUS );
	score += BODY_SCORE_PER_UNIT * intrusion;

	// The hand stays in front of the shoulders.
	const float behind = -Dot( solution.turn.wrist - joints.shoulder, pose.forward ) - 2.0f;
	if ( behind > 0.0f )
	{
		score += BEHIND_SCORE_PER_UNIT * behind;
	}
	return score;
}

Vector3 candidateDirections[CANDIDATE_DIRECTION_COUNT];
bool candidateDirectionsReady = false;

void PrepareCandidateDirections()
{
	if ( candidateDirectionsReady )
	{
		return;
	}
	// Spread evenly over the sphere (a Fibonacci lattice).
	const float goldenAngle = ragdoll::PI_RADIANS * ( 3.0f - sqrtf( 5.0f ) );
	for ( int index = 0; index < CANDIDATE_DIRECTION_COUNT; index++ )
	{
		const float z = 1.0f - 2.0f * ( index + 0.5f ) / CANDIDATE_DIRECTION_COUNT;
		const float radius = sqrtf( 1.0f - z * z );
		const float angle = goldenAngle * index;
		candidateDirections[index] = Vector3( radius * cosf( angle ), radius * sinf( angle ), z );
	}
	candidateDirectionsReady = true;
}

// Picks the way the blade points to meet this bolt: the best of directions spread over the sphere,
// then refined by small turns.
Vector3 ChooseBladeDirection( const CharacterPose &pose, const BoltPath &path, float *bestScoreOut )
{
	PrepareCandidateDirections();
	const Vector3 comfort = ComfortableBladeBase( pose );
	const float referenceAlong = Clamp( Dot( comfort - path.start, path.direction ), path.nearest, path.farthest );
	const Vector3 preferred = PreferredBladeDirection( pose, path.start + path.direction * referenceAlong );

	Vector3 best = preferred;
	float bestScore = ScoreBladeDirection( pose, path, preferred, preferred );
	for ( int index = 0; index < CANDIDATE_DIRECTION_COUNT; index++ )
	{
		const float score = ScoreBladeDirection( pose, path, candidateDirections[index], preferred );
		if ( score < bestScore )
		{
			bestScore = score;
			best = candidateDirections[index];
		}
	}

	static const float REFINE_DEGREES[] = { 12.0f, 6.0f, 3.0f };
	for ( size_t round = 0; round < sizeof( REFINE_DEGREES ) / sizeof( REFINE_DEGREES[0] ); round++ )
	{
		const Vector3 firstAxis = ragdoll::AnyPerpendicular( best );
		const Vector3 secondAxis = Cross( best, firstAxis );
		const Vector3 axes[4] = { firstAxis, -firstAxis, secondAxis, -secondAxis };
		const Vector3 centre = best;
		for ( int axisIndex = 0; axisIndex < 4; axisIndex++ )
		{
			const Vector3 trial = Rotate( ragdoll::QuaternionFromAxisAngle( axes[axisIndex], ragdoll::DegreesToRadians( REFINE_DEGREES[round] ) ), centre );
			const float score = ScoreBladeDirection( pose, path, trial, preferred );
			if ( score < bestScore )
			{
				bestScore = score;
				best = trial;
			}
		}
	}
	if ( bestScoreOut )
	{
		*bestScoreOut = bestScore;
	}
	return Normalized( best );
}

/*
==================
Writing the pose
==================
*/

Transform TurnedFrame( const Transform &frame, const Quaternion &turn, const Vector3 &oldPivot, const Vector3 &newPivot )
{
	return Transform( ragdoll::NormalizedQuaternion( turn * frame.rotation ), newPivot + Rotate( turn, frame.position - oldPivot ) );
}

// Moves every bone of the arm with its segment's turn and writes the overrides.
void WriteArm( gentity_t *entity, const CharacterPose &pose, int armIndex, const ArmTurn &turn, int renderTime )
{
	const SkeletonLayout &layout = *pose.layout;
	const ArmLayout &arm = layout.arms[armIndex];
	const ArmJoints &joints = pose.arms[armIndex];

	static Transform posedFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	posedFrames[layout.skeleton->bones[arm.shoulderBone].parentIndex] = pose.frames[layout.skeleton->bones[arm.shoulderBone].parentIndex];
	for ( int listIndex = 0; listIndex < arm.boneCount; listIndex++ )
	{
		const int boneIndex = arm.bones[listIndex];
		const Transform &frame = pose.frames[boneIndex];
		switch ( arm.segments[listIndex] )
		{
		case SEGMENT_UPPER_ARM:
			posedFrames[boneIndex] = TurnedFrame( frame, turn.upper, joints.shoulder, joints.shoulder );
			break;
		case SEGMENT_FOREARM:
			posedFrames[boneIndex] = TurnedFrame( frame, turn.fore, joints.elbow, turn.elbow );
			break;
		default:
			posedFrames[boneIndex] = TurnedFrame( frame, turn.hand, joints.wrist, turn.wrist );
			break;
		}
	}
	ragdoll::WriteSomeBoneFrames( *layout.skeleton, arm.bones, arm.boneCount, posedFrames, &entity->ghoul2[entity->playerModel], renderTime );
}

// The arm's turn, blended weight of the way from the animation.
ArmTurn WeightedTurn( const ArmJoints &joints, const ArmTurn &turn, float weight )
{
	ArmTurn weighted;
	weighted.upper = Slerp( Quaternion(), turn.upper, weight );
	weighted.fore = Slerp( Quaternion(), turn.fore, weight );
	weighted.hand = Slerp( Quaternion(), turn.hand, weight );
	weighted.elbow = joints.shoulder + Rotate( weighted.upper, joints.elbow - joints.shoulder );
	weighted.wrist = weighted.elbow + Rotate( weighted.fore, joints.wrist - joints.elbow );
	return weighted;
}

// The free hand: on the hilt while it can reach it, otherwise hanging at the side.
void PoseFreeHand( gentity_t *entity, const CharacterPose &pose, const ArmTurn &saberHand, float weight, Reach &reach,
				   float frameSeconds, int renderTime )
{
	const ArmJoints &joints = pose.arms[LEFT_ARM];
	const ArmJoints &saberJoints = pose.arms[RIGHT_ARM];
	const float armLength = joints.upperLength + joints.foreLength;

	const Vector3 grip = saberHand.wrist + Rotate( saberHand.hand, joints.wrist - saberJoints.wrist );
	const bool gripInReach = Length( grip - joints.shoulder ) <= armLength * ( LONGEST_REACH_SHARE - 0.01f );
	const float wantedShare = gripInReach ? 1.0f : 0.0f;
	const float step = TWO_HAND_SHARE_PER_SECOND * frameSeconds;
	reach.twoHandShare = ( reach.twoHandShare < wantedShare ) ? Clamp( reach.twoHandShare + step, 0.0f, wantedShare )
															  : Clamp( reach.twoHandShare - step, wantedShare, 1.0f );

	const Vector3 hanging = joints.shoulder + Normalized( pose.forward * 0.25f + pose.left * 0.3f - pose.up * 0.92f ) * ( armLength * 0.93f );
	const Vector3 freeTarget = Lerp( joints.wrist, hanging, weight );
	const Vector3 target = Lerp( freeTarget, grip, reach.twoHandShare );

	ArmTurn turn;
	SolveTwoBone( joints, target, DefaultPole( pose, LEFT_ARM ), turn );
	turn.hand = Slerp( turn.fore, saberHand.hand, reach.twoHandShare );
	WriteArm( entity, pose, LEFT_ARM, turn, renderTime );
	reach.armPosed[LEFT_ARM] = true;
}

void ClearArmOverrides( gentity_t *entity, const SkeletonLayout &layout, Reach &reach )
{
	for ( int armIndex = 0; armIndex < ARM_COUNT; armIndex++ )
	{
		if ( reach.armPosed[armIndex] )
		{
			const ArmLayout &arm = layout.arms[armIndex];
			ragdoll::ClearSomeBoneOverrides( *layout.skeleton, arm.bones, arm.boneCount, &entity->ghoul2[entity->playerModel] );
			reach.armPosed[armIndex] = false;
		}
	}
}

// Back to no reach at all. Whether overrides are still on the arms is kept, so they get removed.
void ForgetReach( Reach &reach )
{
	if ( reach.missileNumber >= 0 && reach.missileNumber < MAX_GENTITIES )
	{
		reachingDefender[reach.missileNumber] = 0;
	}
	const bool checkedAfterLoad = reach.checkedAfterLoad;
	const bool rightArmPosed = reach.armPosed[RIGHT_ARM];
	const bool leftArmPosed = reach.armPosed[LEFT_ARM];
	reach = Reach();		// zeroed (value-initialised)
	reach.missileNumber = ENTITYNUM_NONE;
	reach.twoHandShare = 1.0f;
	reach.checkedAfterLoad = checkedAfterLoad;
	reach.armPosed[RIGHT_ARM] = rightArmPosed;
	reach.armPosed[LEFT_ARM] = leftArmPosed;
}

// The bolt was deflected (or is gone): hold, then return to the animation.
void Release( Reach &reach, int renderTime, float returnSeconds )
{
	if ( reach.missileNumber >= 0 && reach.missileNumber < MAX_GENTITIES )
	{
		reachingDefender[reach.missileNumber] = 0;
	}
	reach.missileNumber = ENTITYNUM_NONE;
	reach.releaseTime = renderTime;
	reach.releaseWeight = reach.currentWeight;
	reach.releaseSeconds = returnSeconds;
	reach.to = reach.current;
}

} // namespace

/*
==================
G_SaberReach_RegisterCvars
==================
*/
void G_SaberReach_RegisterCvars( void )
{
	g_saberReach = gi.cvar( "g_saberReach", "1", CVAR_ARCHIVE );
}

/*
==================
G_SaberReach_Reset
==================
*/
void G_SaberReach_Reset( void )
{
	for ( int entityNumber = 0; entityNumber < MAX_GENTITIES; entityNumber++ )
	{
		reaches[entityNumber] = Reach();
		reaches[entityNumber].missileNumber = ENTITYNUM_NONE;
		reaches[entityNumber].twoHandShare = 1.0f;
		reachingDefender[entityNumber] = 0;
	}
	layoutCount = 0;	// skeletons are reloaded with the level
}

/*
==================
G_SaberReach_UpdateAndPose
==================
*/
void G_SaberReach_UpdateAndPose( gentity_t *entity, int renderTime, const vec3_t renderOrigin, vec3_t renderAxis[3] )
{
	if ( !g_saberReach || !entity || !entity->client )
	{
		return;
	}
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES || entity->playerModel < 0 || entity->playerModel >= entity->ghoul2.size() )
	{
		return;
	}
	Reach &reach = reaches[entityNumber];

	// The ragdoll owns this body (a corpse, a knockdown or a hit reaction): it writes every bone itself,
	// the arms' included.
	if ( G_Ragdoll_OwnsBody( entity ) )
	{
		ForgetReach( reach );
		reach.armPosed[RIGHT_ARM] = reach.armPosed[LEFT_ARM] = false;
		return;
	}

	const int frameMilliseconds = renderTime - reach.lastDrawTime;
	if ( reach.active && ( frameMilliseconds < 0 || frameMilliseconds > FORGET_AFTER_MILLISECONDS ) )
	{
		ForgetReach( reach );		// not drawn for a while: back to the animation at once
	}
	const float frameSeconds = Clamp( frameMilliseconds * 0.001f, 0.0f, 0.1f );
	reach.lastDrawTime = renderTime;

	const bool alive = entity->health > 0;
	const bool canReach = alive && CanReach( entity );
	IncomingBolt bolt;
	const bool hasBolt = canReach && FindIncomingBolt( entity, renderTime, renderOrigin, reach.missileNumber, bolt );

	// The reachbolt test command's target says, while its bolt flies, why it is not reaching.
	if ( entityNumber == reachTestTarget && level.time < reachTestUntil && ShowsDebugText() && !hasBolt && !reach.active )
	{
		const char *why = !canReach ? reachRefusal : "no bolt due to strike it";
		if ( why != reachTestLastReason )
		{
			gi.Printf( "saber reach %d: not reaching: %s\n", entityNumber, why );
			reachTestLastReason = why;
		}
	}
	if ( !hasBolt && !reach.active && !reach.armPosed[RIGHT_ARM] && !reach.armPosed[LEFT_ARM] && ( reach.checkedAfterLoad || !alive ) )
	{
		return;		// the common case: nothing to do
	}

	const Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &entity->ghoul2[entity->playerModel] ) );
	const SkeletonLayout *layout = skeleton ? LayoutForSkeleton( skeleton ) : NULL;
	if ( !layout )
	{
		ForgetReach( reach );
		reach.checkedAfterLoad = true;
		return;
	}

	// Bone overrides are kept in saved games: a character saved mid-reach would come back with its
	// arm frozen, so after loading each living character's arms are cleared the first time it is drawn.
	if ( !reach.checkedAfterLoad && alive )
	{
		reach.checkedAfterLoad = true;
		if ( g_eSavedGameJustLoaded != eNO )
		{
			reach.armPosed[RIGHT_ARM] = reach.armPosed[LEFT_ARM] = true;
		}
	}

	// Read the animation, not last frame's reach. A character that has just died lets go at once.
	ClearArmOverrides( entity, *layout, reach );
	if ( !alive )
	{
		ForgetReach( reach );
		return;
	}
	if ( !hasBolt && !reach.active )
	{
		return;
	}

	const float modelScale = ModelScaleOf( entity );
	const Transform modelToWorld( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) );
	static CharacterPose pose;
	if ( !ReadPose( entity, *skeleton, *layout, renderTime, modelScale, pose ) )
	{
		ForgetReach( reach );
		return;
	}

	// The bolt's path, in model space.
	BoltPath path;
	if ( hasBolt )
	{
		path.start = modelToWorld.InverseTransformPoint( Vector3::FromArray( bolt.position ) ) / modelScale;
		path.direction = Normalized( InverseRotate( modelToWorld.rotation, Vector3::FromArray( bolt.direction ) ) );
		float farthest;
		if ( bolt.bodyDistance >= 0.0f )
		{
			farthest = bolt.bodyDistance / modelScale - BODY_CLEARANCE;
		}
		else
		{
			farthest = Dot( ( pose.chest + pose.pelvis ) * 0.5f - path.start, path.direction );
		}
		path.farthest = ( farthest > 0.0f ) ? farthest : 0.0f;
		path.nearest = ( path.farthest > MEETING_STRETCH ) ? path.farthest - MEETING_STRETCH : 0.0f;
	}

	if ( hasBolt && bolt.missileNumber != reach.missileNumber )
	{
		// A new bolt: choose how the blade meets it, starting from wherever the blade is now.
		float score = 0.0f;
		const Vector3 direction = ChooseBladeDirection( pose, path, &score );
		if ( reach.active && reach.currentWeight > 0.0f )
		{
			reach.from = reach.current;
			reach.fromWeight = reach.currentWeight;
		}
		else
		{
			reach.fromWeight = 0.0f;
		}
		if ( reach.missileNumber >= 0 && reach.missileNumber < MAX_GENTITIES )
		{
			reachingDefender[reach.missileNumber] = 0;
		}
		reach.missileNumber = bolt.missileNumber;
		reachingDefender[bolt.missileNumber] = entityNumber + 1;
		reach.to.direction = direction;
		reach.progress = 0.0f;
		reach.releaseTime = 0;
		reach.active = true;
		if ( ShowsDebugText() )
		{
			const Vector3 &dir = path.direction;
			gi.Printf( "saber reach %d: bolt %d due in %d ms, from (%.0f %.0f %.0f), blade (%.2f %.2f %.2f), score %.1f%s%s\n",
					   entityNumber, bolt.missileNumber, (int)( bolt.deflectionDistance / bolt.speed * 1000.0f ),
					   -dir.x, -dir.y, -dir.z, direction.x, direction.y, direction.z, score,
					   pose.twoHanded ? ", two-handed" : "", reach.fromWeight > 0.0f ? ", carrying on from the last bolt" : "" );
		}
	}
	else if ( !hasBolt && reach.missileNumber != ENTITYNUM_NONE )
	{
		Release( reach, renderTime, canReach ? RETURN_SECONDS : BUSY_RETURN_SECONDS );
		if ( !canReach )
		{
			reach.releaseTime -= (int)( HOLD_SECONDS * 1000.0f );	// an attack (or the like) takes the arm back without a hold
		}
	}
	else if ( !canReach && reach.active && reach.releaseSeconds > BUSY_RETURN_SECONDS && reach.releaseTime )
	{
		// Mid-return, but an attack takes the arm back: return faster from where it is.
		reach.releaseWeight = reach.currentWeight;
		reach.releaseTime = renderTime - (int)( HOLD_SECONDS * 1000.0f );
		reach.releaseSeconds = BUSY_RETURN_SECONDS;
	}

	// This frame's target and weight.
	float weight;
	BladePose target;
	if ( reach.missileNumber != ENTITYNUM_NONE )
	{
		Vector3 meetingPoint;
		PlaceBlade( pose, path, reach.to.direction, reach.to.base, meetingPoint );
		if ( reach.fromWeight <= 0.0f )
		{
			reach.from = reach.to;
		}

		// Cover what is left of the way in the time left before the bolt arrives.
		const float secondsLeft = bolt.deflectionDistance / bolt.speed;
		const float remaining = 1.0f - reach.progress;
		const float timeForRemaining = ( secondsLeft > SHORTEST_REACH_SECONDS * remaining ) ? secondsLeft : SHORTEST_REACH_SECONDS * remaining;
		reach.progress = ( timeForRemaining > 0.0f ) ? Clamp( reach.progress + remaining * frameSeconds / timeForRemaining, 0.0f, 1.0f ) : 1.0f;
		const float eased = Smooth( reach.progress );
		weight = reach.fromWeight + ( 1.0f - reach.fromWeight ) * eased;
		target = BlendBladePoses( reach.from, reach.to, eased );

		const Vector3 worldMeeting = modelToWorld.TransformPoint( meetingPoint * modelScale );
		reach.meetingPoint = worldMeeting;
		reach.meetingTime = renderTime;
	}
	else
	{
		const float sinceRelease = ( renderTime - reach.releaseTime ) * 0.001f;
		target = reach.to;
		if ( HoldsReachedPose() && reach.releaseWeight > 0.0f )
		{
			weight = reach.releaseWeight;
		}
		else if ( sinceRelease < HOLD_SECONDS )
		{
			weight = reach.releaseWeight;
		}
		else
		{
			const float returned = ( reach.releaseSeconds > 0.0f ) ? ( sinceRelease - HOLD_SECONDS ) / reach.releaseSeconds : 1.0f;
			weight = reach.releaseWeight * ( 1.0f - Smooth( returned ) );
		}
	}

	if ( weight <= 0.001f )
	{
		ForgetReach( reach );
		return;
	}

	SaberArmSolution solution;
	SolveSaberArm( pose, target.base, target.direction, solution );
	const ArmTurn weighted = WeightedTurn( pose.arms[RIGHT_ARM], solution.turn, weight );
	WriteArm( entity, pose, RIGHT_ARM, weighted, renderTime );
	reach.armPosed[RIGHT_ARM] = true;
	if ( pose.twoHanded )
	{
		PoseFreeHand( entity, pose, weighted, weight, reach, frameSeconds, renderTime );
	}

	// Have Ghoul2 rebuild the skeleton now (reading any bone does), so the saber model hanging from the
	// hand, which CG_AddSaberBladeGo reads next, is placed from the posed arm and not from the skeleton
	// built for ReadPose.
	{
		static Transform rebuiltFrames[ragdoll::MAXIMUM_SKELETON_BONES];
		static bool rebuiltFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
		const int wristBone = layout->arms[RIGHT_ARM].wristBone;
		ragdoll::ReadSomeBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime, &wristBone, 1, rebuiltFrames, rebuiltFrameIsValid );
	}

	reach.current = target;
	reach.currentWeight = weight;
}

/*
==================
G_SaberReach_DeflectionPoint
==================
*/
void G_SaberReach_DeflectionPoint( const gentity_t *missile, vec3_t point )
{
	if ( !g_saberReach || !g_saberReach->integer || !missile )
	{
		return;
	}
	const int missileNumber = EntityIndex( missile );
	if ( missileNumber < 0 || missileNumber >= MAX_GENTITIES || !reachingDefender[missileNumber] )
	{
		return;
	}
	const Reach &reach = reaches[reachingDefender[missileNumber] - 1];
	if ( !reach.active || reach.missileNumber != missileNumber || cg.time - reach.meetingTime > DEFLECTION_POINT_MILLISECONDS )
	{
		return;
	}
	vec3_t meetingPoint;
	reach.meetingPoint.CopyToArray( meetingPoint );
	if ( Distance( meetingPoint, point ) < 96.0f )
	{
		if ( ShowsDebugText() )
		{
			gi.Printf( "saber reach %d: bolt %d deflected, effect moved %.0f units to the blade\n",
					   reachingDefender[missileNumber] - 1, missileNumber, Distance( meetingPoint, point ) );
		}
		VectorCopy( meetingPoint, point );
	}
}

/*
==================
G_SaberReach_Command
==================
*/
void G_SaberReach_Command( gentity_t *user )
{
	if ( !user || !user->client || gi.argc() < 3 )
	{
		gi.Printf( "usage: reachbolt <right> <up> [distance] [speed] [me]\n" );
		return;
	}
	const float rightOffset = atof( gi.argv( 1 ) );
	const float upOffset = Clamp( atof( gi.argv( 2 ) ), -40.0f, 40.0f );
	const float distance = ( gi.argc() > 3 ) ? atof( gi.argv( 3 ) ) : 300.0f;
	const float speed = ( gi.argc() > 4 ) ? atof( gi.argv( 4 ) ) : 1150.0f;
	const bool atPlayer = ( gi.argc() > 5 ) && !Q_stricmp( gi.argv( 5 ), "me" );

	// The nearest living NPC: the target when nothing is under the crosshair, or the one who fired
	// a bolt at the player.
	gentity_t *nearestNPC = NULL;
	float nearestDistance = 0.0f;
	for ( int entityNumber = 1; entityNumber < ENTITYNUM_WORLD; entityNumber++ )
	{
		gentity_t *candidate = &g_entities[entityNumber];
		if ( !candidate->inuse || !candidate->client || candidate->health <= 0 )
		{
			continue;
		}
		const float candidateDistance = Distance( candidate->currentOrigin, user->currentOrigin );
		if ( !nearestNPC || candidateDistance < nearestDistance )
		{
			nearestNPC = candidate;
			nearestDistance = candidateDistance;
		}
	}

	gentity_t *target = user;
	gentity_t *owner = nearestNPC;
	if ( !atPlayer )
	{
		vec3_t viewForward, traceEnd;
		AngleVectors( user->client->ps.viewangles, viewForward, NULL, NULL );
		VectorMA( user->client->renderInfo.eyePoint, 4096.0f, viewForward, traceEnd );
		trace_t trace;
		gi.trace( &trace, user->client->renderInfo.eyePoint, NULL, NULL, traceEnd, user->s.number, MASK_SHOT, G2_NOCOLLIDE, 0 );
		target = ( trace.entityNum < ENTITYNUM_WORLD && g_entities[trace.entityNum].client && g_entities[trace.entityNum].health > 0 )
			? &g_entities[trace.entityNum] : nearestNPC;
		owner = user;
	}
	if ( !target || !owner )
	{
		gi.Printf( "reachbolt: needs a living NPC nearby\n" );
		return;
	}

	vec3_t facing = { 0.0f, target->client->ps.viewangles[YAW], 0.0f };
	vec3_t forward, right, chest, aim, start, direction;
	AngleVectors( facing, forward, right, NULL );
	VectorCopy( target->currentOrigin, chest );
	chest[2] += 18.0f;
	VectorCopy( chest, aim );
	aim[2] += upOffset;
	VectorMA( aim, distance, forward, start );
	VectorMA( start, rightOffset, right, start );
	VectorSubtract( aim, start, direction );
	VectorNormalize( direction );

	gentity_t *missile = CreateMissile( start, direction, speed, 10000, owner, qfalse );
	missile->classname = "blaster_proj";
	missile->s.weapon = WP_BLASTER;
	missile->damage = 1;
	missile->dflags = DAMAGE_DEATH_KNOCKBACK;
	missile->methodOfDeath = MOD_BLASTER;
	missile->clipmask = MASK_SHOT | CONTENTS_LIGHTSABER;
	missile->bounceCount = 8;
	reachTestTarget = target->s.number;
	reachTestUntil = level.time + (int)( 1000.0f * distance / ( speed > 1.0f ? speed : 1.0f ) ) + 200;
	reachTestLastReason = NULL;
	const bool canReach = CanReach( target );
	gi.Printf( "reachbolt: bolt %d fired at %d (%s)\n", missile->s.number, target->s.number, canReach ? "can reach" : reachRefusal );
}
