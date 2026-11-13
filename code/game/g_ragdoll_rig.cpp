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

// g_ragdoll_rig.cpp -- connects the ragdoll physics to a Ghoul2 skeleton.
// See g_ragdoll_rig.h for an overview.

#include "g_local.h"
#include "g_ragdoll_rig.h"
#include "../rd-common/mdx_format.h"

#include <string.h>

namespace ragdoll
{

/*
==================
Humanoid body layout

Measured from Jedi Outcast's models/players/_humanoid/_humanoid.gla rest pose. Jedi Academy's
_humanoid.gla has every one of these bones with an identical rest pose (checked bone by bone), so
the same table serves both games. All positions are in model space in the rest pose: +X is the
character's left, -Y is forward and +Z is up.

Capsules run from capsuleStart to capsuleEnd. For most limbs these are the joint positions of
the bone and of the next bone down the limb.
==================
*/

struct BodyDefinition
{
	const char *boneName;
	int parentBody;					// -1 for the root body
	float capsuleStart[3];
	float capsuleEnd[3];
	float radius;
	float mass;
	jointType_t jointType;
	float swingLimitDegrees;		// ball joints only
	float angleMinimumDegrees;		// twist range (ball) or bend range (hinge)
	float angleMaximumDegrees;
	float twistAxis[3];				// ball joints: axis the limb twists about; zero means "along the capsule"
	float bendDirection[3];			// hinges: the direction the limb's tip moves when the joint bends
	muscleGroup_t muscleGroup;
};

static const BodyDefinition HUMANOID_BODIES[HUMANOID_BODY_COUNT] =
{
	// bone             parent                capsule start              capsule end                radius mass  joint                swing  twist/bend range   twist axis           bend direction     group
	{ "pelvis",         -1,                   { -4.0f,  0.4f, 35.0f },  {  4.0f,  0.4f, 35.0f },   5.0f, 11.0f, JOINT_BALL_AND_SOCKET,  0.0f,   0.0f,   0.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_NONE },
	{ "lower_lumbar",   BODY_PELVIS,          {  0.0f,  0.4f, 41.35f }, {  0.0f,  0.4f, 46.47f },  5.5f,  8.0f, JOINT_BALL_AND_SOCKET, 30.0f, -25.0f,  25.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_SPINE },
	{ "upper_lumbar",   BODY_LOWER_SPINE,     {  0.0f,  0.4f, 46.47f }, {  0.0f,  0.4f, 51.59f },  6.0f,  8.0f, JOINT_BALL_AND_SOCKET, 30.0f, -25.0f,  25.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_SPINE },
	{ "thoracic",       BODY_MIDDLE_SPINE,    { -4.5f,  0.6f, 53.5f },  {  4.5f,  0.6f, 53.5f },   5.5f, 12.0f, JOINT_BALL_AND_SOCKET, 25.0f, -25.0f,  25.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_SPINE },
	{ "cervical",       BODY_CHEST,           {  0.0f,  0.4f, 56.71f }, {  0.0f,  0.4f, 59.6f },   2.2f,  1.5f, JOINT_BALL_AND_SOCKET, 40.0f, -45.0f,  45.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_NECK },
	{ "cranium",        BODY_NECK,            {  0.0f, -0.5f, 61.5f },  {  0.0f, -0.5f, 64.5f },   4.0f,  5.0f, JOINT_BALL_AND_SOCKET, 40.0f, -60.0f,  60.0f, { 0, 0, 1 },          { 0, 0, 0 },       MUSCLE_GROUP_NECK },

	{ "rhumerus",       BODY_CHEST,           { -5.92f, 1.13f, 54.51f },{ -13.19f, 0.94f, 46.41f },2.4f,  2.5f, JOINT_BALL_AND_SOCKET,100.0f, -80.0f,  80.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_ARMS },
	{ "rradius",        BODY_RIGHT_UPPER_ARM, { -13.19f, 0.94f, 46.41f },{ -19.0f, 0.31f, 39.91f }, 2.0f,  1.6f, JOINT_HINGE,            0.0f,  -5.0f, 145.0f, { 0, 0, 0 },          { 0, -1, 0 },      MUSCLE_GROUP_ARMS },
	{ "rhand",          BODY_RIGHT_FOREARM,   { -19.0f, 0.31f, 39.91f },{ -20.6f, 0.2f, 36.2f },   1.7f,  0.6f, JOINT_BALL_AND_SOCKET, 70.0f, -40.0f,  40.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_ARMS },
	{ "lhumerus",       BODY_CHEST,           {  5.92f, 1.13f, 54.51f },{ 13.19f, 0.94f, 46.41f }, 2.4f,  2.5f, JOINT_BALL_AND_SOCKET,100.0f, -80.0f,  80.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_ARMS },
	{ "lradius",        BODY_LEFT_UPPER_ARM,  { 13.19f, 0.94f, 46.41f },{ 19.0f, 0.31f, 39.91f },  2.0f,  1.6f, JOINT_HINGE,            0.0f,  -5.0f, 145.0f, { 0, 0, 0 },          { 0, -1, 0 },      MUSCLE_GROUP_ARMS },
	{ "lhand",          BODY_LEFT_FOREARM,    { 19.0f, 0.31f, 39.91f }, { 20.6f, 0.2f, 36.2f },    1.7f,  0.6f, JOINT_BALL_AND_SOCKET, 70.0f, -40.0f,  40.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_ARMS },

	{ "rfemurYZ",       BODY_PELVIS,          { -3.61f, -0.21f, 35.59f },{ -7.41f, 0.82f, 21.40f },3.6f,  9.0f, JOINT_BALL_AND_SOCKET, 80.0f, -35.0f,  35.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_LEGS },
	{ "rtibia",         BODY_RIGHT_THIGH,     { -7.41f, 0.82f, 21.40f },{ -11.15f, 1.83f, 7.47f }, 2.8f,  4.0f, JOINT_HINGE,            0.0f,  -3.0f, 145.0f, { 0, 0, 0 },          { 0, 1, 0 },       MUSCLE_GROUP_LEGS },
	{ "rtalus",         BODY_RIGHT_SHIN,      { -11.3f, 2.4f, 2.2f },   { -12.6f, -6.0f, 2.2f },   2.0f,  1.2f, JOINT_BALL_AND_SOCKET, 35.0f, -20.0f,  20.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_LEGS },
	{ "lfemurYZ",       BODY_PELVIS,          {  3.61f, -0.21f, 35.59f },{ 7.41f, 0.82f, 21.40f }, 3.6f,  9.0f, JOINT_BALL_AND_SOCKET, 80.0f, -35.0f,  35.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_LEGS },
	{ "ltibia",         BODY_LEFT_THIGH,      {  7.41f, 0.82f, 21.40f },{ 11.15f, 1.83f, 7.47f },  2.8f,  4.0f, JOINT_HINGE,            0.0f,  -3.0f, 145.0f, { 0, 0, 0 },          { 0, 1, 0 },       MUSCLE_GROUP_LEGS },
	{ "ltalus",         BODY_LEFT_SHIN,       {  11.3f, 2.4f, 2.2f },   {  12.6f, -6.0f, 2.2f },   2.0f,  1.2f, JOINT_BALL_AND_SOCKET, 35.0f, -20.0f,  20.0f, { 0, 0, 0 },          { 0, 0, 0 },       MUSCLE_GROUP_LEGS },
};

// Joint friction that slows every joint a little, even when its muscles are relaxed.
static const float PASSIVE_JOINT_DAMPING = 3.0f;

// Default damping for muscles: slightly under critical, so limbs settle without wobbling.
static const float MUSCLE_DAMPING_RATIO = 0.8f;

// Bones that are not physical bodies but must follow a specific body. Bones not listed here
// follow the nearest ancestor that is a body (which works for the face and the eyes, and for Jedi
// Academy's robe tail bones ltail and rtail, which hang from the thighs). Rules for bones a skeleton
// does not have are simply never used: tarsal, mc5 and mc7 exist only in Jedi Outcast's skeleton,
// lhang_tag_bone (caught by "lhang") only in Jedi Academy's.
struct CarriedBoneRule
{
	const char *boneNamePrefix;
	int carrierBody;
};

static const CarriedBoneRule CARRIED_BONE_RULES[] =
{
	{ "model_root",	BODY_PELVIS },
	{ "Motion",		BODY_PELVIS },
	{ "rfemurX",	BODY_RIGHT_THIGH },		// twist helper bones sit halfway along the limb
	{ "lfemurX",	BODY_LEFT_THIGH },
	{ "rtarsal",	BODY_RIGHT_FOOT },
	{ "ltarsal",	BODY_LEFT_FOOT },
	{ "rclavical",	BODY_CHEST },
	{ "lclavical",	BODY_CHEST },
	{ "rhumerusX",	BODY_RIGHT_UPPER_ARM },
	{ "lhumerusX",	BODY_LEFT_UPPER_ARM },
	{ "rradiusX",	BODY_RIGHT_FOREARM },
	{ "lradiusX",	BODY_LEFT_FOREARM },
	{ "r_d",		BODY_RIGHT_HAND },		// right fingers, all named r_d<finger>_j<joint>
	{ "mc7",		BODY_RIGHT_HAND },
	{ "rhang",		BODY_RIGHT_HAND },
	{ "l_d",		BODY_LEFT_HAND },		// left fingers
	{ "mc5",		BODY_LEFT_HAND },
	{ "lhang",		BODY_LEFT_HAND },
};

muscleGroup_t MuscleGroupForBody( int bodyIndex )
{
	if ( bodyIndex < 0 || bodyIndex >= HUMANOID_BODY_COUNT )
	{
		return MUSCLE_GROUP_NONE;
	}
	return HUMANOID_BODIES[bodyIndex].muscleGroup;
}

/*
==================
Conversions between engine bone matrices and transforms
==================
*/

// Bone matrices can carry a uniform scale: the rest poses in the humanoid skeletons of both games
// are scaled by 0.64 (0.696 for the face). The scale cancels out of every calculation the rig
// does, but the rotation must be a pure rotation before it is turned into a quaternion, so each
// column is divided by its length first.
static Transform TransformFromBoneMatrix( const mdxaBone_t &matrix )
{
	float rotationRows[3][3];
	for ( int column = 0; column < 3; column++ )
	{
		const Vector3 columnVector( matrix.matrix[0][column], matrix.matrix[1][column], matrix.matrix[2][column] );
		const float columnLength = Length( columnVector );
		const float inverseLength = ( columnLength > SMALL_NUMBER ) ? 1.0f / columnLength : 1.0f;
		for ( int row = 0; row < 3; row++ )
		{
			rotationRows[row][column] = matrix.matrix[row][column] * inverseLength;
		}
	}
	return Transform( QuaternionFromRotationMatrix( rotationRows ),
					  Vector3( matrix.matrix[0][3], matrix.matrix[1][3], matrix.matrix[2][3] ) );
}

static mdxaBone_t BoneMatrixFromTransform( const Transform &transform )
{
	float rotationRows[3][3];
	RotationMatrixFromQuaternion( transform.rotation, rotationRows );

	mdxaBone_t matrix;
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			matrix.matrix[row][column] = rotationRows[row][column];
		}
	}
	matrix.matrix[0][3] = transform.position.x;
	matrix.matrix[1][3] = transform.position.y;
	matrix.matrix[2][3] = transform.position.z;
	return matrix;
}

