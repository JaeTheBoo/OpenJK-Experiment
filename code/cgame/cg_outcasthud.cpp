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

// cg_outcasthud.cpp -- Jedi Outcast's HUD, drawn while its interface is on (ui_style "outcast", see
// G_OutcastInterface). Ported from Jedi Outcast's cgame (codeJK2/cgame/cg_draw.cpp, cg_weapons.cpp).
// Its HUD menus (ui/jk2hud.txt) only say where the parts go; they are drawn here, from its own art
// (gfx/hud/, which its interface reads from Jedi Outcast's game data).

#include "cg_headers.h"

#include "cg_media.h"
#include "../game/g_outcast.h"

static struct
{
	qhandle_t	leftFrame;			// the metal frames, over moving static
	qhandle_t	rightFrame;
	qhandle_t	innerLeft;			// the grey wire frames under the dials
	qhandle_t	innerRight;
	qhandle_t	armor1;				// outer and inner armor rings
	qhandle_t	armor2;
	qhandle_t	health;
	qhandle_t	healthTic;
	qhandle_t	saberStyleFast;
	qhandle_t	saberStyleMed;
	qhandle_t	saberStyleStrong;
	qhandle_t	weaponProngsOn;		// the prongs either side of the weapon, force power and item bars
	qhandle_t	weaponProngsOff;
	qhandle_t	forceProngsOn;
	qhandle_t	inventoryProngsOn;
	qhandle_t	atstHealthFrame;	// the emplaced gun's and the AT-ST's health bar
	qhandle_t	ladyLuckHealthFrame;	// the panel turrets'
} outcastHUD;

typedef struct
{
	int			x;
	int			y;
	int			width;
	int			height;
	const char	*file;
	qhandle_t	tic;
} outcastTic_t;

#define OUTCAST_TICS	14

// The force dial's tics, around the bottom of the right part, and the ammo dial's, around its top
static outcastTic_t outcastForceTics[OUTCAST_TICS] =
{
	{ 11, 41, 20, 10, "gfx/hud/force_tick1", NULL_HANDLE },		// Left Top
	{ 12, 45, 20, 10, "gfx/hud/force_tick2", NULL_HANDLE },
	{ 14, 49, 20, 10, "gfx/hud/force_tick3", NULL_HANDLE },
	{ 17, 52, 20, 10, "gfx/hud/force_tick4", NULL_HANDLE },
	{ 22, 55, 10, 10, "gfx/hud/force_tick5", NULL_HANDLE },
	{ 28, 57, 10, 20, "gfx/hud/force_tick6", NULL_HANDLE },
	{ 34, 59, 10, 10, "gfx/hud/force_tick7", NULL_HANDLE },		// Left bottom

	{ 46, 59, -10, 10, "gfx/hud/force_tick7", NULL_HANDLE },	// Right bottom
	{ 52, 57, -10, 20, "gfx/hud/force_tick6", NULL_HANDLE },
	{ 58, 55, -10, 10, "gfx/hud/force_tick5", NULL_HANDLE },
	{ 63, 52, -20, 10, "gfx/hud/force_tick4", NULL_HANDLE },
	{ 66, 49, -20, 10, "gfx/hud/force_tick3", NULL_HANDLE },
	{ 68, 45, -20, 10, "gfx/hud/force_tick2", NULL_HANDLE },
	{ 69, 41, -20, 10, "gfx/hud/force_tick1", NULL_HANDLE },	// Right top
};

