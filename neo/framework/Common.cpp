/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012 Robert Beckebans

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "precompiled.h"

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
#include "../sys/vita/vita_ui.h"
#define VITA_PHASE(x) do { VITA_MARK("init: " x); Vita_Checkpoint("init: " x); Vita_LogMemory(x); } while (0)
#else
#define VITA_PHASE(x)
#endif
#pragma hdrstop

#include "Common_local.h"

#include "ConsoleHistory.h"
#include "../renderer/AutoRenderBink.h"

#include "../sound/sound.h"

// RB begin
#if defined(USE_DOOMCLASSIC)
#include "../../doomclassic/doom/doomlib.h"
#include "../../doomclassic/doom/d_event.h"
#include "../../doomclassic/doom/d_main.h"
#endif
// RB end

#include "../sys/sys_savegame.h"

#if defined(_DEBUG)
#define BUILD_DEBUG "-debug"
#else
#define BUILD_DEBUG ""
#endif

struct version_s {
    version_s()
    {
        sprintf(string, "%s.%d%s %s %s %s", ENGINE_VERSION, BUILD_NUMBER, BUILD_DEBUG, BUILD_STRING, __DATE__, __TIME__);
    }
    char string[256];
} version;

/*
Vita full quality (vita_cinematicFullQuality): while a cinematic plays every
fidelity cut is off - animation at 15 Hz (vita_animLOD), half-rate AI,
particle thinning, the animated-model light limit, small-light drops and the
light cap, world screens and HUD at a lower rate, the screen copy reuse.
Cutscenes showed them: a man's suitcase moving at 15 Hz, parts of characters
going dark as their lights were cut and brought back. Set by the game every
frame (idGameLocal::RunFrame).
*/
bool vita_fullQuality = false;

idCVar com_version("si_version", version.string, CVAR_SYSTEM | CVAR_ROM | CVAR_SERVERINFO, "engine version");
idCVar com_forceGenericSIMD("com_forceGenericSIMD", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "force generic platform independent SIMD");

#ifdef ID_RETAIL
idCVar com_allowConsole("com_allowConsole", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_INIT, "allow toggling console with the tilde key");
#else
idCVar com_allowConsole("com_allowConsole", "1", CVAR_BOOL | CVAR_SYSTEM | CVAR_INIT, "allow toggling console with the tilde key");
#endif

idCVar com_developer("developer", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "developer mode");
idCVar com_speeds("com_speeds", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "show engine timings");
// DG: support "com_showFPS 2" for fps-only view like in classic doom3 => make it CVAR_INTEGER
idCVar com_showFPS("com_showFPS", "0", CVAR_INTEGER | CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT, "show frames rendered per second. 0: off 1: default bfg values, 2: only show FPS (classic view)");
// DG end
idCVar com_showMemoryUsage("com_showMemoryUsage", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "show total and per frame memory usage");
idCVar com_updateLoadSize("com_updateLoadSize", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "update the load size after loading a map");

idCVar com_productionMode("com_productionMode", "0", CVAR_SYSTEM | CVAR_BOOL, "0 - no special behavior, 1 - building a production build, 2 - running a production build");

idCVar com_japaneseCensorship("com_japaneseCensorship", "0", CVAR_NOCHEAT, "Enable Japanese censorship");

#if defined(__vita__)
// The preload walks _common.preload and resident-loads 362 images and every
// common sound up front. On a console this small that is the difference
// between reaching the menu and running the GPU pools dry: the shell's own 30
// images already take vitaGL from 13.8 MB of free VRAM down to 0.3 MB, and the
// preload then fails allocating a 1.2 MB sound sample. Nothing needs it - every
// asset still loads on demand the first time it is referenced - so the cost is
// hitching, not correctness, and it also saves ~18 s of boot.
idCVar preload_CommonAssets("preload_CommonAssets", "0", CVAR_SYSTEM | CVAR_BOOL, "preload common assets");
#else
idCVar preload_CommonAssets("preload_CommonAssets", "1", CVAR_SYSTEM | CVAR_BOOL, "preload common assets");
#endif

#if defined(__vita__)
// Set in autoexec.cfg to boot straight into a map, e.g. "game/mp/d3xpdm2".
// Consumed by AddStartupCommands(), which is where the engine defers
// command-line startup commands to - see the note there for why a bare devmap
// in autoexec.cfg cannot work.
// Not CVAR_INIT: despite the wording of the rejection message, that flag means
// command line only, and an autoexec.cfg exec does not qualify - the cvar stayed
// empty and no map was ever queued.
idCVar vita_startupMap("vita_startupMap", "", CVAR_SYSTEM, "Vita: devmap this map once startup completes");
#endif

idCVar net_inviteOnly("net_inviteOnly", "1", CVAR_BOOL | CVAR_ARCHIVE, "whether or not the private server you create allows friends to join or invite only");

// DG: add cvar for pause
idCVar com_pause("com_pause", "0", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "set to 1 to pause game, to 0 to unpause again");
// DG end

extern idCVar g_demoMode;

idCVar com_engineHz("com_engineHz", "60", CVAR_FLOAT | CVAR_ARCHIVE, "Frames per second the engine runs at", 10.0f, 1024.0f);
#if defined(__vita__)
// DOOM 3's game logic rate in maps on the Vita (com_engineHz is not used
// there): 30 is one game tic per 30 fps frame - half the AI, script, physics
// and animation work of id's 60, which a fight could not fit in a frame. The
// title screen and menus keep 60 (their animation follows the game tics).
// The game is built for any rate (BFG's GAME_FPS for the scripts, physics
// and animation in milliseconds); VitaPhysCheck.h measures that it plays the
// same. Applied at the next map load.
idCVar vita_gameHz("vita_gameHz", "30", CVAR_ARCHIVE | CVAR_INTEGER, "Vita: DOOM 3's game tics per second in maps (30 or 60; menus are always 60)", 30, 60);
idCVar vita_tuningVersion("vita_tuningVersion", "0", CVAR_ARCHIVE | CVAR_INTEGER, "Vita: the performance settings version the config was saved with (older ones are reset at startup)");
#endif
float com_engineHz_latched = 60.0f; // Latched version of cvar, updated between map loads
int64 com_engineHz_numerator = 100LL * 1000LL;
int64 com_engineHz_denominator = 100LL * 60LL;

// RB begin
#if defined(_WIN32)
HWND com_hwndMsg = NULL;
#endif
// RB end

#ifdef __DOOM_DLL__
idGame* game = NULL;
idGameEdit* gameEdit = NULL;
#endif

idCommonLocal commonLocal;
idCommon* common = &commonLocal;

// RB: defaulted this to 1 because we don't have a sound for the intro .bik video
idCVar com_skipIntroVideos("com_skipIntroVideos", "1", CVAR_BOOL, "skips intro videos");

// For doom classic
struct Globals;

/*
==================
idCommonLocal::idCommonLocal
==================
*/
idCommonLocal::idCommonLocal()
    : readSnapshotIndex(0)
    , writeSnapshotIndex(0)
    , optimalPCTBuffer(0.5f)
    , optimalTimeBuffered(0.0f)
    , optimalTimeBufferedWindow(0.0f)
    , lastPacifierSessionTime(0)
    , lastPacifierGuiTime(0)
    , lastPacifierDialogState(false)
    , showShellRequested(false)
// RB begin
#if defined(USE_DOOMCLASSIC)
    , currentGame(DOOM3_BFG)
    , idealCurrentGame(DOOM3_BFG)
    , doomClassicMaterial(NULL)
#endif
// RB end
{

    snapCurrent.localTime = -1;
    snapPrevious.localTime = -1;
    snapCurrent.serverTime = -1;
    snapPrevious.serverTime = -1;
    snapTimeBuffered = 0.0f;
    effectiveSnapRate = 0.0f;
    totalBufferedTime = 0;
    totalRecvTime = 0;

    com_fullyInitialized = false;
    com_refreshOnPrint = false;
    com_errorEntered = ERP_NONE;
    com_shuttingDown = false;
    com_isJapaneseSKU = false;

    logFile = NULL;

    strcpy(errorMessage, "");

    rd_buffer = NULL;
    rd_buffersize = 0;
    rd_flush = NULL;

    gameDLL = 0;

    loadGUI = NULL;
    nextLoadTip = 0;
    isHellMap = false;
    wipeForced = false;
    defaultLoadscreen = false;

    menuSoundWorld = NULL;

    insideUpdateScreen = false;
    insideExecuteMapChange = false;

    mapSpawnData.savegameFile = NULL;
#if defined(__vita__)
    vitaLastSaveBytes = 0;
    vitaLastStringsBytes = 0;
#endif

    currentMapName.Clear();
    aviDemoShortName.Clear();

    renderWorld = NULL;
    soundWorld = NULL;
    menuSoundWorld = NULL;
    readDemo = NULL;
    writeDemo = NULL;

    gameFrame = 0;
    gameTimeResidual = 0;
    syncNextGameFrame = true;
    mapSpawned = false;
    aviCaptureMode = false;
    timeDemo = TD_NO;

    nextSnapshotSendTime = 0;
    nextUsercmdSendTime = 0;

    clientPrediction = 0;

    saveFile = NULL;
    stringsFile = NULL;

    ClearWipe();
}

/*
==================
idCommonLocal::Quit
==================
*/
void idCommonLocal::Quit()
{

    // don't try to shutdown if we are in a recursive error
    if (!com_errorEntered) {
        Shutdown();
    }
    Sys_Quit();
}

/*
============================================================================

COMMAND LINE FUNCTIONS

+ characters separate the commandLine string into multiple console
command lines.

All of these are valid:

doom +set test blah +map test
doom set test blah+map test
doom set test blah + map test

============================================================================
*/

#define MAX_CONSOLE_LINES 32
int com_numConsoleLines;
idCmdArgs com_consoleLines[MAX_CONSOLE_LINES];

/*
==================
idCommonLocal::ParseCommandLine
==================
*/
void idCommonLocal::ParseCommandLine(int argc, const char* const* argv)
{
    int i, current_count;

    com_numConsoleLines = 0;
    current_count = 0;
    // API says no program path
    for (i = 0; i < argc; i++) {
        if (idStr::Icmp(argv[i], "+connect_lobby") == 0) {
            // Handle Steam bootable invites.

            // RB begin
#if defined(_WIN32)
            session->HandleBootableInvite(_atoi64(argv[i + 1]));
#else
            session->HandleBootableInvite(atol(argv[i + 1]));
#endif
            // RB end
        } else if (argv[i][0] == '+') {
            com_numConsoleLines++;
            com_consoleLines[com_numConsoleLines - 1].AppendArg(argv[i] + 1);
        } else {
            if (!com_numConsoleLines) {
                com_numConsoleLines++;
            }
            com_consoleLines[com_numConsoleLines - 1].AppendArg(argv[i]);
        }
    }
}