/*
==================
Skeleton loading
==================
*/

static const int MAXIMUM_CACHED_SKELETONS = 4;
static Skeleton cachedSkeletons[MAXIMUM_CACHED_SKELETONS];
static int cachedSkeletonCount = 0;

int Skeleton::FindBone( const char *boneName ) const
{
	for ( int boneIndex = 0; boneIndex < boneCount; boneIndex++ )
	{
		if ( !Q_stricmp( bones[boneIndex].name, boneName ) )
		{
			return boneIndex;
		}
	}
	return -1;
}

void ClearSkeletonCache()
{
	cachedSkeletonCount = 0;
}

// Reads the bone hierarchy and rest pose from a .gla file. Only the start of the file (header and
// skeleton) is read; the animation frames that follow are skipped.
static bool ReadSkeletonFile( const char *glaName, Skeleton &skeleton )
{
	char filePath[MAX_QPATH];
	Com_sprintf( filePath, sizeof( filePath ), "%s.gla", glaName );

	fileHandle_t fileHandle;
	const int fileLength = gi.FS_FOpenFile( filePath, &fileHandle, FS_READ );
	if ( fileLength <= (int)sizeof( mdxaHeader_t ) || !fileHandle )
	{
		if ( fileHandle )
		{
			gi.FS_FCloseFile( fileHandle );
		}
		return false;
	}

	mdxaHeader_t header;
	gi.FS_Read( &header, sizeof( header ), fileHandle );

	if ( header.ident != MDXA_IDENT || header.numBones <= 0 || header.numBones > MAXIMUM_SKELETON_BONES )
	{
		gi.FS_FCloseFile( fileHandle );
		return false;
	}

	// The skeleton sits between the header and the animation frames.
	int skeletonEnd = ( header.ofsFrames > header.ofsSkel ) ? header.ofsFrames : header.ofsEnd;
	if ( skeletonEnd > fileLength )
	{
		skeletonEnd = fileLength;
	}
	const int remainingSize = skeletonEnd - (int)sizeof( header );
	if ( remainingSize <= 0 )
	{
		gi.FS_FCloseFile( fileHandle );
		return false;
	}

	// The engine's bone offsets are measured from the start of the file, so keep the header in front.
	unsigned char *fileStart = new unsigned char[skeletonEnd];
	memcpy( fileStart, &header, sizeof( header ) );
	gi.FS_Read( fileStart + sizeof( header ), remainingSize, fileHandle );
	gi.FS_FCloseFile( fileHandle );

	const mdxaSkelOffsets_t *boneOffsets = (const mdxaSkelOffsets_t *)( fileStart + sizeof( mdxaHeader_t ) );
	bool success = true;

	Q_strncpyz( skeleton.glaName, glaName, sizeof( skeleton.glaName ) );
	skeleton.boneCount = header.numBones;

	for ( int boneIndex = 0; boneIndex < header.numBones && success; boneIndex++ )
	{
		const int boneStart = (int)sizeof( mdxaHeader_t ) + boneOffsets->offsets[boneIndex];
		if ( boneStart < 0 || boneStart + (int)sizeof( mdxaSkel_t ) > skeletonEnd )
		{
			success = false;
			break;
		}

		const mdxaSkel_t *fileBone = (const mdxaSkel_t *)( fileStart + boneStart );
		SkeletonBone &bone = skeleton.bones[boneIndex];
		Q_strncpyz( bone.name, fileBone->name, sizeof( bone.name ) );
		bone.parentIndex = fileBone->parent;
		bone.basePose = TransformFromBoneMatrix( fileBone->BasePoseMat );

		if ( bone.parentIndex >= header.numBones )
		{
			success = false;
		}
	}

	delete[] fileStart;
	return success;
}

