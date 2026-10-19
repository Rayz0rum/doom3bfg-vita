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
#pragma hdrstop

#include "Common_local.h"
#if defined(ID_TEST_MATH_NEON) && !defined(__vita__)
#include <xmmintrin.h>
#endif
#if defined(__vita__)
#include <arm_neon.h>
#endif
#include "../renderer/Image.h"
#include "../renderer/ImageOpts.h"

// RB begin
#if defined(USE_DOOMCLASSIC)
#include "../../doomclassic/doom/doomlib.h"
#include "../../doomclassic/doom/globaldata.h"
#endif
// RB end

/*

New for tech4x:

Unlike previous SMP work, the actual GPU command drawing is done in the main thread, which avoids the
OpenGL problems with needing windows to be created by the same thread that creates the context, as well
as the issues with passing context ownership back and forth on the 360.

The game tic and the generation of the draw command list is now run in a separate thread, and overlapped
with the interpretation of the previous draw command list.

While the game tic should be nicely contained, the draw command generation winds through the user interface
code, and is potentially hazardous.  For now, the overlap will be restricted to the renderer back end,
which should also be nicely contained.

*/
// a phase of the main thread's frame: the watchdog's mark, and the benchmark's
// timer (VitaBench.cpp) - every millisecond of a frame is counted in a phase
void VitaBench_Phase(const char* name);
#define FRAME_PHASE(s) do { VITA_MARK(s); VitaBench_Phase(s); } while (0)

#include "../sys/vita/vita_zones.h" // GD_SCOPE: the game/draw thread's tree of passes (VitaBench.cpp)
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
void VitaGD_FrameBegin();
void VitaGD_FrameEnd();
#endif

#if defined(__vita__)
// The stock frame updates the sound system on the main thread after it has
// waited for the game/draw thread - with that thread idle and the back end
// done, so the update (3-12 ms a frame in the benchmark: voices, streaming
// decode, OpenAL calls) was pure serial time on top of both. It now runs at
// the end of the game/draw thread's work instead, alongside the back end.
// Safe for the same reason the stock order was: the sound system is still
// used by one thread at a time - the main thread only touches it (menu
// events, pause, map loads, saves) while the game/draw thread is idle, and
// the game code that starts sounds runs on the game/draw thread before the
// update, as before.
idCVar vita_soundHalfRate("vita_soundHalfRate", "1", CVAR_BOOL | CVAR_ARCHIVE, "Vita: update the sound system every other frame (decoding and mixing are unaffected)");
idCVar vita_soundOnGameThread("vita_soundOnGameThread", "1", CVAR_BOOL, "Vita: update the sound system at the end of the game/draw thread's frame, alongside the render back end, instead of on the main thread after waiting for it");
volatile int vita_soundUs = 0; // sound updates on the game/draw thread, us (perf log, benchmark)
// when the game/draw thread's current frame began (0 between frames): the
// watchdog samples that thread when a frame runs long (vita_main.cpp)
volatile uint64 vita_gdFrameStartUs = 0;
static int vitaPerfSoundUsAtStart = 0; // vita_soundUs when the perf window began
#endif

#define DEFAULT_FIXED_TIC "0"
#define DEFAULT_NO_SLEEP "0"

// Stock 50 ms (three 60 Hz tics) on the Vita too. It was 36 ms (two tics) to
// keep a slow frame from adding a third, but below 30 fps that ran the game
// clock slower than real time while speech and music play in real time: an
// NPC's lip sync fell behind his voice and the whole world slowed down.
idCVar com_deltaTimeClamp("com_deltaTimeClamp", "50", CVAR_INTEGER, "don't process more than this time in a single frame");
#if defined(__vita__)
idCVar vita_maxTicsPerFrame("vita_maxTicsPerFrame", "1", CVAR_SYSTEM | CVAR_INTEGER | CVAR_ARCHIVE, "Vita: game tics run in one frame at most; a slower frame slows the game down instead of catching up (0 = catch up, stock)");
int vita_ticsDropped = 0; // tics not played because of vita_maxTicsPerFrame, for the perf log
#endif

idCVar com_fixedTic("com_fixedTic", DEFAULT_FIXED_TIC, CVAR_BOOL, "run a single game frame per render frame");
idCVar com_noSleep("com_noSleep", DEFAULT_NO_SLEEP, CVAR_BOOL, "don't sleep if the game is running too fast");
idCVar com_smp("com_smp", "1", CVAR_BOOL | CVAR_SYSTEM | CVAR_NOCHEAT, "run the game and draw code in a separate thread");
#if defined(__vita__)
idCVar vita_maxFPS("vita_maxFPS", "30", CVAR_ARCHIVE | CVAR_INTEGER,
    "Vita: frame cap for DOOM 3 (menus and game); 0 = vsync only. Doom/Doom II always run at 35", 0, 60);
// The 30 fps cap used to be a sleep on the main thread with vsync on every
// vblank. The main thread shares core 0 with OpenAL's mixer, which runs above
// it, so the sleep often woke 1-6 ms late; the cap then made up for it on the
// next frames. In the benchmark's lightest views - main thread idle 15 ms a
// frame - 45% of the frames took 33.4-40 ms, and on a 60 Hz screen those were
// shown for three vblanks instead of two: judder, and frames the dynamic
// resolution took for slow ones. Paced by the display instead (vsync on every
// second vblank, the display queue holding the main thread back), each frame
// is on screen for exactly two vblanks whenever the work fits.
idCVar vita_displayPacing("vita_displayPacing", "1", CVAR_BOOL,
    "Vita: at the 30 fps cap, pace frames on every second vblank (1) instead of with a sleep on the main thread and vsync on every vblank (0)");
static int vitaAppliedSwapInterval = -1; // what Frame() last gave vitaGL (-1: apply again)
// The 20 fps fallback (requested 2026-10-17): at the 30 fps cap a frame that
// takes over 43.4 ms is on screen for three vblanks instead of two, and a
// mix of 2- and 3-vblank frames judders (20-27 fps in Underground Junction
// with the flashlight). When frames run over 43.4 ms (4 of the last 10) the
// cap drops to 20 - every frame exactly three vblanks; it goes back to 30
// once the work (frame time minus the vblank and cap waits) has stayed
// under 28 ms for 3 s.
idCVar vita_cap20Fallback("vita_cap20Fallback", "1", CVAR_BOOL | CVAR_ARCHIVE, "Vita: drop the 30 fps cap to 20 while frames take over 43.4 ms (steady three vblanks instead of a 2/3 mix)");
static bool vitaCap20 = false;
static bool vitaDisplayPacedNow = false;  // this frame is paced by the display (vita_displayPacing)
void GLimp_SetSwapInterval(int interval); // sdl_glimp.cpp
// a map load presents its loading screen a few times a second: no vsync wait
// there (two vblanks a present at the 30 fps cap); Frame() puts it back
void Vita_LoadingSwapInterval()
{
    GLimp_SetSwapInterval(0);
    vitaAppliedSwapInterval = -1;
}
// Default: BFG's own sizing, exactly as DOOM 3 BFG shows the classics on a PC
// (805x544 here: the same shape as its 1600x1080 at 1920x1080). Renamed from
// vita_classicScreen so configs that saved the old 16:10 default pick it up.
#define VITA_CLASSIC_ASPECT_DEFAULT "0"
idCVar vita_classicAspect("vita_classicView", VITA_CLASSIC_ASPECT_DEFAULT, CVAR_ARCHIVE | CVAR_INTEGER,
    "Vita: how DOOM/DOOM II fill the screen - 0 = as DOOM 3 BFG on a PC, 1 = exact 4:3, 3 = 870x544 (16:10, full height), 2 = all of it", 0, 3);
idCVar vita_dynamicResolution("vita_dynamicResolution", "1", CVAR_ARCHIVE | CVAR_BOOL,
    "Vita: move the 3D view between 720x408 and 640x360 - down while 30 fps cannot be held or memory is tight, back up when there is time to spare");
extern int vita_dynamicResolutionStep;
extern int vita_textureQualityDrop;
int Vita_DynamicResolutionSteps();
void Vita_RenderSizeForStep(int step, int* w, int* h);
static float vitaEmaBackEnd = 0.0f; // smoothed render back end time (light cap, dynamic resolution)
static int vitaBackEndOverRun = 0;   // back ends over 36 ms in a row (emergency resolution step)
static int Vita_LogRingFullNow()
{
    extern volatile int vita_logRingFullWrites; // vita_main.cpp
    const int n = vita_logRingFullWrites;
    vita_logRingFullWrites = 0;
    return n;
}
static int Vita_LightCapNow()
{
    extern int vita_viewLightCap; // tr_frontend_addlights.cpp
    return vita_viewLightCap;
}
static int Vita_LoadStepNow()
{
    extern int vita_loadStep; // tr_frontend_addlights.cpp
    return vita_loadStep;
}
void Vita_FreeMemoryKB(unsigned int* allFreeKB, unsigned int* cachedFreeKB); // vita_main.cpp
void Vita_NoteGameThread(); // vita_main.cpp
void Vita_LogThreadCpu(double frames); // vita_main.cpp
extern char vita_threadError[];               // Thread.cpp
extern volatile bool vita_threadErrorPending; // Thread.cpp
idCVar vita_perfLog("vita_perfLog", "300", CVAR_INTEGER,
    "Vita: every this many frames, log where DOOM 3's frame time went (0 = off)");

// Frame time breakdown, summed between perf log lines. The game/draw thread
// runs alongside the render back end, so a frame costs roughly
// max(game + draw, back end) + the wait for the GPU and the frame cap.
static uint64 vitaPerfFrameUs = 0, vitaPerfWorstUs = 0, vitaPerfGameUs = 0, vitaPerfWorstGameUs = 0;
static uint64 vitaPerfDrawUs = 0, vitaPerfBackendUs = 0, vitaPerfWorstBackendUs = 0, vitaPerfSyncUs = 0, vitaPerfCapUs = 0;
static int vitaPerfFrames = 0, vitaPerfTics = 0;
static uint64 vitaPerfLastFrameStart = 0;
static uint64 vitaPerfCapThisFrame = 0;
static uint64 vitaPerfGameWaitThisFrame = 0; // main thread waiting for the game/draw thread
static uint64 vitaPerfGameWaitUs = 0;
static uint64 vitaPerfMemCheckUs = 0; // worst cost of the memory check since the last perf line
static int vitaPerfCapHz = 0;          // the frame cap in force (see the fps cap in Frame)
extern idCVar r_swapInterval;
void GLimp_SetSwapInterval(int interval); // sdl_glimp.cpp
extern uint64 vita_frontEndUs[6];     // tr_frontend_main.cpp
// the benchmark asks for the perf lines at the end of each of its segments
static bool vitaPerfFlushRequested = false;
static idStr vitaPerfFlushLabel;
void Vita_PerfFlush(const char* label)
{
    vitaPerfFlushRequested = true;
    vitaPerfFlushLabel = label;
}
#endif
idCVar com_aviDemoSamples("com_aviDemoSamples", "16", CVAR_SYSTEM, "");
idCVar com_aviDemoWidth("com_aviDemoWidth", "256", CVAR_SYSTEM, "");
idCVar com_aviDemoHeight("com_aviDemoHeight", "256", CVAR_SYSTEM, "");
idCVar com_skipGameDraw("com_skipGameDraw", "0", CVAR_SYSTEM | CVAR_BOOL, "");

