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

// g_ragdoll_rig.h -- connects the ragdoll physics to a Ghoul2 skeleton.
//
// A rig knows three things:
//   1. The skeleton: every bone's name, parent and base (rest) pose, read from the .gla file.
//   2. The humanoid body layout: which 18 bones get a physical body, how big each body is,
//      how heavy it is and how its joint may bend.
//   3. How every other bone (fingers, face, twist bones) follows one of those 18 bodies.
//
// The rig writes an override for every bone of the skeleton, not only for the 18 physical ones.
// Each override uses BONE_ANGLES_PREMULT, which replaces the bone's transform relative to its
// parent completely, so the rendered model matches the simulation exactly and the animation is
// ignored. Jedi Outcast's humanoid skeleton needed this because it is almost flat (shins, feet and
// hands are direct children of the pelvis or the chest, so rotating only the physical bones tore
// the limbs apart); Jedi Academy's 53 bone skeleton is a proper hierarchy, but overriding every
// bone works for both and keeps the twist, finger, face and robe tail bones on their bodies.

#ifndef G_RAGDOLL_RIG_H
#define G_RAGDOLL_RIG_H

#include "g_local.h"
#include "g_ragdoll_solver.h"

namespace ragdoll
{

const int MAXIMUM_SKELETON_BONES = 128;

struct SkeletonBone
{
	char name[MAX_QPATH];
	int parentIndex;			// -1 for the root bone
	Transform basePose;			// the bone's frame in model space when the model is in its rest pose
};

struct Skeleton
{
	char glaName[MAX_QPATH];
	int boneCount;
	SkeletonBone bones[MAXIMUM_SKELETON_BONES];

	int FindBone( const char *boneName ) const;
};

// Loads (or returns the cached copy of) the skeleton stored in "<glaName>.gla".
// Returns NULL if the file cannot be read.
const Skeleton *LoadSkeleton( const char *glaName );

// Forgets all cached skeletons (for example when the level changes).
void ClearSkeletonCache();

// The physical bodies of the humanoid ragdoll. Body 0 (the pelvis) is the root; every other body
// is connected to its parent by the joint with index (body index - 1).
enum humanoidBody_t
{
	BODY_PELVIS,
	BODY_LOWER_SPINE,
	BODY_MIDDLE_SPINE,
	BODY_CHEST,
	BODY_NECK,
	BODY_HEAD,
	BODY_RIGHT_UPPER_ARM,
	BODY_RIGHT_FOREARM,
	BODY_RIGHT_HAND,
	BODY_LEFT_UPPER_ARM,
	BODY_LEFT_FOREARM,
	BODY_LEFT_HAND,
	BODY_RIGHT_THIGH,
	BODY_RIGHT_SHIN,
	BODY_RIGHT_FOOT,
	BODY_LEFT_THIGH,
	BODY_LEFT_SHIN,
	BODY_LEFT_FOOT,
	HUMANOID_BODY_COUNT
};

// Joints are grouped so behaviours can tense or relax whole parts of the body at once.
enum muscleGroup_t
{
	MUSCLE_GROUP_NONE,		// the pelvis has no joint
	MUSCLE_GROUP_SPINE,
	MUSCLE_GROUP_NECK,
	MUSCLE_GROUP_ARMS,
	MUSCLE_GROUP_LEGS
};

inline int JointIndexForBody( int bodyIndex )	{ return bodyIndex - 1; }
inline int BodyIndexForJoint( int jointIndex )	{ return jointIndex + 1; }

muscleGroup_t MuscleGroupForBody( int bodyIndex );

// Per-ragdoll information about how the skeleton maps onto the simulation.
struct Rig
{
	const Skeleton *skeleton;
	float modelScale;

	int bodyBoneIndex[HUMANOID_BODY_COUNT];				// skeleton bone driven by each body
	Vector3 centreOfMassInBoneSpace[HUMANOID_BODY_COUNT];	// offset from the bone's origin to the body's centre of mass
	Vector3 lateralAxisInBodySpace[HUMANOID_BODY_COUNT];	// the character's left-right axis, used for bending behaviours
	Vector3 forwardAxisInBodySpace[HUMANOID_BODY_COUNT];	// the direction the character faces, used to tell face up from face down
	float bodyMass[HUMANOID_BODY_COUNT];

