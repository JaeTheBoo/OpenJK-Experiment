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

// g_ragdoll_solver.cpp -- articulated rigid body physics for active ragdolls.
// See g_ragdoll_solver.h for an overview of the method.

#include "g_ragdoll_solver.h"

namespace ragdoll
{

// A body point is treated as moving this far from the centre of mass when estimating how fast
// the fastest point of a spinning body moves.
static const float TYPICAL_BODY_RADIUS = 8.0f;

/*
==================
RigidBody helpers
==================
*/

Vector3 RigidBody::ApplyInverseInertia( const Vector3 &worldVector ) const
{
	const Vector3 localVector = InverseRotate( orientation, worldVector );
	return Rotate( orientation, inverseInertiaLocal * localVector );
}

float RigidBody::GeneralizedInverseMass( const Vector3 &offsetFromCentre, const Vector3 &unitDirection ) const
{
	const Vector3 angularDirection = Cross( offsetFromCentre, unitDirection );
	return inverseMass + Dot( angularDirection, ApplyInverseInertia( angularDirection ) );
}

float RigidBody::GeneralizedInverseInertia( const Vector3 &unitAxis ) const
{
	return Dot( unitAxis, ApplyInverseInertia( unitAxis ) );
}

/*
==================
Constraint helpers

These implement the two basic XPBD corrections. Every joint, limit, muscle and contact is
expressed as one of them.
==================
*/

// Turns a body by a small rotation vector (axis * angle), the first order update q += 0.5 * w * q.
static void RotateBody( RigidBody &body, const Vector3 &rotationVector )
{
	const Quaternion spin( 0.0f, rotationVector.x, rotationVector.y, rotationVector.z );
	const Quaternion change = spin * body.orientation;
	body.orientation = NormalizedQuaternion( Quaternion(
		body.orientation.w + 0.5f * change.w,
		body.orientation.x + 0.5f * change.x,
		body.orientation.y + 0.5f * change.y,
		body.orientation.z + 0.5f * change.z ) );
}

static void ApplyPositionalImpulse( RigidBody &body, const Vector3 &offsetFromCentre, const Vector3 &impulse )
{
	body.position += impulse * body.inverseMass;
	RotateBody( body, body.ApplyInverseInertia( Cross( offsetFromCentre, impulse ) ) );
}

// Moves pointOnFirst by `correction` relative to pointOnSecond, sharing the motion between the
// two bodies according to how easily each one moves at that point. Pass NULL for second when
// the first body is being pushed against the static world.
// Returns the size of the positional impulse that was applied.
static float SolvePositionalCorrection( RigidBody &first, const Vector3 &pointOnFirst,
										RigidBody *second, const Vector3 &pointOnSecond,
										const Vector3 &correction )
{
	const float distance = Length( correction );
	if ( distance < SMALL_NUMBER )
	{
		return 0.0f;
	}
	const Vector3 direction = correction / distance;

	const Vector3 offsetOnFirst = pointOnFirst - first.position;
	float combinedWeight = first.GeneralizedInverseMass( offsetOnFirst, direction );

	Vector3 offsetOnSecond;
	if ( second )
	{
		offsetOnSecond = pointOnSecond - second->position;
		combinedWeight += second->GeneralizedInverseMass( offsetOnSecond, direction );
	}
	if ( combinedWeight < SMALL_NUMBER )
	{
		return 0.0f;
	}

	const float impulseSize = distance / combinedWeight;
	const Vector3 impulse = direction * impulseSize;
	ApplyPositionalImpulse( first, offsetOnFirst, impulse );
	if ( second )
	{
		ApplyPositionalImpulse( *second, offsetOnSecond, -impulse );
	}
	return impulseSize;
}

// Turns the first body by `rotation` (axis * angle) relative to the second body.
// softness is 0 for a rigid constraint; larger values only correct part of the error, which is
// how muscles behave like springs. Pass NULL for second to rotate against the world.
static void SolveAngularCorrection( RigidBody &first, RigidBody *second, const Vector3 &rotation, float softness )
{
	const float angle = Length( rotation );
	if ( angle < SMALL_NUMBER )
	{
		return;
	}
	const Vector3 axis = rotation / angle;

	float combinedWeight = first.GeneralizedInverseInertia( axis );
	if ( second )
	{
		combinedWeight += second->GeneralizedInverseInertia( axis );
	}
	if ( combinedWeight < SMALL_NUMBER )
	{
		return;
	}

	const Vector3 angularImpulse = axis * ( angle / ( combinedWeight * ( 1.0f + softness ) ) );
	RotateBody( first, first.ApplyInverseInertia( angularImpulse ) );
	if ( second )
	{
		RotateBody( *second, -second->ApplyInverseInertia( angularImpulse ) );
	}
}

// The softness that makes a correction behave like a spring with the given natural frequency.
// Derived from the XPBD compliance alpha = inverse inertia / frequency^2, divided by substep^2.
static float SoftnessForFrequency( float naturalFrequency, float substepSeconds )
{
	const float frequencyTimesStep = naturalFrequency * substepSeconds;
	return 1.0f / ( frequencyTimesStep * frequencyTimesStep );
}

// Keeps the signed angle from parentReference to childReference, measured about the unit axis,
// within [minimum, maximum] by turning the child body back about that axis.
static void LimitAngleAboutAxis( RigidBody &child, RigidBody &parent, const Vector3 &axis,
								 const Vector3 &parentReference, const Vector3 &childReference,
								 float minimum, float maximum )
{
	const float angle = atan2f( Dot( Cross( parentReference, childReference ), axis ), Dot( parentReference, childReference ) );
	if ( angle >= minimum && angle <= maximum )
	{
		return;
	}
	const float allowedAngle = Clamp( angle, minimum, maximum );
	SolveAngularCorrection( child, &parent, axis * ( allowedAngle - angle ), 0.0f );
}

/*
==================
Simulation
==================
*/

Simulation::Simulation()
	: bodyCount( 0 ), jointCount( 0 )
{
	for ( int bodyIndex = 0; bodyIndex < MAXIMUM_BODIES; bodyIndex++ )
	{
		bodyTouchedWorldThisStep[bodyIndex] = false;
		bodyTouchedOtherBodyThisStep[bodyIndex] = false;
		for ( int otherBody = 0; otherBody < MAXIMUM_BODIES; otherBody++ )
		{
			bodyPairCollides[bodyIndex][otherBody] = false;
			selfOverlapSeconds[bodyIndex][otherBody] = 0.0f;
		}
		for ( int sphereIndex = 0; sphereIndex < MAXIMUM_SPHERES_PER_BODY; sphereIndex++ )
		{
			WorldContact &contact = contacts[bodyIndex][sphereIndex];
			contact.potential = false;
			contact.touching = false;
			contact.planeDistance = 0.0f;
			contact.normalCorrection = 0.0f;
			contact.approachSpeed = 0.0f;
		}
	}
}

void Simulation::Step( float stepSeconds, const SimulationSettings &settings, WorldSweepFunction sweepFunction, void *userContext )
{
	// A scene holding only this simulation, so self collision still applies. The scene is large,
	// so it is kept in static storage rather than on the stack.
	static Scene standaloneScene;
	standaloneScene.Clear();
	standaloneScene.AddSimulation( this, sweepFunction, userContext );
	standaloneScene.Step( stepSeconds, settings );
}

void Simulation::ApplyImpulse( int bodyIndex, const Vector3 &impulse, const Vector3 &worldPoint )
{
	if ( bodyIndex < 0 || bodyIndex >= bodyCount )
	{
		return;
	}
	RigidBody &body = bodies[bodyIndex];
	body.linearVelocity += impulse * body.inverseMass;
	body.angularVelocity += body.ApplyInverseInertia( Cross( worldPoint - body.position, impulse ) );
}

bool Simulation::IsBodyTouchingWorld( int bodyIndex ) const
{
	return bodyIndex >= 0 && bodyIndex < bodyCount && bodyTouchedWorldThisStep[bodyIndex];
}

bool Simulation::IsBodyTouchingOtherBody( int bodyIndex ) const
{
	return bodyIndex >= 0 && bodyIndex < bodyCount && bodyTouchedOtherBodyThisStep[bodyIndex];
}

void Simulation::ComputeBounds( Vector3 &minimum, Vector3 &maximum ) const
{
	minimum = Vector3( 1.0e9f, 1.0e9f, 1.0e9f );
	maximum = Vector3( -1.0e9f, -1.0e9f, -1.0e9f );
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		const RigidBody &body = bodies[bodyIndex];
		for ( int sphereIndex = 0; sphereIndex < body.sphereCount; sphereIndex++ )
		{
			const Vector3 centre = body.LocalToWorldPoint( body.spheres[sphereIndex].localCentre );
			const float radius = body.spheres[sphereIndex].radius;
			minimum = Vector3( fminf( minimum.x, centre.x - radius ), fminf( minimum.y, centre.y - radius ), fminf( minimum.z, centre.z - radius ) );
			maximum = Vector3( fmaxf( maximum.x, centre.x + radius ), fmaxf( maximum.y, centre.y + radius ), fmaxf( maximum.z, centre.z + radius ) );
		}
	}
}

