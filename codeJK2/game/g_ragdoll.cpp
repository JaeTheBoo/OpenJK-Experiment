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

// g_ragdoll.cpp -- active (muscle driven) death ragdolls for Jedi Outcast.
// See g_ragdoll.h for an overview of the behaviours.

#include "g_headers.h"
#include "g_local.h"
#include "g_ragdoll.h"
#include "g_ragdoll_rig.h"
#include "g_ragdoll_balance.h"
#include "g_slice.h"
#include "wp_saber.h"	// Force power levels

#include <math.h>
#include <string.h>

using ragdoll::Vector3;
using ragdoll::Quaternion;
using ragdoll::Transform;
using ragdoll::RigidBody;
using ragdoll::Joint;
using ragdoll::Simulation;
using ragdoll::SimulationSettings;
using ragdoll::WorldSweepResult;
using ragdoll::Rig;
using ragdoll::Skeleton;
using ragdoll::HUMANOID_BODY_COUNT;

/*
==================
Console variables
==================
*/

cvar_t *g_ragdoll;					// 1 = characters become ragdolls when they die, 0 = canned death animations
cvar_t *g_ragdollMuscleStrength;	// multiplies every muscle; 0 = always limp, 2 = twice as strong
cvar_t *g_ragdollImpulseScale;		// multiplies how hard hits push bodies around
cvar_t *g_ragdollWritheChance;		// chance (0 to 1) that a character moves weakly on the ground before going limp
cvar_t *g_ragdollDebug;				// 1 = print each ragdoll's behaviour changes to the console
cvar_t *g_ragdollKnockdowns;		// 1 = characters knocked down by Force Push or Pull fall as ragdolls and get back up
cvar_t *g_ragdollHitReactions;		// 1 = living characters are physically jolted by hits while they keep fighting
cvar_t *g_ragdollHitReactionScale;	// how hard hits jolt living characters, compared with how hard they push corpses
cvar_t *g_ragdollShotReactions;		// 1 = a hit takes control of an NPC (GTA / Red Dead style): its AI pauses while its
									// body reacts, staggers and recovers; 0 = NPCs only flinch while their AI carries on

/*
==================
Tuning constants
==================
*/

static const int MAXIMUM_RAGDOLLS = 24;

static const float PHYSICS_STEP_SECONDS = 1.0f / 60.0f;
static const int PHYSICS_SUBSTEPS = 8;
static const float LONGEST_FRAME_SECONDS = 0.1f;		// longer frames (hitches, pauses) are shortened to this

static const int DEATH_START_WINDOW_MILLISECONDS = 1000;	// a death older than this starts limp instead of reacting
static const int RECENT_DAMAGE_MILLISECONDS = 500;			// a hit older than this did not cause the death

// The motion of the death animation is carried into the ragdoll by sampling the pose slightly
// earlier and measuring how far every body moved.
static const int VELOCITY_SAMPLE_MILLISECONDS = 50;
static const float MAXIMUM_INHERITED_SPEED = 400.0f;
static const float MAXIMUM_INHERITED_SPIN = 12.0f;		// radians per second
static const float ENTITY_VELOCITY_SHARE = 0.6f;		// how much of the character's movement speed the body keeps

// A limp body falls asleep once none of its parts has moved more than RESTING_MOVEMENT units during
// each REST_CHECK_SECONDS interval for SECONDS_AT_REST_BEFORE_SLEEP. Movement is measured rather than
// speed, because joints and contacts can keep correcting each other in place (shins twisting back and
// forth, for example), which shows up as speed without the body actually going anywhere.
static const float REST_CHECK_SECONDS = 0.5f;
static const float RESTING_MOVEMENT = 1.0f;

// A limp body is damped more and more strongly so it settles instead of jiggling forever from
// small contact fights (such as an arm trapped between the torso and the floor).
static const float SECONDS_LIMP_BEFORE_SETTLING = 1.0f;
static const float SECONDS_TO_FULL_SETTLING = 1.5f;
static const float FULL_SETTLING_DAMPING = 6.0f;		// per second
static const float STOPPED_FALLING_SPEED = 25.0f;	// a falling chest slower than this has landed on something

// Knockdowns: how long a knocked-down character lies stunned, and how getting up works.
static const float KNOCKDOWN_SHORTEST_DAZE_SECONDS = 0.6f;
static const float KNOCKDOWN_LONGEST_DAZE_SECONDS = 1.4f;
static const float KNOCKDOWN_LONGEST_DAZE_BEFORE_GETTING_UP_ANYWAY = 4.0f;	// even if still sliding or rolling
static const float GET_UP_SETTLED_SPEED = 60.0f;		// the body must be moving slower than this to start getting up
static const float GET_UP_BLEND_SECONDS = 0.5f;		// blend from the ragdoll pose into the get-up animation
static const int KNOCKDOWN_START_WINDOW_MILLISECONDS = 1000;	// a knockdown older than this is not turned into a ragdoll

// Hit reactions: the body is fully physical for a moment after the last hit, then fades back to the
// animation. The muscles that pull it back are strongest in the legs (so the character keeps its
// footing) and weakest in the arms (so they fling).
static const float HIT_REACTION_HOLD_SECONDS = 0.2f;
static const float HIT_REACTION_FADE_SECONDS = 0.4f;
static const int HIT_REACTION_START_WINDOW_MILLISECONDS = 300;	// an older hit is not turned into a reaction
static const float HIT_REACTION_SPINE_FREQUENCY = 22.0f;
static const float HIT_REACTION_NECK_FREQUENCY = 18.0f;
static const float HIT_REACTION_ARM_FREQUENCY = 12.0f;
static const float HIT_REACTION_LEG_FREQUENCY = 30.0f;
static const float HIT_REACTION_PELVIS_FREQUENCY = 25.0f;		// how firmly the pelvis follows the animated pelvis
static const float HIT_REACTION_PELVIS_DAMPING = 10.0f;
static const float HIT_REACTION_LONGEST_SPIN = 15.0f;			// radians per second, keeps a big hit from spinning a limb wildly

// Footing and balance while reacting to hits and stumbling (see DriveTowardAnimation).
static const float FOOT_PLANT_FREQUENCY = 30.0f;		// how firmly each foot is held where the animation plants it
static const float FOOT_PLANT_WEIGHT_SHARE = 0.5f;		// each foot's hold is sized for this share of the body's weight
static const float STUMBLE_PELVIS_FREQUENCY = 16.0f;	// the softer hold on the hips while stumbling

// Balance stepping while stumbling, after Euphoria's balancer: the feet step toward the point the
// body is falling toward (the capture point). Distances are for a model of normal size.
static const float STEP_TRIGGER_DISTANCE = 8.0f;		// capture point this far from between the feet: take a step
static const float STEP_SECONDS = 0.26f;				// how long one step takes
static const float STEP_LIFT = 5.0f;					// how high the stepping foot is lifted halfway through
static const float STEP_LONGEST = 34.0f;				// the longest step, measured from the standing foot
static const float MISSTEP_EXTRA_DISTANCE = 14.0f;		// needing a step this much longer than the longest: a misstep
static const float STEP_REACH_SHARE = 0.9f;			// steps land this share of the way to the capture point, so a
														// moving body carries on past the foot and needs another step:
														// the character travels instead of stamping on the spot
static const float STANDING_LEG_REACH = 20.0f;			// a planted foot further than this (horizontally) from the hips is
														// dragged after them, skidding, instead of holding the body back
static const float STAGGER_LEG_FREQUENCY = 4.0f;		// leg muscles while staggering: loose, so the legs swing under the
														// travelling hips instead of braking them
static const float STANCE_HALF_WIDTH = 5.0f;			// steps land this far to the side of the fall line, keeping the feet apart
static const float ALTERNATE_STEP_SHARE = 0.6f;			// the feet take turns; the same foot steps twice only if the other
														// is already less than this share as far from where the body is going
static const float STEPPING_LEG_FREQUENCY = 12.0f;		// leg muscles give while stepping, so the legs reach the placed feet
static const float CENTRE_OF_MASS_VELOCITY_SMOOTHING = 0.3f;	// share of each new measurement, steadies the capture point

// Feet are always put down on real ground. Distances are for a model of normal size.
static const float FOOT_HEIGHT = 2.2f;					// a standing foot's centre above the ground
static const float STEP_UP_LIMIT = 16.0f;				// ground this much higher where a foot would land: the character trips
static const float STEP_DOWN_LIMIT = 20.0f;				// ground this much lower (or none): the character steps into a drop

// Shot reactions that take control of an NPC, in the style of GTA IV/V and Red Dead Redemption
// (see ControlBodyAfterHit).
static const float SHOT_REACTION_IMPULSE_SCALE = 0.35f;		// how hard hits jolt a body that is reacting on its own
static const float SHOT_REACTION_SHORTEST_SECONDS = 0.9f;	// control is kept at least this long after the last hit
static const float RECOVERED_SPEED = 20.0f;				// the body must be moving slower than this (units per second) to recover
static const float LONGEST_CONTROL_SECONDS = 4.0f;		// after this long the character gets its body back regardless
static const int CONTROL_WITHOUT_DRAWING_MILLISECONDS = 500;	// a character out of sight this long gets its body back
static const float STAGGER_BRAKING = 1.2f;				// per second: how quickly the legs slow a staggering body
static const float SETTLE_BRAKING = 3.0f;				// per second: once the stagger is over, the legs plant and stop it
static const float STAGGER_BODY_TILT_DEGREES = 6.0f;	// a staggering body tips the way it is going (the hips lead)
static const float STAGGER_HUNCH_SHARE = 0.4f;			// how much of the stagger's lean becomes a hunch toward the shooter
static const float STAGGER_WANDER_ACCELERATION = 70.0f;	// units per second squared: the legs fighting the body push it about
static const float STAGGER_WANDER_SHORTEST_SECONDS = 0.3f;	// how often that push changes direction
static const float STAGGER_WANDER_LONGEST_SECONDS = 0.6f;
static const float HIP_SUPPORT_FREQUENCY = 18.0f;		// how firmly the legs hold the hips up at standing height
static const float HIP_SUPPORT_HORIZONTAL_SHARE = 0.08f;	// how gently they pull the hips back over the feet
static const float UPRIGHT_PELVIS_FREQUENCY = 25.0f;		// how firmly the hips are kept upright
// Tense muscles: the body takes a hit with a sharp jolt and comes straight back. These have to be
// high because each spine muscle's strength is sized for one segment, but it holds up everything
// above it (the chest, the arms and the head).
static const float CONTROL_SPINE_FREQUENCY = 80.0f;
static const float CONTROL_NECK_FREQUENCY = 50.0f;
static const float CONTROL_ARM_FREQUENCY = 16.0f;

// Flinching toward the wound (see ApplyFlinch), as Euphoria's shot behaviour does: the spine curls
// toward where the hit landed (doubling over a hit in the stomach, arching away from a hit in the
// back), the head dips, the wounded side turns away from the shot and the knees give a little.
static const float FLINCH_RISE_SECONDS = 0.08f;			// how quickly the flinch comes on
static const float FLINCH_HOLD_SECONDS = 0.45f;			// how long after the hit it starts to let go
static const float FLINCH_RELAX_SECONDS = 0.5f;			// how slowly it lets go
static const float FLINCH_FULL_DAMAGE = 16.0f;			// hits this hard flinch fully; weaker ones down to half
static const float TORSO_FLINCH_DEGREES = 28.0f;		// curl toward a wound in the body
static const float LIMB_FLINCH_DEGREES = 14.0f;			// curl toward a wound in an arm, the neck, the head or a leg
static const float ARM_ACROSS_BODY_DISTANCE = 16.0f;	// an arm hit this close to the torso's centre line (an arm held
														// across the chest, holding a rifle) flinches like a body hit
static const float WOUND_TWIST_DEGREES = 12.0f;			// the wounded side turns away from the shot
static const float WOUND_TWIST_FULL_OFFSET = 6.0f;		// a wound this far off the body's centre line twists fully
static const float HIP_DROP_ON_HIT = 3.0f;				// the knees give this much on any hit (units)
static const float HIP_DROP_ON_LEG_HIT = 9.0f;			// and this much when a leg is hit

// A free hand clutches the wound after a hit (see ReachForWound).
static const float CLUTCH_SECONDS = 1.6f;				// how long the hand stays on the wound
static const float CLUTCH_REACH_SECONDS = 0.15f;			// how quickly it gets there
static const float CLUTCH_RELEASE_SECONDS = 0.3f;		// how gently it lets go
static const float CLUTCH_HAND_OFFSET = 2.5f;			// the hand rests this far out from the wound, on the surface
static const float CLUTCH_ARM_FREQUENCY = 30.0f;

// Arms windmill to regain balance while stumbling: raised out to the sides and circling.
static const float WINDMILL_FULL_AT_LEAN_DEGREES = 15.0f;	// leaning this far with the stumble windmills at full strength
static const float WINDMILL_RAISE_DEGREES = 50.0f;		// arms raised this far from hanging straight down (more lifts the weapon overhead)
static const float WINDMILL_CIRCLE_DEGREES = 25.0f;		// the size of the circles
static const float WINDMILL_CIRCLES_PER_SECOND = 2.2f;
// Arm muscles while windmilling. This has to be high: a muscle's strength is sized for the upper arm
// alone turning about its own centre, but the arm pivots at the shoulder and carries the forearm and
// hand, so weaker arms sag under their own weight and trail the circles.
static const float WINDMILL_ARM_FREQUENCY = 80.0f;

// While ragdolled, the knockdown animation's timer is topped up to this every frame. The AI and
// movement code treat the character as knocked down until it runs out, and keeping it short means a
// saved game loaded mid-knockdown still gets up almost at once.
static const int KNOCKDOWN_ANIMATION_HOLD_MILLISECONDS = 500;
static const float SECONDS_AT_REST_BEFORE_SLEEP = 1.0f;
static const float MINIMUM_SECONDS_BEFORE_SLEEP = 1.5f;

// Hits are clamped to this damage range before being turned into an impulse.
static const float SMALLEST_DAMAGE_FOR_IMPULSE = 10.0f;
static const float LARGEST_DAMAGE_FOR_IMPULSE = 150.0f;
static const float FARTHEST_HIT_FROM_BODY = 16.0f;		// hits further than this push the nearest body at its centre

static const char HUMANOID_SKELETON_NAME[] = "models/players/_humanoid/_humanoid";

static const Vector3 WORLD_UP( 0.0f, 0.0f, 1.0f );

/*
==================
Ragdoll state
==================
*/

enum ragdollState_t
{
	RAGDOLL_STAGGERING,
	RAGDOLL_FALLING,
	RAGDOLL_LANDED,
	RAGDOLL_WRITHING,
	RAGDOLL_LIMP,
	RAGDOLL_ASLEEP,
	RAGDOLL_DAZED,			// knocked down but alive: lying stunned for a moment before getting up
	RAGDOLL_GETTING_UP,		// blending from the ragdoll pose into the game's own get-up animation
	RAGDOLL_HIT_REACTION	// alive and still animating: the body is jolted by a hit and pulled back to the animation
};

static const char *RAGDOLL_STATE_NAMES[] = { "staggering", "falling", "landed", "writhing", "limp", "asleep", "dazed", "getting up", "reacting to a hit" };

// What a ragdoll is for, which decides how it starts, behaves and ends.
enum ragdollKind_t
{
	RAGDOLL_KIND_CORPSE,		// a dead body
	RAGDOLL_KIND_KNOCKDOWN,		// a living character knocked over by the Force, who gets back up
	RAGDOLL_KIND_HIT_REACTION	// a living character flinching from a hit without stopping what it is doing
};

static const int HUMANOID_JOINT_COUNT = HUMANOID_BODY_COUNT - 1;

// A slow back-and-forth rotation of one joint, used while writhing.
struct WritheMotion
{
	Vector3 axisInChildSpace;
	float centreAngle;		// radians, the joint leans this way on average
	float swingAngle;		// radians, how far it moves either side of the centre
	float cyclesPerSecond;
	float phase;			// radians
};

struct RagdollInstance
{
	bool inUse;
	int entityNumber;
	int startTime;				// level.time when the ragdoll started, used to recycle the oldest

	ragdollState_t state;
	float secondsInState;
	float secondsSinceStart;
	float secondsAtRest;
	float secondsSinceRestCheck;
	Vector3 restCheckPositions[ragdoll::HUMANOID_BODY_COUNT][ragdoll::MAXIMUM_SPHERES_PER_BODY];	// body part positions at the last rest check

	// Hit reactions: the body follows the live animation through strong muscles, so hits jolt it
	// physically while the character keeps running, aiming and fighting.
	bool isHitReaction;
	float secondsSinceLastHit;						// the physics fades back to the animation as this grows
	bool hasAnimationTargets;						// animationBodyTargets has been read at least once
	Transform animationBodyTargets[HUMANOID_BODY_COUNT];	// where the animation has each body (world space)
	bool hasPreviousPelvisTarget;
	Transform previousPelvisTarget;					// used to carry the body along with the character's movement

	// Balance stepping while stumbling (see StepToKeepBalance). Feet are numbered 0 = right, 1 = left.
	bool isStepping;
	Vector3 plantedFootPositions[2];				// where each foot stands (its body's centre)
	Quaternion footRotationFromPelvis[2];			// each foot's orientation relative to the pelvis when stepping began
	int swingingFoot;								// the foot taking a step, or -1
	int lastSteppedFoot;							// the foot that stepped last, or -1 (the feet take turns)
	float swingSeconds;								// how far into the step
	Vector3 swingStartPosition;
	Vector3 swingTargetPosition;
	bool hasPreviousCentreOfMass;
	Vector3 previousCentreOfMass;
	Vector3 centreOfMassVelocity;					// smoothed, units per second

	// Shot reactions that take control of an NPC (see ControlBodyAfterHit): the AI is paused and the
	// body reacts on its own instead of following the animation, until it has its balance back.
	bool isInControl;
	float secondsInControl;
	int lastDrawnLevelTime;							// control is given back if the character is not drawn for a while
	bool hasReactionPose;
	Quaternion reactionPoseFromPelvis[HUMANOID_BODY_COUNT];	// each body's orientation relative to the hips when the shot landed
	Quaternion reactionPelvisRotation;				// the hips' orientation then (their upright tilt)
	float standingPelvisHeight;						// how high above the feet the hips are held, or -1 until measured
	Vector3 characterOffsetFromPelvis;				// from the hips to the character's origin (horizontal): the character follows the body
	Vector3 controlStartPosition;					// where the hips were when control began (to report how far a stagger went)
	float wanderSecondsLeft;						// until the sideways push of a struggling stagger changes direction
	Vector3 wanderAcceleration;						// that push (units per second squared)

	// Clutching the wound (see ReachForWound).
	int clutchingHand;								// 0 = right, 1 = left, -1 = none
	int woundBody;
	Vector3 woundInBodySpace;						// where on that body the hit landed
	float clutchSecondsLeft;

	// Flinching toward the wound (see ApplyFlinch).
	Vector3 flinchDirection;						// horizontal, from the body toward the wound (toward the shooter for a hit from the front)
	float flinchRadians;							// how far the spine curls toward it at the peak
	float flinchTwistRadians;						// how far the wounded side turns away from the shot (signed, about the vertical)
	float flinchHipDrop;							// how far the knees give at the peak (units, normal-size model)
	float secondsSinceFlinch;

	// Knockdowns: a living character knocked over by the Force, who gets back up afterwards.
	bool isKnockdown;
	int knockdownAnimation;							// the knockdown animation the game chose; kept "playing" so the AI waits
	float dazedSecondsNeeded;						// how long to lie stunned before getting up
	bool getUpRequested;							// the body has settled; start getting up on the next drawn frame
	float getUpSeconds;								// progress of the blend into the get-up animation
	int lastGetUpRenderTime;
	Transform getUpWorldFrames[ragdoll::MAXIMUM_SKELETON_BONES];	// every bone's world frame when getting up began

	// Force powers.
	int gripperEntityNumber;						// who is holding this body up with Force Grip, or -1
	float gripHoldDistance;							// how far in front of the gripper's eyes the body is held
	float gripSecondsHeld;							// used to raise the body smoothly when it is first lifted
	Vector3 gripStartPosition;						// where the chest was when the body was lifted
	float convulsionSecondsLeft;					// Force Lightning twitching still to come
	float secondsUntilNextTwitch;
	Quaternion twitchTargets[HUMANOID_JOINT_COUNT];	// the pose each joint is currently jerking toward

	Simulation simulation;
	Rig rig;

	Transform previousBodyTransforms[HUMANOID_BODY_COUNT];	// for smoothing between physics steps

	float renderYawDegrees;		// the model is always drawn with this yaw while it is a ragdoll
	Vector3 lastServerOrigin;

	// The entity's collision box and contents before it became a ragdoll. While ragdolled the box is
	// stretched around the whole body so shots can reach it; a knocked-down character gets these back
	// when it stands up.
	Vector3 originalMinimum;
	Vector3 originalMaximum;
	int originalContents;

	// Behaviour data.
	Quaternion initialPelvisOrientation;
	Quaternion initialChestOrientation;
	Vector3 chestUpInBodySpace;							// the chest direction that pointed up at death
	Quaternion deathPoseTargets[HUMANOID_JOINT_COUNT];	// joint orientations at the moment of death
	Quaternion groundPoseTargets[HUMANOID_JOINT_COUNT];	// joint orientations when writhing began
	bool writheAllowed;
	float writheDurationSeconds;
	WritheMotion writhe[HUMANOID_JOINT_COUNT];
};

struct DamageRecord
{
	int time;
	vec3_t point;
	vec3_t direction;
	int damage;
	int meansOfDeath;
};

struct DeathRecord
{
	bool pending;
	int time;
	int meansOfDeath;
	int hitLocation;
};

struct KnockdownRecord
{
	bool pending;
	int time;
	int knockdownAnimation;
};

// A hit on a living character, waiting for the character to be drawn so the reaction can start
// from the pose on screen.
struct HitReactionRecord
{
	bool pending;
	int time;
	vec3_t point;
	vec3_t direction;
	int damage;
	int meansOfDeath;
};

static RagdollInstance ragdollInstances[MAXIMUM_RAGDOLLS];

// All ragdolls are stepped together; this is the shared clock (see StepAllRagdolls).
static int lastSceneRenderTime = -1;
static float sceneUnsimulatedSeconds = 0.0f;	// time not yet covered by a fixed physics step

static DamageRecord lastDamage[MAX_GENTITIES];
static DeathRecord pendingDeaths[MAX_GENTITIES];
static KnockdownRecord pendingKnockdowns[MAX_GENTITIES];
static HitReactionRecord pendingHitReactions[MAX_GENTITIES];

// Characters whose leftover bone overrides were cleared after loading a saved game (see
// ClearOverridesLeftInSavedGame).
static bool savedOverridesChecked[MAX_GENTITIES];

// Corpses whose ragdoll was recycled after it came to rest. Their final pose stays on the model,
// so they must not be turned into a ragdoll again.
static bool ragdollFinished[MAX_GENTITIES];

/*
==================
Small helpers
==================
*/

static int EntityIndex( const gentity_t *entity )
{
	return (int)( entity - g_entities );
}

static float ModelScaleOf( const gentity_t *entity )
{
	const float averageScale = ( entity->s.modelScale[0] + entity->s.modelScale[1] + entity->s.modelScale[2] ) / 3.0f;
	return ( averageScale > 0.0f ) ? averageScale : 1.0f;
}

static Quaternion QuaternionFromAxis( vec3_t axis[3] )
{
	// axis[c] is the direction the model's c-th basis vector points, so it is column c.
	float rotationRows[3][3];
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			rotationRows[row][column] = axis[column][row];
		}
	}
	return ragdoll::QuaternionFromRotationMatrix( rotationRows );
}