static outcastTic_t outcastAmmoTics[OUTCAST_TICS] =
{
	{ 12, 34, 10, 10, "gfx/hud/ammo_tick7-l", NULL_HANDLE },	// Bottom
	{ 13, 28, 10, 10, "gfx/hud/ammo_tick6-l", NULL_HANDLE },
	{ 15, 23, 10, 10, "gfx/hud/ammo_tick5-l", NULL_HANDLE },
	{ 19, 19, 10, 10, "gfx/hud/ammo_tick4-l", NULL_HANDLE },
	{ 23, 15, 10, 10, "gfx/hud/ammo_tick3-l", NULL_HANDLE },
	{ 29, 12, 10, 10, "gfx/hud/ammo_tick2-l", NULL_HANDLE },
	{ 34, 11, 10, 10, "gfx/hud/ammo_tick1-l", NULL_HANDLE },

	{ 47, 11, -10, 10, "gfx/hud/ammo_tick1-r", NULL_HANDLE },
	{ 52, 12, -10, 10, "gfx/hud/ammo_tick2-r", NULL_HANDLE },
	{ 58, 15, -10, 10, "gfx/hud/ammo_tick3-r", NULL_HANDLE },
	{ 62, 19, -10, 10, "gfx/hud/ammo_tick4-r", NULL_HANDLE },
	{ 66, 23, -10, 10, "gfx/hud/ammo_tick5-r", NULL_HANDLE },
	{ 68, 28, -10, 10, "gfx/hud/ammo_tick6-r", NULL_HANDLE },
	{ 69, 34, -10, 10, "gfx/hud/ammo_tick7-r", NULL_HANDLE },
};

/*
================
CG_RegisterOutcastHUD

Loads Jedi Outcast's HUD art, when its interface is on (CG_RegisterGraphics).
================
*/
void CG_RegisterOutcastHUD( void )
{
	memset( &outcastHUD, 0, sizeof( outcastHUD ) );

	if ( !G_OutcastInterface() )
	{
		return;
	}

	outcastHUD.leftFrame			= cgi_R_RegisterShaderNoMip( "gfx/hud/static_test" );
	outcastHUD.rightFrame			= cgi_R_RegisterShaderNoMip( "gfx/hud/hudrightframe" );
	outcastHUD.innerLeft			= cgi_R_RegisterShaderNoMip( "gfx/hud/hudleft_innerframe" );
	outcastHUD.innerRight			= cgi_R_RegisterShaderNoMip( "gfx/hud/hudright_innerframe" );
	outcastHUD.armor1				= cgi_R_RegisterShaderNoMip( "gfx/hud/armor1" );
	outcastHUD.armor2				= cgi_R_RegisterShaderNoMip( "gfx/hud/armor2" );
	outcastHUD.health				= cgi_R_RegisterShaderNoMip( "gfx/hud/health" );
	outcastHUD.healthTic			= cgi_R_RegisterShaderNoMip( "gfx/hud/health_tic" );
	outcastHUD.saberStyleFast		= cgi_R_RegisterShader( "gfx/hud/saber_stylesFast" );
	outcastHUD.saberStyleMed		= cgi_R_RegisterShader( "gfx/hud/saber_stylesMed" );
	outcastHUD.saberStyleStrong		= cgi_R_RegisterShader( "gfx/hud/saber_stylesStrong" );
	outcastHUD.weaponProngsOn		= cgi_R_RegisterShaderNoMip( "gfx/hud/prong_on_w" );
	outcastHUD.weaponProngsOff		= cgi_R_RegisterShaderNoMip( "gfx/hud/prong_off" );
	outcastHUD.forceProngsOn		= cgi_R_RegisterShaderNoMip( "gfx/hud/prong_on_f" );
	outcastHUD.inventoryProngsOn	= cgi_R_RegisterShaderNoMip( "gfx/hud/prong_on_i" );
	outcastHUD.atstHealthFrame		= cgi_R_RegisterShaderNoMip( "gfx/hud/atst_health_frame" );
	outcastHUD.ladyLuckHealthFrame	= cgi_R_RegisterShaderNoMip( "gfx/hud/ladyluck_health_frame" );

	for ( int i = 0; i < OUTCAST_TICS; i++ )
	{
		outcastForceTics[i].tic = cgi_R_RegisterShaderNoMip( outcastForceTics[i].file );
		outcastAmmoTics[i].tic = cgi_R_RegisterShaderNoMip( outcastAmmoTics[i].file );
	}

	// Its datapad's frame
	cgs.media.dataPadFrame = cgi_R_RegisterShaderNoMip( "gfx/hud/datapad2" );
}

