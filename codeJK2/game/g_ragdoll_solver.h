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

// g_ragdoll_solver.h -- articulated rigid body physics for active ragdolls.
//
// The solver uses Extended Position Based Dynamics (XPBD), as described in
// "Detailed Rigid Body Simulation with Extended Position Based Dynamics" (Mueller et al., 2020).
// Each step is split into several small substeps. In every substep the bodies are moved by
// their velocities, the constraints (joints, joint limits, muscles and world contacts) correct
// the positions directly, and the velocities are then derived from how far the bodies moved.
// This is stable with large mass ratios and needs only one solver pass per substep.
//
// The solver knows nothing about the game. World collision is provided by a callback that
// sweeps a small sphere through the level, so the game can answer it with its own trace.
//
// Units are game units (roughly inches), seconds, and an arbitrary but consistent mass unit.

#ifndef G_RAGDOLL_SOLVER_H
#define G_RAGDOLL_SOLVER_H

#include "g_ragdoll_math.h"

namespace ragdoll
{

const int MAXIMUM_BODIES = 20;
const int MAXIMUM_JOINTS = 20;
const int MAXIMUM_SPHERES_PER_BODY = 2;

// A sphere attached to a body, used for collision with the world.
struct CollisionSphere
{
	Vector3 localCentre;	// relative to the body's centre of mass, in body space
	float radius;
};

// When set, a body is pulled toward a fixed orientation in world space (used to keep the
// torso upright for a moment while a character staggers).
struct WorldOrientationDrive
{
	Quaternion targetOrientation;
	float naturalFrequency;	// radians per second, 0 disables the drive
	float dampingRatio;
};

// When enabled, a body's centre of mass is pulled toward a point in the world by a spring (used to
// hold a body up with Force Grip while the rest of the ragdoll dangles from it).
struct WorldPositionDrive
{
	bool enabled;
	Vector3 targetPosition;
	float stiffness;		// spring strength (force per unit of distance); size it for the whole ragdoll's weight
	float damping;			// per second, slows the held body so it does not bounce around the target
};

struct RigidBody
{
	Vector3 position;				// centre of mass in world space
	Quaternion orientation;			// rotates body space into world space
	Vector3 linearVelocity;			// world space, units per second
	Vector3 angularVelocity;		// world space, radians per second

	float inverseMass;
	Matrix3 inverseInertiaLocal;	// inverse inertia tensor in body space

	CollisionSphere spheres[MAXIMUM_SPHERES_PER_BODY];
	int sphereCount;

	WorldOrientationDrive worldDrive;
	WorldPositionDrive positionDrive;

	// State at the start of the current substep, used to derive velocities and friction.
	Vector3 substepStartPosition;
	Quaternion substepStartOrientation;

	Vector3 LocalToWorldPoint( const Vector3 &localPoint ) const	{ return Rotate( orientation, localPoint ) + position; }
	Vector3 PointVelocity( const Vector3 &worldPoint ) const		{ return linearVelocity + Cross( angularVelocity, worldPoint - position ); }

	// Multiplies a world space vector by the inverse inertia tensor in world space.
	Vector3 ApplyInverseInertia( const Vector3 &worldVector ) const;

	// How easily the body moves when pushed at offsetFromCentre along a unit direction.
	float GeneralizedInverseMass( const Vector3 &offsetFromCentre, const Vector3 &unitDirection ) const;

	// How easily the body rotates about a unit axis.
	float GeneralizedInverseInertia( const Vector3 &unitAxis ) const;
};

enum jointType_t
{
	JOINT_BALL_AND_SOCKET,	// free rotation inside a cone (swing) plus limited twist (shoulders, hips, spine)
	JOINT_HINGE				// rotation about one axis only (elbows, knees)
};

// A muscle pulls the child body toward a target orientation relative to the parent body.
// Its strength is expressed as a natural frequency so it behaves the same for light and heavy
// limbs: higher values snap to the target faster. A frequency of zero means fully relaxed.
struct JointMuscle
{
	Quaternion targetRelativeOrientation;	// child orientation expressed in parent body space
	float naturalFrequency;					// radians per second
	float dampingRatio;						// 1 = critically damped
};

struct Joint
{
	jointType_t type;
	int parentBodyIndex;
	int childBodyIndex;

