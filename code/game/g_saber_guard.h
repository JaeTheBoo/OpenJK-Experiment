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

// g_saber_guard.h -- the player's lightsaber guard for Jedi Academy (ported from Jedi Outcast's
// codeJK2/game, see the differences below).
//
// With g_saberGuard 1 (the default) the player no longer blocks by itself:
//   * Holding the alt-attack button (right mouse) with the saber in hand raises the guard. Only with
//     the guard up are blaster bolts deflected and saber strikes parried; otherwise the blade does not
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
//
// Jedi Academy (this port) differs from Jedi Outcast in a few places:
//   * Jedi Academy's alt-attack did more with a saber than throw it: with attack it starts a kata
//     (the old control scheme), and with a staff or a saber that cannot be thrown it kicks. All of that
//     moves to "+button10" with the throw, which stands in for the old alt-attack: +button10 throws,
//     attack with +button10 is a kata, and with a staff +button10 kicks. Alt-attack is the guard alone.
//     While riding a vehicle, or looking through binoculars, alt-attack keeps its usual job.
//   * Raising the guard lights a saber that is off (Jedi Academy's own manual blocking did that too).
//   * The guard is held with the game's own manual-blocking stance (PM_SetSaberMove picks the parry
//     the guard faces): single sabers, dual sabers and staffs each use their own parries.
//   * A guard that gives way lets the strike through (the game would otherwise often stop it anyway).
//     As in Jedi Outcast, the game's own rules can still break a block, a perfect block or a parry the
//     guard settled (a strike in a strong style; Jedi Academy's rules also weigh the defence), and the
//     Force paid for it is not given back. The guard holds a stance rather than parrying on its own, so
//     a strike that misses the held blade (from the side the guard does not face, say) still lands, as
//     it did in Jedi Outcast.
//   * The guard drops while a Force power keeps the saber hand busy (grip, lightning, drain) or a
//     Force power's set animation plays (absorb, protect, rage), as the game's own blocking does.
//   * Disarming takes the right-hand saber (Jedi Academy's own disarms do the same; with dual sabers
//     the left one stays in hand). A saber made never to be knocked away (SFL_NOT_DISARMABLE) stays in
//     hand: a parry against it still stops the strike, and a failed parry is paid for like a block.

#ifndef G_SABER_GUARD_H
#define G_SABER_GUARD_H

#include "g_local.h"

// The usercmd button "+button10" sends: throws the lightsaber (and does everything else alt-attack
// did with a saber, see above). Jedi Academy's own buttons use the bits up to 256.
#define BUTTON_SABER_THROW	1024

void G_SaberGuard_RegisterCvars( void );

// True while the rules above apply (g_saberGuard on).
qboolean G_SaberGuard_RulesApply( void );

// Whether the player blocks without guarding. The game's own blocking checks ask this instead of
// reading g_saberAutoBlocking: with the guard rules on, never.
qboolean G_PlayerBlocksAutomatically( void );

// Called by ClientThink for the player before anything reads the command. With the saber out, the
// alt-attack button becomes the guard and the throw button becomes the game's own alt-attack; the
// guard's aim follows the view's turning; while the guard is up the game's blocking window stays
// open (saberBlockingTime), and it closes as soon as the guard drops.
void G_SaberGuard_ReadCommand( gentity_t *player, usercmd_t *command );

// Called by ClientThink after the player has moved: the blade in hand is solid (stops other blades
// and bolts) only while it guards or attacks.
void G_SaberGuard_UpdateBlade( gentity_t *player );

// The torso animation of the game's manual-blocking stance (PM_SetSaberMove): while the player
// guards, the parry the guard faces, for the player's saber style; otherwise defaultAnimation.
int G_SaberGuard_StanceAnimation( gentity_t *character, int defaultAnimation );

// True when the guard rules decide who is disarmed by a strike on this defender (the player): the
// game's own knock-aways and broken parries then take no one's saber.
qboolean G_SaberGuard_OwnsDisarms( const gentity_t *defender );

// A saber strike on a character about to parry it. For the player: settles a parry (disarming the
// attacker, or the player if the guard faces the wrong way) or pays the guard's Force cost (nothing
// for a perfect block). Returns qfalse if the player cannot pay, so the strike lands. Always qtrue for
// anyone else.
qboolean G_SaberGuard_PayForParry( gentity_t *defender, gentity_t *attacker, const vec3_t strikePoint );

// True if the player's guard has just given way to a strike from this attacker (the last
// G_SaberGuard_PayForParry for it, this frame, could not pay): the strike should carry through.
qboolean G_SaberGuard_GaveWay( const gentity_t *defender, const gentity_t *attacker );

// True while a strike from this attacker is being stopped by the player's timed parry, which has just
// knocked one of the sabers away: no saber lock can start from it.
qboolean G_SaberGuard_Parried( const gentity_t *defender, const gentity_t *attacker );

// A missile hitting something. If that is the player or the player's blade: with the guard up, the
// bolt in front and the Force to pay, the bolt is deflected and qtrue is returned (nothing more to
// do). Otherwise *hitEntity becomes the player (a bolt striking the lowered blade goes on into the
// player) and the impact carries on as usual.
qboolean G_SaberGuard_DeflectMissile( gentity_t *missile, gentity_t **hitEntity, trace_t *trace );

// True if the player's guard would deflect this missile were it to strike now: the guard is up, the
// missile is one a guard deflects, it comes from in front and the player has the Force to pay for it.
// With missile NULL, whether the guard is up at all. Changes nothing: the saber arm's reach for an
// incoming bolt asks it (see g_saber_reach.h).
qboolean G_SaberGuard_WillDeflect( gentity_t *player, gentity_t *missile );

#endif // G_SABER_GUARD_H