float Simulation::FastestBodySpeed() const
{
	float fastestSpeed = 0.0f;
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		const RigidBody &body = bodies[bodyIndex];
		const float pointSpeed = Length( body.linearVelocity ) + Length( body.angularVelocity ) * TYPICAL_BODY_RADIUS;
		if ( pointSpeed > fastestSpeed )
		{
			fastestSpeed = pointSpeed;
		}
	}
	return fastestSpeed;
}

/*
==================
Simulation::FindWorldContacts

Sweeps every collision sphere along the path it will travel during this step. Any surface it
would touch becomes a contact plane that the substeps keep the sphere in front of. The sweep is
extended a little toward gravity so spheres resting on the floor keep their floor contact.
==================
*/
void Simulation::FindWorldContacts( float stepSeconds, const SimulationSettings &settings,
									WorldSweepFunction sweepFunction, void *userContext )
{
	const Vector3 gravityDirection = Normalized( settings.gravity );

	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		const RigidBody &body = bodies[bodyIndex];
		for ( int sphereIndex = 0; sphereIndex < body.sphereCount; sphereIndex++ )
		{
			WorldContact &contact = contacts[bodyIndex][sphereIndex];
			contact.potential = false;
			contact.touching = false;

			const CollisionSphere &sphere = body.spheres[sphereIndex];
			const Vector3 centre = body.LocalToWorldPoint( sphere.localCentre );
			const Vector3 sweepEnd = centre + body.PointVelocity( centre ) * stepSeconds + gravityDirection * settings.contactMargin;

			WorldSweepResult result;
			sweepFunction( userContext, centre, sweepEnd, sphere.radius, result );

			if ( result.startedInSolid )
			{
				// The sphere is already inside the world (for example a hand pushed into a wall).
				// Sweep out from the body's centre to find the surface it passed through, so the
				// contact pushes it back out the way it came in.
				sweepFunction( userContext, body.position, centre, sphere.radius, result );
				if ( !result.hit || result.startedInSolid )
				{
					continue;
				}
			}
			else if ( !result.hit )
			{
				continue;
			}

			contact.potential = true;
			contact.normal = result.normal;
			contact.planeDistance = Dot( result.normal, result.contactCentre );
		}
	}
}

void Simulation::IntegrateBodies( float substepSeconds, const SimulationSettings &settings )
{
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];
		body.substepStartPosition = body.position;
		body.substepStartOrientation = body.orientation;

		if ( body.inverseMass > 0.0f )
		{
			body.linearVelocity += settings.gravity * ( body.gravityScale * substepSeconds );
		}
		body.position += body.linearVelocity * substepSeconds;
		RotateBody( body, body.angularVelocity * substepSeconds );
	}
}

void Simulation::SolveMuscles( float substepSeconds )
{
	for ( int jointIndex = 0; jointIndex < jointCount; jointIndex++ )
	{
		const Joint &joint = joints[jointIndex];
		if ( joint.muscle.naturalFrequency <= 0.0f )
		{
			continue;
		}
		RigidBody &parent = bodies[joint.parentBodyIndex];
		RigidBody &child = bodies[joint.childBodyIndex];

		const Quaternion desiredChildOrientation = parent.orientation * joint.muscle.targetRelativeOrientation;
		const Vector3 error = RotationVectorFromQuaternion( desiredChildOrientation * Conjugate( child.orientation ) );
		SolveAngularCorrection( child, &parent, error, SoftnessForFrequency( joint.muscle.naturalFrequency, substepSeconds ) );
	}
}

