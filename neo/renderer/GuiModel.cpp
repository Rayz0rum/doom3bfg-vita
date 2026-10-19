/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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
#include "../sys/vita/vita_zones.h" // VITA_ZONE_COUNT: the gui recording counts in the perf log

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
#endif

const float idGuiModel::STEREO_DEPTH_NEAR = 0.0f;
const float idGuiModel::STEREO_DEPTH_MID = 0.5f;
const float idGuiModel::STEREO_DEPTH_FAR = 1.0f;

/*
================
idGuiModel::idGuiModel
================
*/
idGuiModel::idGuiModel()
{
    // identity color for drawsurf register evaluation
    for (int i = 0; i < MAX_ENTITY_SHADER_PARMS; i++) {
        shaderParms[i] = 1.0f;
    }
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    vitaCapture = NULL;
#endif
}

/*
================
idGuiModel::Clear

Begins collecting draw commands into surfaces
================
*/
void idGuiModel::Clear()
{
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (vitaCapture != NULL) {
        VitaInterruptCapture();
    }
#endif
    surfaces.SetNum(0);
    AdvanceSurf();
}

/*
================
idGuiModel::WriteToDemo
================
*/
void idGuiModel::WriteToDemo(idDemoFile* demo)
{
}

/*
================
idGuiModel::ReadFromDemo
================
*/
void idGuiModel::ReadFromDemo(idDemoFile* demo)
{
}

/*
================
idGuiModel::BeginFrame
================
*/
void idGuiModel::BeginFrame()
{
    vertexBlock = vertexCache.AllocVertex(NULL, ALIGN(MAX_VERTS * sizeof(idDrawVert), VERTEX_CACHE_ALIGN));
    indexBlock = vertexCache.AllocIndex(NULL, ALIGN(MAX_INDEXES * sizeof(triIndex_t), INDEX_CACHE_ALIGN));
    vertexPointer = (idDrawVert*)vertexCache.MappedVertexBuffer(vertexBlock);
    indexPointer = (triIndex_t*)vertexCache.MappedIndexBuffer(indexBlock);
    numVerts = 0;
    numIndexes = 0;
    Clear();
}

idCVar stereoRender_defaultGuiDepth("stereoRender_defaultGuiDepth", "0", CVAR_RENDERER, "Fraction of separation when not specified");
/*
================
EmitSurfaces

For full screen GUIs, we can add in per-surface stereoscopic depth effects
================
*/
void idGuiModel::EmitSurfaces(float modelMatrix[16], float modelViewMatrix[16],
    bool depthHack, bool allowFullScreenStereoDepth, bool linkAsEntity)
{

    viewEntity_t* guiSpace = (viewEntity_t*)R_ClearedFrameAlloc(sizeof(*guiSpace), FRAME_ALLOC_VIEW_ENTITY);
    memcpy(guiSpace->modelMatrix, modelMatrix, sizeof(guiSpace->modelMatrix));
    memcpy(guiSpace->modelViewMatrix, modelViewMatrix, sizeof(guiSpace->modelViewMatrix));
    guiSpace->weaponDepthHack = depthHack;
    guiSpace->isGuiSurface = true;

    // If this is an in-game gui, we need to be able to find the matrix again for head mounted
    // display bypass matrix fixup.
    if (linkAsEntity) {
        guiSpace->next = tr.viewDef->viewEntitys;
        tr.viewDef->viewEntitys = guiSpace;
    }

    //---------------------------
    // make a tech5 renderMatrix
    //---------------------------
    idRenderMatrix viewMat;
    idRenderMatrix::Transpose(*(idRenderMatrix*)modelViewMatrix, viewMat);
    idRenderMatrix::Multiply(tr.viewDef->projectionRenderMatrix, viewMat, guiSpace->mvp);
    if (depthHack) {
        idRenderMatrix::ApplyDepthHack(guiSpace->mvp);
    }

    // to allow 3D-TV effects in the menu system, we define surface flags to set
    // depth fractions between 0=screen and 1=infinity, which directly modulate the
    // screenSeparation parameter for an X offset.
    // The value is stored in the drawSurf sort value, which adjusts the matrix in the
    // backend.
    float defaultStereoDepth = stereoRender_defaultGuiDepth.GetFloat(); // default to at-screen

    // add the surfaces to this view
    for (int i = 0; i < surfaces.Num(); i++) {
        const guiModelSurface_t& guiSurf = surfaces[i];
        if (guiSurf.numIndexes == 0) {
            continue;
        }

        const idMaterial* shader = guiSurf.material;
        drawSurf_t* drawSurf = (drawSurf_t*)R_FrameAlloc(sizeof(*drawSurf), FRAME_ALLOC_DRAW_SURFACE);

        drawSurf->numIndexes = guiSurf.numIndexes;
        drawSurf->ambientCache = vertexBlock;
        // build a vertCacheHandle_t that points inside the allocated block
        drawSurf->indexCache = indexBlock + ((int64)(guiSurf.firstIndex * sizeof(triIndex_t)) << VERTCACHE_OFFSET_SHIFT);
        drawSurf->shadowCache = 0;
        drawSurf->jointCache = 0;
        drawSurf->frontEndGeo = NULL;
        drawSurf->space = guiSpace;
        drawSurf->material = shader;
        drawSurf->extraGLState = guiSurf.glState;
        drawSurf->scissorRect = tr.viewDef->scissor;
        drawSurf->sort = shader->GetSort();
        drawSurf->renderZFail = 0;
        // process the shader expressions for conditionals / color / texcoords
        const float* constRegs = shader->ConstantRegisters();
        if (constRegs) {
            // shader only uses constant values
            drawSurf->shaderRegisters = constRegs;
        } else {
            float* regs = (float*)R_FrameAlloc(shader->GetNumRegisters() * sizeof(float), FRAME_ALLOC_SHADER_REGISTER);
            drawSurf->shaderRegisters = regs;
            shader->EvaluateRegisters(regs, shaderParms, tr.viewDef->renderView.shaderParms, tr.viewDef->renderView.time[1] * 0.001f, NULL);
        }
        R_LinkDrawSurfToView(drawSurf, tr.viewDef);
        if (allowFullScreenStereoDepth) {
            // override sort with the stereoDepth
            // drawSurf->sort = stereoDepth;

            switch (guiSurf.stereoType) {
            case STEREO_DEPTH_TYPE_NEAR:
                drawSurf->sort = STEREO_DEPTH_NEAR;
                break;
            case STEREO_DEPTH_TYPE_MID:
                drawSurf->sort = STEREO_DEPTH_MID;
                break;
            case STEREO_DEPTH_TYPE_FAR:
                drawSurf->sort = STEREO_DEPTH_FAR;
                break;
            case STEREO_DEPTH_TYPE_NONE:
            default:
                drawSurf->sort = defaultStereoDepth;
                break;
            }
        }
    }
}

