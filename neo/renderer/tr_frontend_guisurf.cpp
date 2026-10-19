/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014 Robert Beckebans

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

#pragma hdrstop
#include "precompiled.h"

#include "tr_local.h"
#include "Model_local.h"
#include "../sys/vita/vita_zones.h" // VITA_ZONE_COUNT: the world screen refresh counts in the perf log

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
==========================================================================================

Vita: GUI drawing recorded and played back (a fidelity cut, approved for the
benchmark's 33 ms goal)

World screens - computer panels, monitors, the weapon's ammo counter - are
redrawn every frame by stock BFG: their GUI runs and emits fresh quads (2.4 ms
a frame in CPU Complex). A screen that is far from the view
(vita_guiNearDistance) and not being looked at (its center more than
vita_guiFocusAngle off the view axis, widened by its size) now redraws every
other frame, at 15 Hz, and the frame in between plays back the quads its
last redraw made (idGuiModel::VitaReplay - the same surfaces and vertices).
Screens close by or looked at redraw every frame, as stock. Screens take
turns (half on even frames, half on odd). The HUD uses the same recording
at 15 Hz (PlayerView.cpp, vita_hudHalfRate). The PDA is a full screen menu
of its own and is not touched.

==========================================================================================
*/
idCVar vita_guiLOD("vita_guiLOD", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "Vita: world screens far away and not looked at redraw at 15 Hz (the frame between plays back their last drawing)");
idCVar vita_guiNearDistance("vita_guiNearDistance", "96", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "Vita: world screens closer than this redraw every frame");
idCVar vita_guiFarDistance("vita_guiFarDistance", "320", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "Vita: world screens farther than this (and not looked at) redraw at 7.5 Hz instead of 15; 0 = never");
idCVar vita_guiFocusAngle("vita_guiFocusAngle", "8", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "Vita: world screens within this many degrees of the view's center (plus their own size) redraw every frame");

static const int VITA_GUI_SLOTS = 32;
static vitaGuiCapture_t vitaGuiSlots[VITA_GUI_SLOTS];

static vitaGuiCapture_t* Vita_GuiSlot(const void* key, bool create)
{
    for (int i = 0; i < VITA_GUI_SLOTS; i++) {
        if (vitaGuiSlots[i].key == key) {
            return &vitaGuiSlots[i];
        }
    }
    if (!create) {
        return NULL;
    }
    // the one drawn longest ago
    int oldest = 0;
    for (int i = 1; i < VITA_GUI_SLOTS; i++) {
        if (vitaGuiSlots[i].frame < vitaGuiSlots[oldest].frame) {
            oldest = i;
        }
    }
    vitaGuiCapture_t* slot = &vitaGuiSlots[oldest];
    slot->key = key;
    slot->valid = false;
    slot->frame = -1;
    return slot;
}

void idRenderSystemLocal::VitaGuiCaptureBegin(const void* key)
{
#if !defined(__vita__)
    Vita_GuiSlot(key, true)->kind = 1; // only the HUD records through here
#endif
    guiModel->VitaBeginCapture(Vita_GuiSlot(key, true));
}

void idRenderSystemLocal::VitaGuiCaptureEnd()
{
    guiModel->VitaEndCapture();
}

bool idRenderSystemLocal::VitaGuiReplay(const void* key)
{
    const vitaGuiCapture_t* slot = Vita_GuiSlot(key, false);
    if (slot == NULL || !slot->valid || slot->frame != frameCount - 1) {
        return false;
    }
    guiModel->VitaReplay(slot);
    return true;
}

// a world screen far from the view and not looked at: redrawn 1 frame in 2
// (15 Hz), or 1 in 4 (7.5 Hz) beyond vita_guiFarDistance; 1 = every frame
static int Vita_GuiRateDivisor(const drawSurf_t* drawSurf)
{
    extern bool vita_fullQuality;
    if (!vita_guiLOD.GetBool() || vita_fullQuality || tr.viewDef == NULL || tr.viewDef->viewEntitys == NULL) {
        return 1;
    }
    const idBounds& b = drawSurf->frontEndGeo->bounds;
    const idVec3 localCenter = b.GetCenter();
    idVec3 center;
    R_LocalPointToGlobal(drawSurf->space->modelMatrix, localCenter, center);
    idVec3 dir = center - tr.viewDef->renderView.vieworg;
    const float dist = dir.Normalize();
    if (dist < vita_guiNearDistance.GetFloat()) {
        return 1;
    }
    const float radius = b.GetRadius(localCenter);
    const float cosAngle = idMath::ClampFloat(-1.0f, 1.0f, dir * tr.viewDef->renderView.viewaxis[0]);
    const float allowed = DEG2RAD(vita_guiFocusAngle.GetFloat()) + idMath::ATan(radius, dist);
    if (idMath::ACos(cosAngle) < allowed) {
        return 1; // looked at
    }
    if (vita_guiFarDistance.GetFloat() > 0.0f && dist > vita_guiFarDistance.GetFloat()) {
        return 4;
    }
    return 2;
}
#endif

/*
==========================================================================================

GUI SURFACES

==========================================================================================
*/

/*
================
R_SurfaceToTextureAxis

Calculates two axis for the surface such that a point dotted against
the axis will give a 0.0 to 1.0 range in S and T when inside the gui surface
================
*/
void R_SurfaceToTextureAxis(const srfTriangles_t* tri, idVec3& origin, idVec3 axis[3])
{
    // find the bounds of the texture
    idVec2 boundsMin(999999.0f, 999999.0f);
    idVec2 boundsMax(-999999.0f, -999999.0f);
    for (int i = 0; i < tri->numVerts; i++) {
        const idVec2 uv = tri->verts[i].GetTexCoord();
        boundsMin.x = Min(uv.x, boundsMin.x);
        boundsMax.x = Max(uv.x, boundsMax.x);
        boundsMin.y = Min(uv.y, boundsMin.y);
        boundsMax.y = Max(uv.y, boundsMax.y);
    }

    // use the floor of the midpoint as the origin of the
    // surface, which will prevent a slight misalignment
    // from throwing it an entire cycle off
    const idVec2 boundsOrg(floor((boundsMin.x + boundsMax.x) * 0.5f), floor((boundsMin.y + boundsMax.y) * 0.5f));

    // determine the world S and T vectors from the first drawSurf triangle

    // RB: added check wether GPU skinning is available at all
    const idJointMat* joints = (tri->staticModelWithJoints != NULL && r_useGPUSkinning.GetBool() && glConfig.gpuSkinningAvailable) ? tri->staticModelWithJoints->jointsInverted : NULL;
    // RB end

    const idVec3 aXYZ = idDrawVert::GetSkinnedDrawVertPosition(tri->verts[tri->indexes[0]], joints);
    const idVec3 bXYZ = idDrawVert::GetSkinnedDrawVertPosition(tri->verts[tri->indexes[1]], joints);
    const idVec3 cXYZ = idDrawVert::GetSkinnedDrawVertPosition(tri->verts[tri->indexes[2]], joints);

    const idVec2 aST = tri->verts[tri->indexes[0]].GetTexCoord();
    const idVec2 bST = tri->verts[tri->indexes[1]].GetTexCoord();
    const idVec2 cST = tri->verts[tri->indexes[2]].GetTexCoord();

    float d0[5];
    d0[0] = bXYZ[0] - aXYZ[0];
    d0[1] = bXYZ[1] - aXYZ[1];
    d0[2] = bXYZ[2] - aXYZ[2];
    d0[3] = bST.x - aST.x;
    d0[4] = bST.y - aST.y;

    float d1[5];
    d1[0] = cXYZ[0] - aXYZ[0];
    d1[1] = cXYZ[1] - aXYZ[1];
    d1[2] = cXYZ[2] - aXYZ[2];
    d1[3] = cST.x - aST.x;
    d1[4] = cST.y - aST.y;

    const float area = d0[3] * d1[4] - d0[4] * d1[3];
    if (area == 0.0f) {
        axis[0].Zero();
        axis[1].Zero();
        axis[2].Zero();
        return; // degenerate
    }
    const float inva = 1.0f / area;

    axis[0][0] = (d0[0] * d1[4] - d0[4] * d1[0]) * inva;
    axis[0][1] = (d0[1] * d1[4] - d0[4] * d1[1]) * inva;
    axis[0][2] = (d0[2] * d1[4] - d0[4] * d1[2]) * inva;

    axis[1][0] = (d0[3] * d1[0] - d0[0] * d1[3]) * inva;
    axis[1][1] = (d0[3] * d1[1] - d0[1] * d1[3]) * inva;
    axis[1][2] = (d0[3] * d1[2] - d0[2] * d1[3]) * inva;

    idPlane plane;
    plane.FromPoints(aXYZ, bXYZ, cXYZ);
    axis[2][0] = plane[0];
    axis[2][1] = plane[1];
    axis[2][2] = plane[2];

    // take point 0 and project the vectors to the texture origin
    VectorMA(aXYZ, boundsOrg.x - aST.x, axis[0], origin);
    VectorMA(origin, boundsOrg.y - aST.y, axis[1], origin);
}

/*
=================
R_RenderGuiSurf

Create a texture space on the given surface and
call the GUI generator to create quads for it.
=================
*/
static void R_RenderGuiSurf(idUserInterface* gui, const drawSurf_t* drawSurf)
{
    SCOPED_PROFILE_EVENT("R_RenderGuiSurf");

    // for testing the performance hit
    if (r_skipGuiShaders.GetInteger() == 1) {
        return;
    }

    // don't allow an infinite recursion loop
    if (tr.guiRecursionLevel == 4) {
        return;
    }

    tr.pc.c_guiSurfs++;

    // create the new matrix to draw on this surface
    idVec3 origin, axis[3];
    R_SurfaceToTextureAxis(drawSurf->frontEndGeo, origin, axis);

    float guiModelMatrix[16];
    float modelMatrix[16];

    guiModelMatrix[0 * 4 + 0] = axis[0][0] * (1.0f / 640.0f);
    guiModelMatrix[1 * 4 + 0] = axis[1][0] * (1.0f / 480.0f);
    guiModelMatrix[2 * 4 + 0] = axis[2][0];
    guiModelMatrix[3 * 4 + 0] = origin[0];

    guiModelMatrix[0 * 4 + 1] = axis[0][1] * (1.0f / 640.0f);
    guiModelMatrix[1 * 4 + 1] = axis[1][1] * (1.0f / 480.0f);
    guiModelMatrix[2 * 4 + 1] = axis[2][1];
    guiModelMatrix[3 * 4 + 1] = origin[1];

    guiModelMatrix[0 * 4 + 2] = axis[0][2] * (1.0f / 640.0f);
    guiModelMatrix[1 * 4 + 2] = axis[1][2] * (1.0f / 480.0f);
    guiModelMatrix[2 * 4 + 2] = axis[2][2];
    guiModelMatrix[3 * 4 + 2] = origin[2];

    guiModelMatrix[0 * 4 + 3] = 0.0f;
    guiModelMatrix[1 * 4 + 3] = 0.0f;
    guiModelMatrix[2 * 4 + 3] = 0.0f;
    guiModelMatrix[3 * 4 + 3] = 1.0f;

    R_MatrixMultiply(guiModelMatrix, drawSurf->space->modelMatrix, modelMatrix);

    tr.guiRecursionLevel++;

    // call the gui, which will call the 2D drawing functions
    tr.guiModel->Clear();
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    const int vitaDivisor = Vita_GuiRateDivisor(drawSurf);
    if (vitaDivisor > 1) {
        vitaGuiCapture_t* slot = Vita_GuiSlot(gui, true);
        // screens take turns (their redraw frame: frameCount + phase a
        // multiple of the divisor); one is played back until its next turn,
        // never longer than divisor - 1 frames
        const int mask = vitaDivisor - 1;
        const int phase = (int)(((uintptr_t)gui >> 4) & mask);
        const int age = tr.frameCount - slot->frame;
        const bool fresh = slot->valid && (age == 0 || (age > 0 && age < vitaDivisor && ((tr.frameCount + phase) & mask) != 0));
        if (fresh) {
            if (vitaDivisor == 4) {
                VITA_ZONE_COUNT("world screens played back (7.5 Hz: beyond vita_guiFarDistance, not looked at)");
            } else {
                VITA_ZONE_COUNT("world screens played back (15 Hz: far, not looked at)");
            }
            tr.guiModel->VitaReplay(slot);
        } else {
            if (vitaDivisor == 4) {
                VITA_ZONE_COUNT("world screens redrawn at 7.5 Hz (beyond vita_guiFarDistance, not looked at)");
            } else {
                VITA_ZONE_COUNT("world screens redrawn at 15 Hz (far, not looked at)");
            }
#if !defined(__vita__)
            slot->kind = 0;
#endif
            tr.guiModel->VitaBeginCapture(slot);
            gui->Redraw(tr.viewDef->renderView.time[0]);
            tr.guiModel->VitaEndCapture();
        }
    } else {
        VITA_ZONE_COUNT("world screens redrawn every frame (near or looked at)");
        gui->Redraw(tr.viewDef->renderView.time[0]);
    }
#else
    gui->Redraw(tr.viewDef->renderView.time[0]);
#endif
    tr.guiModel->EmitToCurrentView(modelMatrix, drawSurf->space->weaponDepthHack);
    tr.guiModel->Clear();

    tr.guiRecursionLevel--;
}

/*
================
R_AddInGameGuis
================
*/
void R_AddInGameGuis(const drawSurf_t* const drawSurfs[], const int numDrawSurfs)
{
    SCOPED_PROFILE_EVENT("R_AddInGameGuis");

    // check for gui surfaces
    for (int i = 0; i < numDrawSurfs; i++) {
        const drawSurf_t* drawSurf = drawSurfs[i];

        idUserInterface* gui = drawSurf->material->GlobalGui();

        int guiNum = drawSurf->material->GetEntityGui() - 1;
        if (guiNum >= 0 && guiNum < MAX_RENDERENTITY_GUI) {
            if (drawSurf->space->entityDef != NULL) {
                gui = drawSurf->space->entityDef->parms.gui[guiNum];
            }
        }

        if (gui == NULL) {
            continue;
        }

        idBounds ndcBounds;
        if (!R_PreciseCullSurface(drawSurf, ndcBounds)) {
            // did we ever use this to forward an entity color to a gui that didn't set color?
            //	memcpy( tr.guiShaderParms, shaderParms, sizeof( tr.guiShaderParms ) );
            R_RenderGuiSurf(gui, drawSurf);
        }
    }
}

/*
================,
R_ReloadGuis_f

Reloads any guis that have had their file timestamps changed.
An optional "all" parameter will cause all models to reload, even
if they are not out of date.

Should we also reload the map models?
================
*/
void R_ReloadGuis_f(const idCmdArgs& args)
{
    bool all;

    if (!idStr::Icmp(args.Argv(1), "all")) {
        all = true;
        common->Printf("Reloading all gui files...\n");
    } else {
        all = false;
        common->Printf("Checking for changed gui files...\n");
    }

    uiManager->Reload(all);
}

/*
================,
R_ListGuis_f
================
*/
void R_ListGuis_f(const idCmdArgs& args)
{
    uiManager->ListGuis();
}