static void CG_DrawOutcastScaledPic( const int x, const int y, const int w, const int h, const qhandle_t shader, const int colorIndex, const float scale )
{
	vec4_t color;

	memcpy( color, colorTable[colorIndex], sizeof( vec4_t ) );
	color[0] *= scale;
	color[1] *= scale;
	color[2] *= scale;
	cgi_R_SetColor( color );
	CG_DrawPic( x, y, w, h, shader );
}

/*
================
CG_DrawOutcastMessageLit

The light that flashes while the datapad has news
================
*/
static void CG_DrawOutcastMessageLit( const int x, const int y )
{
	if ( cg.missionInfoFlashTime > cg.time )
	{
		if ( !( ( cg.time / 600 ) & 1 ) )
		{
			if ( !cg.messageLitActive )
			{
				cgi_S_StartSound( NULL, 0, CHAN_AUTO, cgs.media.messageLitSound );
				cg.messageLitActive = qtrue;
			}

			cgi_R_SetColor( colorTable[CT_HUD_RED] );
			CG_DrawPic( x + 33, y + 41, 16, 16, cgs.media.messageLitOn );
		}
		else
		{
			cg.messageLitActive = qfalse;
		}
	}

	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( x + 33, y + 41, 16, 16, cgs.media.messageLitOff );
}

/*
================
CG_DrawOutcastForcePower
================
*/
static void CG_DrawOutcastForcePower( const centity_t *cent, const int x, const int y )
{
	if ( !cent->gent || !cent->gent->client || !cent->gent->client->ps.forcePowersKnown )
	{
		return;
	}

	const playerState_t *ps = &cent->gent->client->ps;

	// Jedi Academy's warning when there is too little Force for a power: the tics flash red, with a sound
	qboolean flash = qfalse;
	if ( cg.forceHUDTotalFlashTime > cg.time )
	{
		flash = qtrue;
		if ( cg.forceHUDNextFlashTime < cg.time )
		{
			cg.forceHUDNextFlashTime = cg.time + 400;
			cgi_S_StartSound( NULL, 0, CHAN_AUTO, cgs.media.noforceSound );
			cg.forceHUDActive = cg.forceHUDActive ? qfalse : qtrue;
		}
	}
	else
	{
		cg.forceHUDNextFlashTime = 0;
		cg.forceHUDActive = qtrue;
	}

	const float inc = (float)ps->forcePowerMax / OUTCAST_TICS;
	float value = ps->forcePower;
	float extra = 0;
	if ( value > ps->forcePowerMax )
	{//supercharged with force
		extra = value - ps->forcePowerMax;
		value = ps->forcePowerMax;
	}

	const int ticColor = flash ? CT_RED : CT_LTGREY;
	for ( int i = OUTCAST_TICS - 1; i >= 0; i-- )
	{
		float percent;
		int colorIndex;

		if ( extra )
		{//supercharged
			colorIndex = CT_WHITE;
			percent = 0.75f + ( sin( cg.time * 0.005f ) * ( ( extra / ps->forcePowerMax ) * 0.25f ) );
		}
		else if ( value <= 0 )
		{
			colorIndex = CT_BLACK;
			percent = 1.0f;
		}
		else if ( value < inc )	// partial tic
		{
			colorIndex = ticColor;
			percent = value / inc;
		}
		else
		{
			colorIndex = ticColor;
			percent = 1.0f;
		}

		CG_DrawOutcastScaledPic( x + outcastForceTics[i].x, y + outcastForceTics[i].y,
			outcastForceTics[i].width, outcastForceTics[i].height, outcastForceTics[i].tic, colorIndex, percent );

		value -= inc;
	}
}