/*
==================
idCommonLocal::SafeMode

Check for "safe" on the command line, which will
skip loading of config file (DoomConfig.cfg)
==================
*/
bool idCommonLocal::SafeMode()
{
    int i;

    for (i = 0; i < com_numConsoleLines; i++) {
        if (!idStr::Icmp(com_consoleLines[i].Argv(0), "safe")
            || !idStr::Icmp(com_consoleLines[i].Argv(0), "cvar_restart")) {
            com_consoleLines[i].Clear();
            return true;
        }
    }
    return false;
}

/*
==================
idCommonLocal::StartupVariable

Searches for command line parameters that are set commands.
If match is not NULL, only that cvar will be looked for.
That is necessary because cddir and basedir need to be set
before the filesystem is started, but all other sets should
be after execing the config and default.
==================
*/
void idCommonLocal::StartupVariable(const char* match)
{
    int i = 0;
    while (i < com_numConsoleLines) {
        if (strcmp(com_consoleLines[i].Argv(0), "set") != 0) {
            i++;
            continue;
        }
        const char* s = com_consoleLines[i].Argv(1);

        if (!match || !idStr::Icmp(s, match)) {
            cvarSystem->SetCVarString(s, com_consoleLines[i].Argv(2));
        }
        i++;
    }
}

/*
==================
idCommonLocal::AddStartupCommands

Adds command line parameters as script statements
Commands are separated by + signs

Returns true if any late commands were added, which
will keep the demoloop from immediately starting
==================
*/
void idCommonLocal::AddStartupCommands()
{
    // quote every token, so args with semicolons can work
    for (int i = 0; i < com_numConsoleLines; i++) {
        if (!com_consoleLines[i].Argc()) {
            continue;
        }
        // directly as tokenized so nothing gets screwed
        cmdSystem->BufferCommandArgs(CMD_EXEC_APPEND, com_consoleLines[i]);
    }

#if defined(__vita__)
    // There is no command line to pass "+devmap x" on, and putting devmap in
    // autoexec.cfg does not work: that file is exec'd from within Init() long
    // before R_InitOpenGL, so the map load starts with no renderer and takes
    // the process down before the first log line after "execing autoexec.cfg".
    //
    // This is the point the engine itself defers startup commands to, so set
    // vita_startupMap in autoexec.cfg instead and the devmap is queued here,
    // exactly as a command-line "+devmap" would have been.
    if (vita_startupMap.GetString()[0] != '\0') {
        Printf("vita_startupMap: queueing devmap %s\n", vita_startupMap.GetString());
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("devmap %s\n", vita_startupMap.GetString()));
    }
#endif
}

/*
==================
idCommonLocal::WriteConfigToFile
==================
*/
void idCommonLocal::WriteConfigToFile(const char* filename)
{
    idFile* f = fileSystem->OpenFileWrite(filename);
    if (!f) {
        Printf("Couldn't write %s.\n", filename);
        return;
    }

    idKeyInput::WriteBindings(f);
    cvarSystem->WriteFlaggedVariables(CVAR_ARCHIVE, "set", f);
    fileSystem->CloseFile(f);
}

/*
===============
idCommonLocal::WriteConfiguration

Writes key bindings and archived cvars to config file if modified
===============
*/
void idCommonLocal::WriteConfiguration()
{
    // if we are quiting without fully initializing, make sure
    // we don't write out anything
    if (!com_fullyInitialized) {
        return;
    }

    if (!(cvarSystem->GetModifiedFlags() & CVAR_ARCHIVE)) {
        return;
    }
    cvarSystem->ClearModifiedFlags(CVAR_ARCHIVE);

    // save to the profile
    idLocalUser* user = session->GetSignInManager().GetMasterLocalUser();
    if (user != NULL) {
        user->SaveProfileSettings();
    }

#ifdef CONFIG_FILE
    // disable printing out the "Writing to:" message
    bool developer = com_developer.GetBool();
    com_developer.SetBool(false);

    WriteConfigToFile(CONFIG_FILE);

    // restore the developer cvar
    com_developer.SetBool(developer);
#endif
}

/*
===============
KeysFromBinding()
Returns the key bound to the command
===============
*/
const char* idCommonLocal::KeysFromBinding(const char* bind)
{
    return idKeyInput::KeysFromBinding(bind);
}

/*
===============
BindingFromKey()
Returns the binding bound to key
===============
*/
const char* idCommonLocal::BindingFromKey(const char* key)
{
    return idKeyInput::BindingFromKey(key);
}

/*
===============
ButtonState()
Returns the state of the button
===============
*/
int idCommonLocal::ButtonState(int key)
{
    return usercmdGen->ButtonState(key);
}

/*
===============
ButtonState()
Returns the state of the key
===============
*/
int idCommonLocal::KeyState(int key)
{
    return usercmdGen->KeyState(key);
}

/*
============
idCmdSystemLocal::PrintMemInfo_f

This prints out memory debugging data
============
*/
#if defined(ID_TEST_CLASSIC_MT)
// PC test builds: start DOOM straight from the console (the threaded classic renderer test)
CONSOLE_COMMAND(testClassic, "starts classic DOOM (test builds)", NULL)
{
    common->SwitchToGame(DOOM_CLASSIC);
}
#endif

CONSOLE_COMMAND(printMemInfo, "prints memory debugging data", NULL)
{
    MemInfo_t mi;
    memset(&mi, 0, sizeof(mi));
    mi.filebase = commonLocal.GetCurrentMapName();

    renderSystem->PrintMemInfo(&mi); // textures and models
    soundSystem->PrintMemInfo(&mi);  // sounds

    common->Printf(" Used image memory: %s bytes\n", idStr::FormatNumber(mi.imageAssetsTotal).c_str());
    mi.assetTotals += mi.imageAssetsTotal;

    common->Printf(" Used model memory: %s bytes\n", idStr::FormatNumber(mi.modelAssetsTotal).c_str());
    mi.assetTotals += mi.modelAssetsTotal;

    common->Printf(" Used sound memory: %s bytes\n", idStr::FormatNumber(mi.soundAssetsTotal).c_str());
    mi.assetTotals += mi.soundAssetsTotal;

    common->Printf(" Used asset memory: %s bytes\n", idStr::FormatNumber(mi.assetTotals).c_str());

    // write overview file
    idFile* f;

    f = fileSystem->OpenFileAppend("maps/printmeminfo.txt");
    if (!f) {
        return;
    }

    f->Printf("total(%s ) image(%s ) model(%s ) sound(%s ): %s\n", idStr::FormatNumber(mi.assetTotals).c_str(), idStr::FormatNumber(mi.imageAssetsTotal).c_str(),
        idStr::FormatNumber(mi.modelAssetsTotal).c_str(), idStr::FormatNumber(mi.soundAssetsTotal).c_str(), mi.filebase.c_str());

    fileSystem->CloseFile(f);
}

/*
==================
Com_Error_f

Just throw a fatal error to test error shutdown procedures.
==================
*/
CONSOLE_COMMAND(error, "causes an error", NULL)
{
    if (!com_developer.GetBool()) {
        commonLocal.Printf("error may only be used in developer mode\n");
        return;
    }

    if (args.Argc() > 1) {
        commonLocal.FatalError("Testing fatal error");
    } else {
        commonLocal.Error("Testing drop error");
    }
}

/*
==================
Com_Freeze_f

Just freeze in place for a given number of seconds to test error recovery.
==================
*/
CONSOLE_COMMAND(freeze, "freezes the game for a number of seconds", NULL)
{
    float s;
    int start, now;

    if (args.Argc() != 2) {
        commonLocal.Printf("freeze <seconds>\n");
        return;
    }

    if (!com_developer.GetBool()) {
        commonLocal.Printf("freeze may only be used in developer mode\n");
        return;
    }

    s = atof(args.Argv(1));

    start = eventLoop->Milliseconds();

    while (1) {
        now = eventLoop->Milliseconds();
        if ((now - start) * 0.001f > s) {
            break;
        }
    }
}

/*
=================
Com_Crash_f

A way to force a bus error for development reasons
=================
*/
CONSOLE_COMMAND(crash, "causes a crash", NULL)
{
    if (!com_developer.GetBool()) {
        commonLocal.Printf("crash may only be used in developer mode\n");
        return;
    }
#ifdef __GNUC__
    __builtin_trap();
#else
    *(int*)0 = 0x12345678;
#endif
}

/*
=================
Com_Quit_f
=================
*/
CONSOLE_COMMAND_SHIP(quit, "quits the game", NULL)
{
    commonLocal.Quit();
}
CONSOLE_COMMAND_SHIP(exit, "exits the game", NULL)
{
    commonLocal.Quit();
}

/*
===============
Com_WriteConfig_f

Write the config file to a specific name
===============
*/
CONSOLE_COMMAND(writeConfig, "writes a config file", NULL)
{
    idStr filename;

    if (args.Argc() != 2) {
        commonLocal.Printf("Usage: writeconfig <filename>\n");
        return;
    }

    filename = args.Argv(1);
    filename.DefaultFileExtension(".cfg");
    commonLocal.Printf("Writing %s.\n", filename.c_str());
    commonLocal.WriteConfigToFile(filename);
}