const Skeleton *LoadSkeleton( const char *glaName )
{
	if ( !glaName || !glaName[0] )
	{
		return NULL;
	}

	for ( int cacheIndex = 0; cacheIndex < cachedSkeletonCount; cacheIndex++ )
	{
		if ( !Q_stricmp( cachedSkeletons[cacheIndex].glaName, glaName ) )
		{
			return &cachedSkeletons[cacheIndex];
		}
	}

	if ( cachedSkeletonCount >= MAXIMUM_CACHED_SKELETONS )
	{
		return NULL;
	}

	Skeleton &newSkeleton = cachedSkeletons[cachedSkeletonCount];
	if ( !ReadSkeletonFile( glaName, newSkeleton ) )
	{
		return NULL;
	}
	cachedSkeletonCount++;
	return &newSkeleton;
}

/*
==================
Reading the current pose
==================
*/

void ReadCurrentBoneFrames( CGhoul2Info_v &ghoul2, int modelIndex, const Skeleton &skeleton, int time,
							Transform *modelSpaceFrames, bool *frameIsValid )
{
	CGhoul2Info *ghoul2Info = &ghoul2[modelIndex];

	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		const char *boneName = skeleton.bones[boneIndex].name;
		frameIsValid[boneIndex] = false;
		modelSpaceFrames[boneIndex] = skeleton.bones[boneIndex].basePose;

		// Bolts look for a surface with the requested name before looking for a bone. If this
		// model has a surface with the same name, a bolt would report the surface instead.
		if ( gi.G2API_GetSurfaceIndex( ghoul2Info, boneName ) != -1 )
		{
			continue;
		}

		const int boltIndex = gi.G2API_AddBolt( ghoul2Info, boneName );
		if ( boltIndex < 0 )
		{
			continue;
		}

		// No angles, origin or scale: the result is the bone's frame in model space.
		mdxaBone_t boltMatrix;
		if ( gi.G2API_GetBoltMatrix( ghoul2, modelIndex, boltIndex, &boltMatrix, vec3_origin, vec3_origin, time, NULL, vec3_origin ) )
		{
			modelSpaceFrames[boneIndex] = TransformFromBoneMatrix( boltMatrix );
			frameIsValid[boneIndex] = true;
		}
	}
}

