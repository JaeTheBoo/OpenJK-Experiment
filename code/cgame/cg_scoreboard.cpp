/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

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

#include "cg_headers.h"

#include "cg_media.h"
#include "../game/objectives.h"
#include "../game/b_local.h"
#include "../game/g_outcast.h"

#define	SCOREBOARD_WIDTH	(26*BIGCHAR_WIDTH)


/*
static void Scoreboard_Draw( void )
{
	vec4_t	newColor;
/*
		player = g_entities[0];
		if( player->client->ps.persistant[PERS_ACCURACY_SHOTS] ) {
			accuracy = player->client->ps.persistant[PERS_ACCURACY_HITS] * 100 / player->client->ps.persistant[PERS_ACCURACY_SHOTS];
		}
*/
/*	cg.LCARSTextTime = 0;	//	Turn off LCARS screen

	// Background
	newColor[0] = colorTable[CT_BLACK][0];
	newColor[1] = colorTable[CT_BLACK][1];
	newColor[2] = colorTable[CT_BLACK][2];
	newColor[3] = 0.5;
	cgi_R_SetColor(newColor);
	CG_DrawPic( 137, 73, 475, 300, cgs.media.whiteShader);	// Background
	CG_DrawPic( 120, 99,  18, 256, cgs.media.whiteShader);	// Background
	CG_DrawPic(  40, 94,  66, 266, cgs.media.whiteShader);	// Background

	// Right side box
	cgi_R_SetColor( colorTable[CT_DKBROWN1]);
	CG_DrawPic( 120, 354,  32, 32, cgs.media.status_corner_16_18);
	CG_DrawPic(  94, 356,  16, 32, cgs.media.status_corner_8_16_b);
	CG_DrawPic(  94, 73,  16, 32, cgs.media.status_corner_8_22);

	CG_DrawPic(135,73,  302, 22, cgs.media.whiteShader);	// Top

	CG_DrawPic(120, 100,  18, 12, cgs.media.whiteShader);	// Middle Top
	CG_DrawPic(120, 353,  18, 4,  cgs.media.whiteShader);	// Middle Bottom

	CG_DrawPic(130,357,  482, 18, cgs.media.whiteShader);	// Bottom

	// Left side box
	cgi_R_SetColor( colorTable[CT_DKBROWN1]);
	CG_DrawPic(40,73,  56, 22, cgs.media.whiteShader);	// Top

	CG_DrawPic(98,95,  8, 17, cgs.media.whiteShader);		// Middle Top
	CG_DrawPic(98,353, 8, 7, cgs.media.whiteShader);		// Middle Bottom

	CG_DrawPic(40,357, 58, 18, cgs.media.whiteShader);	// Bottom

	CG_DrawProportionalString( 356, 208, "%", UI_RIGHT | UI_SMALLFONT, colorTable[CT_LTPURPLE1] );
	CG_DrawProportionalString( 610, 72, ingame_text[IGT_MISSIONANALYSIS],UI_RIGHT| CG_BIGFONT, colorTable[CT_LTORANGE] );

	CG_PrintScreenGraphics(statsmenu_graphics,SMG_MAX);

}
*/



/*
=================
CG_MissionFailed
=================
*/
int statusTextIndex = -1;

// Jedi Outcast's menus have no mission failed menu: as there, the reason is written on the screen and
// the fire button restarts (ESCAPE opens the menus).
static void CG_DrawOutcastMissionFailed(void)
{
	char text[1024]={0};
	char reason[MAX_STRING_CHARS];
	int w;
	const int y = 230;

	cgi_SP_GetStringTextString( "SP_INGAME_MISSIONFAILED", text, sizeof(text) );
	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontMedium, 1.2f);
	cgi_R_Font_DrawString(320 - w/2, y, text, colorTable[CT_HUD_RED], cgs.media.qhFontMedium, -1, 1.2f);

	gi.Cvar_VariableStringBuffer( "ui_missionfailed_text", reason, sizeof(reason) );
	text[0] = '\0';
	cgi_SP_GetStringTextString( (reason[0] == '@') ? reason+1 : reason, text, sizeof(text) );
	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontMedium, 1.2f);
	cgi_R_Font_DrawString(320 - w/2, y+30, text, colorTable[CT_HUD_RED], cgs.media.qhFontMedium, -1, 1.2f);

	cgi_SP_GetStringTextString( "SP_INGAME_RELOADMISSION", text, sizeof(text) );
	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontSmall, 1.0f);
	cgi_R_Font_DrawString(320 - w/2, 450, text, colorTable[CT_CYAN], cgs.media.qhFontSmall, -1, 1.0f);
}