idCVar com_sleepGame("com_sleepGame", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the game time");
idCVar com_sleepDraw("com_sleepDraw", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the draw time");
idCVar com_sleepRender("com_sleepRender", "0", CVAR_SYSTEM | CVAR_INTEGER, "intentionally add a sleep in the render time");

idCVar net_drawDebugHud("net_drawDebugHud", "0", CVAR_SYSTEM | CVAR_INTEGER, "0 = None, 1 = Hud 1, 2 = Hud 2, 3 = Snapshots");

idCVar timescale("timescale", "1", CVAR_SYSTEM | CVAR_FLOAT, "Number of game frames to run per render frame", 0.001f, 100.0f);

extern idCVar in_useJoystick;
extern idCVar in_joystickRumble;

/*
===============
idGameThread::Run

Run in a background thread for performance, but can also
be called directly in the foreground thread for comparison.
===============
*/
int idGameThread::Run()
{
#if defined(__vita__)
    Vita_NoteGameThread();
    vita_gdFrameStartUs = Sys_Microseconds();
#endif
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VitaGD_FrameBegin();
    int gdPass = VitaGD_Push("game tics");
#endif
    VITA_MARK_GAME("game tics");
    commonLocal.frameTiming.startGameTime = Sys_Microseconds();

    // debugging tool to test frame dropping behavior
    if (com_sleepGame.GetInteger()) {
        Sys_Sleep(com_sleepGame.GetInteger());
    }

    if (numGameFrames == 0) {
        // Ensure there's no stale gameReturn data from a paused game
        ret = gameReturn_t();
    }

    if (isClient) {
        // run the game logic
        for (int i = 0; i < numGameFrames; i++) {
            SCOPED_PROFILE_EVENT("Client Prediction");
            if (userCmdMgr) {
                game->ClientRunFrame(*userCmdMgr, (i == numGameFrames - 1), ret);
            }
            if (ret.syncNextGameFrame || ret.sessionCommand[0] != 0) {
                break;
            }
        }
    } else {
        // run the game logic
        for (int i = 0; i < numGameFrames; i++) {
            SCOPED_PROFILE_EVENT("GameTic");
            if (userCmdMgr) {
                game->RunFrame(*userCmdMgr, ret);
            }
            if (ret.syncNextGameFrame || ret.sessionCommand[0] != 0) {
                break;
            }
        }
    }

    // we should have consumed all of our usercmds
    if (userCmdMgr) {
        // RB begin
#if defined(USE_DOOMCLASSIC)
        if (userCmdMgr->HasUserCmdForPlayer(game->GetLocalClientNum()) && common->GetCurrentGame() == DOOM3_BFG)
#else
        if (userCmdMgr->HasUserCmdForPlayer(game->GetLocalClientNum()))
#endif
        // RB end
        {
            idLib::Printf("idGameThread::Run: didn't consume all usercmds\n");
        }
    }

    commonLocal.frameTiming.finishGameTime = Sys_Microseconds();

    SetThreadGameTime((commonLocal.frameTiming.finishGameTime - commonLocal.frameTiming.startGameTime) / 1000);

    // build render commands and geometry
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VitaGD_Pop(gdPass);
    gdPass = VitaGD_Push("draw (game view, front end, HUD, menus)");
#endif
    VITA_MARK_GAME("draw");
    {
        SCOPED_PROFILE_EVENT("Draw");
        commonLocal.Draw();
    }
    VITA_MARK_GAME("idle");
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VitaGD_Pop(gdPass);
#endif

    commonLocal.frameTiming.finishDrawTime = Sys_Microseconds();

    SetThreadRenderTime((commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.finishGameTime) / 1000);

    SetThreadTotalTime((commonLocal.frameTiming.finishDrawTime - commonLocal.frameTiming.startGameTime) / 1000);

#if defined(__vita__)
    // Vita: the sound update (voices, positions, volumes, starts and stops;
    // decoding and mixing run on their own threads) every other frame -
    // vita_soundHalfRate, a cut for the fights: 1.3-1.7 ms of the game/draw
    // thread a frame there
    static int vitaSoundFrame = 0;
    const bool vitaSoundThisFrame = !vita_soundHalfRate.GetBool() || ((vitaSoundFrame++ & 1) == 0);
    if (renderSound && !vitaSoundThisFrame) {
        VITA_ZONE_COUNT("sound updates skipped (vita_soundHalfRate: every other frame)");
    }
    if (renderSound && vitaSoundThisFrame) {
        GD_SCOPE("sound update");
        VITA_MARK_GAME("sound");
        soundSystem->Render();
        const uint64 soundEnd = Sys_Microseconds();
        __sync_fetch_and_add(&vita_soundUs, (int)(soundEnd - commonLocal.frameTiming.finishDrawTime));
        VITA_MARK_GAME("idle");
    }
#endif
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VitaGD_FrameEnd();
#endif
#if defined(__vita__)
    vita_gdFrameStartUs = 0;
#endif

    return 0;
}

/*
===============
idGameThread::RunGameAndDraw

===============
*/
gameReturn_t idGameThread::RunGameAndDraw(int numGameFrames_, idUserCmdMgr& userCmdMgr_, bool isClient_, int startGameFrame)
{
    // this should always immediately return
    this->WaitForThread();

    // save the usercmds for the background thread to pick up
    userCmdMgr = &userCmdMgr_;

    isClient = isClient_;

    // grab the return value created by the last thread execution
    gameReturn_t latchedRet = ret;

    numGameFrames = numGameFrames_;

#if defined(__vita__)
    renderSound = vita_soundOnGameThread.GetBool();
#endif

    // start the thread going
    if (com_smp.GetBool() == false) {
        // run it in the main thread so PIX profiling catches everything
        Run();
    } else {
        this->SignalWork();
    }

    // return the latched result while the thread runs in the background
    return latchedRet;
}

/*
===============
idCommonLocal::DrawWipeModel

Draw the fade material over everything that has been drawn
===============
*/
void idCommonLocal::DrawWipeModel()
{

    if (wipeStartTime >= wipeStopTime) {
        return;
    }

    int currentTime = Sys_Milliseconds();

    if (!wipeHold && currentTime > wipeStopTime) {
        return;
    }

    float fade = (float)(currentTime - wipeStartTime) / (wipeStopTime - wipeStartTime);
    renderSystem->SetColor4(1, 1, 1, fade);
    renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, wipeMaterial);
}

/*
===============
idCommonLocal::Draw
===============
*/

void idCommonLocal::Draw()
{
    // debugging tool to test frame dropping behavior
    if (com_sleepDraw.GetInteger()) {
        Sys_Sleep(com_sleepDraw.GetInteger());
    }

    if (loadGUI != NULL) {
        loadGUI->Render(renderSystem, Sys_Milliseconds());
    }
    // RB begin
#if defined(USE_DOOMCLASSIC)
    else if (currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC) {
        const float sysWidth = renderSystem->GetWidth() * renderSystem->GetPixelAspect();
        const float sysHeight = renderSystem->GetHeight();
        const float sysAspect = sysWidth / sysHeight;
        const float doomAspect = 4.0f / 3.0f;
        const float adjustment = sysAspect / doomAspect;
#if defined(__vita__)
        // vita_classicAspect: 0 (default) is BFG's own sizing, the PC look -
        // slightly wider than 4:3, 805x544; 1 is an exact 4:3 window, 3 the
        // frame's own 16:10 at full height (870x544), 2 the whole screen.
        float barHeight = 0.0f;
        float barWidth = 0.0f;
        if (vita_classicAspect.GetInteger() == 0) {
            barHeight = (adjustment >= 1.0f) ? 0.0f : (1.0f - adjustment) * (float)renderSystem->GetVirtualHeight() * 0.25f;
            barWidth = (adjustment <= 1.0f) ? 0.0f : (adjustment - 1.0f) * (float)renderSystem->GetVirtualWidth() * 0.25f;
        } else if (vita_classicAspect.GetInteger() == 1) {
            barWidth = (adjustment <= 1.0f) ? 0.0f : (1.0f - 1.0f / adjustment) * 0.5f * (float)renderSystem->GetVirtualWidth();
            barHeight = (adjustment >= 1.0f) ? 0.0f : (1.0f - adjustment) * 0.5f * (float)renderSystem->GetVirtualHeight();
        } else if (vita_classicAspect.GetInteger() == 3) {
            // 16:10, the frame's own shape (320x200 / 960x600), as tall as the
            // screen: 870x544 with 45 px bars either side on the Vita
            const float frameAspect = 1.6f;
            barWidth = (sysAspect <= frameAspect) ? 0.0f : (1.0f - frameAspect / sysAspect) * 0.5f * (float)renderSystem->GetVirtualWidth();
        }
#else
        const float barHeight = (adjustment >= 1.0f) ? 0.0f : (1.0f - adjustment) * (float)renderSystem->GetVirtualHeight() * 0.25f;
        const float barWidth = (adjustment <= 1.0f) ? 0.0f : (adjustment - 1.0f) * (float)renderSystem->GetVirtualWidth() * 0.25f;
#endif
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
        renderSystem->DrawStretchPic(barWidth, barHeight, renderSystem->GetVirtualWidth() - barWidth * 2.0f, renderSystem->GetVirtualHeight() - barHeight * 2.0f, 0, 0, 1, 1, doomClassicMaterial);
    }
#endif
    // RB end
    else if (game && game->Shell_IsActive()) {
        VITA_MARK_GAME("draw: game view under the shell");
        GD_SCOPE("menus (the shell) over the game view");
        bool gameDraw = game->Draw(game->GetLocalClientNum());
        if (!gameDraw) {
            renderSystem->SetColor(colorBlack);
            renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial);
        }
        VITA_MARK_GAME("draw: shell (menus)");
        game->Shell_Render();
    } else if (readDemo) {
        renderWorld->RenderScene(&currentDemoRenderView);
        renderSystem->DrawDemoPics();
    } else if (mapSpawned) {
        bool gameDraw = false;
        // normal drawing for both single and multi player
        if (!com_skipGameDraw.GetBool() && Game()->GetLocalClientNum() >= 0) {
            // draw the game view
            int start = Sys_Milliseconds();
            if (game) {
                GD_SCOPE("game view (idGameLocal::Draw)");
                gameDraw = game->Draw(Game()->GetLocalClientNum());
            }
            int end = Sys_Milliseconds();
            time_gameDraw += (end - start); // note time used for com_speeds
        }
        if (!gameDraw) {
            renderSystem->SetColor(colorBlack);
            renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial);
        }

        // save off the 2D drawing from the game
        if (writeDemo) {
            renderSystem->WriteDemoPics();
        }
    } else {
        renderSystem->SetColor4(0, 0, 0, 1);
        renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, whiteMaterial);
    }

    {
        SCOPED_PROFILE_EVENT("Post-Draw");
        GD_SCOPE("wipe, dialogs, console");

        // draw the wipe material on top of this if it hasn't completed yet
        VITA_MARK_GAME("draw: wipe");
        DrawWipeModel();

        VITA_MARK_GAME("draw: dialogs");
        Dialog().Render(loadGUI != NULL);

        // the verification's video test (VitaVerify.cpp): its video, drawn as a
        // PDA disc or a wall screen is
        extern void VitaVerify_Draw();
        VitaVerify_Draw();

        // draw the half console / notify console on top of everything
        VITA_MARK_GAME("draw: console");
        console->Draw(false);
    }
}