/*
==================
Building the ragdoll
==================
*/

static Vector3 ArrayToVector( const float *values )
{
	return Vector3::FromArray( values );
}

// Inverse inertia of a capsule (treated as a cylinder) about its centre, in a frame where the
// capsule runs along unitAxis.
static Matrix3 CapsuleInverseInertia( float mass, float length, float radius, const Vector3 &unitAxis )
{
	const float inertiaAboutAxis = 0.5f * mass * radius * radius;
	const float inertiaAcrossAxis = mass * ( 3.0f * radius * radius + length * length ) / 12.0f;
	const float inverseAbout = 1.0f / inertiaAboutAxis;
	const float inverseAcross = 1.0f / inertiaAcrossAxis;
	const float axisComponents[3] = { unitAxis.x, unitAxis.y, unitAxis.z };

	// inverse = inverseAcross * (I - u u^T) + inverseAbout * u u^T
	Matrix3 inverseInertia;
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			const float identity = ( row == column ) ? 1.0f : 0.0f;
			const float outerProduct = axisComponents[row] * axisComponents[column];
			inverseInertia.element[row][column] = inverseAcross * ( identity - outerProduct ) + inverseAbout * outerProduct;
		}
	}
	return inverseInertia;
}

// Closest distance between two segments, sampled finely enough for the rest pose check below.
static float RestPoseCapsuleGap( const BodyDefinition &first, const BodyDefinition &second )
{
	const Vector3 firstStart = ArrayToVector( first.capsuleStart );
	const Vector3 firstEnd = ArrayToVector( first.capsuleEnd );
	const Vector3 secondStart = ArrayToVector( second.capsuleStart );
	const Vector3 secondEnd = ArrayToVector( second.capsuleEnd );

	const int sampleCount = 16;
	float closestDistance = 1.0e9f;
	for ( int firstSample = 0; firstSample <= sampleCount; firstSample++ )
	{
		const Vector3 firstPoint = firstStart + ( firstEnd - firstStart ) * ( (float)firstSample / sampleCount );
		for ( int secondSample = 0; secondSample <= sampleCount; secondSample++ )
		{
			const Vector3 secondPoint = secondStart + ( secondEnd - secondStart ) * ( (float)secondSample / sampleCount );
			const float distance = Length( firstPoint - secondPoint );
			if ( distance < closestDistance )
			{
				closestDistance = distance;
			}
		}
	}
	return closestDistance - first.radius - second.radius;
}

