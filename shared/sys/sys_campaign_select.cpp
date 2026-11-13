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

// sys_campaign_select.cpp -- campaign chooser for the Jedi Academy single player program.
// See sys_campaign_select.h for an overview.
//
// This runs before Com_Init, so none of the engine's own systems (cvars, the file system,
// Com_Printf to the console) are available yet. It only uses the C library, SDL's message
// boxes, and the operating system's process functions.

// Only the Jedi Academy single player engine shows the chooser. The Jedi Outcast engine compiles
// the same shared sources with JK2_MODE defined, and must not offer to launch itself.
#if defined(_JK2EXE) && !defined(JK2_MODE)

#include "sys_campaign_select.h"
#include "qcommon/qcommon.h"
#include "sys_public.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#	include <windows.h>
#	define PATH_SEPARATOR			"\\"
#	define EXECUTABLE_EXTENSION		".exe"
#else
#	include <spawn.h>
#	include <unistd.h>
extern char **environ;
#	define PATH_SEPARATOR			"/"
#	define EXECUTABLE_EXTENSION		""
#endif

static const char SETTINGS_FILE_NAME[]			= "campaigns.ini";
static const char JEDI_OUTCAST_ENGINE_NAME[]	= "openjo_sp." ARCH_STRING EXECUTABLE_EXTENSION;
static const char CAMPAIGN_ARGUMENT_PREFIX[]	= "--campaign=";
static const char DIALOG_TITLE[]				= "OpenJK";

// Every installed copy of either game has this file inside its GameData folder.
static const char GAME_DATA_MARKER_FILE[]		= "base" PATH_SEPARATOR "assets0.pk3";

typedef enum
{
	CAMPAIGN_NOT_CHOSEN,
	CAMPAIGN_JEDI_ACADEMY,
	CAMPAIGN_JEDI_OUTCAST,
	CAMPAIGN_JEDI_OUTCAST_ON_ACADEMY,	// Jedi Outcast's campaign on this engine and its game code
	CAMPAIGN_QUIT
} campaignChoice_t;

// Jedi Outcast's campaign on this engine keeps its saves and settings apart from Jedi Academy's,
// in this folder of the engine's home folder (it is used as fs_game).
static const char OUTCAST_ON_ACADEMY_GAME_FOLDER[]	= "outcast";

// Values read from campaigns.ini. An empty string means "use the default".
typedef struct
{
	char jediAcademyGameData[MAX_OSPATH];
	char jediOutcastGameData[MAX_OSPATH];
	char jediOutcastEngine[MAX_OSPATH];
} campaignSettings_t;

/*
==================
Small helpers
==================
*/

static void CampaignSelect_ShowError( const char *message )
{
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, DIALOG_TITLE, message, NULL );
}

static bool CampaignSelect_FileExists( const char *path )
{
	struct stat fileInformation;
	return stat( path, &fileInformation ) == 0 && ( fileInformation.st_mode & S_IFREG );
}

// A folder counts as a game's GameData folder when it contains base/assets0.pk3.
static bool CampaignSelect_IsGameDataFolder( const char *folder )
{
	char markerPath[MAX_OSPATH];
	Com_sprintf( markerPath, sizeof( markerPath ), "%s" PATH_SEPARATOR "%s", folder, GAME_DATA_MARKER_FILE );
	return CampaignSelect_FileExists( markerPath );
}

// Removes trailing slashes. On Windows a path ending in a backslash would otherwise escape the
// closing quote when it is placed on a command line ("C:\Games\" is read as C:\Games").
static void CampaignSelect_RemoveTrailingSeparators( char *path )
{
	size_t length = strlen( path );
	while ( length > 1 && ( path[length - 1] == '\\' || path[length - 1] == '/' ) )
	{
		path[--length] = '\0';
	}
}

// Trims leading and trailing whitespace and one pair of surrounding quotes, in place.
// Returns a pointer to the first character of the trimmed text.
static char *CampaignSelect_TrimValue( char *text )
{
	while ( *text && isspace( (unsigned char)*text ) )
	{
		text++;
	}

	size_t length = strlen( text );
	while ( length > 0 && isspace( (unsigned char)text[length - 1] ) )
	{
		text[--length] = '\0';
	}

	if ( length >= 2 && text[0] == '"' && text[length - 1] == '"' )
	{
		text[length - 1] = '\0';
		text++;
	}
	return text;
}

/*
==================
Settings file
==================
*/

