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

// g_outcast.cpp -- Jedi Outcast's campaign played on Jedi Academy's game code, see g_outcast.h.

#include "g_local.h"
#include "g_items.h"
#include "g_outcast.h"
#include "b_local.h"
#include "g_navigator.h"

qboolean G_OutcastDataAvailable( void )
{
	// Set by the campaign chooser to Jedi Outcast's GameData folder, which the engine then searches
	// beneath Jedi Academy's. Read-only once the engine is running.
	static cvar_t *outcastPath = NULL;
	if ( !outcastPath )
	{
		outcastPath = gi.cvar( "fs_outcastpath", "", 0 );
	}
	return ( outcastPath->string[0] ) ? qtrue : qfalse;
}

qboolean G_IsOutcastCampaign( void )
{
	// Chosen at New Game (ui_main.cpp) and kept in saved games, so loading one restores its story.
	static cvar_t *campaign = NULL;
	if ( !campaign )
	{
		campaign = gi.cvar( "g_campaign", "academy", CVAR_SAVEGAME );
	}
	return ( G_OutcastDataAvailable() && !Q_stricmp( campaign->string, "outcast" ) ) ? qtrue : qfalse;
}

const char *G_PlayerScriptName( void )
{
	return G_IsOutcastCampaign() ? "kyle" : "player";
}

int G_Outcast_PlayerWeapon( int weapon )
{
	if ( !G_IsOutcastCampaign() )
	{
		return weapon;
	}
	switch ( weapon )
	{
	case WP_BRYAR_PISTOL:	return WP_BLASTER_PISTOL;
	case WP_STUN_BATON:		return WP_MELEE;
	default:				return weapon;
	}
}

void G_Outcast_GiveStartingGear( gentity_t *player )
{
	gclient_t *client = player->client;

	if ( client->ps.stats[STAT_WEAPONS] & ( 1 << WP_STUN_BATON ) )
	{
		client->ps.stats[STAT_WEAPONS] &= ~( 1 << WP_STUN_BATON );
		client->ps.stats[STAT_WEAPONS] |= ( 1 << G_Outcast_PlayerWeapon( WP_STUN_BATON ) );
	}
	const int pistol = G_Outcast_PlayerWeapon( WP_BRYAR_PISTOL );
	client->ps.stats[STAT_WEAPONS] |= ( 1 << pistol );
	client->ps.inventory[INV_ELECTROBINOCULARS] = 1;

	for ( int ammoType = AMMO_NONE + 1; ammoType < AMMO_THERMAL; ammoType++ )
	{
		client->ps.ammo[ammoType] = ammoData[ammoType].max;
	}

	client->ps.weapon = pistol;
	client->ps.weaponstate = WEAPON_READY;
}

