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

// g_slice.cpp -- lightsaber slicing for Jedi Academy (ported from Jedi Outcast's
// codeJK2/game/g_slice.cpp). See g_slice.h, which also lists what differs from Jedi Outcast.
//
// A cut is made the first time the character is drawn after the blow:
//   1. The character's mesh is posed as it is drawn, and its ragdoll bodies are built in the same pose.
//   2. DivideBodies works out what the cut divides. A body the plane crosses within the blade's reach
//      is split in two: across its length (an arm cut at the bicep) or along it (the spine in a
//      vertical cleave). The bodies and parts of bodies are then grouped into pieces through the
//      joints that still hold them together; a joint stays with the part of a split body on its side
//      of the cut. A body the plane crosses out of the blade's reach is not cut, and holds together
//      whatever is joined to it.
//   3. Every vertex goes to the piece of the body (or part of a body) that moves it most, and
//      slicing::SplitMesh divides the mesh between the pieces, closing the cut faces.
//   4. Each piece becomes a limp ragdoll, its mesh attached to its bodies by the bones' weights. All
//      pieces are simulated together, so they collide with each other.

#include "g_local.h"
#include "../cgame/cg_local.h"
#include "g_slice.h"
#include "g_slice_mesh.h"
#include "g_ragdoll.h"
#include "g_ragdoll_rig.h"
#include "g_ragdoll_solver.h"
#include "wp_saber.h"
#include "g_functions.h"
#include "Q3_Interface.h"

#include <map>
#include <math.h>
#include <sstream>
#include <string>
#include <vector>

using ragdoll::Vector3;
using ragdoll::Quaternion;
using ragdoll::Transform;
using ragdoll::RigidBody;
using ragdoll::Joint;
using ragdoll::Simulation;
using ragdoll::SimulationSettings;
using ragdoll::WorldSweepResult;

extern cvar_t *g_ragdollDebug;
extern void WP_DropWeapon( gentity_t *dropper, vec3_t velocity );
extern qboolean WP_SaberLose( gentity_t *self, vec3_t throwDir );
extern void WP_RemoveSaber( gentity_t *ent, int saberNum );
extern qboolean WP_SaberBladeUseSecondBladeStyle( saberInfo_t *saber, int bladeNum );
extern Vehicle_t *G_IsRidingVehicle( gentity_t *ent );

// Jedi Outcast also stopped slicing in its censored version (ui_iscensored); Jedi Academy has no
// censored version, and its game no longer reads that cvar.
static cvar_t *g_saberSlicing;

/*
==================
Tuning
==================
*/

static const int MAXIMUM_PIECES = ragdoll::MAXIMUM_SCENE_SIMULATIONS;	// the oldest piece is recycled after this many

static const float PHYSICS_STEP_SECONDS = 1.0f / 60.0f;
static const int PHYSICS_SUBSTEPS = 8;
static const float LONGEST_FRAME_SECONDS = 0.1f;		// longer frames (hitches, pauses) are shortened to this

// The console command's cuts.
static const float HORIZONTAL_CUT_HEIGHT = 8.0f;		// above the character's origin: just above the hips
static const float DIAGONAL_CUT_RISE = 0.8f;			// how steeply a diagonal cut climbs across the body
static const float SLICE_PIECE_SEARCH_DISTANCE = 64.0f;	// "slice piece" cuts a piece this close to where the user looks

// Lightsaber cuts.
static const float SMALLEST_SIDEWAYS_SWEEP = 1.0f;		// the blade must move at least this far sideways (on average) to cut, not just stab
static const float REACH_MARGIN = 8.0f;					// the swing carries on after the blow, so the blade reaches a little past its tip
static const float STRUCK_BODY_DISTANCE = 4.0f;			// a body this close to where the blade struck is always cut
static const int SMALLEST_MAIMING_DAMAGE = 10;			// a blow that does not kill must do this much damage to take an arm off
static const int BLADE_SWEEP_SAMPLES = 4;				// blade positions checked between frames when looking for pieces to cut again
static const int SAME_SWING_MILLISECONDS = 500;			// a piece is not cut again by the blade that made it until its swing is over

// Ghoul2 and saved games.
static const int GHOUL2_SURFACE_OFF = 0x00000002;		// G2SURFACEFLAG_OFF (rd-common/mdx_format.h): a surface neither drawn nor hit
static const char *const SAVED_CUTS_VARIABLE = "slicing_maimedCharacters";	// the script variables carrying them in saved games
static const size_t LONGEST_SAVED_CUTS_TEXT = 1000;		// a saved script variable must be shorter than 1024 characters
static const int MOST_SAVED_CUTS_VARIABLES = 8;			// the script system holds only 32 variables in all

// How the pieces start moving once cut.
static const float PIECE_SEPARATION_SPEED = 25.0f;		// units per second away from the cut
static const float PIECE_SLIDE_SPEED = 40.0f;			// pieces above a level or slanted cut also slide off
static const float PIECE_TUMBLE_SPEED = 1.5f;			// radians per second (pieces with no ragdoll bodies only)

// Dividing the ragdoll bodies.
static const float SMALLEST_BODY_SHARE = 0.15f;			// a body cut very near its edge keeps at least this much of its mass
static const float SLIVER_SHARE = 0.15f;				// a side getting less than this share of a body's thickness gets none of it
static const float ACROSS_CUT_MARGIN = 0.25f;			// an end this many radii from the cut counts as on its side
static const float SEVERED_JOINT_DISTANCE = 1.0f;		// a joint closer than this to the cut is cut right through

// Pieces that have no ragdoll bodies (only possible if the skeleton could not be read) are rigid
// bodies sized from their bounding boxes.
static const float PIECE_DENSITY = 0.0008f;				// mass per cubic unit of the bounding box
static const float LIGHTEST_PIECE_MASS = 3.0f;
static const float HEAVIEST_PIECE_MASS = 40.0f;
static const float THINNEST_PIECE_RADIUS = 2.0f;
static const float THICKEST_PIECE_RADIUS = 10.0f;

// Coming to rest: after a while the pieces are slowed more and more, then stop moving altogether.
static const float SETTLING_STARTS_SECONDS = 2.0f;
static const float SECONDS_TO_FULL_SETTLING = 2.0f;
static const float FULL_SETTLING_DAMPING = 4.0f;		// per second
static const float PIECES_FREEZE_SECONDS = 10.0f;

static const Vector3 WORLD_UP( 0.0f, 0.0f, 1.0f );

/*
==================
State
==================
*/

// A cut asked for by a lightsaber blow or the console command, made the next time the character is
// drawn (or, for a severed piece, before the pieces next move).
struct SliceRequest
{
	bool pending;
	Vector3 planePoint;
	Vector3 planeNormal;
	Vector3 slideDirection;		// which way pieces above a level or slanted cut slide off
	bool reachIsLimited;		// a lightsaber only cuts what it reaches; the console command cuts right through
	Vector3 bladeBase;			// where the blade's hilt was when it struck
	float bladeReach;			// how far from the hilt the blade cuts
	bool hasStruckPoint;
	Vector3 struckPoint;		// where the blade struck the body
	bool woundsOnly;			// the blow did not kill: only arms may come off, and the character lives on
	bool divisionIsForced;		// making a cut again: divide exactly forcedDividedBodies, wherever a blade could reach
	int forcedDividedBodies;	// bit mask of ragdoll bodies
	int missingBodies;			// bit mask: bodies cut off earlier, no longer part of the character
	int aimedBody;				// the console command's "arm" cut: across the middle of this body, wherever it is drawn (-1: none)
	int cutterNumber;			// the entity whose blade (or command) makes the cut

	SliceRequest()
		: pending( false ), reachIsLimited( false ), bladeReach( 0.0f ), hasStruckPoint( false ), woundsOnly( false ),
		  divisionIsForced( false ), forcedDividedBodies( 0 ), missingBodies( 0 ), aimedBody( -1 ), cutterNumber( ENTITYNUM_NONE )
	{
	}
};

// The parts a body can be divided into. A body the cut does not divide stays whole.
enum bodyPart_t
{
	WHOLE_BODY,
	FRONT_PART,		// on the side the plane's normal points to
	BACK_PART,
	BODY_PART_COUNT
};

// How a cut divides the character's ragdoll bodies between its pieces.
struct CutLayout
{
	int bodyCount;
	bool isMissing[ragdoll::MAXIMUM_BODIES];			// cut off earlier: in no piece
	bool isSplit[ragdoll::MAXIMUM_BODIES];				// divided into a front and a back part
	bool isSplitAlong[ragdoll::MAXIMUM_BODIES];			// the cut runs along the body (it became two strips)
	int wholeSide[ragdoll::MAXIMUM_BODIES];				// a body not split: +1 or -1 if wholly on one side, 0 if
														// the cut crosses it out of the blade's reach
	RigidBody parts[ragdoll::MAXIMUM_BODIES][BODY_PART_COUNT];		// the whole body and, if split, its two parts
	Vector3 partShift[ragdoll::MAXIMUM_BODIES][BODY_PART_COUNT];	// how each part's body space moved (for joint anchors)
	int pieceOf[ragdoll::MAXIMUM_BODIES][BODY_PART_COUNT];		// the piece each part went to (-1: not used)
	int pieceCount;
};

// Where a mesh vertex sits in one of its piece's bodies, and how much that body moves it.
struct VertexInfluence
{
	int body;
	float weight;
	Vector3 localPosition;
	Vector3 localNormal;
};

// One piece of a cut character.
struct SlicedPiece
{
	bool inUse;
	int sourceEntityNumber;		// the character it came from (its collision box is ignored)
	int createdTime;
	float secondsAlive;

	// The mesh. Texture coordinates and triangles never change; positions and normals are worked out
	// again every frame from the bodies.
	std::vector<VertexInfluence> influences;
	std::vector<int> firstInfluence;		// where each vertex's influences start, plus one past the end
	std::vector<float> positions;			// three per vertex, relative to the drawn entity's origin
	std::vector<float> normals;				// three per vertex
	std::vector<float> textureCoordinates;	// two per vertex
	std::vector<int> indexes;				// grouped into batches by shader
	std::vector<dynamicMeshBatch_t> batches;
	dynamicMesh_t renderMesh;				// points into the vectors above
	std::vector<std::string> batchShaderNames;	// each batch's shader by name, for cutting the piece again
	std::string capShaderName;				// closes the faces of further cuts
	byte shaderRGBA[4];						// the character's colour tint (Jedi Academy's customizable characters)

	Simulation simulation;					// the piece's share of the ragdoll (or one rigid body)
	Transform previousBodyTransforms[ragdoll::MAXIMUM_BODIES];	// for smoothing between physics steps

	bool cutIsPending;						// a blade swept through the piece: it is cut again before the next step
	SliceRequest pendingCut;
	int madeByCutter;						// whose blade made the piece: that same swing does not cut it again
	int madeTime;
};

// A cut a living character survived, kept so it can be made again on the rest pose (after a saved
// game is loaded, and whenever the character is cut again). The plane is kept in the frame of a bone
// the cut went through, so it stays on the limb whatever pose the character is in.
struct SurvivedCut
{
	int bone;					// skeleton bone of a body the cut divided
	Vector3 pointInBone;		// a point on the cut plane, in the bone's frame (model units, unscaled)
	Vector3 normalInBone;
	int dividedBodies;			// bit mask: the ragdoll bodies the cut divided
};

// A character living on without part of its body. Ghoul2 no longer draws its model (its weapon and
// anything else attached still are); what is left of its mesh is drawn instead, bent by its current
// pose every frame (G_Slice_AddMaimedCharacter).
struct MaimedCharacter
{
	std::string meshPath;				// the model and skin it was cut from
	std::string skinPath;
	std::vector<SurvivedCut> cuts;
	int missingBodies;					// bit mask: the ragdoll bodies cut off whole
	bool meshIsBuilt;					// false until made again from the cuts (after loading a saved game)
	slicing::CharacterMesh mesh;		// what is left of the mesh, in the rest pose

	// Drawing, every frame.
	slicing::PosedMesh posed;
	std::vector<float> positions;		// three per vertex, relative to the character's origin
	std::vector<float> normals;
	std::vector<float> textureCoordinates;
	std::vector<int> indexes;			// grouped into batches by shader; filled in once
	std::vector<dynamicMeshBatch_t> batches;
	dynamicMesh_t renderMesh;

	MaimedCharacter() : missingBodies( 0 ), meshIsBuilt( false ) {}
};

static SliceRequest sliceRequests[MAX_GENTITIES];
static std::map<int, MaimedCharacter> maimedCharacters;	// by entity number
static bool characterWasCut[MAX_GENTITIES];		// replaced by its pieces: no longer drawn
static SlicedPiece pieces[MAXIMUM_PIECES];
static ragdoll::Scene pieceScene;					// all pieces are stepped together, so they collide
static int lastStepRenderTime = -1;
static float unsimulatedSeconds = 0.0f;
static std::map<std::string, qhandle_t> shaderHandles;

void G_Slice_RegisterCvars( void )
{
	g_saberSlicing = gi.cvar( "g_saberSlicing", "1", CVAR_ARCHIVE );
}

/*
==================
Helpers
==================
*/

static Quaternion QuaternionFromAxis( const vec3_t axis[3] )
{
	// axis[c] is the direction the model's c-th basis vector points, so it is column c. CG_Player
	// scales the axes of a model drawn at another size (ScaleModelAxis), so each column is made a
	// unit vector first: the scale is applied separately (ModelScaleOf).
	float rotationRows[3][3];
	for ( int column = 0; column < 3; column++ )
	{
		const float length = VectorLength( axis[column] );
		const float inverseLength = ( length > 0.0001f ) ? 1.0f / length : 1.0f;
		for ( int row = 0; row < 3; row++ )
		{
			rotationRows[row][column] = axis[column][row] * inverseLength;
		}
	}
	return ragdoll::QuaternionFromRotationMatrix( rotationRows );
}

// A character's colour tint (renderInfo.customRGBA, which CG_AddRefEntityWithPowerups draws its
// model with): Jedi Academy's customizable characters and Jedi are tinted through their shaders.
static void CharacterColour( const gentity_t *entity, byte *shaderRGBA )
{
	for ( int channel = 0; channel < 4; channel++ )
	{
		shaderRGBA[channel] = entity->client ? entity->client->renderInfo.customRGBA[channel] : 255;
	}
}