void Simulation::SolveWorldDrives( float substepSeconds )
{
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];

		// Position spring: an XPBD positional constraint with compliance 1 / stiffness.
		if ( body.positionDrive.enabled && body.positionDrive.stiffness > 0.0f && body.inverseMass > 0.0f )
		{
			const float compliance = 1.0f / ( body.positionDrive.stiffness * substepSeconds * substepSeconds );
			Vector3 offset = body.positionDrive.targetPosition - body.position;
			offset.x *= body.positionDrive.horizontalShare;
			offset.y *= body.positionDrive.horizontalShare;
			body.position += offset * ( body.inverseMass / ( body.inverseMass + compliance ) );
		}

		if ( body.worldDrive.naturalFrequency <= 0.0f )
		{
			continue;
		}
		const Vector3 error = RotationVectorFromQuaternion( body.worldDrive.targetOrientation * Conjugate( body.orientation ) );
		SolveAngularCorrection( body, NULL, error, SoftnessForFrequency( body.worldDrive.naturalFrequency, substepSeconds ) );
	}
}

void Simulation::SolveJointLimits()
{
	for ( int jointIndex = 0; jointIndex < jointCount; jointIndex++ )
	{
		const Joint &joint = joints[jointIndex];
		RigidBody &parent = bodies[joint.parentBodyIndex];
		RigidBody &child = bodies[joint.childBodyIndex];

		// Where the child's axis would point if the joint were in its neutral pose.
		Quaternion neutralChildOrientation = parent.orientation * joint.neutralRelativeOrientation;
		Vector3 parentAxis = Rotate( neutralChildOrientation, joint.childAxis );
		Vector3 childAxis = Rotate( child.orientation, joint.childAxis );

		if ( joint.type == JOINT_HINGE )
		{
			// Keep the two hinge axes lined up so the joint can only bend one way.
			const Vector3 misalignment = Cross( childAxis, parentAxis );
			const float sineOfAngle = Length( misalignment );
			if ( sineOfAngle > SMALL_NUMBER )
			{
				float angle = asinf( Clamp( sineOfAngle, 0.0f, 1.0f ) );
				if ( Dot( childAxis, parentAxis ) < 0.0f )
				{
					angle = PI_RADIANS - angle;
				}
				SolveAngularCorrection( child, &parent, misalignment * ( angle / sineOfAngle ), 0.0f );
			}

			// Then keep the bend angle inside the allowed range.
			neutralChildOrientation = parent.orientation * joint.neutralRelativeOrientation;
			const Vector3 hingeAxis = Rotate( neutralChildOrientation, joint.childAxis );
			const Vector3 parentReference = Normalized( RemoveComponent( Rotate( neutralChildOrientation, joint.childReference ), hingeAxis ) );
			const Vector3 childReference = Normalized( RemoveComponent( Rotate( child.orientation, joint.childReference ), hingeAxis ) );
			LimitAngleAboutAxis( child, parent, hingeAxis, parentReference, childReference, joint.angleMinimum, joint.angleMaximum );
			continue;
		}

		// Ball and socket: limit the swing (how far the child's axis leans from neutral)...
		const float swingAngle = acosf( Clamp( Dot( parentAxis, childAxis ), -1.0f, 1.0f ) );
		if ( swingAngle > joint.swingLimit )
		{
			Vector3 swingAxis = Normalized( Cross( parentAxis, childAxis ) );
			if ( LengthSquared( swingAxis ) < SMALL_NUMBER )
			{
				swingAxis = AnyPerpendicular( parentAxis );
			}
			SolveAngularCorrection( child, &parent, swingAxis * ( joint.swingLimit - swingAngle ), 0.0f );
		}

		// ...and the twist (how far the child has turned about its own axis).
		neutralChildOrientation = parent.orientation * joint.neutralRelativeOrientation;
		parentAxis = Rotate( neutralChildOrientation, joint.childAxis );
		childAxis = Rotate( child.orientation, joint.childAxis );
		const Vector3 twistAxis = Normalized( parentAxis + childAxis );
		if ( LengthSquared( twistAxis ) < SMALL_NUMBER )
		{
			continue;
		}
		const Vector3 parentReference = Normalized( RemoveComponent( Rotate( neutralChildOrientation, joint.childReference ), twistAxis ) );
		const Vector3 childReference = Normalized( RemoveComponent( Rotate( child.orientation, joint.childReference ), twistAxis ) );
		LimitAngleAboutAxis( child, parent, twistAxis, parentReference, childReference, joint.angleMinimum, joint.angleMaximum );
	}
}

void Simulation::SolveJointAnchors()
{
	for ( int jointIndex = 0; jointIndex < jointCount; jointIndex++ )
	{
		const Joint &joint = joints[jointIndex];
		RigidBody &parent = bodies[joint.parentBodyIndex];
		RigidBody &child = bodies[joint.childBodyIndex];

		const Vector3 anchorOnParent = parent.LocalToWorldPoint( joint.parentAnchor );
		const Vector3 anchorOnChild = child.LocalToWorldPoint( joint.childAnchor );
		SolvePositionalCorrection( child, anchorOnChild, &parent, anchorOnParent, anchorOnParent - anchorOnChild );
	}
}

void Simulation::SolveWorldContacts( const SimulationSettings &settings )
{
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];
		for ( int sphereIndex = 0; sphereIndex < body.sphereCount; sphereIndex++ )
		{
			WorldContact &contact = contacts[bodyIndex][sphereIndex];
			contact.touching = false;
			if ( !contact.potential )
			{
				continue;
			}

			const CollisionSphere &sphere = body.spheres[sphereIndex];
			const Vector3 centre = body.LocalToWorldPoint( sphere.localCentre );

			// Velocity before this substep's corrections, needed later for bouncing.
			contact.approachSpeed = Dot( contact.normal, body.PointVelocity( centre ) );

			const float penetration = contact.planeDistance - Dot( contact.normal, centre );
			if ( penetration <= 0.0f )
			{
				continue;
			}

			// Push the sphere back out of the surface.
			contact.normalCorrection = SolvePositionalCorrection( body, centre, NULL, Vector3(), contact.normal * penetration );
			contact.touching = true;
			bodyTouchedWorldThisStep[bodyIndex] = true;

			// Static friction: if the push was strong enough, undo any sliding along the surface
			// during this substep so feet and hands grip the floor.
			const Vector3 centreAfterPush = body.LocalToWorldPoint( sphere.localCentre );
			const Vector3 centreAtSubstepStart = body.substepStartPosition + Rotate( body.substepStartOrientation, sphere.localCentre );
			const Vector3 sliding = RemoveComponent( centreAfterPush - centreAtSubstepStart, contact.normal );
			const float slidingDistance = Length( sliding );
			if ( slidingDistance < SMALL_NUMBER )
			{
				continue;
			}
			const float slidingWeight = body.GeneralizedInverseMass( centreAfterPush - body.position, sliding / slidingDistance );
			if ( slidingWeight < SMALL_NUMBER )
			{
				continue;
			}
			const float frictionImpulseNeeded = slidingDistance / slidingWeight;
			if ( frictionImpulseNeeded < settings.staticFriction * contact.normalCorrection )
			{
				SolvePositionalCorrection( body, centreAfterPush, NULL, Vector3(), -sliding );
			}
		}
	}
}

