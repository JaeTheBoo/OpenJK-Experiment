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

// g_saber_guard.h -- the player's lightsaber guard (Jedi Outcast).
//
// With g_saberGuard 1 (the default) the player no longer blocks by itself:
//   * Holding the alt-attack button (right mouse) with the saber lit raises the guard. Only with the
//     guard up are blaster bolts deflected and saber strikes parried; otherwise the blade does not
//     stop them (it still meets other blades while attacking).
//   * Every block costs Force power, less the higher the player's Saber Defense; a saber strike costs
//     far more than a bolt. With no Force left the guard gives way and nothing is blocked. Saber
//     Defense 0 cannot guard at all.
//   * Turning the view left, right, up or down aims the guard that way: the saber is held in that
//     parry. A saber strike coming from the side the guard faces is a perfect block and costs nothing.
//   * Raising the guard just before a strike lands is a parry: facing the strike, it knocks the
//     attacker's saber out of their hand; facing the wrong way, the player's own saber is knocked away.
//     No other block disarms anyone (the game's own knock-aways and broken parries no longer do).
//   * The saber is thrown with its own button instead of alt-attack: "+button10" (bind CTRL +button10).
// NPCs block as before. g_saberGuard 2 (or g_ragdollDebug 1) also prints every block to the console.

#ifndef G_SABER_GUARD_H
#define G_SABER_GUARD_H

#include "g_local.h"

// The usercmd button "+button10" sends: throws the lightsaber.
#define BUTTON_SABER_THROW	1024

void G_SaberGuard_RegisterCvars( void );

// True while the rules above apply (g_saberGuard on).
qboolean G_SaberGuard_RulesApply( void );

// Whether the player blocks without guarding. The game's own blocking checks ask this instead of
// reading g_saberAutoBlocking: with the guard rules on, never.
qboolean G_PlayerBlocksAutomatically( void );

// Called by ClientThink for the player before anything reads the command. With the saber out, the
// alt-attack button becomes the guard and the throw button becomes the game's own throw (alt-attack);
// the guard's aim follows the view's turning; while the guard is up the game's blocking window stays
// open.
void G_SaberGuard_ReadCommand( gentity_t *player, usercmd_t *command );

// Called by ClientThink after the player has moved: holds the saber in the guard's parry, and keeps
// the blade solid only while it guards or attacks.
void G_SaberGuard_UpdatePose( gentity_t *player );

// True when the guard rules decide who is disarmed by a strike on this defender (the player): the
// game's own knock-aways and broken parries then take no one's saber.
qboolean G_SaberGuard_OwnsDisarms( const gentity_t *defender );

// A saber strike on a character about to parry it. For the player: settles a parry (disarming the
// attacker, or the player if the guard faces the wrong way) or pays the guard's Force cost (nothing
// for a perfect block). Returns qfalse if the player cannot pay, so the strike lands. Always qtrue for
// anyone else.
qboolean G_SaberGuard_PayForParry( gentity_t *defender, gentity_t *attacker, const vec3_t strikePoint );

// A missile hitting something. If that is the player or the player's blade: with the guard up, the
// bolt in front and the Force to pay, the bolt is deflected and qtrue is returned (nothing more to
// do). Otherwise *hitEntity becomes the player (a bolt striking the lowered blade goes on into the
// player) and the impact carries on as usual.
qboolean G_SaberGuard_DeflectMissile( gentity_t *missile, gentity_t **hitEntity, trace_t *trace );

#endif // G_SABER_GUARD_H