	// For every skeleton bone: the body that carries it and its fixed offset from that body's bone.
	int carrierBody[MAXIMUM_SKELETON_BONES];
	Transform offsetFromCarrierBone[MAXIMUM_SKELETON_BONES];
};

// Reads the current model space frame of every bone (as currently animated and rendered).
// frameIsValid[i] is false for bones that could not be read; they fall back to the rest pose.
void ReadCurrentBoneFrames( CGhoul2Info_v &ghoul2, int modelIndex, const Skeleton &skeleton, int time,
							Transform *modelSpaceFrames, bool *frameIsValid );

// Builds the physical bodies and joints from the current pose.
//   currentFrames   model space bone frames from ReadCurrentBoneFrames
//   modelToWorld    where the model is currently drawn
// Returns false if the skeleton is missing a required bone.
bool BuildHumanoidRagdoll( Rig &rig, Simulation &simulation, const Skeleton &skeleton,
						   const Transform *currentFrames, const bool *frameIsValid,
						   const Transform &modelToWorld, float modelScale );

// Where body bodyIndex's bone would be if the body were at bodyTransform (centre of mass frame).
Transform BoneWorldTransformForBody( const Rig &rig, int bodyIndex, const Transform &bodyTransform );

// Every skeleton bone's frame in (unscaled) model space for the pose given by the body transforms.
//   bodyTransforms  world transform of each body's centre of mass
//   modelToWorld    where the model will be drawn this frame
void ComputeBoneFrames( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld, Transform *boneFrames );

// Writes bone overrides so the model is drawn with every bone at the given model space frame.
void WriteBoneFrames( const Rig &rig, const Transform *boneFrames, CGhoul2Info *ghoul2Info, int time );

// Writes bone overrides so the model is drawn in the pose given by the body transforms
// (ComputeBoneFrames followed by WriteBoneFrames).
void WriteBoneOverrides( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld,
						 CGhoul2Info *ghoul2Info, int time );

// Removes every bone override the rig wrote, giving control back to the animation.
void ClearBoneOverrides( const Rig &rig, CGhoul2Info *ghoul2Info );

// The same for a model that has no rig (for example one restored from a saved game).
void ClearSkeletonBoneOverrides( const Skeleton &skeleton, CGhoul2Info *ghoul2Info );

// The same three calls for only some of the bones, for code that poses part of a living character
// (the saber arm reaching for a blaster bolt, see g_saber_reach.h) and leaves the rest animated.
//   ReadSomeBoneFrames     reads the listed bones' current frames into modelSpaceFrames[boneIndex]
//   WriteSomeBoneFrames    draws the listed bones at boneFrames[boneIndex]; the parent of every listed
//                          bone must hold its current frame in boneFrames (read, or listed itself)
//   ClearSomeBoneOverrides removes the listed bones' overrides
void ReadSomeBoneFrames( CGhoul2Info_v &ghoul2, int modelIndex, const Skeleton &skeleton, int time,
						 const int *boneIndices, int boneCount, Transform *modelSpaceFrames, bool *frameIsValid );
void WriteSomeBoneFrames( const Skeleton &skeleton, const int *boneIndices, int boneCount, const Transform *boneFrames,
						  CGhoul2Info *ghoul2Info, int time );
void ClearSomeBoneOverrides( const Skeleton &skeleton, const int *boneIndices, int boneCount, CGhoul2Info *ghoul2Info );

// Debugging aid: reads back where the engine now draws every bone and prints the bones that are
// furthest from where WriteBoneOverrides asked them to be (position in units, angle in degrees).
void PrintBoneOverrideErrors( const Rig &rig, const Transform *bodyTransforms, const Transform &modelToWorld,
							  CGhoul2Info_v &ghoul2, int modelIndex, int time );

} // namespace ragdoll

#endif // G_RAGDOLL_RIG_H