/*
===============
idCommonLocal::UpdateScreen

This is an out-of-sequence screen update, not the normal game rendering
===============
*/
// DG: added possibility to *not* release mouse in UpdateScreen(), it fucks up the view angle for screenshots
void idCommonLocal::UpdateScreen(bool captureToImage, bool releaseMouse)
{
    if (insideUpdateScreen) {
        return;
    }
    insideUpdateScreen = true;

    // make sure the game / draw thread has completed
    gameThread.WaitForThread();

    // release the mouse capture back to the desktop
    if (releaseMouse)
        Sys_GrabMouseCursor(false);
    // DG end

    // build all the draw commands without running a new game tic
    Draw();

    if (captureToImage) {
        renderSystem->CaptureRenderToImage("_currentRender", false);
    }

    // this should exit right after vsync, with the GPU idle and ready to draw
    const emptyCommand_t* cmd = renderSystem->SwapCommandBuffers(&time_frontend, &time_backend, &time_shadows, &time_gpu);

    // get the GPU busy with new commands
    renderSystem->RenderCommandBuffers(cmd);

    insideUpdateScreen = false;
}
/*
================
idCommonLocal::ProcessGameReturn
================
*/
void idCommonLocal::ProcessGameReturn(const gameReturn_t& ret)
{
    // set joystick rumble
    if (in_useJoystick.GetBool() && in_joystickRumble.GetBool() && !game->Shell_IsActive() && session->GetSignInManager().GetMasterInputDevice() >= 0) {
        Sys_SetRumble(session->GetSignInManager().GetMasterInputDevice(), ret.vibrationLow, ret.vibrationHigh); // Only set the rumble on the active controller
    } else {
        for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
            Sys_SetRumble(i, 0, 0);
        }
    }

    syncNextGameFrame = ret.syncNextGameFrame;

    if (ret.sessionCommand[0]) {
        idCmdArgs args;

        args.TokenizeString(ret.sessionCommand, false);

        if (!idStr::Icmp(args.Argv(0), "map")) {
            MoveToNewMap(args.Argv(1), false);
        } else if (!idStr::Icmp(args.Argv(0), "devmap")) {
            MoveToNewMap(args.Argv(1), true);
        } else if (!idStr::Icmp(args.Argv(0), "died")) {
            if (!IsMultiplayer()) {
                game->Shell_Show(true);
            }
        } else if (!idStr::Icmp(args.Argv(0), "disconnect")) {
            cmdSystem->BufferCommandText(CMD_EXEC_INSERT, "stoprecording ; disconnect");
        } else if (!idStr::Icmp(args.Argv(0), "endOfDemo")) {
            cmdSystem->BufferCommandText(CMD_EXEC_NOW, "endOfDemo");
        }
    }
}

extern idCVar com_forceGenericSIMD;

extern idCVar com_pause;

