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

// g_ragdoll.h -- active (muscle driven) death ragdolls for Jedi Academy (ported from
// Jedi Outcast's codeJK2/game, see the differences below).
//
// When a humanoid character dies, its body is handed to a physics simulation instead of a canned
// death animation. Simulated muscles act on that body over time, in the style of NaturalMotion's
// Euphoria (Grand Theft Auto IV, The Force Unleashed):
//
//   1. Staggering  - the hit knocks the body in the direction of the blow while the legs and torso
//                    still fight to stay up, so the character stumbles before collapsing.
//   2. Falling     - the arms reach out toward the ground in the direction of the fall and the
//                    neck keeps the head up.
//   3. Landed      - the arms give way on impact.
//   4. Writhing    - sometimes the character moves weakly on the ground for a few seconds.
//   5. Limp        - all muscles relax and the body settles under gravity.
//   6. Asleep      - once still, the body stops being simulated (a later hit wakes it again).
//
// Living characters:
//   * Shot reactions (NPCs, in the style of GTA IV/V and Red Dead Redemption): a hit takes control of
//     the character. Its AI pauses (no walking, no shooting) and its body reacts on its own: the hit
//     jolts it, tense muscles hold its posture against the jolt (gravity is compensated, so nothing
//     sags), a free hand clutches the wound, and a hard hit sends it staggering, leaning, windmilling
//     its arms, while a balance controller steps its feet to catch it. The character goes where its
//     body goes. Once balanced and still it blends back into its animation and the AI carries on; a
//     body no step can catch falls over (a knockdown). Bosses and saber users only flinch, so their
//     duels keep their own animations. Switch off with g_ragdollShotReactions 0.
//   * Flinches (the player, and NPCs when shot reactions are off): a powered ragdoll for about half a
//     second, with strong muscles pulling every part toward the live animation while the hit shoves
//     the body, so the character flinches without ever stopping what it is doing.
//   * Killed or knocked down mid-reaction, the body carries on with its momentum.
//   * Stumbles: hits add up. A character knocked far enough off balance stumbles a step or two in
//     the direction of the push, leaning with it, and one pushed further falls over into a
//     knockdown. See g_ragdoll_balance.h.
//   * Knockdowns: see G_Ragdoll_OnKnockdown.
//
// Collision:
//   * Body parts collide with each other (self collision), except parts joined by a joint or that
//     already overlap in the rest pose. Parts that stay tangled for half a second are let go until
//     they separate, so a stuck limb cannot make the whole body jitter or creep along the floor.
//   * Ragdolls collide with each other: every awake ragdoll is simulated in one shared scene.
//   * Living characters push ragdolls (walking into a corpse shoves it aside, at about walking
//     speed) but are never pushed back. Their collision boxes are used as moving obstacles.
//   * A sleeping ragdoll wakes up when another ragdoll or a living character touches it.
//
// All ragdolls are stepped together once per rendered frame (from CG_Player, by whichever
// character is drawn first), so bodies move smoothly at the rendering frame rate. Everything can
// be tuned with the g_ragdoll* cvars.
//
// Jedi Academy (this port) differs from Jedi Outcast in a few places:
//   * Jedi Academy has a ragdoll of its own (G_RagDoll in g_main.cpp, switched by the "broadsword"
//     cvar). It skips every character these ragdolls handle (G_Ragdoll_OwnsBody), so the two never
//     write the same bones; with g_ragdoll 0 it works exactly as before.
//   * The 53 bone skeleton lacks Jedi Outcast's toe and extra finger bones and adds robe tails; all
//     18 bones the rig simulates exist in both, with identical rest poses.
//   * Characters the game carries itself are left alone: vehicle riders, characters held by a
//     rancor, a wampa or a sand creature, Force-drained characters, and pilots killed with their
//     vehicle (noRagTime -1). Alora and Boba Fett join the bosses who never stumble.
//   * Concussion rifle, Force Lightning, Force Drain and gas damage have their own means of death.

#ifndef G_RAGDOLL_H
#define G_RAGDOLL_H

#include "g_local.h"

// Creates the g_ragdoll* console variables. Called from G_InitCvars.
void G_Ragdoll_RegisterCvars( void );

// Forgets every ragdoll. Called when a level starts or ends (including loading a saved game).
void G_Ragdoll_Reset( void );

