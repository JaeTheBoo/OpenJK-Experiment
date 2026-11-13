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
#include "../game/g_outcast.h"


// For printing objectives
static const short objectiveStartingYpos = 75;		// Y starting position for objective text
static const short objectiveStartingXpos = 60;		// X starting position for objective text
static const int objectiveTextBoxWidth = 500;		// Width (in pixels) of text box
static const int objectiveTextBoxHeight = 300;		// Height (in pixels) of text box

const char *showLoadPowersName[] =
{
	"SP_INGAME_HEAL2",
	"SP_INGAME_JUMP2",
	"SP_INGAME_SPEED2",
	"SP_INGAME_PUSH2",
	"SP_INGAME_PULL2",
	"SP_INGAME_MINDTRICK2",
	"SP_INGAME_GRIP2",
	"SP_INGAME_LIGHTNING2",
	"SP_INGAME_SABER_THROW2",
	"SP_INGAME_SABER_OFFENSE2",
	"SP_INGAME_SABER_DEFENSE2",
	NULL,
};

#define MAX_OBJ_GRAPHICS 4
#define OBJ_GRAPHIC_SIZE 240
int obj_graphics[MAX_OBJ_GRAPHICS];

qboolean CG_ForcePower_Valid(int forceKnownBits, int index);

// How far the pictures under some of Jedi Outcast's objectives (CG_DrawOutcastObjectiveGraphic) have
// moved the lines after them down
static int objectiveGraphicsShift;

// Where objective text starts: Jedi Outcast's menus have it further right and lower down
static int CG_ObjectiveXpos( void )
{
	return G_OutcastInterface() ? 108 : objectiveStartingXpos;
}

static int CG_ObjectiveYpos( void )
{
	return ( G_OutcastInterface() ? 79 : objectiveStartingYpos ) + objectiveGraphicsShift;
}

/*
====================
CG_DrawOutcastObjectiveGraphic

Jedi Outcast's story shows the security codes its puzzles need beneath their objectives: the door
code on kejim_post (red, green and blue parts) and the communications code on doom_comm.
====================
*/
static void CG_DrawOutcastObjectiveGraphic( const char *objectiveName, const int y )
{
	const char *shader;
	int width = 32;

	if ( !Q_stricmp( objectiveName, "DOOM_COMM_OBJ4" ) )
	{
		shader = "textures/system/securitycode";
		width = 128;
	}
	else if ( !Q_stricmp( objectiveName, "KEJIM_POST_OBJ3" ) )
	{
		shader = "textures/system/securitycode_red";
	}
	else if ( !Q_stricmp( objectiveName, "KEJIM_POST_OBJ4" ) )
	{
		shader = "textures/system/securitycode_green";
	}
	else if ( !Q_stricmp( objectiveName, "KEJIM_POST_OBJ5" ) )
	{
		shader = "textures/system/securitycode_blue";
	}
	else
	{
		return;
	}

	cgi_R_SetColor( NULL );
	CG_DrawPic( 320 - (width/2), y+8, width, 32, cgi_R_RegisterShaderNoMip( shader ) );
	objectiveGraphicsShift += 32 + 4;
}