/*
========================
idCommonLocal::CheckStartupStorageRequirements
========================
*/
void idCommonLocal::CheckStartupStorageRequirements()
{
    // RB: disabled savegame and profile storage checks, because it fails sometimes without any clear reason
#if 0
	int64 availableSpace = 0;
	
	// ------------------------------------------------------------------------
	// Savegame and Profile required storage
	// ------------------------------------------------------------------------
	{
		// Make sure the save path exists in case it was deleted.
		// If the path cannot be created we can safely assume there is no
		// free space because in that case nothing can be saved anyway.
		const char* savepath = cvarSystem->GetCVarString( "fs_savepath" );
		idStr directory = savepath;
		//idStr directory = fs_savepath.GetString();
		directory += "\\";	// so it doesn't think the last part is a file and ignores in the directory creation
		fileSystem->CreateOSPath( directory );
		
		// Get the free space on the save path.
		availableSpace = Sys_GetDriveFreeSpaceInBytes( savepath );
		
		// If free space fails then get space on drive as a fall back
		// (the directory will be created later anyway)
		if( availableSpace <= 1 )
		{
			idStr savePath( savepath );
			if( savePath.Length() >= 3 )
			{
				if( savePath[ 1 ] == ':' && savePath[ 2 ] == '\\' &&
						( ( savePath[ 0 ] >= 'A' && savePath[ 0 ] <= 'Z' ) ||
						  ( savePath[ 0 ] >= 'a' && savePath[ 0 ] <= 'z' ) ) )
				{
					savePath = savePath.Left( 3 );
					availableSpace = Sys_GetDriveFreeSpaceInBytes( savePath );
				}
			}
		}
	}
	
	const int MIN_SAVE_STORAGE_PROFILE		= 1024 * 1024;
	const int MIN_SAVE_STORAGE_SAVEGAME		= MIN_SAVEGAME_SIZE_BYTES;
	
	uint64 requiredSizeBytes = MIN_SAVE_STORAGE_SAVEGAME + MIN_SAVE_STORAGE_PROFILE;
	
	idLib::Printf( "requiredSizeBytes: %lld\n", requiredSizeBytes );
	
	if( ( int64 )( requiredSizeBytes - availableSpace ) > 0 )
	{
		class idSWFScriptFunction_Continue : public idSWFScriptFunction_RefCounted
		{
		public:
			virtual ~idSWFScriptFunction_Continue() {}
			idSWFScriptVar Call( idSWFScriptObject* thisObject, const idSWFParmList& parms )
			{
				common->Dialog().ClearDialog( GDM_INSUFFICENT_STORAGE_SPACE );
				common->Quit();
				return idSWFScriptVar();
			}
		};
		
		idStaticList< idSWFScriptFunction*, 4 > callbacks;
		idStaticList< idStrId, 4 > optionText;
		callbacks.Append( new( TAG_SWF ) idSWFScriptFunction_Continue() );
		optionText.Append( idStrId( "#STR_SWF_ACCEPT" ) );
		
		// build custom space required string
		// #str_dlg_space_required ~= "There is insufficient storage available.  Please free %s and try again."
		idStr format = idStrId( "#str_dlg_startup_insufficient_storage" ).GetLocalizedString();
		idStr size;
		if( requiredSizeBytes > ( 1024 * 1024 ) )
		{
			size = va( "%.1f MB", ( float )requiredSizeBytes / ( 1024.0f * 1024.0f ) + 0.1f );	// +0.1 to avoid truncation
		}
		else
		{
			size = va( "%.1f KB", ( float )requiredSizeBytes / 1024.0f + 0.1f );
		}
		idStr msg = va( format.c_str(), size.c_str() );
		
		common->Dialog().AddDynamicDialog( GDM_INSUFFICENT_STORAGE_SPACE, callbacks, optionText, true, msg );
	}
#endif
    // RB end

    session->GetAchievementSystem().Start();
}

/*
===============
idCommonLocal::JapaneseCensorship
===============
*/
bool idCommonLocal::JapaneseCensorship() const
{
    return com_japaneseCensorship.GetBool() || com_isJapaneseSKU;
}

/*
===============
idCommonLocal::FilterLangList
===============
*/
void idCommonLocal::FilterLangList(idStrList* list, idStr lang)
{

    idStr temp;
    for (int i = 0; i < list->Num(); i++) {
        temp = (*list)[i];
        temp = temp.Right(temp.Length() - strlen("strings/"));
        temp = temp.Left(lang.Length());
        if (idStr::Icmp(temp, lang) != 0) {
            list->RemoveIndex(i);
            i--;
        }
    }
}

/*
===============
idCommonLocal::InitLanguageDict
===============
*/
extern idCVar sys_lang;
void idCommonLocal::InitLanguageDict()
{
    idStr fileName;

    // D3XP: Instead of just loading a single lang file for each language
    // we are going to load all files that begin with the language name
    // similar to the way pak files work. So you can place english001.lang
    // to add new strings to the english language dictionary
    idFileList* langFiles;
    langFiles = fileSystem->ListFilesTree("strings", ".lang", true);

    idStrList langList = langFiles->GetList();

    // Loop through the list and filter
    idStrList currentLangList = langList;
    FilterLangList(&currentLangList, sys_lang.GetString());

    if (currentLangList.Num() == 0) {
        // reset to english and try to load again
        sys_lang.SetString(ID_LANG_ENGLISH);
        currentLangList = langList;
        FilterLangList(&currentLangList, sys_lang.GetString());
    }

    idLocalization::ClearDictionary();
    for (int i = 0; i < currentLangList.Num(); i++) {
        // common->Printf("%s\n", currentLangList[i].c_str());
        const byte* buffer = NULL;
        int len = fileSystem->ReadFile(currentLangList[i], (void**)&buffer);
        if (len <= 0) {
            assert(false && "couldn't read the language dict file");
            break;
        }
        idLocalization::LoadDictionary(buffer, len, currentLangList[i]);
        fileSystem->FreeFile((void*)buffer);
    }

    fileSystem->FreeFileList(langFiles);

    // the port's own text, in the same language (framework/VitaStrings.cpp)
    extern void Vita_AddPortStrings(const char* lang);
    Vita_AddPortStrings(sys_lang.GetString());
}

/*
=================
ReloadLanguage_f
=================
*/
CONSOLE_COMMAND(reloadLanguage, "reload language dict", NULL)
{
    commonLocal.InitLanguageDict();
}

#include "../renderer/Image.h"

/*
=================
Com_StartBuild_f
=================
*/
CONSOLE_COMMAND(startBuild, "prepares to make a build", NULL)
{
    globalImages->StartBuild();
}

/*
=================
Com_FinishBuild_f
=================
*/
CONSOLE_COMMAND(finishBuild, "finishes the build process", NULL)
{
    if (game) {
        game->CacheDictionaryMedia(NULL);
    }
    globalImages->FinishBuild((args.Argc() > 1));
}