// Called by G_Damage for every hit, before the damage is applied. The last hit is remembered so
// the death ragdoll can react to it, and hits on an existing ragdoll push it immediately.
void G_Ragdoll_RecordDamage( gentity_t *target, const vec3_t direction, const vec3_t point, int damage, int meansOfDeath );

// Called by player_die once the character is dead. The ragdoll starts the next time the
// character is drawn, so it begins from exactly the pose on screen.
void G_Ragdoll_OnDeath( gentity_t *entity, int meansOfDeath, int hitLocation );

// Called by G_FreeEntity before the entity is cleared.
void G_Ragdoll_OnEntityFreed( gentity_t *entity );

/*
==================
G_Ragdoll_OnKnockdown

Called once a living character has been put into a knockdown animation: by WP_ForceKnockdown
(Force Push or Pull), by G_Knockdown (explosions and other heavy blows) and by the stumbling code
(pushed over while off balance). NPCs only, never the player. The character falls as a ragdoll
from the next drawn frame, lies dazed for a moment once it lands, then gets back up with the
game's own get-up animation and its AI carries on. Killed while down, it simply becomes a death
ragdoll. Can be switched off with g_ragdollKnockdowns 0.
==================
*/
void G_Ragdoll_OnKnockdown( gentity_t *entity );

/*
==================
Force powers on ragdolls

G_Ragdoll_ForceThrow  Called by ForceThrow (Force Push and Pull). Flings every ragdoll in front of
                      the user away from them (push) or toward them (pull), using the power's own
                      range and cone. Pushing a ragdoll you are holding with Grip throws it.

G_Ragdoll_ForceGripStart  Called by ForceGrip when there is nothing alive to grip. Lifts the ragdoll
                      the user is aiming at and holds it in front of them, dangling, for as long as
                      the Grip power stays active. Returns qtrue if a ragdoll was picked up.

G_Ragdoll_IsGrippingRagdoll  True while the user is holding a ragdoll with Force Grip. ForceGrip
                      uses this to keep holding instead of starting the power again every frame.

G_Ragdoll_ForceLightning  Called every time Force Lightning fires. Makes every ragdoll it reaches
                      convulse (the game itself does not shock corpses at the highest level).
                      Lightning damage that does reach a corpse (MOD_FORCE_LIGHTNING, or
                      MOD_ELECTROCUTE) also causes convulsions, through G_Ragdoll_RecordDamage.
==================
*/
void G_Ragdoll_ForceThrow( gentity_t *user, qboolean pull, int powerLevel, float range, float cone );
qboolean G_Ragdoll_ForceGripStart( gentity_t *user, float range );
qboolean G_Ragdoll_IsGrippingRagdoll( const gentity_t *user );
void G_Ragdoll_ForceLightning( gentity_t *user, int powerLevel );

/*
==================
G_Ragdoll_UpdateAndPose

Called from CG_Player every frame for every drawn character, after the normal animation has been
set up. If the character is (or should become) a ragdoll, this steps the physics, writes the
pose into the model's bones, and replaces the draw position and orientation.

renderTime    the client's current time in milliseconds (cg.time)
renderOrigin  in: where the model would normally be drawn; out: where to draw the ragdoll
renderAxis    in: the model's normal orientation; out: the ragdoll's orientation
renderAngles  out: the angles matching renderAxis, used by later bolt lookups in CG_Player

Returns qtrue if the character is a ragdoll and the outputs were changed.
==================
*/
qboolean G_Ragdoll_UpdateAndPose( gentity_t *entity, int renderTime, vec3_t renderOrigin, vec3_t renderAxis[3], vec3_t renderAngles );

/*
==================
G_Ragdoll_IsControllingCharacter

True while a shot reaction has taken control of this NPC's body. Called by ClientThink_real before
the character moves: while it returns qtrue the character neither walks nor shoots (its AI keeps
thinking, so it carries on from where it was once it has its balance back).
==================
*/
qboolean G_Ragdoll_IsControllingCharacter( gentity_t *entity );

/*
==================
G_Ragdoll_OwnsBody

True if this character's body is one of these ragdolls, or a corpse about to become one the next
time it is drawn, or a corpse whose ragdoll came to rest and keeps its final pose. Jedi Academy's
own ragdoll (G_RagDoll, called from CG_Player) leaves such characters alone. Always qfalse with
g_ragdoll 0.
==================
*/
qboolean G_Ragdoll_OwnsBody( gentity_t *entity );

#endif // G_RAGDOLL_H