/*
====================
ObjectivePrint_Line

Print a single mission objective
====================
*/
static void ObjectivePrint_Line(const int color, const int objectIndex, int &missionYcnt)
{
	char *str,*strBegin;
	int y,pixelLen,charLen,i;
	const int maxHoldText = 1024;
	char holdText[maxHoldText];
	char finalText[2048];
	qhandle_t	graphic;

	int iYPixelsPerLine = cgi_R_Font_HeightPixels(cgs.media.qhFontMedium, 1.0f);

	// Jedi Outcast's campaign has its own objectives in these slots (g_outcast.h)
	const qboolean outcastObjective = G_IsOutcastCampaign();
	const char *objectiveName = outcastObjective ? G_Outcast_ObjectiveName( objectIndex ) : objectiveTable[objectIndex].name;
	cgi_SP_GetStringTextString( va("OBJECTIVES_%s",objectiveName) , finalText, sizeof(finalText) );

	// A hack to be able to count prisoners
	if (objectIndex==T2_RANCOR_OBJ5 && !outcastObjective)
	{
		char value[64];
		int	currTotal, minTotal;

		gi.Cvar_VariableStringBuffer("ui_prisonerobj_currtotal",value,sizeof(value));
		currTotal = atoi(value);
		gi.Cvar_VariableStringBuffer("ui_prisonerobj_maxtotal",value,sizeof(value));
		minTotal = atoi(value);

		Q_strncpyz(finalText, va(finalText,currTotal,minTotal), sizeof(finalText));
	}

	pixelLen = cgi_R_Font_StrLenPixels(finalText, cgs.media.qhFontMedium, 1.0f);

	str = finalText;

	if (cgi_Language_IsAsian())
	{
		// this is execrable, and should NOT have had to've been done now, but...
		//
		extern const char *CG_DisplayBoxedText(	int iBoxX, int iBoxY, int iBoxWidth, int iBoxHeight,
												const char *psText, int iFontHandle, float fScale,
												const vec4_t v4Color);
		extern int giLinesOutput;
		extern float gfAdvanceHack;

		gfAdvanceHack = 1.0f;	// override internal vertical advance
		y = CG_ObjectiveYpos() + (iYPixelsPerLine * missionYcnt);

		// Advance line if a graphic has printed
		for (i=0;i<MAX_OBJ_GRAPHICS;i++)
		{
			if (obj_graphics[i])
			{
				y += OBJ_GRAPHIC_SIZE + 4;
			}
		}

		CG_DisplayBoxedText(
			CG_ObjectiveXpos(),
			y,
			objectiveTextBoxWidth,
			objectiveTextBoxHeight,
			finalText,	// int iBoxX, int iBoxY, int iBoxWidth, int iBoxHeight, const char *psText
			cgs.media.qhFontMedium,		// int iFontHandle,
			1.0f,						// float fScale,
			colorTable[color]			// const vec4_t v4Color
			);

		gfAdvanceHack = 0.0f;	// restore
		missionYcnt += giLinesOutput;
	}
	else
	{
		// western...
		//
		if (pixelLen < objectiveTextBoxWidth)	// One shot - small enough to print entirely on one line
		{
			y =CG_ObjectiveYpos() + (iYPixelsPerLine * (missionYcnt));

			cgi_R_Font_DrawString (
				CG_ObjectiveXpos(),
				y,
				str,
				colorTable[color],
				cgs.media.qhFontMedium,
				-1,
				1.0f);

			++missionYcnt;
		}
		// Text is too long, break into lines.
		else
		{
			char holdText2[2];
			pixelLen = 0;
			charLen = 0;
			holdText2[1] = '\0';
			strBegin = str;

			while( *str )
			{
				holdText2[0] = *str;
				pixelLen += cgi_R_Font_StrLenPixels(holdText2, cgs.media.qhFontMedium, 1.0f);

				pixelLen += 2; // For kerning
				++charLen;

				if (pixelLen > objectiveTextBoxWidth )
				{	//Reached max length of this line
					//step back until we find a space
					while ((charLen>10) && (*str != ' ' ))
					{
						--str;
						--charLen;
					}

					if (*str==' ')
					{
						++str;	// To get past space
					}

					assert( charLen<maxHoldText );	// Too big?

					Q_strncpyz( holdText, strBegin, charLen);
					holdText[charLen] = '\0';
					strBegin = str;
					pixelLen = 0;
					charLen = 1;

					y = CG_ObjectiveYpos() + (iYPixelsPerLine * missionYcnt);

					CG_DrawProportionalString(
						CG_ObjectiveXpos(),
						y,
						holdText,
						CG_SMALLFONT,
						colorTable[color] );

					++missionYcnt;
				}
				else if (*(str+1) == '\0')
				{
					++charLen;

					assert( charLen<maxHoldText );	// Too big?

					y = CG_ObjectiveYpos() + (iYPixelsPerLine * missionYcnt);

					Q_strncpyz( holdText, strBegin, charLen);
					CG_DrawProportionalString(
						CG_ObjectiveXpos(),
						y, holdText,
						CG_SMALLFONT,
						colorTable[color] );

					++missionYcnt;
					break;
				}
				++str;


			}
		}
	}

	if (objectIndex == T3_BOUNTY_OBJ1 && !outcastObjective)
	{
		y =CG_ObjectiveYpos() + (iYPixelsPerLine * missionYcnt);
		if (obj_graphics[1])
		{
			y += OBJ_GRAPHIC_SIZE + 4;
		}
		if (obj_graphics[2])
		{
			y += OBJ_GRAPHIC_SIZE + 4;
		}
		graphic = cgi_R_RegisterShaderNoMip("textures/system/viewscreen1");
		CG_DrawPic( 355, 50, OBJ_GRAPHIC_SIZE, OBJ_GRAPHIC_SIZE, graphic );
		obj_graphics[3] = qtrue;
	}

	if ( outcastObjective )
	{
		CG_DrawOutcastObjectiveGraphic( objectiveName, CG_ObjectiveYpos() + (iYPixelsPerLine * missionYcnt) );
	}
}