/*
=================
idCommonLocal::RenderSplash
=================
*/
void idCommonLocal::RenderSplash()
{
    const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
    const float sysHeight = renderSystem->GetHeight();
    const float sysAspect = sysWidth / sysHeight;
    const float splashAspect = 16.0f / 9.0f;
    const float adjustment = sysAspect / splashAspect;
    const float barHeight = (adjustment >= 1.0f) ? 0.0f : (1.0f - adjustment) * (float)renderSystem->GetVirtualHeight() * 0.25f;
    const float barWidth = (adjustment <= 1.0f) ? 0.0f : (adjustment - 1.0f) * (float)renderSystem->GetVirtualWidth() * 0.25f;
    if (barHeight > 0.0f) {
        renderSystem->SetColor(colorBlack);
        renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), barHeight, 0, 0, 1, 1, whiteMaterial);
        renderSystem->DrawStretchPic(0, renderSystem->GetVirtualHeight() - barHeight, renderSystem->GetVirtualWidth(), barHeight, 0, 0, 1, 1, whiteMaterial);
    }
    if (barWidth > 0.0f) {
        renderSystem->SetColor(colorBlack);
        renderSystem->DrawStretchPic(0, 0, barWidth, renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial);
        renderSystem->DrawStretchPic(renderSystem->GetVirtualWidth() - barWidth, 0, barWidth, renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial);
    }
    renderSystem->SetColor4(1, 1, 1, 1);
    renderSystem->DrawStretchPic(barWidth, barHeight, renderSystem->GetVirtualWidth() - barWidth * 2.0f, renderSystem->GetVirtualHeight() - barHeight * 2.0f, 0, 0, 1, 1, splashScreen);

    // This build plays no Bink video (the Vita decodes H.264 in hardware), so
    // the legal screen goes without the Bink Video logo and the "Uses Bink
    // Video ... RAD Game Tools, Inc." sentence: the screen is redrawn from
    // pieces of its own image - the logo row up to the Bink logo, then every
    // remaining word, reflowed into a justified block (equal edges, even word
    // spacing, the last line at least half full). The rectangles, in the
    // 1280x720 image's pixels ({ src x0, y0, x1, y1, dst x, y }), come from
    // tools/vita/legal_reflow.py, which measured the words of the three
    // images (english, french, figs) and laid them out.
    if (splashScreen != NULL) {
    // english: 91 rects
    static const short legal_english[][6] = {
        { 73, 292, 640, 392, 73, 292 }, { 78, 409, 175, 436, 78, 409 }, { 178, 409, 201, 436, 183, 409 }, { 205, 409, 259, 436, 214, 409 },
        { 263, 409, 368, 436, 276, 409 }, { 374, 409, 396, 436, 389, 409 }, { 400, 409, 516, 436, 419, 409 }, { 521, 409, 541, 436, 544, 409 },
        { 546, 409, 643, 436, 572, 409 }, { 648, 409, 699, 436, 677, 409 }, { 704, 409, 720, 436, 736, 409 }, { 724, 409, 819, 436, 760, 409 },
        { 824, 409, 891, 436, 863, 409 }, { 895, 409, 1001, 436, 938, 409 }, { 1007, 409, 1115, 436, 78, 438 }, { 79, 438, 182, 465, 197, 438 },
        { 187, 438, 300, 465, 312, 438 }, { 304, 438, 400, 465, 436, 438 }, { 404, 438, 445, 465, 544, 438 }, { 451, 438, 526, 465, 596, 438 },
        { 531, 438, 590, 465, 683, 438 }, { 595, 438, 632, 465, 753, 438 }, { 636, 438, 744, 465, 802, 438 }, { 749, 438, 872, 465, 921, 438 },
        { 876, 438, 1026, 465, 78, 467 }, { 1030, 438, 1152, 465, 234, 467 }, { 79, 467, 146, 492, 362, 467 }, { 151, 467, 190, 492, 435, 467 },
        { 196, 467, 216, 492, 480, 467 }, { 220, 467, 257, 492, 506, 467 }, { 262, 467, 309, 492, 548, 467 }, { 314, 467, 386, 492, 601, 467 },
        { 389, 467, 447, 492, 679, 467 }, { 450, 467, 556, 492, 743, 467 }, { 562, 467, 644, 492, 855, 467 }, { 650, 467, 725, 492, 943, 467 },
        { 732, 467, 752, 492, 1024, 467 }, { 758, 467, 833, 492, 78, 496 }, { 839, 467, 860, 492, 160, 496 }, { 866, 467, 1003, 492, 187, 496 },
        { 1008, 467, 1032, 492, 331, 496 }, { 1035, 467, 1081, 492, 362, 496 }, { 79, 496, 155, 523, 414, 496 }, { 160, 496, 175, 523, 497, 496 },
        { 180, 496, 231, 523, 519, 496 }, { 236, 496, 317, 523, 576, 496 }, { 323, 496, 349, 523, 664, 496 }, { 355, 496, 375, 523, 697, 496 },
        { 380, 496, 482, 523, 723, 496 }, { 488, 496, 508, 523, 832, 496 }, { 513, 496, 568, 523, 859, 496 }, { 573, 496, 615, 523, 920, 496 },
        { 620, 496, 695, 523, 969, 496 }, { 700, 496, 760, 523, 78, 525 }, { 764, 496, 801, 523, 143, 525 }, { 806, 496, 914, 523, 185, 525 },
        { 918, 496, 1041, 523, 298, 525 }, { 1045, 496, 1070, 523, 426, 525 }, { 78, 524, 201, 551, 456, 524 }, { 205, 524, 229, 551, 584, 524 },
        { 232, 524, 252, 551, 613, 524 }, { 257, 524, 354, 551, 638, 524 }, { 359, 524, 405, 551, 740, 524 }, { 410, 524, 430, 551, 791, 524 },
        { 434, 524, 470, 551, 816, 524 }, { 475, 524, 522, 551, 857, 524 }, { 528, 524, 599, 551, 909, 524 }, { 602, 524, 661, 551, 985, 524 },
        { 664, 524, 769, 551, 78, 553 }, { 782, 524, 838, 551, 191, 553 }, { 843, 524, 863, 551, 254, 553 }, { 867, 524, 947, 551, 282, 553 },
        { 959, 524, 1090, 551, 370, 553 }, { 80, 553, 195, 580, 508, 554 }, { 200, 553, 220, 580, 631, 554 }, { 225, 553, 322, 580, 659, 554 },
        { 327, 553, 378, 580, 764, 554 }, { 77, 582, 107, 609, 822, 554 }, { 111, 582, 295, 609, 860, 554 }, { 299, 582, 324, 609, 78, 583 },
        { 327, 582, 385, 609, 108, 583 }, { 390, 582, 463, 609, 171, 583 }, { 468, 582, 505, 609, 249, 583 }, { 508, 582, 545, 609, 291, 583 },
        { 550, 582, 640, 609, 333, 583 }, { 644, 582, 721, 609, 428, 583 }, { 724, 582, 836, 609, 510, 583 }, { 840, 582, 925, 609, 627, 583 },
        { 930, 582, 959, 609, 717, 583 }, { 965, 582, 1034, 609, 751, 583 }, { 1039, 582, 1149, 609, 825, 583 },
    };
    // french: 105 rects
    static const short legal_french[][6] = {
        { 73, 292, 640, 392, 73, 292 }, { 81, 414, 178, 439, 78, 414 }, { 181, 414, 204, 439, 184, 414 }, { 208, 414, 262, 439, 216, 414 },
        { 266, 414, 371, 439, 280, 414 }, { 377, 414, 399, 439, 394, 414 }, { 403, 414, 519, 439, 425, 414 }, { 524, 414, 544, 439, 550, 414 },
        { 549, 414, 646, 439, 580, 414 }, { 651, 414, 696, 439, 686, 414 }, { 701, 414, 743, 439, 740, 414 }, { 747, 414, 826, 439, 791, 414 },
        { 830, 414, 925, 439, 880, 414 }, { 930, 414, 1002, 439, 984, 414 }, { 1015, 414, 1124, 439, 1065, 414 }, { 82, 443, 185, 470, 78, 443 },
        { 189, 443, 303, 470, 187, 443 }, { 307, 443, 403, 470, 306, 443 }, { 407, 443, 430, 470, 408, 443 }, { 434, 443, 466, 470, 436, 443 },
        { 471, 443, 531, 470, 474, 443 }, { 535, 443, 631, 470, 540, 443 }, { 635, 443, 683, 470, 641, 443 }, { 687, 443, 728, 470, 695, 443 },
        { 733, 443, 828, 470, 742, 443 }, { 832, 443, 983, 470, 842, 443 }, { 987, 443, 1015, 470, 999, 443 }, { 1020, 443, 1061, 470, 1032, 443 },
        { 1066, 443, 1161, 470, 1079, 443 }, { 81, 468, 188, 499, 78, 468 }, { 192, 468, 221, 499, 190, 468 }, { 225, 468, 320, 499, 224, 468 },
        { 325, 468, 392, 499, 324, 468 }, { 397, 468, 436, 499, 396, 468 }, { 441, 468, 483, 499, 440, 468 }, { 488, 468, 600, 499, 487, 468 },
        { 604, 468, 659, 499, 604, 468 }, { 664, 468, 719, 499, 664, 468 }, { 723, 468, 810, 499, 724, 468 }, { 815, 468, 873, 499, 816, 468 },
        { 886, 468, 968, 499, 879, 468 }, { 974, 468, 1049, 499, 966, 468 }, { 1055, 468, 1076, 499, 1046, 468 }, { 1082, 468, 1157, 499, 1072, 468 },
        { 1162, 468, 1184, 499, 1152, 468 }, { 82, 501, 219, 527, 78, 501 }, { 224, 501, 248, 527, 223, 501 }, { 251, 501, 297, 527, 255, 501 },
        { 303, 501, 379, 527, 310, 501 }, { 384, 501, 400, 527, 394, 501 }, { 405, 501, 455, 527, 418, 501 }, { 461, 501, 541, 527, 476, 501 },
        { 547, 501, 573, 527, 565, 501 }, { 579, 501, 599, 527, 599, 501 }, { 604, 501, 707, 527, 627, 501 }, { 713, 501, 733, 527, 738, 501 },
        { 737, 501, 793, 527, 767, 501 }, { 797, 501, 821, 527, 831, 501 }, { 825, 501, 857, 527, 863, 501 }, { 862, 501, 921, 527, 903, 501 },
        { 925, 501, 1021, 527, 971, 501 }, { 1025, 501, 1074, 527, 1075, 501 }, { 1077, 501, 1119, 527, 1132, 501 }, { 82, 526, 177, 556, 78, 527 },
        { 181, 526, 332, 556, 182, 527 }, { 336, 526, 364, 556, 343, 527 }, { 369, 526, 410, 556, 380, 527 }, { 415, 526, 511, 556, 430, 527 },
        { 515, 526, 621, 556, 536, 527 }, { 625, 526, 655, 556, 651, 527 }, { 659, 526, 679, 556, 690, 527 }, { 684, 526, 781, 556, 719, 527 },
        { 786, 526, 832, 556, 826, 527 }, { 836, 526, 878, 556, 881, 527 }, { 882, 526, 995, 556, 932, 527 }, { 999, 526, 1054, 556, 1055, 527 },
        { 1059, 526, 1114, 556, 1119, 527 }, { 81, 558, 168, 585, 78, 559 }, { 173, 558, 231, 585, 170, 559 }, { 244, 558, 311, 585, 233, 559 },
        { 316, 558, 336, 585, 305, 559 }, { 340, 558, 415, 585, 330, 559 }, { 418, 558, 471, 585, 410, 559 }, { 476, 558, 576, 585, 468, 559 },
        { 579, 558, 601, 585, 573, 559 }, { 606, 558, 721, 585, 600, 559 }, { 725, 558, 755, 585, 721, 559 }, { 759, 558, 779, 585, 756, 559 },
        { 784, 558, 881, 585, 781, 559 }, { 886, 558, 938, 585, 883, 559 }, { 675, 587, 731, 614, 940, 559 }, { 736, 587, 768, 614, 1001, 559 },
        { 772, 587, 842, 614, 1038, 559 }, { 846, 587, 907, 614, 1113, 559 }, { 911, 587, 1001, 614, 78, 588 }, { 1004, 587, 1027, 614, 173, 588 },
        { 1031, 587, 1126, 614, 201, 588 }, { 81, 616, 231, 643, 301, 588 }, { 235, 616, 250, 643, 456, 588 }, { 255, 616, 309, 643, 476, 588 },
        { 314, 616, 449, 643, 535, 588 }, { 454, 616, 564, 643, 675, 588 }, { 576, 616, 632, 643, 790, 588 }, { 636, 616, 697, 643, 851, 588 },
        { 702, 616, 802, 643, 917, 588 },
    };
    // figs: 89 rects
    static const short legal_figs[][6] = {
        { 73, 292, 640, 392, 73, 292 }, { 78, 413, 201, 440, 78, 413 }, { 205, 413, 259, 440, 207, 413 }, { 262, 413, 368, 440, 268, 413 },
        { 373, 413, 395, 440, 380, 413 }, { 400, 413, 515, 440, 408, 413 }, { 520, 413, 540, 440, 530, 413 }, { 545, 413, 642, 440, 556, 413 },
        { 647, 413, 699, 440, 660, 413 }, { 704, 413, 719, 440, 718, 413 }, { 723, 413, 819, 440, 739, 413 }, { 823, 413, 890, 440, 842, 413 },
        { 895, 413, 1000, 440, 915, 413 }, { 1006, 413, 1115, 440, 78, 442 }, { 78, 442, 181, 469, 192, 442 }, { 186, 442, 299, 469, 300, 442 },
        { 304, 442, 400, 469, 418, 442 }, { 403, 442, 445, 469, 519, 442 }, { 450, 442, 525, 469, 566, 442 }, { 530, 442, 590, 469, 647, 442 },
        { 594, 442, 631, 469, 712, 442 }, { 636, 442, 744, 469, 754, 442 }, { 748, 442, 871, 469, 867, 442 }, { 875, 442, 900, 469, 995, 442 },
        { 903, 442, 1026, 469, 78, 471 }, { 1030, 442, 1054, 469, 207, 471 }, { 77, 471, 173, 496, 237, 471 }, { 177, 471, 244, 496, 339, 471 },
        { 250, 471, 289, 496, 412, 471 }, { 294, 471, 314, 496, 457, 471 }, { 319, 471, 355, 496, 483, 471 }, { 360, 471, 407, 496, 524, 471 },
        { 412, 471, 484, 496, 577, 471 }, { 487, 471, 545, 496, 655, 471 }, { 549, 471, 654, 496, 719, 471 }, { 660, 471, 742, 496, 830, 471 },
        { 748, 471, 824, 496, 918, 471 }, { 830, 471, 850, 496, 1000, 471 }, { 856, 471, 932, 496, 78, 500 }, { 937, 471, 958, 496, 159, 500 },
        { 964, 471, 1040, 496, 185, 500 }, { 1045, 471, 1060, 496, 267, 500 }, { 1065, 471, 1116, 496, 287, 500 }, { 1122, 471, 1202, 496, 343, 500 },
        { 78, 500, 104, 527, 428, 500 }, { 110, 500, 130, 527, 459, 500 }, { 135, 500, 238, 527, 485, 500 }, { 244, 500, 264, 527, 593, 500 },
        { 268, 500, 324, 527, 618, 500 }, { 328, 500, 370, 527, 679, 500 }, { 376, 500, 450, 527, 726, 500 }, { 456, 500, 515, 527, 806, 500 },
        { 519, 500, 556, 527, 870, 500 }, { 561, 500, 669, 527, 912, 500 }, { 673, 500, 796, 527, 78, 529 }, { 801, 500, 951, 527, 208, 529 },
        { 955, 500, 980, 527, 365, 529 }, { 983, 500, 1003, 527, 398, 529 }, { 1008, 500, 1105, 527, 425, 529 }, { 1110, 500, 1155, 527, 529, 529 },
        { 78, 528, 98, 555, 581, 528 }, { 102, 528, 139, 555, 608, 528 }, { 144, 528, 191, 555, 652, 528 }, { 196, 528, 268, 555, 706, 528 },
        { 271, 528, 329, 555, 786, 528 }, { 332, 528, 438, 555, 851, 528 }, { 451, 528, 507, 555, 964, 528 }, { 512, 528, 532, 555, 78, 557 },
        { 536, 528, 616, 555, 103, 557 }, { 628, 528, 758, 555, 188, 557 }, { 763, 528, 878, 555, 324, 557 }, { 883, 528, 903, 555, 444, 557 },
        { 908, 528, 1005, 555, 469, 557 }, { 1010, 528, 1062, 555, 571, 557 }, { 807, 557, 836, 584, 628, 558 }, { 841, 557, 899, 584, 662, 558 },
        { 902, 557, 1025, 584, 725, 558 }, { 1029, 557, 1054, 584, 854, 558 }, { 1057, 557, 1114, 584, 884, 558 }, { 78, 586, 152, 613, 946, 558 },
        { 156, 586, 194, 613, 78, 587 }, { 197, 586, 233, 613, 121, 587 }, { 238, 586, 329, 613, 162, 587 }, { 332, 586, 409, 613, 258, 587 },
        { 413, 586, 524, 613, 340, 587 }, { 528, 586, 614, 613, 456, 587 }, { 618, 586, 648, 613, 547, 587 }, { 653, 586, 723, 613, 582, 587 },
        { 728, 586, 838, 613, 657, 587 },
    };

        const char* name = splashScreen->GetName();
        const short(*rects)[6] = legal_english;
        int numRects = sizeof(legal_english) / sizeof(legal_english[0]);
        if (idStr::FindText(name, "legal_french", false) >= 0) {
            rects = legal_french;
            numRects = sizeof(legal_french) / sizeof(legal_french[0]);
        } else if (idStr::FindText(name, "legal_figs", false) >= 0) {
            rects = legal_figs;
            numRects = sizeof(legal_figs) / sizeof(legal_figs[0]);
        } else if (idStr::FindText(name, "legal_english", false) < 0) {
            numRects = 0; // not a legal screen we know: as it is
        }
        if (numRects > 0) {
            const float sx = barWidth, sy = barHeight;
            const float sw = renderSystem->GetVirtualWidth() - barWidth * 2.0f;
            const float sh = renderSystem->GetVirtualHeight() - barHeight * 2.0f;
            renderSystem->SetColor(colorBlack);
            renderSystem->DrawStretchPic(sx, sy, sw, sh, 0, 0, 1, 1, whiteMaterial);
            renderSystem->SetColor4(1, 1, 1, 1);
            for (int i = 0; i < numRects; i++) {
                const short* r = rects[i];
                renderSystem->DrawStretchPic(sx + r[4] / 1280.0f * sw, sy + r[5] / 720.0f * sh, (r[2] - r[0]) / 1280.0f * sw, (r[3] - r[1]) / 720.0f * sh,
                    r[0] / 1280.0f, r[1] / 720.0f, r[2] / 1280.0f, r[3] / 720.0f, splashScreen);
            }
        }
        renderSystem->SetColor4(1, 1, 1, 1);
    }

    VITA_MARK("splash: swap");
    const emptyCommand_t* cmd = renderSystem->SwapCommandBuffers(&time_frontend, &time_backend, &time_shadows, &time_gpu);
    VITA_MARK("splash: render");
    renderSystem->RenderCommandBuffers(cmd);
    VITA_MARK("splash: done");
}