// Decides which pairs of body parts collide with each other. Parts joined by a joint never do
// (the joint limits keep them apart), and neither do parts whose capsules already overlap in the
// rest pose, such as the two thighs at the hips, because they would push each other forever.
static void ChooseSelfCollisionPairs( Simulation &simulation )
{
	const float restPoseClearance = 0.5f;

	for ( int firstBody = 0; firstBody < HUMANOID_BODY_COUNT; firstBody++ )
	{
		for ( int secondBody = 0; secondBody < HUMANOID_BODY_COUNT; secondBody++ )
		{
			const bool sameBody = ( firstBody == secondBody );
			const bool joined = HUMANOID_BODIES[firstBody].parentBody == secondBody || HUMANOID_BODIES[secondBody].parentBody == firstBody;
			const bool overlapAtRest = RestPoseCapsuleGap( HUMANOID_BODIES[firstBody], HUMANOID_BODIES[secondBody] ) < restPoseClearance;
			simulation.bodyPairCollides[firstBody][secondBody] = !sameBody && !joined && !overlapAtRest;
		}
	}
}

// The body that carries a bone that is not itself a physical body.
static int FindCarrierBody( const Skeleton &skeleton, int boneIndex, const int *bodyForBone )
{
	const char *boneName = skeleton.bones[boneIndex].name;
	for ( size_t ruleIndex = 0; ruleIndex < ARRAY_LEN( CARRIED_BONE_RULES ); ruleIndex++ )
	{
		const CarriedBoneRule &rule = CARRIED_BONE_RULES[ruleIndex];
		if ( !Q_stricmpn( boneName, rule.boneNamePrefix, (int)strlen( rule.boneNamePrefix ) ) )
		{
			return rule.carrierBody;
		}
	}

	// Otherwise follow the nearest ancestor that is a body.
	for ( int ancestor = skeleton.bones[boneIndex].parentIndex; ancestor >= 0; ancestor = skeleton.bones[ancestor].parentIndex )
	{
		if ( bodyForBone[ancestor] >= 0 )
		{
			return bodyForBone[ancestor];
		}
	}
	return BODY_PELVIS;
}