/*
====================
CG_DrawDataPadObjectives

Draw routine for the objective info screen of the data pad.
====================
*/
void CG_DrawDataPadObjectives(const centity_t *cent )
{
	int		i,totalY;
	int		iYPixelsPerLine = cgi_R_Font_HeightPixels(cgs.media.qhFontMedium, 1.0f);

	objectiveGraphicsShift = 0;

	// (Jedi Outcast's menus: white title, blue objectives, their circles hung from the middle of the line)
	const qboolean outcastLayout = G_OutcastInterface();
	const short titleXPos = CG_ObjectiveXpos() - (outcastLayout ? 12 : 22);		// X starting position for title text
	const short titleYPos = CG_ObjectiveYpos() - 23;		// Y starting position for title text
	const short graphic_size = 16;							// Size (width and height) of graphic used to show status of objective
	const short graphicXpos = CG_ObjectiveXpos() - graphic_size - (outcastLayout ? 4 : 8);	// Amount of X to backup from text starting position
	const short graphicYOffset = outcastLayout ? 0 : (iYPixelsPerLine - graphic_size)/2;	// Amount of Y to raise graphic so it's in the center of the text line

	missionInfo_Updated = qfalse;		// This will stop the text from flashing
	cg.missionInfoFlashTime = 0;

	// zero out objective graphics
	for (i=0;i<MAX_OBJ_GRAPHICS;i++)
	{
		obj_graphics[i] = qfalse;
	}

	// Title Text at the top
	char text[1024]={0};
	cgi_SP_GetStringTextString( "SP_INGAME_OBJECTIVES", text, sizeof(text) );
	cgi_R_Font_DrawString (titleXPos, titleYPos, text, colorTable[outcastLayout ? CT_WHITE : CT_TITLE], cgs.media.qhFontMedium, -1, 1.0f);

	int missionYcnt = 0;

	// Print all active objectives
	for (i=0;i<MAX_OBJECTIVES;i++)
	{
		// Is there an objective to see?
		if (cent->gent->client->sess.mission_objectives[i].display)
		{
			// Calculate the Y position
			totalY = CG_ObjectiveYpos() + (iYPixelsPerLine * (missionYcnt))+(iYPixelsPerLine/2);

			//	Draw graphics that show if mission has been accomplished or not
			cgi_R_SetColor(colorTable[CT_BLUE3]);
			CG_DrawPic( (graphicXpos),   (totalY-graphicYOffset),   graphic_size,  graphic_size, cgs.media.messageObjCircle);	// Circle in front
			if (cent->gent->client->sess.mission_objectives[i].status == OBJECTIVE_STAT_SUCCEEDED)
			{
				CG_DrawPic( (graphicXpos),   (totalY-graphicYOffset),   graphic_size,  graphic_size, cgs.media.messageLitOn);	// Center Dot
			}

			// Print current objective text
			ObjectivePrint_Line(outcastLayout ? CT_BLUE3 : CT_WHITE, i, missionYcnt );
		}
	}

	// No mission text?
	if (!missionYcnt && outcastLayout)
	{
		cgi_SP_GetStringTextString( "SP_INGAME_OBJNONE", text, sizeof(text) );
		cgi_R_Font_DrawString( CG_ObjectiveXpos(), CG_ObjectiveYpos(), text, colorTable[CT_LTBLUE1], cgs.media.qhFontMedium, -1, 1.0f );
	}
	else if (!missionYcnt)
	{
		// Set the message a quarter of the way down and in the center of the text box
		int messageYPosition = CG_ObjectiveYpos() + (objectiveTextBoxHeight / 4);

		cgi_SP_GetStringTextString( "SP_INGAME_OBJNONE", text, sizeof(text) );
		int messageXPosition = CG_ObjectiveXpos() + (objectiveTextBoxWidth/2) -  (cgi_R_Font_StrLenPixels(text, cgs.media.qhFontMedium, 1.0f) /2);

		cgi_R_Font_DrawString (
			messageXPosition,
			messageYPosition,
			text,
			colorTable[CT_WHITE],
			cgs.media.qhFontMedium,
			-1,
			1.0f);
	}
}