/*
=================
idCommonLocal::Frame
=================
*/
void idCommonLocal::Frame()
{
#if defined(ID_TEST_MATH_NEON) && !defined(__vita__)
    _mm_setcsr(_mm_getcsr() | 0x8040); // the main thread too (see idSysThread::ThreadProc)
#endif
    try {
        SCOPED_PROFILE_EVENT("Common::Frame");

        // This is the only place this is incremented
        idLib::frameNumber++;
        FRAME_PHASE("frame: start");

        // allow changing SIMD usage on the fly
        if (com_forceGenericSIMD.IsModified()) {
            idSIMD::InitProcessor("doom", com_forceGenericSIMD.GetBool());
            com_forceGenericSIMD.ClearModified();
        }

        // RB begin
#if defined(USE_DOOMCLASSIC)
        // Do the actual switch between Doom 3 and the classics here so
        // that things don't get confused in the middle of the frame.
        PerformGameSwitch();
#endif
        // RB end

        // pump all the events
        FRAME_PHASE("frame: input events");
        Sys_GenerateEvents();

        // write config file if anything changed
        FRAME_PHASE("frame: write config");
        WriteConfiguration();

        FRAME_PHASE("frame: console commands");
        eventLoop->RunEventLoop();

        // Activate the shell if it's been requested
        if (showShellRequested && game) {
            FRAME_PHASE("frame: shell show");
            game->Shell_Show(true);
            showShellRequested = false;
        }

        // if the console or another gui is down, we don't need to hold the mouse cursor
        bool chatting = false;

        // DG: Add pause from com_pause cvar
        // RB begin
#if defined(USE_DOOMCLASSIC)
        if (com_pause.GetInteger() || console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing()
            || (game && game->InhibitControls() && !IsPlayingDoomClassic()))
#else
        if (com_pause.GetInteger() || console->Active() || Dialog().IsDialogActive() || session->IsSystemUIShowing()
            || (game && game->InhibitControls()))
#endif
        // RB end, DG end
        {
            Sys_GrabMouseCursor(false);
            usercmdGen->InhibitUsercmd(INHIBIT_SESSION, true);
            chatting = true;
        } else {
            Sys_GrabMouseCursor(true);
            usercmdGen->InhibitUsercmd(INHIBIT_SESSION, false);
        }

        // RB begin
#if defined(USE_DOOMCLASSIC)
        const bool pauseGame = (!mapSpawned
                                   || (!IsMultiplayer()
                                       && (Dialog().IsDialogPausing() || session->IsSystemUIShowing()
                                           || (game && game->Shell_IsActive()) || com_pause.GetInteger())))
            && !IsPlayingDoomClassic();
#else
        const bool pauseGame = (!mapSpawned
            || (!IsMultiplayer()
                && (Dialog().IsDialogPausing() || session->IsSystemUIShowing()
                    || (game && game->Shell_IsActive()) || com_pause.GetInteger())));
#endif
        // RB end

        // save the screenshot and audio from the last draw if needed
        if (aviCaptureMode) {
            idStr name = va("demos/%s/%s_%05i.tga", aviDemoShortName.c_str(), aviDemoShortName.c_str(), aviDemoFrameCount++);
            renderSystem->TakeScreenshot(com_aviDemoWidth.GetInteger(), com_aviDemoHeight.GetInteger(), name, com_aviDemoSamples.GetInteger(), NULL);

            // remove any printed lines at the top before taking the screenshot
            console->ClearNotifyLines();

            // this will call Draw, possibly multiple times if com_aviDemoSamples is > 1
            renderSystem->TakeScreenshot(com_aviDemoWidth.GetInteger(), com_aviDemoHeight.GetInteger(), name, com_aviDemoSamples.GetInteger(), NULL);
        }

        //--------------------------------------------
        // wait for the GPU to finish drawing
        //
        // It is imporant to minimize the time spent between this
        // section and the call to renderSystem->RenderCommandBuffers(),
        // because the GPU is completely idle.
        //--------------------------------------------
        // this should exit right after vsync, with the GPU idle and ready to draw
        // This may block if the GPU isn't finished renderng the previous frame.
        FRAME_PHASE("frame: swap + wait for GPU");
        frameTiming.startSyncTime = Sys_Microseconds();
        const emptyCommand_t* renderCommands = NULL;
        if (com_smp.GetBool()) {
            renderCommands = renderSystem->SwapCommandBuffers(&time_frontend, &time_backend, &time_shadows, &time_gpu);
        } else {
            // the GPU will stay idle through command generation for minimal
            // input latency
            renderSystem->SwapCommandBuffers_FinishRendering(&time_frontend, &time_backend, &time_shadows, &time_gpu);
        }
        frameTiming.finishSyncTime = Sys_Microseconds();

        //--------------------------------------------
        // Determine how many game tics we are going to run,
        // now that the previous frame is completely finished.
        //
        // It is important that any waiting on the GPU be done
        // before this, or there will be a bad stuttering when
        // dropping frames for performance management.
        //--------------------------------------------

        // input:
        // thisFrameTime
        // com_noSleep
        // com_engineHz
        // com_fixedTic
        // com_deltaTimeClamp
        // IsMultiplayer
        //
        // in/out state:
        // gameFrame
        // gameTimeResidual
        // lastFrameTime
        // syncNextFrame
        //
        // Output:
        // numGameFrames

        // How many game frames to run
        int numGameFrames = 0;

#if defined(__vita__)
        // Frame caps, by request:
        //   DOOM / DOOM II, everywhere in them      35 (their full speed)
        //   DOOM 3 title screen and main menu        60
        //   DOOM 3 maps, loading, pause menu         vita_maxFPS (30)
        // Frames start at least one period apart. DOOM 3 keeps simulating at
        // com_engineHz (two game frames per 30 fps frame); the classics run one
        // 35 Hz tic per frame. 35 does not divide 60, so while a classic game
        // runs the swap latches the new frame at the next vblank without
        // waiting for it (vitaGL vsync interval 0 - SETBUF_NEXTFRAME still
        // flips on a vblank, so nothing tears); with the usual blocking vsync
        // every frame was held to two vblanks, 30 fps.
        FRAME_PHASE("frame: fps cap");
        const uint64 vitaCapStart = Sys_Microseconds();
        const bool vitaClassic = (currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC);
        int vitaCapHz = 0;
        bool vitaDisplayPaced = false;
        if (vitaClassic) {
            vitaCapHz = 35;
        } else if (!mapSpawned && game != NULL && game->Shell_IsActive()) {
            vitaCapHz = 60; // title screen and main menu
        } else {
            vitaCapHz = vita_maxFPS.GetInteger();
            if (vitaCapHz == 30 && vitaCap20 && mapSpawned) {
                vitaCapHz = 20; // the 20 fps fallback (vita_cap20Fallback)
            }
        }
        {
            // BFG's r_swapInterval 2 means "always v-sync" (its options menu
            // sets it), but SDL/vitaGL read 2 as "every second vblank", which
            // held everything - the 60 fps title screen too - to 30. Any vsync
            // setting is one vblank here; the 30 fps cap is vita_maxFPS's job
            // - except at the 30 fps cap with vita_displayPacing (above): two
            // vblanks, and no sleep.
            // Under the 35 and 60 fps caps the swap latches the frame at the
            // next vblank without waiting for it (still flipped on a vblank,
            // so nothing tears): with the blocking wait, a 60 fps menu frame
            // that took 17 ms instead of 16.7 was held for a whole second
            // vblank - the game selection screen fell to 40-45 while it
            // scrolled. The limiter above keeps the pace.
            int& appliedSwapInterval = vitaAppliedSwapInterval;
            const bool displayPaced = vita_displayPacing.GetBool() && !vitaClassic && (vitaCapHz == 30 || vitaCapHz == 20) && r_swapInterval.GetInteger() != 0;
            const int wantSwapInterval = (vitaClassic || vitaCapHz >= 60) ? 0 : (r_swapInterval.GetInteger() != 0 ? (displayPaced ? (vitaCapHz == 20 ? 3 : 2) : 1) : 0);
            if (wantSwapInterval != appliedSwapInterval) {
                appliedSwapInterval = wantSwapInterval;
                GLimp_SetSwapInterval(wantSwapInterval);
            }
            vitaDisplayPaced = displayPaced;
            vitaDisplayPacedNow = displayPaced;
        }
        vitaPerfCapHz = vitaCapHz;
        static uint64 vitaNextFrameUs = 0;
        if (vitaDisplayPaced) {
            vitaNextFrameUs = 0; // the display paces the frames (the swap blocks): no sleep; the cap starts afresh if it comes back
        } else if (vitaCapHz > 0) {
            const uint64 periodUs = 1000000 / vitaCapHz;
            uint64 now = Sys_Microseconds();
            if (vitaNextFrameUs == 0 || now > vitaNextFrameUs + periodUs) {
                vitaNextFrameUs = now;      // first frame, or fell behind: no catching up
            }
            while (now < vitaNextFrameUs) {
                const uint64 left = vitaNextFrameUs - now;
                Sys_Sleep(left > 2000 ? (int)((left - 1000) / 1000) : 0);
                now = Sys_Microseconds();
            }
            vitaNextFrameUs += periodUs;
        }
        vitaPerfCapThisFrame = Sys_Microseconds() - vitaCapStart;
#endif

        for (;;) {
            const int thisFrameTime = Sys_Milliseconds();
            static int lastFrameTime = thisFrameTime; // initialized only the first time
            const int deltaMilliseconds = thisFrameTime - lastFrameTime;
            lastFrameTime = thisFrameTime;

            // if there was a large gap in time since the last frame, or the frame
            // rate is very very low, limit the number of frames we will run
#if defined(__vita__)
            // DOOM/DOOM II catch up on up to four 35 Hz tics (RunDoomClassicFrame),
            // so a slow frame costs smoothness there, never game speed
            // at 30 game tics a second two tics are 67 ms: the clamp lets a
            // frame catch up that far, so the game keeps real time to 15 fps
            const int clampMs = (currentGame == DOOM3_BFG) ? Max(com_deltaTimeClamp.GetInteger(), (int)(2000.0f / com_engineHz_latched) + 1) : Max(com_deltaTimeClamp.GetInteger(), 115);
            const int clampedDeltaMilliseconds = Min(deltaMilliseconds, clampMs);
#else
            const int clampedDeltaMilliseconds = Min(deltaMilliseconds, com_deltaTimeClamp.GetInteger());
#endif

            gameTimeResidual += clampedDeltaMilliseconds * timescale.GetFloat();

            // don't run any frames when paused
            if (pauseGame) {
                gameFrame++;
                gameTimeResidual = 0;
                break;
            }

            // debug cvar to force multiple game tics
            if (com_fixedTic.GetInteger() > 0) {
                numGameFrames = com_fixedTic.GetInteger();
                gameFrame += numGameFrames;
                gameTimeResidual = 0;
                break;
            }

            if (syncNextGameFrame) {
                // don't sleep at all
                syncNextGameFrame = false;
                gameFrame++;
                numGameFrames++;
                gameTimeResidual = 0;
                break;
            }

            for (;;) {
                // How much time to wait before running the next frame,
                // based on com_engineHz
                const int frameDelay = FRAME_TO_MSEC(gameFrame + 1) - FRAME_TO_MSEC(gameFrame);
                if (gameTimeResidual < frameDelay) {
                    break;
                }
                gameTimeResidual -= frameDelay;
                gameFrame++;
                numGameFrames++;
                // if there is enough residual left, we may run additional frames
            }

#if defined(__vita__)
            // one game tic per frame at most (vita_maxTicsPerFrame, a cut the
            // user approved): a frame that took longer than a tic does not
            // catch up with a second tic - the game runs slower for that frame
            // instead of the next frame costing a tic more, which made a slow
            // frame the cause of the next one. The time not played is dropped.
            // At the 20 fps fallback a frame lasts a tic and a half: two tics
            // are allowed then, or the game would run at two thirds speed.
            const int vitaMaxTics = (vitaCap20 && vita_maxTicsPerFrame.GetInteger() > 0) ? Max(2, vita_maxTicsPerFrame.GetInteger()) : vita_maxTicsPerFrame.GetInteger();
            if (currentGame == DOOM3_BFG && vitaMaxTics > 0 && numGameFrames > vitaMaxTics) {
                const int dropped = numGameFrames - vitaMaxTics;
                gameFrame -= dropped;
                numGameFrames -= dropped;
                gameTimeResidual = 0;
                vita_ticsDropped += dropped;
            }
#endif
            if (numGameFrames > 0) {
                // ready to actually run them
                break;
            }

            // if we are vsyncing, we always want to run at least one game
            // frame and never sleep, which might happen due to scheduling issues
            // if we were just looking at real time.
#if defined(__vita__)
            // paced by the display (vita_displayPacing), a frame that came a
            // little early after a late one must not sleep the rest of a tic
            // away: the sleep woke late (core 0's mixer runs above the main
            // thread) and the frame missed its vblank. Like com_noSleep with
            // vsync, which BFG meant for exactly this.
            if (com_noSleep.GetBool() || vitaDisplayPaced) {
#else
            if (com_noSleep.GetBool()) {
#endif
                numGameFrames = 1;
                gameFrame += numGameFrames;
                gameTimeResidual = 0;
                break;
            }

            // not enough time has passed to run a frame, as might happen if
            // we don't have vsync on, or the monitor is running at 120hz while
            // com_engineHz is 60, so sleep a bit and check again
            Sys_Sleep(0);
        }

        //--------------------------------------------
        // It would be better to push as much of this as possible
        // either before or after the renderSystem->SwapCommandBuffers(),
        // because the GPU is completely idle.
        //--------------------------------------------

        // Update session and syncronize to the new session state after sleeping
        FRAME_PHASE("frame: session pump");
        session->UpdateSignInManager();
        session->Pump();
        session->ProcessSnapAckQueue();

        if (session->GetState() == idSession::LOADING) {
            // If the session reports we should be loading a map, load it!
            FRAME_PHASE("frame: map change");
            ExecuteMapChange();
            mapSpawnData.savegameFile = NULL;
#if defined(__vita__)
            VitaReleaseSaveBuffers("level loaded");
#endif
            mapSpawnData.persistentPlayerInfo.Clear();
            return;
        } else if (session->GetState() != idSession::INGAME && mapSpawned) {
            // If the game is running, but the session reports we are not in a game, disconnect
            // This happens when a server disconnects us or we sign out
            LeaveGame();
            return;
        }

        if (mapSpawned && !pauseGame) {
            if (IsClient()) {
                RunNetworkSnapshotFrame();
            }
        }

        ExecuteReliableMessages();

        // send frame and mouse events to active guis
        FRAME_PHASE("frame: gui events");
        GuiFrameEvents();

        //--------------------------------------------
        // Prepare usercmds and kick off the game processing
        // in a background thread
        //--------------------------------------------

        // get the previous usercmd for bypassed head tracking transform
        const usercmd_t previousCmd = usercmdGen->GetCurrentUsercmd();

        // build a new usercmd
        FRAME_PHASE("frame: usercmd");
        int deviceNum = session->GetSignInManager().GetMasterInputDevice();
        usercmdGen->BuildCurrentUsercmd(deviceNum);
        if (deviceNum == -1) {
            for (int i = 0; i < MAX_INPUT_DEVICES; i++) {
                Sys_PollJoystickInputEvents(i);
                Sys_EndJoystickInputEvents();
            }
        }
        if (pauseGame) {
            usercmdGen->Clear();
        }

        usercmd_t newCmd = usercmdGen->GetCurrentUsercmd();

        // Store server game time - don't let time go past last SS time in case we are extrapolating
        if (IsClient()) {
            newCmd.serverGameMilliseconds = std::min(Game()->GetServerGameTimeMs(), Game()->GetSSEndTime());
        } else {
            newCmd.serverGameMilliseconds = Game()->GetServerGameTimeMs();
        }

        userCmdMgr.MakeReadPtrCurrentForPlayer(Game()->GetLocalClientNum());

        // Stuff a copy of this userCmd for each game frame we are going to run.
        // Ideally, the usercmds would be built in another thread so you could
        // still get 60hz control accuracy when the game is running slower.
        for (int i = 0; i < numGameFrames; i++) {
            newCmd.clientGameMilliseconds = FRAME_TO_MSEC(gameFrame - numGameFrames + i + 1);
            userCmdMgr.PutUserCmdForPlayer(game->GetLocalClientNum(), newCmd);
        }

        // RB begin
#if defined(USE_DOOMCLASSIC)
        // If we're in Doom or Doom 2, run tics and upload the new texture.
        if ((GetCurrentGame() == DOOM_CLASSIC || GetCurrentGame() == DOOM2_CLASSIC) && !(Dialog().IsDialogPausing() || session->IsSystemUIShowing())) {
#if defined(__vita__)
            // advance Doom's clock by every 35 Hz frame that came due, not one per
            // drawn frame: a slow frame then costs smoothness, not game speed
            RunDoomClassicFrame(numGameFrames);
#else
            RunDoomClassicFrame();
#endif
        }
#endif
        // RB end

        // start the game / draw command generation thread going in the background
        FRAME_PHASE("frame: start game/draw");
        gameReturn_t ret = gameThread.RunGameAndDraw(numGameFrames, userCmdMgr, IsClient(), gameFrame - numGameFrames);

        if (!com_smp.GetBool()) {
            // in non-smp mode, run the commands we just generated, instead of
            // frame-delayed ones from a background thread
            renderCommands = renderSystem->SwapCommandBuffers_FinishCommandBuffers();
        }

        //----------------------------------------
        // Run the render back end, getting the GPU busy with new commands
        // ASAP to minimize the pipeline bubble.
        //----------------------------------------
        FRAME_PHASE("frame: render back end");
        frameTiming.startRenderTime = Sys_Microseconds();
        renderSystem->RenderCommandBuffers(renderCommands);
        if (com_sleepRender.GetInteger() > 0) {
            // debug tool to test frame adaption
            Sys_Sleep(com_sleepRender.GetInteger());
        }
        frameTiming.finishRenderTime = Sys_Microseconds();

        // make sure the game / draw thread has completed
        // This may block if the game is taking longer than the render back end
        FRAME_PHASE("frame: wait for game/draw");
        const uint64 benchWaitStart = Sys_Microseconds();
#if defined(__vita__)
        const uint64 vitaWaitStart = benchWaitStart;
        extern void Vita_JobsMainWaiting(bool waiting); // ParallelJobList.cpp: core 0's job worker
        Vita_JobsMainWaiting(true);
        gameThread.WaitForThread();
        Vita_JobsMainWaiting(false);
        vitaPerfGameWaitThisFrame = Sys_Microseconds() - vitaWaitStart;
        if (vita_threadErrorPending) {
            // raised on the game/draw thread: handle it here, where Stop()
            // and the drop back to the menu are safe
            vita_threadErrorPending = false;
            Error("%s", vita_threadError);
        }
#else
        gameThread.WaitForThread();
#endif
        const uint64 benchWaitUs = Sys_Microseconds() - benchWaitStart;

        // Send local usermds to the server.
        // This happens after the game frame has run so that prediction data is up to date.
        SendUsercmds(Game()->GetLocalClientNum());

        // Now that we have an updated game frame, we can send out new snapshots to our clients
        FRAME_PHASE("frame: session pump 2");
        session->Pump(); // Pump to get updated usercmds to relay
        SendSnapshots();

        // Render the sound system using the latest commands from the game thread
        if (pauseGame) {
            soundWorld->Pause();
            soundSystem->SetPlayingSoundWorld(menuSoundWorld);
        } else {
            soundWorld->UnPause();
            soundSystem->SetPlayingSoundWorld(soundWorld);
        }
        FRAME_PHASE("frame: sound");
#if defined(__vita__)
        if (!gameThread.RendersSound()) {
            soundSystem->Render();
        }
#else
        soundSystem->Render();
#endif

        // process the game return for map changes, etc
        FRAME_PHASE("frame: game return");
        ProcessGameReturn(ret);

        idLobbyBase& lobby = session->GetActivePlatformLobbyBase();
        if (lobby.HasActivePeers()) {
            if (net_drawDebugHud.GetInteger() == 1) {
                lobby.DrawDebugNetworkHUD();
            }
            if (net_drawDebugHud.GetInteger() == 2) {
                lobby.DrawDebugNetworkHUD2();
            }
            lobby.DrawDebugNetworkHUD_ServerSnapshotMetrics(net_drawDebugHud.GetInteger() == 3);
        }

        // report timing information
        if (com_speeds.GetBool()) {
            static int lastTime = Sys_Milliseconds();
            int nowTime = Sys_Milliseconds();
            int com_frameMsec = nowTime - lastTime;
            lastTime = nowTime;
            Printf("frame:%d all:%3d gfr:%3d rf:%3lld bk:%3lld\n", idLib::frameNumber, com_frameMsec, time_gameFrame, time_frontend / 1000, time_backend / 1000);
            time_gameFrame = 0;
            time_gameDraw = 0;
        }

        // the FPU stack better be empty at this point or some bad code or compiler bug left values on the stack
        if (!Sys_FPU_StackIsEmpty()) {
            Printf("%s", Sys_FPU_GetState());
            FatalError("idCommon::Frame: the FPU stack is not empty at the end of the frame\n");
        }

        mainFrameTiming = frameTiming;

        // the benchmark (VitaBench.cpp): measures this frame and moves its
        // script on - here, where the game/draw thread is idle
        FRAME_PHASE("frame: benchmark");
        {
            extern void VitaBench_Frame(uint64 gameUs, uint64 drawUs, uint64 backendUs, uint64 syncUs, uint64 waitUs, int tics);
            VitaBench_Frame(frameTiming.finishGameTime - frameTiming.startGameTime, frameTiming.finishDrawTime - frameTiming.finishGameTime,
                frameTiming.finishRenderTime - frameTiming.startRenderTime, frameTiming.finishSyncTime - frameTiming.startSyncTime, benchWaitUs, numGameFrames);
        }
        FRAME_PHASE("frame: perf log");
#if defined(__vita__)
        // on the title screen / main menu: L + R held during the boot, or
        // held here for 1.5 s, starts the benchmark (vita_main.cpp)
        if (!mapSpawned && currentGame == DOOM3_BFG) {
            extern bool VitaBench_Active();
            extern bool Vita_BenchmarkTrigger();
            if (!VitaBench_Active() && Vita_BenchmarkTrigger()) {
                // the full verification (VitaVerify.cpp), which plays the
                // benchmark's heavy scenes among its checks; a run that died
                // is picked up where it stopped
                Printf("vita: L + R - starting the full verification\n");
                cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "verify\n");
            }
        }
#endif

#if defined(__vita__)
        if (vita_perfLog.GetInteger() > 0) {
            const uint64 now = Sys_Microseconds();
            if (vitaPerfLastFrameStart != 0) {
                const uint64 frameUs = now - vitaPerfLastFrameStart;
                const uint64 gameUs = frameTiming.finishGameTime - frameTiming.startGameTime;
                const uint64 drawUs = frameTiming.finishDrawTime - frameTiming.finishGameTime;
                const uint64 backendUs = frameTiming.finishRenderTime - frameTiming.startRenderTime;
                vitaPerfFrameUs += frameUs;
                vitaPerfWorstUs = Max(vitaPerfWorstUs, frameUs);
                vitaPerfGameUs += gameUs;
                vitaPerfWorstGameUs = Max(vitaPerfWorstGameUs, gameUs);
                vitaPerfDrawUs += drawUs;
                vitaPerfBackendUs += backendUs;
                vitaPerfWorstBackendUs = Max(vitaPerfWorstBackendUs, backendUs);
                vitaPerfSyncUs += frameTiming.finishSyncTime - frameTiming.startSyncTime;
                vitaPerfCapUs += vitaPerfCapThisFrame;
                vitaPerfTics += numGameFrames;
                vitaPerfFrames++;

                // dynamic resolution: smoothed frame time, GPU wait and idle time
                static float emaFrame = 33.3f, emaSync = 0.0f, emaCap = 0.0f, emaGameWait = 0.0f;
                static int framesSinceChange = 0;
                const float a = 0.08f;
                const uint64 syncUsThisFrame = frameTiming.finishSyncTime - frameTiming.startSyncTime;
                // the work this frame took: what is left of it without the
                // vblank wait and the cap's sleep
                const float workMs = Max(0.0f, (float)((int64)frameUs - (int64)syncUsThisFrame - (int64)vitaPerfCapThisFrame) / 1000.0f);
                // The 20 fps fallback (vita_cap20Fallback, above), by the user's
                // rule (2026-10-19): 30 fps is the norm and dips are fine; 20
                // fps is the exception for aggressive dips and must not linger
                // past 10 s; under 20 is unplayable.
                //   in:  4 of the last 10 frames over 43.4 ms - and only if a
                //        20 fps frame would fit its 50 ms: at 20 fps the game
                //        thread plays 1.5 tics a frame, so in a game-bound fight
                //        the cap made frames slower (delta2a's fight: 16 fps at
                //        the cap, ~28 fps it would have been at 30)
                //   out: the work a 30 fps frame would take under 33.4 ms for
                //        30 frames (dips are fine at 30); or after 10 s, whatever
                //        (30 is tried again: a scene still too heavy comes back
                //        for another 10 s at most); or at once if even the cap
                //        runs under 20 fps (it is not helping)
                // Never in menus, pauses or loads. The work estimates: at 30
                // fps, the main thread's own work or one tic of game plus the
                // front end, whichever is longer; at 20 fps, the main thread's
                // or a tic and a half plus the front end.
                {
                    static unsigned int slowBits = 0;
                    static int fastFrames = 0;
                    static int uselessFrames = 0;
                    static uint64 capSinceUs = 0;
                    static float emaWork30 = 20.0f;
                    static float emaWork20 = 30.0f;
                    static uint64 refusedLogUs = 0;
                    const float gameWaitMs = vitaPerfGameWaitThisFrame / 1000.0f;
                    const float gameTicMs = (numGameFrames > 0) ? (float)gameUs / 1000.0f / numGameFrames : 0.0f;
                    const float mainOwnMs = Max(0.0f, workMs - gameWaitMs);
                    const float work30 = Max(mainOwnMs, gameTicMs + drawUs / 1000.0f);
                    const float work20 = Max(mainOwnMs, 1.5f * gameTicMs + drawUs / 1000.0f);
                    emaWork30 += 0.1f * (work30 - emaWork30);
                    emaWork20 += 0.1f * (work20 - emaWork20);
                    const bool inPlay = mapSpawned && !pauseGame && vita_maxFPS.GetInteger() == 30 && vita_cap20Fallback.GetBool();
                    slowBits = ((slowBits << 1) | (frameUs > 43400 ? 1u : 0u)) & 0x3FFu;
                    int slow = 0;
                    for (unsigned int b = slowBits; b != 0; b &= b - 1) {
                        slow++;
                    }
                    extern void VitaBench_Log(const char* fmt, ...);
                    extern bool VitaBench_Active(); // (the cap's stretches go into its results)
                    // a frame over 1.5 s is a load (the verification's save
                    // check reloads the map, ~90 s with no frames) or a stall:
                    // the cap ends there, counted up to the frame before it.
                    // Run 7 logged "off after 100.8 s (10 s at 20)" - all but a
                    // few seconds of it was the load
                    const bool gap = frameUs > 1500000;
                    if (!inPlay || gap) {
                        if (vitaCap20) {
                            vitaCap20 = false;
                            const double playedS = ((gap ? vitaPerfLastFrameStart : now) - capSinceUs) / 1000000.0;
                            const char* why = gap ? "a load or a stall: no frame for over 1.5 s" : "left play";
                            Printf("vita: frame cap back to 30 (%s) after %.1f s at 20\n", why, playedS);
                            if (VitaBench_Active()) VitaBench_Log("bench   20 fps cap: off after %.1f s (%s)", playedS, why);
                        }
                        slowBits = 0;
                        fastFrames = 0;
                        uselessFrames = 0;
                    } else if (!vitaCap20 && slow >= 4) {
                        if (emaWork20 < 48.0f) {
                            vitaCap20 = true;
                            fastFrames = 0;
                            uselessFrames = 0;
                            capSinceUs = now;
                            Printf("vita: frame cap 30 -> 20 (%d of the last 10 frames over 43.4 ms; work at 30 fps %.1f ms, at 20 fps %.1f ms smoothed)\n", slow, emaWork30, emaWork20);
                            if (VitaBench_Active()) VitaBench_Log("bench   20 fps cap: on (work at 30 fps %.1f ms, at 20 fps %.1f ms)", emaWork30, emaWork20);
                        } else if (now - refusedLogUs > 5000000) {
                            refusedLogUs = now;
                            Printf("vita: frames slow, but a 20 fps frame would take %.1f ms (game %.1f ms a tic): staying at 30\n", emaWork20, gameTicMs);
                        }
                    } else if (vitaCap20) {
                        fastFrames = (emaWork30 < 33.4f) ? fastFrames + 1 : 0;
                        uselessFrames = (emaWork20 > 52.0f) ? uselessFrames + 1 : 0;
                        const char* why = NULL;
                        if (fastFrames >= 30) {
                            why = "light enough for 30 again";
                        } else if (uselessFrames >= 15) {
                            why = "under 20 fps even at the cap - not helping";
                        } else if (now - capSinceUs > 10000000) {
                            why = "10 s at 20 - trying 30 again";
                        }
                        if (why != NULL) {
                            vitaCap20 = false;
                            slowBits = 0;
                            Printf("vita: frame cap 20 -> 30 after %.1f s (%s; work at 30 fps %.1f ms, at 20 fps %.1f ms smoothed)\n", (now - capSinceUs) / 1000000.0, why,
                                emaWork30, emaWork20);
                            if (VitaBench_Active()) VitaBench_Log("bench   20 fps cap: off after %.1f s (%s)", (now - capSinceUs) / 1000000.0, why);
                        }
                    }
                }
                // at the 20 fps fallback every frame lasts 50 ms by design:
                // the controllers below (load step, light cap, dynamic
                // resolution) judge the work instead, or they would keep
                // cutting for frames that are slow on purpose
                emaFrame += a * ((vitaCap20 ? workMs : frameUs / 1000.0f) - emaFrame);
                emaSync += a * ((frameTiming.finishSyncTime - frameTiming.startSyncTime) / 1000.0f - emaSync);
                emaCap += a * (vitaPerfCapThisFrame / 1000.0f - emaCap);
                emaGameWait += a * (vitaPerfGameWaitThisFrame / 1000.0f - emaGameWait);
                vitaPerfGameWaitUs += vitaPerfGameWaitThisFrame;
                framesSinceChange++;

                // The light cap (vita_lightCapAuto): the back end's time is
                // its draw calls. While its smoothed time runs over 26 ms -
                // with the GPU's share, a frame then misses 33.3 ms - the
                // view keeps at most vita_viewLightCap lights with something
                // to draw (the smallest on screen go first,
                // tr_frontend_addlights.cpp), 4 fewer every 8 frames down to
                // 16; under 20 ms it gets 4 back every 30 frames, and the cap
                // goes when it is above the view's lights again.
                {
                    extern int vita_viewLightCap, vita_viewLitLightsLast; // tr_frontend_addlights.cpp
                    extern idCVar vita_lightCapAuto;
                    float& emaBackEnd = vitaEmaBackEnd;
                    vitaBackEndOverRun = (backendUs > 36000) ? vitaBackEndOverRun + 1 : 0;
                    static int framesSinceCapChange = 0;
                    emaBackEnd += 0.15f * (backendUs / 1000.0f - emaBackEnd);
                    framesSinceCapChange++;
                    const int oldCap = vita_viewLightCap;
                    if (!vita_lightCapAuto.GetBool() || !mapSpawned) {
                        vita_viewLightCap = 0;
                    } else if (pauseGame) {
                        // menus and pauses say nothing
                    } else if (emaBackEnd > 26.0f && framesSinceCapChange > 4) {
                        // (6 every 4 frames since run 32: 4 every 8 took 2.5 s
                        // to act in cpu fight 1's 40 ms back end burst)
                        const int from = (vita_viewLightCap > 0) ? vita_viewLightCap : vita_viewLitLightsLast;
                        vita_viewLightCap = Max(16, from - 6);
                    } else if (vita_viewLightCap > 0 && emaBackEnd < 20.0f && framesSinceCapChange > 30) {
                        vita_viewLightCap += 4;
                        if (vita_viewLightCap > vita_viewLitLightsLast + 4) {
                            vita_viewLightCap = 0;
                        }
                    }
                    if (vita_viewLightCap != oldCap) {
                        framesSinceCapChange = 0;
                        static int capLogged = 0;
                        if (capLogged++ < 120) {
                            Printf("vita: light cap %d -> %d (0 = none; back end %.1f ms smoothed, %d lights with something to draw last view)\n",
                                oldCap, vita_viewLightCap, emaBackEnd, vita_viewLitLightsLast);
                        }
                    }
                }

                // The load step (0-6): how far over budget the frames have
                // been lately, whatever held them back - the game/draw thread,
                // the back end or the GPU. Up a step every second the frames
                // run over 34.5 ms, down one every 3 s they hold 30 fps with
                // time to spare. Small lights drop with it
                // (vita_lightMinScreenAreaPerLoadStep, tr_frontend_addlights.cpp).
                {
                    extern int vita_loadStep; // tr_frontend_addlights.cpp
                    static int framesSinceLoadChange = 0;
                    framesSinceLoadChange++;
                    const int oldLoad = vita_loadStep;
                    if (!mapSpawned || pauseGame) {
                        // menus and pauses say nothing about the game's frame rate
                    } else if (emaFrame > 34.5f && vita_loadStep < 6 && framesSinceLoadChange > 30) {
                        vita_loadStep++;
                    } else if (vita_loadStep > 0 && emaFrame < 33.9f && framesSinceLoadChange > 90
                               && (emaCap > 3.0f || emaSync > 3.0f)) {
                        vita_loadStep--;
                    }
                    if (vita_loadStep != oldLoad) {
                        framesSinceLoadChange = 0;
                        static int loadLogged = 0;
                        if (loadLogged++ < 60) {
                            Printf("vita: load step %d -> %d (frame %.1f ms, GPU wait %.1f ms, %.1f ms to spare, main thread waiting for game/draw %.1f ms)\n",
                                oldLoad, vita_loadStep, emaFrame, emaSync, emaCap, emaGameWait);
                        }
                    }
                }

                // Memory: checked every 1.5 s. "Tight" below 24 MB free (see
                // the thresholds below): world textures lose a mip level - two
                // below 14 MB - shrunk a few at a time between frames.
                static bool memTight = false;
                static int framesSinceMemCheck = 0;
                static int framesSinceDrop = 1000;
                framesSinceDrop++;
                if (++framesSinceMemCheck >= 45 && mapSpawned) {
                    framesSinceMemCheck = 0;
                    unsigned int memFreeKB = 0;
                    const int64 checkStart = Sys_Microseconds();
                    Vita_FreeMemoryKB(&memFreeKB, NULL);
                    vitaPerfMemCheckUs = Max(vitaPerfMemCheckUs, (uint64)(Sys_Microseconds() - checkStart));
                    // (run 31: one reading of 34 MB, a dip that was gone a
                    // second later, held the 3D view at its lowest size for
                    // the rest of the run - "recovered" needed 64 MB, more
                    // than enpro and cpu ever have free (~60) - and started
                    // texture shrinking: 20-90 ms of reloads a frame for
                    // seconds, for 1.9 MB. Now: tight on two low readings in
                    // a row (or one under 28 MB), recovered above 48 MB, and
                    // textures shrink only while still tight.)
                    static unsigned int lastFreeKB = 0;
                    static int lowReadings = 0;
                    // (2026-10-17: 40 / 28 / 48 MB, and a tight memory also set
                    // the 3D view to its lowest size - which frees nothing:
                    // the levels played at 40-45 MB free with quarter-size
                    // textures and a 544x312 view. Now tight below 24 MB on two
                    // readings or 18 MB on one, recovered above 36 MB; a
                    // level smaller below 24 MB, two below 14 MB.)
                    lowReadings = (memFreeKB < 24 * 1024) ? lowReadings + 1 : 0;
                    if (lastFreeKB != 0 && lastFreeKB > memFreeKB + 8 * 1024) {
                        static int dropLogged = 0;
                        if (dropLogged++ < 20) {
                            Printf("vita: free memory fell %u MB since the last check (%u -> %u MB) - where it went:\n", (lastFreeKB - memFreeKB) / 1024, lastFreeKB / 1024, memFreeKB / 1024);
                            extern void Vita_LogMemoryFast(const char* tag); // vita_main.cpp (no heap walk)
                            Vita_LogMemoryFast("memory drop");
                        }
                    }
                    lastFreeKB = memFreeKB;
                    if (!memTight && (lowReadings >= 2 || memFreeKB < 18 * 1024)) {
                        memTight = true;
                        Printf("vita: memory tight (%u MB free, %d low readings in a row) - world textures may shrink\n", memFreeKB / 1024, lowReadings);
                    } else if (memTight && memFreeKB > 36 * 1024) {
                        memTight = false;
                        if (vita_textureQualityDrop > 0) {
                            Printf("vita: memory recovered (%u MB free) - texture shrinking stops (textures already shrunk stay)\n", memFreeKB / 1024);
                        } else {
                            Printf("vita: memory recovered (%u MB free)\n", memFreeKB / 1024);
                        }
                        vita_textureQualityDrop = 0;
                    } else if (lowReadings == 1 && !memTight) {
                        Printf("vita: %u MB free - one low reading, not tight yet (two in a row are needed)\n", memFreeKB / 1024);
                    }
                    const int wantDrop = !memTight ? 0 : ((memFreeKB < 14 * 1024) ? 2 : ((memFreeKB < 24 * 1024) ? 1 : 0));
                    if (wantDrop > vita_textureQualityDrop && framesSinceDrop > 300) {
                        vita_textureQualityDrop++;
                        framesSinceDrop = 0;
                        Printf("vita: %u MB free - world textures shrinking by %d mip level(s)\n", memFreeKB / 1024, vita_textureQualityDrop);
                    }
                }

                const int maxStep = Vita_DynamicResolutionSteps() - 1;
                const int oldStep = vita_dynamicResolutionStep;
                // The 3D view's size changes the GPU's work and nothing else.
                // When the main thread spends its frame waiting for the
                // game/draw thread, that thread sets the frame rate: fewer
                // pixels would only blur the picture (the benchmark's fights
                // ran at the lowest size at 13 fps, for nothing).
                const bool gameDrawBound = emaGameWait > 4.0f;
                if (!vita_dynamicResolution.GetBool()) {
                    vita_dynamicResolutionStep = 0;
                } else if (!mapSpawned || pauseGame) {
                    // menus and pauses say nothing about the game's frame rate
                } else if (vitaBackEndOverRun >= 2 && vita_dynamicResolutionStep < maxStep && framesSinceChange > 2) {
                    // two back ends over 36 ms in a row (GPU time inside GL
                    // calls - the bursts at the start of the fights, 0.2-0.5 s,
                    // were over before the smoothed figures acted): two steps
                    // at once
                    vita_dynamicResolutionStep = Min(maxStep, vita_dynamicResolutionStep + 2);
                    vitaBackEndOverRun = 0;
                    VITA_ZONE_COUNT("3D view: emergency step down by 2 (two back ends over 36 ms in a row)");
                } else if (emaFrame > 34.5f && !gameDrawBound && vita_dynamicResolutionStep < maxStep
                           && framesSinceChange > ((emaSync > 4.0f || vitaEmaBackEnd > 28.0f) ? ((vitaEmaBackEnd > 28.0f) ? 8 : 20) : 45)) {
                    // not holding 30 fps, and not for the game/draw thread
                    // (sooner when the time goes to the GPU - in the swap, or
                    // inside the back end's GL calls: a back end over 28 ms
                    // with the GPU wait near 0 was cpu fight 1's 1.5 s burst
                    // of 40-49 ms back ends, its main thread mostly waiting
                    // inside vitaGL for the GPU; the step came every 45 frames)
                    vita_dynamicResolutionStep++;
                    if (vitaEmaBackEnd > 28.0f && emaSync < 4.0f) {
                        VITA_ZONE_COUNT("3D view stepped down for a back end over 28 ms (GPU time inside GL calls)");
                    }
                } else if (vita_dynamicResolutionStep > 0 && framesSinceChange > 120
                           && ((emaFrame < 33.9f && (emaCap > 3.0f || (vitaDisplayPacedNow && emaSync > 3.0f)))
                               || (gameDrawBound && emaSync < 2.0f))) {
                    // (paced by the display, the spare time is in the swap -
                    // waiting for the vblank - not in the cap; without this the
                    // view never got its pixels back once it had dropped)
                    // holding 30 with time to spare - or held back by the
                    // game/draw thread with the GPU idle: give the pixels back
                    vita_dynamicResolutionStep--;
                }
                if (vita_dynamicResolutionStep != oldStep) {
                    framesSinceChange = 0;
                    static int logged = 0;
                    if (logged++ < 40) {
                        int w, h;
                        Vita_RenderSizeForStep(vita_dynamicResolutionStep, &w, &h);
                        Printf("vita: 3D view %dx%d - frame %.1f ms, GPU wait %.1f ms, %.1f ms to spare, main thread waiting for game/draw %.1f ms%s\n",
                            w, h, emaFrame, emaSync, emaCap, emaGameWait, memTight ? ", memory tight" : "");
                    }
                }
                if (vitaPerfFrames >= vita_perfLog.GetInteger() || vitaPerfFlushRequested) {
                    const double n = vitaPerfFrames;
                    if (vitaPerfFlushRequested) {
                        vitaPerfFlushRequested = false;
                        Printf("vita perf window: %s - the %d frames of it (cumulative counters below cover exactly these frames)\n", vitaPerfFlushLabel.c_str(), vitaPerfFrames);
                    }
                    unsigned int freeKB = 0, cachedFreeKB = 0;
                    Vita_FreeMemoryKB(&freeKB, &cachedFreeKB);
                    Printf("vita perf [%s, cap %d]: %.1f fps (worst frame %.0f ms) | game %.1f ms/frame (%.1f tics, worst %.0f; %d tics not played: one a frame at most) | draw %.1f | back end %.1f (worst %.0f) | GPU/swap wait %.1f | cap sleep %.1f | waiting on game %.1f | 3D res step %d | load step %d | light cap %d | log lines written directly (ring full) %d | free %u MB (%u MB cached) | mem check %.1f ms | texture drop %d\n",
                        (currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC) ? "classic" : (mapSpawned ? currentMapName.c_str() : "menu"), vitaPerfCapHz,
                        vitaPerfFrameUs > 0 ? 1000000.0 * n / vitaPerfFrameUs : 0.0, vitaPerfWorstUs / 1000.0,
                        vitaPerfGameUs / 1000.0 / n, vitaPerfTics / n, vitaPerfWorstGameUs / 1000.0, vita_ticsDropped,
                        vitaPerfDrawUs / 1000.0 / n, vitaPerfBackendUs / 1000.0 / n, vitaPerfWorstBackendUs / 1000.0,
                        vitaPerfSyncUs / 1000.0 / n, vitaPerfCapUs / 1000.0 / n, vitaPerfGameWaitUs / 1000.0 / n, vita_dynamicResolutionStep, Vita_LoadStepNow(), Vita_LightCapNow(), Vita_LogRingFullNow(),
                        freeKB / 1024, cachedFreeKB / 1024, vitaPerfMemCheckUs / 1000.0, vita_textureQualityDrop);
                    vitaPerfMemCheckUs = 0;
                    vita_ticsDropped = 0;
                    Printf("vita front end [ms/frame]: find lights+entities %.1f | wait shadow jobs %.1f | lights %.1f | models %.1f | guis+sort %.1f | subviews %.1f\n",
                        vita_frontEndUs[0] / 1000.0 / n, vita_frontEndUs[1] / 1000.0 / n, vita_frontEndUs[2] / 1000.0 / n,
                        vita_frontEndUs[3] / 1000.0 / n, vita_frontEndUs[4] / 1000.0 / n, vita_frontEndUs[5] / 1000.0 / n);
                    memset(vita_frontEndUs, 0, sizeof(vita_frontEndUs));
                    extern uint64 vita_pcSums[8]; // RenderSystem.cpp
                    extern int vita_frameMemoryPeak;      // tr_frontend_main.cpp
                    extern const int vita_frameMemoryMax; // tr_frontend_main.cpp
                    Printf("vita scene [per frame]: %.0f skinned verts | %.0f shadow volumes | %.0f entities | %.0f lights | %.0f draws | %.0f tris (%.0f shadow) | %.0f new interactions | frame memory peak %d KB of %d\n",
                        vita_pcSums[0] / n, vita_pcSums[1] / n, vita_pcSums[2] / n, vita_pcSums[3] / n,
                        vita_pcSums[4] / n, vita_pcSums[5] / n, vita_pcSums[6] / n, vita_pcSums[7] / n,
                        vita_frameMemoryPeak / 1024, vita_frameMemoryMax / 1024);
                    vita_frameMemoryPeak = 0;
                    memset(vita_pcSums, 0, sizeof(vita_pcSums));
                    extern int vita_longJobs;
                    extern uint64 vita_longJobWorstUs;
                    extern const char* vita_longJobWorstName;
                    if (vita_longJobs > 0) {
                        Printf("vita jobs: %d over 10 ms in %d frames, worst %d us ('%s')\n", vita_longJobs, vitaPerfFrames, (int)vita_longJobWorstUs, vita_longJobWorstName);
                        vita_longJobs = 0;
                        vita_longJobWorstUs = 0;
                    }
                    extern volatile int vita_vcCopyUs, vita_vcCopyKB, vita_prtUs, vita_prtModels;
                    extern int64 vita_vcFrameBytes;
                    Printf("vita GPU buffers [per frame]: %.0f KB written (%.0f KB copied in %.1f ms) | particles: %.1f systems built in %.1f ms" "\n",
                        vita_vcFrameBytes / 1024.0 / n, vita_vcCopyKB / n, vita_vcCopyUs / 1000.0 / n, vita_prtModels / n, vita_prtUs / 1000.0 / n);
                    vita_vcCopyUs = vita_vcCopyKB = vita_prtUs = vita_prtModels = 0;
                    vita_vcFrameBytes = 0;
                    {
                        extern volatile int vita_afUs, vita_afCalls, vita_animUs, vita_animCalls, vita_decalUs, vita_decalCalls;
                        Printf("vita game [per frame]: ragdoll physics %.1f ms (%.1f evaluations) | animation %.1f ms (%.0f frames) | decals %.1f ms (%.1f) | sound update on the game/draw thread %.1f ms" "\n",
                            vita_afUs / 1000.0 / n, vita_afCalls / n, vita_animUs / 1000.0 / n, vita_animCalls / n, vita_decalUs / 1000.0 / n, vita_decalCalls / n, (vita_soundUs - vitaPerfSoundUsAtStart) / 1000.0 / n);
                        vitaPerfSoundUsAtStart = vita_soundUs; // a running total (the benchmark takes deltas of it): this window's share
                        vita_afUs = vita_afCalls = vita_animUs = vita_animCalls = vita_decalUs = vita_decalCalls = 0;
                    }
                    extern void Vita_GameThinkReport(double frames); // Game_local.cpp
                    Vita_GameThinkReport(n);
                    if (mapSpawned) {
                        extern void Vita_PhysCheck_Report(); // Game_local.cpp
                        Vita_PhysCheck_Report();
                    }
                    extern void Vita_ZoneReport(double frames); // vita_main.cpp
                    Vita_ZoneReport(n);
                    Vita_LogThreadCpu(n);
                    vitaPerfGameWaitUs = 0;
                    vitaPerfFrameUs = vitaPerfWorstUs = vitaPerfGameUs = vitaPerfWorstGameUs = 0;
                    vitaPerfDrawUs = vitaPerfBackendUs = vitaPerfWorstBackendUs = vitaPerfSyncUs = vitaPerfCapUs = 0;
                    vitaPerfFrames = vitaPerfTics = 0;
                }
            }
            vitaPerfLastFrameStart = now;
        }
#endif

#if defined(__vita__)
        // textures to shrink after vita_textureQualityDrop went up: a few
        // milliseconds' worth per frame, here where the game/draw thread and
        // the render back end are both idle
        // (once a whole round of the image list found nothing to shrink, it
        // rests until the drop changes - images loaded later already load at
        // the current drop)
        static int shrinkIdleAtDrop = -1;
        if (vita_textureQualityDrop == 0) {
            shrinkIdleAtDrop = -1; // a new level starts at drop 0
        }
        if (vita_textureQualityDrop > 0 && currentGame == DOOM3_BFG && vita_textureQualityDrop != shrinkIdleAtDrop) {
            FRAME_PHASE("frame: shrink textures");
            static int shrunkTotal = 0;
            static int64 savedTotal = 0;
            static bool wasWaiting = false;
            int shrunk = 0;
            int64 saved = 0;
            // (a texture is a reload from the memory card - 20-90 ms in run
            // 31: one every third frame at most, the budget only bounds the
            // check of the rest)
            static int shrinkFrame = 0;
            const int waiting = ((++shrinkFrame % 3) == 0) ? globalImages->VitaShrinkLoadedTextures(1, &shrunk, &saved) : 1;
            shrunkTotal += shrunk;
            savedTotal += saved;
            if (shrunk > 0) {
                wasWaiting = true;
            }
            if (wasWaiting && waiting == 0) {
                wasWaiting = false;
                Printf("vita: %d textures shrunk, %d KB freed\n", shrunkTotal, (int)(savedTotal / 1024));
                shrunkTotal = 0;
                savedTotal = 0;
            }
            if (waiting == 0) {
                shrinkIdleAtDrop = vita_textureQualityDrop;
                Printf("vita: texture shrink done for quality drop %d - idle until it changes\n", vita_textureQualityDrop);
            }
        }
#endif
        FRAME_PHASE("frame: savegame pump");
        session->GetSaveGameManager().Pump();
        FRAME_PHASE("frame: done");
    } catch (idException&) {
        // an ERP_DROP was thrown
#if defined(USE_DOOMCLASSIC)
        if (currentGame == DOOM_CLASSIC || currentGame == DOOM2_CLASSIC) {
            return;
        }
#endif

        // kill loading gui
        delete loadGUI;
        loadGUI = NULL;

        // drop back to main menu
        LeaveGame();

        // force the console open to show error messages
        console->Open();
        return;
    }
}