/*
================
CG_DrawOutcastAmmo

The ammo dial, or the saber's style
================
*/
static void CG_DrawOutcastAmmo( const centity_t *cent, const int x, const int y )
{
	const playerState_t *ps = &cg.snap->ps;
	const int weapon = cent->currentState.weapon;

	if ( !weapon || weapon == WP_STUN_BATON )
	{
		return;
	}

	if ( weapon == WP_SABER )
	{
		if ( !cent->gent )
		{
			return;
		}

		if ( !cg.saberAnimLevelPending && cent->gent->client )
		{//uninitialized after a loadgame, cheat across and get it
			cg.saberAnimLevelPending = cent->gent->client->ps.saberAnimLevel;
		}

		// Jedi Academy's styles go with the one of the three they are closest to (CG_DrawSaberStyle)
		qhandle_t style;
		if ( cg.saberAnimLevelPending == SS_FAST || cg.saberAnimLevelPending == SS_TAVION )
		{
			style = outcastHUD.saberStyleFast;
		}
		else if ( cg.saberAnimLevelPending == SS_MEDIUM || cg.saberAnimLevelPending == SS_DUAL || cg.saberAnimLevelPending == SS_STAFF )
		{
			style = outcastHUD.saberStyleMed;
		}
		else
		{
			style = outcastHUD.saberStyleStrong;
		}

		cgi_R_SetColor( colorTable[CT_WHITE] );
		CG_DrawPic( x, y, 80, 40, style );
		return;
	}

	const int ammoIndex = weaponData[weapon].ammoIndex;
	float value = ps->ammo[ammoIndex];

	if ( value < 0 )	// No ammo
	{
		return;
	}

	if ( cg.oldammo < value )
	{
		cg.oldAmmoTime = cg.time + 200;
	}
	cg.oldammo = value;

	int numColor;
	if ( cg.predicted_player_state.weaponstate == WEAPON_FIRING && cg.predicted_player_state.weaponTime > 100 )
	{// Firing or reloading
		numColor = CT_LTGREY;
	}
	else if ( value > 0 )
	{
		numColor = ( cg.oldAmmoTime > cg.time ) ? CT_YELLOW : CT_HUD_ORANGE;
	}
	else
	{
		numColor = CT_RED;
	}

	cgi_R_SetColor( colorTable[numColor] );
	CG_DrawNumField( x + 29, y + 26, 3, (int)value, 6, 12, NUM_FONT_SMALL, qfalse );

	const float inc = (float)ammoData[ammoIndex].max / OUTCAST_TICS;
	for ( int i = OUTCAST_TICS - 1; i >= 0; i-- )
	{
		int colorIndex = CT_WHITE;
		float percent = 1.0f;

		if ( value <= 0 )
		{
			colorIndex = CT_BLACK;
		}
		else if ( value < inc )	// partial tic
		{
			percent = value / inc;
		}

		CG_DrawOutcastScaledPic( x + outcastAmmoTics[i].x, y + outcastAmmoTics[i].y,
			outcastAmmoTics[i].width, outcastAmmoTics[i].height, outcastAmmoTics[i].tic, colorIndex, percent );

		value -= inc;
	}
}

/*
================
CG_DrawOutcastHealth
================
*/
static void CG_DrawOutcastHealth( const int x, const int y )
{
	const playerState_t *ps = &cg.snap->ps;
	const float healthPercent = ps->stats[STAT_MAX_HEALTH] ? (float)ps->stats[STAT_HEALTH] / ps->stats[STAT_MAX_HEALTH] : 0.0f;

	CG_DrawOutcastScaledPic( x, y, 80, 80, outcastHUD.health, CT_HUD_RED, healthPercent );

	// Draw the ticks
	if ( cg.HUDHealthFlag )
	{
		cgi_R_SetColor( colorTable[CT_HUD_RED] );
		CG_DrawPic( x, y, 80, 80, outcastHUD.healthTic );
	}

	cgi_R_SetColor( colorTable[CT_HUD_RED] );
	CG_DrawNumField( x + 16, y + 40, 3, ps->stats[STAT_HEALTH], 6, 12, NUM_FONT_SMALL, qtrue );
}