void Simulation::UpdateVelocities( float substepSeconds )
{
	const float inverseSubstep = 1.0f / substepSeconds;
	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];
		body.linearVelocity = ( body.position - body.substepStartPosition ) * inverseSubstep;

		const Quaternion orientationChange = body.orientation * Conjugate( body.substepStartOrientation );
		body.angularVelocity = orientationChange.VectorPart() * ( 2.0f * inverseSubstep );
		if ( orientationChange.w < 0.0f )
		{
			body.angularVelocity = -body.angularVelocity;
		}
	}
}

/*
==================
Simulation::SolveContactVelocities

Dynamic friction (sliding contacts slow down) and restitution (fast impacts bounce a little).
Slow impacts never bounce, otherwise resting bodies would jitter.
==================
*/
void Simulation::SolveContactVelocities( float substepSeconds, const SimulationSettings &settings )
{
	const float noBounceSpeed = 2.0f * Length( settings.gravity ) * substepSeconds;

	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];
		for ( int sphereIndex = 0; sphereIndex < body.sphereCount; sphereIndex++ )
		{
			const WorldContact &contact = contacts[bodyIndex][sphereIndex];
			if ( !contact.touching )
			{
				continue;
			}

			const Vector3 centre = body.LocalToWorldPoint( body.spheres[sphereIndex].localCentre );
			const Vector3 offsetFromCentre = centre - body.position;
			const Vector3 velocity = body.PointVelocity( centre );
			const float normalSpeed = Dot( contact.normal, velocity );
			const Vector3 tangentialVelocity = velocity - contact.normal * normalSpeed;

			Vector3 velocityChange;

			const float tangentialSpeed = Length( tangentialVelocity );
			if ( tangentialSpeed > SMALL_NUMBER )
			{
				// Friction is proportional to how hard the surface pushed back.
				const float normalWeight = body.GeneralizedInverseMass( offsetFromCentre, contact.normal );
				const float frictionSpeedLimit = settings.dynamicFriction * contact.normalCorrection * normalWeight / substepSeconds;
				const float frictionSpeed = ( frictionSpeedLimit < tangentialSpeed ) ? frictionSpeedLimit : tangentialSpeed;
				velocityChange -= tangentialVelocity * ( frictionSpeed / tangentialSpeed );
			}

			const float bounce = ( -contact.approachSpeed > noBounceSpeed ) ? settings.restitution : 0.0f;
			const float targetNormalSpeed = ( contact.approachSpeed < 0.0f ) ? -bounce * contact.approachSpeed : 0.0f;
			if ( normalSpeed < targetNormalSpeed )
			{
				velocityChange += contact.normal * ( targetNormalSpeed - normalSpeed );
			}

			const float changeSize = Length( velocityChange );
			if ( changeSize < SMALL_NUMBER )
			{
				continue;
			}
			const float weight = body.GeneralizedInverseMass( offsetFromCentre, velocityChange / changeSize );
			if ( weight < SMALL_NUMBER )
			{
				continue;
			}
			const Vector3 impulse = velocityChange / weight;
			body.linearVelocity += impulse * body.inverseMass;
			body.angularVelocity += body.ApplyInverseInertia( Cross( offsetFromCentre, impulse ) );
		}
	}
}

/*
==================
Simulation::SolveJointDamping

Slows the relative rotation of each joint. This combines the joint's passive friction with the
damping half of its muscle, so tensed muscles also resist fast movement.
==================
*/
void Simulation::SolveJointDamping( float substepSeconds )
{
	for ( int jointIndex = 0; jointIndex < jointCount; jointIndex++ )
	{
		const Joint &joint = joints[jointIndex];
		RigidBody &parent = bodies[joint.parentBodyIndex];
		RigidBody &child = bodies[joint.childBodyIndex];

		float dampingRate = joint.passiveDamping;
		if ( joint.muscle.naturalFrequency > 0.0f )
		{
			dampingRate += 2.0f * joint.muscle.dampingRatio * joint.muscle.naturalFrequency;
		}

		const Vector3 relativeAngularVelocity = child.angularVelocity - parent.angularVelocity;
		const Vector3 desiredChange = relativeAngularVelocity * -Clamp( dampingRate * substepSeconds, 0.0f, 1.0f );
		const float changeSize = Length( desiredChange );
		if ( changeSize < SMALL_NUMBER )
		{
			continue;
		}

		const Vector3 axis = desiredChange / changeSize;
		const float combinedWeight = child.GeneralizedInverseInertia( axis ) + parent.GeneralizedInverseInertia( axis );
		if ( combinedWeight < SMALL_NUMBER )
		{
			continue;
		}
		const Vector3 angularImpulse = axis * ( changeSize / combinedWeight );
		child.angularVelocity += child.ApplyInverseInertia( angularImpulse );
		parent.angularVelocity -= parent.ApplyInverseInertia( angularImpulse );
	}
}

void Simulation::ApplyBodyDampingAndLimits( float substepSeconds, const SimulationSettings &settings )
{
	const float linearKeep = 1.0f - Clamp( settings.linearDamping * substepSeconds, 0.0f, 1.0f );
	const float angularKeep = 1.0f - Clamp( settings.angularDamping * substepSeconds, 0.0f, 1.0f );

	for ( int bodyIndex = 0; bodyIndex < bodyCount; bodyIndex++ )
	{
		RigidBody &body = bodies[bodyIndex];
		body.linearVelocity *= linearKeep;
		body.angularVelocity *= angularKeep;

		if ( body.positionDrive.enabled )
		{
			const float verticalKeep = 1.0f - Clamp( body.positionDrive.damping * substepSeconds, 0.0f, 1.0f );
			const float horizontalKeep = 1.0f - Clamp( body.positionDrive.damping * body.positionDrive.horizontalShare * substepSeconds, 0.0f, 1.0f );
			body.linearVelocity.x *= horizontalKeep;
			body.linearVelocity.y *= horizontalKeep;
			body.linearVelocity.z *= verticalKeep;
		}

		// The world orientation drive is damped against the world, not against a parent.
		if ( body.worldDrive.naturalFrequency > 0.0f )
		{
			const float driveDamping = 2.0f * body.worldDrive.dampingRatio * body.worldDrive.naturalFrequency * substepSeconds;
			body.angularVelocity *= 1.0f - Clamp( driveDamping, 0.0f, 1.0f );
		}

		// Speed limits keep a bad contact or a huge impulse from launching a body across the map.
		const float linearSpeed = Length( body.linearVelocity );
		if ( linearSpeed > settings.maximumLinearSpeed )
		{
			body.linearVelocity *= settings.maximumLinearSpeed / linearSpeed;
		}
		const float angularSpeed = Length( body.angularVelocity );
		if ( angularSpeed > settings.maximumAngularSpeed )
		{
			body.angularVelocity *= settings.maximumAngularSpeed / angularSpeed;
		}
	}
}