static void CampaignSelect_WriteSettingsTemplate( const char *settingsPath )
{
	FILE *settingsFile = fopen( settingsPath, "w" );
	if ( !settingsFile )
	{
		return; // Not fatal: the program may be installed in a read only folder.
	}

	fputs(
		"# Campaign chooser settings for the OpenJK single player program.\n"
		"#\n"
		"# When the program starts it asks whether to play Jedi Academy or Jedi Outcast.\n"
		"# These settings say where each game is installed. A GameData folder is the one that\n"
		"# contains the \"base\" folder with assets0.pk3 inside it, for example:\n"
		"#   C:\\Program Files (x86)\\Steam\\steamapps\\common\\Jedi Outcast\\GameData\n"
		"#\n"
		"# Leave a value empty to use its default:\n"
		"#   JediAcademyGameData - the folder this program is in\n"
		"#   JediOutcastGameData - no default, must be set to play Jedi Outcast\n"
		"#   JediOutcastEngine   - " "openjo_sp." ARCH_STRING EXECUTABLE_EXTENSION " in this folder, or in ..\\JediOutcast\n"
		"#\n"
		"# Lines starting with # are comments.\n"
		"\n"
		"JediAcademyGameData =\n"
		"JediOutcastGameData =\n"
		"JediOutcastEngine =\n",
		settingsFile );

	fclose( settingsFile );
}

static void CampaignSelect_LoadSettings( const char *programDirectory, campaignSettings_t *settings )
{
	memset( settings, 0, sizeof( *settings ) );

	char settingsPath[MAX_OSPATH];
	Com_sprintf( settingsPath, sizeof( settingsPath ), "%s" PATH_SEPARATOR "%s", programDirectory, SETTINGS_FILE_NAME );

	FILE *settingsFile = fopen( settingsPath, "r" );
	if ( !settingsFile )
	{
		CampaignSelect_WriteSettingsTemplate( settingsPath );
		return;
	}

	char line[MAX_OSPATH + 64];
	while ( fgets( line, sizeof( line ), settingsFile ) )
	{
		char *text = CampaignSelect_TrimValue( line );
		if ( text[0] == '\0' || text[0] == '#' || text[0] == ';' )
		{
			continue;
		}

		char *equalsSign = strchr( text, '=' );
		if ( !equalsSign )
		{
			continue;
		}
		*equalsSign = '\0';

		const char *key = CampaignSelect_TrimValue( text );
		char *value = CampaignSelect_TrimValue( equalsSign + 1 );

		char *destination = NULL;
		if ( !Q_stricmp( key, "JediAcademyGameData" ) )
		{
			destination = settings->jediAcademyGameData;
		}
		else if ( !Q_stricmp( key, "JediOutcastGameData" ) )
		{
			destination = settings->jediOutcastGameData;
		}
		else if ( !Q_stricmp( key, "JediOutcastEngine" ) )
		{
			destination = settings->jediOutcastEngine;
		}

		if ( destination )
		{
			Q_strncpyz( destination, value, MAX_OSPATH );
			CampaignSelect_RemoveTrailingSeparators( destination );
		}
	}

	fclose( settingsFile );
}

/*
==================
Choosing a campaign
==================
*/

bool Sys_CampaignSelect_IsLauncherArgument( const char *argument )
{
	return !Q_strncmp( argument, CAMPAIGN_ARGUMENT_PREFIX, (int)strlen( CAMPAIGN_ARGUMENT_PREFIX ) );
}

// Returns the campaign requested with --campaign=..., or CAMPAIGN_NOT_CHOSEN when there is none.
static campaignChoice_t CampaignSelect_ChoiceFromArguments( int argc, char **argv )
{
	for ( int argumentIndex = 1; argumentIndex < argc; argumentIndex++ )
	{
		if ( !Sys_CampaignSelect_IsLauncherArgument( argv[argumentIndex] ) )
		{
			continue;
		}

		const char *campaignName = argv[argumentIndex] + strlen( CAMPAIGN_ARGUMENT_PREFIX );
		if ( !Q_stricmp( campaignName, "academy" ) )
		{
			return CAMPAIGN_JEDI_ACADEMY;
		}
		if ( !Q_stricmp( campaignName, "outcast" ) )
		{
			return CAMPAIGN_JEDI_OUTCAST;
		}
		if ( !Q_stricmp( campaignName, "outcast-ja" ) )
		{
			return CAMPAIGN_JEDI_OUTCAST_ON_ACADEMY;
		}
	}
	return CAMPAIGN_NOT_CHOSEN;
}