/*
================
CG_DrawOutcastArmor

Two rings: the outer one is the upper half of the armor, the inner one the lower half.
================
*/
static void CG_DrawOutcastArmor( const int x, const int y )
{
	const playerState_t *ps = &cg.snap->ps;
	const int halfMax = ps->stats[STAT_MAX_HEALTH] / 2;
	float armorPercent = 0.0f;

	if ( halfMax > 0 )
	{
		armorPercent = (float)( ps->stats[STAT_ARMOR] - halfMax ) / halfMax;
		if ( armorPercent < 0 )
		{
			armorPercent = 0;
		}
	}
	CG_DrawOutcastScaledPic( x, y, 80, 80, outcastHUD.armor1, CT_HUD_GREEN, armorPercent );

	if ( armorPercent > 0 )
	{
		armorPercent = 1;
	}
	else if ( halfMax > 0 )
	{
		armorPercent = (float)ps->stats[STAT_ARMOR] / halfMax;
	}
	CG_DrawOutcastScaledPic( x, y, 80, 80, outcastHUD.armor2, CT_HUD_GREEN, armorPercent );

	cgi_R_SetColor( colorTable[CT_HUD_GREEN] );
	CG_DrawNumField( x + 16 + 14, y + 40 + 14, 3, ps->stats[STAT_ARMOR], 6, 12, NUM_FONT_SMALL, qfalse );
}

/*
================
CG_DrawOutcastHUDLeft / CG_DrawOutcastHUDRight

The two parts of the HUD at the bottom corners: health and armor on the left, force and ammo on the
right. The datapad shows them without their metal frames.
================
*/
static void CG_DrawOutcastHUDLeft( const int x, const int y, const qboolean metalFrame )
{
	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( x, y, 80, 80, outcastHUD.innerLeft );

	CG_DrawOutcastArmor( x, y );
	CG_DrawOutcastHealth( x, y );

	if ( metalFrame )
	{
		cgi_R_SetColor( colorTable[CT_WHITE] );
		CG_DrawPic( x, y, 80, 80, outcastHUD.leftFrame );
	}
}

static void CG_DrawOutcastHUDRight( const centity_t *cent, const int x, const int y, const qboolean metalFrame )
{
	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( x, y, 80, 80, outcastHUD.innerRight );

	CG_DrawOutcastForcePower( cent, x, y );
	CG_DrawOutcastAmmo( cent, x, y );
	CG_DrawOutcastMessageLit( x, y );

	if ( metalFrame )
	{
		cgi_R_SetColor( colorTable[CT_WHITE] );
		CG_DrawPic( x, y, 80, 80, outcastHUD.rightFrame );
	}
}

/*
================
CG_DrawOutcastHUD

The player's HUD (CG_DrawHUD)
================
*/
void CG_DrawOutcastHUD( centity_t *cent )
{
	int x, y, w, h;

	if ( cgi_UI_GetMenuInfo( "lefthud", &x, &y, &w, &h ) )
	{
		cgi_R_Set2DAnchor( ANCHOR_LEFT );	// stuck to the left edge of the screen

		cgi_UI_Menu_Paint( cgi_UI_GetMenuByName( "lefthud" ), qtrue );

		// Draw armor & health values
		if ( cg_drawStatus.integer == 2 )
		{
			CG_DrawSmallStringColor( x + 5, y - 60, va( "Armor:%d", cg.snap->ps.stats[STAT_ARMOR] ), colorTable[CT_HUD_GREEN] );
			CG_DrawSmallStringColor( x + 5, y - 40, va( "Health:%d", cg.snap->ps.stats[STAT_HEALTH] ), colorTable[CT_HUD_GREEN] );
		}

		CG_DrawOutcastHUDLeft( x, y, qtrue );

		cgi_R_Set2DAnchor( ANCHOR_CENTRE );
	}

	if ( cgi_UI_GetMenuInfo( "righthud", &x, &y, &w, &h ) )
	{
		cgi_R_Set2DAnchor( ANCHOR_RIGHT );	// stuck to the right edge of the screen

		cgi_UI_Menu_Paint( cgi_UI_GetMenuByName( "righthud" ), qtrue );

		// Draw ammo & force values
		if ( cg_drawStatus.integer == 2 && cent->gent && cent->gent->client )
		{
			if ( cent->currentState.weapon != WP_SABER && cent->currentState.weapon != WP_STUN_BATON )
			{
				const int value = cg.snap->ps.ammo[weaponData[cent->currentState.weapon].ammoIndex];
				CG_DrawSmallStringColor( x, y - 60, va( "Ammo:%d", value ), colorTable[CT_HUD_GREEN] );
			}
			CG_DrawSmallStringColor( x, y - 40, va( "Force:%d", cent->gent->client->ps.forcePower ), colorTable[CT_HUD_GREEN] );
		}

		CG_DrawOutcastHUDRight( cent, x, y, qtrue );

		cgi_R_Set2DAnchor( ANCHOR_CENTRE );
	}
}