static Quaternion YawRotation( float yawDegrees )
{
	return ragdoll::QuaternionFromAxisAngle( WORLD_UP, ragdoll::DegreesToRadians( yawDegrees ) );
}

static Vector3 HorizontalPart( const Vector3 &vector )
{
	return Vector3( vector.x, vector.y, 0.0f );
}

// The hips and legs: the part of the body that keeps a living character standing.
static bool IsLowerBody( int bodyIndex )
{
	switch ( bodyIndex )
	{
	case ragdoll::BODY_PELVIS:
	case ragdoll::BODY_RIGHT_THIGH:
	case ragdoll::BODY_RIGHT_SHIN:
	case ragdoll::BODY_RIGHT_FOOT:
	case ragdoll::BODY_LEFT_THIGH:
	case ragdoll::BODY_LEFT_SHIN:
	case ragdoll::BODY_LEFT_FOOT:
		return true;
	default:
		return false;
	}
}

static void SetState( RagdollInstance &instance, ragdollState_t newState )
{
	instance.state = newState;
	instance.secondsInState = 0.0f;
	if ( newState == RAGDOLL_LIMP )
	{
		// Start measuring rest from scratch, so a body that was just woken does not sleep at once.
		instance.secondsAtRest = 0.0f;
		instance.secondsSinceRestCheck = REST_CHECK_SECONDS;	// take a fresh snapshot on the next step
		for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
		{
			for ( int sphereIndex = 0; sphereIndex < ragdoll::MAXIMUM_SPHERES_PER_BODY; sphereIndex++ )
			{
				instance.restCheckPositions[bodyIndex][sphereIndex] = Vector3( 1.0e9f, 1.0e9f, 1.0e9f );
			}
		}
	}
	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		const Vector3 &pelvisPosition = instance.simulation.bodies[ragdoll::BODY_PELVIS].position;
		gi.Printf( "ragdoll %d: %s (pelvis at %.0f %.0f %.0f)\n", instance.entityNumber, RAGDOLL_STATE_NAMES[newState],
				   pelvisPosition.x, pelvisPosition.y, pelvisPosition.z );
	}
}

/*
==================
Console variables and lifetime
==================
*/

void G_Ragdoll_RegisterCvars( void )
{
	g_ragdoll				= gi.cvar( "g_ragdoll", "1", CVAR_ARCHIVE );
	g_ragdollMuscleStrength	= gi.cvar( "g_ragdollMuscleStrength", "1.0", CVAR_ARCHIVE );
	g_ragdollImpulseScale	= gi.cvar( "g_ragdollImpulseScale", "1.0", CVAR_ARCHIVE );
	g_ragdollWritheChance	= gi.cvar( "g_ragdollWritheChance", "0.5", CVAR_ARCHIVE );
	g_ragdollDebug			= gi.cvar( "g_ragdollDebug", "0", 0 );
	g_ragdollKnockdowns		= gi.cvar( "g_ragdollKnockdowns", "1", CVAR_ARCHIVE );
	g_ragdollHitReactions	= gi.cvar( "g_ragdollHitReactions", "1", CVAR_ARCHIVE );
	g_ragdollHitReactionScale = gi.cvar( "g_ragdollHitReactionScale", "0.4", CVAR_ARCHIVE );
	g_ragdollShotReactions	= gi.cvar( "g_ragdollShotReactions", "1", CVAR_ARCHIVE );
	ragdoll::RegisterBalanceCvars();	// g_ragdollStumbles, g_ragdollBalance
	G_Slice_RegisterCvars();			// g_saberSlicing
}

void G_Ragdoll_Reset( void )
{
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		ragdollInstances[instanceIndex].inUse = false;
	}
	memset( lastDamage, 0, sizeof( lastDamage ) );
	memset( pendingDeaths, 0, sizeof( pendingDeaths ) );
	memset( pendingKnockdowns, 0, sizeof( pendingKnockdowns ) );
	memset( pendingHitReactions, 0, sizeof( pendingHitReactions ) );
	memset( savedOverridesChecked, 0, sizeof( savedOverridesChecked ) );
	memset( ragdollFinished, 0, sizeof( ragdollFinished ) );
	lastSceneRenderTime = -1;
	sceneUnsimulatedSeconds = 0.0f;
	ragdoll::ResetBalance();
	G_Slice_Reset();
	ragdoll::ClearSkeletonCache();
}

// True if the ragdoll is being stepped by the physics. Sleeping bodies and bodies that are getting
// up (now driven by the get-up animation) are not.
static bool IsSimulated( const RagdollInstance &instance )
{
	return instance.inUse && instance.state != RAGDOLL_ASLEEP && instance.state != RAGDOLL_GETTING_UP;
}

static RagdollInstance *FindInstance( int entityNumber )
{
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		RagdollInstance &instance = ragdollInstances[instanceIndex];
		if ( instance.inUse && instance.entityNumber == entityNumber )
		{
			return &instance;
		}
	}
	return NULL;
}

// Stops simulating a ragdoll. With giveBackToAnimation the bone overrides are removed so the
// character animates normally again (used if a dead character is brought back to life).
static void ReleaseInstance( RagdollInstance &instance, bool giveBackToAnimation )
{
	if ( giveBackToAnimation )
	{
		gentity_t *entity = &g_entities[instance.entityNumber];
		if ( entity->inuse && entity->playerModel >= 0 && entity->ghoul2.size() > entity->playerModel )
		{
			ragdoll::ClearBoneOverrides( instance.rig, &entity->ghoul2[entity->playerModel] );

			// The standing collision box and contents go back too.
			instance.originalMinimum.CopyToArray( entity->mins );
			instance.originalMaximum.CopyToArray( entity->maxs );
			entity->contents = instance.originalContents;
			gi.linkentity( entity );
		}
	}
	instance.inUse = false;
}

// Finds a free ragdoll slot, recycling the oldest ragdoll that has come to rest if all are taken.
static RagdollInstance *AllocateInstance( void )
{
	RagdollInstance *oldestSleeping = NULL;
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		RagdollInstance &instance = ragdollInstances[instanceIndex];
		if ( !instance.inUse )
		{
			return &instance;
		}
		if ( instance.state == RAGDOLL_ASLEEP && ( !oldestSleeping || instance.startTime < oldestSleeping->startTime ) )
		{
			oldestSleeping = &instance;
		}
	}

	if ( oldestSleeping )
	{
		// The body keeps its final pose on the model; mark it so it is not picked up again.
		ragdollFinished[oldestSleeping->entityNumber] = true;
		ReleaseInstance( *oldestSleeping, false );
	}
	return oldestSleeping;
}

// A failed mission only pretends the player died (G_PlayerGuiltDeath): health 0 and the "what have I
// done?!" kneel, held until the game is restarted. No ragdoll takes the player then.
static bool IsPlayerKneelingAfterFailedMission( gentity_t *entity )
{
	return entity->s.number == 0 && entity->client && entity->health <= 0 && entity->client->ps.legsAnim == BOTH_FORCEHEAL_START;
}

// True for a dead character with the standard humanoid skeleton that can be simulated.
static bool IsHumanoidCorpse( gentity_t *entity )
{
	if ( !entity || !entity->inuse || !entity->client )
	{
		return false;
	}
	if ( entity->health > 0 || entity->client->ps.pm_type != PM_DEAD || IsPlayerKneelingAfterFailedMission( entity ) )
	{
		return false;
	}
	if ( ( entity->flags & FL_DISINTEGRATED ) || ( entity->s.eFlags & EF_NODRAW ) )
	{
		return false;
	}
	if ( entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return false;
	}

	const char *skeletonName = gi.G2API_GetGLAName( &entity->ghoul2[entity->playerModel] );
	return skeletonName && !Q_stricmp( skeletonName, HUMANOID_SKELETON_NAME );
}

/*
==================
Hits
==================
*/

// How hard each kind of damage pushes the body, per point of damage.
static float ImpulsePerDamagePoint( int meansOfDeath, bool &isExplosive )
{
	isExplosive = false;
	switch ( meansOfDeath )
	{
	case MOD_SABER:
		return 30.0f;

	case MOD_BRYAR:
	case MOD_BRYAR_ALT:
	case MOD_BLASTER:
	case MOD_BLASTER_ALT:
	case MOD_DISRUPTOR:
	case MOD_SNIPER:
	case MOD_BOWCASTER:
	case MOD_BOWCASTER_ALT:
	case MOD_REPEATER:
	case MOD_DEMP2:
	case MOD_DEMP2_ALT:
	case MOD_FLECHETTE:
	case MOD_SEEKER:
	case MOD_EMPLACED:
		return 70.0f;

	case MOD_REPEATER_ALT:
	case MOD_FLECHETTE_ALT:
	case MOD_ROCKET:
	case MOD_ROCKET_ALT:
	case MOD_THERMAL:
	case MOD_THERMAL_ALT:
	case MOD_DETPACK:
	case MOD_LASERTRIP:
	case MOD_LASERTRIP_ALT:
	case MOD_EXPLOSIVE:
	case MOD_EXPLOSIVE_SPLASH:
		isExplosive = true;
		return 90.0f;

	case MOD_MELEE:
	case MOD_KNOCKOUT:
		return 60.0f;

	case MOD_FORCE_GRIP:
		return 10.0f;

	case MOD_ELECTROCUTE:
	case MOD_ENERGY:
	case MOD_ENERGY_SPLASH:
		return 15.0f;

	case MOD_WATER:
	case MOD_SLIME:
	case MOD_LAVA:
	case MOD_CRUSH:
	case MOD_FALLING:
	case MOD_SUICIDE:
	case MOD_TRIGGER_HURT:
		return 0.0f;

	default:
		return 40.0f;
	}
}

// The body closest to a world point, and how far the point is from that body's surface.
static int NearestBody( const Simulation &simulation, const Vector3 &point, float &distanceFromSurface )
{
	int nearestBody = ragdoll::BODY_CHEST;
	distanceFromSurface = 1.0e9f;

	for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
	{
		const RigidBody &body = simulation.bodies[bodyIndex];
		const Vector3 segmentStart = body.LocalToWorldPoint( body.spheres[0].localCentre );
		const Vector3 segmentEnd = body.LocalToWorldPoint( body.spheres[1].localCentre );
		const Vector3 segment = segmentEnd - segmentStart;

		float along = 0.0f;
		const float segmentLengthSquared = ragdoll::LengthSquared( segment );
		if ( segmentLengthSquared > ragdoll::SMALL_NUMBER )
		{
			along = ragdoll::Clamp( ragdoll::Dot( point - segmentStart, segment ) / segmentLengthSquared, 0.0f, 1.0f );
		}
		const float distance = ragdoll::Length( point - ( segmentStart + segment * along ) ) - body.spheres[0].radius;
		if ( distance < distanceFromSurface )
		{
			distanceFromSurface = distance;
			nearestBody = bodyIndex;
		}
	}
	return nearestBody;
}

// Remembers where a living character was hit, so a free hand can clutch the wound (see
// ReachForWound). The left hand clutches, since the right one holds the weapon, unless the wound is
// on the left arm itself. Leg wounds are not clutched: a standing character cannot reach them.
static void NoteWound( RagdollInstance &instance, int hitBody, const Vector3 &hitPoint )
{
	int hand;
	switch ( hitBody )
	{
	case ragdoll::BODY_RIGHT_THIGH:
	case ragdoll::BODY_RIGHT_SHIN:
	case ragdoll::BODY_RIGHT_FOOT:
	case ragdoll::BODY_LEFT_THIGH:
	case ragdoll::BODY_LEFT_SHIN:
	case ragdoll::BODY_LEFT_FOOT:
		return;
	case ragdoll::BODY_LEFT_UPPER_ARM:
	case ragdoll::BODY_LEFT_FOREARM:
	case ragdoll::BODY_LEFT_HAND:
		hand = 0;
		break;
	default:
		hand = 1;
		break;
	}
	const RigidBody &body = instance.simulation.bodies[hitBody];
	instance.clutchingHand = hand;
	instance.woundBody = hitBody;
	instance.woundInBodySpace = ragdoll::InverseRotate( body.orientation, hitPoint - body.position );
	instance.clutchSecondsLeft = CLUTCH_SECONDS;
}

// How far a point is from the line through the middle of the torso (from the hips to the chest).
static float DistanceFromTorsoLine( const Simulation &simulation, const Vector3 &point )
{
	const Vector3 &hips = simulation.bodies[ragdoll::BODY_PELVIS].position;
	const Vector3 torso = simulation.bodies[ragdoll::BODY_CHEST].position - hips;
	const float torsoLengthSquared = ragdoll::LengthSquared( torso );
	const float along = ( torsoLengthSquared > ragdoll::SMALL_NUMBER )
		? ragdoll::Clamp( ragdoll::Dot( point - hips, torso ) / torsoLengthSquared, 0.0f, 1.0f )
		: 0.0f;
	return ragdoll::Length( point - ( hips + torso * along ) );
}

// How strongly the flinch toward the wound shows right now (0 to 1): it comes on fast, holds, and
// lets go slowly.
static float FlinchAmount( const RagdollInstance &instance )
{
	const float seconds = instance.secondsSinceFlinch;
	const float rise = ragdoll::Clamp( seconds / FLINCH_RISE_SECONDS, 0.0f, 1.0f );
	const float hold = 1.0f - ragdoll::Clamp( ( seconds - FLINCH_HOLD_SECONDS ) / FLINCH_RELAX_SECONDS, 0.0f, 1.0f );
	return rise * rise * ( 3.0f - 2.0f * rise ) * hold * hold * ( 3.0f - 2.0f * hold );
}

// Starts the flinch toward a new wound (see ApplyFlinch). How far the body curls depends on where and
// how hard the hit landed; a hit in the body curls it most, and a hit in a leg makes the knees give.
// A weaker hit does not cut short a stronger flinch that is still near its peak.
static void StartFlinch( RagdollInstance &instance, int hitBody, const Vector3 &hitPoint, const Vector3 &hitDirection, int damage )
{
	const Simulation &simulation = instance.simulation;
	const RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];

	float curlDegrees = LIMB_FLINCH_DEGREES;
	float twistDegrees = WOUND_TWIST_DEGREES;
	float hipDrop = HIP_DROP_ON_HIT;
	bool armAcrossBody = false;
	switch ( hitBody )
	{
	case ragdoll::BODY_PELVIS:
	case ragdoll::BODY_LOWER_SPINE:
	case ragdoll::BODY_MIDDLE_SPINE:
	case ragdoll::BODY_CHEST:
		curlDegrees = TORSO_FLINCH_DEGREES;
		break;
	case ragdoll::BODY_RIGHT_UPPER_ARM:
	case ragdoll::BODY_RIGHT_FOREARM:
	case ragdoll::BODY_RIGHT_HAND:
	case ragdoll::BODY_LEFT_UPPER_ARM:
	case ragdoll::BODY_LEFT_FOREARM:
	case ragdoll::BODY_LEFT_HAND:
		// An arm held across the chest (holding a rifle) takes the hits aimed at the body, so the body
		// flinches as if it had been hit itself.
		armAcrossBody = DistanceFromTorsoLine( simulation, hitPoint ) < ARM_ACROSS_BODY_DISTANCE * instance.rig.modelScale;
		if ( armAcrossBody )
		{
			curlDegrees = TORSO_FLINCH_DEGREES;
		}
		break;
	case ragdoll::BODY_RIGHT_THIGH:
	case ragdoll::BODY_RIGHT_SHIN:
	case ragdoll::BODY_RIGHT_FOOT:
	case ragdoll::BODY_LEFT_THIGH:
	case ragdoll::BODY_LEFT_SHIN:
	case ragdoll::BODY_LEFT_FOOT:
		twistDegrees = 0.0f;
		hipDrop = HIP_DROP_ON_LEG_HIT;
		break;
	default:
		break;
	}
	const float strength = ragdoll::Clamp( damage / FLINCH_FULL_DAMAGE, 0.5f, 1.0f );
	const float curlRadians = ragdoll::DegreesToRadians( curlDegrees ) * strength;
	if ( FlinchAmount( instance ) * instance.flinchRadians > curlRadians )
	{
		return;
	}

	// Toward the wound is back along the shot. A shot from straight above or below curls the body forward.
	Vector3 towardWound = ragdoll::Normalized( HorizontalPart( hitDirection * -1.0f ) );
	if ( ragdoll::LengthSquared( towardWound ) < ragdoll::SMALL_NUMBER )
	{
		towardWound = ragdoll::Normalized( HorizontalPart(
			ragdoll::Rotate( chest.orientation, instance.rig.forwardAxisInBodySpace[ragdoll::BODY_CHEST] ) ) );
	}

	// The wounded side turns away from the shot: the twist moves the wound along the shot's direction,
	// fully for a wound well off the body's centre line and not at all for one right on it.
	const Vector3 awayFromShot = towardWound * -1.0f;
	const Vector3 woundOffset = HorizontalPart( hitPoint - chest.position );
	const float offCentre = ragdoll::Length( ragdoll::RemoveComponent( woundOffset, awayFromShot ) );
	const float twistShare = ragdoll::Clamp( offCentre / ( WOUND_TWIST_FULL_OFFSET * instance.rig.modelScale ), 0.0f, 1.0f );
	const float twistSign = ( ragdoll::Cross( woundOffset, awayFromShot ).z >= 0.0f ) ? 1.0f : -1.0f;

	instance.flinchDirection = towardWound;
	instance.flinchRadians = curlRadians;
	instance.flinchTwistRadians = ragdoll::DegreesToRadians( twistDegrees ) * strength * twistShare * twistSign;
	instance.flinchHipDrop = hipDrop * strength;
	instance.secondsSinceFlinch = 0.0f;

	if ( g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: flinches %.0f degrees toward the wound%s\n", instance.entityNumber,
				   ragdoll::RadiansToDegrees( curlRadians ), armAcrossBody ? " (arm held across the body)" : "" );
	}
}

// Pushes the body as a hit of this kind and size would. impulseScale shrinks the push for living
// characters reacting to a hit, whose muscles are fighting back.
static void ApplyHitImpulse( RagdollInstance &instance, const vec3_t point, const vec3_t direction, int damage, int meansOfDeath,
							 float impulseScale )
{
	bool isExplosive;
	const float impulsePerDamage = ImpulsePerDamagePoint( meansOfDeath, isExplosive );
	Vector3 pushDirection = ragdoll::Normalized( Vector3::FromArray( direction ) );
	if ( impulsePerDamage <= 0.0f || ragdoll::LengthSquared( pushDirection ) < ragdoll::SMALL_NUMBER )
	{
		return;
	}

	const float clampedDamage = ragdoll::Clamp( (float)damage, SMALLEST_DAMAGE_FOR_IMPULSE, LARGEST_DAMAGE_FOR_IMPULSE );
	const float impulseSize = clampedDamage * impulsePerDamage * g_ragdollImpulseScale->value * impulseScale;
	if ( impulseSize <= 0.0f )
	{
		return;
	}

	Simulation &simulation = instance.simulation;
	float localizedShare = 1.0f;

	if ( isExplosive )
	{
		// Explosions lift and throw the whole body, with only part of the push at the hit point.
		pushDirection = ragdoll::Normalized( pushDirection + WORLD_UP * 0.5f );
		localizedShare = 0.4f;

		float totalMass = 0.0f;
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			totalMass += instance.rig.bodyMass[bodyIndex];
		}
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			const float massShare = instance.rig.bodyMass[bodyIndex] / totalMass;
			simulation.ApplyImpulse( bodyIndex, pushDirection * ( impulseSize * ( 1.0f - localizedShare ) * massShare ),
									 simulation.bodies[bodyIndex].position );
		}
	}

	const Vector3 hitPoint = Vector3::FromArray( point );
	float distanceFromSurface;
	const int hitBody = NearestBody( simulation, hitPoint, distanceFromSurface );
	const Vector3 pushPoint = ( distanceFromSurface > FARTHEST_HIT_FROM_BODY ) ? simulation.bodies[hitBody].position : hitPoint;
	simulation.ApplyImpulse( hitBody, pushDirection * ( impulseSize * localizedShare ), pushPoint );
	if ( instance.isHitReaction )
	{
		NoteWound( instance, hitBody, pushPoint );
		StartFlinch( instance, hitBody, pushPoint, pushDirection, damage );
	}

	if ( g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: hit (means of death %d, damage %d) pushes %s with impulse %.0f%s\n", instance.entityNumber,
				   meansOfDeath, damage, instance.rig.skeleton->bones[instance.rig.bodyBoneIndex[hitBody]].name, impulseSize,
				   isExplosive ? " (explosive)" : "" );
	}

	if ( instance.state == RAGDOLL_ASLEEP )
	{
		SetState( instance, RAGDOLL_LIMP );
	}
	instance.secondsAtRest = 0.0f;
}

// Defined further down, with the other Force power, behaviour and knockdown code.
static void StartConvulsing( RagdollInstance &instance );
static void SetMuscle( RagdollInstance &instance, int jointIndex, const Quaternion &targetRelativeOrientation, float naturalFrequency );
static bool IsKnockdownAnimation( int animation );
static bool IsGetUpAnimation( int animation );

// True if this kind of damage physically pushes living characters. Damage that does not push at all
// is excluded, and so are Force powers that play their own animation on the victim.
static bool PushesLivingCharacters( int meansOfDeath )
{
	bool isExplosive;
	return ImpulsePerDamagePoint( meansOfDeath, isExplosive ) > 0.0f
		&& meansOfDeath != MOD_ELECTROCUTE && meansOfDeath != MOD_FORCE_GRIP;
}

// How much a hit knocks a living character off balance (see g_ragdoll_balance.h). Unlike the push
// on the body, small hits are not rounded up, so the many tiny hits of a saber sliding across a
// character do not add up to a stumble.
static float BalancePushOfHit( int damage, int meansOfDeath )
{
	bool isExplosive;
	const float countedDamage = ragdoll::Clamp( (float)damage, 0.0f, LARGEST_DAMAGE_FOR_IMPULSE );
	return countedDamage * ImpulsePerDamagePoint( meansOfDeath, isExplosive ) * g_ragdollImpulseScale->value;
}

// True if a hit of this kind on this living character should jolt it physically. Characters that
// are knocked down, getting up, being gripped or manning a mounted gun are left to their animation.
static bool WantsHitReaction( const gentity_t *target, int meansOfDeath )
{
	if ( !g_ragdollHitReactions->integer || !target->client || target->health <= 0 )
	{
		return false;
	}
	if ( !PushesLivingCharacters( meansOfDeath ) )
	{
		return false;
	}
	const playerState_t &state = target->client->ps;
	return !( state.eFlags & EF_FORCE_GRIPPED )
		&& !( target->s.eFlags & EF_LOCKED_TO_WEAPON )
		&& !IsKnockdownAnimation( state.legsAnim )
		&& !IsGetUpAnimation( state.legsAnim );
}

// True if a hit can take control of this living character (see ControlBodyAfterHit): NPCs standing
// on the ground. Never the player, whose movement is never taken over, nor bosses or saber users,
// whose duels rely on their own animations, nor characters already taken over by something else.
static bool CanTakeControlOf( const gentity_t *entity )
{
	if ( !g_ragdollShotReactions->integer || !entity->client || entity->s.number == 0 || entity->health <= 0 )
	{
		return false;
	}
	switch ( entity->client->NPC_class )
	{
	case CLASS_DESANN:
	case CLASS_TAVION:
	case CLASS_LUKE:
		return false;
	default:
		break;
	}
	const playerState_t &state = entity->client->ps;
	return state.weapon != WP_SABER
		&& state.groundEntityNum != ENTITYNUM_NONE
		&& !( state.eFlags & EF_FORCE_GRIPPED )
		&& !( entity->s.eFlags & EF_LOCKED_TO_WEAPON )
		&& !IsKnockdownAnimation( state.legsAnim )
		&& !IsGetUpAnimation( state.legsAnim );
}