// RB begin
#if defined(USE_DOOMCLASSIC)

/*
=================
idCommonLocal::RunDoomClassicFrame
=================
*/
#if defined(__vita__)
// where a classic Doom frame's time goes, logged every couple of seconds
static uint64 vitaClassicDoomUs = 0, vitaClassicPaletteUs = 0, vitaClassicUploadUs = 0;
static uint64 vitaClassicWindowStart = 0;
static int vitaClassicFrames = 0, vitaClassicRendered = 0;
// WAD lump reads (doomclassic/doom/w_wad.cpp W_ReadLump)
int vitaClassicLumpReads = 0;
int64 vitaClassicLumpBytes = 0;
uint64 vitaClassicLumpUs = 0;
// inside "doom tic+render" (doominterface.cpp, d_main.cpp, r_main.cpp)
uint64 vitaClassicTicUs = 0, vitaClassicWallsUs = 0, vitaClassicFlatsUs = 0, vitaClassicSpritesUs = 0;
uint64 vitaClassicNetUs = 0, vitaClassicDisplayUs = 0, vitaClassicSndPosUs = 0, vitaClassicSndSubmitUs = 0, vitaClassicSndEventsUs = 0;
#endif

#if defined(__vita__)
/*
=================
Vita_ClassicCopy

The classic frame's 8-bit indices into the texture's buffer in the uncached
RAM pool (the GPU does the palette lookup - idImage::VitaClassicBeginFrameP8),
in bands spread over the job threads and this one: 64-byte NEON bursts, the
store pattern uncached memory takes best.
=================
*/
struct vitaCopyJob_t {
    const byte* src;
    byte* dst;
    int bytes;
    int pad[13];
};