/*
================
CG_DrawOutcastDataPadHUD

The HUD's dials on the datapad's page of them (CG_DrawDataPadHUD)
================
*/
void CG_DrawOutcastDataPadHUD( centity_t *cent )
{
	CG_DrawOutcastHUDLeft( 34, 286, qfalse );
	CG_DrawOutcastHUDRight( cent, 526, 286, qfalse );

	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( 0, 0, 640, 480, cgs.media.dataPadFrame );
}

/*
================
CG_DrawOutcastCustomHud

The health bar of what the player is using in place of themselves: an emplaced gun, an AT-ST or a
panel turret. Returns whether it drew one (and so the player's HUD is left out).
================
*/
static void CG_DrawOutcastHealthBar( const float health, const float red, const float green, const float blue, const qhandle_t frame )
{
	vec4_t color;

	color[0] = color[1] = color[2] = 0.0f;
	color[3] = 0.3f;
	cgi_R_SetColor( color );
	CG_DrawPic( 14, 480 - 50, 94, 32, cgs.media.whiteShader );

	color[0] = red;
	color[1] = green;
	color[2] = blue;
	color[3] = 0.5f;
	cgi_R_SetColor( color );
	CG_DrawPic( 18, 480 - 41, 87 * health, 19, cgs.media.whiteShader );

	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( 2, 480 - 64, 128, 64, frame );
}

qboolean CG_DrawOutcastCustomHud( const centity_t *cent )
{
	float health = 0;

	cgi_R_Set2DAnchor( ANCHOR_LEFT );	// in the corner of the HUD's left part

	if ( ( cent->currentState.eFlags & EF_LOCKED_TO_WEAPON ) )
	{
		if ( cent->gent && cent->gent->owner )
		{
			if ( ( cent->gent->owner->flags & FL_GODMODE ) )
			{// chair is in godmode, so render the health of the player instead
				health = cent->gent->health / (float)cent->gent->max_health;
			}
			else
			{// render the chair health
				health = cent->gent->owner->health / (float)cent->gent->owner->max_health;
			}
		}

		CG_DrawOutcastHealthBar( health, 1.0f, 0.0f, 0.0f, outcastHUD.atstHealthFrame );
	}
	else if ( ( cent->currentState.eFlags & EF_IN_ATST ) )
	{
		const gentity_t *atst = &g_entities[cg.snap->ps.viewEntity];

		// we just calc the display value from the sum of health and armor
		if ( atst->client )
		{
			// (the max armor comes from the atst_drivable it was, when it can be found)
			const int maxArmor = atst->activator ? atst->activator->max_health : 800;
			health = ( atst->health + atst->client->ps.stats[STAT_ARMOR] ) / (float)( atst->max_health + maxArmor );
		}

		CG_DrawOutcastHealthBar( health, 0.0f, 0.25f, 1.0f, outcastHUD.atstHealthFrame );	// blue-green
	}
	else if ( cg.snap->ps.viewEntity && ( g_entities[cg.snap->ps.viewEntity].dflags & DAMAGE_CUSTOM_HUD ) )
	{// a misc_panel_turret
		const gentity_t *turret = &g_entities[cg.snap->ps.viewEntity];

		if ( turret->max_health )
		{
			health = turret->health / (float)turret->max_health;
		}

		CG_DrawOutcastHealthBar( health, 0.0f, 1.0f, 0.0f, outcastHUD.ladyLuckHealthFrame );
	}
	else
	{
		cgi_R_Set2DAnchor( ANCHOR_CENTRE );
		return qfalse;
	}

	cgi_R_Set2DAnchor( ANCHOR_CENTRE );
	return qtrue;
}

