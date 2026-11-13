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

// g_outcast.h -- Jedi Outcast's campaign played on Jedi Academy's game code.
//
// The campaign chooser can start this engine with Jedi Outcast's game data mounted beneath Jedi
// Academy's (fs_outcastpath, see FS_Startup). Jedi Outcast's levels and scripts then run on this game
// code, and Jedi Academy's own models (its remade Kyle, Jan, stormtroopers...) replace Jedi Outcast's
// where both games have one. What those levels and scripts expect of the game code, and Jedi Academy
// does differently, is handled here.

#ifndef G_OUTCAST_H
#define G_OUTCAST_H

// True when Jedi Outcast's game data is mounted, whichever campaign is played. Its animations, shaders
// and text are then available to both campaigns (Jedi Academy's own always win where both have one).
qboolean G_OutcastDataAvailable( void );

// True while Jedi Outcast's campaign is being played: the g_campaign cvar is "outcast". New Game sets
// it from the player's choice of story (ui_main.cpp), and saved games keep it.
qboolean G_IsOutcastCampaign( void );

// True while Jedi Outcast's menus are loaded (ui_style "outcast", whichever story is played): what the
// game draws within or beside them (the datapad pages, the loading and death screens) is then laid out
// as Jedi Outcast laid it out.
qboolean G_OutcastInterface( void );

// The player's name in scripts and level targets: Jedi Outcast's scripts call him "kyle".
const char *G_PlayerScriptName( void );

// The weapon the player gets in place of one of Jedi Outcast's: Jedi Academy keeps Jedi Outcast's
// Bryar pistol and stun baton for NPCs only (past MAX_PLAYER_WEAPONS, so the player can't select
// them), so the player gets Jedi Academy's own pistol and melee instead. Any other weapon, or any
// weapon outside Jedi Outcast's campaign, comes back as it is.
int G_Outcast_PlayerWeapon( int weapon );

// The player's gear on spawning (a level change or a loaded game then restores what he carried):
// Jedi Outcast always gives the Bryar pistol (held ready), electrobinoculars and full ammunition
// apart from explosives, along with the stun baton or the saber that the player start chooses.
// (The pistol and baton as G_Outcast_PlayerWeapon gives them.)
void G_Outcast_GiveStartingGear( gentity_t *player );

// Jedi Outcast's mission objectives, which take slots 1 onwards of Jedi Academy's objective list:
// the slot for one of Jedi Outcast's objective names (-1 for none), and the name in a slot ("" for none).
int G_Outcast_ObjectiveSlot( const char *name );
const char *G_Outcast_ObjectiveName( int slot );

// Jedi Outcast's small script navgoals: waypoint_navgoal_8, _4, _2 and _1 (spawn functions, the
// number is the half-width of the box). An NPC reaches one by touching it, as in Jedi Outcast; unlike
// there, they are points of the navigation graph, since Jedi Academy only routes NPCs to those.
void SP_waypoint_navgoal_8( gentity_t *ent );
void SP_waypoint_navgoal_4( gentity_t *ent );
void SP_waypoint_navgoal_2( gentity_t *ent );
void SP_waypoint_navgoal_1( gentity_t *ent );

// Called after Jedi Academy's humanoid animation.cfg is read: adds the animations Jedi Academy lacks
// from Jedi Outcast's animation.cfg. Their frames were appended to Jedi Academy's skeleton by the
// renderer (tr_outcast_skeleton.cpp), which says where they start in r_outcastAnimationsStart.
void G_Outcast_AddAnimations( animation_t *animations );

// Saved games hold only Jedi Academy's animations (animFileSet_t), so after loading one this adds
// Jedi Outcast's to the humanoid animation set again.
void G_Outcast_AddAnimationsAfterLoad( void );

#endif // G_OUTCAST_H