// Characters the game places itself every frame: riders on a vehicle, and characters held in a
// rancor's or a wampa's hand or in a sand creature's mouth. CG_Player draws them at the vehicle's or
// the monster's bolt. They, and vehicles themselves, are never cut.
static bool IsCarriedByGameOrVehicle( gentity_t *entity )
{
	if ( !entity->client )
	{
		return false;
	}
	const int heldFlags = EF_HELD_BY_RANCOR | EF_HELD_BY_WAMPA | EF_HELD_BY_SAND_CREATURE;
	return ( entity->client->ps.eFlags & heldFlags ) || entity->client->NPC_class == CLASS_VEHICLE || G_IsRidingVehicle( entity ) != NULL;
}

static float ModelScaleOf( const gentity_t *entity )
{
	const float averageScale = ( entity->s.modelScale[0] + entity->s.modelScale[1] + entity->s.modelScale[2] ) / 3.0f;
	return ( averageScale > 0.0f ) ? averageScale : 1.0f;
}

static float Component( const Vector3 &vector, int axis )
{
	return ( axis == 0 ) ? vector.x : ( ( axis == 1 ) ? vector.y : vector.z );
}

static float SignedDistance( const Vector3 &point, const SliceRequest &request )
{
	return ragdoll::Dot( point - request.planePoint, request.planeNormal );
}

static qhandle_t ShaderHandle( const std::string &shaderName )
{
	const std::map<std::string, qhandle_t>::const_iterator known = shaderHandles.find( shaderName );
	if ( known != shaderHandles.end() )
	{
		return known->second;
	}
	const qhandle_t handle = cgi_R_RegisterShader( shaderName.c_str() );
	shaderHandles[shaderName] = handle;
	return handle;
}

static void ScaleMatrix( ragdoll::Matrix3 &matrix, float factor )
{
	for ( int row = 0; row < 3; row++ )
	{
		for ( int column = 0; column < 3; column++ )
		{
			matrix.element[row][column] *= factor;
		}
	}
}

// How far a point is from a body's capsule (its centre line, less its radius).
static float DistanceFromBody( const RigidBody &body, const Vector3 &point )
{
	const Vector3 start = body.LocalToWorldPoint( body.spheres[0].localCentre );
	const Vector3 end = body.LocalToWorldPoint( body.spheres[1].localCentre );
	const Vector3 segment = end - start;
	const float lengthSquared = ragdoll::LengthSquared( segment );
	const float along = ( lengthSquared > ragdoll::SMALL_NUMBER )
		? ragdoll::Clamp( ragdoll::Dot( point - start, segment ) / lengthSquared, 0.0f, 1.0f ) : 0.0f;
	return ragdoll::Length( point - ( start + segment * along ) ) - body.spheres[0].radius;
}

static void ReleasePiece( SlicedPiece &piece )
{
	piece.inUse = false;
	std::vector<VertexInfluence>().swap( piece.influences );
	std::vector<int>().swap( piece.firstInfluence );
	std::vector<float>().swap( piece.positions );
	std::vector<float>().swap( piece.normals );
	std::vector<float>().swap( piece.textureCoordinates );
	std::vector<int>().swap( piece.indexes );
	std::vector<dynamicMeshBatch_t>().swap( piece.batches );
	std::vector<std::string>().swap( piece.batchShaderNames );
	piece.cutIsPending = false;
}

// For severed pieces the "bones" moving a vertex are the piece's own bodies: each carries itself.
static const int *PieceBodyCarriers( void )
{
	static int carriers[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool filledIn = false;
	if ( !filledIn )
	{
		for ( int bone = 0; bone < ragdoll::MAXIMUM_SKELETON_BONES; bone++ )
		{
			carriers[bone] = ( bone < ragdoll::MAXIMUM_BODIES ) ? bone : -1;
		}
		filledIn = true;
	}
	return carriers;
}

// The ragdoll body whose bones move a vertex most, leaving out the bodies in ignoredBodies (a bit
// mask). -1 if no bone of the vertex is carried by a body.
static int StrongestBody( int boneCount, const int *bones, const float *weights, const int *carrierBody, int bodyCount, int ignoredBodies )
{
	float bodyWeight[ragdoll::MAXIMUM_BODIES];
	for ( int body = 0; body < ragdoll::MAXIMUM_BODIES; body++ )
	{
		bodyWeight[body] = 0.0f;
	}
	for ( int weightIndex = 0; weightIndex < boneCount && weightIndex < 4; weightIndex++ )
	{
		const int bone = bones[weightIndex];
		if ( bone < 0 || bone >= ragdoll::MAXIMUM_SKELETON_BONES )
		{
			continue;
		}
		const int body = carrierBody[bone];
		if ( body >= 0 && body < bodyCount && !( ignoredBodies & ( 1 << body ) ) )
		{
			bodyWeight[body] += weights[weightIndex];
		}
	}
	int strongest = -1;
	for ( int body = 0; body < bodyCount && body < ragdoll::MAXIMUM_BODIES; body++ )
	{
		if ( bodyWeight[body] > 0.0f && ( strongest < 0 || bodyWeight[body] > bodyWeight[strongest] ) )
		{
			strongest = body;
		}
	}
	return strongest;
}

// A free piece slot, or the oldest piece's if all are taken.
static SlicedPiece &FreePiece( void )
{
	SlicedPiece *oldest = &pieces[0];
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		if ( !pieces[pieceIndex].inUse )
		{
			return pieces[pieceIndex];
		}
		if ( pieces[pieceIndex].createdTime < oldest->createdTime )
		{
			oldest = &pieces[pieceIndex];
		}
	}
	ReleasePiece( *oldest );
	return *oldest;
}

// Groups items into sets of items joined to each other (a "union-find" structure).
class JoinedSets
{
public:
	explicit JoinedSets( int itemCount ) : leader( itemCount )
	{
		for ( int item = 0; item < itemCount; item++ )
		{
			leader[item] = item;
		}
	}

	int SetOf( int item )
	{
		while ( leader[item] != item )
		{
			leader[item] = leader[leader[item]];
			item = leader[item];
		}
		return item;
	}

	void Join( int first, int second )
	{
		leader[SetOf( first )] = SetOf( second );
	}

private:
	std::vector<int> leader;
};

static int PartNode( int body, int part )
{
	return body * BODY_PART_COUNT + part;
}

static bool IsArmBody( int body )
{
	return body >= ragdoll::BODY_RIGHT_UPPER_ARM && body <= ragdoll::BODY_LEFT_HAND;
}

/*
==================
Dividing the ragdoll bodies
==================
*/

// Keeps the part of a body that is on one side of the cut (side +1: the side the plane normal points
// to, -1: the other), as a capsule of its own. A cut across the body keeps the length on this side;
// a cut along it keeps the strip of its thickness on this side. Returns false if (almost) none of
// the body is on this side. localShift receives how the body's own coordinates moved (its centre
// moves), so points given in its space, such as joint anchors, can be moved with it.
static bool ClipBodyToSide( RigidBody &body, const Vector3 &planePoint, const Vector3 &planeNormal, float side,
							Vector3 &localShift, bool &wasCut, bool &cutAlong )
{
	const Vector3 start = body.LocalToWorldPoint( body.spheres[0].localCentre );
	const Vector3 end = body.LocalToWorldPoint( body.spheres[1].localCentre );
	const float radius = body.spheres[0].radius;
	const float startDistance = side * ragdoll::Dot( start - planePoint, planeNormal );	// positive: on this side
	const float endDistance = side * ragdoll::Dot( end - planePoint, planeNormal );
	localShift = Vector3( 0.0f, 0.0f, 0.0f );
	wasCut = false;
	cutAlong = false;

	if ( startDistance >= radius && endDistance >= radius )
	{
		return true;		// wholly on this side
	}
	if ( startDistance <= -radius && endDistance <= -radius )
	{
		return false;		// wholly on the other side
	}

	Vector3 newStart = start;
	Vector3 newEnd = end;
	float newRadius = radius;
	float share;
	const float margin = ACROSS_CUT_MARGIN * radius;
	if ( ( startDistance > margin && endDistance < -margin ) || ( startDistance < -margin && endDistance > margin ) )
	{
		// The cut goes across the body: this side keeps the length up to the cut.
		const float fraction = startDistance / ( startDistance - endDistance );
		const Vector3 cutPoint = start + ( end - start ) * fraction;
		const bool startIsKept = startDistance > 0.0f;
		const Vector3 keptEnd = startIsKept ? start : end;
		const float keptEndDistance = startIsKept ? startDistance : endDistance;
		const float originalLength = ragdoll::Length( end - start );
		share = ( originalLength > 0.01f ) ? ragdoll::Length( cutPoint - keptEnd ) / originalLength : 0.5f;

		// The capsule's rounded end must not poke through the cut face: its centre is pulled back
		// until it is a radius from the plane (a stub too short for that gets a smaller radius).
		const float pullBack = ragdoll::Clamp( radius / keptEndDistance, 0.0f, 1.0f );
		const Vector3 cutEnd = cutPoint + ( keptEnd - cutPoint ) * pullBack;
		if ( keptEndDistance < radius )
		{
			newRadius = keptEndDistance;
		}
		newStart = startIsKept ? keptEnd : cutEnd;
		newEnd = startIsKept ? cutEnd : keptEnd;
	}
	else
	{
		// The cut runs along the body: measured across it (toward this side) the body spans from
		// centre - radius to centre + radius, and this side keeps the part beyond the cut.
		const float centreDistance = ( startDistance + endDistance ) * 0.5f;
		const float farEdge = centreDistance + radius;
		const float nearEdge = ( centreDistance - radius > 0.0f ) ? centreDistance - radius : 0.0f;
		const float width = farEdge - nearEdge;
		if ( width <= SLIVER_SHARE * 2.0f * radius )
		{
			return false;	// only a sliver on this side
		}
		newRadius = width * 0.5f;
		const Vector3 offset = planeNormal * ( side * ( ( nearEdge + farEdge ) * 0.5f - centreDistance ) );
		newStart = start + offset;
		newEnd = end + offset;
		share = width / ( 2.0f * radius );
		cutAlong = true;
	}
	share = ragdoll::Clamp( share, SMALLEST_BODY_SHARE, 1.0f );
	wasCut = true;

	const Vector3 newCentre = ( newStart + newEnd ) * 0.5f;
	localShift = ragdoll::InverseRotate( body.orientation, body.position - newCentre );
	body.position = newCentre;
	body.substepStartPosition = newCentre;
	body.spheres[0].localCentre = ragdoll::InverseRotate( body.orientation, newStart - newCentre );
	body.spheres[0].radius = newRadius;
	body.spheres[1].localCentre = ragdoll::InverseRotate( body.orientation, newEnd - newCentre );
	body.spheres[1].radius = newRadius;
	body.inverseMass /= share;
	ScaleMatrix( body.inverseInertiaLocal, 1.0f / share );
	return true;
}

// True if the blade reaches the place where the cut crosses a body.
static bool BladeReachesBody( const RigidBody &body, const SliceRequest &request )
{
	if ( !request.reachIsLimited )
	{
		return true;
	}
	if ( request.hasStruckPoint && DistanceFromBody( body, request.struckPoint ) <= STRUCK_BODY_DISTANCE )
	{
		return true;	// the blade struck this one
	}
	const Vector3 start = body.LocalToWorldPoint( body.spheres[0].localCentre );
	const Vector3 end = body.LocalToWorldPoint( body.spheres[1].localCentre );
	const float startDistance = SignedDistance( start, request );
	const float endDistance = SignedDistance( end, request );
	Vector3 crossing;
	if ( ( startDistance > 0.0f ) != ( endDistance > 0.0f ) )
	{
		crossing = start + ( end - start ) * ( startDistance / ( startDistance - endDistance ) );
	}
	else
	{
		const Vector3 middle = ( start + end ) * 0.5f;
		crossing = middle - request.planeNormal * SignedDistance( middle, request );
	}
	return ragdoll::Length( crossing - request.bladeBase ) <= request.bladeReach + body.spheres[0].radius;
}

// A joint holds one part of each of its two bodies together: two parts of each when the cut runs
// along both bodies or right through the joint, or when a whole body holds both parts of a split
// one. Fills in the parent's and child's parts and returns how many pairs are held (1 or 2).
static int JointConnections( const CutLayout &layout, const Simulation &whole, const Joint &joint, const SliceRequest &request,
							 int *parentParts, int *childParts )
{
	const int parent = joint.parentBodyIndex;
	const int child = joint.childBodyIndex;
	if ( layout.isMissing[parent] || layout.isMissing[child] )
	{
		return 0;
	}
	const float anchorDistance = SignedDistance( whole.bodies[child].LocalToWorldPoint( joint.childAnchor ), request );
	const int anchorPart = ( anchorDistance >= 0.0f ) ? FRONT_PART : BACK_PART;
	const bool anchorOnCut = fabsf( anchorDistance ) <= SEVERED_JOINT_DISTANCE;

	if ( !layout.isSplit[parent] && !layout.isSplit[child] )
	{
		parentParts[0] = WHOLE_BODY;
		childParts[0] = WHOLE_BODY;
		return 1;
	}
	if ( layout.isSplit[parent] && layout.isSplit[child] )
	{
		// Between two strips (a lengthwise cut down the spine, say) each side keeps its half of the
		// joint, as it does when the cut goes right through the joint. Otherwise the joint stays with
		// the parts on its side.
		if ( ( layout.isSplitAlong[parent] && layout.isSplitAlong[child] ) || anchorOnCut )
		{
			parentParts[0] = FRONT_PART;
			childParts[0] = FRONT_PART;
			parentParts[1] = BACK_PART;
			childParts[1] = BACK_PART;
			return 2;
		}
		parentParts[0] = anchorPart;
		childParts[0] = anchorPart;
		return 1;
	}

	// One body split, the other whole. A whole body on one side of the cut holds on to the part on
	// its side. One the cut crosses (out of the blade's reach) holds the part on the joint's side,
	// or both parts if the joint is right on the cut.
	const bool parentIsSplit = layout.isSplit[parent];
	const int wholeBody = parentIsSplit ? child : parent;
	int *splitParts = parentIsSplit ? parentParts : childParts;
	int *wholeParts = parentIsSplit ? childParts : parentParts;
	wholeParts[0] = WHOLE_BODY;
	wholeParts[1] = WHOLE_BODY;
	if ( layout.wholeSide[wholeBody] != 0 )
	{
		splitParts[0] = ( layout.wholeSide[wholeBody] > 0 ) ? FRONT_PART : BACK_PART;
		return 1;
	}
	if ( !anchorOnCut )
	{
		splitParts[0] = anchorPart;
		return 1;
	}
	splitParts[0] = FRONT_PART;
	splitParts[1] = BACK_PART;
	return 2;
}