/*

//-------------------------------------------------------
static void CG_DrawForceCount( const int force, int x, float *y, const int pad,qboolean *hasForcePowers )
{
	char	s[MAX_STRING_CHARS];
	int		val, textColor;
	char	text[1024]={0};

	gi.Cvar_VariableStringBuffer( va("playerfplvl%d", force ),s, sizeof(s) );

	sscanf( s, "%d",&val );

	if ((val<1) || (val> NUM_FORCE_POWERS))
	{
		return;
	}

	textColor = CT_ICON_BLUE;

	// Draw title
	cgi_SP_GetStringTextString( showLoadPowersName[force], text, sizeof(text) );
	CG_DrawProportionalString( x, *y, text, CG_BIGFONT, colorTable[textColor] );


	// Draw icons
	cgi_R_SetColor( colorTable[CT_WHITE]);
	const int iconSize = 30;
	if ( val >= 0 )
	{
		x -= 10;	// Back up from title a little

		for ( int i = 0; i < val; i++ )
		{
			CG_DrawPic( x - iconSize - i * (iconSize + 10) , *y, iconSize, iconSize, force_icons[force] );
		}
	}

	*y += pad;

	*hasForcePowers = qtrue;
}


/ *
====================
CG_LoadScreen_PersonalInfo
====================
*/
/*
static void CG_LoadScreen_PersonalInfo(void)
{
	float	x, y;
	int		pad = 25;
	char	text[1024]={0};
	qboolean	hasForcePowers;

	y = 65 + 30;

	pad = 28;
	x = 300;
	hasForcePowers=qfalse;

	CG_DrawForceCount( FP_HEAL, x, &y, pad,&hasForcePowers);
	CG_DrawForceCount( FP_LEVITATION, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_SPEED, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_PUSH, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_PULL, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_TELEPATHY, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_GRIP, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_LIGHTNING, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_SABERTHROW, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_SABER_OFFENSE, x, &y, pad,&hasForcePowers );
	CG_DrawForceCount( FP_SABER_DEFENSE, x, &y, pad,&hasForcePowers );

	if (hasForcePowers)
	{
		cgi_SP_GetStringTextString( "SP_INGAME_CURRENTFORCEPOWERS", text, sizeof(text) );
		CG_DrawProportionalString( 200, 65, text, CG_CENTER | CG_BIGFONT, colorTable[CT_WHITE] );
	}
	else
	{	//you are only totally empty on the very first map?
//		cgi_SP_GetStringTextString( "SP_INGAME_NONE", text, sizeof(text) );
//		CG_DrawProportionalString( 320, y+30, text, CG_CENTER | CG_BIGFONT, colorTable[CT_ICON_BLUE] );
		cgi_SP_GetStringTextString( "SP_INGAME_ALONGTIME", text, sizeof(text) );
		int w = cgi_R_Font_StrLenPixels(text,cgs.media.qhFontMedium, 1.5f);
		cgi_R_Font_DrawString((320)-(w/2), y+40, text,  colorTable[CT_ICON_BLUE], cgs.media.qhFontMedium, -1, 1.5f);
	}

}
*/

// barBottom: where the bar ends (Jedi Outcast's sits on the bottom of the screen)
static void CG_LoadBar( const int barBottom )
{
	const int numticks = 9, tickwidth = 40, tickheight = 8;
	const int tickpadx = 20, tickpady = 12;
	const int capwidth = 8;
	const int barwidth = numticks*tickwidth+tickpadx*2+capwidth*2, barleft = ((640-barwidth)/2);
	const int barheight = tickheight + tickpady*2, bartop = barBottom-barheight;
	const int capleft = barleft+tickpadx, tickleft = capleft+capwidth, ticktop = bartop+tickpady;

	cgi_R_SetColor( colorTable[CT_WHITE]);
	// Draw background
	CG_DrawPic(barleft, bartop, barwidth, barheight, cgs.media.levelLoad);

	// Draw left cap (backwards)
	CG_DrawPic(tickleft, ticktop, -capwidth, tickheight, cgs.media.loadTickCap);

	// Draw bar
	CG_DrawPic(tickleft, ticktop, tickwidth*cg.loadLCARSStage, tickheight, cgs.media.loadTick);

	// Draw right cap
	CG_DrawPic(tickleft+tickwidth*cg.loadLCARSStage, ticktop, capwidth, tickheight, cgs.media.loadTickCap);
}