/*
=================
idCommonLocal::RenderBink
=================
*/
void idCommonLocal::RenderBink(const char* path)
{
    const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
    const float sysHeight = renderSystem->GetHeight();
    const float sysAspect = sysWidth / sysHeight;
    const float movieAspect = (16.0f / 9.0f);
    const float imageWidth = renderSystem->GetVirtualWidth() * movieAspect / sysAspect;
    const float chop = 0.5f * (renderSystem->GetVirtualWidth() - imageWidth);

    idStr materialText;
    materialText.Format("{ translucent { videoMap %s } }", path);

    idMaterial* material = const_cast<idMaterial*>(declManager->FindMaterial("splashbink"));
    material->Parse(materialText.c_str(), materialText.Length(), false);
    material->ResetCinematicTime(Sys_Milliseconds());

    // RB: FFmpeg might return the wrong play length so I changed the intro video to play max 30 seconds until finished
    int cinematicLength = 30000; // material->CinematicLength();
    int mouseEvents[MAX_MOUSE_EVENTS][2];

    bool escapeEvent = false;
    while ((Sys_Milliseconds() <= (material->GetCinematicStartTime() + cinematicLength)) && material->CinematicIsPlaying()) {
        renderSystem->DrawStretchPic(chop, 0, imageWidth, renderSystem->GetVirtualHeight(), 0, 0, 1, 1, material);
        const emptyCommand_t* cmd = renderSystem->SwapCommandBuffers(&time_frontend, &time_backend, &time_shadows, &time_gpu);
        renderSystem->RenderCommandBuffers(cmd);

        Sys_GenerateEvents();

        // queue system events ready for polling
        Sys_GetEvent();

        // RB: allow to escape video by pressing anything
        int numKeyEvents = Sys_PollKeyboardInputEvents();
        if (numKeyEvents > 0) {
            for (int i = 0; i < numKeyEvents; i++) {
                int key;
                bool state;

                if (Sys_ReturnKeyboardInputEvent(i, key, state)) {
                    if (key == K_ESCAPE && state == true) {
                        escapeEvent = true;
                    }
                    break;
                }
            }

            Sys_EndKeyboardInputEvents();
        }

        int numMouseEvents = Sys_PollMouseInputEvents(mouseEvents);
        if (numMouseEvents > 0) {
            for (int i = 0; i < numMouseEvents; i++) {
                int action = mouseEvents[i][0];
                switch (action) {
                case M_ACTION1:
                case M_ACTION2:
                case M_ACTION3:
                case M_ACTION4:
                case M_ACTION5:
                case M_ACTION6:
                case M_ACTION7:
                case M_ACTION8:
                    escapeEvent = true;
                    break;

                default: // some other undefined button
                    break;
                }
            }
        }

        int numJoystickEvents = Sys_PollJoystickInputEvents(0);
        if (numJoystickEvents > 0) {
            for (int i = 0; i < numJoystickEvents; i++) {
                int action;
                int value;

                if (Sys_ReturnJoystickInputEvent(i, action, value)) {
                    if (action >= J_ACTION1 && action <= J_ACTION_MAX) {
                        if (value != 0) {
                            escapeEvent = true;
                            break;
                        }
                    }
                }
            }

            Sys_EndJoystickInputEvents();
        }

        if (escapeEvent) {
            break;
        }

        Sys_Sleep(10);
    }
    // RB end

    material->MakeDefault();
}

/*
=================
idCommonLocal::InitSIMD
=================
*/
void idCommonLocal::InitSIMD()
{
    idSIMD::InitProcessor("doom", com_forceGenericSIMD.GetBool());
    com_forceGenericSIMD.ClearModified();
}

/*
=================
idCommonLocal::LoadGameDLL
=================
*/
void idCommonLocal::LoadGameDLL()
{
#ifdef __DOOM_DLL__
    char dllPath[MAX_OSPATH];

    gameImport_t gameImport;
    gameExport_t gameExport;
    GetGameAPI_t GetGameAPI;

    fileSystem->FindDLL("game", dllPath, true);

    if (!dllPath[0]) {
        common->FatalError("couldn't find game dynamic library");
        return;
    }
    common->DPrintf("Loading game DLL: '%s'\n", dllPath);
    gameDLL = sys->DLL_Load(dllPath);
    if (!gameDLL) {
        common->FatalError("couldn't load game dynamic library");
        return;
    }

    const char* functionName = "GetGameAPI";
    GetGameAPI = (GetGameAPI_t)Sys_DLL_GetProcAddress(gameDLL, functionName);
    if (!GetGameAPI) {
        Sys_DLL_Unload(gameDLL);
        gameDLL = NULL;
        common->FatalError("couldn't find game DLL API");
        return;
    }

    gameImport.version = GAME_API_VERSION;
    gameImport.sys = ::sys;
    gameImport.common = ::common;
    gameImport.cmdSystem = ::cmdSystem;
    gameImport.cvarSystem = ::cvarSystem;
    gameImport.fileSystem = ::fileSystem;
    gameImport.renderSystem = ::renderSystem;
    gameImport.soundSystem = ::soundSystem;
    gameImport.renderModelManager = ::renderModelManager;
    gameImport.uiManager = ::uiManager;
    gameImport.declManager = ::declManager;
    gameImport.AASFileManager = ::AASFileManager;
    gameImport.collisionModelManager = ::collisionModelManager;

    gameExport = *GetGameAPI(&gameImport);

    if (gameExport.version != GAME_API_VERSION) {
        Sys_DLL_Unload(gameDLL);
        gameDLL = NULL;
        common->FatalError("wrong game DLL API version");
        return;
    }

    game = gameExport.game;
    gameEdit = gameExport.gameEdit;

#endif

    // initialize the game object
    if (game != NULL) {
        game->Init();
    }
}

/*
=================
idCommonLocal::UnloadGameDLL
=================
*/
void idCommonLocal::CleanupShell()
{
    if (game != NULL) {
        game->Shell_Cleanup();
    }
}

/*
=================
idCommonLocal::UnloadGameDLL
=================
*/
void idCommonLocal::UnloadGameDLL()
{

    // shut down the game object
    if (game != NULL) {
        game->Shutdown();
    }

#ifdef __DOOM_DLL__

    if (gameDLL) {
        Sys_DLL_Unload(gameDLL);
        gameDLL = NULL;
    }
    game = NULL;
    gameEdit = NULL;

#endif
}