/*
====================
EmitToCurrentView
====================
*/
void idGuiModel::EmitToCurrentView(float modelMatrix[16], bool depthHack)
{
    float modelViewMatrix[16];
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (vitaCapture != NULL) {
        VitaInterruptCapture();
    }
#endif

    R_MatrixMultiply(modelMatrix, tr.viewDef->worldSpace.modelViewMatrix, modelViewMatrix);

    EmitSurfaces(modelMatrix, modelViewMatrix, depthHack, false /* stereoDepthSort */, true /* link as entity */);
}

// DG: move function declaration here (=> out of EmitFullScreen() method) because it confused clang
// (and possibly other compilers that just didn't complain and silently made it a float variable
// initialized to something, probably 0.0f)
float GetScreenSeparationForGuis();
// DG end

/*
================
idGuiModel::EmitFullScreen

Creates a view that covers the screen and emit the surfaces
================
*/
void idGuiModel::EmitFullScreen()
{
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (vitaCapture != NULL) {
        VitaInterruptCapture();
    }
#endif

    if (surfaces[0].numIndexes == 0) {
        return;
    }

    SCOPED_PROFILE_EVENT("Gui::EmitFullScreen");

    viewDef_t* viewDef = (viewDef_t*)R_ClearedFrameAlloc(sizeof(*viewDef), FRAME_ALLOC_VIEW_DEF);
    viewDef->is2Dgui = true;
    tr.GetCroppedViewport(&viewDef->viewport);

    bool stereoEnabled = (renderSystem->GetStereo3DMode() != STEREO3D_OFF);
    if (stereoEnabled) {
        const float screenSeparation = GetScreenSeparationForGuis();

        // this will be negated on the alternate eyes, both rendered each frame
        viewDef->renderView.stereoScreenSeparation = screenSeparation;

        extern idCVar stereoRender_swapEyes;
        viewDef->renderView.viewEyeBuffer = 0; // render to both buffers
        if (stereoRender_swapEyes.GetBool()) {
            viewDef->renderView.stereoScreenSeparation = -screenSeparation;
        }
    }

    viewDef->scissor.x1 = 0;
    viewDef->scissor.y1 = 0;
    viewDef->scissor.x2 = viewDef->viewport.x2 - viewDef->viewport.x1;
    viewDef->scissor.y2 = viewDef->viewport.y2 - viewDef->viewport.y1;

    viewDef->projectionMatrix[0 * 4 + 0] = 2.0f / renderSystem->GetVirtualWidth();
    viewDef->projectionMatrix[0 * 4 + 1] = 0.0f;
    viewDef->projectionMatrix[0 * 4 + 2] = 0.0f;
    viewDef->projectionMatrix[0 * 4 + 3] = 0.0f;

    viewDef->projectionMatrix[1 * 4 + 0] = 0.0f;
    viewDef->projectionMatrix[1 * 4 + 1] = -2.0f / renderSystem->GetVirtualHeight();
    viewDef->projectionMatrix[1 * 4 + 2] = 0.0f;
    viewDef->projectionMatrix[1 * 4 + 3] = 0.0f;

    viewDef->projectionMatrix[2 * 4 + 0] = 0.0f;
    viewDef->projectionMatrix[2 * 4 + 1] = 0.0f;
    viewDef->projectionMatrix[2 * 4 + 2] = -2.0f;
    viewDef->projectionMatrix[2 * 4 + 3] = 0.0f;

    viewDef->projectionMatrix[3 * 4 + 0] = -1.0f;
    viewDef->projectionMatrix[3 * 4 + 1] = 1.0f;
    viewDef->projectionMatrix[3 * 4 + 2] = -1.0f;
    viewDef->projectionMatrix[3 * 4 + 3] = 1.0f;

    // make a tech5 renderMatrix for faster culling
    idRenderMatrix::Transpose(*(idRenderMatrix*)viewDef->projectionMatrix, viewDef->projectionRenderMatrix);

    viewDef->worldSpace.modelMatrix[0 * 4 + 0] = 1.0f;
    viewDef->worldSpace.modelMatrix[1 * 4 + 1] = 1.0f;
    viewDef->worldSpace.modelMatrix[2 * 4 + 2] = 1.0f;
    viewDef->worldSpace.modelMatrix[3 * 4 + 3] = 1.0f;

    viewDef->worldSpace.modelViewMatrix[0 * 4 + 0] = 1.0f;
    viewDef->worldSpace.modelViewMatrix[1 * 4 + 1] = 1.0f;
    viewDef->worldSpace.modelViewMatrix[2 * 4 + 2] = 1.0f;
    viewDef->worldSpace.modelViewMatrix[3 * 4 + 3] = 1.0f;

    viewDef->maxDrawSurfs = surfaces.Num();
    viewDef->drawSurfs = (drawSurf_t**)R_FrameAlloc(viewDef->maxDrawSurfs * sizeof(viewDef->drawSurfs[0]), FRAME_ALLOC_DRAW_SURFACE_POINTER);
    viewDef->numDrawSurfs = 0;

#if 1
    // RB: give renderView the current time to calculate 2D shader effects
    int shaderTime = tr.frameShaderTime * 1000; // Sys_Milliseconds();
    viewDef->renderView.time[0] = shaderTime;
    viewDef->renderView.time[1] = shaderTime;
    // RB end
#endif

    viewDef_t* oldViewDef = tr.viewDef;
    tr.viewDef = viewDef;

    EmitSurfaces(viewDef->worldSpace.modelMatrix, viewDef->worldSpace.modelViewMatrix,
        false /* depthHack */, stereoEnabled /* stereoDepthSort */, false /* link as entity */);

    tr.viewDef = oldViewDef;

    // add the command to draw this view
    R_AddDrawViewCmd(viewDef, true);
}