static campaignChoice_t CampaignSelect_AskPlayer( void )
{
	const SDL_MessageBoxButtonData buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,	CAMPAIGN_JEDI_ACADEMY,	"Jedi Academy" },
		{ 0,										CAMPAIGN_JEDI_OUTCAST,	"Jedi Outcast" },
		{ 0,										CAMPAIGN_JEDI_OUTCAST_ON_ACADEMY,	"Jedi Outcast (Academy engine)" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,	CAMPAIGN_QUIT,			"Quit" },
	};

	Uint32 dialogFlags = SDL_MESSAGEBOX_INFORMATION;
#if SDL_VERSION_ATLEAST( 2, 0, 12 )
	dialogFlags |= SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT;
#endif

	const SDL_MessageBoxData dialog =
	{
		dialogFlags,
		NULL,
		DIALOG_TITLE,
		"Which campaign would you like to play?\n\n"
		"Jedi Academy: create your own Jedi.\n"
		"Jedi Outcast: play as Kyle Katarn.\n"
		"Jedi Outcast (Academy engine): Kyle's campaign on Jedi Academy's engine (work in progress).",
		(int)ARRAY_LEN( buttons ),
		buttons,
		NULL
	};

	int pressedButton = CAMPAIGN_NOT_CHOSEN;
	if ( SDL_ShowMessageBox( &dialog, &pressedButton ) < 0 )
	{
		// The dialog could not be shown at all (for example no display is available).
		// Fall back to the original behaviour of this program.
		return CAMPAIGN_JEDI_ACADEMY;
	}

	if ( pressedButton == CAMPAIGN_JEDI_ACADEMY || pressedButton == CAMPAIGN_JEDI_OUTCAST
		|| pressedButton == CAMPAIGN_JEDI_OUTCAST_ON_ACADEMY )
	{
		return (campaignChoice_t)pressedButton;
	}
	return CAMPAIGN_QUIT; // Quit was pressed or the dialog was closed.
}

/*
==================
Starting Jedi Outcast
==================
*/

// Finds the Jedi Outcast engine: the configured path, then next to this program, then in the
// sibling JediOutcast folder that "cmake --install" creates. Returns false if none exist.
static bool CampaignSelect_FindJediOutcastEngine( const char *programDirectory, const campaignSettings_t *settings,
												  char *enginePath, size_t enginePathSize )
{
	if ( settings->jediOutcastEngine[0] )
	{
		Q_strncpyz( enginePath, settings->jediOutcastEngine, (int)enginePathSize );
		return CampaignSelect_FileExists( enginePath );
	}

	Com_sprintf( enginePath, (int)enginePathSize, "%s" PATH_SEPARATOR "%s", programDirectory, JEDI_OUTCAST_ENGINE_NAME );
	if ( CampaignSelect_FileExists( enginePath ) )
	{
		return true;
	}

	Com_sprintf( enginePath, (int)enginePathSize, "%s" PATH_SEPARATOR ".." PATH_SEPARATOR "JediOutcast" PATH_SEPARATOR "%s",
				 programDirectory, JEDI_OUTCAST_ENGINE_NAME );
	return CampaignSelect_FileExists( enginePath );
}

// Starts the Jedi Outcast engine as a separate process and returns without waiting for it.
//   fs_basepath points at the Jedi Outcast game data.
//   fs_cdpath points at the engine's own folder, which is where its game module
//   (OpenJK/jospgamex86_64.dll) is installed.
static bool CampaignSelect_StartJediOutcastEngine( const char *enginePath, const char *gameDataFolder )
{
	char engineFolder[MAX_OSPATH];
	Q_strncpyz( engineFolder, Sys_Dirname( enginePath ), sizeof( engineFolder ) );
	CampaignSelect_RemoveTrailingSeparators( engineFolder );

#ifdef _WIN32
	char commandLine[MAX_STRING_CHARS];
	Com_sprintf( commandLine, sizeof( commandLine ), "\"%s\" +set fs_basepath \"%s\" +set fs_cdpath \"%s\"",
				 enginePath, gameDataFolder, engineFolder );

	STARTUPINFOA startupInformation;
	PROCESS_INFORMATION processInformation;
	memset( &startupInformation, 0, sizeof( startupInformation ) );
	memset( &processInformation, 0, sizeof( processInformation ) );
	startupInformation.cb = sizeof( startupInformation );

	if ( !CreateProcessA( enginePath, commandLine, NULL, NULL, FALSE, 0, NULL, engineFolder,
						  &startupInformation, &processInformation ) )
	{
		return false;
	}

	// This program does not wait for the game, so the handles are not needed.
	CloseHandle( processInformation.hThread );
	CloseHandle( processInformation.hProcess );
	return true;
#else
	char basePathValue[MAX_OSPATH];
	char cdPathValue[MAX_OSPATH];
	Q_strncpyz( basePathValue, gameDataFolder, sizeof( basePathValue ) );
	Q_strncpyz( cdPathValue, engineFolder, sizeof( cdPathValue ) );

	char enginePathCopy[MAX_OSPATH];
	char setCommand[] = "+set";
	char basePathName[] = "fs_basepath";
	char cdPathName[] = "fs_cdpath";
	Q_strncpyz( enginePathCopy, enginePath, sizeof( enginePathCopy ) );

	char *engineArguments[] =
	{
		enginePathCopy,
		setCommand, basePathName, basePathValue,
		setCommand, cdPathName, cdPathValue,
		NULL
	};

	pid_t processId;
	return posix_spawn( &processId, enginePath, NULL, NULL, engineArguments, environ ) == 0;
#endif
}