// Hands a living character's body over to its shot reaction: from the next physics step the AI is
// paused and the body reacts on its own (see ControlBodyAfterHit). The character then follows the
// body (see KeepCharacterWithBody), keeping its current offset from the hips.
static void TakeControl( RagdollInstance &instance, const gentity_t *entity )
{
	const Vector3 &pelvisPosition = instance.simulation.bodies[ragdoll::BODY_PELVIS].position;
	instance.characterOffsetFromPelvis = HorizontalPart( Vector3::FromArray( entity->currentOrigin ) - pelvisPosition );
	instance.controlStartPosition = pelvisPosition;
	instance.wanderSecondsLeft = 0.0f;
	instance.isInControl = true;
	instance.secondsInControl = 0.0f;
	instance.hasReactionPose = false;
	instance.isStepping = false;
	instance.lastDrawnLevelTime = level.time;

	if ( g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: shot reaction takes control (AI paused)\n", instance.entityNumber );
	}
}

// How hard hits push this ragdoll compared with a corpse: a body reacting on its own takes most of
// the push, one whose muscles keep it on its animation much less.
static float HitImpulseScaleFor( const RagdollInstance &instance )
{
	if ( !instance.isHitReaction )
	{
		return 1.0f;
	}
	return instance.isInControl ? SHOT_REACTION_IMPULSE_SCALE : g_ragdollHitReactionScale->value;
}

void G_Ragdoll_RecordDamage( gentity_t *target, const vec3_t direction, const vec3_t point, int damage, int meansOfDeath )
{
	if ( !target )
	{
		return;
	}
	const int entityNumber = EntityIndex( target );
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES )
	{
		return;
	}

	DamageRecord &record = lastDamage[entityNumber];
	record.time = level.time;
	record.damage = damage;
	record.meansOfDeath = meansOfDeath;
	VectorCopy( point ? point : target->currentOrigin, record.point );
	if ( direction )
	{
		VectorCopy( direction, record.direction );
	}
	else
	{
		VectorClear( record.direction );
	}

	if ( !g_ragdoll || !g_ragdoll->integer )
	{
		return;
	}

	// A hard hit on a living character knocks it off balance: it may stumble or fall over.
	if ( direction && target->client && target->health > 0 && PushesLivingCharacters( meansOfDeath ) )
	{
		ragdoll::LoseBalance( target, direction, record.point, BalancePushOfHit( damage, meansOfDeath ) );
	}

	// Hitting a body that is already a ragdoll pushes it straight away (gently, if it is a living
	// character whose muscles are fighting back).
	RagdollInstance *instance = FindInstance( entityNumber );
	if ( instance )
	{
		if ( instance->isHitReaction )
		{
			instance->secondsSinceLastHit = 0.0f;	// another hit keeps the reaction going
			if ( !instance->isInControl && direction && PushesLivingCharacters( meansOfDeath ) && CanTakeControlOf( target ) )
			{
				TakeControl( *instance, target );	// shot again while getting back to what it was doing
			}
		}
		ApplyHitImpulse( *instance, record.point, record.direction, damage, meansOfDeath, HitImpulseScaleFor( *instance ) );
		if ( meansOfDeath == MOD_ELECTROCUTE )
		{
			StartConvulsing( *instance );
		}
		return;
	}

	// A living character: jolt it physically the next time it is drawn (the reaction needs the pose
	// on screen to start from).
	if ( direction && WantsHitReaction( target, meansOfDeath ) )
	{
		HitReactionRecord &hit = pendingHitReactions[entityNumber];
		hit.pending = true;
		hit.time = level.time;
		VectorCopy( record.point, hit.point );
		VectorCopy( record.direction, hit.direction );
		hit.damage = damage;
		hit.meansOfDeath = meansOfDeath;
	}
}

/*
==================
Force powers

Push and Pull fling bodies, Grip lifts a body and holds it dangling in front of the user, and
Lightning makes bodies convulse.
==================
*/

extern void WP_ForcePowerStop( gentity_t *self, forcePowers_t forcePower );

// How fast Force Push and Pull fling a body at each power level (units per second).
static const float FORCE_PUSH_SPEED[NUM_FORCE_POWER_LEVELS] = { 0.0f, 350.0f, 500.0f, 700.0f };
static const float FORCE_PULL_SPEED[NUM_FORCE_POWER_LEVELS] = { 0.0f, 300.0f, 400.0f, 550.0f };
static const float FORCE_THROW_LIFT = 0.35f;				// upward share of the throw direction, so bodies leave the floor
static const float FORCE_THROW_UPPER_BODY_EXTRA = 0.25f;	// the upper body is thrown harder than the legs, so bodies tumble
static const float FORCE_THROW_DISTANCE_FALLOFF = 0.4f;		// a body at the edge of the range is thrown this much slower
static const float FORCE_PULL_ARRIVAL_FACTOR = 2.0f;		// pull speed is capped at distance * this, so bodies land near the user
static const float FORCE_AIM_TOLERANCE = 24.0f;				// powers with a cone of 1 must be aimed this close to a body

static const float GRIP_AIM_TOLERANCE = 40.0f;				// how close to a body Grip must be aimed to lift it
static const float GRIP_MINIMUM_HOLD_DISTANCE = 64.0f;
static const float GRIP_MAXIMUM_HOLD_DISTANCE = 160.0f;
static const float GRIP_HOLD_FREQUENCY = 7.0f;				// radians per second: how firmly the held body follows the hold point
static const float GRIP_HOLD_DAMPING = 5.0f;				// per second
static const float GRIP_HOLD_LIFT = 24.0f;					// the body is held this far above the line of sight, feet off the ground
static const float GRIP_LIFT_SECONDS = 0.7f;				// how long it takes to raise a body from where it lay to the hold point

// Force Lightning reaches ragdolls the same way the power reaches living targets: at the highest
// level it arcs over a wide cone, below that it is a narrow beam.
static const float LIGHTNING_ARC_RANGE = 512.0f;
static const float LIGHTNING_ARC_CONE = 0.5f;
static const float LIGHTNING_BEAM_RANGE = 1024.0f;
static const float LIGHTNING_BEAM_AIM_TOLERANCE = 24.0f;
static const float LIGHTNING_PUSH_SPEED = 8.0f;			// each zap (20 per second) nudges the body away from the user

static const float CONVULSION_SECONDS_PER_HIT = 0.25f;		// each lightning zap adds this much twitching
static const float LONGEST_CONVULSION_SECONDS = 0.8f;		// so twitching stops soon after the lightning does
static const float TWITCH_INTERVAL_SECONDS = 0.07f;			// how often the muscles jerk toward a new random pose
static const float TWITCH_ANGLE_DEGREES = 25.0f;
static const float TWITCH_MUSCLE_FREQUENCY = 22.0f;

static void UserEyeAndAim( const gentity_t *user, Vector3 &eye, Vector3 &forward )
{
	vec3_t forwardArray;
	AngleVectors( user->client->ps.viewangles, forwardArray, NULL, NULL );
	eye = Vector3::FromArray( user->client->renderInfo.eyePoint );
	forward = ragdoll::Normalized( Vector3::FromArray( forwardArray ) );
}

// How far the aim line passes from the nearest part of a ragdoll (zero or less means it goes
// through it). alongAim receives how far down the aim line that nearest point is.
static float AimMissDistance( const RagdollInstance &instance, const Vector3 &eye, const Vector3 &forward, float range, float &alongAim )
{
	float smallestMiss = 1.0e9f;
	alongAim = 0.0f;
	for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
	{
		const RigidBody &body = instance.simulation.bodies[bodyIndex];
		const Vector3 points[3] = {
			body.position,
			body.LocalToWorldPoint( body.spheres[0].localCentre ),
			body.LocalToWorldPoint( body.spheres[1].localCentre ) };
		for ( int pointIndex = 0; pointIndex < 3; pointIndex++ )
		{
			const float along = ragdoll::Dot( points[pointIndex] - eye, forward );
			if ( along < 0.0f || along > range )
			{
				continue;
			}
			const float miss = ragdoll::Length( points[pointIndex] - ( eye + forward * along ) ) - body.spheres[0].radius;
			if ( miss < smallestMiss )
			{
				smallestMiss = miss;
				alongAim = along;
			}
		}
	}
	return smallestMiss;
}

// True if nothing solid is between the user's eyes and a point on the ragdoll.
static bool UserCanSee( const gentity_t *user, const Vector3 &eye, const Vector3 &point )
{
	vec3_t startArray, endArray;
	eye.CopyToArray( startArray );
	point.CopyToArray( endArray );
	trace_t trace;
	gi.trace( &trace, startArray, NULL, NULL, endArray, user->s.number, MASK_SOLID, G2_NOCOLLIDE, 0 );
	return trace.fraction >= 1.0f;
}

static float TotalRagdollMass( const RagdollInstance &instance )
{
	float totalMass = 0.0f;
	for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
	{
		totalMass += instance.rig.bodyMass[bodyIndex];
	}
	return totalMass;
}

static void WakeUp( RagdollInstance &instance )
{
	if ( instance.state == RAGDOLL_ASLEEP )
	{
		SetState( instance, RAGDOLL_LIMP );
	}
}

// Lets go of a body held with Force Grip. It keeps whatever speed it had, so swinging the view
// while holding a body and then letting go flings it.
static void ReleaseGrip( RagdollInstance &instance )
{
	instance.gripperEntityNumber = -1;
	instance.simulation.bodies[ragdoll::BODY_CHEST].positionDrive.enabled = false;
}

static bool IsUpperBody( int bodyIndex )
{
	return bodyIndex >= ragdoll::BODY_MIDDLE_SPINE && bodyIndex <= ragdoll::BODY_LEFT_HAND;
}

void G_Ragdoll_ForceThrow( gentity_t *user, qboolean pull, int powerLevel, float range, float cone )
{
	if ( !g_ragdoll || !g_ragdoll->integer || !user || !user->client )
	{
		return;
	}
	const int level = (int)ragdoll::Clamp( (float)powerLevel, 0.0f, (float)( NUM_FORCE_POWER_LEVELS - 1 ) );
	const float throwSpeed = pull ? FORCE_PULL_SPEED[level] : FORCE_PUSH_SPEED[level];
	if ( throwSpeed <= 0.0f )
	{
		return;
	}

	Vector3 eye, forward;
	UserEyeAndAim( user, eye, forward );
	const int userNumber = EntityIndex( user );

	if ( g_ragdollDebug->integer >= 2 )
	{
		gi.Printf( "force %s used: level %d, range %.0f, cone %.2f\n", pull ? "pull" : "push", level, range, cone );
	}

	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		RagdollInstance &instance = ragdollInstances[instanceIndex];
		if ( !instance.inUse || instance.isHitReaction )
		{
			continue;	// a character flinching from a hit is still alive: the game's own push decides what happens
		}

		// In range and inside the power's cone (or aimed right at the body for a cone of 1).
		const Vector3 chestPosition = instance.simulation.bodies[ragdoll::BODY_CHEST].position;
		const Vector3 toChest = chestPosition - eye;
		const float distance = ragdoll::Length( toChest );
		if ( distance > range || distance < ragdoll::SMALL_NUMBER )
		{
			continue;
		}
		if ( cone >= 1.0f )
		{
			float alongAim;
			if ( AimMissDistance( instance, eye, forward, range, alongAim ) > FORCE_AIM_TOLERANCE )
			{
				continue;
			}
		}
		else if ( ragdoll::Dot( toChest / distance, forward ) < cone )
		{
			continue;
		}
		if ( !UserCanSee( user, eye, chestPosition ) )
		{
			continue;
		}

		// Pushing a body you are holding throws it.
		if ( instance.gripperEntityNumber == userNumber )
		{
			ReleaseGrip( instance );
			WP_ForcePowerStop( user, FP_GRIP );
		}

		// Throw along the ground (plus some lift), not along the line of sight: a body lying on the
		// floor is below the user's eyes, and pushing it down into the floor would just stop it.
		const Vector3 awayAlongGround = ragdoll::Normalized( HorizontalPart( toChest ) );
		float bodySpeed = throwSpeed * ( 1.0f - FORCE_THROW_DISTANCE_FALLOFF * distance / range );
		const Vector3 throwDirection = ragdoll::Normalized( ( pull ? -awayAlongGround : awayAlongGround ) + WORLD_UP * FORCE_THROW_LIFT );
		if ( pull )
		{
			bodySpeed = ( bodySpeed < distance * FORCE_PULL_ARRIVAL_FACTOR ) ? bodySpeed : distance * FORCE_PULL_ARRIVAL_FACTOR;
		}

		for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
		{
			const float tumble = IsUpperBody( bodyIndex ) ? 1.0f + FORCE_THROW_UPPER_BODY_EXTRA : 1.0f - FORCE_THROW_UPPER_BODY_EXTRA * 0.4f;
			instance.simulation.bodies[bodyIndex].linearVelocity += throwDirection * ( bodySpeed * tumble );
		}
		WakeUp( instance );
		instance.secondsAtRest = 0.0f;

		if ( g_ragdollDebug->integer )
		{
			gi.Printf( "ragdoll %d: force %s at %.0f units/s\n", instance.entityNumber, pull ? "pulled" : "pushed", bodySpeed );
		}
	}
}

qboolean G_Ragdoll_ForceGripStart( gentity_t *user, float range )
{
	if ( !g_ragdoll || !g_ragdoll->integer || !user || !user->client )
	{
		return qfalse;
	}

	Vector3 eye, forward;
	UserEyeAndAim( user, eye, forward );

	// The body the user is aiming most closely at.
	RagdollInstance *bestInstance = NULL;
	float bestMiss = GRIP_AIM_TOLERANCE;
	float bestDistance = 0.0f;
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		RagdollInstance &instance = ragdollInstances[instanceIndex];
		// Knocked-down and flinching characters are alive, so the game's own Grip handles them instead.
		if ( !instance.inUse || instance.gripperEntityNumber >= 0 || instance.isKnockdown || instance.isHitReaction )
		{
			continue;
		}
		float alongAim;
		const float miss = AimMissDistance( instance, eye, forward, range, alongAim );
		if ( miss < bestMiss && UserCanSee( user, eye, instance.simulation.bodies[ragdoll::BODY_CHEST].position ) )
		{
			bestInstance = &instance;
			bestMiss = miss;
			bestDistance = alongAim;
		}
	}
	if ( !bestInstance )
	{
		return qfalse;
	}

	bestInstance->gripperEntityNumber = EntityIndex( user );
	bestInstance->gripHoldDistance = ragdoll::Clamp( bestDistance, GRIP_MINIMUM_HOLD_DISTANCE, GRIP_MAXIMUM_HOLD_DISTANCE );
	bestInstance->gripSecondsHeld = 0.0f;
	bestInstance->gripStartPosition = bestInstance->simulation.bodies[ragdoll::BODY_CHEST].position;
	SetState( *bestInstance, RAGDOLL_LIMP );	// a held body hangs loose
	if ( g_ragdollDebug->integer )
	{
		gi.Printf( "ragdoll %d: lifted with force grip\n", bestInstance->entityNumber );
	}
	return qtrue;
}

qboolean G_Ragdoll_IsGrippingRagdoll( const gentity_t *user )
{
	if ( !user )
	{
		return qfalse;
	}
	const int userNumber = EntityIndex( user );
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		if ( ragdollInstances[instanceIndex].inUse && ragdollInstances[instanceIndex].gripperEntityNumber == userNumber )
		{
			return qtrue;
		}
	}
	return qfalse;
}

void G_Ragdoll_ForceLightning( gentity_t *user, int powerLevel )
{
	if ( !g_ragdoll || !g_ragdoll->integer || !user || !user->client )
	{
		return;
	}

	Vector3 eye, forward;
	UserEyeAndAim( user, eye, forward );
	const bool arcs = powerLevel > FORCE_LEVEL_2;

	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		RagdollInstance &instance = ragdollInstances[instanceIndex];
		if ( !instance.inUse )
		{
			continue;
		}

		const Vector3 chestPosition = instance.simulation.bodies[ragdoll::BODY_CHEST].position;
		const Vector3 toChest = chestPosition - eye;
		const float distance = ragdoll::Length( toChest );
		if ( distance < ragdoll::SMALL_NUMBER )
		{
			continue;
		}
		if ( arcs )
		{
			if ( distance > LIGHTNING_ARC_RANGE || ragdoll::Dot( toChest / distance, forward ) < LIGHTNING_ARC_CONE )
			{
				continue;
			}
		}
		else
		{
			float alongAim;
			if ( AimMissDistance( instance, eye, forward, LIGHTNING_BEAM_RANGE, alongAim ) > LIGHTNING_BEAM_AIM_TOLERANCE )
			{
				continue;
			}
		}
		if ( !UserCanSee( user, eye, chestPosition ) )
		{
			continue;
		}

		StartConvulsing( instance );
		const Vector3 nudge = ragdoll::Normalized( toChest ) * LIGHTNING_PUSH_SPEED;
		for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
		{
			instance.simulation.bodies[bodyIndex].linearVelocity += nudge;
		}
	}
}

// Each step: keeps a gripped body's chest at the hold point in front of the user, or lets go if
// the user stopped gripping.
static void UpdateForceGrip( RagdollInstance &instance )
{
	if ( instance.gripperEntityNumber < 0 )
	{
		return;
	}

	const gentity_t *user = &g_entities[instance.gripperEntityNumber];
	const bool stillGripping = user->inuse && user->client && user->health > 0
		&& ( user->client->ps.forcePowersActive & ( 1 << FP_GRIP ) )
		&& user->client->ps.forceGripEntityNum >= ENTITYNUM_WORLD;	// not gripping a living character instead
	if ( !stillGripping )
	{
		ReleaseGrip( instance );
		return;
	}

	Vector3 eye, forward;
	UserEyeAndAim( user, eye, forward );

	// Raise the body smoothly from where it lay (an ease-in-out curve), otherwise the sudden jerk
	// swings the legs up over the head.
	instance.gripSecondsHeld += PHYSICS_STEP_SECONDS;
	const float liftProgress = ragdoll::Clamp( instance.gripSecondsHeld / GRIP_LIFT_SECONDS, 0.0f, 1.0f );
	const float easedProgress = liftProgress * liftProgress * ( 3.0f - 2.0f * liftProgress );
	const Vector3 holdPoint = eye + forward * instance.gripHoldDistance + WORLD_UP * GRIP_HOLD_LIFT;

	// The spring is sized for the whole ragdoll, so the body hangs only a little below the hold point.
	RigidBody &chest = instance.simulation.bodies[ragdoll::BODY_CHEST];
	chest.positionDrive.enabled = true;
	chest.positionDrive.targetPosition = instance.gripStartPosition + ( holdPoint - instance.gripStartPosition ) * easedProgress;
	chest.positionDrive.stiffness = TotalRagdollMass( instance ) * GRIP_HOLD_FREQUENCY * GRIP_HOLD_FREQUENCY;
	chest.positionDrive.damping = GRIP_HOLD_DAMPING;
	instance.secondsAtRest = 0.0f;
}

// Force Lightning: each hit adds a little more twitching.
static void StartConvulsing( RagdollInstance &instance )
{
	if ( g_ragdollDebug->integer && instance.convulsionSecondsLeft <= 0.0f )
	{
		gi.Printf( "ragdoll %d: convulsing from force lightning\n", instance.entityNumber );
	}
	instance.convulsionSecondsLeft = ragdoll::Clamp( instance.convulsionSecondsLeft + CONVULSION_SECONDS_PER_HIT,
													 0.0f, LONGEST_CONVULSION_SECONDS );
	WakeUp( instance );
}

// While convulsing, every muscle jerks toward a new random pose near the current one many times a
// second, overriding whatever the current behaviour wanted.
static void ApplyConvulsions( RagdollInstance &instance )
{
	if ( instance.convulsionSecondsLeft <= 0.0f )
	{
		return;
	}
	instance.convulsionSecondsLeft -= PHYSICS_STEP_SECONDS;
	instance.secondsUntilNextTwitch -= PHYSICS_STEP_SECONDS;

	const Simulation &simulation = instance.simulation;
	if ( instance.secondsUntilNextTwitch <= 0.0f )
	{
		instance.secondsUntilNextTwitch = TWITCH_INTERVAL_SECONDS;
		for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
		{
			const Joint &joint = simulation.joints[jointIndex];
			const Quaternion currentRelative = ragdoll::Conjugate( simulation.bodies[joint.parentBodyIndex].orientation )
				* simulation.bodies[joint.childBodyIndex].orientation;
			const Vector3 randomAxis = ragdoll::Normalized( Vector3( Q_flrand( -1.0f, 1.0f ), Q_flrand( -1.0f, 1.0f ), Q_flrand( -1.0f, 1.0f ) ) );
			const float randomAngle = ragdoll::DegreesToRadians( Q_flrand( -TWITCH_ANGLE_DEGREES, TWITCH_ANGLE_DEGREES ) );
			instance.twitchTargets[jointIndex] = ragdoll::NormalizedQuaternion(
				currentRelative * ragdoll::QuaternionFromAxisAngle( randomAxis, randomAngle ) );
		}
	}

	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		SetMuscle( instance, jointIndex, instance.twitchTargets[jointIndex], TWITCH_MUSCLE_FREQUENCY );
	}
}

// Ends a hit reaction's hold on the animation so the body can carry on as a death or knockdown
// ragdoll. The pose the body is in right now becomes the starting pose for those behaviours.
static void StopFollowingAnimation( RagdollInstance &instance )
{
	Simulation &simulation = instance.simulation;
	RigidBody &pelvis = simulation.bodies[ragdoll::BODY_PELVIS];
	RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];
	pelvis.positionDrive.enabled = false;
	pelvis.worldDrive.naturalFrequency = 0.0f;

	// The feet are no longer held to the ground: the whole body is a ragdoll now.
	RigidBody &rightFoot = simulation.bodies[ragdoll::BODY_RIGHT_FOOT];
	RigidBody &leftFoot = simulation.bodies[ragdoll::BODY_LEFT_FOOT];
	rightFoot.positionDrive.enabled = false;
	rightFoot.worldDrive.naturalFrequency = 0.0f;
	leftFoot.positionDrive.enabled = false;
	leftFoot.worldDrive.naturalFrequency = 0.0f;

	// Nothing holds the body up any more: gravity comes back, and the shot reaction lets go.
	for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
	{
		simulation.bodies[bodyIndex].gravityScale = 1.0f;
	}
	pelvis.positionDrive.horizontalShare = 1.0f;
	instance.isInControl = false;
	instance.clutchSecondsLeft = 0.0f;
	instance.isStepping = false;

	instance.isHitReaction = false;
	instance.secondsSinceStart = 0.0f;
	instance.initialPelvisOrientation = pelvis.orientation;
	instance.initialChestOrientation = chest.orientation;
	instance.chestUpInBodySpace = ragdoll::InverseRotate( chest.orientation, WORLD_UP );
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		const Joint &joint = simulation.joints[jointIndex];
		instance.deathPoseTargets[jointIndex] = ragdoll::NormalizedQuaternion(
			ragdoll::Conjugate( simulation.bodies[joint.parentBodyIndex].orientation ) * simulation.bodies[joint.childBodyIndex].orientation );
		instance.groundPoseTargets[jointIndex] = instance.deathPoseTargets[jointIndex];
	}
}