/*
===================
CG_DrawOutcastIconBackground

The bar behind the weapon, force power and item icons while one is being chosen, which opens and
closes, and the prongs at its ends (always shown).
===================
*/
void CG_DrawOutcastIconBackground( void )
{
	int x2, y2, w2, h2;
	int xAdd;
	qhandle_t background, prongs;

	if ( cg.zoomMode != 0 || !cg_drawHUD.integer || cg_hudFiles.integer )
	{
		return;
	}

	if ( cg.snap->ps.viewEntity > 0 && cg.snap->ps.viewEntity < ENTITYNUM_WORLD )
	{
		return;
	}

	if ( !cgi_UI_GetMenuInfo( "iconbackground", &x2, &y2, &w2, &h2 ) )
	{
		return;
	}

	const int prongLeftX = x2 + 37;
	const int prongRightX = x2 + 544;

	if ( ( cg.inventorySelectTime + WEAPON_SELECT_TIME ) > cg.time || cgs.media.currentBackground == ICON_INVENTORY )
	{
		background = cgs.media.inventoryIconBackground;
	}
	else if ( ( cg.weaponSelectTime + WEAPON_SELECT_TIME ) > cg.time || cgs.media.currentBackground == ICON_WEAPONS )
	{
		background = cgs.media.weaponIconBackground;
	}
	else
	{
		background = cgs.media.forceIconBackground;
	}

	if ( ( cg.iconSelectTime + WEAPON_SELECT_TIME ) < cg.time )	// Time is up for the HUD to display
	{
		if ( cg.iconHUDActive )		// The time is up, but we still need to move the prongs back to their original position
		{
			cg.iconHUDPercent = 1.0f - ( cg.time - ( cg.iconSelectTime + WEAPON_SELECT_TIME ) ) / 130.0f;

			if ( cg.iconHUDPercent < 0 )
			{
				cg.iconHUDActive = qfalse;
				cg.iconHUDPercent = 0;
			}

			xAdd = (int)( 8 * cg.iconHUDPercent );

			const int height = (int)( 60.0f * cg.iconHUDPercent );
			cgi_R_SetColor( colorTable[CT_WHITE] );
			CG_DrawPic( x2 + 60, y2 + 30, 460, -height, background );		// Top half
			CG_DrawPic( x2 + 60, y2 + 30 - 2, 460, height, background );	// Bottom half
		}
		else
		{
			xAdd = 0;
		}

		prongs = outcastHUD.weaponProngsOff;
	}
	else
	{
		if ( !cg.iconHUDActive )
		{
			cg.iconHUDPercent = ( cg.time - cg.iconSelectTime ) / 130.0f;

			// Calc how far into opening sequence we are
			if ( cg.iconHUDPercent > 1 )
			{
				cg.iconHUDActive = qtrue;
				cg.iconHUDPercent = 1;
			}
			else if ( cg.iconHUDPercent < 0 )
			{
				cg.iconHUDPercent = 0;
			}
		}
		else
		{
			cg.iconHUDPercent = 1;
		}

		const int height = (int)( 60.0f * cg.iconHUDPercent );
		cgi_R_SetColor( colorTable[CT_WHITE] );
		CG_DrawPic( x2 + 60, y2 + 30, 460, -height, background );		// Top half
		CG_DrawPic( x2 + 60, y2 + 30 - 2, 460, height, background );	// Bottom half

		// And now for the prongs
		if ( ( cg.inventorySelectTime + WEAPON_SELECT_TIME ) > cg.time )
		{
			cgs.media.currentBackground = ICON_INVENTORY;
			prongs = outcastHUD.inventoryProngsOn;
		}
		else if ( ( cg.weaponSelectTime + WEAPON_SELECT_TIME ) > cg.time )
		{
			cgs.media.currentBackground = ICON_WEAPONS;
			prongs = outcastHUD.weaponProngsOn;
		}
		else
		{
			cgs.media.currentBackground = ICON_FORCE;
			prongs = outcastHUD.forceProngsOn;
		}

		xAdd = (int)( 8 * cg.iconHUDPercent );
	}

	// The side prongs stand next to the left and right parts of the HUD, so they go with them to the
	// edges of the screen
	cgi_R_SetColor( colorTable[CT_WHITE] );
	cgi_R_Set2DAnchor( ANCHOR_LEFT );
	CG_DrawPic( prongLeftX + xAdd, y2 - 10, 40, 80, prongs );
	cgi_R_Set2DAnchor( ANCHOR_RIGHT );
	CG_DrawPic( prongRightX - xAdd, y2 - 10, -40, 80, prongs );
	cgi_R_Set2DAnchor( ANCHOR_CENTRE );
}