/*
=================
idCommonLocal::IsInitialized
=================
*/
bool idCommonLocal::IsInitialized() const
{
    return com_fullyInitialized;
}

//======================================================================================

#if defined(__vita__)
/*
=================
Vita_ApplyTuning

The performance settings are saved in the config, so a build that changes
their defaults would never see them: when the config's tuning version is
older than this build's, the listed settings are reset to this build's
values once (and the version saved). Run twice: after the config (the
engine's settings), and again after the game code is loaded - the game's
settings (vita_afMaxMoveTime...) only exist from then on, and a value set
before took the saved one back when they were registered (run 34 played
with vita_afMaxMoveTime 4 although the reset had set 1.25).
=================
*/
/*
=================
Vita_ApplyFixedSettings

The settings the Vita menus do not show (System: render resolution, dynamic
resolution, frame rate cap, shadows; Game Options: flashlight shadows) are
held at these values at every start, whatever an older config saved. Run
after the config (engine settings) and after the game code loaded
(g_weaponShadows is the game's). The console can still change them for a
session.
=================
*/
static void Vita_ApplyFixedSettings(bool game)
{
    static const char* engineFixed[][2] = {
        { "vita_renderResolution", "720x408" }, // top of the 3D view's range (display 960x544)
        { "vita_dynamicResolution", "1" },      // ... down to 544x312
        { "vita_maxFPS", "30" },
        { "r_skipShadows", "0" },
        { NULL, NULL }
    };
    static const char* gameFixed[][2] = {
        { "g_weaponShadows", "1" }, // "Flashlight Shadows": the flashlight / muzzle light casts shadows
        { NULL, NULL }
    };
    const char* (*fixed)[2] = game ? gameFixed : engineFixed;
    for (int i = 0; fixed[i][0] != NULL; i++) {
        const char* was = cvarSystem->GetCVarString(fixed[i][0]);
        if (idStr::Cmp(was, fixed[i][1]) != 0) {
            common->Printf("vita: fixed setting %s = %s (the config had %s)\n", fixed[i][0], fixed[i][1], was);
            cvarSystem->SetCVarString(fixed[i][0], fixed[i][1]);
        }
    }
}

// Game Options -> Language: the language picked in the menu, applied at the
// next start (sys_lang only changes before anything localized is loaded -
// voice-overs, strings and the savegame language would mix otherwise)
idCVar vita_language("vita_language", "", CVAR_SYSTEM | CVAR_ARCHIVE, "Vita: the language to start in next time (english, french, italian, german, spanish, japanese; empty keeps sys_lang)");

static void Vita_ApplyLanguage()
{
    extern idCVar sys_lang;
    const char* want = vita_language.GetString();
    if (want[0] == '\0') {
        common->Printf("vita: language %s\n", sys_lang.GetString());
        return;
    }
    for (int i = 0; i < Sys_NumLangs(); i++) {
        if (idStr::Icmp(want, Sys_Lang(i)) == 0) {
            if (idStr::Icmp(sys_lang.GetString(), Sys_Lang(i)) != 0) {
                common->Printf("vita: language %s (picked in Game Options; sys_lang was %s)\n", Sys_Lang(i), sys_lang.GetString());
                sys_lang.SetString(Sys_Lang(i));
            } else {
                common->Printf("vita: language %s\n", sys_lang.GetString());
            }
            return;
        }
    }
    common->Warning("vita: vita_language '%s' is not a language this game knows - keeping %s", want, sys_lang.GetString());
}

static const int VITA_TUNING_VERSION = 6;
static int vitaTuningSavedVersion = -1;
static void Vita_ApplyTuning(bool final)
{
    static const char* tuning[][2] = {
        { "vita_animNearDistance", "192" },   // 15 Hz monster animation from 192 units (was 384)
        { "vita_animFocusAngle", "0" },
        { "vita_guiNearDistance", "96" },     // world screens at 15 Hz beyond 96 units (was 192)
        { "vita_guiFocusAngle", "8" },        // ... unless within 8 degrees of the view center (was 20)
        { "vita_aiFarDistance", "512" },      // reachability every other tic beyond 512 (was 1024)
        { "vita_ikDistance", "384" },         // walk IK skipped beyond 384 (was 640)
        { "vita_afMaxMoveTime", "0.9" },      // ragdolls put to rest after 0.9 s of motion (was 4, 2, 1.25)
        { "vita_maxTicsPerFrame", "1" },
        { "vita_animLOD", "1" },
        { "vita_animatedShadowLights", "1" },
        { NULL, NULL }
    };
    if (vitaTuningSavedVersion < 0) {
        vitaTuningSavedVersion = cvarSystem->GetCVarInteger("vita_tuningVersion");
    }
    if (vitaTuningSavedVersion >= VITA_TUNING_VERSION) {
        return;
    }
    for (int i = 0; tuning[i][0] != NULL; i++) {
        cvarSystem->SetCVarString(tuning[i][0], tuning[i][1]);
    }
    if (final) {
        cvarSystem->SetCVarInteger("vita_tuningVersion", VITA_TUNING_VERSION);
        for (int i = 0; tuning[i][0] != NULL; i++) {
            common->Printf("vita: tuning: %s = %s (now %s)\n", tuning[i][0], tuning[i][1], cvarSystem->GetCVarString(tuning[i][0]));
        }
    }
    common->Printf("vita: performance settings reset to this build's (tuning version %d, the config had %d)%s\n", VITA_TUNING_VERSION, vitaTuningSavedVersion,
        final ? " - after the game code loaded" : " - engine settings");
}
#endif

/*
=================
idCommonLocal::Init
=================
*/
void idCommonLocal::Init(int argc, const char* const* argv, const char* cmdline)
{
    try {
        // set interface pointers used by idLib
        idLib::sys = sys;
        idLib::common = common;
        idLib::cvarSystem = cvarSystem;
        idLib::fileSystem = fileSystem;

        // initialize idLib
        idLib::Init();

        // clear warning buffer
        ClearWarnings(GAME_NAME " initialization");

        idLib::Printf("Command line: %s\n", cmdline);
        //::MessageBox( NULL, cmdline, "blah", MB_OK );
        // parse command line options
        idCmdArgs args;
        if (cmdline) {
            // tokenize if the OS doesn't do it for us
            args.TokenizeString(cmdline, true);
            argv = args.GetArgs(&argc);
        }
        ParseCommandLine(argc, argv);

        // init console command system
        cmdSystem->Init();

        // init CVar system
        cvarSystem->Init();

        // register all static CVars
        idCVar::RegisterStaticVars();

        idLib::Printf("QA Timing INIT: %06dms\n", Sys_Milliseconds());

        // print engine version
        Printf("%s\n", version.string);

        // initialize key input/binding, done early so bind command exists
        idKeyInput::Init();

        // init the console so we can take prints
        console->Init();

        // get architecture info
        Sys_Init();

        // initialize networking
        Sys_InitNetworking();

        // override cvars from command line
        StartupVariable(NULL);

        consoleUsed = com_allowConsole.GetBool();

        if (Sys_AlreadyRunning()) {
            Sys_Quit();
        }

        // initialize processor specific SIMD implementation
        InitSIMD();

        // initialize the file system
        fileSystem->Init();

        const char* defaultLang = Sys_DefaultLanguage();
        com_isJapaneseSKU = (idStr::Icmp(defaultLang, ID_LANG_JAPANESE) == 0);

        // Allow the system to set a default lanugage
        Sys_SetLanguageFromSystem();

        // Pre-allocate our 20 MB save buffer here on time, instead of on-demand for each save....

        saveFile.SetNameAndType(SAVEGAME_CHECKPOINT_FILENAME, SAVEGAMEFILE_BINARY);
        stringsFile.SetNameAndType(SAVEGAME_STRINGS_FILENAME, SAVEGAMEFILE_BINARY);
#if defined(__vita__)
        // The Vita: the save buffers exist only while a game is saved or
        // loaded (Common_load.cpp), and the file system gets no block buffer
        // of theirs - see idCommonLocal::VitaReleaseSaveBuffers
        fileSystem->BeginLevelLoad("_startup", NULL, 0);
#else
        saveFile.PreAllocate(MIN_SAVEGAME_SIZE_BYTES);
        stringsFile.PreAllocate(MAX_SAVEGAME_STRING_TABLE_SIZE);

        fileSystem->BeginLevelLoad("_startup", saveFile.GetDataPtr(), saveFile.GetAllocated());
#endif

        // initialize the declaration manager
        declManager->Init();

        // init journalling, etc
        eventLoop->Init();

        // init the parallel job manager
        parallelJobManager->Init();

        // exec the startup scripts
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "exec default.cfg\n");

#ifdef CONFIG_FILE
        // skip the config file if "safe" is on the command line
        if (!SafeMode() && !g_demoMode.GetBool()) {
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "exec " CONFIG_FILE "\n");
        }
#endif

        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "exec autoexec.cfg\n");

        // run cfg execution
        cmdSystem->ExecuteCommandBuffer();

#if defined(__vita__)
        // the saved config carries key binds too; the Vita's pad scheme is
        // fixed, so it goes on top of whatever the config files bound
        Vita_ApplyControlScheme();

        // performance settings reset to this build's (engine settings; the
        // game's follow after LoadGameDLL)
        Vita_ApplyTuning(false);
        Vita_ApplyFixedSettings(false);
        Vita_ApplyLanguage();
#endif

        // re-override anything from the config files with command line args
        StartupVariable(NULL);

        // if any archived cvars are modified after this, we will trigger a writing of the config file
        cvarSystem->ClearModifiedFlags(CVAR_ARCHIVE);

        // init OpenGL, which will open a window and connect sound and input hardware
        renderSystem->InitOpenGL();

        // Support up to 2 digits after the decimal point
#if defined(__vita__)
        com_engineHz_denominator = 100LL * 60;       // the title screen and menus: 60 (vita_gameHz is for maps)
        com_engineHz_latched = 60.0f;
#else
        com_engineHz_denominator = 100LL * com_engineHz.GetFloat();
        com_engineHz_latched = com_engineHz.GetFloat();