void G_Ragdoll_OnDeath( gentity_t *entity, int meansOfDeath, int hitLocation )
{
	if ( !entity || !g_ragdoll || !g_ragdoll->integer )
	{
		return;
	}
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES )
	{
		return;
	}

	DeathRecord &record = pendingDeaths[entityNumber];
	record.pending = true;
	record.time = level.time;
	record.meansOfDeath = meansOfDeath;
	record.hitLocation = hitLocation;
	ragdollFinished[entityNumber] = false;

	pendingHitReactions[entityNumber].pending = false;
	ragdoll::ForgetBalance( entityNumber );		// a stumble ends with death; the body carries its speed on

	// Killed while knocked down or while reacting to a hit: the body carries on as a death ragdoll,
	// keeping all of its momentum.
	RagdollInstance *instance = FindInstance( entityNumber );
	if ( instance && instance->isHitReaction )
	{
		StopFollowingAnimation( *instance );
		bool deathWasExplosive = false;
		ImpulsePerDamagePoint( meansOfDeath, deathWasExplosive );
		instance->writheAllowed = !deathWasExplosive && hitLocation != HL_HEAD
			&& Q_flrand( 0.0f, 1.0f ) < g_ragdollWritheChance->value;
		record.pending = false;
		SetState( *instance, RAGDOLL_STAGGERING );
	}
	else if ( instance && instance->isKnockdown )
	{
		instance->isKnockdown = false;
		record.pending = false;
		if ( instance->state == RAGDOLL_DAZED || instance->state == RAGDOLL_GETTING_UP )
		{
			SetState( *instance, RAGDOLL_LIMP );
		}
	}
}

void G_Ragdoll_OnKnockdown( gentity_t *entity )
{
	if ( !entity || !entity->client || !g_ragdoll || !g_ragdoll->integer || !g_ragdollKnockdowns->integer )
	{
		return;
	}
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES )
	{
		return;		// the player is never knocked down as a ragdoll (their camera follows the body)
	}
	RagdollInstance *instance = FindInstance( entityNumber );
	if ( instance && instance->isHitReaction )
	{
		// Knocked over while still flinching from a hit: carry on as a knockdown, thrown by the push.
		StopFollowingAnimation( *instance );
		instance->isKnockdown = true;
		instance->knockdownAnimation = entity->client->ps.legsAnim;
		const Vector3 pushVelocity = Vector3::FromArray( entity->client->ps.velocity );
		for ( int bodyIndex = 0; bodyIndex < instance->simulation.bodyCount; bodyIndex++ )
		{
			instance->simulation.bodies[bodyIndex].linearVelocity += pushVelocity;
		}
		SetState( *instance, RAGDOLL_STAGGERING );
		return;
	}
	if ( instance )
	{
		return;		// already a ragdoll; the push itself (G_Ragdoll_ForceThrow) moves the body
	}

	KnockdownRecord &record = pendingKnockdowns[entityNumber];
	record.pending = true;
	record.time = level.time;
	record.knockdownAnimation = entity->client->ps.legsAnim;
}

void G_Ragdoll_OnEntityFreed( gentity_t *entity )
{
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES )
	{
		return;
	}

	RagdollInstance *instance = FindInstance( entityNumber );
	if ( instance )
	{
		ReleaseInstance( *instance, false );	// the model is about to be destroyed anyway
	}
	memset( &lastDamage[entityNumber], 0, sizeof( lastDamage[entityNumber] ) );
	memset( &pendingDeaths[entityNumber], 0, sizeof( pendingDeaths[entityNumber] ) );
	ragdollFinished[entityNumber] = false;
	ragdoll::ForgetBalance( entityNumber );
	G_Slice_ForgetCharacter( entityNumber );
}

/*
==================
Starting a ragdoll
==================
*/

// Gives every body the velocity it had in the animation just before the ragdoll started, plus a
// share of the character's own movement, so a running character keeps running as it falls.
static void InheritAnimationMotion( RagdollInstance &instance, const Transform *earlierFrames, const bool *earlierFrameIsValid,
									const Transform &modelToWorld, const Vector3 &entityVelocity, float entityVelocityShare )
{
	const float sampleSeconds = VELOCITY_SAMPLE_MILLISECONDS / 1000.0f;
	const float modelScale = instance.rig.modelScale;

	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		RigidBody &body = instance.simulation.bodies[bodyIndex];
		body.linearVelocity = entityVelocity * entityVelocityShare;

		const int boneIndex = instance.rig.bodyBoneIndex[bodyIndex];
		if ( !earlierFrameIsValid[boneIndex] )
		{
			continue;
		}

		const Transform earlierBone( ragdoll::NormalizedQuaternion( modelToWorld.rotation * earlierFrames[boneIndex].rotation ),
									 modelToWorld.TransformPoint( earlierFrames[boneIndex].position * modelScale ) );
		const Vector3 earlierCentre = earlierBone.TransformPoint( instance.rig.centreOfMassInBoneSpace[bodyIndex] );

		Vector3 animationVelocity = ( body.position - earlierCentre ) / sampleSeconds;
		const float animationSpeed = ragdoll::Length( animationVelocity );
		if ( animationSpeed > MAXIMUM_INHERITED_SPEED )
		{
			animationVelocity *= MAXIMUM_INHERITED_SPEED / animationSpeed;
		}

		Vector3 animationSpin = ragdoll::RotationVectorFromQuaternion( body.orientation * ragdoll::Conjugate( earlierBone.rotation ) ) / sampleSeconds;
		const float spinSpeed = ragdoll::Length( animationSpin );
		if ( spinSpeed > MAXIMUM_INHERITED_SPIN )
		{
			animationSpin *= MAXIMUM_INHERITED_SPIN / spinSpeed;
		}

		body.linearVelocity += animationVelocity;
		body.angularVelocity = animationSpin;
	}
}

// Chooses how each joint moves while writhing. Positive angles about the left-right axis curl
// the spine and neck forward, swing the legs and arms backward, so the centre angles below are
// chosen to pull the body into a loose, protective curl.
static void ChooseWritheMotions( RagdollInstance &instance )
{
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		const int bodyIndex = ragdoll::BodyIndexForJoint( jointIndex );
		const Joint &joint = instance.simulation.joints[jointIndex];
		WritheMotion &motion = instance.writhe[jointIndex];

		motion.axisInChildSpace = instance.rig.lateralAxisInBodySpace[bodyIndex];
		motion.cyclesPerSecond = Q_flrand( 0.5f, 1.1f );
		motion.phase = Q_flrand( 0.0f, 2.0f * ragdoll::PI_RADIANS );

		float centreDegrees = 0.0f;
		float swingDegrees = 0.0f;
		switch ( bodyIndex )
		{
		case ragdoll::BODY_LOWER_SPINE:
		case ragdoll::BODY_MIDDLE_SPINE:
		case ragdoll::BODY_CHEST:
			centreDegrees = 10.0f;
			swingDegrees = 10.0f;
			break;
		case ragdoll::BODY_NECK:
		case ragdoll::BODY_HEAD:
			centreDegrees = 8.0f;
			swingDegrees = 12.0f;
			break;
		case ragdoll::BODY_RIGHT_UPPER_ARM:
		case ragdoll::BODY_LEFT_UPPER_ARM:
			centreDegrees = -25.0f;		// toward the chest
			swingDegrees = 20.0f;
			break;
		case ragdoll::BODY_RIGHT_THIGH:
		case ragdoll::BODY_LEFT_THIGH:
			centreDegrees = -20.0f;		// knees drawn up
			swingDegrees = 20.0f;
			break;
		case ragdoll::BODY_RIGHT_FOREARM:
		case ragdoll::BODY_LEFT_FOREARM:
		case ragdoll::BODY_RIGHT_SHIN:
		case ragdoll::BODY_LEFT_SHIN:
			motion.axisInChildSpace = joint.childAxis;	// hinges can only move about their own axis
			centreDegrees = 35.0f;		// bent
			swingDegrees = 25.0f;
			break;
		default:						// hands and feet
			swingDegrees = 10.0f;
			break;
		}
		motion.centreAngle = ragdoll::DegreesToRadians( centreDegrees );
		motion.swingAngle = ragdoll::DegreesToRadians( swingDegrees );
	}
}

// Starts a ragdoll from the pose currently on screen.
//   reactToDeath  a fresh death or knockdown: stagger and react to the blow (otherwise start limp)
//   kind          a corpse, a knockdown (alive, gets back up) or a hit reaction (alive, keeps animating)
static RagdollInstance *StartRagdoll( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3],
									  bool reactToDeath, ragdollKind_t kind )
{
	const bool isKnockdown = ( kind == RAGDOLL_KIND_KNOCKDOWN );
	const bool isHitReaction = ( kind == RAGDOLL_KIND_HIT_REACTION );
	const bool isCorpse = ( kind == RAGDOLL_KIND_CORPSE );
	const int entityNumber = EntityIndex( entity );
	const Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &entity->ghoul2[entity->playerModel] ) );
	if ( !skeleton )
	{
		if ( isCorpse )
		{
			ragdollFinished[entityNumber] = true;	// this corpse can never be a ragdoll, do not try again
		}
		return NULL;
	}

	RagdollInstance *instance = AllocateInstance();
	if ( !instance )
	{
		return NULL;
	}

	// Read the pose slightly in the past first, then the current pose, so the model's cached
	// skeleton is left at the current time for the rest of the frame.
	static Transform earlierFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static Transform currentFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool earlierFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool currentFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime - VELOCITY_SAMPLE_MILLISECONDS,
									earlierFrames, earlierFrameIsValid );
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime,
									currentFrames, currentFrameIsValid );

	const Transform modelToWorld( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) );
	if ( !ragdoll::BuildHumanoidRagdoll( instance->rig, instance->simulation, *skeleton, currentFrames, currentFrameIsValid,
										 modelToWorld, ModelScaleOf( entity ) ) )
	{
		if ( isCorpse )
		{
			ragdollFinished[entityNumber] = true;	// missing bones, do not try again (the slot was never marked as used)
		}
		return NULL;
	}

	instance->inUse = true;
	instance->entityNumber = entityNumber;
	instance->startTime = level.time;
	instance->secondsInState = 0.0f;
	instance->secondsSinceStart = 0.0f;
	instance->secondsAtRest = 0.0f;
	instance->lastServerOrigin = Vector3( 1.0e9f, 1.0e9f, 1.0e9f );
	instance->renderYawDegrees = ragdoll::RadiansToDegrees( atan2f( renderAxis[0][1], renderAxis[0][0] ) );
	instance->gripperEntityNumber = -1;
	instance->gripHoldDistance = 0.0f;
	instance->convulsionSecondsLeft = 0.0f;
	instance->secondsUntilNextTwitch = 0.0f;
	instance->originalMinimum = Vector3::FromArray( entity->mins );
	instance->originalMaximum = Vector3::FromArray( entity->maxs );
	instance->originalContents = entity->contents;
	instance->isKnockdown = isKnockdown;
	instance->knockdownAnimation = pendingKnockdowns[entityNumber].knockdownAnimation;
	instance->dazedSecondsNeeded = Q_flrand( KNOCKDOWN_SHORTEST_DAZE_SECONDS, KNOCKDOWN_LONGEST_DAZE_SECONDS );
	instance->getUpRequested = false;
	instance->getUpSeconds = 0.0f;
	instance->lastGetUpRenderTime = renderTime;
	instance->isHitReaction = isHitReaction;
	instance->secondsSinceLastHit = 0.0f;
	instance->hasAnimationTargets = false;
	instance->hasPreviousPelvisTarget = false;
	instance->isStepping = false;
	instance->swingingFoot = -1;
	instance->lastSteppedFoot = -1;
	instance->hasPreviousCentreOfMass = false;
	instance->isInControl = false;
	instance->secondsInControl = 0.0f;
	instance->lastDrawnLevelTime = level.time;
	instance->hasReactionPose = false;
	instance->standingPelvisHeight = -1.0f;
	instance->wanderSecondsLeft = 0.0f;
	instance->wanderAcceleration = Vector3( 0.0f, 0.0f, 0.0f );
	instance->clutchingHand = -1;
	instance->woundBody = ragdoll::BODY_CHEST;
	instance->clutchSecondsLeft = 0.0f;
	instance->flinchDirection = Vector3( 1.0f, 0.0f, 0.0f );
	instance->flinchRadians = 0.0f;
	instance->flinchTwistRadians = 0.0f;
	instance->flinchHipDrop = 0.0f;
	instance->secondsSinceFlinch = 1.0e6f;
	const bool takesControl = isHitReaction && CanTakeControlOf( entity );

	// A knockdown keeps all of the push's speed (the Force threw the whole character) and a death
	// keeps part of the character's running speed. A shot reaction that takes control keeps all of
	// it (a running character shot carries on stumbling forward); one that follows the animation
	// keeps none, since the body is carried along with the character's movement instead.
	const float entityVelocityShare = isKnockdown ? 1.0f : ( isHitReaction ? ( takesControl ? 1.0f : 0.0f ) : ENTITY_VELOCITY_SHARE );
	InheritAnimationMotion( *instance, earlierFrames, earlierFrameIsValid, modelToWorld,
							Vector3::FromArray( entity->client->ps.velocity ), entityVelocityShare );

	const Simulation &simulation = instance->simulation;
	instance->initialPelvisOrientation = simulation.bodies[ragdoll::BODY_PELVIS].orientation;
	instance->initialChestOrientation = simulation.bodies[ragdoll::BODY_CHEST].orientation;
	instance->chestUpInBodySpace = ragdoll::InverseRotate( instance->initialChestOrientation, WORLD_UP );
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		instance->deathPoseTargets[jointIndex] = simulation.joints[jointIndex].muscle.targetRelativeOrientation;
		instance->groundPoseTargets[jointIndex] = instance->deathPoseTargets[jointIndex];
	}

	const DeathRecord &death = pendingDeaths[entityNumber];
	bool deathWasExplosive = false;
	ImpulsePerDamagePoint( death.meansOfDeath, deathWasExplosive );
	instance->writheAllowed = reactToDeath
		&& isCorpse
		&& !deathWasExplosive
		&& death.hitLocation != HL_HEAD
		&& Q_flrand( 0.0f, 1.0f ) < g_ragdollWritheChance->value;
	instance->writheDurationSeconds = Q_flrand( 2.5f, 4.5f );
	ChooseWritheMotions( *instance );

	instance->state = isHitReaction ? RAGDOLL_HIT_REACTION : ( reactToDeath ? RAGDOLL_STAGGERING : RAGDOLL_LIMP );
	SetState( *instance, instance->state );
	if ( takesControl )
	{
		TakeControl( *instance, entity );
	}

	// React to the blow that killed the character.
	const DamageRecord &killingHit = lastDamage[entityNumber];
	if ( reactToDeath && killingHit.time && level.time - killingHit.time <= RECENT_DAMAGE_MILLISECONDS )
	{
		ApplyHitImpulse( *instance, killingHit.point, killingHit.direction, killingHit.damage, killingHit.meansOfDeath, 1.0f );
	}

	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		instance->previousBodyTransforms[bodyIndex] = Transform( simulation.bodies[bodyIndex].orientation, simulation.bodies[bodyIndex].position );
	}
	return instance;
}

/*
==================
Behaviours

Each physics step, the current behaviour decides which muscles are tense, how strong they are
and which pose they pull toward. The physics then works out what the body actually does.
==================
*/

static void SetMuscle( RagdollInstance &instance, int jointIndex, const Quaternion &targetRelativeOrientation, float naturalFrequency )
{
	ragdoll::JointMuscle &muscle = instance.simulation.joints[jointIndex].muscle;
	muscle.targetRelativeOrientation = targetRelativeOrientation;
	muscle.naturalFrequency = naturalFrequency;
}

// Tenses every joint of a muscle group toward its pose at the moment of death.
static void HoldDeathPose( RagdollInstance &instance, ragdoll::muscleGroup_t group, float naturalFrequency )
{
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		if ( ragdoll::MuscleGroupForBody( ragdoll::BodyIndexForJoint( jointIndex ) ) == group )
		{
			SetMuscle( instance, jointIndex, instance.deathPoseTargets[jointIndex], naturalFrequency );
		}
	}
}

// The relative orientation of a hinge bent by bendRadians from its neutral pose.
static Quaternion HingeBentBy( const Joint &joint, float bendRadians )
{
	return ragdoll::NormalizedQuaternion( joint.neutralRelativeOrientation * ragdoll::QuaternionFromAxisAngle( joint.childAxis, bendRadians ) );
}

// Reaches both arms toward the ground in the direction the body is falling, elbows slightly bent,
// the way people try to break a fall.
static void BraceArmsForFall( RagdollInstance &instance, float naturalFrequency )
{
	const Simulation &simulation = instance.simulation;
	const RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];

	// Fall toward where the chest is moving, or failing that where it is leaning.
	Vector3 fallDirection = HorizontalPart( chest.linearVelocity );
	if ( ragdoll::Length( fallDirection ) < 40.0f )
	{
		fallDirection = HorizontalPart( ragdoll::Rotate( chest.orientation, instance.chestUpInBodySpace ) );
	}
	fallDirection = ragdoll::Normalized( fallDirection );

	const int upperArmBodies[2] = { ragdoll::BODY_RIGHT_UPPER_ARM, ragdoll::BODY_LEFT_UPPER_ARM };
	for ( int side = 0; side < 2; side++ )
	{
		const int upperArmBody = upperArmBodies[side];
		const int shoulderJoint = ragdoll::JointIndexForBody( upperArmBody );
		const int elbowJoint = ragdoll::JointIndexForBody( upperArmBody + 1 );
		const int wristJoint = ragdoll::JointIndexForBody( upperArmBody + 2 );
		const Joint &shoulder = simulation.joints[shoulderJoint];
		const RigidBody &upperArm = simulation.bodies[upperArmBody];
		const RigidBody &shoulderParent = simulation.bodies[shoulder.parentBodyIndex];

		// Reach forward-and-down, a little out to the side of the shoulder.
		const Vector3 shoulderPosition = upperArm.LocalToWorldPoint( shoulder.childAnchor );
		const Vector3 outward = ragdoll::Normalized( HorizontalPart( shoulderPosition - chest.position ) );
		const Vector3 reachDirection = ragdoll::Normalized( fallDirection * 0.8f - WORLD_UP * 0.6f + outward * 0.3f );

		const Vector3 armDirection = ragdoll::Rotate( upperArm.orientation, shoulder.childAxis );
		const Quaternion reachingOrientation = ragdoll::ShortestArc( armDirection, reachDirection ) * upperArm.orientation;
		SetMuscle( instance, shoulderJoint, ragdoll::NormalizedQuaternion( ragdoll::Conjugate( shoulderParent.orientation ) * reachingOrientation ), naturalFrequency );
		SetMuscle( instance, elbowJoint, HingeBentBy( simulation.joints[elbowJoint], ragdoll::DegreesToRadians( 25.0f ) ), naturalFrequency * 0.8f );
		SetMuscle( instance, wristJoint, instance.deathPoseTargets[wristJoint], naturalFrequency * 0.4f );
	}
}

static void Writhe( RagdollInstance &instance, float intensity )
{
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		const WritheMotion &motion = instance.writhe[jointIndex];
		const float angle = motion.centreAngle
			+ motion.swingAngle * sinf( 2.0f * ragdoll::PI_RADIANS * motion.cyclesPerSecond * instance.secondsInState + motion.phase );
		const Quaternion target = ragdoll::NormalizedQuaternion(
			instance.groundPoseTargets[jointIndex] * ragdoll::QuaternionFromAxisAngle( motion.axisInChildSpace, angle ) );

		float naturalFrequency;
		switch ( ragdoll::MuscleGroupForBody( ragdoll::BodyIndexForJoint( jointIndex ) ) )
		{
		case ragdoll::MUSCLE_GROUP_SPINE:	naturalFrequency = 9.0f;	break;
		case ragdoll::MUSCLE_GROUP_NECK:	naturalFrequency = 7.0f;	break;
		case ragdoll::MUSCLE_GROUP_LEGS:	naturalFrequency = 8.0f;	break;
		default:							naturalFrequency = 7.0f;	break;
		}
		SetMuscle( instance, jointIndex, target, naturalFrequency * intensity );
	}
}

static bool IsTorsoTouchingGround( const Simulation &simulation )
{
	return simulation.IsBodyTouchingWorld( ragdoll::BODY_PELVIS )
		|| simulation.IsBodyTouchingWorld( ragdoll::BODY_LOWER_SPINE )
		|| simulation.IsBodyTouchingWorld( ragdoll::BODY_MIDDLE_SPINE )
		|| simulation.IsBodyTouchingWorld( ragdoll::BODY_CHEST )
		|| simulation.IsBodyTouchingWorld( ragdoll::BODY_HEAD );
}