// Jedi Outcast's mission objectives, in Jedi Outcast's order (its objectives.h). Scripts name them
// (SET_OBJECTIVE_SHOW "KEJIM_POST_OBJ1") and the datapad shows OBJECTIVES_<name> from Jedi Outcast's
// strings. They use slots 1 onwards of Jedi Academy's objective list (slot 0 is Jedi Academy's light
// side objective, which the game code watches), so saved games keep their layout.
static const char *const outcastObjectives[] =
{
	"KEJIM_POST_OBJ1", "KEJIM_POST_OBJ2", "KEJIM_BASE_OBJ1", "KEJIM_BASE_OBJ2",
	"KEJIM_BASE_OBJ3", "ARTUS_MINE_OBJ1", "ARTUS_MINE_OBJ2", "ARTUS_MINE_OBJ3",
	"ARTUS_DETENTION_OBJ1", "ARTUS_DETENTION_OBJ2", "ARTUS_TOPSIDE_OBJ1", "ARTUS_TOPSIDE_OBJ2",
	"YAVIN_TEMPLE_OBJ1", "YAVIN_TRIAL_OBJ1", "YAVIN_TRIAL_OBJ2", "NS_STREETS_OBJ1",
	"NS_STREETS_OBJ2", "NS_STREETS_OBJ3", "NS_HIDEOUT_OBJ1", "NS_HIDEOUT_OBJ2",
	"NS_STARPAD_OBJ1", "NS_STARPAD_OBJ2", "NS_STARPAD_OBJ3", "NS_STARPAD_OBJ4",
	"NS_STARPAD_OBJ5", "BESPIN_UNDERCITY_OBJ1", "BESPIN_UNDERCITY_OBJ2", "BESPIN_STREETS_OBJ1",
	"BESPIN_STREETS_OBJ2", "BESPIN_PLATFORM_OBJ1", "BESPIN_PLATFORM_OBJ2", "CAIRN_BAY_OBJ1",
	"CAIRN_BAY_OBJ2", "CAIRN_ASSEMBLY_OBJ1", "CAIRN_REACTOR_OBJ1", "CAIRN_REACTOR_OBJ2",
	"CAIRN_DOCK1_OBJ1", "CAIRN_DOCK1_OBJ2", "DOOM_COMM_OBJ1", "DOOM_COMM_OBJ2",
	"DOOM_COMM_OBJ3", "DOOM_DETENTION_OBJ1", "DOOM_DETENTION_OBJ2", "DOOM_SHIELDS_OBJ1",
	"DOOM_SHIELDS_OBJ2", "YAVIN_SWAMP_OBJ1", "YAVIN_SWAMP_OBJ2", "YAVIN_CANYON_OBJ1",
	"YAVIN_CANYON_OBJ2", "YAVIN_COURTYARD_OBJ1", "YAVIN_COURTYARD_OBJ2", "YAVIN_FINAL_OBJ1",
	"KEJIM_POST_OBJ3", "KEJIM_POST_OBJ4", "KEJIM_POST_OBJ5", "ARTUS_DETENTION_OBJ3",
	"DOOM_COMM_OBJ4", "DOOM_SHIELDS_OBJ3", "DEMO_OBJ1", "DEMO_OBJ2",
	"DEMO_OBJ3", "DEMO_OBJ4",
};

int G_Outcast_ObjectiveSlot( const char *name )
{
	for ( int i = 0; i < (int)ARRAY_LEN( outcastObjectives ); i++ )
	{
		if ( !Q_stricmp( name, outcastObjectives[i] ) )
		{
			return i + 1;
		}
	}
	return -1;
}

const char *G_Outcast_ObjectiveName( int slot )
{
	return ( slot >= 1 && slot <= (int)ARRAY_LEN( outcastObjectives ) ) ? outcastObjectives[slot - 1] : "";
}

static void G_Outcast_SpawnSizedNavGoal( gentity_t *ent, int halfWidth )
{
	VectorSet( ent->mins, -halfWidth, -halfWidth, -24 );
	VectorSet( ent->maxs, halfWidth, halfWidth, 32 );
	ent->s.origin[2] += 0.125;
	if ( !( ent->spawnflags & 1 ) && G_CheckInSolid( ent, qfalse ) )
	{//SOLID_OK not set
		gi.Printf( S_COLOR_RED "ERROR: %s %s at %s in solid!\n", ent->classname, ent->targetname, vtos( ent->currentOrigin ) );
	}
	// Jedi Academy counts the goal reached within this radius or once it is inside the NPC's box. Jedi
	// Outcast's touch-reach (the NPC's box touching this small one) is a few units wider than the box;
	// Jedi Academy's own navgoals use 12, which its NPCs reliably get within when they stop.
	TAG_Add( ent->targetname, NULL, ent->s.origin, ent->s.angles, 12, RTF_NAVGOAL );

	ent->classname = "navgoal";

	// Unlike Jedi Outcast, Jedi Academy only routes NPCs to points in its navigation graph, so the
	// goal joins it as Jedi Academy's own navgoals do (or a cutscene can wait forever for an NPC
	// to arrive, as valley's first one did)
	NAV::SpawnedPoint( ent, NAV::PT_GOALNODE );

	G_FreeEntity( ent );
}