// Works out how the cut divides the character's bodies and which piece each body (or part of a
// body) ends up in. Returns the number of pieces: 1 if the cut does not separate anything.
static int DivideBodies( const Simulation &whole, const SliceRequest &request, CutLayout &layout )
{
	layout.bodyCount = whole.bodyCount;
	for ( int body = 0; body < whole.bodyCount; body++ )
	{
		const RigidBody &original = whole.bodies[body];
		layout.isSplit[body] = false;
		layout.isSplitAlong[body] = false;
		for ( int part = 0; part < BODY_PART_COUNT; part++ )
		{
			layout.parts[body][part] = original;
			layout.partShift[body][part] = Vector3( 0.0f, 0.0f, 0.0f );
			layout.pieceOf[body][part] = -1;
		}
		layout.isMissing[body] = ( request.missingBodies & ( 1 << body ) ) != 0;
		if ( layout.isMissing[body] )
		{
			layout.wholeSide[body] = 0;
			continue;
		}
		if ( original.sphereCount < 2 )
		{
			layout.wholeSide[body] = ( SignedDistance( original.position, request ) >= 0.0f ) ? 1 : -1;
			continue;
		}

		bool frontWasCut, backWasCut, frontCutAlong, backCutAlong;
		const bool hasFront = ClipBodyToSide( layout.parts[body][FRONT_PART], request.planePoint, request.planeNormal, 1.0f,
											  layout.partShift[body][FRONT_PART], frontWasCut, frontCutAlong );
		const bool hasBack = ClipBodyToSide( layout.parts[body][BACK_PART], request.planePoint, request.planeNormal, -1.0f,
											 layout.partShift[body][BACK_PART], backWasCut, backCutAlong );
		if ( hasFront && hasBack )
		{
			layout.wholeSide[body] = 0;
			bool isCut = request.divisionIsForced ? ( request.forcedDividedBodies & ( 1 << body ) ) != 0
												   : BladeReachesBody( original, request );
			if ( request.woundsOnly && !IsArmBody( body ) )
			{
				isCut = false;		// a blow that does not kill only cuts through arms
			}
			if ( isCut )
			{
				layout.isSplit[body] = true;
				layout.isSplitAlong[body] = frontCutAlong || backCutAlong;
			}
		}
		else
		{
			layout.wholeSide[body] = hasFront ? 1 : -1;
		}
	}

	// Group the bodies and parts through the joints that hold them together.
	JoinedSets sets( whole.bodyCount * BODY_PART_COUNT );
	for ( int jointIndex = 0; jointIndex < whole.jointCount; jointIndex++ )
	{
		const Joint &joint = whole.joints[jointIndex];
		int parentParts[2];
		int childParts[2];
		const int connectionCount = JointConnections( layout, whole, joint, request, parentParts, childParts );
		for ( int connection = 0; connection < connectionCount; connection++ )
		{
			sets.Join( PartNode( joint.parentBodyIndex, parentParts[connection] ), PartNode( joint.childBodyIndex, childParts[connection] ) );
		}
	}

	// A body whose two parts are still held together (around the end of the cut) is not split after all.
	for ( int body = 0; body < whole.bodyCount; body++ )
	{
		if ( layout.isSplit[body] && sets.SetOf( PartNode( body, FRONT_PART ) ) == sets.SetOf( PartNode( body, BACK_PART ) ) )
		{
			layout.isSplit[body] = false;
			layout.isSplitAlong[body] = false;
			sets.Join( PartNode( body, WHOLE_BODY ), PartNode( body, FRONT_PART ) );
		}
	}

	// Number the pieces. The first is the one holding the first body (the pelvis).
	std::vector<int> pieceOfSet( whole.bodyCount * BODY_PART_COUNT, -1 );
	layout.pieceCount = 0;
	for ( int body = 0; body < whole.bodyCount; body++ )
	{
		if ( layout.isMissing[body] )
		{
			continue;
		}
		for ( int part = 0; part < BODY_PART_COUNT; part++ )
		{
			const bool partIsUsed = layout.isSplit[body] ? ( part != WHOLE_BODY ) : ( part == WHOLE_BODY );
			if ( !partIsUsed )
			{
				continue;
			}
			const int set = sets.SetOf( PartNode( body, part ) );
			if ( pieceOfSet[set] < 0 )
			{
				pieceOfSet[set] = layout.pieceCount++;
			}
			layout.pieceOf[body][part] = pieceOfSet[set];
		}
	}
	return layout.pieceCount;
}

// The piece a mesh vertex goes to: that of the body moving it most, or for a split body, that of the
// part on the vertex's side of the cut.
static int PieceOfVertex( const slicing::PosedVertex &vertex, const int *carrierBody, const Simulation &whole,
						  const CutLayout &layout, const SliceRequest &request )
{
	int strongest = StrongestBody( vertex.boneCount, vertex.bones, vertex.weights, carrierBody, whole.bodyCount, request.missingBodies );
	if ( strongest < 0 )
	{
		float nearestDistance = 1.0e9f;
		for ( int body = 0; body < whole.bodyCount; body++ )
		{
			if ( layout.isMissing[body] )
			{
				continue;
			}
			const float distance = DistanceFromBody( whole.bodies[body], vertex.position );
			if ( distance < nearestDistance )
			{
				nearestDistance = distance;
				strongest = body;
			}
		}
	}
	if ( strongest < 0 )
	{
		return 0;
	}
	const int part = layout.isSplit[strongest] ? ( ( SignedDistance( vertex.position, request ) >= 0.0f ) ? FRONT_PART : BACK_PART ) : WHOLE_BODY;
	const int piece = layout.pieceOf[strongest][part];
	return ( piece >= 0 ) ? piece : 0;
}

// Builds one piece's limp ragdoll from the bodies and parts of bodies the cut gave it. pieceBodyOf
// receives, for each of the character's bodies, its index in the piece (-1 if it is not in this
// piece). Returns false if the piece has no bodies.
static bool BuildPieceBodies( const Simulation &whole, const CutLayout &layout, int pieceIndex, const SliceRequest &request,
							  Simulation &piece, int *pieceBodyOf )
{
	for ( int body = 0; body < ragdoll::MAXIMUM_BODIES; body++ )
	{
		pieceBodyOf[body] = -1;
	}
	piece.bodyCount = 0;
	piece.jointCount = 0;

	for ( int body = 0; body < whole.bodyCount && piece.bodyCount < ragdoll::MAXIMUM_BODIES; body++ )
	{
		for ( int part = 0; part < BODY_PART_COUNT; part++ )
		{
			if ( layout.pieceOf[body][part] != pieceIndex )
			{
				continue;
			}
			RigidBody copied = layout.parts[body][part];		// keeps its velocity: a piece cut again carries on moving
			copied.worldDrive.naturalFrequency = 0.0f;
			copied.positionDrive.enabled = false;
			copied.positionDrive.horizontalShare = 1.0f;
			copied.gravityScale = 1.0f;
			pieceBodyOf[body] = piece.bodyCount;
			piece.bodies[piece.bodyCount++] = copied;
		}
	}
	if ( piece.bodyCount == 0 )
	{
		return false;
	}

	// The joints holding this piece's parts together. They are limp: the character is dead.
	for ( int jointIndex = 0; jointIndex < whole.jointCount; jointIndex++ )
	{
		const Joint &joint = whole.joints[jointIndex];
		const int parent = joint.parentBodyIndex;
		const int child = joint.childBodyIndex;
		int parentParts[2];
		int childParts[2];
		const int connectionCount = JointConnections( layout, whole, joint, request, parentParts, childParts );
		for ( int connection = 0; connection < connectionCount && piece.jointCount < ragdoll::MAXIMUM_JOINTS; connection++ )
		{
			const int parentPart = parentParts[connection];
			const int childPart = childParts[connection];
			if ( layout.pieceOf[parent][parentPart] != pieceIndex || layout.pieceOf[child][childPart] != pieceIndex )
			{
				continue;
			}
			Joint copied = joint;
			copied.parentBodyIndex = pieceBodyOf[parent];
			copied.childBodyIndex = pieceBodyOf[child];
			copied.parentAnchor = joint.parentAnchor + layout.partShift[parent][parentPart];
			copied.childAnchor = joint.childAnchor + layout.partShift[child][childPart];
			copied.muscle.naturalFrequency = 0.0f;
			piece.joints[piece.jointCount++] = copied;
		}
	}

	for ( int first = 0; first < whole.bodyCount; first++ )
	{
		for ( int second = 0; second < whole.bodyCount; second++ )
		{
			if ( pieceBodyOf[first] >= 0 && pieceBodyOf[second] >= 0 )
			{
				piece.bodyPairCollides[pieceBodyOf[first]][pieceBodyOf[second]] = whole.bodyPairCollides[first][second];
				piece.selfOverlapSeconds[pieceBodyOf[first]][pieceBodyOf[second]] = 0.0f;
			}
		}
	}
	return true;
}

// A piece with no ragdoll bodies falls as one rigid body: a capsule along the longest side of its
// bounds, with the mass and spin resistance of a box that size.
static void BuildRigidPieceBody( Simulation &simulation, const Vector3 &centre, const Vector3 &size )
{
	simulation.bodyCount = 1;
	simulation.jointCount = 0;
	simulation.bodyPairCollides[0][0] = false;

	const float mass = ragdoll::Clamp( size.x * size.y * size.z * PIECE_DENSITY, LIGHTEST_PIECE_MASS, HEAVIEST_PIECE_MASS );
	RigidBody &body = simulation.bodies[0];
	body.position = centre;
	body.orientation = Quaternion();
	body.linearVelocity = Vector3( 0.0f, 0.0f, 0.0f );
	body.angularVelocity = Vector3( 0.0f, 0.0f, 0.0f );
	body.inverseMass = 1.0f / mass;
	body.inverseInertiaLocal = ragdoll::Matrix3();
	body.inverseInertiaLocal.element[0][0] = 12.0f / ( mass * ( size.y * size.y + size.z * size.z ) + 1.0e-3f );
	body.inverseInertiaLocal.element[1][1] = 12.0f / ( mass * ( size.x * size.x + size.z * size.z ) + 1.0e-3f );
	body.inverseInertiaLocal.element[2][2] = 12.0f / ( mass * ( size.x * size.x + size.y * size.y ) + 1.0e-3f );
	body.gravityScale = 1.0f;

	int longestAxis = 0;
	for ( int axis = 1; axis < 3; axis++ )
	{
		if ( Component( size, axis ) > Component( size, longestAxis ) )
		{
			longestAxis = axis;
		}
	}
	const float sideA = Component( size, ( longestAxis + 1 ) % 3 );
	const float sideB = Component( size, ( longestAxis + 2 ) % 3 );
	const float radius = ragdoll::Clamp( ( sideA < sideB ? sideA : sideB ) * 0.5f, THINNEST_PIECE_RADIUS, THICKEST_PIECE_RADIUS );
	float reach = Component( size, longestAxis ) * 0.5f - radius;
	if ( reach < 0.0f )
	{
		reach = 0.0f;
	}
	const Vector3 axisDirection( longestAxis == 0 ? 1.0f : 0.0f, longestAxis == 1 ? 1.0f : 0.0f, longestAxis == 2 ? 1.0f : 0.0f );
	body.sphereCount = 2;
	body.spheres[0].localCentre = axisDirection * reach;
	body.spheres[0].radius = radius;
	body.spheres[1].localCentre = axisDirection * -reach;
	body.spheres[1].radius = radius;

	body.worldDrive.targetOrientation = body.orientation;
	body.worldDrive.naturalFrequency = 0.0f;
	body.worldDrive.dampingRatio = 1.0f;
	body.positionDrive.enabled = false;
	body.positionDrive.stiffness = 0.0f;
	body.positionDrive.damping = 0.0f;
	body.positionDrive.horizontalShare = 1.0f;
	body.substepStartPosition = body.position;
	body.substepStartOrientation = body.orientation;
}

/*
==================
Pieces
==================
*/

// Attaches a vertex to the piece's bodies that its bones belong to (up to four, weighted as the
// bones were). A vertex whose bones all ended up in another piece goes with the nearest body.
static void AttachVertex( SlicedPiece &piece, const slicing::PosedVertex &vertex, const int *carrierBody, const int *pieceBodyOf )
{
	const Simulation &simulation = piece.simulation;
	float bodyWeight[ragdoll::MAXIMUM_BODIES];
	for ( int body = 0; body < ragdoll::MAXIMUM_BODIES; body++ )
	{
		bodyWeight[body] = 0.0f;
	}
	if ( carrierBody && pieceBodyOf )
	{
		for ( int weightIndex = 0; weightIndex < vertex.boneCount; weightIndex++ )
		{
			const int bone = vertex.bones[weightIndex];
			if ( bone < 0 || bone >= ragdoll::MAXIMUM_SKELETON_BONES )
			{
				continue;
			}
			const int characterBody = carrierBody[bone];
			if ( characterBody < 0 || characterBody >= ragdoll::MAXIMUM_BODIES || pieceBodyOf[characterBody] < 0 )
			{
				continue;
			}
			bodyWeight[pieceBodyOf[characterBody]] += vertex.weights[weightIndex];
		}
	}

	// The four bodies that move it most.
	int chosenBodies[4];
	float chosenWeights[4];
	int chosenCount = 0;
	float totalWeight = 0.0f;
	for ( int pick = 0; pick < 4; pick++ )
	{
		int best = -1;
		for ( int body = 0; body < simulation.bodyCount; body++ )
		{
			if ( bodyWeight[body] > 0.001f && ( best < 0 || bodyWeight[body] > bodyWeight[best] ) )
			{
				best = body;
			}
		}
		if ( best < 0 )
		{
			break;
		}
		chosenBodies[chosenCount] = best;
		chosenWeights[chosenCount] = bodyWeight[best];
		totalWeight += bodyWeight[best];
		bodyWeight[best] = 0.0f;
		chosenCount++;
	}
	if ( chosenCount == 0 )
	{
		int nearest = 0;
		float nearestDistance = 1.0e9f;
		for ( int body = 0; body < simulation.bodyCount; body++ )
		{
			const float distance = DistanceFromBody( simulation.bodies[body], vertex.position );
			if ( distance < nearestDistance )
			{
				nearestDistance = distance;
				nearest = body;
			}
		}
		chosenBodies[0] = nearest;
		chosenWeights[0] = 1.0f;
		totalWeight = 1.0f;
		chosenCount = 1;
	}

	for ( int chosen = 0; chosen < chosenCount; chosen++ )
	{
		const RigidBody &body = simulation.bodies[chosenBodies[chosen]];
		VertexInfluence influence;
		influence.body = chosenBodies[chosen];
		influence.weight = chosenWeights[chosen] / totalWeight;
		influence.localPosition = ragdoll::InverseRotate( body.orientation, vertex.position - body.position );
		influence.localNormal = ragdoll::InverseRotate( body.orientation, vertex.normal );
		piece.influences.push_back( influence );
	}
}