// Decides whether to move on to the next behaviour.
static void UpdateBehaviourState( RagdollInstance &instance )
{
	const Simulation &simulation = instance.simulation;
	const RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];
	const Vector3 chestUp = ragdoll::Rotate( chest.orientation, instance.chestUpInBodySpace );
	const float chestTiltDegrees = ragdoll::RadiansToDegrees( acosf( ragdoll::Clamp( ragdoll::Dot( chestUp, WORLD_UP ), -1.0f, 1.0f ) ) );
	const bool torsoOnGround = IsTorsoTouchingGround( simulation );

	switch ( instance.state )
	{
	case RAGDOLL_STAGGERING:
		if ( torsoOnGround )
		{
			SetState( instance, RAGDOLL_LANDED );
		}
		else if ( instance.secondsInState > 0.9f || chestTiltDegrees > 50.0f )
		{
			SetState( instance, RAGDOLL_FALLING );
		}
		break;

	case RAGDOLL_FALLING:
		// Landing on the floor, or coming to a stop on something else (such as another body).
		if ( torsoOnGround || ( instance.secondsInState > 0.3f && ragdoll::Length( chest.linearVelocity ) < STOPPED_FALLING_SPEED ) )
		{
			SetState( instance, RAGDOLL_LANDED );
		}
		else if ( instance.secondsInState > 3.0f )
		{
			// Fell a long way: a dead body gives up, a knocked-down one lies stunned wherever it is.
			SetState( instance, instance.isKnockdown ? RAGDOLL_DAZED : RAGDOLL_LIMP );
		}
		break;

	case RAGDOLL_LANDED:
		if ( instance.secondsInState > 0.4f )
		{
			// Writhing and lying dazed both move around the pose the body landed in.
			for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
			{
				const Joint &joint = simulation.joints[jointIndex];
				instance.groundPoseTargets[jointIndex] = ragdoll::NormalizedQuaternion(
					ragdoll::Conjugate( simulation.bodies[joint.parentBodyIndex].orientation ) * simulation.bodies[joint.childBodyIndex].orientation );
			}

			if ( instance.isKnockdown )
			{
				SetState( instance, RAGDOLL_DAZED );
			}
			else if ( instance.writheAllowed )
			{
				SetState( instance, RAGDOLL_WRITHING );
			}
			else
			{
				SetState( instance, RAGDOLL_LIMP );
			}
		}
		break;

	case RAGDOLL_DAZED:
		// Get up once stunned long enough and the body has settled, or after a while regardless.
		if ( !instance.getUpRequested
			&& ( ( instance.secondsInState > instance.dazedSecondsNeeded && simulation.FastestBodySpeed() < GET_UP_SETTLED_SPEED )
				|| instance.secondsInState > KNOCKDOWN_LONGEST_DAZE_BEFORE_GETTING_UP_ANYWAY ) )
		{
			instance.getUpRequested = true;		// started on the next drawn frame (it needs the model)
		}
		break;

	case RAGDOLL_GETTING_UP:
	case RAGDOLL_HIT_REACTION:		// driven by the animation instead (DriveTowardAnimation)
		break;

	case RAGDOLL_WRITHING:
		if ( instance.secondsInState > instance.writheDurationSeconds )
		{
			SetState( instance, RAGDOLL_LIMP );
		}
		break;

	case RAGDOLL_LIMP:
	{
		instance.secondsSinceRestCheck += PHYSICS_STEP_SECONDS;
		if ( instance.secondsSinceRestCheck >= REST_CHECK_SECONDS )
		{
			// How far the furthest-moving part has gone since the last check.
			float largestMovement = 0.0f;
			for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
			{
				const RigidBody &body = simulation.bodies[bodyIndex];
				for ( int sphereIndex = 0; sphereIndex < body.sphereCount; sphereIndex++ )
				{
					const Vector3 position = body.LocalToWorldPoint( body.spheres[sphereIndex].localCentre );
					const float movement = ragdoll::Length( position - instance.restCheckPositions[bodyIndex][sphereIndex] );
					largestMovement = ( movement > largestMovement ) ? movement : largestMovement;
					instance.restCheckPositions[bodyIndex][sphereIndex] = position;
				}
			}
			instance.secondsAtRest = ( largestMovement < RESTING_MOVEMENT ) ? instance.secondsAtRest + instance.secondsSinceRestCheck : 0.0f;
			instance.secondsSinceRestCheck = 0.0f;

			if ( g_ragdollDebug->integer >= 2 && largestMovement < 1.0e6f )	// the very first check has no earlier snapshot
			{
				gi.Printf( "ragdoll %d: limp, largest movement in the last %.1f seconds %.2f (rests below %.1f)\n",
						   instance.entityNumber, REST_CHECK_SECONDS, largestMovement, RESTING_MOVEMENT );
			}
		}

		if ( g_ragdollDebug->integer >= 3 && fmodf( instance.secondsInState, 1.0f ) < PHYSICS_STEP_SECONDS )
		{
			// Report which body part is moving fastest, to help find anything that will not settle.
			int fastestBody = 0;
			float fastestSpeed = -1.0f;
			for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
			{
				const RigidBody &body = simulation.bodies[bodyIndex];
				const float speed = ragdoll::Length( body.linearVelocity ) + ragdoll::Length( body.angularVelocity ) * 8.0f;
				if ( speed > fastestSpeed )
				{
					fastestSpeed = speed;
					fastestBody = bodyIndex;
				}
			}
			const RigidBody &fastest = simulation.bodies[fastestBody];
			gi.Printf( "ragdoll %d: fastest part %s moving %.1f spinning %.1f\n", instance.entityNumber,
					   instance.rig.skeleton->bones[instance.rig.bodyBoneIndex[fastestBody]].name,
					   ragdoll::Length( fastest.linearVelocity ), ragdoll::Length( fastest.angularVelocity ) );
		}
		if ( instance.secondsAtRest > SECONDS_AT_REST_BEFORE_SLEEP && instance.secondsSinceStart > MINIMUM_SECONDS_BEFORE_SLEEP
			&& instance.gripperEntityNumber < 0 && instance.convulsionSecondsLeft <= 0.0f )	// never sleep while held or shocked
		{
			SetState( instance, RAGDOLL_ASLEEP );
		}
		break;
	}

	case RAGDOLL_ASLEEP:
		break;
	}
}

// Sets every muscle for the current behaviour.
static void ApplyBehaviourMuscles( RagdollInstance &instance )
{
	Simulation &simulation = instance.simulation;
	const float time = instance.secondsInState;

	// Start from fully relaxed.
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		SetMuscle( instance, jointIndex, instance.deathPoseTargets[jointIndex], 0.0f );
	}
	simulation.bodies[ragdoll::BODY_PELVIS].worldDrive.naturalFrequency = 0.0f;
	simulation.bodies[ragdoll::BODY_CHEST].worldDrive.naturalFrequency = 0.0f;

	switch ( instance.state )
	{
	case RAGDOLL_STAGGERING:
	{
		// The legs give way quickly while the torso still tries to stay upright, which turns the
		// hit into a stumble rather than an instant collapse.
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_SPINE, 18.0f * expf( -time / 1.0f ) );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_NECK, 20.0f );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_ARMS, 9.0f );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_LEGS, 26.0f * expf( -time / 0.3f ) );

		const float uprightFrequency = 9.0f * expf( -time / 0.35f );
		RigidBody &pelvis = simulation.bodies[ragdoll::BODY_PELVIS];
		RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];
		pelvis.worldDrive.targetOrientation = instance.initialPelvisOrientation;
		pelvis.worldDrive.naturalFrequency = uprightFrequency;
		pelvis.worldDrive.dampingRatio = 0.9f;
		chest.worldDrive.targetOrientation = instance.initialChestOrientation;
		chest.worldDrive.naturalFrequency = uprightFrequency;
		chest.worldDrive.dampingRatio = 0.9f;
		break;
	}

	case RAGDOLL_FALLING:
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_SPINE, 10.0f * expf( -instance.secondsSinceStart / 1.5f ) );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_NECK, 16.0f );	// keep the head up
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_LEGS, 3.0f );
		BraceArmsForFall( instance, 13.0f );
		break;

	case RAGDOLL_LANDED:
	{
		// The impact knocks the strength out of everything.
		const float fade = expf( -time / 0.2f );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_SPINE, 6.0f * fade );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_NECK, 10.0f * fade );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_ARMS, 4.0f * fade );
		HoldDeathPose( instance, ragdoll::MUSCLE_GROUP_LEGS, 2.0f * fade );
		break;
	}

	case RAGDOLL_WRITHING:
	{
		// Ease in, then weaken steadily until the end.
		const float easeIn = ragdoll::Clamp( time / 0.4f, 0.0f, 1.0f );
		const float weakening = ragdoll::Clamp( 1.0f - time / instance.writheDurationSeconds, 0.0f, 1.0f );
		Writhe( instance, easeIn * weakening );
		break;
	}

	case RAGDOLL_DAZED:
	{
		// Stunned: loosely holding the landed pose, slowly gathering strength to get up.
		const float recovering = ragdoll::Clamp( time / instance.dazedSecondsNeeded, 0.0f, 1.0f );
		for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
		{
			float naturalFrequency;
			switch ( ragdoll::MuscleGroupForBody( ragdoll::BodyIndexForJoint( jointIndex ) ) )
			{
			case ragdoll::MUSCLE_GROUP_SPINE:	naturalFrequency = 4.0f;	break;
			case ragdoll::MUSCLE_GROUP_NECK:	naturalFrequency = 8.0f;	break;
			default:							naturalFrequency = 3.0f;	break;
			}
			SetMuscle( instance, jointIndex, instance.groundPoseTargets[jointIndex], naturalFrequency * ( 0.5f + 0.5f * recovering ) );
		}
		break;
	}

	case RAGDOLL_LIMP:
	case RAGDOLL_ASLEEP:
	case RAGDOLL_GETTING_UP:
	case RAGDOLL_HIT_REACTION:		// driven by the animation instead (DriveTowardAnimation)
		break;
	}

	// Force Lightning twitching overrides the behaviour while it lasts.
	ApplyConvulsions( instance );

	// Global strength setting.
	const float strengthScale = ( g_ragdollMuscleStrength->value > 0.0f ) ? g_ragdollMuscleStrength->value : 0.0f;
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		simulation.joints[jointIndex].muscle.naturalFrequency *= strengthScale;
	}
	simulation.bodies[ragdoll::BODY_PELVIS].worldDrive.naturalFrequency *= strengthScale;
	simulation.bodies[ragdoll::BODY_CHEST].worldDrive.naturalFrequency *= strengthScale;
}

/*
==================
Physics stepping
==================
*/

// Answers the solver's sphere sweeps with the game's box trace. A box slightly smaller than the
// sphere is used, which is close enough for bodies and much cheaper than a true sphere sweep.
// The context is the ragdoll doing the sweep, so its own corpse is ignored.
static void SweepSphereThroughWorld( void *userContext, const Vector3 &start, const Vector3 &end, float radius, WorldSweepResult &result )
{
	const RagdollInstance &sweepingRagdoll = *(const RagdollInstance *)userContext;
	const float halfSize = radius * 0.9f;
	vec3_t boxMinimum = { -halfSize, -halfSize, -halfSize };
	vec3_t boxMaximum = { halfSize, halfSize, halfSize };
	vec3_t startPoint, endPoint;
	start.CopyToArray( startPoint );
	end.CopyToArray( endPoint );

	trace_t trace;
	gi.trace( &trace, startPoint, boxMinimum, boxMaximum, endPoint, sweepingRagdoll.entityNumber, MASK_SOLID, G2_NOCOLLIDE, 0 );

	result.startedInSolid = trace.startsolid || trace.allsolid;
	result.hit = trace.fraction < 1.0f;
	result.contactCentre = Vector3::FromArray( trace.endpos );
	result.normal = Vector3::FromArray( trace.plane.normal );
}

/*
==================
Stepping every ragdoll together

All awake ragdolls are simulated in one shared scene so they collide with each other. Living
characters near them are added as obstacles that push bodies but are not pushed back. The scene
is stepped once per rendered frame, by whichever character happens to be drawn first.
==================
*/

static ragdoll::Scene ragdollScene;

// Living characters closer than this to a ragdoll are added to the scene as obstacles.
static const float OBSTACLE_SEARCH_DISTANCE = 64.0f;

// A sleeping ragdoll wakes up when a moving ragdoll or a moving living character comes this close.
// Something standing still next to it does not wake it, otherwise the two would wake and sleep
// over and over.
static const float WAKE_DISTANCE = 4.0f;
static const float WAKING_SPEED = 20.0f;

static void InstanceBounds( const RagdollInstance &instance, float growth, Vector3 &minimum, Vector3 &maximum )
{
	instance.simulation.ComputeBounds( minimum, maximum );
	minimum -= Vector3( growth, growth, growth );
	maximum += Vector3( growth, growth, growth );
}

static bool BoundsOverlap( const Vector3 &firstMinimum, const Vector3 &firstMaximum, const Vector3 &secondMinimum, const Vector3 &secondMaximum )
{
	return firstMinimum.x <= secondMaximum.x && firstMaximum.x >= secondMinimum.x
		&& firstMinimum.y <= secondMaximum.y && firstMaximum.y >= secondMinimum.y
		&& firstMinimum.z <= secondMaximum.z && firstMaximum.z >= secondMinimum.z;
}

// The collision boxes of every living, visible character, gathered once per physics step.
static const int MAXIMUM_LIVING_CHARACTERS = 64;
static Vector3 livingCharacterMinimum[MAXIMUM_LIVING_CHARACTERS];
static Vector3 livingCharacterMaximum[MAXIMUM_LIVING_CHARACTERS];
static float livingCharacterSpeed[MAXIMUM_LIVING_CHARACTERS];
static int livingCharacterCount = 0;

static void GatherLivingCharacters( void )
{
	livingCharacterCount = 0;
	for ( int entityNumber = 0; entityNumber < MAX_GENTITIES && livingCharacterCount < MAXIMUM_LIVING_CHARACTERS; entityNumber++ )
	{
		const gentity_t *entity = &g_entities[entityNumber];
		if ( entity->inuse && entity->client && entity->health > 0
			&& !( entity->s.eFlags & EF_NODRAW ) && ( entity->contents & CONTENTS_BODY )
			&& !FindInstance( entityNumber ) )	// a knocked-down character is its own ragdoll; its box must not push it
		{
			livingCharacterMinimum[livingCharacterCount] = Vector3::FromArray( entity->absmin );
			livingCharacterMaximum[livingCharacterCount] = Vector3::FromArray( entity->absmax );
			livingCharacterSpeed[livingCharacterCount] = ragdoll::Length( Vector3::FromArray( entity->client->ps.velocity ) );
			livingCharacterCount++;
		}
	}
}

// Wakes sleeping ragdolls that another ragdoll or a living character is touching.
static void WakeDisturbedRagdolls( void )
{
	for ( int sleeperIndex = 0; sleeperIndex < MAXIMUM_RAGDOLLS; sleeperIndex++ )
	{
		RagdollInstance &sleeper = ragdollInstances[sleeperIndex];
		if ( !sleeper.inUse || sleeper.state != RAGDOLL_ASLEEP )
		{
			continue;
		}
		Vector3 sleeperMinimum, sleeperMaximum;
		InstanceBounds( sleeper, WAKE_DISTANCE, sleeperMinimum, sleeperMaximum );

		bool disturbed = false;
		for ( int otherIndex = 0; otherIndex < MAXIMUM_RAGDOLLS && !disturbed; otherIndex++ )
		{
			const RagdollInstance &other = ragdollInstances[otherIndex];
			if ( !IsSimulated( other ) )
			{
				continue;
			}
			if ( other.simulation.FastestBodySpeed() < WAKING_SPEED )
			{
				continue;
			}
			Vector3 otherMinimum, otherMaximum;
			InstanceBounds( other, 0.0f, otherMinimum, otherMaximum );
			disturbed = BoundsOverlap( sleeperMinimum, sleeperMaximum, otherMinimum, otherMaximum );
		}
		for ( int characterIndex = 0; characterIndex < livingCharacterCount && !disturbed; characterIndex++ )
		{
			if ( livingCharacterSpeed[characterIndex] < WAKING_SPEED )
			{
				continue;
			}
			disturbed = BoundsOverlap( sleeperMinimum, sleeperMaximum, livingCharacterMinimum[characterIndex], livingCharacterMaximum[characterIndex] );
		}

		if ( disturbed )
		{
			SetState( sleeper, RAGDOLL_LIMP );
			sleeper.secondsAtRest = 0.0f;
		}
	}
}

// Adds every living character near an awake ragdoll to the scene as an obstacle.
static void AddLivingCharacterObstacles( void )
{
	Vector3 awakeMinimum( 1.0e9f, 1.0e9f, 1.0e9f );
	Vector3 awakeMaximum( -1.0e9f, -1.0e9f, -1.0e9f );
	bool anyAwake = false;
	for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
	{
		const RagdollInstance &instance = ragdollInstances[instanceIndex];
		if ( !IsSimulated( instance ) )
		{
			continue;
		}
		Vector3 instanceMinimum, instanceMaximum;
		InstanceBounds( instance, OBSTACLE_SEARCH_DISTANCE, instanceMinimum, instanceMaximum );
		awakeMinimum = Vector3( fminf( awakeMinimum.x, instanceMinimum.x ), fminf( awakeMinimum.y, instanceMinimum.y ), fminf( awakeMinimum.z, instanceMinimum.z ) );
		awakeMaximum = Vector3( fmaxf( awakeMaximum.x, instanceMaximum.x ), fmaxf( awakeMaximum.y, instanceMaximum.y ), fmaxf( awakeMaximum.z, instanceMaximum.z ) );
		anyAwake = true;
	}
	if ( !anyAwake )
	{
		return;
	}

	for ( int characterIndex = 0; characterIndex < livingCharacterCount; characterIndex++ )
	{
		if ( BoundsOverlap( awakeMinimum, awakeMaximum, livingCharacterMinimum[characterIndex], livingCharacterMaximum[characterIndex] ) )
		{
			ragdollScene.AddObstacle( livingCharacterMinimum[characterIndex], livingCharacterMaximum[characterIndex],
									  livingCharacterSpeed[characterIndex] );
		}
	}
}

// Slows every body of a limp ragdoll, more strongly the longer it has been limp, so it settles.
static void ApplySettlingDamping( RagdollInstance &instance )
{
	const float settlingProgress = ragdoll::Clamp(
		( instance.secondsInState - SECONDS_LIMP_BEFORE_SETTLING ) / SECONDS_TO_FULL_SETTLING, 0.0f, 1.0f );
	if ( settlingProgress <= 0.0f )
	{
		return;
	}
	const float keep = expf( -FULL_SETTLING_DAMPING * settlingProgress * PHYSICS_STEP_SECONDS );
	for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
	{
		RigidBody &body = instance.simulation.bodies[bodyIndex];
		body.linearVelocity *= keep;
		body.angularVelocity *= keep;
	}
}

// How much of a stumble's lean each body takes: none for the hips and legs (which step under the
// body), rising up the spine, and all of it from the chest up (head and arms lean with the chest).
static float StumbleLeanShare( int bodyIndex )
{
	if ( IsLowerBody( bodyIndex ) )
	{
		return 0.0f;
	}
	switch ( bodyIndex )
	{
	case ragdoll::BODY_LOWER_SPINE:
		return 0.35f;
	case ragdoll::BODY_MIDDLE_SPINE:
		return 0.7f;
	default:
		return 1.0f;
	}
}

// The bodies of the two feet, in the order used for the stepping data: right, then left.
static const int FOOT_BODIES[2] = { ragdoll::BODY_RIGHT_FOOT, ragdoll::BODY_LEFT_FOOT };

// Returns the body's centre of mass and updates its smoothed velocity. The velocity is measured from
// one physics step to the next, so it includes the body being carried along with the character.
static Vector3 MeasureCentreOfMass( RagdollInstance &instance, float totalMass )
{
	Vector3 centreOfMass( 0.0f, 0.0f, 0.0f );
	for ( int bodyIndex = 0; bodyIndex < instance.simulation.bodyCount; bodyIndex++ )
	{
		centreOfMass += instance.simulation.bodies[bodyIndex].position * instance.rig.bodyMass[bodyIndex];
	}
	centreOfMass = centreOfMass / totalMass;

	if ( instance.hasPreviousCentreOfMass )
	{
		const Vector3 measuredVelocity = ( centreOfMass - instance.previousCentreOfMass ) / PHYSICS_STEP_SECONDS;
		instance.centreOfMassVelocity += ( measuredVelocity - instance.centreOfMassVelocity ) * CENTRE_OF_MASS_VELOCITY_SMOOTHING;
	}
	else
	{
		instance.centreOfMassVelocity = Vector3( 0.0f, 0.0f, 0.0f );
	}
	instance.previousCentreOfMass = centreOfMass;
	instance.hasPreviousCentreOfMass = true;
	return centreOfMass;
}

// Euphoria's arm windmilling: turns the arms' target orientations so they are raised out to the
// sides and swing in circles (the two arms half a circle apart), as people flail to keep their
// balance. amount (0 to 1) blends from the animation's arm pose to full windmilling. The forearms
// and hands turn with the upper arms, so the elbows and wrists keep the animation's bend.
static void WindmillArms( const RagdollInstance &instance, Quaternion *targetRotations, float amount )
{
	if ( amount <= 0.0f )
	{
		return;
	}
	const ragdoll::Rig &rig = instance.rig;
	const Vector3 forward = ragdoll::Normalized( HorizontalPart(
		ragdoll::Rotate( targetRotations[ragdoll::BODY_CHEST], rig.forwardAxisInBodySpace[ragdoll::BODY_CHEST] ) ) );
	if ( ragdoll::LengthSquared( forward ) < ragdoll::SMALL_NUMBER )
	{
		return;		// the chest faces straight up or down; there is no sideways to raise the arms toward
	}
	const Vector3 left = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, forward ) );
	const float circleAngle = 2.0f * ragdoll::PI_RADIANS * WINDMILL_CIRCLES_PER_SECOND * instance.secondsSinceStart;
	const float raiseRadians = ragdoll::DegreesToRadians( WINDMILL_RAISE_DEGREES );
	const float circleRadians = ragdoll::DegreesToRadians( WINDMILL_CIRCLE_DEGREES );

	for ( int side = 0; side < 2; side++ )
	{
		const bool isRightArm = ( side == 0 );
		const int upperArm = isRightArm ? ragdoll::BODY_RIGHT_UPPER_ARM : ragdoll::BODY_LEFT_UPPER_ARM;
		const int forearm = isRightArm ? ragdoll::BODY_RIGHT_FOREARM : ragdoll::BODY_LEFT_FOREARM;
		const int hand = isRightArm ? ragdoll::BODY_RIGHT_HAND : ragdoll::BODY_LEFT_HAND;
		const Vector3 outward = isRightArm ? left * -1.0f : left;
		const float phase = circleAngle + ( isRightArm ? 0.0f : ragdoll::PI_RADIANS );

		// Where the upper arm should point: raised out to the side, circling forward and back.
		const float raise = raiseRadians + circleRadians * cosf( phase );
		Vector3 desiredDirection = WORLD_UP * -cosf( raise ) + outward * sinf( raise );
		desiredDirection = ragdoll::Rotate( ragdoll::QuaternionFromAxisAngle( outward, circleRadians * sinf( phase ) ), desiredDirection );

		// Where the animation points it (from the shoulder end of the capsule to the elbow end).
		const RigidBody &upperArmBody = instance.simulation.bodies[upperArm];
		const Vector3 armInBodySpace = ragdoll::Normalized( upperArmBody.spheres[1].localCentre - upperArmBody.spheres[0].localCentre );
		const Vector3 animatedDirection = ragdoll::Rotate( targetRotations[upperArm], armInBodySpace );

		const Quaternion turn = ragdoll::Slerp( Quaternion(), ragdoll::ShortestArc( animatedDirection, desiredDirection ), amount );		const int armBodies[3] = { upperArm, forearm, hand };
		for ( int armSlot = 0; armSlot < 3; armSlot++ )
		{
			targetRotations[armBodies[armSlot]] = ragdoll::NormalizedQuaternion( turn * targetRotations[armBodies[armSlot]] );
		}
	}
}

/*
==================
Ground under the feet
==================
*/

// Finds the ground under a point, searching from searchUp above it to searchDown below it. Returns
// false if there is no ground in that range, or if the search starts inside something solid (a wall,
// or a crate too tall to step onto).
static bool FindGroundBelow( int ignoreEntityNumber, const Vector3 &point, float searchUp, float searchDown, float &groundHeight )
{
	vec3_t start = { point.x, point.y, point.z + searchUp };
	vec3_t end = { point.x, point.y, point.z - searchDown };
	vec3_t boxMinimum = { -2.0f, -2.0f, 0.0f };
	vec3_t boxMaximum = { 2.0f, 2.0f, 1.0f };
	trace_t trace;
	gi.trace( &trace, start, boxMinimum, boxMaximum, end, ignoreEntityNumber, MASK_SOLID, G2_NOCOLLIDE, 0 );
	if ( trace.startsolid || trace.allsolid || trace.fraction >= 1.0f )
	{
		return false;
	}
	groundHeight = trace.endpos[2];
	return true;
}