#endif

        // start the sound system, but don't do any hardware operations yet
        soundSystem->Init();

        // initialize the renderSystem data structures
        renderSystem->Init();

        whiteMaterial = declManager->FindMaterial("_white");

        if (idStr::Icmp(sys_lang.GetString(), ID_LANG_FRENCH) == 0) {
            // If the user specified french, we show french no matter what SKU
            splashScreen = declManager->FindMaterial("guis/assets/splash/legal_french");
        } else if (idStr::Icmp(defaultLang, ID_LANG_FRENCH) == 0) {
            // If the lead sku is french (ie: europe), display figs
            splashScreen = declManager->FindMaterial("guis/assets/splash/legal_figs");
        } else {
            // Otherwise show it in english
            splashScreen = declManager->FindMaterial("guis/assets/splash/legal_english");
        }

        const int legalMinTime = 4000;
        const bool showVideo = (!com_skipIntroVideos.GetBool() && fileSystem->UsingResourceFiles());
        if (showVideo) {
            // Forward slashes: idCinematic::InitFromFile() takes this path apart to
            // build the .roq name it tries first, and the Vita filesystem does not
            // want backslashes. The .bik spelling here is still correct - the
            // extension is stripped and .roq is attempted before any Bink fallback.
            RenderBink("video/loadvideo.bik");
            RenderSplash();
            RenderSplash();
        } else {
            idLib::Printf("Skipping Intro Videos!\n");
            // display the legal splash screen
            // No clue why we have to render this twice to show up...
            RenderSplash();
            RenderSplash();
        }

        int legalStartTime = Sys_Milliseconds();
        declManager->Init2();

        // initialize string database so we can use it for loading messages
        InitLanguageDict();

        // spawn the game thread, even if we are going to run without SMP
        // one meg stack, because it can parse decls from gui surfaces (unfortunately)
        // use a lower priority so job threads can run on the same core
        gameThread.StartWorkerThread("Game/Draw", CORE_1B, THREAD_BELOW_NORMAL, 0x100000);
        // boost this thread's priority, so it will prevent job threads from running while
        // the render back end still has work to do

        // init the user command input code
        usercmdGen->Init();

        Sys_SetRumble(0, 0, 0);

        // initialize the user interfaces
        uiManager->Init();

        // startup the script debugger
        // DebuggerServerInit();

        // load the game dll
        VITA_PHASE("LoadGameDLL");
        LoadGameDLL();
#if defined(__vita__)
        Vita_ApplyTuning(true); // the game's settings exist now
        Vita_ApplyFixedSettings(true);
#endif

        // On the PC touch them all so they get included in the resource build
        if (!fileSystem->UsingResourceFiles()) {
            declManager->FindMaterial("guis/assets/splash/legal_english");
            declManager->FindMaterial("guis/assets/splash/legal_french");
            declManager->FindMaterial("guis/assets/splash/legal_figs");
            // register the japanese font so it gets included
            renderSystem->RegisterFont("DFPHeiseiGothicW7");
            // Make sure all videos get touched because you can bring videos from one map to another, they need to be included in all maps
            for (int i = 0; i < declManager->GetNumDecls(DECL_VIDEO); i++) {
                declManager->DeclByIndex(DECL_VIDEO, i);
            }
        }

        VITA_PHASE("UnloadResourceContainer");
        fileSystem->UnloadResourceContainer("_ordered");

        // the same idRenderWorld will be used for all games
        // and demos, insuring that level specific models
        // will be freed
        VITA_PHASE("AllocRenderWorld");
        renderWorld = renderSystem->AllocRenderWorld();
        soundWorld = soundSystem->AllocSoundWorld(renderWorld);

        menuSoundWorld = soundSystem->AllocSoundWorld(NULL);
        menuSoundWorld->PlaceListener(vec3_origin, mat3_identity, 0);

        // init the session
        VITA_PHASE("session->Initialize");
        session->Initialize();
        session->InitializeSoundRelatedSystems();

        VITA_PHASE("InitializeMPMapsModes");
        InitializeMPMapsModes();

        // leaderboards need to be initialized after InitializeMPMapsModes, which populates the MP Map list.
        if (game != NULL) {
            game->Leaderboards_Init();
        }

        VITA_PHASE("CreateMainMenu");
        CreateMainMenu();

        VITA_PHASE("commonDialog.Init");
        commonDialog.Init();

        // load the console history file
        consoleHistory.LoadHistoryFile();

        VITA_PHASE("AddStartupCommands");
        AddStartupCommands();

        VITA_PHASE("StartMenu");
        StartMenu(true);

        while (Sys_Milliseconds() - legalStartTime < legalMinTime) {
            RenderSplash();
            Sys_GenerateEvents();
            Sys_Sleep(10);
        };

        // print all warnings queued during initialization
        VITA_PHASE("PrintWarnings");
        PrintWarnings();

        // remove any prints from the notify lines
        console->ClearNotifyLines();

        CheckStartupStorageRequirements();

        if (preload_CommonAssets.GetBool() && fileSystem->UsingResourceFiles()) {
            idPreloadManifest manifest;
            manifest.LoadManifest("_common.preload");
            globalImages->Preload(manifest, false);
            soundSystem->Preload(manifest);
        }

        fileSystem->EndLevelLoad();

#if defined(__vita__)
        // The menus first draw the Vita button glyphs on the game/draw thread,
        // where an unparsed material is an error: parse it here, on the main one.
        Vita_UI_Material();
#endif
        // RB begin
#if defined(USE_DOOMCLASSIC)
        // Initialize support for Doom classic.
        doomClassicMaterial = declManager->FindMaterial("_doomClassic");
        idImage* image = globalImages->GetImage("_doomClassic");
        if (image != NULL) {
            idImageOpts opts;
            opts.format = FMT_RGBA8;
            opts.colorFormat = CFM_DEFAULT;
#if defined(__vita__)
            // the 320x200 frame is blown up 3x into this with nearest filtering
            // on the GPU, then drawn with linear filtering: sharp, even pixels
            opts.width = DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_UPSCALE;
            opts.height = DOOMCLASSIC_RENDERHEIGHT * DOOMCLASSIC_UPSCALE;
#else
            opts.width = DOOMCLASSIC_RENDERWIDTH;
            opts.height = DOOMCLASSIC_RENDERHEIGHT;
#endif
            opts.numLevels = 1;
            image->AllocImage(opts, TF_LINEAR, TR_REPEAT);
        }
#endif
        // RB end

        com_fullyInitialized = true;

        // No longer need the splash screen
        if (splashScreen != NULL) {
            for (int i = 0; i < splashScreen->GetNumStages(); i++) {
                idImage* image = splashScreen->GetStage(i)->texture.image;
                if (image != NULL) {
                    image->PurgeImage();
                }
            }
        }

        Printf("--- Common Initialization Complete ---\n");

        idLib::Printf("QA Timing IIS: %06dms\n", Sys_Milliseconds());
    } catch (idException&) {
        Sys_Error("Error during initialization");
    }
}

/*
=================
idCommonLocal::Shutdown
=================
*/
void idCommonLocal::Shutdown()
{

    if (com_shuttingDown) {
        return;
    }
    com_shuttingDown = true;

    // Kill any pending saves...
    printf("session->GetSaveGameManager().CancelToTerminate();\n");
    session->GetSaveGameManager().CancelToTerminate();

    // kill sound first
    printf("soundSystem->StopAllSounds();\n");
    soundSystem->StopAllSounds();

    // shutdown the script debugger
    // DebuggerServerShutdown();

    if (aviCaptureMode) {
        printf("EndAVICapture();\n");
        EndAVICapture();
    }

    printf("Stop();\n");
    Stop();

    printf("CleanupShell();\n");
    CleanupShell();

    printf("delete loadGUI;\n");
    delete loadGUI;
    loadGUI = NULL;

    printf("delete renderWorld;\n");
    // SRS - Call FreeRenderWorld() vs. delete, otherwise worlds list not updated on shutdown
    renderSystem->FreeRenderWorld(renderWorld);
    renderWorld = NULL;

    printf("delete soundWorld;\n");
    // SRS - Call FreeSoundWorld() vs. delete, otherwise soundWorlds list not updated and can segfault in soundSystem->Shutdown()
    soundSystem->FreeSoundWorld(soundWorld);
    soundWorld = NULL;

    printf("delete menuSoundWorld;\n");
    // SRS - Call FreeSoundWorld() vs. delete, otherwise soundWorlds list not updated and can segfault in soundSystem->Shutdown()
    soundSystem->FreeSoundWorld(menuSoundWorld);
    menuSoundWorld = NULL;

    // shut down the session
    printf("session->ShutdownSoundRelatedSystems();\n");
    session->ShutdownSoundRelatedSystems();
    printf("session->Shutdown();\n");
    session->Shutdown();

    // shutdown, deallocate leaderboard definitions.
    if (game != NULL) {
        printf("game->Leaderboards_Shutdown();\n");
        game->Leaderboards_Shutdown();
    }

    // shut down the user interfaces
    printf("uiManager->Shutdown();\n");
    uiManager->Shutdown();

    // shut down the sound system
    printf("soundSystem->Shutdown();\n");
    soundSystem->Shutdown();

    // shut down the user command input code
    printf("usercmdGen->Shutdown();\n");
    usercmdGen->Shutdown();

    // shut down the event loop
    printf("eventLoop->Shutdown();\n");
    eventLoop->Shutdown();

    // shutdown the decl manager
    printf("declManager->Shutdown();\n");
    declManager->Shutdown();

    // shut down the renderSystem
    printf("renderSystem->Shutdown();\n");
    renderSystem->Shutdown();

    printf("commonDialog.Shutdown();\n");
    commonDialog.Shutdown();

    // unload the game dll
    printf("UnloadGameDLL();\n");
    UnloadGameDLL();

    printf("saveFile.Clear( true );\n");
    saveFile.Clear(true);
    printf("stringsFile.Clear( true );\n");
    stringsFile.Clear(true);

    // only shut down the log file after all output is done
    printf("CloseLogFile();\n");
    CloseLogFile();

    // shut down the file system
    printf("fileSystem->Shutdown( false );\n");
    fileSystem->Shutdown(false);

    // shut down non-portable system services
    printf("Sys_Shutdown();\n");
    Sys_Shutdown();

    // shut down the console
    printf("console->Shutdown();\n");
    console->Shutdown();

    // shut down the key system
    printf("idKeyInput::Shutdown();\n");
    idKeyInput::Shutdown();

    // shut down the cvar system
    printf("cvarSystem->Shutdown();\n");
    cvarSystem->Shutdown();

    // shut down the console command system
    printf("cmdSystem->Shutdown();\n");
    cmdSystem->Shutdown();

    // free any buffered warning messages
    printf("ClearWarnings( GAME_NAME \" shutdown\" );\n");
    ClearWarnings(GAME_NAME " shutdown");
    printf("warningCaption.Clear();\n");
    warningCaption.Clear();
    printf("errorList.Clear();\n");
    errorList.Clear();

    // shutdown idLib
    printf("idLib::ShutDown();\n");
    idLib::ShutDown();
}