// Turns one part of a divided mesh into a piece: a limp ragdoll made of the bodies the cut gave it
// (or one rigid body if the character has no ragdoll; fallbackSide then says which side of the cut
// it is on), with its mesh attached to them, starting to move away from the cut. carrierBody gives
// the body carrying each bone the mesh's vertices name; shaderRGBA is the colour it is drawn in.
static void AddPiece( const slicing::PosedMesh &mesh, const Simulation *wholeRagdoll, const int *carrierBody, const CutLayout *layout,
					  int pieceIndex, float fallbackSide, const SliceRequest &request, int sourceEntityNumber,
					  const std::string &capShaderName, const byte *shaderRGBA )
{
	if ( mesh.indexes.empty() || mesh.vertices.empty() )
	{
		return;
	}
	SlicedPiece &piece = FreePiece();
	ReleasePiece( piece );

	int pieceBodyOf[ragdoll::MAXIMUM_BODIES];
	const bool articulated = wholeRagdoll && layout && BuildPieceBodies( *wholeRagdoll, *layout, pieceIndex, request, piece.simulation, pieceBodyOf );
	float side = fallbackSide;
	if ( articulated )
	{
		// Which side of the cut the piece is on, judged by where its weight is.
		float weightedDistance = 0.0f;
		for ( int body = 0; body < piece.simulation.bodyCount; body++ )
		{
			const RigidBody &rigidBody = piece.simulation.bodies[body];
			weightedDistance += SignedDistance( rigidBody.position, request ) / rigidBody.inverseMass;
		}
		side = ( weightedDistance >= 0.0f ) ? 1.0f : -1.0f;
	}
	else
	{
		Vector3 minimum = mesh.vertices[0].position;
		Vector3 maximum = mesh.vertices[0].position;
		for ( size_t vertex = 1; vertex < mesh.vertices.size(); vertex++ )
		{
			const Vector3 &position = mesh.vertices[vertex].position;
			minimum = Vector3( position.x < minimum.x ? position.x : minimum.x, position.y < minimum.y ? position.y : minimum.y,
							   position.z < minimum.z ? position.z : minimum.z );
			maximum = Vector3( position.x > maximum.x ? position.x : maximum.x, position.y > maximum.y ? position.y : maximum.y,
							   position.z > maximum.z ? position.z : maximum.z );
		}
		BuildRigidPieceBody( piece.simulation, ( minimum + maximum ) * 0.5f, maximum - minimum );
		piece.simulation.bodies[0].angularVelocity = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, request.planeNormal * side ) ) * PIECE_TUMBLE_SPEED;
	}

	// Away from the cut; a piece above a level or slanted cut also slides off it.
	const Vector3 awayFromCut = request.planeNormal * side;
	Vector3 startVelocity = awayFromCut * PIECE_SEPARATION_SPEED;
	if ( ragdoll::Dot( awayFromCut, WORLD_UP ) > 0.1f )
	{
		startVelocity += request.slideDirection * PIECE_SLIDE_SPEED;
	}
	for ( int body = 0; body < piece.simulation.bodyCount; body++ )
	{
		RigidBody &rigidBody = piece.simulation.bodies[body];
		rigidBody.linearVelocity += startVelocity;
		rigidBody.substepStartPosition = rigidBody.position;
		rigidBody.substepStartOrientation = rigidBody.orientation;
		piece.previousBodyTransforms[body] = Transform( rigidBody.orientation, rigidBody.position );
	}

	// The mesh: every vertex attached to its bodies, the triangles grouped by shader.
	const size_t vertexCount = mesh.vertices.size();
	piece.influences.reserve( vertexCount * 2 );
	piece.firstInfluence.reserve( vertexCount + 1 );
	piece.textureCoordinates.reserve( vertexCount * 2 );
	for ( size_t vertex = 0; vertex < vertexCount; vertex++ )
	{
		piece.firstInfluence.push_back( (int)piece.influences.size() );
		AttachVertex( piece, mesh.vertices[vertex], articulated ? carrierBody : NULL, articulated ? pieceBodyOf : NULL );
		piece.textureCoordinates.push_back( mesh.vertices[vertex].s );
		piece.textureCoordinates.push_back( mesh.vertices[vertex].t );
	}
	piece.firstInfluence.push_back( (int)piece.influences.size() );
	piece.positions.assign( vertexCount * 3, 0.0f );
	piece.normals.assign( vertexCount * 3, 0.0f );

	for ( size_t shaderSlot = 0; shaderSlot < mesh.shaderNames.size(); shaderSlot++ )
	{
		dynamicMeshBatch_t batch;
		batch.shader = ShaderHandle( mesh.shaderNames[shaderSlot] );
		batch.firstIndex = (int)piece.indexes.size();
		for ( size_t triangle = 0; triangle < mesh.triangleShaders.size(); triangle++ )
		{
			if ( mesh.triangleShaders[triangle] == (int)shaderSlot )
			{
				piece.indexes.push_back( mesh.indexes[triangle * 3] );
				piece.indexes.push_back( mesh.indexes[triangle * 3 + 1] );
				piece.indexes.push_back( mesh.indexes[triangle * 3 + 2] );
			}
		}
		batch.numIndexes = (int)piece.indexes.size() - batch.firstIndex;
		if ( batch.numIndexes > 0 )
		{
			piece.batches.push_back( batch );
			piece.batchShaderNames.push_back( mesh.shaderNames[shaderSlot] );
		}
	}

	piece.renderMesh.numVertexes = (int)vertexCount;
	piece.renderMesh.xyz = (const float (*)[3])&piece.positions[0];
	piece.renderMesh.normal = (const float (*)[3])&piece.normals[0];
	piece.renderMesh.st = (const float (*)[2])&piece.textureCoordinates[0];
	piece.renderMesh.numIndexes = (int)piece.indexes.size();
	piece.renderMesh.indexes = &piece.indexes[0];
	piece.renderMesh.numBatches = (int)piece.batches.size();
	piece.renderMesh.batches = piece.batches.empty() ? NULL : &piece.batches[0];

	piece.sourceEntityNumber = sourceEntityNumber;
	piece.capShaderName = capShaderName;
	for ( int channel = 0; channel < 4; channel++ )
	{
		piece.shaderRGBA[channel] = shaderRGBA[channel];
	}
	piece.cutIsPending = false;
	piece.madeByCutter = request.cutterNumber;
	piece.madeTime = level.time;
	piece.createdTime = level.time;
	piece.secondsAlive = 0.0f;
	piece.inUse = true;

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: piece with %d bodies and %d joints, %d triangles%s\n", piece.simulation.bodyCount, piece.simulation.jointCount,
				   (int)piece.indexes.size() / 3, articulated ? "" : " (rigid)" );
	}
}

// Moves every vertex of a piece to where its bodies have it (smoothed between physics steps). The
// positions are written relative to meshOrigin (the first body's centre), and meshRadius receives
// how far the furthest vertex is from it.
static void BendPieceMesh( SlicedPiece &piece, float fraction, Vector3 &meshOrigin, float &meshRadius )
{
	Transform bodyTransforms[ragdoll::MAXIMUM_BODIES];
	for ( int body = 0; body < piece.simulation.bodyCount; body++ )
	{
		const RigidBody &rigidBody = piece.simulation.bodies[body];
		const Transform &previous = piece.previousBodyTransforms[body];
		bodyTransforms[body] = Transform( ragdoll::Slerp( previous.rotation, rigidBody.orientation, fraction ),
										  previous.position + ( rigidBody.position - previous.position ) * fraction );
	}
	meshOrigin = bodyTransforms[0].position;
	float largestDistanceSquared = 0.0f;

	const int vertexCount = (int)piece.firstInfluence.size() - 1;
	for ( int vertex = 0; vertex < vertexCount; vertex++ )
	{
		Vector3 position( 0.0f, 0.0f, 0.0f );
		Vector3 normal( 0.0f, 0.0f, 0.0f );
		for ( int influenceIndex = piece.firstInfluence[vertex]; influenceIndex < piece.firstInfluence[vertex + 1]; influenceIndex++ )
		{
			const VertexInfluence &influence = piece.influences[influenceIndex];
			const Transform &bodyTransform = bodyTransforms[influence.body];
			position += bodyTransform.TransformPoint( influence.localPosition ) * influence.weight;
			normal += ragdoll::Rotate( bodyTransform.rotation, influence.localNormal ) * influence.weight;
		}
		normal = ragdoll::Normalized( normal );
		const Vector3 fromOrigin = position - meshOrigin;
		const float distanceSquared = ragdoll::LengthSquared( fromOrigin );
		if ( distanceSquared > largestDistanceSquared )
		{
			largestDistanceSquared = distanceSquared;
		}
		piece.positions[vertex * 3] = fromOrigin.x;
		piece.positions[vertex * 3 + 1] = fromOrigin.y;
		piece.positions[vertex * 3 + 2] = fromOrigin.z;
		piece.normals[vertex * 3] = normal.x;
		piece.normals[vertex * 3 + 1] = normal.y;
		piece.normals[vertex * 3 + 2] = normal.z;
	}
	meshRadius = sqrtf( largestDistanceSquared );
}

static void SweepPieceThroughWorld( void *userContext, const Vector3 &start, const Vector3 &end, float radius, WorldSweepResult &result )
{
	const SlicedPiece &piece = *(const SlicedPiece *)userContext;
	const float halfSize = radius * 0.9f;
	vec3_t boxMinimum = { -halfSize, -halfSize, -halfSize };
	vec3_t boxMaximum = { halfSize, halfSize, halfSize };
	vec3_t startPoint, endPoint;
	start.CopyToArray( startPoint );
	end.CopyToArray( endPoint );

	trace_t trace;
	gi.trace( &trace, startPoint, boxMinimum, boxMaximum, endPoint, piece.sourceEntityNumber, MASK_SOLID, G2_NOCOLLIDE, 0 );

	result.startedInSolid = trace.startsolid || trace.allsolid;
	result.hit = trace.fraction < 1.0f;
	result.contactCentre = Vector3::FromArray( trace.endpos );
	result.normal = Vector3::FromArray( trace.plane.normal );
}

/*
==================
Lightsaber swings
==================
*/

// The swing of a lightsaber blade since the last frame: the blade then and the blade now.
struct BladeSwing
{
	Vector3 baseBefore;
	Vector3 baseNow;
	Vector3 directionBefore;
	Vector3 directionNow;
	float length;
	Vector3 sweptNormal;	// the normal of the patch the blade swept through, as long as twice its area
};

// One lit blade of a character's sabers: Jedi Academy gives every character up to two sabers
// (ps.saber[0] in the right hand, ps.saber[1] in the left when ps.dualSabers is set), each with
// one or more blades (a staff has two). Returns NULL if there is no such blade, or it is not lit (or
// was only just switched on, so it has no swing yet).
static bladeInfo_t *LitBlade( gentity_t *saberUser, int saberNum, int bladeNum )
{
	if ( !saberUser || !saberUser->client || saberNum < 0 || saberNum >= MAX_SABERS )
	{
		return NULL;
	}
	playerState_t &state = saberUser->client->ps;
	if ( ( saberNum == 1 && !state.dualSabers ) || bladeNum < 0 || bladeNum >= state.saber[saberNum].numBlades || bladeNum >= MAX_BLADES )
	{
		return NULL;
	}
	bladeInfo_t &blade = state.saber[saberNum].blade[bladeNum];
	if ( blade.length <= 0.0f || blade.lengthOld <= 0.0f || VectorCompare( blade.muzzlePointOld, vec3_origin ) || VectorCompare( blade.muzzleDirOld, vec3_origin ) )
	{
		return NULL;
	}
	return &blade;
}

// Reads one blade's swing. Returns false if the character has no such lit blade, or if the blade is
// not moving sideways enough to cut: a thrust stabs, it does not cut.
static bool ReadBladeSwing( gentity_t *saberUser, int saberNum, int bladeNum, BladeSwing &swing )
{
	const bladeInfo_t *blade = LitBlade( saberUser, saberNum, bladeNum );
	if ( !blade )
	{
		return false;
	}
	swing.length = blade->length;
	swing.baseBefore = Vector3::FromArray( blade->muzzlePointOld );
	swing.baseNow = Vector3::FromArray( blade->muzzlePoint );
	swing.directionBefore = ragdoll::Normalized( Vector3::FromArray( blade->muzzleDirOld ) );
	swing.directionNow = ragdoll::Normalized( Vector3::FromArray( blade->muzzleDir ) );
	const Vector3 tipBefore = swing.baseBefore + swing.directionBefore * swing.length;
	const Vector3 tipNow = swing.baseNow + swing.directionNow * swing.length;
	// The swept patch is four-sided; the cross product of its diagonals is its normal.
	swing.sweptNormal = ragdoll::Cross( tipNow - swing.baseBefore, tipBefore - swing.baseNow );
	return ragdoll::Length( swing.sweptNormal ) >= 2.0f * swing.length * SMALLEST_SIDEWAYS_SWEEP;
}