/*
==================
StepToKeepBalance

Euphoria's balancer, simplified: while a character is staggering, its feet are placed by this
instead of an animation. Each physics step the capture point is worked out: the spot on the ground
the body's centre of mass is heading for (where it is plus its speed times the time a body of its
height takes to topple). While the capture point stays between the feet the character stands its
ground. Once it drifts out, the foot farther from it steps there in a short lifted arc, landing a
little to its own side so the feet stay apart, and the feet take turns for as long as the body keeps
moving. Feet are always put down on real ground.

A capture point beyond the reach of a single step cannot be caught, and neither can a step into a
drop or onto something too tall: the character has misstepped (or tripped) and the function returns
false (the caller then lets it fall as a ragdoll). pelvisReference is the orientation the feet keep
their angle to. While staggering, steps land a little short so the body carries on and travels;
otherwise they land right where the body is going and stop it. footTargets and footRotations
receive where each foot (right, left) should be.
==================
*/
static bool StepToKeepBalance( RagdollInstance &instance, const Vector3 &centreOfMass, const Quaternion &pelvisReference,
							   bool staggering, Vector3 *footTargets, Quaternion *footRotations )
{
	const Simulation &simulation = instance.simulation;
	const float modelScale = instance.rig.modelScale;
	const float footHeight = FOOT_HEIGHT * modelScale;

	if ( !instance.isStepping )
	{
		// Start from where the feet are right now, put down on the ground under them (a running
		// character has one in the air), keeping their angle to the hips.
		const RigidBody &pelvis = simulation.bodies[ragdoll::BODY_PELVIS];
		for ( int foot = 0; foot < 2; foot++ )
		{
			const RigidBody &footBody = simulation.bodies[FOOT_BODIES[foot]];
			instance.plantedFootPositions[foot] = footBody.position;
			float groundHeight;
			if ( FindGroundBelow( instance.entityNumber, footBody.position, 8.0f * modelScale, 40.0f * modelScale, groundHeight ) )
			{
				instance.plantedFootPositions[foot].z = groundHeight + footHeight;
			}
			instance.footRotationFromPelvis[foot] = ragdoll::NormalizedQuaternion( ragdoll::Conjugate( pelvis.orientation ) * footBody.orientation );
		}
		instance.swingingFoot = -1;
		instance.lastSteppedFoot = -1;
		instance.isStepping = true;
	}

	// A planted foot that the hips have left behind is dragged after them (skidding), rather than
	// pulling the travelling body back through the leg.
	const Vector3 &hips = simulation.bodies[ragdoll::BODY_PELVIS].position;
	for ( int foot = 0; foot < 2; foot++ )
	{
		if ( foot == instance.swingingFoot )
		{
			continue;
		}
		Vector3 &plantedFoot = instance.plantedFootPositions[foot];
		const Vector3 fromHips = HorizontalPart( plantedFoot - hips );
		const float distance = ragdoll::Length( fromHips );
		const float reach = STANDING_LEG_REACH * modelScale;
		if ( distance > reach )
		{
			const Vector3 dragged = fromHips * ( reach / distance );
			plantedFoot = Vector3( hips.x + dragged.x, hips.y + dragged.y, plantedFoot.z );
		}
	}

	const Vector3 *planted = instance.plantedFootPositions;
	const float groundHeight = ( planted[0].z < planted[1].z ) ? planted[0].z : planted[1].z;
	const float heightAboveFeet = ( centreOfMass.z - groundHeight > 10.0f * modelScale ) ? centreOfMass.z - groundHeight : 10.0f * modelScale;
	const float toppleSeconds = sqrtf( heightAboveFeet / ( g_gravity->value > 1.0f ? g_gravity->value : 1.0f ) );
	const Vector3 capturePoint = HorizontalPart( centreOfMass + instance.centreOfMassVelocity * toppleSeconds );

	bool balanceCaught = true;
	if ( instance.swingingFoot < 0 )
	{
		const Vector3 betweenFeet = HorizontalPart( ( planted[0] + planted[1] ) * 0.5f );
		if ( ragdoll::Length( capturePoint - betweenFeet ) > STEP_TRIGGER_DISTANCE * modelScale )
		{
			// Step with the foot that is farther from where the body is going, but let the feet take
			// turns as people do: the same foot steps twice only if the other is already close.
			const float rightFootDistance = ragdoll::Length( HorizontalPart( planted[0] ) - capturePoint );
			const float leftFootDistance = ragdoll::Length( HorizontalPart( planted[1] ) - capturePoint );
			int swingFoot = ( rightFootDistance >= leftFootDistance ) ? 0 : 1;
			if ( swingFoot == instance.lastSteppedFoot )
			{
				const float swingDistance = ( swingFoot == 0 ) ? rightFootDistance : leftFootDistance;
				const float otherDistance = ( swingFoot == 0 ) ? leftFootDistance : rightFootDistance;
				if ( otherDistance >= ALTERNATE_STEP_SHARE * swingDistance )
				{
					swingFoot = 1 - swingFoot;
				}
			}
			instance.lastSteppedFoot = swingFoot;
			const Vector3 &standingFoot = planted[1 - swingFoot];

			// Land a little to the stepping foot's own side of the fall line, so the feet stay apart.
			const Vector3 fallDirection = ragdoll::Normalized( HorizontalPart( instance.centreOfMassVelocity ) );
			Vector3 sideways = HorizontalPart( planted[swingFoot] - standingFoot );
			if ( ragdoll::LengthSquared( fallDirection ) > ragdoll::SMALL_NUMBER )
			{
				sideways = ragdoll::RemoveComponent( sideways, fallDirection );
			}
			const Vector3 landingPoint = capturePoint + ragdoll::Normalized( sideways ) * ( STANCE_HALF_WIDTH * modelScale );

			// No step can be longer than a leg allows. Far beyond that, the body cannot be caught.
			Vector3 step = HorizontalPart( landingPoint - standingFoot );
			const float stepLength = ragdoll::Length( step );
			const float longestStep = STEP_LONGEST * modelScale;
			const char *missReason = "";
			if ( stepLength > longestStep + MISSTEP_EXTRA_DISTANCE * modelScale )
			{
				balanceCaught = false;
				missReason = ", out of reach";
			}

			// While staggering, the foot lands most of the way there, so the moving body carries on past
			// it and the character travels, step after step. Otherwise it lands right where the body is
			// going and stops it (landing short would leave the body creeping on).
			if ( staggering )
			{
				step = step * STEP_REACH_SHARE;
			}
			const float reachedLength = ragdoll::Length( step );
			if ( reachedLength > longestStep )
			{
				step = step * ( longestStep / reachedLength );
			}

			// The foot must land on ground: not into a drop, not onto something too tall to step onto.
			Vector3 landing( standingFoot.x + step.x, standingFoot.y + step.y, standingFoot.z );
			const float standingGround = standingFoot.z - footHeight;
			float landingGround;
			if ( FindGroundBelow( instance.entityNumber, Vector3( landing.x, landing.y, standingGround ),
								  STEP_UP_LIMIT * modelScale, STEP_DOWN_LIMIT * modelScale, landingGround ) )
			{
				landing.z = landingGround + footHeight;
			}
			else
			{
				balanceCaught = false;
				missReason = ", no ground to land on";
			}

			instance.swingingFoot = swingFoot;
			instance.swingSeconds = 0.0f;
			instance.swingStartPosition = planted[swingFoot];
			instance.swingTargetPosition = landing;

			if ( g_ragdollDebug->integer )
			{
				gi.Printf( "ragdoll %d: steps with the %s foot (%.0f units from the standing foot%s)\n", instance.entityNumber,
						   swingFoot == 0 ? "right" : "left", ragdoll::Length( step ), missReason );
			}
		}
	}
	else
	{
		instance.swingSeconds += PHYSICS_STEP_SECONDS;
		if ( instance.swingSeconds >= STEP_SECONDS )
		{
			instance.plantedFootPositions[instance.swingingFoot] = instance.swingTargetPosition;	// the foot lands
			instance.swingingFoot = -1;
		}
	}

	for ( int foot = 0; foot < 2; foot++ )
	{
		footTargets[foot] = instance.plantedFootPositions[foot];
		if ( foot == instance.swingingFoot )
		{
			const float progress = ragdoll::Clamp( instance.swingSeconds / STEP_SECONDS, 0.0f, 1.0f );
			const float eased = progress * progress * ( 3.0f - 2.0f * progress );
			footTargets[foot] = instance.swingStartPosition + ( instance.swingTargetPosition - instance.swingStartPosition ) * eased
				+ WORLD_UP * ( STEP_LIFT * modelScale * sinf( ragdoll::PI_RADIANS * progress ) );
		}
		footRotations[foot] = ragdoll::NormalizedQuaternion( pelvisReference * instance.footRotationFromPelvis[foot] );
	}
	return balanceCaught;
}

/*
==================
Hit reaction building blocks
==================
*/

// Tilts the upper body's target orientations toward the stagger direction (rotating about this
// axis tips "up" toward the lean direction).
static void ApplyStumbleLean( Quaternion *targetRotations, const Vector3 &leanDirection, float leanRadians )
{
	const Vector3 leanAxis = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, leanDirection ) );
	if ( leanRadians <= 0.0f || ragdoll::LengthSquared( leanAxis ) < ragdoll::SMALL_NUMBER )
	{
		return;
	}
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const float leanShare = StumbleLeanShare( bodyIndex );
		if ( leanShare > 0.0f )
		{
			targetRotations[bodyIndex] = ragdoll::NormalizedQuaternion(
				ragdoll::QuaternionFromAxisAngle( leanAxis, leanRadians * leanShare ) * targetRotations[bodyIndex] );
		}
	}
}

/*
==================
ApplyFlinch

Euphoria's flinch toward the wound: turns the target orientations so the spine curls toward where
the hit landed (doubling over a hit from the front, arching away from one in the back), the head
dips a little further than the chest, and the wounded side turns away from the shot. The hips and
legs are left to the balance controller. See StartFlinch for how big each flinch is.
==================
*/
static void ApplyFlinch( const RagdollInstance &instance, Quaternion *targetRotations )
{
	const float amount = FlinchAmount( instance );
	if ( amount <= 0.0f || instance.flinchRadians <= 0.0f )
	{
		return;
	}

	// The wounded side turns away from the shot: the chest and everything above fully, the spine partly.
	if ( instance.flinchTwistRadians != 0.0f )
	{
		for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
		{
			const float share = StumbleLeanShare( bodyIndex );
			if ( share > 0.0f )
			{
				targetRotations[bodyIndex] = ragdoll::NormalizedQuaternion(
					ragdoll::QuaternionFromAxisAngle( WORLD_UP, instance.flinchTwistRadians * amount * share ) * targetRotations[bodyIndex] );
			}
		}
	}

	// The spine curls toward the wound (rotating about this axis tips "up" toward the wound), and the
	// head goes a little further.
	const Vector3 curlAxis = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, instance.flinchDirection ) );
	if ( ragdoll::LengthSquared( curlAxis ) < ragdoll::SMALL_NUMBER )
	{
		return;
	}
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		float share = StumbleLeanShare( bodyIndex );
		if ( bodyIndex == ragdoll::BODY_NECK )
		{
			share *= 1.3f;
		}
		else if ( bodyIndex == ragdoll::BODY_HEAD )
		{
			share *= 1.5f;
		}
		if ( share > 0.0f )
		{
			targetRotations[bodyIndex] = ragdoll::NormalizedQuaternion(
				ragdoll::QuaternionFromAxisAngle( curlAxis, instance.flinchRadians * amount * share ) * targetRotations[bodyIndex] );
		}
	}
}

// The bodies of one arm: upper arm, forearm and hand. hand is 0 for the right arm, 1 for the left.
static void ArmBodies( int hand, int &upperArm, int &forearm, int &handBody )
{
	upperArm = ( hand == 0 ) ? ragdoll::BODY_RIGHT_UPPER_ARM : ragdoll::BODY_LEFT_UPPER_ARM;
	forearm = ( hand == 0 ) ? ragdoll::BODY_RIGHT_FOREARM : ragdoll::BODY_LEFT_FOREARM;
	handBody = ( hand == 0 ) ? ragdoll::BODY_RIGHT_HAND : ragdoll::BODY_LEFT_HAND;
}

// True if the body belongs to the arm that is clutching a wound right now.
static bool IsClutchingArmBody( const RagdollInstance &instance, int bodyIndex )
{
	if ( instance.clutchingHand < 0 || instance.clutchSecondsLeft <= 0.0f )
	{
		return false;
	}
	int upperArm, forearm, handBody;
	ArmBodies( instance.clutchingHand, upperArm, forearm, handBody );
	return bodyIndex == upperArm || bodyIndex == forearm || bodyIndex == handBody;
}

/*
==================
ReachForWound

Euphoria's hand on the wound: turns the clutching arm's target orientations so the hand goes to
where the character was hit and stays on it as the body moves. The arm is solved as two segments
from the shoulder (upper arm, then forearm plus most of the hand), with the elbow bent down and out
to the side; the hand reaches in quickly, holds, and lets go gently.
==================
*/
static void ReachForWound( const RagdollInstance &instance, Quaternion *targetRotations )
{
	if ( instance.clutchingHand < 0 || instance.clutchSecondsLeft <= 0.0f )
	{
		return;
	}
	const float secondsClutching = CLUTCH_SECONDS - instance.clutchSecondsLeft;
	const float amount = ragdoll::Clamp( secondsClutching / CLUTCH_REACH_SECONDS, 0.0f, 1.0f )
		* ragdoll::Clamp( instance.clutchSecondsLeft / CLUTCH_RELEASE_SECONDS, 0.0f, 1.0f );
	if ( amount <= 0.0f )
	{
		return;
	}

	const Simulation &simulation = instance.simulation;
	const float modelScale = instance.rig.modelScale;
	int upperArm, forearm, handBody;
	ArmBodies( instance.clutchingHand, upperArm, forearm, handBody );
	const RigidBody &upperArmBody = simulation.bodies[upperArm];
	const RigidBody &forearmBody = simulation.bodies[forearm];
	const RigidBody &handRigidBody = simulation.bodies[handBody];
	const RigidBody &woundedBody = simulation.bodies[instance.woundBody];

	// Where the hand goes: on the wound, resting on the surface rather than inside the body.
	const Vector3 woundPoint = woundedBody.LocalToWorldPoint( instance.woundInBodySpace );
	const Vector3 outFromBody = ragdoll::Normalized( woundPoint - woundedBody.position );
	const Vector3 handTarget = woundPoint + outFromBody * ( CLUTCH_HAND_OFFSET * modelScale );

	const Vector3 shoulder = upperArmBody.LocalToWorldPoint( upperArmBody.spheres[0].localCentre );
	const Vector3 upperArmAxis = upperArmBody.spheres[1].localCentre - upperArmBody.spheres[0].localCentre;
	const Vector3 forearmAxis = forearmBody.spheres[1].localCentre - forearmBody.spheres[0].localCentre;
	const float upperArmLength = ragdoll::Length( upperArmAxis );
	const float lowerArmLength = ragdoll::Length( forearmAxis )
		+ 0.6f * ragdoll::Length( handRigidBody.spheres[1].localCentre - handRigidBody.spheres[0].localCentre );
	const Vector3 toTarget = handTarget - shoulder;
	const float targetDistance = ragdoll::Length( toTarget );
	if ( targetDistance < ragdoll::SMALL_NUMBER || upperArmLength < ragdoll::SMALL_NUMBER || lowerArmLength < ragdoll::SMALL_NUMBER )
	{
		return;
	}
	const Vector3 reachDirection = toTarget / targetDistance;
	const float fullReach = upperArmLength + lowerArmLength;
	const float reach = ragdoll::Clamp( targetDistance, 0.35f * fullReach, 0.98f * fullReach );

	// The elbow, by the law of cosines, bent down and out to the side.
	const float alongReach = ( upperArmLength * upperArmLength - lowerArmLength * lowerArmLength + reach * reach ) / ( 2.0f * reach );
	const float elbowOffsetSquared = upperArmLength * upperArmLength - alongReach * alongReach;
	const float elbowOffset = ( elbowOffsetSquared > 0.0f ) ? sqrtf( elbowOffsetSquared ) : 0.0f;
	const Vector3 left = ragdoll::Normalized( ragdoll::Rotate( simulation.bodies[ragdoll::BODY_CHEST].orientation,
															   instance.rig.lateralAxisInBodySpace[ragdoll::BODY_CHEST] ) );
	const Vector3 outward = ( instance.clutchingHand == 0 ) ? left * -1.0f : left;
	Vector3 bendDirection = ragdoll::RemoveComponent( outward * 0.6f - WORLD_UP, reachDirection );
	if ( ragdoll::LengthSquared( bendDirection ) < ragdoll::SMALL_NUMBER )
	{
		bendDirection = ragdoll::AnyPerpendicular( reachDirection );
	}
	bendDirection = ragdoll::Normalized( bendDirection );
	const Vector3 elbow = shoulder + reachDirection * alongReach + bendDirection * elbowOffset;
	const Vector3 wrist = shoulder + reachDirection * reach;

	// Turn the arm's targets into that shape; each segment carries the ones below it.
	const Quaternion upperArmTurn = ragdoll::Slerp( Quaternion(), ragdoll::ShortestArc(
		ragdoll::Rotate( targetRotations[upperArm], ragdoll::Normalized( upperArmAxis ) ), ragdoll::Normalized( elbow - shoulder ) ), amount );
	targetRotations[upperArm] = ragdoll::NormalizedQuaternion( upperArmTurn * targetRotations[upperArm] );
	targetRotations[forearm] = ragdoll::NormalizedQuaternion( upperArmTurn * targetRotations[forearm] );
	targetRotations[handBody] = ragdoll::NormalizedQuaternion( upperArmTurn * targetRotations[handBody] );
	const Quaternion forearmTurn = ragdoll::Slerp( Quaternion(), ragdoll::ShortestArc(
		ragdoll::Rotate( targetRotations[forearm], ragdoll::Normalized( forearmAxis ) ), ragdoll::Normalized( wrist - elbow ) ), amount );
	targetRotations[forearm] = ragdoll::NormalizedQuaternion( forearmTurn * targetRotations[forearm] );
	targetRotations[handBody] = ragdoll::NormalizedQuaternion( forearmTurn * targetRotations[handBody] );
}

// Sets every joint's muscle toward the target orientations, with strengths for the situation: a
// flinch while following the animation (legs strongest, arms weakest so they fling), or tense
// muscles while a shot reaction has control. Stepping legs give, so they can reach the placed feet,
// and staggering legs are loose, so they swing under the travelling hips; windmilling (staggering)
// and clutching arms are strong, so they move instead of trailing.
static void SetHitReactionMuscles( RagdollInstance &instance, const Quaternion *targetRotations, bool staggering )
{
	const Simulation &simulation = instance.simulation;
	for ( int jointIndex = 0; jointIndex < HUMANOID_JOINT_COUNT; jointIndex++ )
	{
		const Joint &joint = simulation.joints[jointIndex];
		const Quaternion targetRelative = ragdoll::NormalizedQuaternion(
			ragdoll::Conjugate( targetRotations[joint.parentBodyIndex] ) * targetRotations[joint.childBodyIndex] );

		float naturalFrequency;
		switch ( ragdoll::MuscleGroupForBody( joint.childBodyIndex ) )
		{
		case ragdoll::MUSCLE_GROUP_SPINE:
			naturalFrequency = instance.isInControl ? CONTROL_SPINE_FREQUENCY : HIT_REACTION_SPINE_FREQUENCY;
			break;
		case ragdoll::MUSCLE_GROUP_NECK:
			naturalFrequency = instance.isInControl ? CONTROL_NECK_FREQUENCY : HIT_REACTION_NECK_FREQUENCY;
			break;
		case ragdoll::MUSCLE_GROUP_ARMS:
			if ( IsClutchingArmBody( instance, joint.childBodyIndex ) )
			{
				naturalFrequency = CLUTCH_ARM_FREQUENCY;
			}
			else if ( staggering )
			{
				naturalFrequency = WINDMILL_ARM_FREQUENCY;
			}
			else
			{
				naturalFrequency = instance.isInControl ? CONTROL_ARM_FREQUENCY : HIT_REACTION_ARM_FREQUENCY;
			}
			break;
		default:
			if ( staggering && instance.isInControl )
			{
				naturalFrequency = STAGGER_LEG_FREQUENCY;
			}
			else
			{
				naturalFrequency = instance.isStepping ? STEPPING_LEG_FREQUENCY : HIT_REACTION_LEG_FREQUENCY;
			}
			break;
		}
		SetMuscle( instance, jointIndex, targetRelative, naturalFrequency );
	}
}

// Holds each foot (right, left) on its spot and at its angle.
static void HoldFeet( RagdollInstance &instance, const Vector3 *footTargets, const Quaternion *footRotations, float totalMass )
{
	for ( int foot = 0; foot < 2; foot++ )
	{
		RigidBody &footBody = instance.simulation.bodies[FOOT_BODIES[foot]];
		footBody.positionDrive.enabled = true;
		footBody.positionDrive.targetPosition = footTargets[foot];
		footBody.positionDrive.stiffness = totalMass * FOOT_PLANT_FREQUENCY * FOOT_PLANT_FREQUENCY * FOOT_PLANT_WEIGHT_SHARE;
		footBody.positionDrive.damping = HIT_REACTION_PELVIS_DAMPING;
		footBody.positionDrive.horizontalShare = 1.0f;
		footBody.worldDrive.targetOrientation = footRotations[foot];
		footBody.worldDrive.naturalFrequency = FOOT_PLANT_FREQUENCY;
		footBody.worldDrive.dampingRatio = 1.0f;
	}
}

/*
==================
FollowAnimation

A hit reaction that leaves the character in charge (the player's flinches, and an NPC blending back
into its animation after a shot reaction): carries the body along with the character's own movement
and pulls every part toward where the animation has it, so only the jolt from the hit is left to
the physics.
==================
*/
static void FollowAnimation( RagdollInstance &instance, bool stumbling, const Vector3 &leanDirection, float leanRadians )
{
	Simulation &simulation = instance.simulation;
	const Transform &pelvisTarget = instance.animationBodyTargets[ragdoll::BODY_PELVIS];

	// Move and turn the whole body with the animated pelvis, so a running or turning character does
	// not leave its body behind.
	if ( instance.hasPreviousPelvisTarget )
	{
		const Quaternion turn = ragdoll::NormalizedQuaternion( pelvisTarget.rotation * ragdoll::Conjugate( instance.previousPelvisTarget.rotation ) );
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			RigidBody &body = simulation.bodies[bodyIndex];
			const Vector3 offsetFromPelvis = body.position - instance.previousPelvisTarget.position;
			body.position = pelvisTarget.position + ragdoll::Rotate( turn, offsetFromPelvis );
			body.orientation = ragdoll::NormalizedQuaternion( turn * body.orientation );
		}
	}
	instance.previousPelvisTarget = pelvisTarget;
	instance.hasPreviousPelvisTarget = true;

	Quaternion targetRotations[HUMANOID_BODY_COUNT];
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		targetRotations[bodyIndex] = instance.animationBodyTargets[bodyIndex].rotation;
	}
	ApplyStumbleLean( targetRotations, leanDirection, leanRadians );
	if ( stumbling )
	{
		WindmillArms( instance, targetRotations,
					  ragdoll::Clamp( leanRadians / ragdoll::DegreesToRadians( WINDMILL_FULL_AT_LEAN_DEGREES ), 0.0f, 1.0f ) );
	}
	SetHitReactionMuscles( instance, targetRotations, stumbling );

	// The pelvis is held to the animated pelvis, in position and orientation.
	const float totalMass = TotalRagdollMass( instance );
	RigidBody &pelvis = simulation.bodies[ragdoll::BODY_PELVIS];
	pelvis.positionDrive.enabled = true;
	pelvis.positionDrive.targetPosition = pelvisTarget.position;
	pelvis.positionDrive.stiffness = totalMass * HIT_REACTION_PELVIS_FREQUENCY * HIT_REACTION_PELVIS_FREQUENCY;
	pelvis.positionDrive.damping = HIT_REACTION_PELVIS_DAMPING;
	pelvis.positionDrive.horizontalShare = 1.0f;
	pelvis.worldDrive.targetOrientation = pelvisTarget.rotation;
	pelvis.worldDrive.naturalFrequency = HIT_REACTION_PELVIS_FREQUENCY;
	pelvis.worldDrive.dampingRatio = 1.0f;
	simulation.bodies[ragdoll::BODY_CHEST].worldDrive.naturalFrequency = 0.0f;

	// The feet stay where the animation has them (or step, if somehow staggering while following).
	const Vector3 centreOfMass = MeasureCentreOfMass( instance, totalMass );
	Vector3 footTargets[2];
	Quaternion footRotations[2];
	if ( stumbling )
	{
		if ( !StepToKeepBalance( instance, centreOfMass, pelvisTarget.rotation, stumbling, footTargets, footRotations ) )
		{
			ragdoll::LoseFooting( instance.entityNumber, HorizontalPart( instance.centreOfMassVelocity ) );
		}
	}
	else
	{
		instance.isStepping = false;
		for ( int foot = 0; foot < 2; foot++ )
		{
			footTargets[foot] = instance.animationBodyTargets[FOOT_BODIES[foot]].position;
			footRotations[foot] = instance.animationBodyTargets[FOOT_BODIES[foot]].rotation;
		}
	}
	HoldFeet( instance, footTargets, footRotations, totalMass );
}

