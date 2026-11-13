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

// sys_campaign_select.h -- campaign chooser shown when the Jedi Academy single player program starts.
//
// Before the engine starts, a dialog asks which game to start:
//
//   * Jedi Academy  - the engine continues starting up. When Jedi Outcast's install location is set,
//                     its game data is mounted beneath Jedi Academy's (fs_outcastpath) and New Game
//                     asks which story to play: Jedi Academy's (create your own character) or Jedi
//                     Outcast's (as Kyle Katarn, on Jedi Academy's game code; see code/game/g_outcast.h).
//   * Jedi Outcast (original)
//                   - the separate Jedi Outcast engine (openjo_sp) is started with the Jedi Outcast
//                     game data, where you play as Kyle Katarn, and this program exits.
//   * Quit          - this program exits without starting anything.
//
// The install locations of both games are read from "campaigns.ini" next to the executable. A
// commented template is written there the first time the program runs without one.
//
// The dialog can be skipped with a launch argument, for shortcuts and automated testing:
//   --campaign=academy    start Jedi Academy directly
//   --campaign=outcast    start Jedi Outcast directly
//   --campaign=outcast-ja start Jedi Academy with Jedi Outcast's story already chosen (g_campaign)
//
// Only the Jedi Academy single player engine uses this. The Jedi Outcast engine, the multiplayer
// engine and the dedicated server are unaffected.

#ifndef SYS_CAMPAIGN_SELECT_H
#define SYS_CAMPAIGN_SELECT_H

#include <stddef.h>

/*
==================
Sys_CampaignSelect_Run

Shows the campaign chooser (unless a --campaign argument was given) and acts on the choice.

programDirectory           Folder that contains this executable.
argc, argv                 The program's command line, exactly as passed to main.
extraCommandLine           Receives engine commands to append to the command line when Jedi
                           Academy (or Jedi Outcast on its engine) was chosen, for example
                           "+set fs_basepath ...". May be empty.
extraCommandLineSize       Size of the extraCommandLine buffer in bytes.

Returns true when the Jedi Academy engine should continue starting up, or false when the
program should exit (Jedi Outcast was started, the player chose Quit, or an error was shown).
==================
*/
bool Sys_CampaignSelect_Run( const char *programDirectory, int argc, char **argv,
							 char *extraCommandLine, size_t extraCommandLineSize );

/*
==================
Sys_CampaignSelect_IsLauncherArgument

Returns true for command line arguments that belong to the campaign chooser (--campaign=...).
These must not be passed on to the engine, which does not understand them.
==================
*/
bool Sys_CampaignSelect_IsLauncherArgument( const char *argument );

#endif // SYS_CAMPAIGN_SELECT_H