/*
==================
Capsule geometry
==================
*/

// The capsule of a body: the segment between its collision spheres, with their radius.
static void BodyCapsule( const RigidBody &body, Vector3 &segmentStart, Vector3 &segmentEnd, float &radius )
{
	segmentStart = body.LocalToWorldPoint( body.spheres[0].localCentre );
	segmentEnd = ( body.sphereCount > 1 ) ? body.LocalToWorldPoint( body.spheres[1].localCentre ) : segmentStart;
	radius = body.spheres[0].radius;
}

// The closest pair of points between segment (firstStart, firstEnd) and segment (secondStart,
// secondEnd). From "Real-Time Collision Detection" by Christer Ericson, section 5.1.9.
static void ClosestPointsBetweenSegments( const Vector3 &firstStart, const Vector3 &firstEnd,
										  const Vector3 &secondStart, const Vector3 &secondEnd,
										  Vector3 &closestOnFirst, Vector3 &closestOnSecond )
{
	const Vector3 firstDirection = firstEnd - firstStart;
	const Vector3 secondDirection = secondEnd - secondStart;
	const Vector3 startOffset = firstStart - secondStart;
	const float firstLengthSquared = Dot( firstDirection, firstDirection );
	const float secondLengthSquared = Dot( secondDirection, secondDirection );
	const float secondAlongOffset = Dot( secondDirection, startOffset );

	float firstFraction;
	float secondFraction;
	if ( firstLengthSquared <= SMALL_NUMBER && secondLengthSquared <= SMALL_NUMBER )
	{
		firstFraction = 0.0f;
		secondFraction = 0.0f;
	}
	else if ( firstLengthSquared <= SMALL_NUMBER )
	{
		firstFraction = 0.0f;
		secondFraction = Clamp( secondAlongOffset / secondLengthSquared, 0.0f, 1.0f );
	}
	else
	{
		const float firstAlongOffset = Dot( firstDirection, startOffset );
		if ( secondLengthSquared <= SMALL_NUMBER )
		{
			secondFraction = 0.0f;
			firstFraction = Clamp( -firstAlongOffset / firstLengthSquared, 0.0f, 1.0f );
		}
		else
		{
			const float directionsDot = Dot( firstDirection, secondDirection );
			const float denominator = firstLengthSquared * secondLengthSquared - directionsDot * directionsDot;
			firstFraction = ( denominator > SMALL_NUMBER )
				? Clamp( ( directionsDot * secondAlongOffset - firstAlongOffset * secondLengthSquared ) / denominator, 0.0f, 1.0f )
				: 0.0f;
			secondFraction = ( directionsDot * firstFraction + secondAlongOffset ) / secondLengthSquared;
			if ( secondFraction < 0.0f )
			{
				secondFraction = 0.0f;
				firstFraction = Clamp( -firstAlongOffset / firstLengthSquared, 0.0f, 1.0f );
			}
			else if ( secondFraction > 1.0f )
			{
				secondFraction = 1.0f;
				firstFraction = Clamp( ( directionsDot - firstAlongOffset ) / firstLengthSquared, 0.0f, 1.0f );
			}
		}
	}

	closestOnFirst = firstStart + firstDirection * firstFraction;
	closestOnSecond = secondStart + secondDirection * secondFraction;
}

// How far any point of the body could travel in stepSeconds, used to decide which pairs might
// touch during a step.
static float BodyTravelDistance( const RigidBody &body, float stepSeconds )
{
	return ( Length( body.linearVelocity ) + Length( body.angularVelocity ) * TYPICAL_BODY_RADIUS ) * stepSeconds;
}

static bool BoxesOverlap( const Vector3 &firstMinimum, const Vector3 &firstMaximum, const Vector3 &secondMinimum, const Vector3 &secondMaximum )
{
	return firstMinimum.x <= secondMaximum.x && firstMaximum.x >= secondMinimum.x
		&& firstMinimum.y <= secondMaximum.y && firstMaximum.y >= secondMinimum.y
		&& firstMinimum.z <= secondMaximum.z && firstMaximum.z >= secondMinimum.z;
}

/*
==================
Scene
==================
*/

// Extra distance added when looking for pairs that might touch during a step.
static const float CONTACT_SEARCH_MARGIN = 2.0f;

// Friction between two body parts. Lower than against the floor, so piled bodies can slide apart.
static const float BODY_PAIR_FRICTION = 0.4f;

// Parts of the same body that stay overlapping this long stop colliding until they separate.
static const float SECONDS_BEFORE_TANGLED_PAIR_IS_IGNORED = 0.5f;
static const float TANGLED_OVERLAP_DEPTH = 1.0f;	// overlaps shallower than this are ordinary resting contact

// The solver turns every position correction into velocity, so pushing an overlapping body out in
// one tiny substep would launch it. These caps limit how fast contacts may separate bodies.
static const float MAXIMUM_SEPARATION_SPEED = 60.0f;		// two overlapping body parts easing apart
static const float OBSTACLE_EXTRA_PUSH_SPEED = 40.0f;		// an obstacle pushes at its own speed plus this

// Obstacles (living characters) move a body only part of the way out each substep, so walking
// into a corpse shoves it smoothly instead of teleporting it.
static const float OBSTACLE_PUSH_FRACTION = 0.5f;

// Where a point that is on the body now was at the start of the current substep.
static Vector3 PointAtSubstepStart( const RigidBody &body, const Vector3 &worldPoint )
{
	const Vector3 localPoint = InverseRotate( body.orientation, worldPoint - body.position );
	return body.substepStartPosition + Rotate( body.substepStartOrientation, localPoint );
}

// Limits a separating correction so the contact never separates faster than maximumSpeed.
//
// The solver keeps whatever velocity a correction creates, so limiting the size of each push is
// not enough: the body would keep its speed and be pushed again every substep. Instead, the push
// is reduced by how far the contact is already separating this substep (separatingDistance, which
// is negative when the two sides are approaching). An approaching contact therefore still gets
// its full correction, while an old overlap is resolved at no more than maximumSpeed.
static Vector3 LimitSeparation( const Vector3 &correction, float separatingDistance, float maximumSpeed, float substepSeconds )
{
	const float allowedDistance = maximumSpeed * substepSeconds - separatingDistance;
	if ( allowedDistance <= 0.0f )
	{
		return Vector3();
	}
	const float distance = Length( correction );
	return ( distance > allowedDistance ) ? correction * ( allowedDistance / distance ) : correction;
}