// The body has its balance back: the character faces the way its hips now face, the AI carries on,
// and the drawn pose blends from the body back into the animation (see PoseHitReaction).
static void RecoverControl( RagdollInstance &instance, gentity_t *entity )
{
	if ( entity && entity->inuse && entity->client )
	{
		const RigidBody &pelvis = instance.simulation.bodies[ragdoll::BODY_PELVIS];
		const Vector3 hipsForward = HorizontalPart(
			ragdoll::Rotate( pelvis.orientation, instance.rig.forwardAxisInBodySpace[ragdoll::BODY_PELVIS] ) );
		if ( ragdoll::Length( hipsForward ) > 0.1f )
		{
			const float yawDegrees = ragdoll::RadiansToDegrees( atan2f( hipsForward.y, hipsForward.x ) );
			entity->client->ps.viewangles[YAW] = yawDegrees;
			entity->currentAngles[YAW] = yawDegrees;
			if ( entity->NPC )
			{
				entity->NPC->desiredYaw = yawDegrees;
				entity->NPC->lockedDesiredYaw = yawDegrees;
			}
		}
	}
	instance.isInControl = false;
	instance.clutchSecondsLeft = 0.0f;
	instance.isStepping = false;
	instance.hasPreviousPelvisTarget = false;
	instance.secondsSinceLastHit = HIT_REACTION_HOLD_SECONDS;	// start blending back to the animation now
	instance.simulation.bodies[ragdoll::BODY_PELVIS].positionDrive.horizontalShare = 1.0f;

	if ( g_ragdollDebug->integer )
	{
		// How far the hit turned the body, for tuning: the hips' heading now against when the shot landed.
		const Vector3 &forwardInPelvis = instance.rig.forwardAxisInBodySpace[ragdoll::BODY_PELVIS];
		const Vector3 headingNow = HorizontalPart( ragdoll::Rotate( instance.simulation.bodies[ragdoll::BODY_PELVIS].orientation, forwardInPelvis ) );
		const Vector3 headingThen = HorizontalPart( ragdoll::Rotate( instance.reactionPelvisRotation, forwardInPelvis ) );
		const float turnedDegrees = ragdoll::RadiansToDegrees( atan2f( ragdoll::Cross( headingThen, headingNow ).z, ragdoll::Dot( headingThen, headingNow ) ) );
		const float travelled = ragdoll::Length( HorizontalPart( instance.simulation.bodies[ragdoll::BODY_PELVIS].position - instance.controlStartPosition ) );
		gi.Printf( "ragdoll %d: regains its balance after %.1f seconds, turned %.0f degrees, moved %.0f units (AI resumes)\n",
				   instance.entityNumber, instance.secondsInControl, turnedDegrees, travelled );
	}
}

/*
==================
ControlBodyAfterHit

A shot reaction in control of an NPC, in the style of GTA IV/V and Red Dead Redemption: the AI is
paused (see G_Ragdoll_IsControllingCharacter) and the body no longer follows an animation. Instead,
as Euphoria's behaviours do:
  * the muscles hold the posture the shot caught the character in, tense enough to take the jolt
    and come back, with gravity compensated so nothing sags;
  * the legs hold the hips up at standing height but let them be shoved sideways, and the balance
    controller steps the feet to catch the body;
  * a hard hit sends the whole body staggering (the balance module decides how hard), the upper body
    leans with it and the arms windmill, while the legs brake it;
  * the free hand clutches the wound.
Once the body is balanced and still again, control goes back to the animation and the AI carries
on. A body that no step can catch falls over (LoseFooting).
==================
*/
static void ControlBodyAfterHit( RagdollInstance &instance, bool stumbling, const Vector3 &leanDirection, float leanRadians )
{
	Simulation &simulation = instance.simulation;
	RigidBody &pelvis = simulation.bodies[ragdoll::BODY_PELVIS];
	const float totalMass = TotalRagdollMass( instance );
	const float modelScale = instance.rig.modelScale;
	instance.secondsInControl += PHYSICS_STEP_SECONDS;

	// The first step: remember the posture the shot caught the character in, relative to its hips.
	if ( !instance.hasReactionPose )
	{
		instance.reactionPelvisRotation = pelvis.orientation;
		for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
		{
			instance.reactionPoseFromPelvis[bodyIndex] = ragdoll::NormalizedQuaternion(
				ragdoll::Conjugate( pelvis.orientation ) * simulation.bodies[bodyIndex].orientation );
		}
		instance.hasReactionPose = true;
		instance.isStepping = false;			// the feet are put down on the ground again below
		instance.standingPelvisHeight = -1.0f;	// measured once they are
	}

	// A hard hit sends the whole body staggering; the legs brake it.
	Vector3 push;
	if ( ragdoll::TakeStumblePush( instance.entityNumber, push ) )
	{
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			simulation.bodies[bodyIndex].linearVelocity += push;
		}
	}
	const float braking = expf( -( stumbling ? STAGGER_BRAKING : SETTLE_BRAKING ) * PHYSICS_STEP_SECONDS );
	for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
	{
		Vector3 &velocity = simulation.bodies[bodyIndex].linearVelocity;
		velocity.x *= braking;
		velocity.y *= braking;
	}

	// Struggling for balance: while staggering, the legs fighting the body push it about, so the
	// stagger wanders across the ground instead of running in a straight line.
	if ( stumbling )
	{
		instance.wanderSecondsLeft -= PHYSICS_STEP_SECONDS;
		if ( instance.wanderSecondsLeft <= 0.0f )
		{
			const Vector3 travel = ragdoll::Normalized( HorizontalPart( leanDirection ) );
			const Vector3 sideways = ragdoll::Cross( WORLD_UP, travel );
			instance.wanderAcceleration = ( sideways * Q_flrand( -1.0f, 1.0f ) + travel * Q_flrand( -0.3f, 0.3f ) )
				* ( STAGGER_WANDER_ACCELERATION * modelScale );
			instance.wanderSecondsLeft = Q_flrand( STAGGER_WANDER_SHORTEST_SECONDS, STAGGER_WANDER_LONGEST_SECONDS );
		}
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			simulation.bodies[bodyIndex].linearVelocity += instance.wanderAcceleration * PHYSICS_STEP_SECONDS;
		}
	}

	// Upright hips facing the way they faced when the shot landed: a hit twists them for a moment,
	// and the legs turn them back (left free to turn, a hit on one side spins the body around).
	const Quaternion &uprightPelvis = instance.reactionPelvisRotation;

	// Staggering, the whole body tips the way it is going (the hips lead), so the feet keep having to
	// catch it and the character travels instead of stamping on the spot.
	Quaternion bodyTilt;
	const Vector3 tiltAxis = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, leanDirection ) );
	if ( stumbling && leanRadians > 0.0f && ragdoll::LengthSquared( tiltAxis ) > ragdoll::SMALL_NUMBER )
	{
		const float staggerStrength = ragdoll::Clamp( leanRadians / ragdoll::DegreesToRadians( WINDMILL_FULL_AT_LEAN_DEGREES ), 0.0f, 1.0f );
		bodyTilt = ragdoll::QuaternionFromAxisAngle( tiltAxis, ragdoll::DegreesToRadians( STAGGER_BODY_TILT_DEGREES ) * staggerStrength );
	}
	const Quaternion leaningPelvis = ragdoll::NormalizedQuaternion( bodyTilt * uprightPelvis );
	pelvis.worldDrive.targetOrientation = leaningPelvis;
	pelvis.worldDrive.naturalFrequency = UPRIGHT_PELVIS_FREQUENCY;
	pelvis.worldDrive.dampingRatio = 1.0f;
	simulation.bodies[ragdoll::BODY_CHEST].worldDrive.naturalFrequency = 0.0f;

	// Every body's target: the caught posture carried by the upright hips, curled toward the wound and hunched while staggering,
	// arms windmilling to keep balance, and the free hand on the wound.
	Quaternion targetRotations[HUMANOID_BODY_COUNT];
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		targetRotations[bodyIndex] = ragdoll::NormalizedQuaternion( leaningPelvis * instance.reactionPoseFromPelvis[bodyIndex] );
	}
	ApplyFlinch( instance, targetRotations );
	ApplyStumbleLean( targetRotations, leanDirection * -1.0f, leanRadians * STAGGER_HUNCH_SHARE );	// a little hunched toward the shots
	if ( stumbling )
	{
		WindmillArms( instance, targetRotations,
					  ragdoll::Clamp( leanRadians / ragdoll::DegreesToRadians( WINDMILL_FULL_AT_LEAN_DEGREES ), 0.0f, 1.0f ) );
	}
	ReachForWound( instance, targetRotations );
	if ( instance.clutchSecondsLeft > 0.0f )
	{
		instance.clutchSecondsLeft -= PHYSICS_STEP_SECONDS;
	}
	SetHitReactionMuscles( instance, targetRotations, stumbling );

	// The feet: placed by the balance controller, always on the ground.
	const Vector3 centreOfMass = MeasureCentreOfMass( instance, totalMass );
	Vector3 footTargets[2];
	Quaternion footRotations[2];
	if ( !StepToKeepBalance( instance, centreOfMass, uprightPelvis, stumbling, footTargets, footRotations ) )
	{
		ragdoll::LoseFooting( instance.entityNumber, HorizontalPart( instance.centreOfMassVelocity ) );
	}
	HoldFeet( instance, footTargets, footRotations, totalMass );

	// The hips: held up at standing height above the feet by the legs, which also pull them gently
	// back over the feet but let a hit shove them sideways.
	const Vector3 *planted = instance.plantedFootPositions;
	const float feetHeight = ( planted[0].z < planted[1].z ) ? planted[0].z : planted[1].z;
	if ( instance.standingPelvisHeight < 0.0f )
	{
		instance.standingPelvisHeight = pelvis.position.z - feetHeight;
	}
	const Vector3 betweenFeet = ( planted[0] + planted[1] ) * 0.5f;
	pelvis.positionDrive.enabled = true;
	const float kneesGive = instance.flinchHipDrop * FlinchAmount( instance ) * modelScale;	// the knees give a little when hit
	pelvis.positionDrive.targetPosition = Vector3( betweenFeet.x, betweenFeet.y, feetHeight + instance.standingPelvisHeight - kneesGive );
	pelvis.positionDrive.stiffness = totalMass * HIP_SUPPORT_FREQUENCY * HIP_SUPPORT_FREQUENCY;
	pelvis.positionDrive.damping = HIT_REACTION_PELVIS_DAMPING;
	pelvis.positionDrive.horizontalShare = stumbling ? 0.0f : HIP_SUPPORT_HORIZONTAL_SHARE;	// staggering, the feet chase the hips

	// Balanced and still again (or long enough regardless): the character gets its body back.
	const float horizontalSpeed = ragdoll::Length( HorizontalPart( instance.centreOfMassVelocity ) );
	const bool settled = instance.secondsSinceLastHit >= SHOT_REACTION_SHORTEST_SECONDS
		&& !stumbling
		&& instance.swingingFoot < 0
		&& horizontalSpeed < RECOVERED_SPEED * modelScale;
	if ( settled || instance.secondsInControl >= LONGEST_CONTROL_SECONDS )
	{
		RecoverControl( instance, &g_entities[instance.entityNumber] );
	}
}

// Each physics step of a hit reaction, whichever kind it is.
static void DriveHitReaction( RagdollInstance &instance )
{
	Vector3 leanDirection( 0.0f, 0.0f, 0.0f );
	float leanRadians = 0.0f;
	const bool stumbling = ragdoll::StumbleLean( instance.entityNumber, leanDirection, leanRadians );
	if ( stumbling )
	{
		instance.secondsSinceLastHit = 0.0f;	// stay physical for the whole stagger
	}
	else
	{
		instance.secondsSinceLastHit += PHYSICS_STEP_SECONDS;
	}
	instance.secondsSinceFlinch += PHYSICS_STEP_SECONDS;

	// Gravity compensation: a living character's muscles hold its body up, so the body reacts to
	// hits without sagging. Gravity comes back if the character dies or falls (StopFollowingAnimation).
	Simulation &simulation = instance.simulation;
	for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
	{
		simulation.bodies[bodyIndex].gravityScale = 0.0f;
	}

	if ( instance.isInControl )
	{
		ControlBodyAfterHit( instance, stumbling, leanDirection, leanRadians );
	}
	else if ( instance.hasAnimationTargets )
	{
		FollowAnimation( instance, stumbling, leanDirection, leanRadians );
	}

	// Keep a big hit from spinning a light limb wildly.
	for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
	{
		RigidBody &body = simulation.bodies[bodyIndex];
		const float spin = ragdoll::Length( body.angularVelocity );
		if ( spin > HIT_REACTION_LONGEST_SPIN )
		{
			body.angularVelocity *= HIT_REACTION_LONGEST_SPIN / spin;
		}
	}
}

static void StepAllRagdolls( int renderTime )
{
	if ( renderTime == lastSceneRenderTime )
	{
		return;		// already stepped this frame
	}

	float frameSeconds = ( lastSceneRenderTime < 0 ) ? 0.0f : ( renderTime - lastSceneRenderTime ) / 1000.0f;
	lastSceneRenderTime = renderTime;
	if ( frameSeconds < 0.0f )
	{
		frameSeconds = 0.0f;	// time went backwards (for example after loading)
	}
	if ( frameSeconds > LONGEST_FRAME_SECONDS )
	{
		frameSeconds = LONGEST_FRAME_SECONDS;
	}

	SimulationSettings settings;
	settings.gravity = Vector3( 0.0f, 0.0f, -g_gravity->value );
	settings.substepCount = PHYSICS_SUBSTEPS;
	settings.staticFriction = 0.8f;
	settings.dynamicFriction = 0.6f;
	settings.restitution = 0.1f;
	settings.contactMargin = 1.5f;
	settings.linearDamping = 0.05f;
	settings.angularDamping = 0.3f;
	settings.maximumLinearSpeed = 1500.0f;
	settings.maximumAngularSpeed = 40.0f;

	sceneUnsimulatedSeconds += frameSeconds;
	while ( sceneUnsimulatedSeconds >= PHYSICS_STEP_SECONDS )
	{
		sceneUnsimulatedSeconds -= PHYSICS_STEP_SECONDS;
		GatherLivingCharacters();
		WakeDisturbedRagdolls();

		ragdollScene.Clear();
		for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
		{
			RagdollInstance &instance = ragdollInstances[instanceIndex];
			if ( !IsSimulated( instance ) )
			{
				continue;
			}

			for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
			{
				const RigidBody &body = instance.simulation.bodies[bodyIndex];
				instance.previousBodyTransforms[bodyIndex] = Transform( body.orientation, body.position );
			}

			instance.secondsInState += PHYSICS_STEP_SECONDS;
			instance.secondsSinceStart += PHYSICS_STEP_SECONDS;
			if ( instance.isHitReaction )
			{
				DriveHitReaction( instance );
			}
			else
			{
				UpdateForceGrip( instance );
				UpdateBehaviourState( instance );
				ApplyBehaviourMuscles( instance );
			}

			ragdollScene.AddSimulation( &instance.simulation, SweepSphereThroughWorld, &instance );
		}
		AddLivingCharacterObstacles();
		ragdollScene.Step( PHYSICS_STEP_SECONDS, settings );

		for ( int instanceIndex = 0; instanceIndex < MAXIMUM_RAGDOLLS; instanceIndex++ )
		{
			if ( ragdollInstances[instanceIndex].inUse && ragdollInstances[instanceIndex].state == RAGDOLL_LIMP )
			{
				ApplySettlingDamping( ragdollInstances[instanceIndex] );
			}
		}
	}
}

// The body transforms to draw, smoothed between the last two physics steps.
static void InterpolatedBodyTransforms( const RagdollInstance &instance, Transform *bodyTransforms )
{
	const float fraction = ( instance.state == RAGDOLL_ASLEEP ) ? 1.0f
		: ragdoll::Clamp( sceneUnsimulatedSeconds / PHYSICS_STEP_SECONDS, 0.0f, 1.0f );

	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const RigidBody &body = instance.simulation.bodies[bodyIndex];
		const Transform &previous = instance.previousBodyTransforms[bodyIndex];
		bodyTransforms[bodyIndex] = Transform(
			ragdoll::Slerp( previous.rotation, body.orientation, fraction ),
			previous.position + ( body.position - previous.position ) * fraction );
	}
}

// Extra room around the physics bodies when stretching the collision box, because the visible model
// (armour, helmets, hands) is a little larger than the simplified capsules.
static const float COLLISION_BOX_MARGIN = 4.0f;

// Moves the game entity along with the body, so sounds, the death camera and the game's idea of
// where the character is follow it. The origin sits above the lowest point of the body, as if the
// character were standing there.
//
// The collision box is also stretched around the whole body. Shots and sabers first have to enter
// this box before the game checks the actual model, so a standing-sized box would let most of a
// body lying full length go unhit. A knocked-down character temporarily uses corpse contents, which
// shots still hit but characters can walk through, so the larger box does not block anyone.
static Vector3 UpdateEntityPosition( RagdollInstance &instance, gentity_t *entity )
{
	const Simulation &simulation = instance.simulation;
	Vector3 bodyMinimum, bodyMaximum;
	simulation.ComputeBounds( bodyMinimum, bodyMaximum );

	// The origin uses the original (standing) box height, so it does not depend on the stretched box.
	const Vector3 &pelvisPosition = simulation.bodies[ragdoll::BODY_PELVIS].position;
	const Vector3 entityOrigin( pelvisPosition.x, pelvisPosition.y, bodyMinimum.z - instance.originalMinimum.z + 1.0f );

	const Vector3 margin( COLLISION_BOX_MARGIN, COLLISION_BOX_MARGIN, COLLISION_BOX_MARGIN );
	const Vector3 boxMinimum = bodyMinimum - entityOrigin - margin;
	const Vector3 boxMaximum = bodyMaximum - entityOrigin + margin;

	const bool originMoved = ragdoll::LengthSquared( entityOrigin - instance.lastServerOrigin ) > 0.25f;
	const bool boxChanged = ragdoll::LengthSquared( boxMinimum - Vector3::FromArray( entity->mins ) ) > 0.25f
		|| ragdoll::LengthSquared( boxMaximum - Vector3::FromArray( entity->maxs ) ) > 0.25f;
	if ( !originMoved && !boxChanged )
	{
		return entityOrigin;
	}

	vec3_t originArray;
	entityOrigin.CopyToArray( originArray );
	G_SetOrigin( entity, originArray );
	VectorCopy( originArray, entity->client->ps.origin );
	VectorClear( entity->client->ps.velocity );
	boxMinimum.CopyToArray( entity->mins );
	boxMaximum.CopyToArray( entity->maxs );
	if ( instance.isKnockdown )
	{
		entity->contents = CONTENTS_CORPSE;
	}
	gi.linkentity( entity );
	instance.lastServerOrigin = entityOrigin;
	return entityOrigin;
}

// Gives a knocked-down character back its standing collision box and contents.
static void RestoreCollisionBox( const RagdollInstance &instance, gentity_t *entity )
{
	instance.originalMinimum.CopyToArray( entity->mins );
	instance.originalMaximum.CopyToArray( entity->maxs );
	entity->contents = instance.originalContents;
	gi.linkentity( entity );
}

// Debug check (g_ragdollDebug 2): asks the engine where it now draws a few bones and prints how far
// that is from where the physics put them. Both numbers should be close to zero; a large error means
// the bone overrides do not reproduce the simulated pose.
static void PrintPoseError( RagdollInstance &instance, gentity_t *entity, const Transform *bodyTransforms,
							int renderTime, const vec3_t renderOrigin, const vec3_t renderAngles )
{
	static const int checkedBodies[] = { ragdoll::BODY_PELVIS, ragdoll::BODY_HEAD, ragdoll::BODY_RIGHT_HAND, ragdoll::BODY_LEFT_FOOT };
	const float modelScale = instance.rig.modelScale;
	vec3_t scaleVector = { modelScale, modelScale, modelScale };

	gi.Printf( "ragdoll %d pose error:", instance.entityNumber );
	for ( size_t checkIndex = 0; checkIndex < ARRAY_LEN( checkedBodies ); checkIndex++ )
	{
		const int bodyIndex = checkedBodies[checkIndex];
		const char *boneName = instance.rig.skeleton->bones[instance.rig.bodyBoneIndex[bodyIndex]].name;
		const int boltIndex = gi.G2API_AddBolt( &entity->ghoul2[entity->playerModel], boneName );

		mdxaBone_t boltMatrix;
		vec3_t drawnPosition;
		gi.G2API_GetBoltMatrix( entity->ghoul2, entity->playerModel, boltIndex, &boltMatrix, renderAngles, renderOrigin,
								renderTime, NULL, scaleVector );
		gi.G2API_GiveMeVectorFromMatrix( boltMatrix, ORIGIN, drawnPosition );

		const Vector3 simulatedPosition = ragdoll::BoneWorldTransformForBody( instance.rig, bodyIndex, bodyTransforms[bodyIndex] ).position;
		gi.Printf( " %s %.1f", boneName, ragdoll::Length( Vector3::FromArray( drawnPosition ) - simulatedPosition ) );
	}
	gi.Printf( "\n" );
}

/*
==================
Knockdowns

A living character knocked over by Force Push or Pull falls as a ragdoll. While it is down, the
game still believes it is playing its knockdown animation, so the AI and movement code wait. Once
the body has settled, the character is moved to where the body lies and the knockdown is allowed
to end: the game then plays its own get-up animation (including the acrobatic Force get-ups of
Jedi), and the visible pose blends from the ragdoll into that animation. After that the AI simply
carries on.
==================
*/

extern SavedGameJustLoaded_e g_eSavedGameJustLoaded;

static bool IsKnockdownAnimation( int animation )
{
	return animation >= BOTH_KNOCKDOWN1 && animation <= BOTH_KNOCKDOWN5;
}

static bool IsGetUpAnimation( int animation )
{
	return animation >= BOTH_GETUP1 && animation <= BOTH_FORCE_GETUP_B6;
}

// True for a living, visible character with the standard humanoid skeleton.
static bool IsLivingHumanoidCharacter( gentity_t *entity )
{
	if ( !entity->inuse || !entity->client || entity->health <= 0 || ( entity->s.eFlags & EF_NODRAW ) )
	{
		return false;
	}
	if ( entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return false;
	}
	const char *skeletonName = gi.G2API_GetGLAName( &entity->ghoul2[entity->playerModel] );
	return skeletonName && !Q_stricmp( skeletonName, HUMANOID_SKELETON_NAME );
}

// Bone overrides are stored in saved games. A character saved mid-knockdown would come back with its
// ragdoll pose frozen on top of its animation, so after loading, each living character has any
// leftover overrides removed the first time it is drawn.
static void ClearOverridesLeftInSavedGame( gentity_t *entity )
{
	const int entityNumber = EntityIndex( entity );
	if ( savedOverridesChecked[entityNumber] )
	{
		return;
	}
	savedOverridesChecked[entityNumber] = true;
	if ( g_eSavedGameJustLoaded == eNO || !IsLivingHumanoidCharacter( entity ) )
	{
		return;
	}
	const Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &entity->ghoul2[entity->playerModel] ) );
	if ( skeleton )
	{
		ragdoll::ClearSkeletonBoneOverrides( *skeleton, &entity->ghoul2[entity->playerModel] );
	}
}