/*
=============
AdvanceSurf
=============
*/
void idGuiModel::AdvanceSurf()
{
    guiModelSurface_t s;

    if (surfaces.Num()) {
        s.material = surf->material;
        s.glState = surf->glState;
    } else {
        s.material = tr.defaultMaterial;
        s.glState = 0;
    }

    // advance indexes so the pointer to each surface will be 16 byte aligned
    numIndexes = ALIGN(numIndexes, 8);

    s.numIndexes = 0;
    s.firstIndex = numIndexes;

    surfaces.Append(s);
    surf = &surfaces[surfaces.Num() - 1];
}

/*
=============
AllocTris
=============
*/
idDrawVert* idGuiModel::AllocTris(int vertCount, const triIndex_t* tempIndexes, int indexCount, const idMaterial* material, const uint64 glState, const stereoDepthType_t stereoType)
{
    if (material == NULL) {
        return NULL;
    }
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    if (vitaCapture != NULL) {
        // recorded, written into the frame's buffers when played back
        vitaGuiCapture_t* cap = vitaCapture;
        vitaGuiCall_t& call = cap->calls.Alloc();
        call.firstVert = cap->verts.Num();
        call.numVerts = vertCount;
        call.firstIndex = cap->indexes.Num();
        call.numIndexes = indexCount;
        call.material = material;
        call.glState = glState;
        call.stereoType = stereoType;
        cap->verts.SetNum(call.firstVert + vertCount);
        cap->indexes.SetNum(call.firstIndex + indexCount);
        if (indexCount > 0) {
            memcpy(cap->indexes.Ptr() + call.firstIndex, tempIndexes, indexCount * sizeof(triIndex_t));
        }
        return cap->verts.Ptr() + call.firstVert;
    }
#endif
    if (numIndexes + indexCount > MAX_INDEXES) {
        static int warningFrame = 0;
        if (warningFrame != tr.frameCount) {
            warningFrame = tr.frameCount;
            idLib::Warning("idGuiModel::AllocTris: MAX_INDEXES exceeded");
        }
        return NULL;
    }
    if (numVerts + vertCount > MAX_VERTS) {
        static int warningFrame = 0;
        if (warningFrame != tr.frameCount) {
            warningFrame = tr.frameCount;
            idLib::Warning("idGuiModel::AllocTris: MAX_VERTS exceeded");
        }
        return NULL;
    }

    // break the current surface if we are changing to a new material or we can't
    // fit the data into our allocated block
    if (material != surf->material || glState != surf->glState || stereoType != surf->stereoType) {
        if (surf->numIndexes) {
            AdvanceSurf();
        }
        surf->material = material;
        surf->glState = glState;
        surf->stereoType = stereoType;
    }

    int startVert = numVerts;
    int startIndex = numIndexes;

    numVerts += vertCount;
    numIndexes += indexCount;

    surf->numIndexes += indexCount;

    if ((startIndex & 1) || (indexCount & 1)) {
        // slow for write combined memory!
        // this should be very rare, since quads are always an even index count
        for (int i = 0; i < indexCount; i++) {
            indexPointer[startIndex + i] = startVert + tempIndexes[i];
        }
    } else {
        for (int i = 0; i < indexCount; i += 2) {
            WriteIndexPair(indexPointer + startIndex + i, startVert + tempIndexes[i], startVert + tempIndexes[i + 1]);
        }
    }

    return vertexPointer + startVert;
}

