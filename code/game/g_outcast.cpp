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

qboolean G_IsOutcastCampaign( void )
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

const char *G_PlayerScriptName( void )
{
	return G_IsOutcastCampaign() ? "kyle" : "player";
}

void G_Outcast_GiveStartingGear( gentity_t *player )
{
	gclient_t *client = player->client;

	client->ps.stats[STAT_WEAPONS] |= ( 1 << WP_BRYAR_PISTOL );
	client->ps.inventory[INV_ELECTROBINOCULARS] = 1;

	for ( int ammoType = AMMO_NONE + 1; ammoType < AMMO_THERMAL; ammoType++ )
	{
		client->ps.ammo[ammoType] = ammoData[ammoType].max;
	}

	client->ps.weapon = WP_BRYAR_PISTOL;
	client->ps.weaponstate = WEAPON_READY;
}

void G_Outcast_AddAnimations( animation_t *animations )
{
	if ( !G_IsOutcastCampaign() )
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
	if ( !G_IsOutcastCampaign() )
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