// How close a point is to a line segment.
static float DistanceFromSegment( const Vector3 &point, const Vector3 &start, const Vector3 &end )
{
	const Vector3 segment = end - start;
	const float lengthSquared = ragdoll::LengthSquared( segment );
	const float along = ( lengthSquared > ragdoll::SMALL_NUMBER ) ? ragdoll::Clamp( ragdoll::Dot( point - start, segment ) / lengthSquared, 0.0f, 1.0f ) : 0.0f;
	return ragdoll::Length( point - ( start + segment * along ) );
}

// The blade that struck a blow. Jedi Academy applies a blade's damage without saying which blade it
// was, so it is the lit blade that passed closest to where the blow landed (checked where the blade
// was, is now and halfway between): one blade of a staff is always far from the other's blows, and
// each of two sabers is in its own hand. With no point to go by, the first lit blade.
static bool FindStrikingBlade( gentity_t *attacker, const vec3_t hitPoint, int &saberNum, int &bladeNum )
{
	float nearestDistance = 1.0e9f;
	saberNum = -1;
	bladeNum = -1;
	for ( int saber = 0; saber < MAX_SABERS; saber++ )
	{
		for ( int blade = 0; blade < MAX_BLADES; blade++ )
		{
			const bladeInfo_t *lit = LitBlade( attacker, saber, blade );
			if ( !lit )
			{
				continue;
			}
			if ( !hitPoint )
			{
				saberNum = saber;
				bladeNum = blade;
				return true;
			}
			const Vector3 point = Vector3::FromArray( hitPoint );
			const Vector3 baseBefore = Vector3::FromArray( lit->muzzlePointOld );
			const Vector3 baseNow = Vector3::FromArray( lit->muzzlePoint );
			const Vector3 directionBefore = ragdoll::Normalized( Vector3::FromArray( lit->muzzleDirOld ) );
			const Vector3 directionNow = ragdoll::Normalized( Vector3::FromArray( lit->muzzleDir ) );
			float distance = 1.0e9f;
			for ( int sample = 0; sample <= 2; sample++ )
			{
				const float fraction = sample * 0.5f;
				const Vector3 base = baseBefore + ( baseNow - baseBefore ) * fraction;
				const Vector3 direction = ragdoll::Normalized( directionBefore + ( directionNow - directionBefore ) * fraction );
				const float sampleDistance = DistanceFromSegment( point, base, base + direction * lit->length );
				distance = ( sampleDistance < distance ) ? sampleDistance : distance;
			}
			if ( distance < nearestDistance )
			{
				nearestDistance = distance;
				saberNum = saber;
				bladeNum = blade;
			}
		}
	}
	return saberNum >= 0;
}

// True unless the saber's definition keeps this blade from ever dismembering (blunt or stabbing
// weapons: SFL2_NO_DISMEMBERMENT, or SFL2_NO_DISMEMBERMENT2 for blades using the second blade style),
// as WP_SaberApplyDamage does.
static bool BladeCanCut( gentity_t *saberUser, int saberNum, int bladeNum )
{
	saberInfo_t &saber = saberUser->client->ps.saber[saberNum];
	const int noDismemberment = WP_SaberBladeUseSecondBladeStyle( &saber, bladeNum ) ? SFL2_NO_DISMEMBERMENT2 : SFL2_NO_DISMEMBERMENT;
	return ( saber.saberFlags2 & noDismemberment ) == 0;
}

// Sets up a cut along a blade's swing, through the point where it struck.
static void SetUpSwingCut( const BladeSwing &swing, const Vector3 &struckPoint, bool struckPointIsKnown, SliceRequest &request )
{
	request = SliceRequest();
	request.planeNormal = ragdoll::Normalized( swing.sweptNormal );
	request.planePoint = struckPoint;
	request.struckPoint = struckPoint;
	request.hasStruckPoint = struckPointIsKnown;
	request.reachIsLimited = true;
	request.bladeBase = swing.baseNow;
	request.bladeReach = swing.length + REACH_MARGIN;

	// The way the blade was moving where it struck: pieces above a level or slanted cut slide off that way.
	const float alongBlade = ragdoll::Clamp( ragdoll::Dot( struckPoint - swing.baseNow, swing.directionNow ), 0.0f, swing.length );
	const Vector3 motion = ( swing.baseNow + swing.directionNow * alongBlade ) - ( swing.baseBefore + swing.directionBefore * alongBlade );
	request.slideDirection = ragdoll::Normalized( motion - request.planeNormal * ragdoll::Dot( motion, request.planeNormal ) );
}

// Asks for a character to be cut along an attacker's blade swing the next time it is drawn.
// woundsOnly: the blow did not kill, so only arms may come off.
static bool RequestSaberCut( gentity_t *target, gentity_t *attacker, const vec3_t hitPoint, bool woundsOnly )
{
	if ( !g_saberSlicing || !g_saberSlicing->integer )
	{
		return false;
	}
	if ( !target || !target->client || target->s.number <= 0 || target->s.number >= MAX_GENTITIES )
	{
		return false;		// the player is never cut
	}
	if ( target->playerModel < 0 || target->ghoul2.size() <= target->playerModel || characterWasCut[target->s.number]
		|| IsCarriedByGameOrVehicle( target ) )
	{
		return false;
	}
	SliceRequest &request = sliceRequests[target->s.number];
	if ( request.pending && ( woundsOnly || !request.woundsOnly ) )
	{
		return true;		// another blow this frame already asked for a cut at least as deep
	}
	int saberNum, bladeNum;
	BladeSwing swing;
	if ( !FindStrikingBlade( attacker, hitPoint, saberNum, bladeNum ) || !BladeCanCut( attacker, saberNum, bladeNum )
		|| !ReadBladeSwing( attacker, saberNum, bladeNum, swing ) )
	{
		return false;
	}
	const Vector3 struck = hitPoint ? Vector3::FromArray( hitPoint ) : swing.baseNow + swing.directionNow * ( swing.length * 0.5f );
	SetUpSwingCut( swing, struck, hitPoint != NULL, request );
	request.cutterNumber = attacker->s.number;
	request.woundsOnly = woundsOnly;
	request.pending = true;

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: saber %s on %d, cut plane normal (%.2f %.2f %.2f), saber %d blade %d %.0f long\n", woundsOnly ? "wound" : "kill",
				   target->s.number, request.planeNormal.x, request.planeNormal.y, request.planeNormal.z, saberNum, bladeNum, swing.length );
	}
	return true;
}

qboolean G_Slice_SaberKill( gentity_t *target, gentity_t *attacker, const vec3_t hitPoint )
{
	return RequestSaberCut( target, attacker, hitPoint, false ) ? qtrue : qfalse;
}

void G_Slice_SaberWound( gentity_t *target, gentity_t *attacker, const vec3_t hitPoint, int damage, int hitLocation )
{
	const bool armWasHit = hitLocation == HL_ARM_RT || hitLocation == HL_ARM_LT || hitLocation == HL_HAND_RT || hitLocation == HL_HAND_LT;
	if ( armWasHit && damage >= SMALLEST_MAIMING_DAMAGE )
	{
		RequestSaberCut( target, attacker, hitPoint, true );
	}
}

// How close two segments come, firstStart-firstEnd and secondStart-secondEnd. closestOnFirst receives
// the point of the first segment nearest the second.
static float SegmentDistance( const Vector3 &firstStart, const Vector3 &firstEnd, const Vector3 &secondStart, const Vector3 &secondEnd,
							  Vector3 &closestOnFirst )
{
	const Vector3 firstDirection = firstEnd - firstStart;
	const Vector3 secondDirection = secondEnd - secondStart;
	const Vector3 betweenStarts = firstStart - secondStart;
	const float firstLengthSquared = ragdoll::Dot( firstDirection, firstDirection );
	const float secondLengthSquared = ragdoll::Dot( secondDirection, secondDirection );
	const float secondAlongBetween = ragdoll::Dot( secondDirection, betweenStarts );
	float firstFraction = 0.0f;
	float secondFraction = 0.0f;
	if ( firstLengthSquared <= ragdoll::SMALL_NUMBER )
	{
		if ( secondLengthSquared > ragdoll::SMALL_NUMBER )
		{
			secondFraction = ragdoll::Clamp( secondAlongBetween / secondLengthSquared, 0.0f, 1.0f );
		}
	}
	else
	{
		const float firstAlongBetween = ragdoll::Dot( firstDirection, betweenStarts );
		if ( secondLengthSquared <= ragdoll::SMALL_NUMBER )
		{
			firstFraction = ragdoll::Clamp( -firstAlongBetween / firstLengthSquared, 0.0f, 1.0f );
		}
		else
		{
			const float directionsDot = ragdoll::Dot( firstDirection, secondDirection );
			const float denominator = firstLengthSquared * secondLengthSquared - directionsDot * directionsDot;
			firstFraction = ( denominator > ragdoll::SMALL_NUMBER )
				? ragdoll::Clamp( ( directionsDot * secondAlongBetween - firstAlongBetween * secondLengthSquared ) / denominator, 0.0f, 1.0f )
				: 0.0f;
			secondFraction = ( directionsDot * firstFraction + secondAlongBetween ) / secondLengthSquared;
			if ( secondFraction < 0.0f )
			{
				secondFraction = 0.0f;
				firstFraction = ragdoll::Clamp( -firstAlongBetween / firstLengthSquared, 0.0f, 1.0f );
			}
			else if ( secondFraction > 1.0f )
			{
				secondFraction = 1.0f;
				firstFraction = ragdoll::Clamp( ( directionsDot - firstAlongBetween ) / firstLengthSquared, 0.0f, 1.0f );
			}
		}
	}
	closestOnFirst = firstStart + firstDirection * firstFraction;
	return ragdoll::Length( closestOnFirst - ( secondStart + secondDirection * secondFraction ) );
}

void G_Slice_SaberSweep( gentity_t *saberUser, int saberNum, int bladeNum )
{
	if ( !g_saberSlicing || !g_saberSlicing->integer )
	{
		return;
	}
	BladeSwing swing;
	if ( !ReadBladeSwing( saberUser, saberNum, bladeNum, swing ) || !BladeCanCut( saberUser, saberNum, bladeNum ) )
	{
		return;
	}

	// Check the blade at a few points of its swing against every body of every piece.
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		SlicedPiece &piece = pieces[pieceIndex];
		if ( !piece.inUse || piece.cutIsPending || piece.indexes.empty() )
		{
			continue;
		}
		if ( piece.madeByCutter == saberUser->s.number && level.time - piece.madeTime < SAME_SWING_MILLISECONDS )
		{
			continue;		// the swing that made the piece carries on through it: that is not a new cut
		}
		for ( int body = 0; body < piece.simulation.bodyCount && !piece.cutIsPending; body++ )
		{
			const RigidBody &rigidBody = piece.simulation.bodies[body];
			const Vector3 bodyStart = rigidBody.LocalToWorldPoint( rigidBody.spheres[0].localCentre );
			const Vector3 bodyEnd = rigidBody.LocalToWorldPoint( rigidBody.spheres[1].localCentre );
			for ( int sample = 0; sample <= BLADE_SWEEP_SAMPLES; sample++ )
			{
				const float fraction = (float)sample / BLADE_SWEEP_SAMPLES;
				const Vector3 base = swing.baseBefore + ( swing.baseNow - swing.baseBefore ) * fraction;
				const Vector3 direction = ragdoll::Normalized( swing.directionBefore + ( swing.directionNow - swing.directionBefore ) * fraction );
				Vector3 struck;
				if ( SegmentDistance( base, base + direction * swing.length, bodyStart, bodyEnd, struck ) <= rigidBody.spheres[0].radius )
				{
					SetUpSwingCut( swing, struck, true, piece.pendingCut );
					piece.pendingCut.cutterNumber = saberUser->s.number;
					piece.cutIsPending = true;
					break;
				}
			}
		}
	}
}

/*
==================
Cutting pieces again
==================
*/

