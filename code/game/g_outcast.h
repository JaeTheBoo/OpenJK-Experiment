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

// True while Jedi Outcast's campaign is being played.
qboolean G_IsOutcastCampaign( void );

// The player's name in scripts and level targets: Jedi Outcast's scripts call him "kyle".
const char *G_PlayerScriptName( void );

// The player's gear on spawning (a level change or a loaded game then restores what he carried):
// Jedi Outcast always gives the Bryar pistol (held ready), electrobinoculars and full ammunition
// apart from explosives, along with the stun baton or the saber that the player start chooses.
void G_Outcast_GiveStartingGear( gentity_t *player );

// Called after Jedi Academy's humanoid animation.cfg is read: adds the animations Jedi Academy lacks
// from Jedi Outcast's animation.cfg. Their frames were appended to Jedi Academy's skeleton by the
// renderer (tr_outcast_skeleton.cpp), which says where they start in r_outcastAnimationsStart.
void G_Outcast_AddAnimations( animation_t *animations );

// Saved games hold only Jedi Academy's animations (animFileSet_t), so after loading one this adds
// Jedi Outcast's to the humanoid animation set again.
void G_Outcast_AddAnimationsAfterLoad( void );

#endif // G_OUTCAST_H