#if !defined(__vita__) && defined(ID_VITA_PHYSCHECK)
idCVar vita_guiLODMeasure("vita_guiLODMeasure", "0", CVAR_RENDERER | CVAR_BOOL, "PC test build: compare each 15 Hz GUI recording with the one before (what an interpolation could use)");
#endif
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
=============
idGuiModel::VitaBeginCapture / VitaEndCapture / VitaReplay

See vitaGuiCapture_t (GuiModel.h). The playback goes through AllocTris with
the recorded arguments in the recorded order, so the surfaces split where they
did and the vertices and indexes land as they did.
=============
*/
void idGuiModel::VitaBeginCapture(vitaGuiCapture_t* cap)
{
    if (vitaCapture != NULL) {
        VitaInterruptCapture();
    }
    if (cap->verts.GetGranularity() < 1024) {
        cap->verts.SetGranularity(1024);
        cap->indexes.SetGranularity(1024);
        cap->calls.SetGranularity(256);
    }
#if !defined(__vita__)
    if (vita_guiLODMeasure.GetBool() && cap->valid) {
        cap->prevVerts = cap->verts;
        cap->prevIndexes = cap->indexes;
        cap->prevCalls = cap->calls;
        cap->prevValid = true;
    } else if (!cap->valid) {
        cap->prevValid = false;
    }
#endif
    cap->verts.SetNum(0);
    cap->indexes.SetNum(0);
    cap->calls.SetNum(0);
    cap->valid = false;
    vitaCapture = cap;
}