// Cuts a severed piece again where a blade swept through it. It is divided the same way a character
// is, its own bodies standing in for the character's bones.
static void CutPieceAgain( int pieceIndex )
{
	SlicedPiece &piece = pieces[pieceIndex];
	piece.cutIsPending = false;
	const SliceRequest request = piece.pendingCut;

	// The piece's mesh where it is now, each vertex moved by the bodies it is attached to.
	static slicing::PosedMesh posed;
	posed.Clear();
	const int vertexCount = (int)piece.firstInfluence.size() - 1;
	for ( int vertex = 0; vertex < vertexCount; vertex++ )
	{
		slicing::PosedVertex placed;
		placed.position = Vector3( 0.0f, 0.0f, 0.0f );
		placed.normal = Vector3( 0.0f, 0.0f, 0.0f );
		placed.boneCount = 0;
		for ( int influenceIndex = piece.firstInfluence[vertex]; influenceIndex < piece.firstInfluence[vertex + 1]; influenceIndex++ )
		{
			const VertexInfluence &influence = piece.influences[influenceIndex];
			const RigidBody &body = piece.simulation.bodies[influence.body];
			placed.position += body.LocalToWorldPoint( influence.localPosition ) * influence.weight;
			placed.normal += ragdoll::Rotate( body.orientation, influence.localNormal ) * influence.weight;
			if ( placed.boneCount < 4 )
			{
				placed.bones[placed.boneCount] = influence.body;
				placed.weights[placed.boneCount] = influence.weight;
				placed.boneCount++;
			}
		}
		placed.normal = ragdoll::Normalized( placed.normal );
		placed.s = piece.textureCoordinates[vertex * 2];
		placed.t = piece.textureCoordinates[vertex * 2 + 1];
		posed.vertices.push_back( placed );
	}
	for ( size_t batch = 0; batch < piece.batches.size() && batch < piece.batchShaderNames.size(); batch++ )
	{
		const int shaderSlot = posed.ShaderSlot( piece.batchShaderNames[batch] );
		const int firstIndex = piece.batches[batch].firstIndex;
		for ( int index = firstIndex; index + 2 < firstIndex + piece.batches[batch].numIndexes; index += 3 )
		{
			posed.indexes.push_back( piece.indexes[index] );
			posed.indexes.push_back( piece.indexes[index + 1] );
			posed.indexes.push_back( piece.indexes[index + 2] );
			posed.triangleShaders.push_back( shaderSlot );
		}
	}

	static Simulation whole;
	static CutLayout layout;
	whole = piece.simulation;
	const int pieceCount = DivideBodies( whole, request, layout );
	if ( pieceCount < 2 )
	{
		if ( g_ragdollDebug && g_ragdollDebug->integer )
		{
			gi.Printf( "slice: the cut through a piece of %d bodies did not divide it\n", whole.bodyCount );
		}
		return;
	}
	static std::vector<int> vertexPieces;
	vertexPieces.resize( posed.vertices.size() );
	for ( size_t vertex = 0; vertex < posed.vertices.size(); vertex++ )
	{
		vertexPieces[vertex] = PieceOfVertex( posed.vertices[vertex], PieceBodyCarriers(), whole, layout, request );
	}
	static std::vector<slicing::PosedMesh> pieceMeshes;
	slicing::SplitMesh( posed, vertexPieces, pieceCount, request.planePoint, request.planeNormal, piece.capShaderName, pieceMeshes );
	int piecesWithTriangles = 0;
	for ( int newPiece = 0; newPiece < pieceCount; newPiece++ )
	{
		piecesWithTriangles += pieceMeshes[newPiece].indexes.empty() ? 0 : 1;
	}
	if ( piecesWithTriangles < 2 )
	{
		if ( g_ragdollDebug && g_ragdollDebug->integer )
		{
			gi.Printf( "slice: the cut through a piece left %d of its %d parts with triangles\n", piecesWithTriangles, pieceCount );
		}
		return;
	}

	// The new pieces replace this one.
	const std::string capShaderName = piece.capShaderName;
	const int sourceEntityNumber = piece.sourceEntityNumber;
	byte shaderRGBA[4];
	for ( int channel = 0; channel < 4; channel++ )
	{
		shaderRGBA[channel] = piece.shaderRGBA[channel];
	}
	ReleasePiece( piece );
	for ( int newPiece = 0; newPiece < pieceCount; newPiece++ )
	{
		AddPiece( pieceMeshes[newPiece], &whole, PieceBodyCarriers(), &layout, newPiece, ( newPiece == 0 ) ? 1.0f : -1.0f, request,
				  sourceEntityNumber, capShaderName, shaderRGBA );
	}
	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: a blade cut a piece into %d\n", piecesWithTriangles );
	}
}

/*
==================
Maimed characters
==================
*/

// The piece holding the pelvis: what is left of a character that lives on.
static int PieceHoldingPelvis( const CutLayout &layout )
{
	const int pelvis = ragdoll::BODY_PELVIS;
	return layout.pieceOf[pelvis][layout.isSplit[pelvis] ? FRONT_PART : WHOLE_BODY];
}

// The skin a character is drawn with. Its model keeps the skin's index among the configuration
// strings, which are saved with the game, so this stays right after a saved game is loaded.
static std::string SkinPathOf( gentity_t *entity, const std::string &meshPath )
{
	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	if ( model.mCustomSkin > 0 && model.mCustomSkin < MAX_CHARSKINS )
	{
		char skinPath[MAX_QPATH];
		gi.GetConfigstring( CS_CHARSKINS + model.mCustomSkin, skinPath, sizeof( skinPath ) );
		if ( skinPath[0] )
		{
			return skinPath;
		}
	}
	const size_t lastSlash = meshPath.find_last_of( "/\\" );
	return ( lastSlash == std::string::npos ? std::string() : meshPath.substr( 0, lastSlash + 1 ) ) + "model_default.skin";
}

// Which of a model's mesh surfaces are switched on. Some characters have parts switched off, and the
// surfaces of limbs cut off are switched off too.
static void FindShownSurfaces( gentity_t *entity, const slicing::CharacterMesh &mesh, std::vector<bool> &surfaceIsShown )
{
	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	surfaceIsShown.resize( mesh.surfaces.size() );
	for ( size_t surface = 0; surface < mesh.surfaces.size(); surface++ )
	{
		const int flags = gi.G2API_GetSurfaceRenderStatus( &model, mesh.surfaces[surface].name.c_str() );
		surfaceIsShown[surface] = ( flags < 0 ) || !( flags & GHOUL2_SURFACE_OFF );
	}
}

// Switches off the model's surfaces left with nothing to draw (every vertex on bodies cut off), so
// blades and shots no longer hit them.
static void SwitchOffLostSurfaces( gentity_t *entity, const slicing::CharacterMesh &original, const int *carrierBody, int bodyCount,
								   int missingBodies )
{
	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	for ( size_t surfaceIndex = 0; surfaceIndex < original.surfaces.size(); surfaceIndex++ )
	{
		const slicing::MeshSurface &surface = original.surfaces[surfaceIndex];
		bool everyVertexIsLost = !surface.vertices.empty();
		for ( size_t vertex = 0; vertex < surface.vertices.size() && everyVertexIsLost; vertex++ )
		{
			const slicing::BindVertex &bindVertex = surface.vertices[vertex];
			const int strongest = StrongestBody( bindVertex.boneCount, bindVertex.bones, bindVertex.weights, carrierBody, bodyCount, 0 );
			everyVertexIsLost = strongest >= 0 && ( missingBodies & ( 1 << strongest ) ) != 0;
		}
		if ( everyVertexIsLost )
		{
			gi.G2API_SetSurfaceOnOff( &model, surface.name.c_str(), GHOUL2_SURFACE_OFF );
		}
	}
}

// Makes what is left of a maimed character's mesh from its model's own mesh: every cut it survived
// is made again on the rest pose, keeping the part holding the pelvis each time. Ghoul2 stops drawing
// the model, and the surfaces of the limbs cut off are switched off.
static void RebuildMaimedMesh( gentity_t *entity, MaimedCharacter &maimed )
{
	maimed.meshIsBuilt = true;		// once, even if it fails
	maimed.mesh.surfaces.clear();
	maimed.indexes.clear();			// the drawing buffers are filled in again for the new mesh
	if ( entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return;
	}
	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	const ragdoll::Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &model ) );
	const slicing::CharacterMesh *original = slicing::LoadCharacterMesh( maimed.meshPath.c_str(), maimed.skinPath.c_str() );
	if ( !skeleton || !original )
	{
		return;
	}

	// The rest pose: every bone where the model was made, in model space, at scale 1.
	static Transform restFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool restFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	for ( int bone = 0; bone < skeleton->boneCount; bone++ )
	{
		restFrames[bone] = skeleton->bones[bone].basePose;
		restFrameIsValid[bone] = true;
	}
	const Transform modelSpace( Quaternion(), Vector3( 0.0f, 0.0f, 0.0f ) );
	static ragdoll::Rig rig;
	static Simulation restRagdoll;
	static CutLayout layout;
	if ( !ragdoll::BuildHumanoidRagdoll( rig, restRagdoll, *skeleton, restFrames, restFrameIsValid, modelSpace, 1.0f ) )
	{
		return;
	}
	std::vector<bool> surfaceIsShown;
	FindShownSurfaces( entity, *original, surfaceIsShown );
	static slicing::PosedMesh remaining;
	slicing::PoseCharacterMesh( *original, *skeleton, restFrames, modelSpace, 1.0f, remaining, &surfaceIsShown );

	int missingBodies = 0;
	static std::vector<int> vertexPieces;
	static std::vector<slicing::PosedMesh> pieceMeshes;
	for ( size_t cutIndex = 0; cutIndex < maimed.cuts.size(); cutIndex++ )
	{
		const SurvivedCut &cut = maimed.cuts[cutIndex];
		if ( cut.bone < 0 || cut.bone >= skeleton->boneCount )
		{
			continue;
		}
		SliceRequest request;
		request.planePoint = restFrames[cut.bone].TransformPoint( cut.pointInBone );
		request.planeNormal = ragdoll::Normalized( ragdoll::Rotate( restFrames[cut.bone].rotation, cut.normalInBone ) );
		request.divisionIsForced = true;
		request.forcedDividedBodies = cut.dividedBodies;
		request.missingBodies = missingBodies;
		const int pieceCount = DivideBodies( restRagdoll, request, layout );
		const int keptPiece = PieceHoldingPelvis( layout );
		if ( pieceCount < 2 || keptPiece < 0 )
		{
			continue;
		}
		vertexPieces.resize( remaining.vertices.size() );
		for ( size_t vertex = 0; vertex < remaining.vertices.size(); vertex++ )
		{
			vertexPieces[vertex] = PieceOfVertex( remaining.vertices[vertex], rig.carrierBody, restRagdoll, layout, request );
		}
		slicing::SplitMesh( remaining, vertexPieces, pieceCount, request.planePoint, request.planeNormal, original->capShaderName, pieceMeshes );
		remaining = pieceMeshes[keptPiece];

		// Bodies with no part left in the kept piece are gone.
		for ( int body = 0; body < restRagdoll.bodyCount; body++ )
		{
			if ( layout.isMissing[body] )
			{
				continue;
			}
			bool partIsKept = false;
			for ( int part = 0; part < BODY_PART_COUNT; part++ )
			{
				partIsKept = partIsKept || layout.pieceOf[body][part] == keptPiece;
			}
			if ( !partIsKept )
			{
				missingBodies |= 1 << body;
			}
		}
	}
	maimed.missingBodies = missingBodies;
	slicing::MakeRestMesh( remaining, maimed.mesh );
	maimed.mesh.capShaderName = original->capShaderName;

	model.mFlags |= GHOUL2_NORENDER;
	SwitchOffLostSurfaces( entity, *original, rig.carrierBody, restRagdoll.bodyCount, missingBodies );
	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: made what is left of %d from %d cuts (missing bodies %x, %d triangles)\n", entity->s.number, (int)maimed.cuts.size(),
				   missingBodies, (int)remaining.triangleShaders.size() );
	}
}

// A character that loses a hand drops what it was holding in it. The right hand holds the weapon:
// a lightsaber (or a staff) falls away, as when it is knocked from the hand, and anything else is
// dropped as when the Force pulls it away. The left hand holds the second of two sabers, which falls
// to the ground as a saber pickup, the way a dead character drops it (TossClientItems).
static void DisarmCharacter( gentity_t *entity, bool losesRightHand, bool losesLeftHand )
{
	gclient_t *client = entity->client;
	if ( !client )
	{
		return;
	}
	if ( client->ps.weapon == WP_SABER )
	{
		if ( losesLeftHand && client->ps.dualSabers && entity->weaponModel[1] >= 0 && client->ps.saber[1].name && client->ps.saber[1].name[0] )
		{
			if ( G_DropSaberItem( client->ps.saber[1].name, client->ps.saber[1].blade[0].color, client->renderInfo.handLPoint,
								  client->ps.velocity, entity->currentAngles ) != NULL )
			{
				WP_RemoveSaber( entity, 1 );
			}
		}
		if ( losesRightHand && !client->ps.saberInFlight && entity->weaponModel[0] >= 0 )
		{
			WP_SaberLose( entity, NULL );
		}
	}
	else if ( losesRightHand && client->ps.weapon != WP_NONE )
	{
		WP_DropWeapon( entity, NULL );
	}
}

// A blow that did not kill. If the cut takes off nothing but parts of arms, those parts fall away and
// the character lives on without them (and without its weapon, if its right hand went). Returns false
// if the cut would take off more than that: the blow then only wounds, as usual.
static bool MaimCharacter( gentity_t *entity, const CutLayout &layout, const ragdoll::Rig &rig, const Simulation &wholeRagdoll,
						   const std::vector<slicing::PosedMesh> &pieceMeshes, const SliceRequest &request, const Transform *boneFrames,
						   const Transform &modelToWorld, float modelScale, const std::string &meshPath, const std::string &skinPath,
						   const std::string &capShaderName )
{
	const int keptPiece = PieceHoldingPelvis( layout );
	if ( layout.pieceCount < 2 || keptPiece < 0 )
	{
		return false;
	}
	int referenceBody = -1;
	int dividedBodies = 0;
	bool losesRightHand = false;
	bool losesLeftHand = false;
	for ( int body = 0; body < layout.bodyCount; body++ )
	{
		if ( layout.isMissing[body] )
		{
			continue;
		}
		for ( int part = 0; part < BODY_PART_COUNT; part++ )
		{
			const int piece = layout.pieceOf[body][part];
			if ( piece < 0 || piece == keptPiece )
			{
				continue;
			}
			if ( !IsArmBody( body ) )
			{
				return false;		// more than an arm would come off
			}
			losesRightHand = losesRightHand || body == ragdoll::BODY_RIGHT_HAND;
			losesLeftHand = losesLeftHand || body == ragdoll::BODY_LEFT_HAND;
		}
		if ( layout.isSplit[body] )
		{
			dividedBodies |= 1 << body;
			if ( referenceBody < 0 )
			{
				referenceBody = body;
			}
		}
	}
	bool somethingVisibleComesOff = false;
	for ( int piece = 0; piece < layout.pieceCount; piece++ )
	{
		somethingVisibleComesOff = somethingVisibleComesOff || ( piece != keptPiece && !pieceMeshes[piece].indexes.empty() );
	}
	if ( referenceBody < 0 || !somethingVisibleComesOff )
	{
		return false;
	}

	// The parts cut off fall away.
	byte shaderRGBA[4];
	CharacterColour( entity, shaderRGBA );
	for ( int piece = 0; piece < layout.pieceCount; piece++ )
	{
		if ( piece != keptPiece )
		{
			AddPiece( pieceMeshes[piece], &wholeRagdoll, rig.carrierBody, &layout, piece, -1.0f, request, entity->s.number, capShaderName,
					  shaderRGBA );
		}
	}

	// Remember the cut in the frame of a bone it went through, then make what is left of the mesh.
	MaimedCharacter &maimed = maimedCharacters[entity->s.number];
	if ( maimed.cuts.empty() )
	{
		maimed.meshPath = meshPath;
		maimed.skinPath = skinPath;
	}
	const int bone = rig.bodyBoneIndex[referenceBody];
	const Vector3 pointInModel = modelToWorld.InverseTransformPoint( request.planePoint ) * ( 1.0f / modelScale );
	SurvivedCut cut;
	cut.bone = bone;
	cut.pointInBone = boneFrames[bone].InverseTransformPoint( pointInModel );
	cut.normalInBone = ragdoll::InverseRotate( boneFrames[bone].rotation, ragdoll::InverseRotate( modelToWorld.rotation, request.planeNormal ) );
	cut.dividedBodies = dividedBodies;
	maimed.cuts.push_back( cut );
	RebuildMaimedMesh( entity, maimed );

	if ( losesRightHand || losesLeftHand )
	{
		DisarmCharacter( entity, losesRightHand, losesLeftHand );
	}
	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: maimed %d (%d cuts survived, missing bodies %x%s%s)\n", entity->s.number, (int)maimed.cuts.size(),
				   maimed.missingBodies, losesRightHand ? ", lost the right hand" : "", losesLeftHand ? ", lost the left hand" : "" );
	}
	return true;
}

