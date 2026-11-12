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

// g_slice.cpp -- lightsaber slicing, prototype. See g_slice.h.

#include "g_headers.h"
#include "g_local.h"
#include "../cgame/cg_local.h"
#include "g_slice.h"
#include "g_slice_mesh.h"
#include "g_ragdoll.h"
#include "g_ragdoll_rig.h"
#include "g_ragdoll_solver.h"

#include <map>
#include <math.h>
#include <string>
#include <vector>

using ragdoll::Vector3;
using ragdoll::Quaternion;
using ragdoll::Transform;
using ragdoll::RigidBody;
using ragdoll::Simulation;
using ragdoll::SimulationSettings;
using ragdoll::WorldSweepResult;

extern cvar_t *g_ragdollDebug;

/*
==================
Tuning
==================
*/

static const int MAXIMUM_PIECES = 16;					// the oldest piece is recycled after this many

static const float PHYSICS_STEP_SECONDS = 1.0f / 60.0f;
static const int PHYSICS_SUBSTEPS = 8;
static const float LONGEST_FRAME_SECONDS = 0.1f;		// longer frames (hitches, pauses) are shortened to this

static const float HORIZONTAL_CUT_HEIGHT = 8.0f;		// above the character's origin: just above the hips
static const float DIAGONAL_CUT_RISE = 0.8f;			// how steeply a diagonal cut climbs across the body

// How the pieces start moving once cut.
static const float PIECE_SEPARATION_SPEED = 25.0f;		// units per second away from the cut
static const float PIECE_SLIDE_SPEED = 40.0f;			// the upper piece of a level or slanted cut also slides off
static const float PIECE_TUMBLE_SPEED = 1.5f;			// radians per second, tipping away from the cut

// The pieces' rigid bodies, sized from their bounding boxes.
static const float PIECE_DENSITY = 0.0008f;				// mass per cubic unit of the bounding box
static const float LIGHTEST_PIECE_MASS = 3.0f;
static const float HEAVIEST_PIECE_MASS = 40.0f;
static const float THINNEST_PIECE_RADIUS = 2.0f;		// the collision capsule's radius
static const float THICKEST_PIECE_RADIUS = 10.0f;

static const Vector3 WORLD_UP( 0.0f, 0.0f, 1.0f );

/*
==================
State
==================
*/

// A cut asked for by the console command, made the next time the character is drawn.
struct SliceRequest
{
	bool pending;
	Vector3 planePoint;
	Vector3 planeNormal;
	Vector3 slideDirection;		// horizontal: which way the upper piece slides off
};

// One piece of a cut character: its mesh in its own space, and the rigid body that moves it.
struct SlicedPiece
{
	bool inUse;
	int sourceEntityNumber;		// the character it came from (its collision box is ignored)
	int createdTime;

	std::vector<float> positions;			// three per vertex, in the piece's own space
	std::vector<float> normals;				// three per vertex
	std::vector<float> textureCoordinates;	// two per vertex
	std::vector<int> indexes;				// grouped into batches by shader
	std::vector<dynamicMeshBatch_t> batches;
	dynamicMesh_t renderMesh;				// points into the vectors above
	float radius;							// from the centre to the furthest corner of its bounds

	Simulation simulation;					// one rigid body
	Transform previousTransform;			// for smoothing between physics steps
};

static SliceRequest sliceRequests[MAX_GENTITIES];
static bool characterWasCut[MAX_GENTITIES];		// replaced by its pieces: no longer drawn
static SlicedPiece pieces[MAXIMUM_PIECES];
static int lastStepRenderTime = -1;
static float unsimulatedSeconds = 0.0f;
static std::map<std::string, qhandle_t> shaderHandles;

/*
==================
Helpers
==================
*/

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

static float ModelScaleOf( const gentity_t *entity )
{
	const float averageScale = ( entity->s.modelScale[0] + entity->s.modelScale[1] + entity->s.modelScale[2] ) / 3.0f;
	return ( averageScale > 0.0f ) ? averageScale : 1.0f;
}