bool BuildHumanoidRagdoll( Rig &rig, Simulation &simulation, const Skeleton &skeleton,
						   const Transform *currentFrames, const bool *frameIsValid,
						   const Transform &modelToWorld, float modelScale )
{
	rig.skeleton = &skeleton;
	rig.modelScale = modelScale;

	int bodyForBone[MAXIMUM_SKELETON_BONES];
	for ( int boneIndex = 0; boneIndex < MAXIMUM_SKELETON_BONES; boneIndex++ )
	{
		bodyForBone[boneIndex] = -1;
	}

	// Every body needs its bone, and that bone's current pose.
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const int boneIndex = skeleton.FindBone( HUMANOID_BODIES[bodyIndex].boneName );
		if ( boneIndex < 0 || !frameIsValid[boneIndex] )
		{
			return false;
		}
		rig.bodyBoneIndex[bodyIndex] = boneIndex;
		bodyForBone[boneIndex] = bodyIndex;
	}

	const float massScale = modelScale * modelScale * modelScale;
	const Vector3 modelLeftAxis( 1.0f, 0.0f, 0.0f );
	const Vector3 modelForwardAxis( 0.0f, -1.0f, 0.0f );

	simulation = Simulation();
	simulation.bodyCount = HUMANOID_BODY_COUNT;
	simulation.jointCount = HUMANOID_BODY_COUNT - 1;

	// Bodies.
	Transform boneWorldTransforms[HUMANOID_BODY_COUNT];
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const BodyDefinition &definition = HUMANOID_BODIES[bodyIndex];
		const Transform &basePose = skeleton.bones[rig.bodyBoneIndex[bodyIndex]].basePose;
		const Transform &currentFrame = currentFrames[rig.bodyBoneIndex[bodyIndex]];

		// Where the bone is in the world right now.
		boneWorldTransforms[bodyIndex] = Transform(
			NormalizedQuaternion( modelToWorld.rotation * currentFrame.rotation ),
			modelToWorld.TransformPoint( currentFrame.position * modelScale ) );

		// The capsule in the bone's own space. Rest pose points on the bone move rigidly with it.
		const Vector3 capsuleStart = basePose.InverseTransformPoint( ArrayToVector( definition.capsuleStart ) ) * modelScale;
		const Vector3 capsuleEnd = basePose.InverseTransformPoint( ArrayToVector( definition.capsuleEnd ) ) * modelScale;
		const Vector3 centreOfMass = ( capsuleStart + capsuleEnd ) * 0.5f;
		const Vector3 capsuleAxis = capsuleEnd - capsuleStart;

		rig.centreOfMassInBoneSpace[bodyIndex] = centreOfMass;
		rig.lateralAxisInBodySpace[bodyIndex] = Normalized( InverseRotate( basePose.rotation, modelLeftAxis ) );
		rig.forwardAxisInBodySpace[bodyIndex] = Normalized( InverseRotate( basePose.rotation, modelForwardAxis ) );
		rig.bodyMass[bodyIndex] = definition.mass * massScale;

		RigidBody &body = simulation.bodies[bodyIndex];
		body.orientation = boneWorldTransforms[bodyIndex].rotation;
		body.position = boneWorldTransforms[bodyIndex].TransformPoint( centreOfMass );
		body.linearVelocity = Vector3();
		body.angularVelocity = Vector3();
		body.inverseMass = 1.0f / rig.bodyMass[bodyIndex];
		body.inverseInertiaLocal = CapsuleInverseInertia( rig.bodyMass[bodyIndex], Length( capsuleAxis ),
														  definition.radius * modelScale, Normalized( capsuleAxis ) );
		body.sphereCount = 2;
		body.spheres[0].localCentre = capsuleStart - centreOfMass;
		body.spheres[0].radius = definition.radius * modelScale;
		body.spheres[1].localCentre = capsuleEnd - centreOfMass;
		body.spheres[1].radius = definition.radius * modelScale;
		body.worldDrive.targetOrientation = body.orientation;
		body.worldDrive.naturalFrequency = 0.0f;
		body.worldDrive.dampingRatio = 1.0f;
		body.positionDrive.enabled = false;
		body.positionDrive.stiffness = 0.0f;
		body.positionDrive.damping = 0.0f;
		body.positionDrive.horizontalShare = 1.0f;
		body.gravityScale = 1.0f;
		body.substepStartPosition = body.position;
		body.substepStartOrientation = body.orientation;
	}

	// Joints, one per non-root body, connecting it to its parent at the child bone's origin.
	for ( int bodyIndex = 1; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const BodyDefinition &definition = HUMANOID_BODIES[bodyIndex];
		const int parentIndex = definition.parentBody;
		const RigidBody &parent = simulation.bodies[parentIndex];
		const RigidBody &child = simulation.bodies[bodyIndex];
		const Transform &parentBasePose = skeleton.bones[rig.bodyBoneIndex[parentIndex]].basePose;
		const Transform &childBasePose = skeleton.bones[rig.bodyBoneIndex[bodyIndex]].basePose;

		Joint &joint = simulation.joints[JointIndexForBody( bodyIndex )];
		joint.type = definition.jointType;
		joint.parentBodyIndex = parentIndex;
		joint.childBodyIndex = bodyIndex;

		const Vector3 anchorInWorld = boneWorldTransforms[bodyIndex].position;
		joint.parentAnchor = InverseRotate( parent.orientation, anchorInWorld - parent.position );
		joint.childAnchor = InverseRotate( child.orientation, anchorInWorld - child.position );

		joint.neutralRelativeOrientation = NormalizedQuaternion( Conjugate( parentBasePose.rotation ) * childBasePose.rotation );

		// The limb's direction in the rest pose, used as the twist axis or to find the hinge axis.
		Vector3 limbDirection = ArrayToVector( definition.twistAxis );
		if ( LengthSquared( limbDirection ) < SMALL_NUMBER )
		{
			limbDirection = ArrayToVector( definition.capsuleEnd ) - ArrayToVector( definition.capsuleStart );
		}
		limbDirection = Normalized( limbDirection );

		if ( definition.jointType == JOINT_HINGE )
		{
			// Turning the limb about (limb x bend) by a positive angle moves its tip toward the
			// bend direction, so positive hinge angles always mean "bending".
			const Vector3 hingeAxis = Normalized( Cross( limbDirection, ArrayToVector( definition.bendDirection ) ) );
			joint.childAxis = Normalized( InverseRotate( childBasePose.rotation, hingeAxis ) );
			joint.childReference = Normalized( InverseRotate( childBasePose.rotation, limbDirection ) );
			joint.swingLimit = 0.0f;
		}
		else
		{
			joint.childAxis = Normalized( InverseRotate( childBasePose.rotation, limbDirection ) );
			joint.childReference = AnyPerpendicular( joint.childAxis );
			joint.swingLimit = DegreesToRadians( definition.swingLimitDegrees );
		}
		joint.angleMinimum = DegreesToRadians( definition.angleMinimumDegrees );
		joint.angleMaximum = DegreesToRadians( definition.angleMaximumDegrees );
		joint.passiveDamping = PASSIVE_JOINT_DAMPING;

		joint.muscle.targetRelativeOrientation = NormalizedQuaternion( Conjugate( parent.orientation ) * child.orientation );
		joint.muscle.naturalFrequency = 0.0f;
		joint.muscle.dampingRatio = MUSCLE_DAMPING_RATIO;
	}

	ChooseSelfCollisionPairs( simulation );

	// Every other bone follows a body, keeping the offset it has right now (so curled fingers
	// stay curled). Bones that could not be read keep their rest pose offset instead.
	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		if ( bodyForBone[boneIndex] >= 0 )
		{
			rig.carrierBody[boneIndex] = bodyForBone[boneIndex];
			rig.offsetFromCarrierBone[boneIndex] = Transform();
			continue;
		}

		const int carrierBody = FindCarrierBody( skeleton, boneIndex, bodyForBone );
		const int carrierBone = rig.bodyBoneIndex[carrierBody];
		rig.carrierBody[boneIndex] = carrierBody;

		if ( frameIsValid[boneIndex] )
		{
			rig.offsetFromCarrierBone[boneIndex] = Compose( Inverse( currentFrames[carrierBone] ), currentFrames[boneIndex] );
		}
		else
		{
			rig.offsetFromCarrierBone[boneIndex] = Compose( Inverse( skeleton.bones[carrierBone].basePose ), skeleton.bones[boneIndex].basePose );
		}
	}

	return true;
}