void G_Slice_AddMaimedCharacter( gentity_t *entity, const refEntity_t &drawnModel, int renderTime )
{
	if ( !entity || !entity->client )
	{
		return;
	}
	const std::map<int, MaimedCharacter>::iterator found = maimedCharacters.find( entity->s.number );
	if ( found == maimedCharacters.end() || entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return;
	}
	// A cloaked character's body is not drawn (only its shimmer, which Ghoul2 draws from the model;
	// a maimed one has none).
	const int *powerups = entity->client->ps.powerups;
	if ( powerups[PW_CLOAKED] || powerups[PW_UNCLOAKING] || powerups[PW_DISRUPTION] )
	{
		return;
	}
	const float *drawnOrigin = drawnModel.origin;
	const float *lightingOrigin = ( drawnModel.renderfx & RF_LIGHTING_ORIGIN ) ? drawnModel.lightingOrigin : drawnModel.origin;
	MaimedCharacter &maimed = found->second;
	if ( !maimed.meshIsBuilt )
	{
		RebuildMaimedMesh( entity, maimed );
	}
	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	const ragdoll::Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &model ) );
	if ( maimed.mesh.surfaces.empty() || !skeleton )
	{
		return;
	}

	// Bend what is left of the mesh into the pose the character is drawn in.
	static Transform boneFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool boneFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime, boneFrames, boneFrameIsValid );
	const Vector3 origin = Vector3::FromArray( drawnOrigin );
	const Transform modelToWorld( QuaternionFromAxis( drawnModel.axis ), origin );
	slicing::PoseCharacterMesh( maimed.mesh, *skeleton, boneFrames, modelToWorld, ModelScaleOf( entity ), maimed.posed );

	// The triangles, texture coordinates and shader batches never change: they are filled in once.
	const slicing::PosedMesh &posed = maimed.posed;
	const size_t vertexCount = posed.vertices.size();
	if ( maimed.indexes.empty() )
	{
		maimed.textureCoordinates.resize( vertexCount * 2 );
		for ( size_t vertex = 0; vertex < vertexCount; vertex++ )
		{
			maimed.textureCoordinates[vertex * 2] = posed.vertices[vertex].s;
			maimed.textureCoordinates[vertex * 2 + 1] = posed.vertices[vertex].t;
		}
		maimed.batches.clear();
		for ( size_t shaderSlot = 0; shaderSlot < posed.shaderNames.size(); shaderSlot++ )
		{
			dynamicMeshBatch_t batch;
			batch.shader = ShaderHandle( posed.shaderNames[shaderSlot] );
			batch.firstIndex = (int)maimed.indexes.size();
			for ( size_t triangle = 0; triangle < posed.triangleShaders.size(); triangle++ )
			{
				if ( posed.triangleShaders[triangle] == (int)shaderSlot )
				{
					maimed.indexes.push_back( posed.indexes[triangle * 3] );
					maimed.indexes.push_back( posed.indexes[triangle * 3 + 1] );
					maimed.indexes.push_back( posed.indexes[triangle * 3 + 2] );
				}
			}
			batch.numIndexes = (int)maimed.indexes.size() - batch.firstIndex;
			if ( batch.numIndexes > 0 )
			{
				maimed.batches.push_back( batch );
			}
		}
	}
	if ( maimed.indexes.empty() || maimed.textureCoordinates.size() != vertexCount * 2 )
	{
		return;
	}
	maimed.positions.resize( vertexCount * 3 );
	maimed.normals.resize( vertexCount * 3 );
	float largestDistanceSquared = 0.0f;
	for ( size_t vertex = 0; vertex < vertexCount; vertex++ )
	{
		const Vector3 fromOrigin = posed.vertices[vertex].position - origin;
		const float distanceSquared = ragdoll::LengthSquared( fromOrigin );
		if ( distanceSquared > largestDistanceSquared )
		{
			largestDistanceSquared = distanceSquared;
		}
		fromOrigin.CopyToArray( &maimed.positions[vertex * 3] );
		posed.vertices[vertex].normal.CopyToArray( &maimed.normals[vertex * 3] );
	}

	maimed.renderMesh.numVertexes = (int)vertexCount;
	maimed.renderMesh.xyz = (const float (*)[3])&maimed.positions[0];
	maimed.renderMesh.normal = (const float (*)[3])&maimed.normals[0];
	maimed.renderMesh.st = (const float (*)[2])&maimed.textureCoordinates[0];
	maimed.renderMesh.numIndexes = (int)maimed.indexes.size();
	maimed.renderMesh.indexes = &maimed.indexes[0];
	maimed.renderMesh.numBatches = (int)maimed.batches.size();
	maimed.renderMesh.batches = maimed.batches.empty() ? NULL : &maimed.batches[0];

	refEntity_t meshEntity;
	memset( &meshEntity, 0, sizeof( meshEntity ) );
	meshEntity.reType = RT_DYNAMIC_MESH;
	meshEntity.dynamicMesh = &maimed.renderMesh;
	meshEntity.axis[0][0] = 1.0f;
	meshEntity.axis[1][1] = 1.0f;
	meshEntity.axis[2][2] = 1.0f;
	VectorCopy( drawnOrigin, meshEntity.origin );
	VectorCopy( lightingOrigin, meshEntity.lightingOrigin );
	meshEntity.renderfx = RF_LIGHTING_ORIGIN | RF_SHADOW_PLANE;	// (shadows as a character's, when they are on)
	meshEntity.radius = sqrtf( largestDistanceSquared );
	for ( int channel = 0; channel < 4; channel++ )
	{
		meshEntity.shaderRGBA[channel] = drawnModel.shaderRGBA[channel];	// the character's colour tint
	}
	cgi_R_AddRefEntityToScene( &meshEntity );
}

/*
==================
Saved games
==================
*/

// The cuts maimed characters survived travel in saved games as script variables, which are saved and
// loaded with the others (a game saved before slicing existed simply has none). One record per
// character, separated by ';' (the name of a three-part skin contains '|'): "entity mesh skin
// cutCount", then "bone point normal dividedBodies" for each cut.
//
// Jedi Academy refuses to load a script variable of 1024 characters or more (it ends the game with an
// error), so the records are shared out between variables of at most LONGEST_SAVED_CUTS_TEXT
// characters: "slicing_maimedCharacters", then "slicing_maimedCharacters2", "...3" and so on.

static const char RECORD_SEPARATOR = ';';

static std::string SavedCutsVariableName( int variableIndex )
{
	return ( variableIndex == 0 ) ? std::string( SAVED_CUTS_VARIABLE ) : std::string( va( "%s%d", SAVED_CUTS_VARIABLE, variableIndex + 1 ) );
}

void G_Slice_FinishWriteSaveGame( void )
{
	for ( int variableIndex = 0; variableIndex < MOST_SAVED_CUTS_VARIABLES; variableIndex++ )
	{
		const std::string name = SavedCutsVariableName( variableIndex );
		if ( Quake3Game()->VariableDeclared( name.c_str() ) != CQuake3GameInterface::VTYPE_NONE )
		{
			Quake3Game()->FreeVariable( name.c_str() );
		}
	}
}

void G_Slice_WriteSaveGame( void )
{
	std::vector<std::string> variableTexts;
	for ( std::map<int, MaimedCharacter>::const_iterator maimed = maimedCharacters.begin(); maimed != maimedCharacters.end(); ++maimed )
	{
		if ( maimed->second.cuts.empty() )
		{
			continue;
		}
		char text[MAX_QPATH * 2 + 64];
		Com_sprintf( text, sizeof( text ), "%d %s %s %d", maimed->first, maimed->second.meshPath.c_str(), maimed->second.skinPath.c_str(),
					 (int)maimed->second.cuts.size() );
		std::string record = text;
		for ( size_t cutIndex = 0; cutIndex < maimed->second.cuts.size(); cutIndex++ )
		{
			const SurvivedCut &cut = maimed->second.cuts[cutIndex];
			Com_sprintf( text, sizeof( text ), " %d %g %g %g %g %g %g %d", cut.bone, cut.pointInBone.x, cut.pointInBone.y, cut.pointInBone.z,
						 cut.normalInBone.x, cut.normalInBone.y, cut.normalInBone.z, cut.dividedBodies );
			record += text;
		}
		record += RECORD_SEPARATOR;
		if ( record.size() > LONGEST_SAVED_CUTS_TEXT )
		{
			gi.Printf( S_COLOR_YELLOW "slice: the wounds of %d are too many to save\n", maimed->first );
			continue;
		}
		if ( variableTexts.empty() || variableTexts.back().size() + record.size() > LONGEST_SAVED_CUTS_TEXT )
		{
			if ( (int)variableTexts.size() >= MOST_SAVED_CUTS_VARIABLES )
			{
				gi.Printf( S_COLOR_YELLOW "slice: too many maimed characters to save them all\n" );
				break;
			}
			variableTexts.push_back( std::string() );
		}
		variableTexts.back() += record;
	}
	G_Slice_FinishWriteSaveGame();
	for ( size_t variableIndex = 0; variableIndex < variableTexts.size(); variableIndex++ )
	{
		const std::string name = SavedCutsVariableName( (int)variableIndex );
		Quake3Game()->DeclareVariable( TK_STRING, name.c_str() );
		Quake3Game()->SetStringVariable( name.c_str(), variableTexts[variableIndex].c_str() );
	}
}

void G_Slice_ReadSaveGame( void )
{
	for ( int variableIndex = 0; variableIndex < MOST_SAVED_CUTS_VARIABLES; variableIndex++ )
	{
		const std::string name = SavedCutsVariableName( variableIndex );
		if ( Quake3Game()->VariableDeclared( name.c_str() ) != CQuake3GameInterface::VTYPE_STRING )
		{
			break;
		}
		const char *text = NULL;
		if ( Quake3Game()->GetStringVariable( name.c_str(), &text ) && text )
		{
			std::istringstream records( text );
			std::string record;
			while ( std::getline( records, record, RECORD_SEPARATOR ) )
			{
				std::istringstream fields( record );
				int entityNumber = -1;
				int cutCount = 0;
				MaimedCharacter maimed;
				if ( !( fields >> entityNumber >> maimed.meshPath >> maimed.skinPath >> cutCount ) )
				{
					continue;
				}
				for ( int cutIndex = 0; cutIndex < cutCount; cutIndex++ )
				{
					SurvivedCut cut;
					if ( fields >> cut.bone >> cut.pointInBone.x >> cut.pointInBone.y >> cut.pointInBone.z >> cut.normalInBone.x
								>> cut.normalInBone.y >> cut.normalInBone.z >> cut.dividedBodies )
					{
						maimed.cuts.push_back( cut );
					}
				}
				if ( entityNumber > 0 && entityNumber < MAX_GENTITIES && !maimed.cuts.empty() )
				{
					maimedCharacters[entityNumber] = maimed;		// its mesh is made again the first time it is drawn
				}
			}
		}
	}
	G_Slice_FinishWriteSaveGame();		// the variables are the slicer's, not the scripts'
	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: the saved game had %d maimed characters\n", (int)maimedCharacters.size() );
	}
}