void SP_waypoint_navgoal_8( gentity_t *ent ) { G_Outcast_SpawnSizedNavGoal( ent, 8 ); }
void SP_waypoint_navgoal_4( gentity_t *ent ) { G_Outcast_SpawnSizedNavGoal( ent, 4 ); }
void SP_waypoint_navgoal_2( gentity_t *ent ) { G_Outcast_SpawnSizedNavGoal( ent, 2 ); }
void SP_waypoint_navgoal_1( gentity_t *ent ) { G_Outcast_SpawnSizedNavGoal( ent, 1 ); }

void G_Outcast_AddAnimations( animation_t *animations )
{
	if ( !G_OutcastDataAvailable() )
	{
		return;
	}

	// The renderer adds Jedi Outcast's frames when it first reads the skeleton (already read when a
	// level loads its characters, maybe not yet when a saved game is being read)
	gi.G2API_PrecacheGhoul2Model( "models/players/_humanoid/_humanoid.gla" );
	const int firstOutcastFrame = gi.Cvar_VariableIntegerValue( "r_outcastAnimationsStart" );
	if ( firstOutcastFrame <= 0 )
	{
		gi.Printf( S_COLOR_YELLOW "Jedi Outcast's animations were not added to the humanoid skeleton\n" );
		return;
	}

	// Jedi Outcast's own list, which Jedi Academy's file of the same name hides
	char *text = NULL;
	if ( gi.FS_ReadOutcastFile( "models/players/_humanoid/_humanoid.cfg", (void **)&text ) <= 0
		&& gi.FS_ReadOutcastFile( "models/players/_humanoid/animation.cfg", (void **)&text ) <= 0 )
	{
		gi.Printf( S_COLOR_YELLOW "Jedi Outcast's humanoid animation.cfg was not found\n" );
		return;
	}

	// Each line: name, first frame, number of frames, loop frames, frames per second
	int numAdded = 0;
	const char *text_p = text;
	COM_BeginParseSession();
	while ( 1 )
	{
		const char *token = COM_Parse( &text_p );
		if ( !token || !token[0] )
		{
			break;
		}

		const int animNum = GetIDForString( animTable, token );
		if ( animNum < 0 )
		{//not an animation (or one this game has no name for): skip the rest of the line
			while ( token[0] )
			{
				token = COM_ParseExt( &text_p, qfalse );
			}
			continue;
		}
		int firstFrame, numFrames, loopFrames;
		float fps;
		token = COM_Parse( &text_p );	firstFrame = atoi( token );
		token = COM_Parse( &text_p );	numFrames = atoi( token );
		token = COM_Parse( &text_p );	loopFrames = atoi( token );
		token = COM_Parse( &text_p );	fps = atof( token );

		if ( animations[animNum].numFrames > 0 || numFrames <= 0 )
		{//Jedi Academy has its own
			continue;
		}

		animation_t &animation = animations[animNum];
		animation.glaIndex = 0;
		animation.firstFrame = firstOutcastFrame + firstFrame;
		animation.numFrames = numFrames;
		animation.loopFrames = loopFrames;
		if ( fps == 0 )
		{
			fps = 1;
		}
		// as G_ParseAnimationFile: negative frames per second play backwards
		animation.frameLerp = ( fps < 0 ) ? floor( 1000.0f / fps ) : ceil( 1000.0f / fps );
		numAdded++;
	}
	COM_EndParseSession();
	gi.FS_FreeFile( text );

	gi.Printf( "Added %d of Jedi Outcast's animations from frame %d of the humanoid skeleton\n", numAdded, firstOutcastFrame );
}

void G_Outcast_AddAnimationsAfterLoad( void )
{
	if ( !G_OutcastDataAvailable() )
	{
		return;
	}
	for ( int fileIndex = 0; fileIndex < level.numKnownAnimFileSets; fileIndex++ )
	{
		if ( !Q_stricmp( level.knownAnimFileSets[fileIndex].filename, "_humanoid" ) )
		{
			G_Outcast_AddAnimations( level.knownAnimFileSets[fileIndex].animations );
		}
	}
}