void CG_MissionFailed(void)
{
	char *text;

	if (!cg.missionFailedScreen)
	{
		if ( !G_OutcastInterface() )
		{
			cgi_UI_SetActive_Menu("missionfailed_menu");
		}
		cg.missionFailedScreen = qtrue;

		switch (statusTextIndex)
		{
			case -1:	//Our HERO DIED!!!
				text = "@SP_INGAME_MISSIONFAILED_PLAYER";
				break;
			case MISSIONFAILED_JAN:
				text = "@SP_INGAME_MISSIONFAILED_JAN";
					break;
			case MISSIONFAILED_LUKE:
				text = "@SP_INGAME_MISSIONFAILED_LUKE";
					break;
			case MISSIONFAILED_LANDO:
				text = "@SP_INGAME_MISSIONFAILED_LANDO";
					break;
			case MISSIONFAILED_R5D2:
				text = "@SP_INGAME_MISSIONFAILED_R5D2";
					break;
			case MISSIONFAILED_WARDEN:
				text = "@SP_INGAME_MISSIONFAILED_WARDEN";
					break;
			case MISSIONFAILED_PRISONERS:
				text = "@SP_INGAME_MISSIONFAILED_PRISONERS";
					break;
			case MISSIONFAILED_EMPLACEDGUNS:
				text = "@SP_INGAME_MISSIONFAILED_EMPLACEDGUNS";
					break;
			case MISSIONFAILED_LADYLUCK:
				text = "@SP_INGAME_MISSIONFAILED_LADYLUCK";
					break;
			case MISSIONFAILED_KYLECAPTURE:
				text = "@SP_INGAME_MISSIONFAILED_KYLECAPTURE";
					break;
			case MISSIONFAILED_TOOMANYALLIESDIED:
				text = "@SP_INGAME_MISSIONFAILED_TOOMANYALLIESDIED";
					break;

			case MISSIONFAILED_CHEWIE:
				text = "@SP_INGAME_MISSIONFAILED_CHEWIE";
					break;

			case MISSIONFAILED_KYLE:
				text = "@SP_INGAME_MISSIONFAILED_KYLE";
					break;

			case MISSIONFAILED_ROSH:
				text = "@SP_INGAME_MISSIONFAILED_ROSH";
					break;

			case MISSIONFAILED_WEDGE:
				text = "@SP_INGAME_MISSIONFAILED_WEDGE";
					break;

			case MISSIONFAILED_TURNED:
				text = "@SP_INGAME_MISSIONFAILED_TURNED";
					break;

			default:
				text = "@SP_INGAME_MISSIONFAILED_UNKNOWN";
					break;
		}

		gi.cvar_set("ui_missionfailed_text", text);
	}

	if ( G_OutcastInterface() )
	{
		CG_DrawOutcastMissionFailed();
	}
//	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontMedium, 1.2f);
//		cgi_R_Font_DrawString(320 - w/2, y+30, text, colorTable[CT_HUD_RED], cgs.media.qhFontMedium, -1, 1.2f);

//		cgi_SP_GetStringTextString( "SP_INGAME_RELOADMISSION", text, sizeof(text) );
//	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontSmall, 1.0f);
//		cgi_R_Font_DrawString(320 - w/2, 450, text, colorTable[CT_CYAN], cgs.media.qhFontSmall, -1, 1.0f);

}

// One statistic of CG_MissionCompletion: its name in gold, its value in white after it
static void CG_MissionCompletionLine( const int x, const int y, const char *label, const char *value )
{
	char text[1024]={0};

	cgi_SP_GetStringTextString( label, text, sizeof(text) );
	const int w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontSmall, 0.8f);
	cgi_R_Font_DrawString(x,   y, text,  colorTable[CT_LTGOLD1], cgs.media.qhFontSmall, -1, 0.8f);
	cgi_R_Font_DrawString(x+w, y, value, colorTable[CT_WHITE],   cgs.media.qhFontSmall, -1, 0.8f);
}

// A title above a column of statistics
static void CG_MissionCompletionHeading( const int x, const int y, const char *label )
{
	char text[1024]={0};

	cgi_SP_GetStringTextString( label, text, sizeof(text) );
	cgi_R_Font_DrawString(x, y, text, colorTable[CT_WHITE], cgs.media.qhFontSmall, -1, 0.8f);
}