Scene::Scene()
	: entryCount( 0 ), obstacleCount( 0 ), pairContactCount( 0 ), obstacleContactCount( 0 )
{
}

void Scene::Clear()
{
	entryCount = 0;
	obstacleCount = 0;
	pairContactCount = 0;
	obstacleContactCount = 0;
}

void Scene::AddSimulation( Simulation *simulation, WorldSweepFunction sweepFunction, void *sweepContext )
{
	if ( entryCount >= MAXIMUM_SCENE_SIMULATIONS || !simulation )
	{
		return;
	}
	SceneEntry &entry = entries[entryCount++];
	entry.simulation = simulation;
	entry.sweepFunction = sweepFunction;
	entry.sweepContext = sweepContext;
}

void Scene::AddObstacle( const Vector3 &minimum, const Vector3 &maximum, float speed )
{
	if ( obstacleCount >= MAXIMUM_SCENE_OBSTACLES )
	{
		return;
	}
	obstacles[obstacleCount].minimum = minimum;
	obstacles[obstacleCount].maximum = maximum;
	obstacles[obstacleCount].speed = speed;
	obstacleCount++;
}

void Scene::Step( float stepSeconds, const SimulationSettings &settings )
{
	if ( stepSeconds <= 0.0f || entryCount == 0 || settings.substepCount <= 0 )
	{
		return;
	}

	for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
	{
		SceneEntry &entry = entries[entryIndex];
		Simulation &simulation = *entry.simulation;
		simulation.FindWorldContacts( stepSeconds, settings, entry.sweepFunction, entry.sweepContext );
		for ( int bodyIndex = 0; bodyIndex < simulation.bodyCount; bodyIndex++ )
		{
			simulation.bodyTouchedWorldThisStep[bodyIndex] = false;
			simulation.bodyTouchedOtherBodyThisStep[bodyIndex] = false;
		}
	}
	FindBodyPairContacts( stepSeconds );
	FindObstacleContacts( stepSeconds );

	const float substepSeconds = stepSeconds / settings.substepCount;
	for ( int substep = 0; substep < settings.substepCount; substep++ )
	{
		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			entries[entryIndex].simulation->IntegrateBodies( substepSeconds, settings );
		}

		// Softer constraints are solved first so the rigid ones (limits, anchors, contacts) have
		// the final say in each substep. The world comes last: nothing may push a body into a wall.
		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			Simulation &simulation = *entries[entryIndex].simulation;
			simulation.SolveMuscles( substepSeconds );
			simulation.SolveWorldDrives( substepSeconds );
			simulation.SolveJointLimits();
			simulation.SolveJointAnchors();
		}
		SolveBodyPairContacts( substepSeconds );
		SolveObstacleContacts( substepSeconds );
		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			entries[entryIndex].simulation->SolveWorldContacts( settings );
		}

		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			entries[entryIndex].simulation->UpdateVelocities( substepSeconds );
		}

		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			entries[entryIndex].simulation->SolveContactVelocities( substepSeconds, settings );
		}
		SolveBodyPairFriction( substepSeconds, settings );
		for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
		{
			Simulation &simulation = *entries[entryIndex].simulation;
			simulation.SolveJointDamping( substepSeconds );
			simulation.ApplyBodyDampingAndLimits( substepSeconds, settings );
		}
	}

	UpdateTangledSelfPairs( stepSeconds );
}

// Measures how long each self-collision pair has stayed overlapping, including pairs that are
// currently being ignored, so they collide again as soon as they come apart.
void Scene::UpdateTangledSelfPairs( float stepSeconds )
{
	for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
	{
		Simulation &simulation = *entries[entryIndex].simulation;
		for ( int firstBody = 0; firstBody < simulation.bodyCount; firstBody++ )
		{
			for ( int secondBody = firstBody + 1; secondBody < simulation.bodyCount; secondBody++ )
			{
				if ( !simulation.bodyPairCollides[firstBody][secondBody] )
				{
					continue;
				}

				Vector3 firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond;
				float firstRadius, secondRadius;
				BodyCapsule( simulation.bodies[firstBody], firstStart, firstEnd, firstRadius );
				BodyCapsule( simulation.bodies[secondBody], secondStart, secondEnd, secondRadius );
				ClosestPointsBetweenSegments( firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond );
				// Parts merely resting against each other sit at almost exactly the touching distance;
				// only a real overlap counts as tangled.
				const float tangledDistance = firstRadius + secondRadius - TANGLED_OVERLAP_DEPTH;
				const bool overlapping = LengthSquared( closestOnFirst - closestOnSecond ) < tangledDistance * tangledDistance;

				float &overlapSeconds = simulation.selfOverlapSeconds[firstBody][secondBody];
				overlapSeconds = overlapping ? overlapSeconds + stepSeconds : 0.0f;
			}
		}
	}
}

void Scene::TryAddBodyPair( Simulation *firstSimulation, int firstBody, Simulation *secondSimulation, int secondBody, float stepSeconds )
{
	if ( pairContactCount >= MAXIMUM_BODY_PAIR_CONTACTS )
	{
		return;
	}

	const RigidBody &first = firstSimulation->bodies[firstBody];
	const RigidBody &second = secondSimulation->bodies[secondBody];
	Vector3 firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond;
	float firstRadius, secondRadius;
	BodyCapsule( first, firstStart, firstEnd, firstRadius );
	BodyCapsule( second, secondStart, secondEnd, secondRadius );
	ClosestPointsBetweenSegments( firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond );

	const float reach = firstRadius + secondRadius + BodyTravelDistance( first, stepSeconds )
		+ BodyTravelDistance( second, stepSeconds ) + CONTACT_SEARCH_MARGIN;
	if ( LengthSquared( closestOnFirst - closestOnSecond ) > reach * reach )
	{
		return;
	}

	BodyPairContact &contact = pairContacts[pairContactCount++];
	contact.firstSimulation = firstSimulation;
	contact.firstBody = firstBody;
	contact.secondSimulation = secondSimulation;
	contact.secondBody = secondBody;
	contact.normalCorrection = 0.0f;
	contact.touching = false;
}

