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

// g_ragdoll.h -- active (muscle driven) death ragdolls for Jedi Outcast.
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
//   * Hit reactions: a shot or saber hit makes a living character a powered ragdoll for about half a
//     second. Strong muscles pull every part toward the live animation while the hit shoves the body,
//     so the character flinches (torso twisting, head snapping, arms flinging) without ever stopping
//     what it is doing. Killed or knocked down mid-reaction, the body carries on with its momentum.
//   * Knockdowns: see G_Ragdoll_OnForceKnockdown.
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
G_Ragdoll_OnForceKnockdown

Called by WP_ForceKnockdown after Force Push or Pull knocks a living character over (NPCs only,
never the player). The character falls as a ragdoll from the next drawn frame, lies dazed for a
moment once it lands, then gets back up with the game's own get-up animation and its AI carries
on. Killed while down, it simply becomes a death ragdoll. Can be switched off with
g_ragdollKnockdowns 0.
==================
*/
void G_Ragdoll_OnForceKnockdown( gentity_t *entity );

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
                      Lightning damage that does reach a corpse (MOD_ELECTROCUTE) also causes
                      convulsions, through G_Ragdoll_RecordDamage.
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

#endif // G_RAGDOLL_H