/*
=================
CG_MissionCompletion

Jedi Outcast's mission status screen: the statistics of the level just finished, over the next
level's loading screen. Its level changes ask for it (target_level_change unless marked HIDEINFO, or
the SET_MISSION_STATUS_SCREEN script command, through cg_missionstatusscreen); the map command and
loading a game clear it. Jedi Academy shows its statistics in its mission menus instead, so only
Jedi Outcast's loading screen draws this (CG_DrawOutcastLoadingScreen). The player's session has
been carried over to the new level by then, so these are still the last level's numbers.
=================
*/
void CG_MissionCompletion(void)
{
	const gclient_t *client = g_entities[0].client;
	if ( !client )
	{
		return;
	}
	const missionStats_t &stats = client->sess.missionStats;

	char text[1024]={0};
	char of[1024]={0};
	int w,x,y;
	const int pad = 18;

	cgi_SP_GetStringTextString( "SP_INGAME_MISSIONCOMPLETION", text, sizeof(text) );
	w = cgi_R_Font_StrLenPixels(text, cgs.media.qhFontMedium, 1.2f);
	cgi_R_Font_DrawString(320 - w/2, 53, text, colorTable[CT_LTGOLD1], cgs.media.qhFontMedium, -1, 1.2f);

	x = 75;
	y = 86;
	cgi_SP_GetStringTextString( "SP_INGAME_SECRETAREAS_OF", of, sizeof(of) );
	CG_MissionCompletionLine( x, y, "SP_INGAME_SECRETAREAS", va("%d %s %d", stats.secretsFound, of, stats.totalSecrets) );

	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_ENEMIESKILLED", va("%d", stats.enemiesKilled) );

	// the weapon fired the most (none shown if nothing was)
	int wpn=0,i;
	int max_wpn = stats.weaponUsed[0];
	for (i = 1; i<WP_NUM_WEAPONS; i++)
	{
		if (stats.weaponUsed[i] > max_wpn)
		{
			max_wpn = stats.weaponUsed[i];
			wpn = i;
		}
	}
	if ( G_IsOutcastCampaign() )
	{// its story gives the player Jedi Academy's pistol and melee in place of its Bryar pistol and
	 // stun baton (G_Outcast_PlayerWeapon): name them as Jedi Outcast did
		if ( wpn == WP_BLASTER_PISTOL )
		{
			wpn = WP_BRYAR_PISTOL;
		}
		else if ( wpn == WP_MELEE )
		{
			wpn = WP_STUN_BATON;
		}
	}
	text[0] = '\0';
	const gitem_t *wItem = wpn ? FindItemForWeapon( (weapon_t)wpn ) : NULL;
	if ( wItem && wItem->classname )
	{
		cgi_SP_GetStringTextString( va("SP_INGAME_%s",wItem->classname ), text, sizeof( text ));
	}
	y += pad;
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_FAVORITEWEAPON", text );

	x = 334+70;
	y = 86;
	CG_MissionCompletionLine( x, y, "SP_INGAME_SHOTSFIRED", va("%d", stats.shotsFired) );

	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_HITS", va("%d", stats.hits) );

	y += pad;
	const float percent = stats.shotsFired? 100.0f * (float)stats.hits / stats.shotsFired : 0;
	CG_MissionCompletionLine( x, y, "SP_INGAME_ACCURACY", va("%.2f%%", percent) );

	if ( stats.weaponUsed[WP_SABER] <= 0 )
	{
		return; //don't have saber yet, so don't print any stats
	}
	// the Force powers Jedi Outcast has, under the right column
	y = 180;
	CG_MissionCompletionHeading( x, y, "SP_INGAME_FORCEUSE" );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_HEAL", va("%d", stats.forceUsed[FP_HEAL]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_SPEED", va("%d", stats.forceUsed[FP_SPEED]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_PULL", va("%d", stats.forceUsed[FP_PULL]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_PUSH", va("%d", stats.forceUsed[FP_PUSH]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_MINDTRICK", va("%d", stats.forceUsed[FP_TELEPATHY]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_GRIP", va("%d", stats.forceUsed[FP_GRIP]) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_LIGHTNING", va("%d", stats.forceUsed[FP_LIGHTNING]) );

	// the lightsaber, under the left column
	x = 140;
	y = 180;
	CG_MissionCompletionHeading( x, y, "SP_INGAME_LIGHTSABERUSE" );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_THROWN", va("%d", stats.saberThrownCnt) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_BLOCKS", va("%d", stats.saberBlocksCnt) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_LEGATTACKS", va("%d", stats.legAttacksCnt) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_ARMATTACKS", va("%d", stats.armAttacksCnt) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_BODYATTACKS", va("%d", stats.torsoAttacksCnt) );
	y += pad;
	CG_MissionCompletionLine( x, y, "SP_INGAME_OTHERATTACKS", va("%d", stats.otherAttacksCnt) );
}

/*
=================
CG_DrawScoreboard

Draw the normal in-game scoreboard
return value is bool to NOT draw centerstring
=================
*/
qboolean CG_DrawScoreboard( void )
{
	// don't draw anything if the menu is up
	if ( cg_paused.integer )
	{
		return qfalse;
	}

	// Character is either dead, or a script has brought up the screen
	if (((cg.predicted_player_state.pm_type == PM_DEAD) && (cg.missionStatusDeadTime < level.time))
		|| (cg.missionStatusShow))
	{
		CG_MissionFailed();
		return qtrue;
	}

	return qfalse;
}

void ScoreBoardReset(void)
{
}

//================================================================================