int CG_WeaponCheck( int weaponIndex );

// For printing load screen icons
const int	MAXLOADICONSPERROW = 8;		// Max icons displayed per row
const int	MAXLOADWEAPONS = 16;
const int	MAXLOAD_FORCEICONSIZE = 40;	// Size of force power icons
const int	MAXLOAD_FORCEICONPAD = 12;	// Padding space between icons

static int CG_DrawLoadWeaponsPrintRow( const char *itemName, int weaponsBits,int rowIconCnt, int startIndex)
{
	int		i,endIndex=0, printedIconCnt=0;
	int		iconSize;
	int		holdX,x,y,pad;
	int		yOffset = 0;
	int		width,height;
	vec4_t	color;
	qhandle_t	background;

	if (!cgi_UI_GetMenuItemInfo(
		"loadScreen",
		itemName,
		&x,
		&y,
		&width,
		&height,
		color,
		&background))
	{
		return(0);
	}

	cgi_R_SetColor( color );

	iconSize = 60;
	pad = 12;

	// calculate placement of weapon icons
	holdX = x + (width - ((iconSize*rowIconCnt) + (pad * (rowIconCnt-1))))/2;

	for (i=startIndex;i<MAXLOADWEAPONS;i++)
	{
		if ( !(weaponsBits & ( 1 << i )))	// Does he have this weapon?
		{
			continue;
		}

		if (weaponData[i].weaponIcon[0])
		{
			weaponInfo_t	*weaponInfo;
			CG_RegisterWeapon( i );
			weaponInfo = &cg_weapons[i];
			endIndex = i;

	// NOTE : during loading screen always show the have ammo icon
	//		if (!CG_WeaponCheck(i))
	//		{
	//			CG_DrawPic( holdX, y+yOffset, iconSize, iconSize, weaponInfo->weaponIconNoAmmo );
	//		}
	//		else
			{
				CG_DrawPic( holdX, y+yOffset, iconSize, iconSize, weaponInfo->weaponIcon );
			}

			printedIconCnt++;
			if (printedIconCnt==MAXLOADICONSPERROW)
			{
				break;
			}

			holdX += (iconSize+pad);
		}
	}

	return (endIndex);
}

// Print weapons the player is carrying
// Two rows print if there are too many
static void CG_DrawLoadWeapons( int weaponBits )
{
	int		i,endIndex=0;
	int		iconCnt,rowIconCnt;

	// count the number of weapons owned
	iconCnt = 0;
	for ( i = 1 ; i < MAXLOADWEAPONS ; i++ )
	{
		if ( weaponBits & ( 1 << i ) )
		{
			iconCnt++;
		}
	}

	if (!iconCnt)	// If no weapons, don't display
	{
		return;
	}

	// Single line of icons
	if (iconCnt<=MAXLOADICONSPERROW)
	{
		CG_DrawLoadWeaponsPrintRow("weaponicons_singlerow", weaponBits, iconCnt,0);
	}
	// Two lines of icons
	else
	{
		// Print top row
		endIndex = CG_DrawLoadWeaponsPrintRow("weaponicons_row1", weaponBits, MAXLOADICONSPERROW,0);

		// Print second row
		rowIconCnt = iconCnt - MAXLOADICONSPERROW;
		CG_DrawLoadWeaponsPrintRow("weaponicons_row2", weaponBits, rowIconCnt,endIndex+1);
	}

	cgi_R_SetColor( NULL );
}


static int CG_DrawLoadForcePrintRow( const char *itemName, int forceBits,int rowIconCnt, int startIndex)
{
	int		i,endIndex=0, printedIconCnt=0;
	int		holdX,x,y;
	int		yOffset = 0;
	int		width,height;
	vec4_t	color;
	qhandle_t	background;

	if (!cgi_UI_GetMenuItemInfo(
		"loadScreen",
		itemName,
		&x,
		&y,
		&width,
		&height,
		color,
		&background))
	{
		return(0);
	}

	cgi_R_SetColor( color );

	// calculate placement of weapon icons
	holdX = x + (width - ((MAXLOAD_FORCEICONSIZE*rowIconCnt) + (MAXLOAD_FORCEICONPAD * (rowIconCnt-1))))/2;

	for (i=startIndex;i<MAX_SHOWPOWERS;i++)
	{
		if (!CG_ForcePower_Valid(forceBits,i))	// Does he have this power?
		{
			continue;
		}

		if (force_icons[showPowers[i]])
		{
			endIndex = i;

			CG_DrawPic( holdX, y+yOffset, MAXLOAD_FORCEICONSIZE, MAXLOAD_FORCEICONSIZE, force_icons[showPowers[i]] );

			printedIconCnt++;
			if (printedIconCnt==MAXLOADICONSPERROW)
			{
				break;
			}

			holdX += (MAXLOAD_FORCEICONSIZE+MAXLOAD_FORCEICONPAD);
		}
	}

	return (endIndex);
}