	// The point where the bodies are connected, in each body's own space.
	Vector3 parentAnchor;
	Vector3 childAnchor;

	// The child's orientation relative to the parent in the skeleton's neutral (base) pose.
	// Joint limits are measured from this orientation.
	Quaternion neutralRelativeOrientation;

	// In child body space: the twist axis (ball joints) or the hinge axis (hinges), and a unit
	// vector perpendicular to it used to measure twist or bend angles.
	Vector3 childAxis;
	Vector3 childReference;

	float swingLimit;		// ball joints: largest angle between the parent and child twist axes (radians)
	float angleMinimum;		// ball joints: twist range; hinges: bend range (radians)
	float angleMaximum;

	float passiveDamping;	// joint friction that slows relative rotation even when relaxed (per second)

	JointMuscle muscle;
};

// Result of sweeping a collision sphere through the world.
struct WorldSweepResult
{
	bool hit;				// the sweep touched something before reaching the end
	bool startedInSolid;	// the start position was already inside the world
	Vector3 contactCentre;	// where the sphere centre stopped
	Vector3 normal;			// surface normal at the contact, pointing out of the surface
};

// Sweeps a sphere of the given radius from start to end and reports the first contact.
typedef void ( *WorldSweepFunction )( void *userContext, const Vector3 &start, const Vector3 &end,
									  float radius, WorldSweepResult &result );

// A possible contact between one collision sphere and the world, found once per step and
// enforced in every substep of that step.
struct WorldContact
{
	bool potential;				// a surface is close enough to matter during this step
	bool touching;				// the sphere was pushed out of the surface in the last substep
	Vector3 normal;
	float planeDistance;		// the sphere centre must stay where Dot( normal, centre ) >= planeDistance
	float normalCorrection;		// size of the last push out of the surface (positional impulse)
	float approachSpeed;		// normal speed before the substep solve, for restitution
};

struct SimulationSettings
{
	Vector3 gravity;
	int substepCount;
	float staticFriction;
	float dynamicFriction;
	float restitution;			// bounciness of world contacts, 0 = no bounce
	float contactMargin;		// extra distance swept toward gravity so resting contacts are found
	float linearDamping;		// per second
	float angularDamping;		// per second
	float maximumLinearSpeed;
	float maximumAngularSpeed;
};

class Simulation
{
public:
	RigidBody bodies[MAXIMUM_BODIES];
	int bodyCount;

	Joint joints[MAXIMUM_JOINTS];
	int jointCount;

	// Which pairs of this simulation's own bodies may collide with each other (self collision).
	// Bodies joined by a joint, or that already overlap in the rest pose, must not.
	bool bodyPairCollides[MAXIMUM_BODIES][MAXIMUM_BODIES];

	// How long each pair of this simulation's own bodies has been overlapping without a break. A pair
	// that stays tangled (for example knees pressed together) is let go after a while, because
	// fighting it forever only makes the whole body jitter and creep across the floor.
	float selfOverlapSeconds[MAXIMUM_BODIES][MAXIMUM_BODIES];

	Simulation();

	// Advances this simulation on its own by stepSeconds. Use Scene to step several simulations
	// together so they can collide with each other.
	void Step( float stepSeconds, const SimulationSettings &settings, WorldSweepFunction sweepFunction, void *userContext );

	// Instantly changes a body's velocity as if hit at worldPoint with the given impulse.
	void ApplyImpulse( int bodyIndex, const Vector3 &impulse, const Vector3 &worldPoint );

	// True if any collision sphere of the body touched the world during the last step.
	bool IsBodyTouchingWorld( int bodyIndex ) const;

	// True if the body touched another body or an obstacle during the last step.
	bool IsBodyTouchingOtherBody( int bodyIndex ) const;

	// The fastest point speed of any body, used to decide when the ragdoll has come to rest.
	float FastestBodySpeed() const;

	// The smallest box containing every collision sphere.
	void ComputeBounds( Vector3 &minimum, Vector3 &maximum ) const;

private:
	friend class Scene;