static void Vita_ClassicCopyJob(vitaCopyJob_t* j)
{
    const byte* s = j->src;
    byte* d = j->dst;
    int n = j->bytes;
    for (; n >= 64; n -= 64, s += 64, d += 64) {
        const uint8x16_t a = vld1q_u8(s), b = vld1q_u8(s + 16), c = vld1q_u8(s + 32), e = vld1q_u8(s + 48);
        vst1q_u8(d, a);
        vst1q_u8(d + 16, b);
        vst1q_u8(d + 32, c);
        vst1q_u8(d + 48, e);
    }
    memcpy(d, s, n);
}

static void Vita_ClassicCopy(const byte* src, byte* dst, int bytes)
{
    const int parts = idMath::ClampInt(1, 4, cvarSystem->GetCVarInteger("jobs_numThreads") + 1);
    static vitaCopyJob_t jobs[4];
    for (int t = 0; t < parts; t++) {
        // band edges on 64-byte multiples
        const int b0 = (int)(((int64)bytes * t / parts) & ~63);
        const int b1 = (t == parts - 1) ? bytes : (int)(((int64)bytes * (t + 1) / parts) & ~63);
        jobs[t].src = src + b0;
        jobs[t].dst = dst + b0;
        jobs[t].bytes = b1 - b0;
    }
    if (parts == 1) {
        Vita_ClassicCopyJob(&jobs[0]);
        return;
    }
    static idParallelJobList* copyJobs = NULL;
    if (copyJobs == NULL) {
        copyJobs = parallelJobManager->AllocJobList(JOBLIST_UTILITY, JOBLIST_PRIORITY_HIGH, 8, 0, NULL);
    }
    for (int t = 0; t < parts; t++) {
        copyJobs->AddJob((jobRun_t)Vita_ClassicCopyJob, &jobs[t]);
    }
    copyJobs->Submit();
    copyJobs->Wait();
}