// Returns true if campaigns.ini points at Jedi Outcast's game data, or shows why not and returns false.
static bool CampaignSelect_CheckJediOutcastGameData( const char *programDirectory, const campaignSettings_t *settings )
{
	char errorMessage[MAX_STRING_CHARS];

	if ( !settings->jediOutcastGameData[0] )
	{
		Com_sprintf( errorMessage, sizeof( errorMessage ),
					 "The Jedi Outcast install location has not been set.\n\n"
					 "Open %s" PATH_SEPARATOR "%s and set JediOutcastGameData to your Jedi Outcast GameData folder.",
					 programDirectory, SETTINGS_FILE_NAME );
		CampaignSelect_ShowError( errorMessage );
		return false;
	}

	if ( !CampaignSelect_IsGameDataFolder( settings->jediOutcastGameData ) )
	{
		Com_sprintf( errorMessage, sizeof( errorMessage ),
					 "Jedi Outcast game data was not found in:\n%s\n\n"
					 "JediOutcastGameData in %s must point at the folder that contains base" PATH_SEPARATOR "assets0.pk3.",
					 settings->jediOutcastGameData, SETTINGS_FILE_NAME );
		CampaignSelect_ShowError( errorMessage );
		return false;
	}
	return true;
}

static void CampaignSelect_LaunchJediOutcast( const char *programDirectory, const campaignSettings_t *settings )
{
	char errorMessage[MAX_STRING_CHARS];

	if ( !CampaignSelect_CheckJediOutcastGameData( programDirectory, settings ) )
	{
		return;
	}

	char enginePath[MAX_OSPATH];
	if ( !CampaignSelect_FindJediOutcastEngine( programDirectory, settings, enginePath, sizeof( enginePath ) ) )
	{
		Com_sprintf( errorMessage, sizeof( errorMessage ),
					 "The Jedi Outcast engine (%s) was not found.\n\n"
					 "Build it with -DBuildJK2SPEngine=ON -DBuildJK2SPGame=ON -DBuildJK2SPRdVanilla=ON, "
					 "or set JediOutcastEngine in %s.",
					 JEDI_OUTCAST_ENGINE_NAME, SETTINGS_FILE_NAME );
		CampaignSelect_ShowError( errorMessage );
		return;
	}

	if ( !CampaignSelect_StartJediOutcastEngine( enginePath, settings->jediOutcastGameData ) )
	{
		Com_sprintf( errorMessage, sizeof( errorMessage ), "The Jedi Outcast engine could not be started:\n%s", enginePath );
		CampaignSelect_ShowError( errorMessage );
	}
}

/*
==================
Continuing as Jedi Academy
==================
*/

// Returns true if any command line argument mentions the given cvar, so a value the player typed
// on the command line always takes priority over campaigns.ini.
static bool CampaignSelect_ArgumentsMention( int argc, char **argv, const char *cvarName )
{
	for ( int argumentIndex = 1; argumentIndex < argc; argumentIndex++ )
	{
		if ( !Q_stricmp( argv[argumentIndex], cvarName ) )
		{
			return true;
		}
	}
	return false;
}