	bool bodyTouchedOtherBodyThisStep[MAXIMUM_BODIES];

	WorldContact contacts[MAXIMUM_BODIES][MAXIMUM_SPHERES_PER_BODY];
	bool bodyTouchedWorldThisStep[MAXIMUM_BODIES];

	void FindWorldContacts( float stepSeconds, const SimulationSettings &settings,
							WorldSweepFunction sweepFunction, void *userContext );
	void IntegrateBodies( float substepSeconds, const SimulationSettings &settings );
	void SolveMuscles( float substepSeconds );
	void SolveWorldDrives( float substepSeconds );
	void SolveJointLimits();
	void SolveJointAnchors();
	void SolveWorldContacts( const SimulationSettings &settings );
	void UpdateVelocities( float substepSeconds );
	void SolveContactVelocities( float substepSeconds, const SimulationSettings &settings );
	void SolveJointDamping( float substepSeconds );
	void ApplyBodyDampingAndLimits( float substepSeconds, const SimulationSettings &settings );
};

/*
==================
Scene

Steps several simulations together so their bodies can collide with each other, with themselves
and with moving obstacles. Each body part is treated as a capsule (the segment between its two
collision spheres, with their radius).

Obstacles are boxes that push bodies but are never pushed back (for example living characters,
which are animated rather than simulated). They are given at their current position every step,
so a moving obstacle shoves bodies along with it.

Usage each step: Clear(), AddSimulation() for every simulation, AddObstacle() for every
obstacle, then Step().
==================
*/

const int MAXIMUM_SCENE_SIMULATIONS = 32;
const int MAXIMUM_SCENE_OBSTACLES = 64;
const int MAXIMUM_BODY_PAIR_CONTACTS = 2048;
const int MAXIMUM_OBSTACLE_CONTACTS = 1024;

struct SceneObstacle
{
	Vector3 minimum;
	Vector3 maximum;
	float speed;	// how fast the obstacle is moving; it never pushes bodies much faster than this
};

class Scene
{
public:
	Scene();

	void Clear();
	void AddSimulation( Simulation *simulation, WorldSweepFunction sweepFunction, void *sweepContext );
	void AddObstacle( const Vector3 &minimum, const Vector3 &maximum, float speed );
	void Step( float stepSeconds, const SimulationSettings &settings );

private:
	struct SceneEntry
	{
		Simulation *simulation;
		WorldSweepFunction sweepFunction;
		void *sweepContext;
	};

	// Two capsules that may touch during this step (from the same or from different simulations).
	struct BodyPairContact
	{
		Simulation *firstSimulation;
		int firstBody;
		Simulation *secondSimulation;
		int secondBody;
		float normalCorrection;	// size of the last push apart, used for friction
		Vector3 normal;			// from the second body toward the first
		bool touching;
	};

	// A capsule that may touch an obstacle during this step.
	struct ObstacleContact
	{
		Simulation *simulation;
		int body;
		int obstacle;
	};

	SceneEntry entries[MAXIMUM_SCENE_SIMULATIONS];
	int entryCount;
	SceneObstacle obstacles[MAXIMUM_SCENE_OBSTACLES];
	int obstacleCount;
	BodyPairContact pairContacts[MAXIMUM_BODY_PAIR_CONTACTS];
	int pairContactCount;
	ObstacleContact obstacleContacts[MAXIMUM_OBSTACLE_CONTACTS];
	int obstacleContactCount;

	void FindBodyPairContacts( float stepSeconds );
	void TryAddBodyPair( Simulation *firstSimulation, int firstBody, Simulation *secondSimulation, int secondBody, float stepSeconds );
	void FindObstacleContacts( float stepSeconds );
	void SolveBodyPairContacts( float substepSeconds );
	void SolveObstacleContacts( float substepSeconds );
	void SolveBodyPairFriction( float substepSeconds, const SimulationSettings &settings );
	void UpdateTangledSelfPairs( float stepSeconds );
};

} // namespace ragdoll

#endif // G_RAGDOLL_SOLVER_H