int			loadForcePowerLevel[NUM_FORCE_POWERS];

/*
===============
ForcePowerDataPad_Valid
===============
*/
qboolean CG_ForcePower_Valid(int forceKnownBits, int index)
{
	if ((forceKnownBits & (1 << showPowers[index])) &&
		loadForcePowerLevel[showPowers[index]])	// Does he have the force power?
	{
		return qtrue;
	}

	return qfalse;
}

// Print force powers the player is using
// Two rows print if there are too many
static void CG_DrawLoadForcePowers( int forceBits )
{
	int		i,endIndex=0;
	int		iconCnt=0,rowIconCnt;

	// Count the number of force powers known
	for (i=0; i<MAX_SHOWPOWERS; ++i)
	{
		if (CG_ForcePower_Valid(forceBits, i))
		{
			iconCnt++;
		}
	}

	if (!iconCnt)	// If no force powers, don't display
	{
		return;
	}

	// Single line of icons
	if (iconCnt<=MAXLOADICONSPERROW)
	{
		CG_DrawLoadForcePrintRow("forceicons_singlerow", forceBits, iconCnt,0);
	}
	// Two lines of icons
	else
	{
		// Print top row
		endIndex = CG_DrawLoadForcePrintRow("forceicons_row1", forceBits, MAXLOADICONSPERROW,0);

		// Print second row
		rowIconCnt = iconCnt - MAXLOADICONSPERROW;
		CG_DrawLoadForcePrintRow("forceicons_row2", forceBits, rowIconCnt,endIndex+1);
	}

	cgi_R_SetColor( NULL );
}

// Get the player weapons and force power info
static void CG_GetLoadScreenInfo(int *weaponBits,int *forceBits)
{
	char	s[MAX_STRING_CHARS];
	int		iDummy,i;
	float	fDummy;
	const char	*var;

	gi.Cvar_VariableStringBuffer( sCVARNAME_PLAYERSAVE, s, sizeof(s) );

	// Get player weapons and force powers known
	if (s[0])
	{
	//				|general info				  |-force powers
		sscanf( s, "%i %i %i %i %i %i %i %f %f %f %i %i",
				&iDummy,	//	&client->ps.stats[STAT_HEALTH],
				&iDummy,	//	&client->ps.stats[STAT_ARMOR],
				&*weaponBits,//	&client->ps.stats[STAT_WEAPONS],
				&iDummy,	//	&client->ps.stats[STAT_ITEMS],
				&iDummy,	//	&client->ps.weapon,
				&iDummy,	//	&client->ps.weaponstate,
				&iDummy,	//	&client->ps.batteryCharge,
				&fDummy,	//	&client->ps.viewangles[0],
				&fDummy,	//	&client->ps.viewangles[1],
				&fDummy,	//	&client->ps.viewangles[2],
							//force power data
				&*forceBits,	//	&client->ps.forcePowersKnown,
				&iDummy		//	&client->ps.forcePower,

				);
	}

	// the new JK2 stuff - force powers, etc...
	//
	gi.Cvar_VariableStringBuffer( "playerfplvl", s, sizeof(s) );
	i=0;
	var = strtok( s, " " );
	while( var != NULL )
	{
		/* While there are tokens in "s" */
		loadForcePowerLevel[i++] = atoi(var);
		/* Get next token: */
		var = strtok( NULL, " " );
	}
}