/*
==================
Writing the pose back to the model
==================
*/

Transform BoneWorldTransformForBody( const Rig &rig, int bodyIndex, const Transform &bodyTransform )
{
	return Transform( bodyTransform.rotation,
					  bodyTransform.position - Rotate( bodyTransform.rotation, rig.centreOfMassInBoneSpace[bodyIndex] ) );
}

void ComputeBoneFrames( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld, Transform *boneFrames )
{
	const Skeleton &skeleton = *rig.skeleton;
	const Transform worldToModel = Inverse( modelToWorld );
	const float inverseScale = 1.0f / rig.modelScale;

	Transform bodyBoneFrames[HUMANOID_BODY_COUNT];
	for ( int bodyIndex = 0; bodyIndex < HUMANOID_BODY_COUNT; bodyIndex++ )
	{
		const Transform boneInWorld = BoneWorldTransformForBody( rig, bodyIndex, bodyTransforms[bodyIndex] );
		const Transform boneInModel = Compose( worldToModel, boneInWorld );
		bodyBoneFrames[bodyIndex] = Transform( boneInModel.rotation, boneInModel.position * inverseScale );
	}

	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		boneFrames[boneIndex] = Compose( bodyBoneFrames[rig.carrierBody[boneIndex]], rig.offsetFromCarrierBone[boneIndex] );
	}
}