/*
========================
idCommonLocal::CreateMainMenu
========================
*/
void idCommonLocal::CreateMainMenu()
{
    if (game != NULL) {
        // note which media we are going to need to load
        declManager->BeginLevelLoad();
        renderSystem->BeginLevelLoad();
        soundSystem->BeginLevelLoad();
        uiManager->BeginLevelLoad();

        // create main inside an "empty" game level load - so assets get
        // purged automagically when we transition to a "real" map
        VITA_PHASE("Shell_CreateMenu");
        game->Shell_CreateMenu(false);
        VITA_PHASE("Shell_Show");
        game->Shell_Show(true);
        VITA_PHASE("Shell_SyncWithSession");
        game->Shell_SyncWithSession();

        // load
        VITA_PHASE("renderSystem->EndLevelLoad");
        renderSystem->EndLevelLoad();
        VITA_PHASE("soundSystem->EndLevelLoad");
        soundSystem->EndLevelLoad();
        VITA_PHASE("declManager->EndLevelLoad");
        declManager->EndLevelLoad();
        VITA_PHASE("uiManager->EndLevelLoad");
        uiManager->EndLevelLoad("");
        VITA_PHASE("CreateMainMenu done");
    }
}

/*
===============
idCommonLocal::Stop

called on errors and game exits
===============
*/
void idCommonLocal::Stop(bool resetSession)
{
    ClearWipe();

    // clear mapSpawned and demo playing flags
    UnloadMap();

    soundSystem->StopAllSounds();

    insideUpdateScreen = false;
    insideExecuteMapChange = false;

    // drop all guis
    ExitMenu();

    if (resetSession) {
        session->QuitMatchToTitle();
    }
}

/*
===============
idCommonLocal::BusyWait
===============
*/
void idCommonLocal::BusyWait()
{
    Sys_GenerateEvents();

    const bool captureToImage = false;
    UpdateScreen(captureToImage);

    session->UpdateSignInManager();
    session->Pump();
}

/*
===============
idCommonLocal::WaitForSessionState
===============
*/
bool idCommonLocal::WaitForSessionState(idSession::sessionState_t desiredState)
{
    if (session->GetState() == desiredState) {
        return true;
    }

    while (true) {
        BusyWait();

        idSession::sessionState_t sessionState = session->GetState();
        if (sessionState == desiredState) {
            return true;
        }
        if (sessionState != idSession::LOADING && sessionState != idSession::SEARCHING && sessionState != idSession::CONNECTING && sessionState != idSession::BUSY && sessionState != desiredState) {
            return false;
        }

        Sys_Sleep(10);
    }
}

/*
========================
idCommonLocal::LeaveGame
========================
*/
void idCommonLocal::LeaveGame()
{

    const bool captureToImage = false;
    UpdateScreen(captureToImage);

    ResetNetworkingState();

    Stop(false);

    CreateMainMenu();

    StartMenu();
}

/*
===============
idCommonLocal::ProcessEvent
===============
*/
bool idCommonLocal::ProcessEvent(const sysEvent_t* event)
{
    // hitting escape anywhere brings up the menu
    if (game && game->IsInGame()) {
        if (event->evType == SE_KEY && event->evValue2 == 1 && (event->evValue == K_ESCAPE || event->evValue == K_JOY9)) {
            if (!game->Shell_IsActive()) {

                // menus / etc
                if (MenuEvent(event)) {
                    return true;
                }

                console->Close();

                StartMenu();
                return true;
            } else {
                console->Close();

                // menus / etc
                if (MenuEvent(event)) {
                    return true;
                }

                game->Shell_ClosePause();
            }
        }
    }

    // let the pull-down console take it if desired
    if (console->ProcessEvent(event, false)) {
        return true;
    }
    if (session->ProcessInputEvent(event)) {
        return true;
    }

    if (Dialog().IsDialogActive()) {
        Dialog().HandleDialogEvent(event);
        return true;
    }

    // RB begin
#if defined(USE_DOOMCLASSIC)

    // Let Doom classic run events.
    if (IsPlayingDoomClassic()) {
        // Translate the event to Doom classic format.
        event_t classicEvent;
        if (event->evType == SE_KEY) {

            if (event->evValue2 == 1) {
                classicEvent.type = ev_keydown;
            } else if (event->evValue2 == 0) {
                classicEvent.type = ev_keyup;
            }

#if defined(__vita__)
            // Every key goes to the classic game, so command bindings never run:
            // the rear-touch 180 degree turn is one (it turns the DOOM 3 view,
            // which the classic tic builder follows).
            if (event->evValue2 == 1 && idStr::Icmp(idKeyInput::GetBinding(event->GetKey()), "vita_quickTurn") == 0) {
                cmdSystem->BufferCommandText(CMD_EXEC_NOW, "vita_quickTurn\n");
            }
#endif
            DoomLib::SetPlayer(0);

            extern Globals* g;
            if (g != NULL) {
                classicEvent.data1 = DoomLib::RemapControl(event->GetKey());

                D_PostEvent(&classicEvent);
            }
            DoomLib::SetPlayer(-1);
        }

        // Let the classics eat all events.
        return true;
    }
#endif
    // RB end

    // menus / etc
    if (MenuEvent(event)) {
        return true;
    }

    // if we aren't in a game, force the console to take it
    if (!mapSpawned) {
        console->ProcessEvent(event, true);
        return true;
    }

    // in game, exec bindings for all key downs
    if (event->evType == SE_KEY && event->evValue2 == 1) {
        idKeyInput::ExecKeyBinding(event->evValue);
        return true;
    }

    return false;
}

/*
========================
idCommonLocal::ResetPlayerInput
========================
*/
void idCommonLocal::ResetPlayerInput(int playerIndex)
{
    userCmdMgr.ResetPlayer(playerIndex);
}

// RB begin
#if defined(USE_DOOMCLASSIC)

/*
========================
idCommonLocal::SwitchToGame
========================
*/
void idCommonLocal::SwitchToGame(currentGame_t newGame)
{
    idealCurrentGame = newGame;
}

/*
========================
idCommonLocal::PerformGameSwitch
========================
*/
void idCommonLocal::PerformGameSwitch()
{
    // If the session state is past the menu, we should be in Doom 3.
    // This will happen if, for example, we accept an invite while playing
    // Doom or Doom 2.
    if (session->GetState() > idSession::IDLE) {
        idealCurrentGame = DOOM3_BFG;
    }

    if (currentGame == idealCurrentGame) {
        return;
    }

    const int DOOM_CLASSIC_HZ = 35;

    if (idealCurrentGame == DOOM_CLASSIC || idealCurrentGame == DOOM2_CLASSIC) {
        // Pause Doom 3 sound.
        if (menuSoundWorld != NULL) {
            menuSoundWorld->Pause();
        }

        DoomLib::skipToNew = false;
        DoomLib::skipToLoad = false;

        // Reset match parameters for the classics.
        DoomLib::matchParms = idMatchParameters();

        // The classics use the usercmd manager too, clear it.
        userCmdMgr.SetDefaults();

        // Classics need a local user too.
        session->UpdateSignInManager();
        session->GetSignInManager().RegisterLocalUser(0);

        com_engineHz_denominator = 100LL * DOOM_CLASSIC_HZ;
        com_engineHz_latched = DOOM_CLASSIC_HZ;

        DoomLib::SetCurrentExpansion(idealCurrentGame);

    } else if (idealCurrentGame == DOOM3_BFG) {
        DoomLib::Interface.Shutdown();
#if defined(__vita__)
        com_engineHz_denominator = 100LL * 60; // back to the menus
        com_engineHz_latched = 60.0f;
#else
        com_engineHz_denominator = 100LL * com_engineHz.GetFloat();
        com_engineHz_latched = com_engineHz.GetFloat();
#endif

        // Don't MoveToPressStart if we have an invite, we need to go
        // directly to the lobby.
        if (session->GetState() <= idSession::IDLE) {
            session->MoveToPressStart();
        }

        // Unpause Doom 3 sound.
        if (menuSoundWorld != NULL) {
            menuSoundWorld->UnPause();
        }
    }

    currentGame = idealCurrentGame;
}

#endif // #if defined(USE_DOOMCLASSIC)
// RB end

/*
==================
Common_WritePrecache_f
==================
*/
CONSOLE_COMMAND(writePrecache, "writes precache commands", NULL)
{
    if (args.Argc() != 2) {
        common->Printf("USAGE: writePrecache <execFile>\n");
        return;
    }
    idStr str = args.Argv(1);
    str.DefaultFileExtension(".cfg");
    idFile* f = fileSystem->OpenFileWrite(str);
    declManager->WritePrecacheCommands(f);
    renderModelManager->WritePrecacheCommands(f);
    uiManager->WritePrecacheCommands(f);

    fileSystem->CloseFile(f);
}

/*
================
Common_Disconnect_f
================
*/
CONSOLE_COMMAND_SHIP(disconnect, "disconnects from a game", NULL)
{
    session->QuitMatch();
}

/*
===============
Common_Hitch_f
===============
*/
CONSOLE_COMMAND(hitch, "hitches the game", NULL)
{
    if (args.Argc() == 2) {
        Sys_Sleep(atoi(args.Argv(1)));
    } else {
        Sys_Sleep(100);
    }
}

CONSOLE_COMMAND(showStringMemory, "shows memory used by strings", NULL)
{
    idStr::ShowMemoryUsage_f(args);
}
CONSOLE_COMMAND(showDictMemory, "shows memory used by dictionaries", NULL)
{
    idDict::ShowMemoryUsage_f(args);
}
CONSOLE_COMMAND(listDictKeys, "lists all keys used by dictionaries", NULL)
{
    idDict::ListKeys_f(args);
}
CONSOLE_COMMAND(listDictValues, "lists all values used by dictionaries", NULL)
{
    idDict::ListValues_f(args);
}
CONSOLE_COMMAND(testSIMD, "test SIMD code", NULL)
{
    idSIMD::Test_f(args);
}

// RB begin
CONSOLE_COMMAND(testFormattingSizes, "test printf format security", 0)
{
    common->Printf(" sizeof( int32 ): %" PRIuSIZE " bytes\n", sizeof(int32));
    common->Printf(" sizeof( int64 ): %" PRIuSIZE " bytes\n", sizeof(int64));
}
// RB end