/*
====================
CG_DrawLoadingScreen

Load screen displays the map pic, the mission briefing and weapons/force powers. After a level change
of Jedi Outcast's story, the statistics of the level before take the briefing's place (its levels have
none): Jedi Outcast's mission status screen, in this screen's look.
====================
*/
static void CG_DrawLoadingScreen( qhandle_t	levelshot ,const char *mapName)
{
	int xPos,yPos,width,height;
	vec4_t	color;
	qhandle_t	background;
	int weapons=0, forcepowers=0;

	// Jedi Outcast's level changes ask for the statistics (Jedi Academy's story never does): they go in the briefing's
	// box, if the menu has it
	int briefingX, briefingY, briefingW, briefingH;
	vec4_t briefingColor;
	const qboolean missionStatus = ( gi.Cvar_VariableIntegerValue( "cg_missionstatusscreen" )
		&& cgi_UI_GetMenuItemInfo(
			"loadScreen",
			"briefingtext",
			&briefingX,
			&briefingY,
			&briefingW,
			&briefingH,
			briefingColor,
			&background) ) ? qtrue : qfalse;

	// Get mission briefing for load screen
	if ( missionStatus )
	{
		cgi_Cvar_Set( "ui_missionbriefing", "" );	// the statistics take its place (below)
	}
	else if (cgi_SP_GetStringTextString( va("BRIEFINGS_%s",mapName), NULL, 0 ) == 0)
	{
		cgi_Cvar_Set( "ui_missionbriefing", "@BRIEFINGS_NONE" );
	}
	else
	{
		cgi_Cvar_Set( "ui_missionbriefing", va("@BRIEFINGS_%s",mapName) );
	}

	// Print background
	if (cgi_UI_GetMenuItemInfo(
		"loadScreen",
		"background",
		&xPos,
		&yPos,
		&width,
		&height,
		color,
		&background))
	{
		cgi_R_SetColor( color );
		CG_DrawPic( xPos, yPos, width, height, background );
	}

	// Print level pic
	if (cgi_UI_GetMenuItemInfo(
		"loadScreen",
		"mappic",
		&xPos,
		&yPos,
		&width,
		&height,
		color,
		&background))
	{
		//if (!levelshot)
		//{// No level shot so use screenshot.
 	//		CG_DrawPic( xPos, yPos, 1, 1, 0);	//force the tess to flush
		//	cgi_R_DrawScreenShot( xPos, yPos+height, width, -height );
		//}
		//else
		{
			cgi_R_SetColor( color );
			CG_DrawPic( xPos, yPos, width, height, levelshot );
		}

		// The briefing's box reaches down behind the weapons and Force powers: the statistics keep to the upper
		// part of the screen, beside the level's picture and no lower than it
		if ( missionStatus && yPos + height > briefingY )
		{
			briefingH = Q_min( briefingH, yPos + height - briefingY );
		}
	}

	// Get player weapons and force power info
	CG_GetLoadScreenInfo(&weapons,&forcepowers);

	// Print weapon icons
	if (weapons)
	{
		CG_DrawLoadWeapons(weapons);
	}

	// Print force power icons
	if (forcepowers)
	{
		CG_DrawLoadForcePowers(forcepowers);
	}

	// The level statistics in the briefing's box (drawn before the menu, so that its scan lines go over them too)
	if ( missionStatus )
	{
		CG_MissionCompletionBriefing( briefingX, briefingY, briefingW, briefingH, briefingColor );
	}
}

/*
====================
CG_DrawOutcastLoadingScreen

Jedi Outcast's load screen (its menus have no "loadscreen" menu): the level's picture over the whole
screen, in its shape, and after a level change the statistics of the level before over it. A new
story begins on black with "A long time ago...". A saved game shows its own picture instead.
====================
*/
static void CG_DrawOutcastLoadingScreen( qhandle_t levelshot, const char *mapName )
{
	extern SavedGameJustLoaded_e g_eSavedGameJustLoaded;
	static const vec4_t black = { 0.0f, 0.0f, 0.0f, 1.0f };

	// black beside the picture on a wide screen, and behind it while it loads
	CG_FillRect( -SCREEN_WIDTH, 0, 3*SCREEN_WIDTH, SCREEN_HEIGHT, black );

	if ( g_eSavedGameJustLoaded != eFULL && ( !strcmp( mapName, "kejim_post" ) || !strcmp( mapName, "yavin1" ) ) )
	{//the first level of either story
		char text[1024]={0};
		cgi_SP_GetStringTextString( "SP_INGAME_ALONGTIME", text, sizeof(text) );

		const int w = cgi_R_Font_StrLenPixels( text, cgs.media.qhFontMedium, 1.0f );
		cgi_R_Font_DrawString( 320-(w/2), 140, text, colorTable[CT_ICON_BLUE], cgs.media.qhFontMedium, -1, 1.0f );
		return;
	}

	// A saved game being loaded shows its picture, as in Jedi Outcast: the game as the player last saw it, and on a
	// return to a hub level, that level as the player left it (the engine reads it from saves/<name>.jpg). One saved
	// without a picture shows the level's. (The statistics below are never on: loading a game turns them off.)
	const qboolean drewSavedGamePicture = ( g_eSavedGameJustLoaded == eFULL
		&& cgi_R_DrawLoadingScreenshot( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT ) ) ? qtrue : qfalse;

	// (a level without a picture stays black: the fallback is Jedi Academy's title art)
	if ( !drewSavedGamePicture && cgi_R_RegisterShaderNoMip( va( "levelshots/%s", mapName ) ) )
	{
		cgi_R_SetColor( NULL );
		cgi_R_Set2DAnchor( ANCHOR_KEEP_SHAPE );
		CG_DrawPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, levelshot );
		cgi_R_Set2DAnchor( ANCHOR_CENTRE );
	}

	// the mission status screen, which Jedi Outcast's level changes ask for (CG_MissionCompletion)
	if ( gi.Cvar_VariableIntegerValue( "cg_missionstatusscreen" ) )
	{
		CG_MissionCompletion();
	}
}