void PrintBoneOverrideErrors( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld,
							  CGhoul2Info_v &ghoul2, int modelIndex, int time )
{
	const Skeleton &skeleton = *rig.skeleton;
	Transform desiredFrames[MAXIMUM_SKELETON_BONES];
	Transform drawnFrames[MAXIMUM_SKELETON_BONES];
	bool drawnFrameIsValid[MAXIMUM_SKELETON_BONES];
	ComputeBoneFrames( rig, bodyTransforms, modelToWorld, desiredFrames );
	ReadCurrentBoneFrames( ghoul2, modelIndex, skeleton, time, drawnFrames, drawnFrameIsValid );

	gi.Printf( "bones more than 0.5 units or 2 degrees from their target:\n" );
	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		if ( !drawnFrameIsValid[boneIndex] )
		{
			continue;
		}
		const float positionError = Length( drawnFrames[boneIndex].position - desiredFrames[boneIndex].position );
		const Vector3 rotationError = RotationVectorFromQuaternion( drawnFrames[boneIndex].rotation * Conjugate( desiredFrames[boneIndex].rotation ) );
		const float angleErrorDegrees = RadiansToDegrees( Length( rotationError ) );
		if ( positionError > 0.5f || angleErrorDegrees > 2.0f )
		{
			const int parentIndex = skeleton.bones[boneIndex].parentIndex;
			gi.Printf( "  %-16s (parent %-14s) position %6.1f  angle %6.1f\n", skeleton.bones[boneIndex].name,
					   parentIndex >= 0 ? skeleton.bones[parentIndex].name : "-", positionError, angleErrorDegrees );
		}
	}
}

void WriteBoneOverrides( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld,
						 CGhoul2Info *ghoul2Info, int time )
{
	Transform boneFrames[MAXIMUM_SKELETON_BONES];
	ComputeBoneFrames( rig, bodyTransforms, modelToWorld, boneFrames );
	WriteBoneFrames( rig, boneFrames, ghoul2Info, time );
}

void WriteBoneFrames( const Rig &rig, const Transform *boneFrames, CGhoul2Info *ghoul2Info, int time )
{
	const Skeleton &skeleton = *rig.skeleton;

	// The engine draws bone i with its "final" matrix, which is the bone's frame multiplied by the
	// inverse of its rest pose. With BONE_ANGLES_PREMULT the final matrix becomes
	// parentFinal * override, so the override is inverse(parentFinal) * final.
	Transform finalMatrices[MAXIMUM_SKELETON_BONES];
	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		finalMatrices[boneIndex] = Compose( boneFrames[boneIndex], Inverse( skeleton.bones[boneIndex].basePose ) );
	}

	// The root bone is not relative to a parent but to the skeleton's root matrix. Despite being
	// named identityMatrix in the renderer (tr_ghoul2.cpp), that matrix turns the model 90 degrees
	// about its up axis, so the root override has to undo that turn.
	const Transform inverseRootMatrix = Inverse( Transform( QuaternionFromAxisAngle( Vector3( 0.0f, 0.0f, 1.0f ), PI_RADIANS * 0.5f ), Vector3() ) );

	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		const int parentIndex = skeleton.bones[boneIndex].parentIndex;
		const Transform boneOverride = ( parentIndex < 0 )
			? Compose( inverseRootMatrix, finalMatrices[boneIndex] )
			: Compose( Inverse( finalMatrices[parentIndex] ), finalMatrices[boneIndex] );

		// Set by name: the renderer does not export the by-index version of this call, and the
		// name lookup also stays correct if other game code adds or removes bone list entries.
		gi.G2API_SetBoneAnglesMatrix( ghoul2Info, skeleton.bones[boneIndex].name, BoneMatrixFromTransform( boneOverride ),
									  BONE_ANGLES_PREMULT, NULL, 0, time );
	}
}

void ClearBoneOverrides( const Rig &rig, CGhoul2Info *ghoul2Info )
{
	ClearSkeletonBoneOverrides( *rig.skeleton, ghoul2Info );
}

void ClearSkeletonBoneOverrides( const Skeleton &skeleton, CGhoul2Info *ghoul2Info )
{
	for ( int boneIndex = 0; boneIndex < skeleton.boneCount; boneIndex++ )
	{
		const int boneListIndex = gi.G2API_GetBoneIndex( ghoul2Info, skeleton.bones[boneIndex].name, qfalse );
		if ( boneListIndex >= 0 )
		{
			gi.G2API_StopBoneAnglesIndex( ghoul2Info, boneListIndex );
		}
	}
}

} // namespace ragdoll