// Builds the engine commands that point Jedi Academy at its game data and game module.
// Returns false (after showing an error) if the configured game data folder is wrong.
static bool CampaignSelect_BuildJediAcademyCommandLine( const char *programDirectory, const campaignSettings_t *settings,
														int argc, char **argv,
														char *extraCommandLine, size_t extraCommandLineSize )
{
	extraCommandLine[0] = '\0';

	// No setting means the program is installed inside the Jedi Academy GameData folder, which
	// is the engine's default, so nothing needs to be added.
	if ( !settings->jediAcademyGameData[0] || CampaignSelect_ArgumentsMention( argc, argv, "fs_basepath" ) )
	{
		return true;
	}

	if ( !CampaignSelect_IsGameDataFolder( settings->jediAcademyGameData ) )
	{
		char errorMessage[MAX_STRING_CHARS];
		Com_sprintf( errorMessage, sizeof( errorMessage ),
					 "Jedi Academy game data was not found in:\n%s\n\n"
					 "JediAcademyGameData in %s must point at the folder that contains base" PATH_SEPARATOR "assets0.pk3.",
					 settings->jediAcademyGameData, SETTINGS_FILE_NAME );
		CampaignSelect_ShowError( errorMessage );
		return false;
	}

	Com_sprintf( extraCommandLine, (int)extraCommandLineSize, "+set fs_basepath \"%s\" ", settings->jediAcademyGameData );

	// The game module (OpenJK/jagamex86_64.dll) sits next to this program rather than in the
	// game data folder, so let the engine search this folder for it as well.
	if ( !CampaignSelect_ArgumentsMention( argc, argv, "fs_cdpath" ) )
	{
		char programFolder[MAX_OSPATH];
		Q_strncpyz( programFolder, programDirectory, sizeof( programFolder ) );
		CampaignSelect_RemoveTrailingSeparators( programFolder );

		char cdPathCommand[MAX_OSPATH + 32];
		Com_sprintf( cdPathCommand, sizeof( cdPathCommand ), "+set fs_cdpath \"%s\" ", programFolder );
		Q_strcat( extraCommandLine, (int)extraCommandLineSize, cdPathCommand );
	}
	return true;
}

/*
==================
Jedi Outcast on the Academy engine
==================
*/

// The Jedi Academy command line, plus Jedi Outcast's game data mounted beneath Jedi Academy's
// (fs_outcastpath, which also tells the game code which campaign is played) and a folder of its own
// for saves and settings. Returns false (after showing an error) if either game's data is missing.
static bool CampaignSelect_BuildOutcastOnAcademyCommandLine( const char *programDirectory, const campaignSettings_t *settings,
															 int argc, char **argv,
															 char *extraCommandLine, size_t extraCommandLineSize )
{
	if ( !CampaignSelect_CheckJediOutcastGameData( programDirectory, settings )
		|| !CampaignSelect_BuildJediAcademyCommandLine( programDirectory, settings, argc, argv,
														extraCommandLine, extraCommandLineSize ) )
	{
		return false;
	}

	char outcastCommands[MAX_OSPATH + 64];
	if ( !CampaignSelect_ArgumentsMention( argc, argv, "fs_outcastpath" ) )
	{
		Com_sprintf( outcastCommands, sizeof( outcastCommands ), "+set fs_outcastpath \"%s\" ", settings->jediOutcastGameData );
		Q_strcat( extraCommandLine, (int)extraCommandLineSize, outcastCommands );
	}
	if ( !CampaignSelect_ArgumentsMention( argc, argv, "fs_game" ) )
	{
		Com_sprintf( outcastCommands, sizeof( outcastCommands ), "+set fs_game %s ", OUTCAST_ON_ACADEMY_GAME_FOLDER );
		Q_strcat( extraCommandLine, (int)extraCommandLineSize, outcastCommands );
	}
	return true;
}

/*
==================
Sys_CampaignSelect_Run
==================
*/
bool Sys_CampaignSelect_Run( const char *programDirectory, int argc, char **argv,
							 char *extraCommandLine, size_t extraCommandLineSize )
{
	extraCommandLine[0] = '\0';

	campaignSettings_t settings;
	CampaignSelect_LoadSettings( programDirectory, &settings );

	campaignChoice_t choice = CampaignSelect_ChoiceFromArguments( argc, argv );
	if ( choice == CAMPAIGN_NOT_CHOSEN )
	{
		choice = CampaignSelect_AskPlayer();
	}

	switch ( choice )
	{
	case CAMPAIGN_JEDI_ACADEMY:
		return CampaignSelect_BuildJediAcademyCommandLine( programDirectory, &settings, argc, argv,
														   extraCommandLine, extraCommandLineSize );

	case CAMPAIGN_JEDI_OUTCAST:
		CampaignSelect_LaunchJediOutcast( programDirectory, &settings );
		return false;

	case CAMPAIGN_JEDI_OUTCAST_ON_ACADEMY:
		return CampaignSelect_BuildOutcastOnAcademyCommandLine( programDirectory, &settings, argc, argv,
																extraCommandLine, extraCommandLineSize );

	default:
		return false;
	}
}

#endif // defined(_JK2EXE) && !defined(JK2_MODE)