// Keeps the game believing the character is still in its knockdown animation, so it does not try
// to get up (or think, move or shoot) while the body is a ragdoll.
static void KeepKnockdownAnimation( const RagdollInstance &instance, gentity_t *entity )
{
	playerState_t &state = entity->client->ps;
	const int knockdownAnimation = IsKnockdownAnimation( instance.knockdownAnimation ) ? instance.knockdownAnimation : BOTH_KNOCKDOWN1;
	if ( !IsKnockdownAnimation( state.legsAnim ) )
	{
		state.legsAnim = knockdownAnimation;		// for example a pain animation from being shot while down
	}
	if ( !IsKnockdownAnimation( state.torsoAnim ) )
	{
		state.torsoAnim = knockdownAnimation;
	}
	if ( state.legsAnimTimer < KNOCKDOWN_ANIMATION_HOLD_MILLISECONDS )
	{
		state.legsAnimTimer = KNOCKDOWN_ANIMATION_HOLD_MILLISECONDS;
	}
	if ( state.torsoAnimTimer < KNOCKDOWN_ANIMATION_HOLD_MILLISECONDS )
	{
		state.torsoAnimTimer = KNOCKDOWN_ANIMATION_HOLD_MILLISECONDS;
	}
}

// Removes the ragdoll's bone overrides and reads the pose the animation alone would show.
static void ReadAnimationPose( const RagdollInstance &instance, gentity_t *entity, int renderTime,
							   Transform *modelSpaceFrames, bool *frameIsValid )
{
	ragdoll::ClearBoneOverrides( instance.rig, &entity->ghoul2[entity->playerModel] );
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *instance.rig.skeleton, renderTime,
									modelSpaceFrames, frameIsValid );
}

// The body has settled: line the character up with it and let the game play its get-up animation.
static void BeginGetUp( RagdollInstance &instance, gentity_t *entity, int renderTime )
{
	const Simulation &simulation = instance.simulation;
	const ragdoll::Rig &rig = instance.rig;
	const float modelScale = rig.modelScale;

	// Lying on its back (chest facing up) or on its front decides which knockdown the game ends and
	// therefore which get-up it plays: knockdown 1 ends on the back, knockdown 3 face down.
	const RigidBody &chest = simulation.bodies[ragdoll::BODY_CHEST];
	const Vector3 chestFront = ragdoll::Rotate( chest.orientation, rig.forwardAxisInBodySpace[ragdoll::BODY_CHEST] );
	const int lyingAnimation = ( chestFront.z >= 0.0f ) ? BOTH_KNOCKDOWN1 : BOTH_KNOCKDOWN3;

	// Remember every bone's world frame now; the blend starts from exactly this pose.
	Transform bodyTransforms[HUMANOID_BODY_COUNT];
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		bodyTransforms[bodyIndex] = Transform( simulation.bodies[bodyIndex].orientation, simulation.bodies[bodyIndex].position );
	}
	static Transform boneFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ComputeBoneFrames( rig, bodyTransforms, Transform(), boneFrames );	// identity: frames come out in world space
	for ( int boneIndex = 0; boneIndex < rig.skeleton->boneCount; boneIndex++ )
	{
		instance.getUpWorldFrames[boneIndex] = Transform( boneFrames[boneIndex].rotation, boneFrames[boneIndex].position * modelScale );
	}

	// Freeze the model on the last frame of that knockdown animation: the lying pose the get-up
	// animation starts from.
	const int animationFileIndex = entity->client->clientInfo.animFileIndex;
	if ( animationFileIndex >= 0 && animationFileIndex < MAX_ANIM_FILES )
	{
		const animation_t &animation = level.knownAnimFileSets[animationFileIndex].animations[lyingAnimation];
		const int lastFrame = animation.firstFrame + animation.numFrames - 1;
		CGhoul2Info *ghoul2Info = &entity->ghoul2[entity->playerModel];
		const int animatedBones[2] = { entity->rootBone, entity->lowerLumbarBone };
		for ( int boneSlot = 0; boneSlot < 2; boneSlot++ )
		{
			if ( animatedBones[boneSlot] >= 0 )
			{
				gi.G2API_SetBoneAnimIndex( ghoul2Info, animatedBones[boneSlot], lastFrame, lastFrame + 1,
										   BONE_ANIM_OVERRIDE_FREEZE, 1.0f, renderTime, (float)lastFrame, -1 );
			}
		}
	}

	// Turn and move the character so that lying pose lies where the body is: the line from pelvis to
	// head must point the same way, and the pelvis must be in the same place.
	static Transform animationFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool animationFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ReadAnimationPose( instance, entity, renderTime, animationFrames, animationFrameIsValid );

	const int pelvisBone = rig.bodyBoneIndex[ragdoll::BODY_PELVIS];
	const int headBone = rig.bodyBoneIndex[ragdoll::BODY_HEAD];
	const Vector3 animationBodyLine = HorizontalPart( animationFrames[headBone].position - animationFrames[pelvisBone].position );
	const Vector3 ragdollBodyLine = HorizontalPart( instance.getUpWorldFrames[headBone].position - instance.getUpWorldFrames[pelvisBone].position );
	float yawDegrees = instance.renderYawDegrees;
	if ( ragdoll::Length( animationBodyLine ) > 1.0f && ragdoll::Length( ragdollBodyLine ) > 1.0f )
	{
		yawDegrees = ragdoll::RadiansToDegrees( atan2f( ragdollBodyLine.y, ragdollBodyLine.x ) - atan2f( animationBodyLine.y, animationBodyLine.x ) );
	}
	const Vector3 drawOrigin = instance.getUpWorldFrames[pelvisBone].position
		- ragdoll::Rotate( YawRotation( yawDegrees ), animationFrames[pelvisBone].position * modelScale );
	const Vector3 entityOrigin = drawOrigin - Vector3( 0.0f, 0.0f, 24.0f * ( modelScale - 1.0f ) );	// undo CG_Player's height adjustment

	vec3_t originArray;
	entityOrigin.CopyToArray( originArray );
	G_SetOrigin( entity, originArray );
	VectorCopy( originArray, entity->client->ps.origin );
	VectorClear( entity->client->ps.velocity );
	entity->client->ps.viewangles[YAW] = yawDegrees;
	entity->currentAngles[YAW] = yawDegrees;
	if ( entity->NPC )
	{
		entity->NPC->desiredYaw = yawDegrees;
		entity->NPC->lockedDesiredYaw = yawDegrees;
	}
	RestoreCollisionBox( instance, entity );	// standing box and contents again (this also links the entity)

	// End the knockdown: the movement code now picks and plays the matching get-up animation.
	entity->client->ps.legsAnim = lyingAnimation;
	entity->client->ps.torsoAnim = lyingAnimation;
	entity->client->ps.legsAnimTimer = 0;
	entity->client->ps.torsoAnimTimer = 0;

	instance.getUpSeconds = 0.0f;
	instance.lastGetUpRenderTime = renderTime;
	SetState( instance, RAGDOLL_GETTING_UP );
}

// Each frame while getting up: blends from the ragdoll pose into the live get-up animation, then
// hands the character back to the animation completely.
static void PoseGetUp( RagdollInstance &instance, gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3] )
{
	float frameSeconds = ( renderTime - instance.lastGetUpRenderTime ) / 1000.0f;
	instance.lastGetUpRenderTime = renderTime;
	frameSeconds = ragdoll::Clamp( frameSeconds, 0.0f, LONGEST_FRAME_SECONDS );
	instance.getUpSeconds += frameSeconds;

	const float progress = ragdoll::Clamp( instance.getUpSeconds / GET_UP_BLEND_SECONDS, 0.0f, 1.0f );
	const float blend = progress * progress * ( 3.0f - 2.0f * progress );

	static Transform animationFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool animationFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ReadAnimationPose( instance, entity, renderTime, animationFrames, animationFrameIsValid );

	if ( progress >= 1.0f )
	{
		// Fully blended: the animation (and the AI) are in charge again.
		if ( g_ragdollDebug->integer )
		{
			gi.Printf( "ragdoll %d: back on its feet\n", instance.entityNumber );
		}
		ReleaseInstance( instance, false );		// the overrides were already removed above
		return;
	}

	// The ragdoll pose, expressed in the space the model is drawn in this frame.
	const Transform worldToModel = Inverse( Transform( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) ) );
	const float inverseScale = 1.0f / instance.rig.modelScale;
	static Transform blendedFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	for ( int boneIndex = 0; boneIndex < instance.rig.skeleton->boneCount; boneIndex++ )
	{
		Transform ragdollFrame = Compose( worldToModel, instance.getUpWorldFrames[boneIndex] );
		ragdollFrame.position = ragdollFrame.position * inverseScale;
		if ( !animationFrameIsValid[boneIndex] )
		{
			blendedFrames[boneIndex] = ragdollFrame;
			continue;
		}
		const Transform &animationFrame = animationFrames[boneIndex];
		blendedFrames[boneIndex] = Transform(
			ragdoll::Slerp( ragdollFrame.rotation, animationFrame.rotation, blend ),
			ragdollFrame.position + ( animationFrame.position - ragdollFrame.position ) * blend );
	}
	ragdoll::WriteBoneFrames( instance.rig, blendedFrames, &entity->ghoul2[entity->playerModel], renderTime );
}

/*
==================
Hit reactions

A living character hit by a shot or a saber becomes a powered ragdoll for a moment: every body part
is pulled toward where the live animation has it by strong muscles, and the hit shoves the body away
from that pose. The drawn pose blends from the physics back to the animation, so the character
flinches without ever stopping what it is doing.
==================
*/

// While a shot reaction has control, the character goes where its body goes (a stagger moves it),
// instead of the body following the character. The collision box is swept there, so it never ends
// up inside a wall, and the movement code (given nothing to do, see G_Ragdoll_IsControllingCharacter)
// keeps it on the ground.
static void KeepCharacterWithBody( const RagdollInstance &instance, gentity_t *entity )
{
	const Vector3 &pelvisPosition = instance.simulation.bodies[ragdoll::BODY_PELVIS].position;
	vec3_t destination = { pelvisPosition.x + instance.characterOffsetFromPelvis.x,
						   pelvisPosition.y + instance.characterOffsetFromPelvis.y,
						   entity->currentOrigin[2] };
	trace_t trace;
	gi.trace( &trace, entity->currentOrigin, entity->mins, entity->maxs, destination, entity->s.number, entity->clipmask, G2_NOCOLLIDE, 0 );
	if ( !trace.startsolid && !trace.allsolid && trace.fraction > 0.0f )
	{
		G_SetOrigin( entity, trace.endpos );
		VectorCopy( trace.endpos, entity->client->ps.origin );
		gi.linkentity( entity );
	}
	entity->client->ps.velocity[0] = 0.0f;
	entity->client->ps.velocity[1] = 0.0f;
}

// Each frame while reacting to a hit: reads the live animation (the target the muscles pull toward
// during the next physics steps) and draws a blend of the physical body and the animation.
static void PoseHitReaction( RagdollInstance &instance, gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3] )
{
	const ragdoll::Rig &rig = instance.rig;
	const float modelScale = rig.modelScale;
	const Transform modelToWorld( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) );

	static Transform animationFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool animationFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ReadAnimationPose( instance, entity, renderTime, animationFrames, animationFrameIsValid );

	// Where the animation has each body this frame.
	bool everyBodyWasRead = true;
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const int boneIndex = rig.bodyBoneIndex[bodyIndex];
		if ( !animationFrameIsValid[boneIndex] )
		{
			everyBodyWasRead = false;
			break;
		}
		const Transform boneInWorld( ragdoll::NormalizedQuaternion( modelToWorld.rotation * animationFrames[boneIndex].rotation ),
									 modelToWorld.TransformPoint( animationFrames[boneIndex].position * modelScale ) );
		instance.animationBodyTargets[bodyIndex] = Transform( boneInWorld.rotation, boneInWorld.TransformPoint( rig.centreOfMassInBoneSpace[bodyIndex] ) );
	}
	instance.hasAnimationTargets = everyBodyWasRead;

	// Fully physical while a shot reaction has control and just after a hit, then an eased fade back
	// to the animation.
	const float fade = ragdoll::Clamp( ( instance.secondsSinceLastHit - HIT_REACTION_HOLD_SECONDS ) / HIT_REACTION_FADE_SECONDS, 0.0f, 1.0f );
	const float physicsWeight = instance.isInControl ? 1.0f : 1.0f - fade * fade * ( 3.0f - 2.0f * fade );
	if ( physicsWeight <= 0.0f || !everyBodyWasRead )
	{
		if ( g_ragdollDebug->integer )
		{
			gi.Printf( "ragdoll %d: hit reaction over\n", instance.entityNumber );
		}
		ReleaseInstance( instance, false );		// the overrides were already removed above
		return;
	}

	Transform bodyTransforms[HUMANOID_BODY_COUNT];
	InterpolatedBodyTransforms( instance, bodyTransforms );
	static Transform physicsFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ComputeBoneFrames( rig, bodyTransforms, modelToWorld, physicsFrames );

	static Transform blendedFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	for ( int boneIndex = 0; boneIndex < rig.skeleton->boneCount; boneIndex++ )
	{
		if ( !animationFrameIsValid[boneIndex] )
		{
			blendedFrames[boneIndex] = physicsFrames[boneIndex];
			continue;
		}
		const Transform &animationFrame = animationFrames[boneIndex];
		const Transform &physicsFrame = physicsFrames[boneIndex];
		blendedFrames[boneIndex] = Transform(
			ragdoll::Slerp( animationFrame.rotation, physicsFrame.rotation, physicsWeight ),
			animationFrame.position + ( physicsFrame.position - animationFrame.position ) * physicsWeight );
	}
	ragdoll::WriteBoneFrames( rig, blendedFrames, &entity->ghoul2[entity->playerModel], renderTime );
}

/*
==================
G_Ragdoll_UpdateAndPose
==================
*/
qboolean G_Ragdoll_UpdateAndPose( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3], vec3_t renderAngles )
{
	if ( !entity || !entity->client || !g_ragdoll )
	{
		return qfalse;
	}
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES )
	{
		return qfalse;
	}

	// A character being cut in two (see g_slice.h) is replaced by its pieces, from exactly the pose
	// on screen, before anything else happens to it.
	if ( G_Slice_CutIfRequested( entity, renderTime, renderOrigin, renderAxis ) )
	{
		return qfalse;
	}

	// Step every ragdoll once per frame, whichever character is drawn first. This runs for living
	// characters too, so ragdolls keep moving even when no corpse happens to be drawn first.
	if ( g_ragdoll->integer )
	{
		StepAllRagdolls( renderTime );

		// A stumbling character is carried along and steps with its legs; one pushed over falls as a
		// knockdown ragdoll (started just below, or turned from its hit reaction straight away).
		if ( entity->health > 0 && ragdoll::UpdateBalance( entity ) == ragdoll::BALANCE_FALLING )
		{
			G_Ragdoll_OnKnockdown( entity );
		}
	}

	RagdollInstance *instance = FindInstance( entityNumber );

	// The mission failed: the player kneels as the game animates it, even out of a hit reaction.
	if ( IsPlayerKneelingAfterFailedMission( entity ) )
	{
		if ( instance )
		{
			ReleaseInstance( *instance, true );
		}
		return qfalse;
	}

	// A living character with no ragdoll: the common case, unless it was just knocked down or hit.
	if ( !instance && entity->health > 0 )
	{
		ClearOverridesLeftInSavedGame( entity );
		if ( !g_ragdoll->integer )
		{
			return qfalse;
		}

		KnockdownRecord &knockdown = pendingKnockdowns[entityNumber];
		HitReactionRecord &hit = pendingHitReactions[entityNumber];
		if ( knockdown.pending )
		{
			knockdown.pending = false;
			hit.pending = false;	// the knockdown is the bigger reaction
			if ( level.time - knockdown.time <= KNOCKDOWN_START_WINDOW_MILLISECONDS
				&& IsLivingHumanoidCharacter( entity )
				&& IsKnockdownAnimation( entity->client->ps.legsAnim ) )	// not already up, nor taken over by something else
			{
				instance = StartRagdoll( entity, renderTime, renderOrigin, renderAxis, true, RAGDOLL_KIND_KNOCKDOWN );
			}
		}
		else if ( hit.pending )
		{
			hit.pending = false;
			if ( level.time - hit.time <= HIT_REACTION_START_WINDOW_MILLISECONDS
				&& IsLivingHumanoidCharacter( entity )
				&& WantsHitReaction( entity, hit.meansOfDeath ) )
			{
				instance = StartRagdoll( entity, renderTime, renderOrigin, renderAxis, false, RAGDOLL_KIND_HIT_REACTION );
				if ( instance )
				{
					ApplyHitImpulse( *instance, hit.point, hit.direction, hit.damage, hit.meansOfDeath, HitImpulseScaleFor( *instance ) );
				}
			}
		}
		if ( !instance )
		{
			return qfalse;
		}
	}

	if ( !g_ragdoll->integer )
	{
		if ( instance )
		{
			// Switched off: a corpse stays where it lies, a knocked-down character goes back to animating.
			ReleaseInstance( *instance, entity->health > 0 );
			ragdollFinished[entityNumber] = ( entity->health <= 0 );
		}
		return qfalse;
	}

	// Killed while knocked down (normally already handled by G_Ragdoll_OnDeath).
	if ( instance && instance->isKnockdown && entity->health <= 0 )
	{
		instance->isKnockdown = false;
		if ( instance->state == RAGDOLL_DAZED || instance->state == RAGDOLL_GETTING_UP )
		{
			SetState( *instance, RAGDOLL_LIMP );
		}
	}

	// Killed while reacting to a hit (normally already handled by G_Ragdoll_OnDeath).
	if ( instance && instance->isHitReaction && entity->health <= 0 )
	{
		StopFollowingAnimation( *instance );
		SetState( *instance, RAGDOLL_STAGGERING );
	}

	if ( instance && instance->isHitReaction )
	{
		// Hidden, gripped or mounted on a gun: the game animates the character itself, so stop.
		if ( !IsLivingHumanoidCharacter( entity ) || ( entity->client->ps.eFlags & EF_FORCE_GRIPPED )
			|| ( entity->s.eFlags & EF_LOCKED_TO_WEAPON ) )
		{
			ReleaseInstance( *instance, true );
			return qfalse;
		}
		instance->lastDrawnLevelTime = level.time;
		if ( instance->isInControl )
		{
			KeepCharacterWithBody( *instance, entity );
		}
		PoseHitReaction( *instance, entity, renderTime, renderOrigin, renderAxis );
		return qfalse;		// drawn where the game puts the character, so nothing to replace
	}
	else if ( instance && instance->isKnockdown )
	{
		// Hidden, or grabbed by the game's own Force Grip (which animates and moves the character
		// itself): give the character straight back to the animation.
		if ( !IsLivingHumanoidCharacter( entity ) || ( entity->client->ps.eFlags & EF_FORCE_GRIPPED ) )
		{
			ReleaseInstance( *instance, true );
			return qfalse;
		}
	}
	else if ( instance && !IsHumanoidCorpse( entity ) )
	{
		// Brought back to life, disintegrated or hidden.
		ReleaseInstance( *instance, entity->health > 0 );
		return qfalse;
	}

	if ( !instance )
	{
		if ( ragdollFinished[entityNumber] || !IsHumanoidCorpse( entity ) )
		{
			return qfalse;
		}

		// A fresh death reacts to the killing blow. Corpses that were already dead (for example
		// after loading a saved game) simply go limp and settle from their current pose.
		DeathRecord &death = pendingDeaths[entityNumber];
		const bool reactToDeath = death.pending && level.time - death.time <= DEATH_START_WINDOW_MILLISECONDS;
		instance = StartRagdoll( entity, renderTime, renderOrigin, renderAxis, reactToDeath, RAGDOLL_KIND_CORPSE );
		death.pending = false;
		if ( !instance )
		{
			return qfalse;
		}
	}

	if ( instance->isKnockdown )
	{
		if ( instance->state == RAGDOLL_GETTING_UP )
		{
			PoseGetUp( *instance, entity, renderTime, renderOrigin, renderAxis );
			return qfalse;		// drawn where the game puts the character, so nothing to replace
		}

		// If the game already started getting up (the character was off screen, so nothing kept the
		// knockdown going), get up now instead of forcing it back down.
		if ( IsGetUpAnimation( entity->client->ps.legsAnim ) && instance->state != RAGDOLL_GETTING_UP )
		{
			instance->getUpRequested = true;
			instance->state = RAGDOLL_DAZED;
		}
		else
		{
			KeepKnockdownAnimation( *instance, entity );
		}

		if ( instance->state == RAGDOLL_DAZED && instance->getUpRequested )
		{
			BeginGetUp( *instance, entity, renderTime );
			PoseGetUp( *instance, entity, renderTime, renderOrigin, renderAxis );
			return qfalse;
		}
	}

	Transform bodyTransforms[HUMANOID_BODY_COUNT];
	InterpolatedBodyTransforms( *instance, bodyTransforms );

	// Draw the model at the entity's position with a fixed yaw. This matches how CG_Player draws
	// an ordinary corpse, so the pose stays put if the ragdoll is ever released.
	Vector3 drawOrigin = UpdateEntityPosition( *instance, entity );
	const float modelScale = instance->rig.modelScale;
	if ( modelScale != 1.0f )
	{
		drawOrigin.z += 24.0f * ( modelScale - 1.0f );	// the same height adjustment CG_Player applies
	}

	VectorSet( renderAngles, 0.0f, instance->renderYawDegrees, 0.0f );
	AnglesToAxis( renderAngles, renderAxis );
	drawOrigin.CopyToArray( renderOrigin );

	const Transform modelToWorld( YawRotation( instance->renderYawDegrees ), drawOrigin );
	ragdoll::WriteBoneOverrides( instance->rig, bodyTransforms, modelToWorld, &entity->ghoul2[entity->playerModel], renderTime );

	// Full per-bone check, once, on the frame the ragdoll starts.
	if ( g_ragdollDebug->integer >= 3 && instance->secondsSinceStart == 0.0f )
	{
		gi.Printf( "ragdoll %d ", instance->entityNumber );
		ragdoll::PrintBoneOverrideErrors( instance->rig, bodyTransforms, modelToWorld, entity->ghoul2, entity->playerModel, renderTime );
	}

	// Check the pose for the first few frames, then once every couple of seconds.
	if ( g_ragdollDebug->integer >= 2 && ( instance->secondsSinceStart < 0.05f || ( renderTime / 50 ) % 40 == 0 ) )
	{
		PrintPoseError( *instance, entity, bodyTransforms, renderTime, renderOrigin, renderAngles );
	}
	return qtrue;
}

/*
==================
G_Ragdoll_IsControllingCharacter
==================
*/
qboolean G_Ragdoll_IsControllingCharacter( gentity_t *entity )
{
	if ( !entity || !g_ragdoll || !g_ragdoll->integer )
	{
		return qfalse;
	}
	const int entityNumber = EntityIndex( entity );
	if ( entityNumber <= 0 || entityNumber >= MAX_GENTITIES )
	{
		return qfalse;
	}
	RagdollInstance *instance = FindInstance( entityNumber );
	if ( !instance || !instance->isHitReaction || !instance->isInControl )
	{
		return qfalse;
	}

	// Out of sight for a while, the body is no longer being simulated: give the character its body
	// back rather than leave it frozen.
	if ( level.time - instance->lastDrawnLevelTime > CONTROL_WITHOUT_DRAWING_MILLISECONDS )
	{
		RecoverControl( *instance, entity );
		return qfalse;
	}
	return qtrue;
}