void idCommonLocal::RunDoomClassicFrame(int framesDue)
#else
void idCommonLocal::RunDoomClassicFrame()
#endif
{
    static int doomTics = 0;
#if defined(__vita__)
    static_assert(DOOMCLASSIC_RENDERWIDTH == SCREENWIDTH && DOOMCLASSIC_RENDERHEIGHT == SCREENHEIGHT,
        "the classic Doom framebuffer and the texture it is copied into must be the same size");
    const uint64 t0 = Sys_Microseconds();
    if (vitaClassicWindowStart == 0) {
        vitaClassicWindowStart = t0;
    }
#endif

    if (DoomLib::expansionDirty) {

        // re-Initialize the Doom Engine.
        DoomLib::Interface.Shutdown();
        DoomLib::Interface.Startup(1, false);
        DoomLib::expansionDirty = false;
    }

#if defined(__vita__)
    const bool rendered = DoomLib::Interface.Frame(doomTics, &userCmdMgr);
    const uint64 t1 = Sys_Microseconds();
    vitaClassicDoomUs += t1 - t0;
    idImage* classicImage = globalImages->GetImage("_doomClassic");
    if (rendered && classicImage != NULL) {
        vitaClassicRendered++;
        Globals* data = (Globals*)DoomLib::GetGlobalData(0);
        // the frame goes up as it is, 8-bit, with its palette: the GPU's
        // texture unit does the lookup (22 ms a frame on the CPU before)
        uint32* palDst = NULL;
        byte* dst = classicImage->VitaClassicBeginFrameP8(DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT, &palDst);
        if (dst != NULL) {
            // XColorMap entries are 0xRRGGBBAA; the P8_ABGR palette wants the
            // bytes R,G,B,A, i.e. 0xAABBGGRR as a little-endian word
            for (int i = 0; i < 256; i++) {
                const unsigned int c = data->XColorMap[i];
                palDst[i] = ((c >> 24) & 0xFF) | (((c >> 16) & 0xFF) << 8) | (((c >> 8) & 0xFF) << 16) | 0xFF000000u;
            }
            static_assert((DOOMCLASSIC_RENDERWIDTH & 7) == 0, "the texture's rows are padded to 8 texels; the frame's must match to copy it in one piece");
            Vita_ClassicCopy(data->screens[0], dst, DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_RENDERHEIGHT);
            classicImage->VitaClassicEndFrameP8();
        }
    }
    const uint64 t2 = Sys_Microseconds();
    vitaClassicPaletteUs += t2 - t1;
#else
    if (DoomLib::Interface.Frame(doomTics, &userCmdMgr)) {
        Globals* data = (Globals*)DoomLib::GetGlobalData(0);

        idArray<unsigned int, 256> palette;
        std::copy(data->XColorMap, data->XColorMap + palette.Num(), palette.Ptr());

        // Do the palette lookup.
        for (int row = 0; row < DOOMCLASSIC_RENDERHEIGHT; ++row) {
            for (int column = 0; column < DOOMCLASSIC_RENDERWIDTH; ++column) {
                const int doomScreenPixelIndex = row * DOOMCLASSIC_RENDERWIDTH + column;
                const byte paletteIndex = data->screens[0][doomScreenPixelIndex];
                const unsigned int paletteColor = palette[paletteIndex];
                const byte red = (paletteColor & 0xFF000000) >> 24;
                const byte green = (paletteColor & 0x00FF0000) >> 16;
                const byte blue = (paletteColor & 0x0000FF00) >> 8;

                const int imageDataPixelIndex = row * DOOMCLASSIC_RENDERWIDTH * DOOMCLASSIC_BYTES_PER_PIXEL + column * DOOMCLASSIC_BYTES_PER_PIXEL;
                doomClassicImageData[imageDataPixelIndex] = red;
                doomClassicImageData[imageDataPixelIndex + 1] = green;
                doomClassicImageData[imageDataPixelIndex + 2] = blue;
                doomClassicImageData[imageDataPixelIndex + 3] = 255;
            }
        }
    }
#endif

#if !defined(__vita__)
    renderSystem->UploadImage("_doomClassic", doomClassicImageData.Ptr(), DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT);
#endif
#if defined(__vita__)
    doomTics += idMath::ClampInt(1, 4, framesDue);
#else
    doomTics++;
#endif
#if defined(__vita__)
    const uint64 t3 = Sys_Microseconds();
    vitaClassicUploadUs += t3 - t2;
    vitaClassicFrames++;
    if (t3 - vitaClassicWindowStart >= 2000000) {
        const double frames = vitaClassicFrames;
        const double wallMs = (t3 - vitaClassicWindowStart) / 1000.0;
        Printf("vita: classic doom %dx%d: %.1f fps over %.1f s (%d frames, %d rendered) - per frame: doom tic+render %.1f ms, 8-bit frame copy %.1f ms (palette lookup on the GPU), after it %.1f ms, everything else %.1f ms\n",
            DOOMCLASSIC_RENDERWIDTH, DOOMCLASSIC_RENDERHEIGHT, frames * 1000.0 / wallMs, wallMs / 1000.0, vitaClassicFrames, vitaClassicRendered,
            vitaClassicDoomUs / 1000.0 / frames, vitaClassicPaletteUs / 1000.0 / frames, vitaClassicUploadUs / 1000.0 / frames,
            (wallMs - (vitaClassicDoomUs + vitaClassicPaletteUs + vitaClassicUploadUs) / 1000.0) / frames);
        Printf("vita: classic doom WAD reads in that window: %d lumps, %lld KB, %.1f ms (included in the tic+render time)\n",
            vitaClassicLumpReads, (long long)(vitaClassicLumpBytes / 1024), vitaClassicLumpUs / 1000.0);
        extern uint64 vitaClassicMtDrawUs; // r_draw.cpp
        extern idCVar vita_classicThreads;
        Printf("vita: classic doom tic+render per frame: game tics %.1f, display %.1f (BSP+walls %.1f, floors %.1f, sprites %.1f, net %.1f; pixels drawn on %d threads %.1f), sound positions %.1f, sound submit %.1f, sound events %.1f ms\n",
            vitaClassicTicUs / 1000.0 / frames, vitaClassicDisplayUs / 1000.0 / frames, vitaClassicWallsUs / 1000.0 / frames,
            vitaClassicFlatsUs / 1000.0 / frames, vitaClassicSpritesUs / 1000.0 / frames, vitaClassicNetUs / 1000.0 / frames,
            vita_classicThreads.GetBool() ? idMath::ClampInt(1, 4, cvarSystem->GetCVarInteger("jobs_numThreads") + 1) : 1, vitaClassicMtDrawUs / 1000.0 / frames,
            vitaClassicSndPosUs / 1000.0 / frames, vitaClassicSndSubmitUs / 1000.0 / frames, vitaClassicSndEventsUs / 1000.0 / frames);
        vitaClassicMtDrawUs = 0;
        vitaClassicTicUs = vitaClassicWallsUs = vitaClassicFlatsUs = vitaClassicSpritesUs = 0;
        vitaClassicNetUs = vitaClassicDisplayUs = vitaClassicSndPosUs = vitaClassicSndSubmitUs = vitaClassicSndEventsUs = 0;
        vitaClassicLumpReads = 0;
        vitaClassicLumpBytes = 0;
        vitaClassicLumpUs = 0;
        vitaClassicDoomUs = vitaClassicPaletteUs = vitaClassicUploadUs = 0;
        vitaClassicFrames = vitaClassicRendered = 0;
        vitaClassicWindowStart = t3;
    }
#endif
}

#endif
// RB end