/*
==================
G_Slice_Command
==================
*/
void G_Slice_Command( gentity_t *user, const char *mode )
{
	if ( !user || !user->client )
	{
		return;
	}
	vec3_t forward, right, up, eye, end;
	AngleVectors( user->client->ps.viewangles, forward, right, up );
	VectorCopy( user->client->ps.origin, eye );
	eye[2] += user->client->ps.viewheight;
	VectorMA( eye, 4096.0f, forward, end );

	trace_t trace;
	gi.trace( &trace, eye, NULL, NULL, end, user->s.number, MASK_SHOT, G2_NOCOLLIDE, 0 );
	const Vector3 flatForward = ragdoll::Normalized( Vector3( forward[0], forward[1], 0.0f ) );
	const Vector3 flatRight = ragdoll::Normalized( Vector3( right[0], right[1], 0.0f ) );

	if ( mode && ( mode[0] == 'p' || mode[0] == 'P' ) )
	{
		// The severed piece nearest where the user is looking is cut down the middle.
		const Vector3 aimedAt = Vector3::FromArray( trace.endpos );
		SlicedPiece *nearestPiece = NULL;
		Vector3 nearestBodyPosition;
		float nearestDistance = SLICE_PIECE_SEARCH_DISTANCE;
		for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
		{
			for ( int body = 0; pieces[pieceIndex].inUse && body < pieces[pieceIndex].simulation.bodyCount; body++ )
			{
				const float distance = ragdoll::Length( pieces[pieceIndex].simulation.bodies[body].position - aimedAt );
				if ( distance < nearestDistance )
				{
					nearestDistance = distance;
					nearestPiece = &pieces[pieceIndex];
					nearestBodyPosition = pieces[pieceIndex].simulation.bodies[body].position;
				}
			}
		}
		if ( !nearestPiece )
		{
			gi.Printf( "slice: aim at a severed piece first\n" );
			return;
		}
		nearestPiece->pendingCut = SliceRequest();
		nearestPiece->pendingCut.planePoint = nearestBodyPosition;
		nearestPiece->pendingCut.planeNormal = flatRight;
		nearestPiece->pendingCut.slideDirection = flatForward;
		nearestPiece->cutIsPending = true;
		gi.Printf( "slice: cutting the piece %.0f units from the crosshair\n", nearestDistance );
		return;
	}

	if ( trace.entityNum < 0 || trace.entityNum >= ENTITYNUM_WORLD )
	{
		gi.Printf( "slice: aim at a character first\n" );
		return;
	}
	gentity_t *target = &g_entities[trace.entityNum];
	if ( !target->client || target->playerModel < 0 || target->ghoul2.size() <= target->playerModel )
	{
		gi.Printf( "slice: that is not a character\n" );
		return;
	}
	if ( IsCarriedByGameOrVehicle( target ) )
	{
		gi.Printf( "slice: vehicles, their riders and characters held by monsters are never cut\n" );
		return;
	}

	// The cut is set up as seen by the user: "straight down the middle" is a vertical plane facing
	// sideways to the user's view. It goes right through the body, wherever a blade could reach.
	SliceRequest &request = sliceRequests[target->s.number];
	request = SliceRequest();
	request.planePoint = Vector3::FromArray( target->currentOrigin );
	request.slideDirection = flatForward;
	if ( mode && ( mode[0] == 'h' || mode[0] == 'H' ) )
	{
		request.planeNormal = WORLD_UP;
		request.planePoint.z += HORIZONTAL_CUT_HEIGHT;
	}
	else if ( mode && ( mode[0] == 'd' || mode[0] == 'D' ) )
	{
		request.planeNormal = ragdoll::Normalized( flatRight + WORLD_UP * DIAGONAL_CUT_RISE );
		request.planePoint.z += HORIZONTAL_CUT_HEIGHT;
	}
	else if ( mode && ( mode[0] == 'a' || mode[0] == 'A' ) )
	{
		// The right forearm comes off and the character lives on. The plane is set when the cut is
		// made, across the middle of the forearm wherever it is drawn.
		request.woundsOnly = true;
		request.aimedBody = ragdoll::BODY_RIGHT_FOREARM;
	}
	else
	{
		request.planeNormal = flatRight;
	}
	request.pending = true;

	// Any other cut kills. The body is replaced by its pieces the next time it is drawn.
	if ( !request.woundsOnly && target->health > 0 )
	{
		G_Damage( target, user, user, NULL, NULL, target->health + 1000, DAMAGE_NO_PROTECTION | DAMAGE_NO_KNOCKBACK, MOD_UNKNOWN );
	}
}

/*
==================
G_Slice_CutIfRequested
==================
*/
qboolean G_Slice_CutIfRequested( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3] )
{
	const int entityNumber = entity->s.number;
	if ( entityNumber < 0 || entityNumber >= MAX_GENTITIES || !sliceRequests[entityNumber].pending )
	{
		return qfalse;
	}
	SliceRequest request = sliceRequests[entityNumber];
	sliceRequests[entityNumber].pending = false;
	if ( entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return qfalse;
	}

	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	const ragdoll::Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &model ) );
	const std::string meshPath = model.mFileName;
	const std::string skinPath = SkinPathOf( entity, meshPath );
	const slicing::CharacterMesh *original = slicing::LoadCharacterMesh( meshPath.c_str(), skinPath.c_str() );
	if ( !skeleton || !original || original->surfaces.empty() )
	{
		gi.Printf( "slice: could not read the mesh or skeleton of %s\n", meshPath.c_str() );
		return qfalse;
	}

	// The mesh to cut: what is left of the character if it was maimed before, else its model's own
	// (without the surfaces switched off on it).
	const slicing::CharacterMesh *mesh = original;
	std::vector<bool> surfaceIsShown;
	const std::map<int, MaimedCharacter>::iterator maimed = maimedCharacters.find( entityNumber );
	if ( maimed != maimedCharacters.end() )
	{
		if ( !maimed->second.meshIsBuilt )
		{
			RebuildMaimedMesh( entity, maimed->second );
		}
		if ( !maimed->second.mesh.surfaces.empty() )
		{
			mesh = &maimed->second.mesh;
			request.missingBodies = maimed->second.missingBodies;
		}
	}
	if ( mesh == original )
	{
		FindShownSurfaces( entity, *original, surfaceIsShown );
	}

	// Pose the mesh and build the ragdoll bodies exactly as the character is drawn this frame.
	static Transform boneFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool boneFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime, boneFrames, boneFrameIsValid );
	const Transform modelToWorld( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) );
	const float modelScale = ModelScaleOf( entity );

	static slicing::PosedMesh posed;
	slicing::PoseCharacterMesh( *mesh, *skeleton, boneFrames, modelToWorld, modelScale, posed, surfaceIsShown.empty() ? NULL : &surfaceIsShown );
	static ragdoll::Rig rig;
	static Simulation wholeRagdoll;
	static CutLayout layout;
	const bool haveRagdoll = ragdoll::BuildHumanoidRagdoll( rig, wholeRagdoll, *skeleton, boneFrames, boneFrameIsValid, modelToWorld, modelScale );
	if ( request.woundsOnly && !haveRagdoll )
	{
		return qfalse;		// without bodies there is no telling an arm from the rest
	}
	if ( haveRagdoll && request.aimedBody >= 0 && request.aimedBody < wholeRagdoll.bodyCount )
	{
		// Across the middle of the aimed body, cutting nothing else.
		const RigidBody &aimed = wholeRagdoll.bodies[request.aimedBody];
		request.planePoint = aimed.position;
		request.planeNormal = ragdoll::Normalized( aimed.LocalToWorldPoint( aimed.spheres[1].localCentre ) -
												   aimed.LocalToWorldPoint( aimed.spheres[0].localCentre ) );
		request.reachIsLimited = true;
		request.bladeBase = aimed.position;
		request.bladeReach = 0.0f;
	}

	// Which piece every vertex goes to.
	static std::vector<int> vertexPieces;
	vertexPieces.resize( posed.vertices.size() );
	int pieceCount;
	if ( haveRagdoll )
	{
		pieceCount = DivideBodies( wholeRagdoll, request, layout );
		for ( size_t vertex = 0; vertex < posed.vertices.size(); vertex++ )
		{
			vertexPieces[vertex] = PieceOfVertex( posed.vertices[vertex], rig.carrierBody, wholeRagdoll, layout, request );
		}
	}
	else
	{
		// No bodies to go by: the plane divides the whole mesh in two.
		pieceCount = 2;
		for ( size_t vertex = 0; vertex < posed.vertices.size(); vertex++ )
		{
			vertexPieces[vertex] = ( SignedDistance( posed.vertices[vertex].position, request ) >= 0.0f ) ? 0 : 1;
		}
	}

	static std::vector<slicing::PosedMesh> pieceMeshes;
	slicing::SplitMesh( posed, vertexPieces, pieceCount, request.planePoint, request.planeNormal, original->capShaderName, pieceMeshes );
	int piecesWithTriangles = 0;
	for ( int piece = 0; piece < pieceCount; piece++ )
	{
		piecesWithTriangles += pieceMeshes[piece].indexes.empty() ? 0 : 1;
	}
	if ( piecesWithTriangles < 2 )
	{
		if ( g_ragdollDebug && g_ragdollDebug->integer )
		{
			gi.Printf( "slice: the cut on %d did not separate anything\n", entityNumber );
		}
		return qfalse;
	}

	// A blow that did not kill only takes off arms; the character lives on.
	if ( request.woundsOnly )
	{
		MaimCharacter( entity, layout, rig, wholeRagdoll, pieceMeshes, request, boneFrames, modelToWorld, modelScale, meshPath, skinPath,
					   original->capShaderName );
		return qfalse;
	}

	byte shaderRGBA[4];
	CharacterColour( entity, shaderRGBA );
	for ( int piece = 0; piece < pieceCount; piece++ )
	{
		AddPiece( pieceMeshes[piece], haveRagdoll ? &wholeRagdoll : NULL, rig.carrierBody, haveRagdoll ? &layout : NULL, piece,
				  ( piece == 0 ) ? 1.0f : -1.0f, request, entityNumber, original->capShaderName, shaderRGBA );
	}

	// The pieces replace the character: stop any ragdoll on it and hide it. (A character's entity
	// flags are copied from its player state every frame, so the player state is what counts.)
	G_Ragdoll_OnEntityFreed( entity );
	maimedCharacters.erase( entityNumber );
	characterWasCut[entityNumber] = true;
	entity->s.eFlags |= EF_NODRAW;
	if ( entity->client )
	{
		entity->client->ps.eFlags |= EF_NODRAW;
	}
	entity->contents = 0;
	gi.linkentity( entity );

	// Nothing is left of the character but its pieces, so its body is removed in the next game frame,
	// the way Jedi Academy removes a disintegrated one (player_die). Kept hidden instead, it would turn
	// solid again (a dead NPC's think makes its body a corpse every frame) and blades and shots would
	// keep striking it, invisible; it would also come back from a saved game without its pieces.
	entity->takedamage = qfalse;
	entity->e_ThinkFunc = thinkF_G_FreeEntity;
	entity->nextthink = level.time + FRAMETIME;

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: cut %d into %d pieces (%d triangles before, skin %s, cap shader %s)\n", entityNumber, piecesWithTriangles,
				   (int)posed.triangleShaders.size(), skinPath.c_str(), original->capShaderName.empty() ? "none" : original->capShaderName.c_str() );
	}
	return qtrue;
}

/*
==================
G_Slice_AddPiecesToScene
==================
*/
void G_Slice_AddPiecesToScene( int renderTime )
{
	bool anyPiece = false;
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		anyPiece = anyPiece || pieces[pieceIndex].inUse;
	}
	if ( !anyPiece )
	{
		lastStepRenderTime = renderTime;
		unsimulatedSeconds = 0.0f;
		return;
	}

	// Pieces a blade swept through since the last frame are cut again first.
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		if ( pieces[pieceIndex].inUse && pieces[pieceIndex].cutIsPending )
		{
			CutPieceAgain( pieceIndex );
		}
	}

	// Fixed physics steps, the same way the ragdolls are stepped. All moving pieces are stepped
	// together, so they collide with each other.
	float frameSeconds = ( lastStepRenderTime < 0 ) ? 0.0f : ( renderTime - lastStepRenderTime ) / 1000.0f;
	lastStepRenderTime = renderTime;
	frameSeconds = ragdoll::Clamp( frameSeconds, 0.0f, LONGEST_FRAME_SECONDS );
	unsimulatedSeconds += frameSeconds;

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

	while ( unsimulatedSeconds >= PHYSICS_STEP_SECONDS )
	{
		unsimulatedSeconds -= PHYSICS_STEP_SECONDS;
		pieceScene.Clear();
		for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
		{
			SlicedPiece &piece = pieces[pieceIndex];
			if ( !piece.inUse )
			{
				continue;
			}
			piece.secondsAlive += PHYSICS_STEP_SECONDS;
			Simulation &simulation = piece.simulation;
			const float settling = ragdoll::Clamp( ( piece.secondsAlive - SETTLING_STARTS_SECONDS ) / SECONDS_TO_FULL_SETTLING, 0.0f, 1.0f );
			const float keep = expf( -FULL_SETTLING_DAMPING * settling * PHYSICS_STEP_SECONDS );
			for ( int body = 0; body < simulation.bodyCount; body++ )
			{
				RigidBody &rigidBody = simulation.bodies[body];
				piece.previousBodyTransforms[body] = Transform( rigidBody.orientation, rigidBody.position );
				rigidBody.linearVelocity *= keep;
				rigidBody.angularVelocity *= keep;
			}
			if ( piece.secondsAlive < PIECES_FREEZE_SECONDS )
			{
				pieceScene.AddSimulation( &simulation, SweepPieceThroughWorld, &piece );
			}
		}
		pieceScene.Step( PHYSICS_STEP_SECONDS, settings );
	}

	// Bend each piece's mesh to its bodies and draw it. The mesh is placed in the world by its origin
	// alone (its vertices are already turned the way the bodies are).
	const float fraction = ragdoll::Clamp( unsimulatedSeconds / PHYSICS_STEP_SECONDS, 0.0f, 1.0f );
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		SlicedPiece &piece = pieces[pieceIndex];
		if ( !piece.inUse || piece.renderMesh.numBatches <= 0 || piece.simulation.bodyCount <= 0 )
		{
			continue;
		}
		Vector3 meshOrigin;
		float meshRadius;
		BendPieceMesh( piece, fraction, meshOrigin, meshRadius );

		refEntity_t entity;
		memset( &entity, 0, sizeof( entity ) );
		entity.reType = RT_DYNAMIC_MESH;
		entity.dynamicMesh = &piece.renderMesh;
		entity.axis[0][0] = 1.0f;
		entity.axis[1][1] = 1.0f;
		entity.axis[2][2] = 1.0f;
		meshOrigin.CopyToArray( entity.origin );
		meshOrigin.CopyToArray( entity.lightingOrigin );
		entity.renderfx = RF_LIGHTING_ORIGIN | RF_SHADOW_PLANE;	// (shadows as a character's, when they are on)
		entity.radius = meshRadius;
		for ( int channel = 0; channel < 4; channel++ )
		{
			entity.shaderRGBA[channel] = piece.shaderRGBA[channel];
		}
		cgi_R_AddRefEntityToScene( &entity );
	}
}

qboolean G_Slice_WasCut( int entityNumber )
{
	return ( entityNumber >= 0 && entityNumber < MAX_GENTITIES && characterWasCut[entityNumber] ) ? qtrue : qfalse;
}

void G_Slice_ForgetCharacter( int entityNumber )
{
	if ( entityNumber >= 0 && entityNumber < MAX_GENTITIES )
	{
		characterWasCut[entityNumber] = false;
		sliceRequests[entityNumber].pending = false;
		maimedCharacters.erase( entityNumber );
	}
}

/*
==================
G_Slice_Reset
==================
*/
void G_Slice_Reset( void )
{
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		ReleasePiece( pieces[pieceIndex] );
	}
	for ( int entityNumber = 0; entityNumber < MAX_GENTITIES; entityNumber++ )
	{
		sliceRequests[entityNumber].pending = false;
		characterWasCut[entityNumber] = false;
	}
	maimedCharacters.clear();
	shaderHandles.clear();
	slicing::ClearCharacterMeshCache();
	lastStepRenderTime = -1;
	unsimulatedSeconds = 0.0f;
}