#if !defined(__vita__)
/*
=============
Vita_GuiMeasure (PC test build, vita_guiLODMeasure 1)

What interpolating a 15 Hz GUI between its recordings could use: a new
recording against the one before it - the same (replay is already right),
the same layout with vertices that moved, faded or scrolled (a blend of the
two would be possible), or a different layout (nothing to blend).
=============
*/
static void Vita_GuiMeasure(const vitaGuiCapture_t* cap)
{
    if (!cap->prevValid) {
        return;
    }
    bool sameLayout = cap->calls.Num() == cap->prevCalls.Num() && cap->verts.Num() == cap->prevVerts.Num() && cap->indexes.Num() == cap->prevIndexes.Num();
    for (int i = 0; sameLayout && i < cap->calls.Num(); i++) {
        const vitaGuiCall_t& a = cap->calls[i];
        const vitaGuiCall_t& b = cap->prevCalls[i];
        sameLayout = a.numVerts == b.numVerts && a.numIndexes == b.numIndexes && a.material == b.material && a.glState == b.glState;
    }
    if (sameLayout && cap->indexes.Num() > 0) {
        sameLayout = memcmp(cap->indexes.Ptr(), cap->prevIndexes.Ptr(), cap->indexes.Num() * sizeof(triIndex_t)) == 0;
    }
    if (!sameLayout) {
        if (cap->kind == 1) {
            VITA_ZONE_COUNT("gui measure: HUD - different layout (nothing to blend)");
        } else {
            VITA_ZONE_COUNT("gui measure: world screen - different layout (nothing to blend)");
        }
        return;
    }
    int moved = 0, recolored = 0, retextured = 0;
    for (int i = 0; i < cap->verts.Num(); i++) {
        const idDrawVert& a = cap->verts[i];
        const idDrawVert& b = cap->prevVerts[i];
        if (memcmp(&a.xyz, &b.xyz, sizeof(a.xyz)) != 0) {
            moved++;
        }
        if (memcmp(a.color, b.color, sizeof(a.color)) != 0) {
            recolored++;
        }
        if (a.GetTexCoordNativeS() != b.GetTexCoordNativeS() || a.GetTexCoordNativeT() != b.GetTexCoordNativeT()) {
            retextured++;
        }
    }
    if (cap->kind == 1) {
        if (moved + recolored + retextured == 0) {
            VITA_ZONE_COUNT("gui measure: HUD - identical (replay exact)");
        } else {
            VITA_ZONE_COUNT("gui measure: HUD - same layout, vertices changed (blendable)");
        }
        VITA_ZONE_ADD("gui measure: HUD - vertices recorded", cap->verts.Num());
        VITA_ZONE_ADD("gui measure: HUD - vertices moved", moved);
        VITA_ZONE_ADD("gui measure: HUD - vertices recolored (fades)", recolored);
        VITA_ZONE_ADD("gui measure: HUD - vertices with new texture coordinates", retextured);
    } else {
        if (moved + recolored + retextured == 0) {
            VITA_ZONE_COUNT("gui measure: world screen - identical (replay exact)");
        } else {
            VITA_ZONE_COUNT("gui measure: world screen - same layout, vertices changed (blendable)");
        }
        VITA_ZONE_ADD("gui measure: world screen - vertices recorded", cap->verts.Num());
        VITA_ZONE_ADD("gui measure: world screen - vertices moved", moved);
        VITA_ZONE_ADD("gui measure: world screen - vertices recolored (fades)", recolored);
        VITA_ZONE_ADD("gui measure: world screen - vertices with new texture coordinates", retextured);
    }
}
#endif

void idGuiModel::VitaEndCapture()
{
    vitaGuiCapture_t* cap = vitaCapture;
    if (cap == NULL) {
        return; // interrupted: already played back, left invalid
    }
    vitaCapture = NULL;
    VitaReplay(cap);
    VITA_ZONE_COUNT("gui recordings completed");
#if !defined(__vita__)
    if (vita_guiLODMeasure.GetBool()) {
        Vita_GuiMeasure(cap);
    }
#endif
    cap->valid = true;
    cap->frame = tr.frameCount;
}

void idGuiModel::VitaInterruptCapture()
{
    VITA_ZONE_COUNT("gui recording interrupted (model emptied or emitted mid-draw)");
    vitaGuiCapture_t* cap = vitaCapture;
    vitaCapture = NULL;
    VitaReplay(cap);
    cap->valid = false;
}

void idGuiModel::VitaReplay(const vitaGuiCapture_t* cap)
{
    for (int i = 0; i < cap->calls.Num(); i++) {
        const vitaGuiCall_t& call = cap->calls[i];
        idDrawVert* dst = AllocTris(call.numVerts, cap->indexes.Ptr() + call.firstIndex, call.numIndexes, call.material, call.glState, call.stereoType);
        if (dst != NULL && call.numVerts > 0) {
            memcpy(dst, cap->verts.Ptr() + call.firstVert, call.numVerts * sizeof(idDrawVert));
        }
    }
}
#endif