/*
====================
CG_DrawInformation

Draw all the status / pacifier stuff during level loading
====================
*/
void CG_DrawInformation( void ) {
	int		y;

	// draw the dialog background
	const char	*info	= CG_ConfigString( CS_SERVERINFO );
	const char	*s		= Info_ValueForKey( info, "mapname" );

	qhandle_t	levelshot;

	extern SavedGameJustLoaded_e g_eSavedGameJustLoaded;	// hack! (hey, it's the last week of coding, ok?
//	if ( g_eSavedGameJustLoaded == eFULL )
//	{
//		levelshot = 0;	//use the loaded thumbnail instead of the levelshot
//	}
//	else
	{
		levelshot = cgi_R_RegisterShaderNoMip( va( "levelshots/%s", s ) );
	#ifndef FINAL_BUILD
		if (!levelshot && !strncmp(s, "work/",5) )
		{
			levelshot = cgi_R_RegisterShaderNoMip( va( "levelshots/%s", s+5 ) );
		}
	#endif
		if (!levelshot) {
			levelshot = cgi_R_RegisterShaderNoMip( "menu/art/unknownmap" );
		}
	}

	if ( G_OutcastInterface() )
	{
		CG_DrawOutcastLoadingScreen( levelshot, s );
	}
	else if ( g_eSavedGameJustLoaded != eFULL && !strcmp(s,"yavin1") )//special case for first map!
	{
		char	text[1024]={0};

		//
		cgi_R_SetColor( colorTable[CT_BLACK] );
		CG_DrawPic( 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, cgs.media.whiteShader );

		cgi_SP_GetStringTextString( "SP_INGAME_ALONGTIME", text, sizeof(text) );

		int w = cgi_R_Font_StrLenPixels(text,cgs.media.qhFontMedium, 1.0f);
		cgi_R_Font_DrawString((320)-(w/2), 140, text,  colorTable[CT_ICON_BLUE], cgs.media.qhFontMedium, -1, 1.0f);
	}
	else
	{
		CG_DrawLoadingScreen(levelshot, s);
		cgi_UI_Menu_Paint( cgi_UI_GetMenuByName( "loadscreen" ), qtrue );
		//cgi_UI_MenuPaintAll();
	}

	CG_LoadBar( G_OutcastInterface() ? 480 : 475 );


	// the first 150 rows are reserved for the client connection
	// screen to write into
//	if ( cg.processedSnapshotNum == 0 )
	{
		// still loading
		// print the current item being loaded

#ifdef _DEBUG
		cgi_R_Font_DrawString( 40, 416, va("LOADING ... %s",cg.infoScreenText),colorTable[CT_LTGOLD1], cgs.media.qhFontSmall, -1, 1.0f );
#endif
	}

	// draw info string information

	y = 20;
	// map-specific message (long map name)
	s = CG_ConfigString( CS_MESSAGE );

	if ( s[0] )
	{
		if (s[0] == '@')
		{
			char text[1024]={0};
			cgi_SP_GetStringTextString( s+1, text, sizeof(text) );
			cgi_R_Font_DrawString( 15, y, va("\"%s\"",text),colorTable[CT_WHITE],cgs.media.qhFontMedium, -1, 1.0f );
		}
		else
		{
			cgi_R_Font_DrawString( 15, y, va("\"%s\"",s),colorTable[CT_WHITE],cgs.media.qhFontMedium, -1, 1.0f );
		}
		y += 20;
	}
}