/*
===================
CG_DrawOutcastDataPadIconBackground

The same bar and prongs behind the datapad's rows of weapons, force powers and items
===================
*/
void CG_DrawOutcastDataPadIconBackground( const int backgroundType )
{
	const int x2 = 0;
	const int y2 = 295;
	const int prongLeftX = x2 + 97;
	const int prongRightX = x2 + 544;
	const int xAdd = 8;
	const int height = 60;
	qhandle_t background, prongs;

	if ( backgroundType == ICON_INVENTORY )
	{
		background = cgs.media.inventoryIconBackground;
		prongs = outcastHUD.inventoryProngsOn;
	}
	else if ( backgroundType == ICON_WEAPONS )
	{
		background = cgs.media.weaponIconBackground;
		prongs = outcastHUD.weaponProngsOn;
	}
	else
	{
		background = cgs.media.forceIconBackground;
		prongs = outcastHUD.forceProngsOn;
	}

	cgi_R_SetColor( colorTable[CT_WHITE] );
	CG_DrawPic( x2 + 110, y2 + 30, 410, -height, background );		// Top half
	CG_DrawPic( x2 + 110, y2 + 30 - 2, 410, height, background );	// Bottom half

	CG_DrawPic( prongLeftX + xAdd, y2 - 10, 40, 80, prongs );
	CG_DrawPic( prongRightX - xAdd, y2 - 10, -40, 80, prongs );
}

/*
===================
CG_DrawOutcastDataPadUpdated

The note under the middle of the screen while the datapad has news, and what the news is
===================
*/
void CG_DrawOutcastDataPadUpdated( void )
{
	char text[1024] = { 0 };
	int y = ( SCREEN_HEIGHT / 2 ) + 80;

	const char *const lines[] =
	{
		"INGAME_DATAPAD_UPDATED",
		cg_updatedDataPadForcePower1.integer ? "INGAME_NEW_FORCE_POWER_INFO" : NULL,
		cg_updatedDataPadObjective.integer ? "INGAME_NEW_OBJECTIVE_INFO" : NULL,
	};

	for ( size_t i = 0; i < ARRAY_LEN( lines ); i++ )
	{
		if ( !lines[i] )
		{
			continue;
		}

		cgi_SP_GetStringTextString( lines[i], text, sizeof( text ) );
		const int w = cgi_R_Font_StrLenPixels( text, cgs.media.qhFontMedium, 1.0f );
		cgi_R_Font_DrawString( ( SCREEN_WIDTH / 2 ) - ( w / 2 ), y, text, colorTable[CT_LTRED1], cgs.media.qhFontMedium, -1, 1.0f );
		y += 25;
	}
}