static float Component( const Vector3 &vector, int axis )
{
	return ( axis == 0 ) ? vector.x : ( ( axis == 1 ) ? vector.y : vector.z );
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

static void ReleasePiece( SlicedPiece &piece )
{
	piece.inUse = false;
	std::vector<float>().swap( piece.positions );
	std::vector<float>().swap( piece.normals );
	std::vector<float>().swap( piece.textureCoordinates );
	std::vector<int>().swap( piece.indexes );
	std::vector<dynamicMeshBatch_t>().swap( piece.batches );
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

/*
==================
Pieces
==================
*/

// Gives a piece its rigid body: a capsule along the longest side of its bounds, with the mass and
// spin resistance of a box that size, starting to move away from the cut.
static void SetUpPieceBody( SlicedPiece &piece, const Vector3 &centre, const Vector3 &size, const Vector3 &awayFromCut, const Vector3 &slide )
{
	Simulation &simulation = piece.simulation;
	simulation.bodyCount = 1;
	simulation.jointCount = 0;
	simulation.bodyPairCollides[0][0] = false;

	const float mass = ragdoll::Clamp( size.x * size.y * size.z * PIECE_DENSITY, LIGHTEST_PIECE_MASS, HEAVIEST_PIECE_MASS );
	RigidBody &body = simulation.bodies[0];
	body.position = centre;
	body.orientation = Quaternion();
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
	const float thinnerSide = ( Component( size, ( longestAxis + 1 ) % 3 ) < Component( size, ( longestAxis + 2 ) % 3 ) )
		? Component( size, ( longestAxis + 1 ) % 3 ) : Component( size, ( longestAxis + 2 ) % 3 );
	const float radius = ragdoll::Clamp( thinnerSide * 0.5f, THINNEST_PIECE_RADIUS, THICKEST_PIECE_RADIUS );
	float reach = Component( size, longestAxis ) * 0.5f - radius;
	if ( reach < 0.0f )
	{
		reach = 0.0f;
	}
	Vector3 axisDirection( 0.0f, 0.0f, 0.0f );
	if ( longestAxis == 0 ) axisDirection.x = 1.0f;
	else if ( longestAxis == 1 ) axisDirection.y = 1.0f;
	else axisDirection.z = 1.0f;
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

	body.linearVelocity = awayFromCut * PIECE_SEPARATION_SPEED + slide * PIECE_SLIDE_SPEED;
	body.angularVelocity = ragdoll::Normalized( ragdoll::Cross( WORLD_UP, awayFromCut ) ) * PIECE_TUMBLE_SPEED;
	body.substepStartPosition = body.position;
	body.substepStartOrientation = body.orientation;
	piece.previousTransform = Transform( body.orientation, body.position );
}

// Turns one side of a cut into a piece: its mesh is moved into the piece's own space (centred on its
// bounds) and grouped by shader, and it gets a rigid body.
static void AddPiece( const slicing::PosedMesh &mesh, const Vector3 &awayFromCut, const Vector3 &slide, int sourceEntityNumber )
{
	if ( mesh.indexes.empty() || mesh.vertices.empty() )
	{
		return;
	}
	SlicedPiece &piece = FreePiece();
	ReleasePiece( piece );

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
	const Vector3 centre = ( minimum + maximum ) * 0.5f;

	piece.positions.reserve( mesh.vertices.size() * 3 );
	piece.normals.reserve( mesh.vertices.size() * 3 );
	piece.textureCoordinates.reserve( mesh.vertices.size() * 2 );
	for ( size_t vertex = 0; vertex < mesh.vertices.size(); vertex++ )
	{
		const slicing::PosedVertex &source = mesh.vertices[vertex];
		const Vector3 local = source.position - centre;
		piece.positions.push_back( local.x );
		piece.positions.push_back( local.y );
		piece.positions.push_back( local.z );
		piece.normals.push_back( source.normal.x );
		piece.normals.push_back( source.normal.y );
		piece.normals.push_back( source.normal.z );
		piece.textureCoordinates.push_back( source.s );
		piece.textureCoordinates.push_back( source.t );
	}

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
		}
	}

	piece.renderMesh.numVertexes = (int)mesh.vertices.size();
	piece.renderMesh.xyz = (const float (*)[3])&piece.positions[0];
	piece.renderMesh.normal = (const float (*)[3])&piece.normals[0];
	piece.renderMesh.st = (const float (*)[2])&piece.textureCoordinates[0];
	piece.renderMesh.numIndexes = (int)piece.indexes.size();
	piece.renderMesh.indexes = &piece.indexes[0];
	piece.renderMesh.numBatches = (int)piece.batches.size();
	piece.renderMesh.batches = piece.batches.empty() ? NULL : &piece.batches[0];
	piece.radius = ragdoll::Length( maximum - centre );

	SetUpPieceBody( piece, centre, maximum - minimum, awayFromCut, slide );
	piece.sourceEntityNumber = sourceEntityNumber;
	piece.createdTime = level.time;
	piece.inUse = true;
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

	// The cut is set up as seen by the user: "straight down the middle" is a vertical plane facing
	// sideways to the user's view.
	const Vector3 flatForward = ragdoll::Normalized( Vector3( forward[0], forward[1], 0.0f ) );
	const Vector3 flatRight = ragdoll::Normalized( Vector3( right[0], right[1], 0.0f ) );
	SliceRequest &request = sliceRequests[target->s.number];
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
	else
	{
		request.planeNormal = flatRight;
	}
	request.pending = true;

	// A cut like this kills. The body is replaced by its pieces the next time it is drawn.
	if ( target->health > 0 )
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
	const SliceRequest request = sliceRequests[entityNumber];
	sliceRequests[entityNumber].pending = false;
	if ( entity->playerModel < 0 || entity->ghoul2.size() <= entity->playerModel )
	{
		return qfalse;
	}

	CGhoul2Info &model = entity->ghoul2[entity->playerModel];
	const ragdoll::Skeleton *skeleton = ragdoll::LoadSkeleton( gi.G2API_GetGLAName( &model ) );
	const std::string meshPath = model.mFileName;
	const size_t lastSlash = meshPath.find_last_of( "/\\" );
	const std::string skinPath = ( lastSlash == std::string::npos ? std::string() : meshPath.substr( 0, lastSlash + 1 ) ) + "model_default.skin";
	const slicing::CharacterMesh *mesh = slicing::LoadCharacterMesh( meshPath.c_str(), skinPath.c_str() );
	if ( !skeleton || !mesh || mesh->surfaces.empty() )
	{
		gi.Printf( "slice: could not read the mesh or skeleton of %s\n", meshPath.c_str() );
		return qfalse;
	}

	// Pose the mesh exactly as it is drawn this frame, then cut it.
	static Transform boneFrames[ragdoll::MAXIMUM_SKELETON_BONES];
	static bool boneFrameIsValid[ragdoll::MAXIMUM_SKELETON_BONES];
	ragdoll::ReadCurrentBoneFrames( entity->ghoul2, entity->playerModel, *skeleton, renderTime, boneFrames, boneFrameIsValid );
	const Transform modelToWorld( QuaternionFromAxis( renderAxis ), Vector3::FromArray( renderOrigin ) );

	static slicing::PosedMesh posed;
	static slicing::PosedMesh front;
	static slicing::PosedMesh back;
	slicing::PoseCharacterMesh( *mesh, *skeleton, boneFrames, modelToWorld, ModelScaleOf( entity ), posed );
	if ( !slicing::SliceMesh( posed, request.planePoint, request.planeNormal, mesh->capShaderName, front, back ) )
	{
		gi.Printf( "slice: the cut missed the body\n" );
		return qfalse;
	}

	// The piece above a level or slanted cut slides off; the pieces of an upright cut fall apart.
	const Vector3 noSlide( 0.0f, 0.0f, 0.0f );
	const float upward = ragdoll::Dot( request.planeNormal, WORLD_UP );
	AddPiece( front, request.planeNormal, ( upward > 0.1f ) ? request.slideDirection : noSlide, entityNumber );
	AddPiece( back, request.planeNormal * -1.0f, ( upward < -0.1f ) ? request.slideDirection : noSlide, entityNumber );

	// The pieces replace the character: stop any ragdoll on it and hide it. (A character's entity
	// flags are copied from its player state every frame, so the player state is what counts.)
	G_Ragdoll_OnEntityFreed( entity );
	characterWasCut[entityNumber] = true;
	entity->s.eFlags |= EF_NODRAW;
	if ( entity->client )
	{
		entity->client->ps.eFlags |= EF_NODRAW;
	}
	entity->contents = 0;
	gi.linkentity( entity );

	if ( g_ragdollDebug && g_ragdollDebug->integer )
	{
		gi.Printf( "slice: cut %d into pieces of %d and %d triangles (%d triangles before, cap shader %s)\n", entityNumber,
				   (int)front.triangleShaders.size(), (int)back.triangleShaders.size(), (int)posed.triangleShaders.size(),
				   mesh->capShaderName.empty() ? "none" : mesh->capShaderName.c_str() );
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

	// Fixed physics steps, the same way the ragdolls are stepped.
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
		for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
		{
			SlicedPiece &piece = pieces[pieceIndex];
			if ( piece.inUse )
			{
				const RigidBody &body = piece.simulation.bodies[0];
				piece.previousTransform = Transform( body.orientation, body.position );
				piece.simulation.Step( PHYSICS_STEP_SECONDS, settings, SweepPieceThroughWorld, &piece );
			}
		}
	}

	// Where the pieces are, once a second (g_ragdollDebug 1).
	static int lastReportTime = 0;
	if ( g_ragdollDebug && g_ragdollDebug->integer && renderTime - lastReportTime >= 1000 )
	{
		lastReportTime = renderTime;
		for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
		{
			const SlicedPiece &piece = pieces[pieceIndex];
			if ( piece.inUse )
			{
				const Vector3 &position = piece.simulation.bodies[0].position;
				gi.Printf( "slice: piece %d at %.0f %.0f %.0f, %d vertices, %d batches, radius %.0f\n", pieceIndex,
						   position.x, position.y, position.z, piece.renderMesh.numVertexes, piece.renderMesh.numBatches, piece.radius );
			}
		}
	}

	// Draw each piece where it is, smoothed between the last two physics steps.
	const float fraction = ragdoll::Clamp( unsimulatedSeconds / PHYSICS_STEP_SECONDS, 0.0f, 1.0f );
	for ( int pieceIndex = 0; pieceIndex < MAXIMUM_PIECES; pieceIndex++ )
	{
		const SlicedPiece &piece = pieces[pieceIndex];
		if ( !piece.inUse || piece.renderMesh.numBatches <= 0 )
		{
			continue;
		}
		const RigidBody &body = piece.simulation.bodies[0];
		const Vector3 position = piece.previousTransform.position + ( body.position - piece.previousTransform.position ) * fraction;
		const Quaternion orientation = ragdoll::Slerp( piece.previousTransform.rotation, body.orientation, fraction );

		refEntity_t entity;
		memset( &entity, 0, sizeof( entity ) );
		entity.reType = RT_DYNAMIC_MESH;
		entity.dynamicMesh = &piece.renderMesh;
		position.CopyToArray( entity.origin );
		VectorCopy( entity.origin, entity.oldorigin );
		VectorCopy( entity.origin, entity.lightingOrigin );
		ragdoll::Rotate( orientation, Vector3( 1.0f, 0.0f, 0.0f ) ).CopyToArray( entity.axis[0] );
		ragdoll::Rotate( orientation, Vector3( 0.0f, 1.0f, 0.0f ) ).CopyToArray( entity.axis[1] );
		ragdoll::Rotate( orientation, Vector3( 0.0f, 0.0f, 1.0f ) ).CopyToArray( entity.axis[2] );
		entity.renderfx = RF_LIGHTING_ORIGIN;
		entity.radius = piece.radius;
		entity.shaderRGBA[0] = entity.shaderRGBA[1] = entity.shaderRGBA[2] = entity.shaderRGBA[3] = 255;
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
	shaderHandles.clear();
	slicing::ClearCharacterMeshCache();
	lastStepRenderTime = -1;
	unsimulatedSeconds = 0.0f;
}
