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

// g_ragdoll_balance.h -- balance and stumbling for living characters (Jedi Outcast).
//
// Every living character has a balance meter. Each hit knocks it off balance by how hard the hit
// pushes (the same push that jolts the body in a hit reaction), and shots to the legs count for
// more. Balance comes back quickly: whatever was lost halves every half second. Depending on how
// far off balance the character is:
//
//   * Slightly     - nothing here; the character only reacts to the hit itself (see the shot
//                    reactions in g_ragdoll.h).
//   * Off balance  - the character staggers. This module only decides when and how hard: it leaves
//                    a push (TakeStumblePush) that the physics adds to the body, says how far the
//                    upper body leans with it and how hard the arms windmill (StumbleLean), and the
//                    balance controller in g_ragdoll.cpp (StepToKeepBalance) steps the feet to catch
//                    the body. More hits keep it staggering.
//   * Pushed over  - one huge hit, more hits while already staggering, staggering off a ledge, or a
//                    misstep (the body moving further than any step can catch, LoseFooting): the
//                    character falls over into a knockdown (a ragdoll, see G_Ragdoll_OnKnockdown)
//                    and gets back up afterwards.
//
// Only NPCs stumble; the player's own movement is never taken over. Bosses, characters that cannot
// be knocked back, and characters busy with something special (rolling, flipping, saber locks,
// crouching, being gripped, manning a gun) are left alone.
//
// Console variables:
//   g_ragdollStumbles  1 = hits can make characters stumble and fall over, 0 = never
//   g_ragdollBalance   how much push a character can take in a short time before it stumbles;
//                      falling over takes a little over twice as much. Bigger is steadier.

#ifndef G_RAGDOLL_BALANCE_H
#define G_RAGDOLL_BALANCE_H

#include "g_local.h"
#include "g_ragdoll_math.h"

namespace ragdoll
{

enum balanceOutcome_t
{
	BALANCE_STEADY,		// not stumbling
	BALANCE_STUMBLING,	// stumbling this frame
	BALANCE_FALLING		// has just been knocked over; the caller starts the knockdown ragdoll
};

// Creates the console variables. Called from G_Ragdoll_RegisterCvars.
void RegisterBalanceCvars( void );

// Forgets every character's balance. Called when a level starts or ends.
void ResetBalance( void );

// Forgets one character's balance, for example when it dies or its entity is freed.
void ForgetBalance( int entityNumber );

// Called for every hit on a living character, before the damage is applied.
//   pushDirection  the direction of the hit (only its horizontal part matters)
//   hitPoint       where the hit landed, used to tell leg hits apart
//   push           how hard the hit pushes: damage times how hard that kind of damage pushes
void LoseBalance( gentity_t *character, const vec3_t pushDirection, const vec3_t hitPoint, float push );

// Called every drawn frame for every living character. Ends staggers that are over and carries out
// falls. When it returns BALANCE_FALLING the character has already been put into its knockdown
// animation, and the caller should turn the fall into a ragdoll.
balanceOutcome_t UpdateBalance( gentity_t *character );

// Called by the physics when a character's body is moving further than any step can catch (or is
// about to step into a drop): it has misstepped. It falls over (toward fallDirection) on the next
// drawn frame.
void LoseFooting( int entityNumber, const Vector3 &fallDirection );

// The speed a new stagger adds to the character's body (a horizontal velocity), once per hit.
// Returns false when there is no push waiting.
bool TakeStumblePush( int entityNumber, Vector3 &velocityChange );

// While a character is stumbling: the horizontal direction its upper body should lean and by how
// much (radians). Returns false when the character is not stumbling.
bool StumbleLean( int entityNumber, Vector3 &leanDirection, float &leanRadians );

} // namespace ragdoll

#endif // G_RAGDOLL_BALANCE_H