void Scene::FindBodyPairContacts( float stepSeconds )
{
	pairContactCount = 0;

	// Each simulation's bounds, grown by how far it can move this step.
	Vector3 boundsMinimum[MAXIMUM_SCENE_SIMULATIONS];
	Vector3 boundsMaximum[MAXIMUM_SCENE_SIMULATIONS];
	for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
	{
		const Simulation &simulation = *entries[entryIndex].simulation;
		simulation.ComputeBounds( boundsMinimum[entryIndex], boundsMaximum[entryIndex] );
		const float growth = simulation.FastestBodySpeed() * stepSeconds + CONTACT_SEARCH_MARGIN;
		boundsMinimum[entryIndex] -= Vector3( growth, growth, growth );
		boundsMaximum[entryIndex] += Vector3( growth, growth, growth );
	}

	for ( int firstEntry = 0; firstEntry < entryCount; firstEntry++ )
	{
		Simulation *firstSimulation = entries[firstEntry].simulation;

		// Self collision between body parts that are allowed to touch and are not hopelessly tangled.
		for ( int firstBody = 0; firstBody < firstSimulation->bodyCount; firstBody++ )
		{
			for ( int secondBody = firstBody + 1; secondBody < firstSimulation->bodyCount; secondBody++ )
			{
				if ( firstSimulation->bodyPairCollides[firstBody][secondBody]
					&& firstSimulation->selfOverlapSeconds[firstBody][secondBody] < SECONDS_BEFORE_TANGLED_PAIR_IS_IGNORED )
				{
					TryAddBodyPair( firstSimulation, firstBody, firstSimulation, secondBody, stepSeconds );
				}
			}
		}

		// Collision with every other simulation that is close enough.
		for ( int secondEntry = firstEntry + 1; secondEntry < entryCount; secondEntry++ )
		{
			if ( !BoxesOverlap( boundsMinimum[firstEntry], boundsMaximum[firstEntry], boundsMinimum[secondEntry], boundsMaximum[secondEntry] ) )
			{
				continue;
			}
			Simulation *secondSimulation = entries[secondEntry].simulation;
			for ( int firstBody = 0; firstBody < firstSimulation->bodyCount; firstBody++ )
			{
				for ( int secondBody = 0; secondBody < secondSimulation->bodyCount; secondBody++ )
				{
					TryAddBodyPair( firstSimulation, firstBody, secondSimulation, secondBody, stepSeconds );
				}
			}
		}
	}
}

void Scene::FindObstacleContacts( float stepSeconds )
{
	obstacleContactCount = 0;

	for ( int entryIndex = 0; entryIndex < entryCount; entryIndex++ )
	{
		Simulation *simulation = entries[entryIndex].simulation;
		for ( int bodyIndex = 0; bodyIndex < simulation->bodyCount; bodyIndex++ )
		{
			const RigidBody &body = simulation->bodies[bodyIndex];
			Vector3 segmentStart, segmentEnd;
			float radius;
			BodyCapsule( body, segmentStart, segmentEnd, radius );
			const float reach = radius + BodyTravelDistance( body, stepSeconds ) + CONTACT_SEARCH_MARGIN;
			const Vector3 bodyMinimum( fminf( segmentStart.x, segmentEnd.x ) - reach, fminf( segmentStart.y, segmentEnd.y ) - reach, fminf( segmentStart.z, segmentEnd.z ) - reach );
			const Vector3 bodyMaximum( fmaxf( segmentStart.x, segmentEnd.x ) + reach, fmaxf( segmentStart.y, segmentEnd.y ) + reach, fmaxf( segmentStart.z, segmentEnd.z ) + reach );

			for ( int obstacleIndex = 0; obstacleIndex < obstacleCount; obstacleIndex++ )
			{
				if ( obstacleContactCount >= MAXIMUM_OBSTACLE_CONTACTS )
				{
					return;
				}
				if ( BoxesOverlap( bodyMinimum, bodyMaximum, obstacles[obstacleIndex].minimum, obstacles[obstacleIndex].maximum ) )
				{
					ObstacleContact &contact = obstacleContacts[obstacleContactCount++];
					contact.simulation = simulation;
					contact.body = bodyIndex;
					contact.obstacle = obstacleIndex;
				}
			}
		}
	}
}

/*
==================
Scene::SolveBodyPairContacts

Pushes overlapping capsules apart along the line between their closest points, sharing the
motion according to how easily each body moves there.
==================
*/
void Scene::SolveBodyPairContacts( float substepSeconds )
{
	for ( int contactIndex = 0; contactIndex < pairContactCount; contactIndex++ )
	{
		BodyPairContact &contact = pairContacts[contactIndex];
		RigidBody &first = contact.firstSimulation->bodies[contact.firstBody];
		RigidBody &second = contact.secondSimulation->bodies[contact.secondBody];
		contact.touching = false;

		Vector3 firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond;
		float firstRadius, secondRadius;
		BodyCapsule( first, firstStart, firstEnd, firstRadius );
		BodyCapsule( second, secondStart, secondEnd, secondRadius );
		ClosestPointsBetweenSegments( firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond );

		const Vector3 separation = closestOnFirst - closestOnSecond;
		const float distance = Length( separation );
		const float overlap = firstRadius + secondRadius - distance;
		if ( overlap <= 0.0f )
		{
			continue;
		}

		// Segments that cross exactly have no direction between them; push straight up.
		const Vector3 normal = ( distance > SMALL_NUMBER ) ? separation / distance : Vector3( 0.0f, 0.0f, 1.0f );
		const Vector3 surfaceOfFirst = closestOnFirst - normal * firstRadius;
		const Vector3 surfaceOfSecond = closestOnSecond + normal * secondRadius;

		// How far the two surfaces have already moved apart during this substep.
		const Vector3 firstMotion = surfaceOfFirst - PointAtSubstepStart( first, surfaceOfFirst );
		const Vector3 secondMotion = surfaceOfSecond - PointAtSubstepStart( second, surfaceOfSecond );
		const float separatingDistance = Dot( firstMotion - secondMotion, normal );

		contact.normalCorrection = SolvePositionalCorrection( first, surfaceOfFirst, &second, surfaceOfSecond,
			LimitSeparation( normal * overlap, separatingDistance, MAXIMUM_SEPARATION_SPEED, substepSeconds ) );
		contact.normal = normal;
		contact.touching = true;
		contact.firstSimulation->bodyTouchedOtherBodyThisStep[contact.firstBody] = true;
		contact.secondSimulation->bodyTouchedOtherBodyThisStep[contact.secondBody] = true;
	}
}

/*
==================
Scene::SolveBodyPairFriction

Slows the sliding of touching body parts against each other, in proportion to how hard they
were pushed apart.
==================
*/
void Scene::SolveBodyPairFriction( float substepSeconds, const SimulationSettings &settings )
{
	(void)settings;

	for ( int contactIndex = 0; contactIndex < pairContactCount; contactIndex++ )
	{
		const BodyPairContact &contact = pairContacts[contactIndex];
		if ( !contact.touching )
		{
			continue;
		}
		RigidBody &first = contact.firstSimulation->bodies[contact.firstBody];
		RigidBody &second = contact.secondSimulation->bodies[contact.secondBody];

		Vector3 firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond;
		float firstRadius, secondRadius;
		BodyCapsule( first, firstStart, firstEnd, firstRadius );
		BodyCapsule( second, secondStart, secondEnd, secondRadius );
		ClosestPointsBetweenSegments( firstStart, firstEnd, secondStart, secondEnd, closestOnFirst, closestOnSecond );
		const Vector3 contactPoint = ( closestOnFirst + closestOnSecond ) * 0.5f;
		const Vector3 offsetOnFirst = contactPoint - first.position;
		const Vector3 offsetOnSecond = contactPoint - second.position;

		const Vector3 relativeVelocity = first.PointVelocity( contactPoint ) - second.PointVelocity( contactPoint );
		const Vector3 slidingVelocity = RemoveComponent( relativeVelocity, contact.normal );
		const float slidingSpeed = Length( slidingVelocity );
		if ( slidingSpeed < SMALL_NUMBER )
		{
			continue;
		}
		const Vector3 slidingDirection = slidingVelocity / slidingSpeed;

		const float slidingWeight = first.GeneralizedInverseMass( offsetOnFirst, slidingDirection )
			+ second.GeneralizedInverseMass( offsetOnSecond, slidingDirection );
		const float normalWeight = first.GeneralizedInverseMass( offsetOnFirst, contact.normal )
			+ second.GeneralizedInverseMass( offsetOnSecond, contact.normal );
		if ( slidingWeight < SMALL_NUMBER )
		{
			continue;
		}

		const float frictionSpeedLimit = BODY_PAIR_FRICTION * contact.normalCorrection * normalWeight / substepSeconds;
		const float speedChange = ( frictionSpeedLimit < slidingSpeed ) ? frictionSpeedLimit : slidingSpeed;
		const Vector3 impulse = slidingDirection * -( speedChange / slidingWeight );

		first.linearVelocity += impulse * first.inverseMass;
		first.angularVelocity += first.ApplyInverseInertia( Cross( offsetOnFirst, impulse ) );
		second.linearVelocity -= impulse * second.inverseMass;
		second.angularVelocity -= second.ApplyInverseInertia( Cross( offsetOnSecond, impulse ) );
	}
}

/*
==================
Scene::SolveObstacleContacts

Pushes body parts out of obstacle boxes. Each capsule is tested at its two ends and its middle.
Obstacles never push a body downward, because that would only fight the floor contact (a body
lying under a character's feet is shoved sideways instead).
==================
*/
void Scene::SolveObstacleContacts( float substepSeconds )
{
	for ( int contactIndex = 0; contactIndex < obstacleContactCount; contactIndex++ )
	{
		const ObstacleContact &contact = obstacleContacts[contactIndex];
		RigidBody &body = contact.simulation->bodies[contact.body];
		const SceneObstacle &obstacle = obstacles[contact.obstacle];

		Vector3 segmentStart, segmentEnd;
		float radius;
		BodyCapsule( body, segmentStart, segmentEnd, radius );
		const Vector3 samplePoints[3] = { segmentStart, ( segmentStart + segmentEnd ) * 0.5f, segmentEnd };

		for ( int sampleIndex = 0; sampleIndex < 3; sampleIndex++ )
		{
			const Vector3 &point = samplePoints[sampleIndex];
			const Vector3 closestInBox(
				Clamp( point.x, obstacle.minimum.x, obstacle.maximum.x ),
				Clamp( point.y, obstacle.minimum.y, obstacle.maximum.y ),
				Clamp( point.z, obstacle.minimum.z, obstacle.maximum.z ) );
			const Vector3 offsetFromBox = point - closestInBox;
			const float distanceFromBox = Length( offsetFromBox );

			Vector3 correction;
			if ( distanceFromBox > SMALL_NUMBER )
			{
				// Outside the box but closer than the radius.
				if ( distanceFromBox >= radius )
				{
					continue;
				}
				correction = offsetFromBox * ( ( radius - distanceFromBox ) / distanceFromBox );
			}
			else
			{
				// Inside the box: leave through the nearest side or the top.
				const float pushes[5] = {
					point.x - obstacle.minimum.x + radius,	// out through -x
					obstacle.maximum.x - point.x + radius,	// out through +x
					point.y - obstacle.minimum.y + radius,	// out through -y
					obstacle.maximum.y - point.y + radius,	// out through +y
					obstacle.maximum.z - point.z + radius	// out through the top
				};
				const Vector3 directions[5] = {
					Vector3( -1.0f, 0.0f, 0.0f ), Vector3( 1.0f, 0.0f, 0.0f ),
					Vector3( 0.0f, -1.0f, 0.0f ), Vector3( 0.0f, 1.0f, 0.0f ),
					Vector3( 0.0f, 0.0f, 1.0f ) };
				int shortest = 0;
				for ( int sideIndex = 1; sideIndex < 5; sideIndex++ )
				{
					if ( pushes[sideIndex] < pushes[shortest] )
					{
						shortest = sideIndex;
					}
				}
				correction = directions[shortest] * pushes[shortest];
			}

			if ( correction.z < 0.0f )
			{
				correction.z = 0.0f;
			}
			if ( LengthSquared( correction ) < SMALL_NUMBER )
			{
				continue;
			}

			// How far this point has already moved away from the obstacle during this substep.
			const Vector3 pushDirection = Normalized( correction );
			const float separatingDistance = Dot( point - PointAtSubstepStart( body, point ), pushDirection );

			const Vector3 limitedCorrection = LimitSeparation( correction * OBSTACLE_PUSH_FRACTION, separatingDistance,
															   obstacle.speed + OBSTACLE_EXTRA_PUSH_SPEED, substepSeconds );
			if ( LengthSquared( limitedCorrection ) < SMALL_NUMBER )
			{
				continue;
			}
			SolvePositionalCorrection( body, point, NULL, Vector3(), limitedCorrection );
			contact.simulation->bodyTouchedOtherBodyThisStep[contact.body] = true;
		}
	}
}

} // namespace ragdoll
