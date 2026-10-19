/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans
Copyright (C) 2014 Carl Kenner

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
#include "../sys/vita/vita_zones.h" // VITA_ZONE_COUNT (vita_currentRenderReuse)

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
#include <psp2/kernel/sysmem.h> // sceKernelIsPSVitaTV (the stretch's sharpening)
#endif

#if defined(__vita__)
idCVar vita_forceWhiteFalloff("vita_forceWhiteFalloff", "0", CVAR_RENDERER | CVAR_BOOL,
    "diagnostic: bind a white texture in place of the light falloff image");
#endif

idCVar r_drawEyeColor("r_drawEyeColor", "0", CVAR_RENDERER | CVAR_BOOL, "Draw a colored box, red = left eye, blue = right eye, grey = non-stereo");
idCVar r_motionBlur("r_motionBlur", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "1 - 5, log2 of the number of motion blur samples");
idCVar r_forceZPassStencilShadows("r_forceZPassStencilShadows", "0", CVAR_RENDERER | CVAR_BOOL, "force Z-pass rendering for performance testing");
#if defined(__vita__)
// Preload draws every Z-fail shadow volume twice (a pass counting all its
// fragments, then a Z-pass one) - the stock choice for the Xbox 360, whose
// Z-cull rejects Z-fail fragments poorly. The Vita's tile-based GPU has no
// such cull, so the single Z-fail draw (Carmack's reverse) gives the same
// stencil counts for half the draws and half the fill.
idCVar r_useStencilShadowPreload("r_useStencilShadowPreload", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil shadow preload algorithm instead of Z-fail");
#else
idCVar r_useStencilShadowPreload("r_useStencilShadowPreload", "1", CVAR_RENDERER | CVAR_BOOL, "use stencil shadow preload algorithm instead of Z-fail");
#endif
idCVar r_skipShaderPasses("r_skipShaderPasses", "0", CVAR_RENDERER | CVAR_BOOL, "");
idCVar r_skipInteractionFastPath("r_skipInteractionFastPath", "1", CVAR_RENDERER | CVAR_BOOL, "");
idCVar r_useLightStencilSelect("r_useLightStencilSelect", "0", CVAR_RENDERER | CVAR_BOOL, "use stencil select pass");

extern idCVar stereoRender_swapEyes;

backEndState_t backEnd;

/*
================
SetVertexParm
================
*/
static ID_INLINE void SetVertexParm(renderParm_t rp, const float* value)
{
    renderProgManager.SetUniformValue(rp, value);
}

/*
================
SetVertexParms
================
*/
static ID_INLINE void SetVertexParms(renderParm_t rp, const float* value, int num)
{
    for (int i = 0; i < num; i++) {
        renderProgManager.SetUniformValue((renderParm_t)(rp + i), value + (i * 4));
    }
}

/*
================
SetFragmentParm
================
*/
static ID_INLINE void SetFragmentParm(renderParm_t rp, const float* value)
{
    renderProgManager.SetUniformValue(rp, value);
}

/*
================
RB_SetMVP
================
*/
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
// Vita: where the render back end's time goes, pass by pass, summed for the
// frame on the main thread (VitaBench takes and clears them at the end of
// each frame: the per-hitch "back end passes" line and the segment averages)
// - 0 depth pre-pass, 1 stencil shadow volumes, 2 light interaction passes,
// 3 interactions in all (1 + 2 + their set up), 4 shader passes (ambient,
// translucent, GUIs, 2D), 5 fog and blend lights, 6 framebuffer copies
// (_currentRender / _currentDepth), 7 the views drawn in all (RB_DrawView),
// 8 the soft alpha-tested edges' map (RB_VitaAlphaEdgeMask), 9 the 3D view's
// stretch in all (RB_PostProcess: the map, the copy, the sharpening pass)
uint64 vita_rbPassUs[10];
int vita_rbDrawCalls = 0;
struct vitaRBTimer_t {
    int pass;
    uint64 start;
    explicit vitaRBTimer_t(int p) : pass(p), start(Sys_Microseconds()) {}
    ~vitaRBTimer_t()
    {
        vita_rbPassUs[pass] += Sys_Microseconds() - start;
    }
};
#define VITA_RB_PASS(p) vitaRBTimer_t vitaRBTimer_pass(p)
#else
#define VITA_RB_PASS(p)
#endif

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
// _currentRender reuse (vita_currentRenderReuse, RB_DrawViewInternal)
idCVar vita_currentRenderReuse("vita_screenCopyReuse2", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "Vita: the main view's screen copy for warping effects (heat haze, glass, distortion) is reused while nothing it shows has changed - the view still, and every surface drawn before it (geometry, entity placement, shader parameters, lights) the same as when the copy was made; any change, a video or a subview on screen, and a new copy is made");
static uint64 vitaLastCopySignature = 0;

/*
==================
RB_VitaSceneSignature

A fingerprint of everything the main view drew before its screen copy: each
surface's material, geometry (vertex cache handles - frame-temporary ones,
i.e. anything animated, skinned, a particle or a GUI, are new every frame),
the placement of its entity and its evaluated shader registers (time-driven
scrolling, pulsing, flicker), and each light's place, projection, shader
registers and interaction / shadow surfaces. The same fingerprint as when the
last copy was made means the copy is what a new one would show (2026-10-13
the reuse only checked that the view stood still, and glass showed things
behind it moving at 15 Hz). 0 = can not be reused (a video or subview drawn).
==================
*/
static uint64 RB_VitaHashWords(uint64 h, const void* data, int bytes)
{
    const uint32* w = (const uint32*)data;
    for (int i = 0; i < bytes / 4; i++) {
        h = (h ^ w[i]) * 1099511628211ull;
    }
    return h;
}

static uint64 RB_VitaHashSurf(uint64 h, const drawSurf_t* surf)
{
    h = RB_VitaHashWords(h, &surf->material, sizeof(surf->material));
    h = RB_VitaHashWords(h, &surf->indexCache, sizeof(surf->indexCache));
    h = RB_VitaHashWords(h, &surf->ambientCache, sizeof(surf->ambientCache));
    h = RB_VitaHashWords(h, &surf->shadowCache, sizeof(surf->shadowCache));
    h = RB_VitaHashWords(h, &surf->jointCache, sizeof(surf->jointCache));
    h = RB_VitaHashWords(h, &surf->numIndexes, sizeof(surf->numIndexes));
    h = RB_VitaHashWords(h, &surf->extraGLState, sizeof(surf->extraGLState));
    if (surf->space != NULL) {
        h = RB_VitaHashWords(h, surf->space->modelMatrix, sizeof(surf->space->modelMatrix));
    }
    if (surf->material != NULL && surf->shaderRegisters != NULL) {
        h = RB_VitaHashWords(h, surf->shaderRegisters, surf->material->GetNumRegisters() * (int)sizeof(float));
    }
    return h;
}

static bool RB_VitaMaterialIsLive(const idMaterial* m)
{
    if (m == NULL) {
        return false;
    }
    if (m->HasSubview()) {
        return true; // a mirror or remote camera: its picture is redrawn every frame
    }
    for (int i = 0; i < m->GetNumStages(); i++) {
        if (m->GetStage(i)->texture.cinematic != NULL) {
            return true; // a video
        }
    }
    return false;
}

static uint64 RB_VitaSceneSignature(const viewDef_t* viewDef, drawSurf_t** drawSurfs, int numSurfs)
{
    uint64 h = 1469598103934665603ull;
    // the view itself, exactly (place, direction, field of view)
    h = RB_VitaHashWords(h, &viewDef->renderView.vieworg, sizeof(viewDef->renderView.vieworg));
    h = RB_VitaHashWords(h, &viewDef->renderView.viewaxis, sizeof(viewDef->renderView.viewaxis));
    h = RB_VitaHashWords(h, &viewDef->renderView.fov_x, sizeof(float));
    h = RB_VitaHashWords(h, &viewDef->renderView.fov_y, sizeof(float));
    for (int i = 0; i < numSurfs; i++) {
        const drawSurf_t* surf = drawSurfs[i];
        if (RB_VitaMaterialIsLive(surf->material)) {
            return 0;
        }
        h = RB_VitaHashSurf(h, surf);
    }
    for (const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        h = RB_VitaHashWords(h, &vLight->lightDef, sizeof(vLight->lightDef));
        h = RB_VitaHashWords(h, &vLight->globalLightOrigin, sizeof(vLight->globalLightOrigin));
        h = RB_VitaHashWords(h, vLight->lightProject, sizeof(vLight->lightProject));
        h = RB_VitaHashWords(h, &vLight->lightShader, sizeof(vLight->lightShader));
        if (RB_VitaMaterialIsLive(vLight->lightShader)) {
            return 0;
        }
        if (vLight->lightShader != NULL && vLight->shaderRegisters != NULL) {
            h = RB_VitaHashWords(h, vLight->shaderRegisters, vLight->lightShader->GetNumRegisters() * (int)sizeof(float));
        }
        const drawSurf_t* lists[5] = { vLight->globalShadows, vLight->localInteractions, vLight->localShadows, vLight->globalInteractions, vLight->translucentInteractions };
        for (int l = 0; l < 5; l++) {
            h = (h ^ (uint64)(0x9e37 + l)) * 1099511628211ull;
            for (const drawSurf_t* s = lists[l]; s != NULL; s = s->nextOnLight) {
                h = RB_VitaHashSurf(h, s);
            }
        }
    }
    return h != 0 ? h : 1;
}
// the view the last copy was made from (vita_currentRenderReuse)
static idVec3 vitaLastCopyOrigin(0, 0, 0);
static idMat3 vitaLastCopyAxis(mat3_identity);
extern bool vita_fullQuality; // cinematics: no cuts (Game_local / common_frame)
static int vitaMainViewDraws = 0;
static int vitaLastCopyDraw = -100;
static bool vitaLastCopyMain = false;
static bool vitaLastWasReuse = false;
static int vitaLastCopyRect[4] = { 0, 0, 0, 0 };
#endif

void RB_SetMVP(const idRenderMatrix& mvp)
{
    SetVertexParms(RENDERPARM_MVPMATRIX_X, mvp[0], 4);
}

/*
================
RB_SetMVPWithStereoOffset
================
*/
static void RB_SetMVPWithStereoOffset(const idRenderMatrix& mvp, const float stereoOffset)
{
    idRenderMatrix offset = mvp;
    offset[0][3] += stereoOffset;

    SetVertexParms(RENDERPARM_MVPMATRIX_X, offset[0], 4);
}

static const float zero[4] = { 0, 0, 0, 0 };
static const float one[4] = { 1, 1, 1, 1 };
static const float negOne[4] = { -1, -1, -1, -1 };

/*
================
RB_SetVertexColorParms
================
*/
static void RB_SetVertexColorParms(stageVertexColor_t svc)
{
    switch (svc) {
    case SVC_IGNORE:
        SetVertexParm(RENDERPARM_VERTEXCOLOR_MODULATE, zero);
        SetVertexParm(RENDERPARM_VERTEXCOLOR_ADD, one);
        break;
    case SVC_MODULATE:
        SetVertexParm(RENDERPARM_VERTEXCOLOR_MODULATE, one);
        SetVertexParm(RENDERPARM_VERTEXCOLOR_ADD, zero);
        break;
    case SVC_INVERSE_MODULATE:
        SetVertexParm(RENDERPARM_VERTEXCOLOR_MODULATE, negOne);
        SetVertexParm(RENDERPARM_VERTEXCOLOR_ADD, one);
        break;
    }
}

/*
================
RB_DrawElementsWithCounters
================
*/
void RB_DrawElementsWithCounters(const drawSurf_t* surf)
{
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    vita_rbDrawCalls++;
#endif
    // get vertex buffer
    const vertCacheHandle_t vbHandle = surf->ambientCache;
    idVertexBuffer* vertexBuffer;
    if (vertexCache.CacheIsStatic(vbHandle)) {
        vertexBuffer = &vertexCache.staticData.vertexBuffer;
    } else {
        const uint64 frameNum = (int)(vbHandle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
        if (frameNum != ((vertexCache.currentFrame - 1) & VERTCACHE_FRAME_MASK)) {
            idLib::Warning("RB_DrawElementsWithCounters, vertexBuffer == NULL");
            return;
        }
        vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
    }
    const int vertOffset = (int)(vbHandle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;

    // get index buffer
    const vertCacheHandle_t ibHandle = surf->indexCache;
    idIndexBuffer* indexBuffer;
    if (vertexCache.CacheIsStatic(ibHandle)) {
        indexBuffer = &vertexCache.staticData.indexBuffer;
    } else {
        const uint64 frameNum = (int)(ibHandle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
        if (frameNum != ((vertexCache.currentFrame - 1) & VERTCACHE_FRAME_MASK)) {
            idLib::Warning("RB_DrawElementsWithCounters, indexBuffer == NULL");
            return;
        }
        indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
    }
    // RB: 64 bit fixes, changed int to GLintptrARB
    const GLintptrARB indexOffset = (GLintptrARB)(ibHandle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;
    // RB end

    RENDERLOG_PRINTF("Binding Buffers: %p:%i %p:%i\n", vertexBuffer, vertOffset, indexBuffer, indexOffset);

    if (surf->jointCache) {
        // DG: this happens all the time in the erebus1 map with blendlight.vfp,
        // so don't call assert (through verify) here until it's fixed (if fixable)
        // else the game crashes on linux when using debug builds

        // FIXME: fix this properly if possible?
        // RB: yes but it would require an additional blend light skinned shader
        // if( !verify( renderProgManager.ShaderUsesJoints() ) )
        if (!renderProgManager.ShaderUsesJoints())
        // DG end
        {
            return;
        }
    } else {
        if (!verify(!renderProgManager.ShaderUsesJoints() || renderProgManager.ShaderHasOptionalSkinning())) {
            return;
        }
    }

    if (surf->jointCache) {
        idJointBuffer jointBuffer;
        if (!vertexCache.GetJointBuffer(surf->jointCache, &jointBuffer)) {
            idLib::Warning("RB_DrawElementsWithCounters, jointBuffer == NULL");
            return;
        }
        assert((jointBuffer.GetOffset() & (glConfig.uniformBufferOffsetAlignment - 1)) == 0);

        // RB: 64 bit fixes, changed GLuint to GLintptrARB
        const GLintptrARB ubo = reinterpret_cast<GLintptrARB>(jointBuffer.GetAPIObject());
        // RB end

        qglBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetNumJoints() * sizeof(idJointMat));
    }

    renderProgManager.CommitUniforms();

    // RB: 64 bit fixes, changed GLuint to GLintptrARB
    if (backEnd.glState.currentIndexBuffer != (GLintptrARB)indexBuffer->GetAPIObject() || !r_useStateCaching.GetBool()) {
        qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, (GLintptrARB)indexBuffer->GetAPIObject());
        backEnd.glState.currentIndexBuffer = (GLintptrARB)indexBuffer->GetAPIObject();
    }

#if defined(__vita__)
    // vitaGL accepts glDrawElementsBaseVertex() but never reads baseVertex - it
    // forwards a literal 0 into the GXM draw, where that third parameter is a
    // highest-index hint rather than a base. BFG leans on baseVertex for every
    // surface: a frame's vertex cache packs them all into one buffer and each
    // surface's indices are relative to its own start. Ignored, every surface
    // but the first reads another surface's vertices - silently, and with
    // nothing reaching the screen.
    //
    // Fold the base into the attribute offsets instead. That is exactly
    // equivalent, because vitaGL stores those as byte offsets into the bound
    // buffer, and key the layout cache on it so the pointers are re-emitted
    // whenever the base moves.
    const uintptr_t vertBase = (uintptr_t)vertOffset;

    if ((backEnd.glState.vertexLayout != LAYOUT_DRAW_VERT) || (backEnd.glState.currentVertexBuffer != (GLintptrARB)vertexBuffer->GetAPIObject()) || (backEnd.glState.currentVertOffset != vertBase) || !r_useStateCaching.GetBool()) {
        qglBindBufferARB(GL_ARRAY_BUFFER_ARB, (GLintptrARB)vertexBuffer->GetAPIObject());
        backEnd.glState.currentVertexBuffer = (GLintptrARB)vertexBuffer->GetAPIObject();
        backEnd.glState.currentVertOffset = vertBase;

        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_VERTEX);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_NORMAL);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR2);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_ST);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_TANGENT);

        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_XYZ_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_NORMAL_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_COLOR_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_COLOR2_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_ST_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(vertBase + DRAWVERT_TANGENT_OFFSET));

        backEnd.glState.vertexLayout = LAYOUT_DRAW_VERT;
    }

    qglDrawElements(GL_TRIANGLES,
        r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
        GL_INDEX_TYPE,
        (triIndex_t*)indexOffset);
#else
    if ((backEnd.glState.vertexLayout != LAYOUT_DRAW_VERT) || (backEnd.glState.currentVertexBuffer != (GLintptrARB)vertexBuffer->GetAPIObject()) || !r_useStateCaching.GetBool()) {
        qglBindBufferARB(GL_ARRAY_BUFFER_ARB, (GLintptrARB)vertexBuffer->GetAPIObject());
        backEnd.glState.currentVertexBuffer = (GLintptrARB)vertexBuffer->GetAPIObject();

        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_VERTEX);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_NORMAL);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR2);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_ST);
        qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_TANGENT);

        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_VERTEX, 3, GL_FLOAT, GL_FALSE, sizeof(idDrawVert), (void*)(DRAWVERT_XYZ_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_NORMAL, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(DRAWVERT_NORMAL_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(DRAWVERT_COLOR_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(DRAWVERT_COLOR2_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_ST, 2, GL_HALF_FLOAT, GL_TRUE, sizeof(idDrawVert), (void*)(DRAWVERT_ST_OFFSET));
        qglVertexAttribPointerARB(PC_ATTRIB_INDEX_TANGENT, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idDrawVert), (void*)(DRAWVERT_TANGENT_OFFSET));

        backEnd.glState.vertexLayout = LAYOUT_DRAW_VERT;
    }
    // RB end

    qglDrawElementsBaseVertex(GL_TRIANGLES,
        r_singleTriangle.GetBool() ? 3 : surf->numIndexes,
        GL_INDEX_TYPE,
        (triIndex_t*)indexOffset,
        vertOffset / sizeof(idDrawVert));
#endif

    // RB: added stats
    backEnd.pc.c_drawElements++;
    backEnd.pc.c_drawIndexes += surf->numIndexes;
    // RB end
}

/*
======================
RB_GetShaderTextureMatrix
======================
*/
static void RB_GetShaderTextureMatrix(const float* shaderRegisters, const textureStage_t* texture, float matrix[16])
{
    matrix[0 * 4 + 0] = shaderRegisters[texture->matrix[0][0]];
    matrix[1 * 4 + 0] = shaderRegisters[texture->matrix[0][1]];
    matrix[2 * 4 + 0] = 0.0f;
    matrix[3 * 4 + 0] = shaderRegisters[texture->matrix[0][2]];

    matrix[0 * 4 + 1] = shaderRegisters[texture->matrix[1][0]];
    matrix[1 * 4 + 1] = shaderRegisters[texture->matrix[1][1]];
    matrix[2 * 4 + 1] = 0.0f;
    matrix[3 * 4 + 1] = shaderRegisters[texture->matrix[1][2]];

    // we attempt to keep scrolls from generating incredibly large texture values, but
    // center rotations and center scales can still generate offsets that need to be > 1
    if (matrix[3 * 4 + 0] < -40.0f || matrix[12] > 40.0f) {
        matrix[3 * 4 + 0] -= (int)matrix[3 * 4 + 0];
    }
    if (matrix[13] < -40.0f || matrix[13] > 40.0f) {
        matrix[13] -= (int)matrix[13];
    }

    matrix[0 * 4 + 2] = 0.0f;
    matrix[1 * 4 + 2] = 0.0f;
    matrix[2 * 4 + 2] = 1.0f;
    matrix[3 * 4 + 2] = 0.0f;

    matrix[0 * 4 + 3] = 0.0f;
    matrix[1 * 4 + 3] = 0.0f;
    matrix[2 * 4 + 3] = 0.0f;
    matrix[3 * 4 + 3] = 1.0f;
}

/*
======================
RB_LoadShaderTextureMatrix
======================
*/
static void RB_LoadShaderTextureMatrix(const float* shaderRegisters, const textureStage_t* texture)
{
    float texS[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    float texT[4] = { 0.0f, 1.0f, 0.0f, 0.0f };

    if (texture->hasMatrix) {
        float matrix[16];
        RB_GetShaderTextureMatrix(shaderRegisters, texture, matrix);
        texS[0] = matrix[0 * 4 + 0];
        texS[1] = matrix[1 * 4 + 0];
        texS[2] = matrix[2 * 4 + 0];
        texS[3] = matrix[3 * 4 + 0];

        texT[0] = matrix[0 * 4 + 1];
        texT[1] = matrix[1 * 4 + 1];
        texT[2] = matrix[2 * 4 + 1];
        texT[3] = matrix[3 * 4 + 1];

        RENDERLOG_PRINTF("Setting Texture Matrix\n");
        renderLog.Indent();
        RENDERLOG_PRINTF("Texture Matrix S : %4.3f, %4.3f, %4.3f, %4.3f\n", texS[0], texS[1], texS[2], texS[3]);
        RENDERLOG_PRINTF("Texture Matrix T : %4.3f, %4.3f, %4.3f, %4.3f\n", texT[0], texT[1], texT[2], texT[3]);
        renderLog.Outdent();
    }

    SetVertexParm(RENDERPARM_TEXTUREMATRIX_S, texS);
    SetVertexParm(RENDERPARM_TEXTUREMATRIX_T, texT);
}

/*
=====================
RB_BakeTextureMatrixIntoTexgen
=====================
*/
static void RB_BakeTextureMatrixIntoTexgen(idPlane lightProject[3], const float* textureMatrix)
{
    float genMatrix[16];
    float final[16];

    genMatrix[0 * 4 + 0] = lightProject[0][0];
    genMatrix[1 * 4 + 0] = lightProject[0][1];
    genMatrix[2 * 4 + 0] = lightProject[0][2];
    genMatrix[3 * 4 + 0] = lightProject[0][3];

    genMatrix[0 * 4 + 1] = lightProject[1][0];
    genMatrix[1 * 4 + 1] = lightProject[1][1];
    genMatrix[2 * 4 + 1] = lightProject[1][2];
    genMatrix[3 * 4 + 1] = lightProject[1][3];

    genMatrix[0 * 4 + 2] = 0.0f;
    genMatrix[1 * 4 + 2] = 0.0f;
    genMatrix[2 * 4 + 2] = 0.0f;
    genMatrix[3 * 4 + 2] = 0.0f;

    genMatrix[0 * 4 + 3] = lightProject[2][0];
    genMatrix[1 * 4 + 3] = lightProject[2][1];
    genMatrix[2 * 4 + 3] = lightProject[2][2];
    genMatrix[3 * 4 + 3] = lightProject[2][3];

    R_MatrixMultiply(genMatrix, textureMatrix, final);

    lightProject[0][0] = final[0 * 4 + 0];
    lightProject[0][1] = final[1 * 4 + 0];
    lightProject[0][2] = final[2 * 4 + 0];
    lightProject[0][3] = final[3 * 4 + 0];

    lightProject[1][0] = final[0 * 4 + 1];
    lightProject[1][1] = final[1 * 4 + 1];
    lightProject[1][2] = final[2 * 4 + 1];
    lightProject[1][3] = final[3 * 4 + 1];
}

/*
======================
RB_BindVariableStageImage

Handles generating a cinematic frame if needed
======================
*/
static void RB_BindVariableStageImage(const textureStage_t* texture, const float* shaderRegisters)
{
    if (texture->cinematic) {
        cinData_t cin;

        if (r_skipDynamicTextures.GetBool()) {
            globalImages->defaultImage->Bind();
            return;
        }

        // offset time by shaderParm[7] (FIXME: make the time offset a parameter of the shader?)
        // We make no attempt to optimize for multiple identical cinematics being in view, or
        // for cinematics going at a lower framerate than the renderer.
        cin = texture->cinematic->ImageForTime(backEnd.viewDef->renderView.time[0] + idMath::Ftoi(1000.0f * backEnd.viewDef->renderView.shaderParms[11]));
        if (cin.imageY != NULL) {
            GL_SelectTexture(0);
            cin.imageY->Bind();
            GL_SelectTexture(1);
            cin.imageCr->Bind();
            GL_SelectTexture(2);
            cin.imageCb->Bind();

        } else if (cin.image != NULL) {
            // Carl: A single RGB image works better with the FFMPEG BINK codec.
            GL_SelectTexture(0);
            cin.image->Bind();
            renderProgManager.BindShader_TextureVertexColor();
        } else {
            globalImages->blackImage->Bind();
            // because the shaders may have already been set - we need to make sure we are not using a bink shader which would
            // display incorrectly.  We may want to get rid of RB_BindVariableStageImage and inline the code so that the
            // SWF GUI case is handled better, too
            renderProgManager.BindShader_TextureVertexColor();
        }
    } else {
        // FIXME: see why image is invalid
        if (texture->image != NULL) {
            texture->image->Bind();
        }
    }
}

/*
================
RB_PrepareStageTexturing
================
*/
static void RB_PrepareStageTexturing(const shaderStage_t* pStage, const drawSurf_t* surf)
{
    float useTexGenParm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // set the texture matrix if needed
    RB_LoadShaderTextureMatrix(surf->shaderRegisters, &pStage->texture);

    // texgens
    if (pStage->texture.texgen == TG_REFLECT_CUBE) {

        // see if there is also a bump map specified
        const shaderStage_t* bumpStage = surf->material->GetBumpStage();
        if (bumpStage != NULL) {
            // per-pixel reflection mapping with bump mapping
            GL_SelectTexture(1);
            bumpStage->texture.image->Bind();
            GL_SelectTexture(0);

            RENDERLOG_PRINTF("TexGen: TG_REFLECT_CUBE: Bumpy Environment\n");
            if (surf->jointCache) {
                renderProgManager.BindShader_BumpyEnvironmentSkinned();
            } else {
                renderProgManager.BindShader_BumpyEnvironment();
            }
        } else {
            RENDERLOG_PRINTF("TexGen: TG_REFLECT_CUBE: Environment\n");
            if (surf->jointCache) {
                renderProgManager.BindShader_EnvironmentSkinned();
            } else {
                renderProgManager.BindShader_Environment();
            }
        }

    } else if (pStage->texture.texgen == TG_SKYBOX_CUBE) {

        renderProgManager.BindShader_SkyBox();

    } else if (pStage->texture.texgen == TG_WOBBLESKY_CUBE) {

        const int* parms = surf->material->GetTexGenRegisters();

        float wobbleDegrees = surf->shaderRegisters[parms[0]] * (idMath::PI / 180.0f);
        float wobbleSpeed = surf->shaderRegisters[parms[1]] * (2.0f * idMath::PI / 60.0f);
        float rotateSpeed = surf->shaderRegisters[parms[2]] * (2.0f * idMath::PI / 60.0f);

        idVec3 axis[3];
        {
            // very ad-hoc "wobble" transform
            float s, c;
            idMath::SinCos(wobbleSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, s, c);

            float ws, wc;
            idMath::SinCos(wobbleDegrees, ws, wc);

            axis[2][0] = ws * c;
            axis[2][1] = ws * s;
            axis[2][2] = wc;

            axis[1][0] = -s * s * ws;
            axis[1][2] = -s * ws * ws;
            axis[1][1] = idMath::Sqrt(idMath::Fabs(1.0f - (axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2])));

            // make the second vector exactly perpendicular to the first
            axis[1] -= (axis[2] * axis[1]) * axis[2];
            axis[1].Normalize();

            // construct the third with a cross
            axis[0].Cross(axis[1], axis[2]);
        }

        // add the rotate
        float rs, rc;
        idMath::SinCos(rotateSpeed * backEnd.viewDef->renderView.time[0] * 0.001f, rs, rc);

        float transform[12];
        transform[0 * 4 + 0] = axis[0][0] * rc + axis[1][0] * rs;
        transform[0 * 4 + 1] = axis[0][1] * rc + axis[1][1] * rs;
        transform[0 * 4 + 2] = axis[0][2] * rc + axis[1][2] * rs;
        transform[0 * 4 + 3] = 0.0f;

        transform[1 * 4 + 0] = axis[1][0] * rc - axis[0][0] * rs;
        transform[1 * 4 + 1] = axis[1][1] * rc - axis[0][1] * rs;
        transform[1 * 4 + 2] = axis[1][2] * rc - axis[0][2] * rs;
        transform[1 * 4 + 3] = 0.0f;

        transform[2 * 4 + 0] = axis[2][0];
        transform[2 * 4 + 1] = axis[2][1];
        transform[2 * 4 + 2] = axis[2][2];
        transform[2 * 4 + 3] = 0.0f;

        SetVertexParms(RENDERPARM_WOBBLESKY_X, transform, 3);
        renderProgManager.BindShader_WobbleSky();

    } else if ((pStage->texture.texgen == TG_SCREEN) || (pStage->texture.texgen == TG_SCREEN2)) {

        useTexGenParm[0] = 1.0f;
        useTexGenParm[1] = 1.0f;
        useTexGenParm[2] = 1.0f;
        useTexGenParm[3] = 1.0f;

        float mat[16];
        R_MatrixMultiply(surf->space->modelViewMatrix, backEnd.viewDef->projectionMatrix, mat);

        RENDERLOG_PRINTF("TexGen : %s\n", (pStage->texture.texgen == TG_SCREEN) ? "TG_SCREEN" : "TG_SCREEN2");
        renderLog.Indent();

        float plane[4];
        plane[0] = mat[0 * 4 + 0];
        plane[1] = mat[1 * 4 + 0];
        plane[2] = mat[2 * 4 + 0];
        plane[3] = mat[3 * 4 + 0];
        SetVertexParm(RENDERPARM_TEXGEN_0_S, plane);
        RENDERLOG_PRINTF("TEXGEN_S = %4.3f, %4.3f, %4.3f, %4.3f\n", plane[0], plane[1], plane[2], plane[3]);

        plane[0] = mat[0 * 4 + 1];
        plane[1] = mat[1 * 4 + 1];
        plane[2] = mat[2 * 4 + 1];
        plane[3] = mat[3 * 4 + 1];
        SetVertexParm(RENDERPARM_TEXGEN_0_T, plane);
        RENDERLOG_PRINTF("TEXGEN_T = %4.3f, %4.3f, %4.3f, %4.3f\n", plane[0], plane[1], plane[2], plane[3]);

        plane[0] = mat[0 * 4 + 3];
        plane[1] = mat[1 * 4 + 3];
        plane[2] = mat[2 * 4 + 3];
        plane[3] = mat[3 * 4 + 3];
        SetVertexParm(RENDERPARM_TEXGEN_0_Q, plane);
        RENDERLOG_PRINTF("TEXGEN_Q = %4.3f, %4.3f, %4.3f, %4.3f\n", plane[0], plane[1], plane[2], plane[3]);

        renderLog.Outdent();

    } else if (pStage->texture.texgen == TG_DIFFUSE_CUBE) {

        // As far as I can tell, this is never used
        idLib::Warning("Using Diffuse Cube! Please contact Brian!");

    } else if (pStage->texture.texgen == TG_GLASSWARP) {

        // As far as I can tell, this is never used
        idLib::Warning("Using GlassWarp! Please contact Brian!");
    }

    SetVertexParm(RENDERPARM_TEXGEN_0_ENABLED, useTexGenParm);
}

/*
================
RB_FinishStageTexturing
================
*/
static void RB_FinishStageTexturing(const shaderStage_t* pStage, const drawSurf_t* surf)
{

    if (pStage->texture.cinematic) {
        // unbind the extra bink textures
        GL_SelectTexture(1);
        globalImages->BindNull();
        GL_SelectTexture(2);
        globalImages->BindNull();
        GL_SelectTexture(0);
    }

    if (pStage->texture.texgen == TG_REFLECT_CUBE) {
        // see if there is also a bump map specified
        const shaderStage_t* bumpStage = surf->material->GetBumpStage();
        if (bumpStage != NULL) {
            // per-pixel reflection mapping with bump mapping
            GL_SelectTexture(1);
            globalImages->BindNull();
            GL_SelectTexture(0);
        } else {
            // per-pixel reflection mapping without bump mapping
        }
        renderProgManager.Unbind();
    }
}

/*
=========================================================================================

DEPTH BUFFER RENDERING

=========================================================================================
*/

/*
==================
RB_FillDepthBufferGeneric
==================
*/
static void RB_FillDepthBufferGeneric(const drawSurf_t* const* drawSurfs, int numDrawSurfs)
{
    for (int i = 0; i < numDrawSurfs; i++) {
        const drawSurf_t* drawSurf = drawSurfs[i];
        const idMaterial* shader = drawSurf->material;

        // translucent surfaces don't put anything in the depth buffer and don't
        // test against it, which makes them fail the mirror clip plane operation
        if (shader->Coverage() == MC_TRANSLUCENT) {
            continue;
        }

        // get the expressions for conditionals / color / texcoords
        const float* regs = drawSurf->shaderRegisters;

        // if all stages of a material have been conditioned off, don't do anything
        int stage = 0;
        for (; stage < shader->GetNumStages(); stage++) {
            const shaderStage_t* pStage = shader->GetStage(stage);
            // check the stage enable condition
            if (regs[pStage->conditionRegister] != 0) {
                break;
            }
        }
        if (stage == shader->GetNumStages()) {
            continue;
        }

        // change the matrix if needed
        if (drawSurf->space != backEnd.currentSpace) {
            RB_SetMVP(drawSurf->space->mvp);

            backEnd.currentSpace = drawSurf->space;
        }

        uint64 surfGLState = 0;

        // set polygon offset if necessary
        if (shader->TestMaterialFlag(MF_POLYGONOFFSET)) {
            surfGLState |= GLS_POLYGON_OFFSET;
            GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
        }

        // subviews will just down-modulate the color buffer
        float color[4];
        if (shader->GetSort() == SS_SUBVIEW) {
            surfGLState |= GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO | GLS_DEPTHFUNC_LESS;
            color[0] = 1.0f;
            color[1] = 1.0f;
            color[2] = 1.0f;
            color[3] = 1.0f;
        } else {
            // others just draw black
            color[0] = 0.0f;
            color[1] = 0.0f;
            color[2] = 0.0f;
            color[3] = 1.0f;
        }

        renderLog.OpenBlock(shader->GetName());

        bool drawSolid = false;
        if (shader->Coverage() == MC_OPAQUE) {
            drawSolid = true;
        } else if (shader->Coverage() == MC_PERFORATED) {
            // we may have multiple alpha tested stages
            // if the only alpha tested stages are condition register omitted,
            // draw a normal opaque surface
            bool didDraw = false;

            // perforated surfaces may have multiple alpha tested stages
            for (stage = 0; stage < shader->GetNumStages(); stage++) {
                const shaderStage_t* pStage = shader->GetStage(stage);

                if (!pStage->hasAlphaTest) {
                    continue;
                }

                // check the stage enable condition
                if (regs[pStage->conditionRegister] == 0) {
                    continue;
                }

                // if we at least tried to draw an alpha tested stage,
                // we won't draw the opaque surface
                didDraw = true;

                // set the alpha modulate
                color[3] = regs[pStage->color.registers[3]];

                // skip the entire stage if alpha would be black
                if (color[3] <= 0.0f) {
                    continue;
                }

                uint64 stageGLState = surfGLState;

                // set privatePolygonOffset if necessary
                if (pStage->privatePolygonOffset) {
                    GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset);
                    stageGLState |= GLS_POLYGON_OFFSET;
                }

                GL_Color(color);

#ifdef USE_CORE_PROFILE
                GL_State(stageGLState);
                idVec4 alphaTestValue(regs[pStage->alphaTestRegister]);
                SetFragmentParm(RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr());
#else
                GL_State(stageGLState | GLS_ALPHATEST_FUNC_GREATER | GLS_ALPHATEST_MAKE_REF(idMath::Ftob(255.0f * regs[pStage->alphaTestRegister])));
#endif

                if (drawSurf->jointCache) {
                    renderProgManager.BindShader_TextureVertexColorSkinned();
                } else {
                    renderProgManager.BindShader_TextureVertexColor();
                }

                RB_SetVertexColorParms(SVC_IGNORE);

                // bind the texture
                GL_SelectTexture(0);
                pStage->texture.image->Bind();

                // set texture matrix and texGens
                RB_PrepareStageTexturing(pStage, drawSurf);

                // must render with less-equal for Z-Cull to work properly
                assert((GL_GetCurrentState() & GLS_DEPTHFUNC_BITS) == GLS_DEPTHFUNC_LESS);

                // draw it
                RB_DrawElementsWithCounters(drawSurf);

                // clean up
                RB_FinishStageTexturing(pStage, drawSurf);

                // unset privatePolygonOffset if necessary
                if (pStage->privatePolygonOffset) {
                    GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
                }
            }

            if (!didDraw) {
                drawSolid = true;
            }
        }

        // draw the entire surface solid
        if (drawSolid) {
            if (shader->GetSort() == SS_SUBVIEW) {
                renderProgManager.BindShader_Color();
                GL_Color(color);
                GL_State(surfGLState);
            } else {
                if (drawSurf->jointCache) {
                    renderProgManager.BindShader_DepthSkinned();
                } else {
                    renderProgManager.BindShader_Depth();
                }
                GL_State(surfGLState | GLS_ALPHAMASK);
            }

            // must render with less-equal for Z-Cull to work properly
            assert((GL_GetCurrentState() & GLS_DEPTHFUNC_BITS) == GLS_DEPTHFUNC_LESS);

            // draw it
            RB_DrawElementsWithCounters(drawSurf);
        }

        renderLog.CloseBlock();
    }

#ifdef USE_CORE_PROFILE
    SetFragmentParm(RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr());
#endif
}

/*
=====================
RB_FillDepthBufferFast

Optimized fast path code.

If there are subview surfaces, they must be guarded in the depth buffer to allow
the mirror / subview to show through underneath the current view rendering.

Surfaces with perforated shaders need the full shader setup done, but should be
drawn after the opaque surfaces.

The bulk of the surfaces should be simple opaque geometry that can be drawn very rapidly.

If there are no subview surfaces, we could clear to black and use fast-Z rendering
on the 360.
=====================
*/
static void RB_FillDepthBufferFast(drawSurf_t** drawSurfs, int numDrawSurfs)
{
    VITA_RB_PASS(0);
    if (numDrawSurfs == 0) {
        return;
    }

    // if we are just doing 2D rendering, no need to fill the depth buffer
    if (backEnd.viewDef->viewEntitys == NULL) {
        return;
    }

    renderLog.OpenMainBlock(MRB_FILL_DEPTH_BUFFER);
    renderLog.OpenBlock("RB_FillDepthBufferFast");

    GL_StartDepthPass(backEnd.viewDef->scissor);

    // force MVP change on first surface
    backEnd.currentSpace = NULL;

    // draw all the subview surfaces, which will already be at the start of the sorted list,
    // with the general purpose path
    GL_State(GLS_DEFAULT);

    int surfNum;
    for (surfNum = 0; surfNum < numDrawSurfs; surfNum++) {
        if (drawSurfs[surfNum]->material->GetSort() != SS_SUBVIEW) {
            break;
        }
        RB_FillDepthBufferGeneric(&drawSurfs[surfNum], 1);
    }

    const drawSurf_t** perforatedSurfaces = (const drawSurf_t**)_alloca(numDrawSurfs * sizeof(drawSurf_t*));
    int numPerforatedSurfaces = 0;

    // draw all the opaque surfaces and build up a list of perforated surfaces that
    // we will defer drawing until all opaque surfaces are done
    GL_State(GLS_DEFAULT);

    // continue checking past the subview surfaces
    for (; surfNum < numDrawSurfs; surfNum++) {
        const drawSurf_t* surf = drawSurfs[surfNum];
        const idMaterial* shader = surf->material;

        // translucent surfaces don't put anything in the depth buffer
        if (shader->Coverage() == MC_TRANSLUCENT) {
            continue;
        }
        if (shader->Coverage() == MC_PERFORATED) {
            // save for later drawing
            perforatedSurfaces[numPerforatedSurfaces] = surf;
            numPerforatedSurfaces++;
            continue;
        }

        // set polygon offset?

        // set mvp matrix
        if (surf->space != backEnd.currentSpace) {
            RB_SetMVP(surf->space->mvp);
            backEnd.currentSpace = surf->space;
        }

        renderLog.OpenBlock(shader->GetName());

        if (surf->jointCache) {
            renderProgManager.BindShader_DepthSkinned();
        } else {
            renderProgManager.BindShader_Depth();
        }

        // must render with less-equal for Z-Cull to work properly
        assert((GL_GetCurrentState() & GLS_DEPTHFUNC_BITS) == GLS_DEPTHFUNC_LESS);

        // draw it solid
        RB_DrawElementsWithCounters(surf);

        renderLog.CloseBlock();
    }

    // draw all perforated surfaces with the general code path
    if (numPerforatedSurfaces > 0) {
        RB_FillDepthBufferGeneric(perforatedSurfaces, numPerforatedSurfaces);
    }

    // Allow platform specific data to be collected after the depth pass.
    GL_FinishDepthPass();

    renderLog.CloseBlock();
    renderLog.CloseMainBlock();
}

/*
=========================================================================================

GENERAL INTERACTION RENDERING

=========================================================================================
*/

const int INTERACTION_TEXUNIT_BUMP = 0;
const int INTERACTION_TEXUNIT_FALLOFF = 1;
const int INTERACTION_TEXUNIT_PROJECTION = 2;
const int INTERACTION_TEXUNIT_DIFFUSE = 3;
const int INTERACTION_TEXUNIT_SPECULAR = 4;

/*
==================
RB_SetupInteractionStage
==================
*/
static void RB_SetupInteractionStage(const shaderStage_t* surfaceStage, const float* surfaceRegs, const float lightColor[4],
    idVec4 matrix[2], float color[4])
{

    if (surfaceStage->texture.hasMatrix) {
        matrix[0][0] = surfaceRegs[surfaceStage->texture.matrix[0][0]];
        matrix[0][1] = surfaceRegs[surfaceStage->texture.matrix[0][1]];
        matrix[0][2] = 0.0f;
        matrix[0][3] = surfaceRegs[surfaceStage->texture.matrix[0][2]];

        matrix[1][0] = surfaceRegs[surfaceStage->texture.matrix[1][0]];
        matrix[1][1] = surfaceRegs[surfaceStage->texture.matrix[1][1]];
        matrix[1][2] = 0.0f;
        matrix[1][3] = surfaceRegs[surfaceStage->texture.matrix[1][2]];

        // we attempt to keep scrolls from generating incredibly large texture values, but
        // center rotations and center scales can still generate offsets that need to be > 1
        if (matrix[0][3] < -40.0f || matrix[0][3] > 40.0f) {
            matrix[0][3] -= idMath::Ftoi(matrix[0][3]);
        }
        if (matrix[1][3] < -40.0f || matrix[1][3] > 40.0f) {
            matrix[1][3] -= idMath::Ftoi(matrix[1][3]);
        }
    } else {
        matrix[0][0] = 1.0f;
        matrix[0][1] = 0.0f;
        matrix[0][2] = 0.0f;
        matrix[0][3] = 0.0f;

        matrix[1][0] = 0.0f;
        matrix[1][1] = 1.0f;
        matrix[1][2] = 0.0f;
        matrix[1][3] = 0.0f;
    }

    if (color != NULL) {
        for (int i = 0; i < 4; i++) {
            // clamp here, so cards with a greater range don't look different.
            // we could perform overbrighting like we do for lights, but
            // it doesn't currently look worth it.
            color[i] = idMath::ClampFloat(0.0f, 1.0f, surfaceRegs[surfaceStage->color.registers[i]]) * lightColor[i];
        }
    }
}

/*
=================
RB_DrawSingleInteraction
=================
*/
static void RB_DrawSingleInteraction(drawInteraction_t* din)
{
    if (din->bumpImage == NULL) {
        // stage wasn't actually an interaction
        return;
    }

    if (din->diffuseImage == NULL || r_skipDiffuse.GetBool()) {
        // this isn't a YCoCg black, but it doesn't matter, because
        // the diffuseColor will also be 0
        din->diffuseImage = globalImages->blackImage;
    }
    if (din->specularImage == NULL || r_skipSpecular.GetBool() || din->ambientLight) {
        din->specularImage = globalImages->blackImage;
    }
    if (r_skipBump.GetBool()) {
        din->bumpImage = globalImages->flatNormalMap;
    }

    // if we wouldn't draw anything, don't call the Draw function
    const bool diffuseIsBlack = (din->diffuseImage == globalImages->blackImage)
        || ((din->diffuseColor[0] <= 0) && (din->diffuseColor[1] <= 0) && (din->diffuseColor[2] <= 0));
    const bool specularIsBlack = (din->specularImage == globalImages->blackImage)
        || ((din->specularColor[0] <= 0) && (din->specularColor[1] <= 0) && (din->specularColor[2] <= 0));
    if (diffuseIsBlack && specularIsBlack) {
        return;
    }

    // bump matrix
    SetVertexParm(RENDERPARM_BUMPMATRIX_S, din->bumpMatrix[0].ToFloatPtr());
    SetVertexParm(RENDERPARM_BUMPMATRIX_T, din->bumpMatrix[1].ToFloatPtr());

    // diffuse matrix
    SetVertexParm(RENDERPARM_DIFFUSEMATRIX_S, din->diffuseMatrix[0].ToFloatPtr());
    SetVertexParm(RENDERPARM_DIFFUSEMATRIX_T, din->diffuseMatrix[1].ToFloatPtr());

    // specular matrix
    SetVertexParm(RENDERPARM_SPECULARMATRIX_S, din->specularMatrix[0].ToFloatPtr());
    SetVertexParm(RENDERPARM_SPECULARMATRIX_T, din->specularMatrix[1].ToFloatPtr());

    RB_SetVertexColorParms(din->vertexColor);

    SetFragmentParm(RENDERPARM_DIFFUSEMODIFIER, din->diffuseColor.ToFloatPtr());
    SetFragmentParm(RENDERPARM_SPECULARMODIFIER, din->specularColor.ToFloatPtr());

    // texture 0 will be the per-surface bump map
    GL_SelectTexture(INTERACTION_TEXUNIT_BUMP);
    din->bumpImage->Bind();

    // texture 3 is the per-surface diffuse map
    GL_SelectTexture(INTERACTION_TEXUNIT_DIFFUSE);
    din->diffuseImage->Bind();

    // texture 4 is the per-surface specular map
    GL_SelectTexture(INTERACTION_TEXUNIT_SPECULAR);
    din->specularImage->Bind();

    RB_DrawElementsWithCounters(din->surf);
}

/*
=================
RB_SetupForFastPathInteractions

These are common for all fast path surfaces
=================
*/
static void RB_SetupForFastPathInteractions(const idVec4& diffuseColor, const idVec4& specularColor)
{
    const idVec4 sMatrix(1, 0, 0, 0);
    const idVec4 tMatrix(0, 1, 0, 0);

    // bump matrix
    SetVertexParm(RENDERPARM_BUMPMATRIX_S, sMatrix.ToFloatPtr());
    SetVertexParm(RENDERPARM_BUMPMATRIX_T, tMatrix.ToFloatPtr());

    // diffuse matrix
    SetVertexParm(RENDERPARM_DIFFUSEMATRIX_S, sMatrix.ToFloatPtr());
    SetVertexParm(RENDERPARM_DIFFUSEMATRIX_T, tMatrix.ToFloatPtr());

    // specular matrix
    SetVertexParm(RENDERPARM_SPECULARMATRIX_S, sMatrix.ToFloatPtr());
    SetVertexParm(RENDERPARM_SPECULARMATRIX_T, tMatrix.ToFloatPtr());

    RB_SetVertexColorParms(SVC_IGNORE);

    SetFragmentParm(RENDERPARM_DIFFUSEMODIFIER, diffuseColor.ToFloatPtr());
    SetFragmentParm(RENDERPARM_SPECULARMODIFIER, specularColor.ToFloatPtr());
}

/*
=============
RB_RenderInteractions

With added sorting and trivial path work.
=============
*/
static void RB_RenderInteractions(const drawSurf_t* surfList, const viewLight_t* vLight, int depthFunc, bool performStencilTest, bool useLightDepthBounds)
{
    VITA_RB_PASS(2);
    if (surfList == NULL) {
        return;
    }

    // change the scissor if needed, it will be constant across all the surfaces lit by the light
    if (!backEnd.currentScissor.Equals(vLight->scissorRect) && r_useScissor.GetBool()) {
        GL_Scissor(backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1,
            backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
            vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
            vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1);
        backEnd.currentScissor = vLight->scissorRect;
    }

    // perform setup here that will be constant for all interactions
    if (performStencilTest) {
        GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_EQUAL | GLS_STENCIL_MAKE_REF(STENCIL_SHADOW_TEST_VALUE) | GLS_STENCIL_MAKE_MASK(STENCIL_SHADOW_MASK_VALUE));

    } else {
        GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | depthFunc | GLS_STENCIL_FUNC_ALWAYS);
    }

    // some rare lights have multiple animating stages, loop over them outside the surface list
    const idMaterial* lightShader = vLight->lightShader;
    const float* lightRegs = vLight->shaderRegisters;

    drawInteraction_t inter = {};
    inter.ambientLight = lightShader->IsAmbientLight();

    //---------------------------------
    // Split out the complex surfaces from the fast-path surfaces
    // so we can do the fast path ones all in a row.
    // The surfaces should already be sorted by space because they
    // are added single-threaded, and there is only a negligable amount
    // of benefit to trying to sort by materials.
    //---------------------------------
    static const int MAX_INTERACTIONS_PER_LIGHT = 1024;
    static const int MAX_COMPLEX_INTERACTIONS_PER_LIGHT = 256;
    idStaticList<const drawSurf_t*, MAX_INTERACTIONS_PER_LIGHT> allSurfaces;
    idStaticList<const drawSurf_t*, MAX_COMPLEX_INTERACTIONS_PER_LIGHT> complexSurfaces;
    for (const drawSurf_t* walk = surfList; walk != NULL; walk = walk->nextOnLight) {

        // make sure the triangle culling is done
        if (walk->shadowVolumeState != SHADOWVOLUME_DONE) {
            assert(walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED || walk->shadowVolumeState == SHADOWVOLUME_DONE);

            uint64 start = Sys_Microseconds();
            while (walk->shadowVolumeState == SHADOWVOLUME_UNFINISHED) {
                Sys_Yield();
            }
            uint64 end = Sys_Microseconds();

            backEnd.pc.shadowMicroSec += end - start;
        }

        const idMaterial* surfaceShader = walk->material;
        if (surfaceShader->GetFastPathBumpImage()) {
            allSurfaces.Append(walk);
        } else {
            complexSurfaces.Append(walk);
        }
    }
    for (int i = 0; i < complexSurfaces.Num(); i++) {
        allSurfaces.Append(complexSurfaces[i]);
    }

    bool lightDepthBoundsDisabled = false;

    for (int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++) {
        const shaderStage_t* lightStage = lightShader->GetStage(lightStageNum);

        // ignore stages that fail the condition
        if (!lightRegs[lightStage->conditionRegister]) {
            continue;
        }

        const float lightScale = r_lightScale.GetFloat();
        const idVec4 lightColor(
            lightScale * lightRegs[lightStage->color.registers[0]],
            lightScale * lightRegs[lightStage->color.registers[1]],
            lightScale * lightRegs[lightStage->color.registers[2]],
            lightRegs[lightStage->color.registers[3]]);
        // apply the world-global overbright and the 2x factor for specular
        const idVec4 diffuseColor = lightColor;
        const idVec4 specularColor = lightColor * 2.0f;

        float lightTextureMatrix[16];
        if (lightStage->texture.hasMatrix) {
            RB_GetShaderTextureMatrix(lightRegs, &lightStage->texture, lightTextureMatrix);
        }

        // texture 1 will be the light falloff texture
        GL_SelectTexture(INTERACTION_TEXUNIT_FALLOFF);
#if defined(__vita__)
        // Diagnostic: substitute a solid white texture for the light falloff.
        // A shader probe measured lightFalloff as (0,0,0,0) with valid
        // texcoords, which zeroes lightColor and silently disables every
        // per-light interaction - the whole scene has been lit by the ambient
        // pass alone. With white here the lights should come back, without
        // distance attenuation. If they do, the falloff image is the disease
        // and everything downstream of it is healthy.
        if (vita_forceWhiteFalloff.GetBool()) {
            globalImages->whiteImage->Bind();
        } else {
        // Report each distinct falloff image once. A shader probe showed this
        // texture sampling as (0,0,0,0) with valid texcoords, which zeroes
        // lightColor and silently disables the entire per-light pass, so it
        // matters exactly which image and format is landing here.
        // Once per distinct image: logging every change between consecutive
        // lights wrote two lines per frame as soon as the flashlight (its own
        // falloff) was on, and each line is an open/write/close on the card -
        // that alone halved the frame rate.
        {
            static const idImage* seenFalloff[64];
            static int numSeenFalloff = 0;
            bool seen = false;
            for (int k = 0; k < numSeenFalloff; k++) {
                if (seenFalloff[k] == vLight->falloffImage) {
                    seen = true;
                    break;
                }
            }
            if (!seen && numSeenFalloff < 64) {
                seenFalloff[numSeenFalloff++] = vLight->falloffImage;
                const idImageOpts& fo = vLight->falloffImage->GetOpts();
                Vita_Checkpoint("falloff '%s' fmt %d cfm %d %dx%d levels %d loaded %d",
                    vLight->falloffImage->GetName(), (int)fo.format, (int)fo.colorFormat,
                    fo.width, fo.height, fo.numLevels,
                    vLight->falloffImage->IsLoaded() ? 1 : 0);
            }
        }
        vLight->falloffImage->Bind();
        }
#else
        vLight->falloffImage->Bind();
#endif

        // texture 2 will be the light projection texture
        GL_SelectTexture(INTERACTION_TEXUNIT_PROJECTION);
        lightStage->texture.image->Bind();
#if defined(__vita__)
        // Report each distinct light projection image the first time a light
        // uses it. These are the RGB565 images that used to reach the GPU
        // byte-swapped; the matching "rgb565 '...'" upload line shows whether
        // this one was converted.
        {
            static const idImage* seenProj[96];
            static int numSeenProj = 0;
            const idImage* pi = lightStage->texture.image;
            bool seen = false;
            for (int k = 0; k < numSeenProj; k++) {
                if (seenProj[k] == pi) {
                    seen = true;
                    break;
                }
            }
            if (!seen && numSeenProj < 96) {
                seenProj[numSeenProj++] = pi;
                const idImageOpts& po = pi->GetOpts();
                Vita_Checkpoint("light projection #%d '%s' fmt %d cfm %d %dx%d levels %d%s, on light shader '%s'%s",
                    numSeenProj, pi->GetName(), (int)po.format, (int)po.colorFormat, po.width, po.height, po.numLevels,
                    po.format == FMT_RGB565 ? " (RGB565: byte-swapped at upload)" : "",
                    lightShader->GetName(), lightShader->IsAmbientLight() ? " [ambient]" : "");
            }
            Vita_CensusLight(lightShader->IsAmbientLight(), allSurfaces.Num(),
                vLight->scissorRect.IsEmpty() ? 0 : (vLight->scissorRect.x2 - vLight->scissorRect.x1 + 1) * (vLight->scissorRect.y2 - vLight->scissorRect.y1 + 1),
                lightShader->GetName());
        }
#endif

        // force the light textures to not use anisotropic filtering, which is wasted on them
        // all of the texture sampler parms should be constant for all interactions, only
        // the actual texture image bindings will change

        //----------------------------------
        // For all surfaces on this light list, generate an interaction for this light stage
        //----------------------------------

        // setup renderparms assuming we will be drawing trivial surfaces first
        RB_SetupForFastPathInteractions(diffuseColor, specularColor);

        // even if the space does not change between light stages, each light stage may need a different lightTextureMatrix baked in
        backEnd.currentSpace = NULL;

        for (int sortedSurfNum = 0; sortedSurfNum < allSurfaces.Num(); sortedSurfNum++) {
            const drawSurf_t* const surf = allSurfaces[sortedSurfNum];

            // select the render prog
            if (lightShader->IsAmbientLight()) {
                if (surf->jointCache) {
                    renderProgManager.BindShader_InteractionAmbientSkinned();
                } else {
                    renderProgManager.BindShader_InteractionAmbient();
                }
            } else {
                if (surf->jointCache) {
                    renderProgManager.BindShader_InteractionSkinned();
                } else {
                    renderProgManager.BindShader_Interaction();
                }
            }

            const idMaterial* surfaceShader = surf->material;
            const float* surfaceRegs = surf->shaderRegisters;

            inter.surf = surf;

            // change the MVP matrix, view/light origin and light projection vectors if needed
            if (surf->space != backEnd.currentSpace) {
                backEnd.currentSpace = surf->space;

                // turn off the light depth bounds test if this model is rendered with a depth hack
                if (useLightDepthBounds) {
                    if (!surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f) {
                        if (lightDepthBoundsDisabled) {
                            GL_DepthBoundsTest(vLight->scissorRect.zmin, vLight->scissorRect.zmax);
                            lightDepthBoundsDisabled = false;
                        }
                    } else {
                        if (!lightDepthBoundsDisabled) {
                            GL_DepthBoundsTest(0.0f, 0.0f);
                            lightDepthBoundsDisabled = true;
                        }
                    }
                }

                // model-view-projection
                RB_SetMVP(surf->space->mvp);

                // tranform the light/view origin into model local space
                idVec4 localLightOrigin(0.0f);
                idVec4 localViewOrigin(1.0f);
                R_GlobalPointToLocal(surf->space->modelMatrix, vLight->globalLightOrigin, localLightOrigin.ToVec3());
                R_GlobalPointToLocal(surf->space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3());

                // set the local light/view origin
                SetVertexParm(RENDERPARM_LOCALLIGHTORIGIN, localLightOrigin.ToFloatPtr());
                SetVertexParm(RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr());

                // transform the light project into model local space
                idPlane lightProjection[4];
                for (int i = 0; i < 4; i++) {
                    R_GlobalPlaneToLocal(surf->space->modelMatrix, vLight->lightProject[i], lightProjection[i]);
                }

                // optionally multiply the local light projection by the light texture matrix
                if (lightStage->texture.hasMatrix) {
                    RB_BakeTextureMatrixIntoTexgen(lightProjection, lightTextureMatrix);
                }

                // set the light projection
                SetVertexParm(RENDERPARM_LIGHTPROJECTION_S, lightProjection[0].ToFloatPtr());
                SetVertexParm(RENDERPARM_LIGHTPROJECTION_T, lightProjection[1].ToFloatPtr());
                SetVertexParm(RENDERPARM_LIGHTPROJECTION_Q, lightProjection[2].ToFloatPtr());
                SetVertexParm(RENDERPARM_LIGHTFALLOFF_S, lightProjection[3].ToFloatPtr());
            }

            // check for the fast path
            if (surfaceShader->GetFastPathBumpImage() && !r_skipInteractionFastPath.GetBool()) {
                renderLog.OpenBlock(surf->material->GetName());

                // texture 0 will be the per-surface bump map
                GL_SelectTexture(INTERACTION_TEXUNIT_BUMP);
                surfaceShader->GetFastPathBumpImage()->Bind();

                // texture 3 is the per-surface diffuse map
                GL_SelectTexture(INTERACTION_TEXUNIT_DIFFUSE);
                surfaceShader->GetFastPathDiffuseImage()->Bind();

                // texture 4 is the per-surface specular map
                GL_SelectTexture(INTERACTION_TEXUNIT_SPECULAR);
                surfaceShader->GetFastPathSpecularImage()->Bind();

                RB_DrawElementsWithCounters(surf);

                renderLog.CloseBlock();
                continue;
            }

            renderLog.OpenBlock(surf->material->GetName());

            inter.bumpImage = NULL;
            inter.specularImage = NULL;
            inter.diffuseImage = NULL;
            inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
            inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

            // go through the individual surface stages
            //
            // This is somewhat arcane because of the old support for video cards that had to render
            // interactions in multiple passes.
            //
            // We also have the very rare case of some materials that have conditional interactions
            // for the "hell writing" that can be shined on them.
            for (int surfaceStageNum = 0; surfaceStageNum < surfaceShader->GetNumStages(); surfaceStageNum++) {
                const shaderStage_t* surfaceStage = surfaceShader->GetStage(surfaceStageNum);

                switch (surfaceStage->lighting) {
                case SL_COVERAGE: {
                    // ignore any coverage stages since they should only be used for the depth fill pass
                    // for diffuse stages that use alpha test.
                    break;
                }
                case SL_AMBIENT: {
                    // ignore ambient stages while drawing interactions
                    break;
                }
                case SL_BUMP: {
                    // ignore stage that fails the condition
                    if (!surfaceRegs[surfaceStage->conditionRegister]) {
                        break;
                    }
                    // draw any previous interaction
                    if (inter.bumpImage != NULL) {
                        RB_DrawSingleInteraction(&inter);
                    }
                    inter.bumpImage = surfaceStage->texture.image;
                    inter.diffuseImage = NULL;
                    inter.specularImage = NULL;
                    RB_SetupInteractionStage(surfaceStage, surfaceRegs, NULL,
                        inter.bumpMatrix, NULL);
                    break;
                }
                case SL_DIFFUSE: {
                    // ignore stage that fails the condition
                    if (!surfaceRegs[surfaceStage->conditionRegister]) {
                        break;
                    }
                    // draw any previous interaction
                    if (inter.diffuseImage != NULL) {
                        RB_DrawSingleInteraction(&inter);
                    }
                    inter.diffuseImage = surfaceStage->texture.image;
                    inter.vertexColor = surfaceStage->vertexColor;
                    RB_SetupInteractionStage(surfaceStage, surfaceRegs, diffuseColor.ToFloatPtr(),
                        inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr());
                    break;
                }
                case SL_SPECULAR: {
                    // ignore stage that fails the condition
                    if (!surfaceRegs[surfaceStage->conditionRegister]) {
                        break;
                    }
                    // draw any previous interaction
                    if (inter.specularImage != NULL) {
                        RB_DrawSingleInteraction(&inter);
                    }
                    inter.specularImage = surfaceStage->texture.image;
                    inter.vertexColor = surfaceStage->vertexColor;
                    RB_SetupInteractionStage(surfaceStage, surfaceRegs, specularColor.ToFloatPtr(),
                        inter.specularMatrix, inter.specularColor.ToFloatPtr());
                    break;
                }
                }
            }

            // draw the final interaction
            RB_DrawSingleInteraction(&inter);

            renderLog.CloseBlock();
        }
    }

    if (useLightDepthBounds && lightDepthBoundsDisabled) {
        GL_DepthBoundsTest(vLight->scissorRect.zmin, vLight->scissorRect.zmax);
    }

    renderProgManager.Unbind();
}

/*
==============================================================================================

STENCIL SHADOW RENDERING

==============================================================================================
*/

/*
=====================
RB_StencilShadowPass

The stencil buffer should have been set to 128 on any surfaces that might receive shadows.
=====================
*/
/*
=====================
Two-sided stencil fallback

The stencil shadow passes drive the front and back faces of a volume with
different stencil ops inside a single two-sided draw. A driver without separate
stencil (pre-GL 2.0, no GL_ATI_separate_stencil) cannot express that, but the
same result comes out of drawing the volume twice with single-sided ops, once
per face orientation, at the cost of a second draw call per surface.

Note that only the *ops* differ between faces here - the stencil func and mask
come from GL_State() and are the same on both sides - so the func never needs
splitting.

RB_ShadowStencilOp() records the requested pair and applies it directly when
separate stencil is present. RB_DrawShadowVolume() then issues either the one
two-sided draw or the two culled draws.
=====================
*/
struct shadowStencilOp_t {
    GLenum fail;
    GLenum zfail;
    GLenum zpass;
};
static shadowStencilOp_t rb_shadowStencilFront = { GL_KEEP, GL_KEEP, GL_INCR };
static shadowStencilOp_t rb_shadowStencilBack = { GL_KEEP, GL_KEEP, GL_DECR };

static void RB_ShadowStencilOp(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
    shadowStencilOp_t& slot = (face == GL_FRONT) ? rb_shadowStencilFront : rb_shadowStencilBack;
    slot.fail = fail;
    slot.zfail = zfail;
    slot.zpass = zpass;

    if (glConfig.twoSidedStencilAvailable) {
        qglStencilOpSeparate(face, fail, zfail, zpass);
    }
}

// The caller has already folded baseVertex into the attribute offsets on Vita,
// because vitaGL accepts glDrawElementsBaseVertex() but never applies the base.
// Passing it again here would double-count it, so every shadow draw goes through
// one place that knows which of the two entry points is correct.
static ID_INLINE void RB_ShadowDrawElements(GLsizei numIndexes, const void* indexOffset, GLint baseVertex)
{
#if defined(__vita__)
    (void)baseVertex;
    qglDrawElements(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, (triIndex_t*)indexOffset);
#else
    qglDrawElementsBaseVertex(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, (triIndex_t*)indexOffset, baseVertex);
#endif
}

static void RB_DrawShadowVolume(GLsizei numIndexes, const void* indexOffset, GLint baseVertex)
{
    if (glConfig.twoSidedStencilAvailable) {
        RB_ShadowDrawElements(numIndexes, indexOffset, baseVertex);
        return;
    }

    // CT_BACK_SIDED culls back faces, leaving the front ones; CT_FRONT_SIDED is
    // the reverse. See GL_Cull().
    GL_Cull(CT_BACK_SIDED);
    qglStencilOp(rb_shadowStencilFront.fail, rb_shadowStencilFront.zfail, rb_shadowStencilFront.zpass);
    RB_ShadowDrawElements(numIndexes, indexOffset, baseVertex);

    GL_Cull(CT_FRONT_SIDED);
    qglStencilOp(rb_shadowStencilBack.fail, rb_shadowStencilBack.zfail, rb_shadowStencilBack.zpass);
    RB_ShadowDrawElements(numIndexes, indexOffset, baseVertex);

    // The caller set CT_TWO_SIDED for the whole pass; put it back so the next
    // surface starts from the state it expects.
    GL_Cull(CT_TWO_SIDED);

    backEnd.pc.c_shadowElements++;
    backEnd.pc.c_shadowIndexes += numIndexes;
}

static void RB_StencilShadowPass(const drawSurf_t* drawSurfs, const viewLight_t* vLight)
{
    VITA_RB_PASS(1);
    if (r_skipShadows.GetBool()) {
        return;
    }

    if (drawSurfs == NULL) {
        return;
    }

    RENDERLOG_PRINTF("---------- RB_StencilShadowPass ----------\n");

    renderProgManager.BindShader_Shadow();

    GL_SelectTexture(0);
    globalImages->BindNull();

    uint64 glState = 0;

    // for visualizing the shadows
    if (r_showShadows.GetInteger()) {
        // set the debug shadow color
        SetFragmentParm(RENDERPARM_COLOR, colorMagenta.ToFloatPtr());
        if (r_showShadows.GetInteger() == 2) {
            // draw filled in
            glState = GLS_DEPTHMASK | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_LESS;
        } else {
            // draw as lines, filling the depth buffer
            glState = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_POLYMODE_LINE | GLS_DEPTHFUNC_ALWAYS;
        }
    } else {
        // don't write to the color or depth buffer, just the stencil buffer
        glState = GLS_DEPTHMASK | GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHFUNC_LESS;
    }

    GL_PolygonOffset(r_shadowPolygonFactor.GetFloat(), -r_shadowPolygonOffset.GetFloat());

    // the actual stencil func will be set in the draw code, but we need to make sure it isn't
    // disabled here, and that the value will get reset for the interactions without looking
    // like a no-change-required
    GL_State(glState | GLS_STENCIL_OP_FAIL_KEEP | GLS_STENCIL_OP_ZFAIL_KEEP | GLS_STENCIL_OP_PASS_INCR | GLS_STENCIL_MAKE_REF(STENCIL_SHADOW_TEST_VALUE) | GLS_STENCIL_MAKE_MASK(STENCIL_SHADOW_MASK_VALUE) | GLS_POLYGON_OFFSET);

    // Two Sided Stencil reduces two draw calls to one for slightly faster shadows
    GL_Cull(CT_TWO_SIDED);

    // process the chain of shadows with the current rendering state
    backEnd.currentSpace = NULL;

    for (const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight) {
        if (drawSurf->scissorRect.IsEmpty()) {
            continue; // !@# FIXME: find out why this is sometimes being hit!
                      // temporarily jump over the scissor and draw so the gl error callback doesn't get hit
        }

        // make sure the shadow volume is done
        if (drawSurf->shadowVolumeState != SHADOWVOLUME_DONE) {
            assert(drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED || drawSurf->shadowVolumeState == SHADOWVOLUME_DONE);

            uint64 start = Sys_Microseconds();
            while (drawSurf->shadowVolumeState == SHADOWVOLUME_UNFINISHED) {
                Sys_Yield();
            }
            uint64 end = Sys_Microseconds();

            backEnd.pc.shadowMicroSec += end - start;
        }

        if (drawSurf->numIndexes == 0) {
            continue; // a job may have created an empty shadow volume
        }

        if (!backEnd.currentScissor.Equals(drawSurf->scissorRect) && r_useScissor.GetBool()) {
            // change the scissor
            GL_Scissor(backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
                backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
                drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
                drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1);
            backEnd.currentScissor = drawSurf->scissorRect;
        }

        if (drawSurf->space != backEnd.currentSpace) {
            // change the matrix
            RB_SetMVP(drawSurf->space->mvp);

            // set the local light position to allow the vertex program to project the shadow volume end cap to infinity
            idVec4 localLight(0.0f);
            R_GlobalPointToLocal(drawSurf->space->modelMatrix, vLight->globalLightOrigin, localLight.ToVec3());
            SetVertexParm(RENDERPARM_LOCALLIGHTORIGIN, localLight.ToFloatPtr());

            backEnd.currentSpace = drawSurf->space;
        }

        if (r_showShadows.GetInteger() == 0) {
            if (drawSurf->jointCache) {
                renderProgManager.BindShader_ShadowSkinned();
            } else {
                renderProgManager.BindShader_Shadow();
            }
        } else {
            if (drawSurf->jointCache) {
                renderProgManager.BindShader_ShadowDebugSkinned();
            } else {
                renderProgManager.BindShader_ShadowDebug();
            }
        }

        // set depth bounds per shadow
        if (r_useShadowDepthBounds.GetBool()) {
            GL_DepthBoundsTest(drawSurf->scissorRect.zmin, drawSurf->scissorRect.zmax);
        }

        // Determine whether or not the shadow volume needs to be rendered with Z-pass or
        // Z-fail. It is worthwhile to spend significant resources to reduce the number of
        // cases where shadow volumes need to be rendered with Z-fail because Z-fail
        // rendering can be significantly slower even on today's hardware. For instance,
        // on NVIDIA hardware Z-fail rendering causes the Z-Cull to be used in reverse:
        // Z-near becomes Z-far (trivial accept becomes trivial reject). Using the Z-Cull
        // in reverse is far less efficient because the Z-Cull only stores Z-near per 16x16
        // pixels while the Z-far is stored per 4x2 pixels. (The Z-near coallesce buffer
        // which has 4x4 granularity is only used when updating the depth which is not the
        // case for shadow volumes.) Note that it is also important to NOT use a Z-Cull
        // reconstruct because that would clear the Z-near of the Z-Cull which results in
        // no trivial rejection for Z-fail stencil shadow rendering.

        const bool renderZPass = (drawSurf->renderZFail == 0) || r_forceZPassStencilShadows.GetBool();

        if (renderZPass) {
            // Z-pass
            RB_ShadowStencilOp(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
            RB_ShadowStencilOp(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
        } else if (r_useStencilShadowPreload.GetBool()) {
            // preload + Z-pass
            RB_ShadowStencilOp(GL_FRONT, GL_KEEP, GL_DECR, GL_DECR);
            RB_ShadowStencilOp(GL_BACK, GL_KEEP, GL_INCR, GL_INCR);
        } else {
            // Z-fail: the preload and Z-pass ops above summed into one draw -
            // only the hidden fragments count, front faces down, back faces up.
            // (Stock code set nothing here, so a Z-fail volume reused whatever
            // ops the previous volume left: Z-pass ones after a Z-pass volume.)
            RB_ShadowStencilOp(GL_FRONT, GL_KEEP, GL_DECR, GL_KEEP);
            RB_ShadowStencilOp(GL_BACK, GL_KEEP, GL_INCR, GL_KEEP);
        }

        // get vertex buffer
        const vertCacheHandle_t vbHandle = drawSurf->shadowCache;
        idVertexBuffer* vertexBuffer;
        if (vertexCache.CacheIsStatic(vbHandle)) {
            vertexBuffer = &vertexCache.staticData.vertexBuffer;
        } else {
            const uint64 frameNum = (int)(vbHandle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
            if (frameNum != ((vertexCache.currentFrame - 1) & VERTCACHE_FRAME_MASK)) {
                idLib::Warning("RB_DrawElementsWithCounters, vertexBuffer == NULL");
                continue;
            }
            vertexBuffer = &vertexCache.frameData[vertexCache.drawListNum].vertexBuffer;
        }
        const int vertOffset = (int)(vbHandle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;

        // get index buffer
        const vertCacheHandle_t ibHandle = drawSurf->indexCache;
        idIndexBuffer* indexBuffer;
        if (vertexCache.CacheIsStatic(ibHandle)) {
            indexBuffer = &vertexCache.staticData.indexBuffer;
        } else {
            const uint64 frameNum = (int)(ibHandle >> VERTCACHE_FRAME_SHIFT) & VERTCACHE_FRAME_MASK;
            if (frameNum != ((vertexCache.currentFrame - 1) & VERTCACHE_FRAME_MASK)) {
                idLib::Warning("RB_DrawElementsWithCounters, indexBuffer == NULL");
                continue;
            }
            indexBuffer = &vertexCache.frameData[vertexCache.drawListNum].indexBuffer;
        }
        const uint64 indexOffset = (int)(ibHandle >> VERTCACHE_OFFSET_SHIFT) & VERTCACHE_OFFSET_MASK;

        RENDERLOG_PRINTF("Binding Buffers: %p %p\n", vertexBuffer, indexBuffer);

        // RB: 64 bit fixes, changed GLuint to GLintptrARB
        if (backEnd.glState.currentIndexBuffer != (GLintptrARB)indexBuffer->GetAPIObject() || !r_useStateCaching.GetBool()) {
            qglBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, (GLintptrARB)indexBuffer->GetAPIObject());
            backEnd.glState.currentIndexBuffer = (GLintptrARB)indexBuffer->GetAPIObject();
        }

        // Same baseVertex problem as RB_DrawElementsWithCounters - vitaGL
        // accepts glDrawElementsBaseVertex() but never applies the base, so
        // every shadow volume but the first would read another surface's
        // vertices. Fold it into the attribute offsets and key the layout
        // cache on it, exactly as that function does.
#if defined(__vita__)
        const uintptr_t shadowVertBase = (uintptr_t)vertOffset;
#else
        const uintptr_t shadowVertBase = 0;
#endif

        if (drawSurf->jointCache) {
            assert(renderProgManager.ShaderUsesJoints());

            idJointBuffer jointBuffer;
            if (!vertexCache.GetJointBuffer(drawSurf->jointCache, &jointBuffer)) {
                idLib::Warning("RB_DrawElementsWithCounters, jointBuffer == NULL");
                continue;
            }
            assert((jointBuffer.GetOffset() & (glConfig.uniformBufferOffsetAlignment - 1)) == 0);

            const GLintptrARB ubo = reinterpret_cast<GLintptrARB>(jointBuffer.GetAPIObject());
            qglBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, jointBuffer.GetOffset(), jointBuffer.GetNumJoints() * sizeof(idJointMat));

            if ((backEnd.glState.vertexLayout != LAYOUT_DRAW_SHADOW_VERT_SKINNED) || (backEnd.glState.currentVertexBuffer != (GLintptrARB)vertexBuffer->GetAPIObject())
#if defined(__vita__)
                || (backEnd.glState.currentVertOffset != shadowVertBase)
#endif
                || !r_useStateCaching.GetBool()) {
                qglBindBufferARB(GL_ARRAY_BUFFER_ARB, (GLintptrARB)vertexBuffer->GetAPIObject());
                backEnd.glState.currentVertexBuffer = (GLintptrARB)vertexBuffer->GetAPIObject();
#if defined(__vita__)
                backEnd.glState.currentVertOffset = shadowVertBase;
#endif

                qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_VERTEX);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_NORMAL);
                qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR);
                qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR2);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_ST);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_TANGENT);

                qglVertexAttribPointerARB(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof(idShadowVertSkinned), (void*)(shadowVertBase + SHADOWVERTSKINNED_XYZW_OFFSET));
                qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idShadowVertSkinned), (void*)(shadowVertBase + SHADOWVERTSKINNED_COLOR_OFFSET));
                qglVertexAttribPointerARB(PC_ATTRIB_INDEX_COLOR2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(idShadowVertSkinned), (void*)(shadowVertBase + SHADOWVERTSKINNED_COLOR2_OFFSET));

                backEnd.glState.vertexLayout = LAYOUT_DRAW_SHADOW_VERT_SKINNED;
            }

        } else {

            if ((backEnd.glState.vertexLayout != LAYOUT_DRAW_SHADOW_VERT) || (backEnd.glState.currentVertexBuffer != (GLintptrARB)vertexBuffer->GetAPIObject())
#if defined(__vita__)
                || (backEnd.glState.currentVertOffset != shadowVertBase)
#endif
                || !r_useStateCaching.GetBool()) {
                qglBindBufferARB(GL_ARRAY_BUFFER_ARB, (GLintptrARB)vertexBuffer->GetAPIObject());
                backEnd.glState.currentVertexBuffer = (GLintptrARB)vertexBuffer->GetAPIObject();
#if defined(__vita__)
                backEnd.glState.currentVertOffset = shadowVertBase;
#endif

                qglEnableVertexAttribArrayARB(PC_ATTRIB_INDEX_VERTEX);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_NORMAL);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_COLOR2);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_ST);
                qglDisableVertexAttribArrayARB(PC_ATTRIB_INDEX_TANGENT);

                qglVertexAttribPointerARB(PC_ATTRIB_INDEX_VERTEX, 4, GL_FLOAT, GL_FALSE, sizeof(idShadowVert), (void*)(shadowVertBase + SHADOWVERT_XYZW_OFFSET));

                backEnd.glState.vertexLayout = LAYOUT_DRAW_SHADOW_VERT;
            }
        }
        // RB end

        renderProgManager.CommitUniforms();

        if (drawSurf->jointCache) {
            RB_DrawShadowVolume(r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, (const void*)indexOffset, vertOffset / sizeof(idShadowVertSkinned));
        } else {
            RB_DrawShadowVolume(r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, (const void*)indexOffset, vertOffset / sizeof(idShadowVert));
        }

        // RB: added stats
        backEnd.pc.c_shadowElements++;
        backEnd.pc.c_shadowIndexes += drawSurf->numIndexes;
        // RB end

        if (!renderZPass && r_useStencilShadowPreload.GetBool()) {
            // render again with Z-pass
            RB_ShadowStencilOp(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
            RB_ShadowStencilOp(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);

            if (drawSurf->jointCache) {
                RB_DrawShadowVolume(r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, (const void*)indexOffset, vertOffset / sizeof(idShadowVertSkinned));
            } else {
                RB_DrawShadowVolume(r_singleTriangle.GetBool() ? 3 : drawSurf->numIndexes, (const void*)indexOffset, vertOffset / sizeof(idShadowVert));
            }

            // RB: added stats
            backEnd.pc.c_shadowElements++;
            backEnd.pc.c_shadowIndexes += drawSurf->numIndexes;
            // RB end
        }
    }

    // cleanup the shadow specific rendering state

    GL_Cull(CT_FRONT_SIDED);

    // reset depth bounds
    if (r_useShadowDepthBounds.GetBool()) {
        if (r_useLightDepthBounds.GetBool()) {
            GL_DepthBoundsTest(vLight->scissorRect.zmin, vLight->scissorRect.zmax);
        } else {
            GL_DepthBoundsTest(0.0f, 0.0f);
        }
    }
}

/*
==================
RB_StencilSelectLight

Deform the zeroOneCubeModel to exactly cover the light volume. Render the deformed cube model to the stencil buffer in
such a way that only fragments that are directly visible and contained within the volume will be written creating a
mask to be used by the following stencil shadow and draw interaction passes.
==================
*/
static void RB_StencilSelectLight(const viewLight_t* vLight)
{
    renderLog.OpenBlock("Stencil Select");

    // enable the light scissor
    if (!backEnd.currentScissor.Equals(vLight->scissorRect) && r_useScissor.GetBool()) {
        GL_Scissor(backEnd.viewDef->viewport.x1 + vLight->scissorRect.x1,
            backEnd.viewDef->viewport.y1 + vLight->scissorRect.y1,
            vLight->scissorRect.x2 + 1 - vLight->scissorRect.x1,
            vLight->scissorRect.y2 + 1 - vLight->scissorRect.y1);
        backEnd.currentScissor = vLight->scissorRect;
    }

    // clear stencil buffer to 0 (not drawable)
    uint64 glStateMinusStencil = GL_GetCurrentStateMinusStencil();
    GL_State(glStateMinusStencil | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF(STENCIL_SHADOW_TEST_VALUE) | GLS_STENCIL_MAKE_MASK(STENCIL_SHADOW_MASK_VALUE)); // make sure stencil mask passes for the clear
    GL_Clear(false, false, true, 0, 0.0f, 0.0f, 0.0f, 0.0f);                                                                                                      // clear to 0 for stencil select

    // set the depthbounds
    GL_DepthBoundsTest(vLight->scissorRect.zmin, vLight->scissorRect.zmax);

    GL_State(GLS_COLORMASK | GLS_ALPHAMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_LESS | GLS_STENCIL_FUNC_ALWAYS | GLS_STENCIL_MAKE_REF(STENCIL_SHADOW_TEST_VALUE) | GLS_STENCIL_MAKE_MASK(STENCIL_SHADOW_MASK_VALUE));
    GL_Cull(CT_TWO_SIDED);

    renderProgManager.BindShader_Depth();

    // set the matrix for deforming the 'zeroOneCubeModel' into the frustum to exactly cover the light volume
    idRenderMatrix invProjectMVPMatrix;
    idRenderMatrix::Multiply(backEnd.viewDef->worldSpace.mvp, vLight->inverseBaseLightProject, invProjectMVPMatrix);
    RB_SetMVP(invProjectMVPMatrix);

    // two-sided stencil test
    if (glConfig.twoSidedStencilAvailable) {
        qglStencilOpSeparate(GL_FRONT, GL_KEEP, GL_REPLACE, GL_ZERO);
        qglStencilOpSeparate(GL_BACK, GL_KEEP, GL_ZERO, GL_REPLACE);

        RB_DrawElementsWithCounters(&backEnd.zeroOneCubeSurface);
    } else {
        // Same mask, built from two culled passes. See RB_DrawShadowVolume().
        GL_Cull(CT_BACK_SIDED);
        qglStencilOp(GL_KEEP, GL_REPLACE, GL_ZERO);
        RB_DrawElementsWithCounters(&backEnd.zeroOneCubeSurface);

        GL_Cull(CT_FRONT_SIDED);
        qglStencilOp(GL_KEEP, GL_ZERO, GL_REPLACE);
        RB_DrawElementsWithCounters(&backEnd.zeroOneCubeSurface);
    }

    // reset stencil state

    GL_Cull(CT_FRONT_SIDED);

    renderProgManager.Unbind();

    // unset the depthbounds
    GL_DepthBoundsTest(0.0f, 0.0f);

    renderLog.CloseBlock();
}

/*
==============================================================================================

DRAW INTERACTIONS

==============================================================================================
*/
/*
==================
RB_DrawInteractions
==================
*/
static void RB_DrawInteractions()
{
    VITA_RB_PASS(3);
    if (r_skipInteractions.GetBool()) {
        return;
    }

    renderLog.OpenMainBlock(MRB_DRAW_INTERACTIONS);
    renderLog.OpenBlock("RB_DrawInteractions");

    GL_SelectTexture(0);

    const bool useLightDepthBounds = r_useLightDepthBounds.GetBool();

    //
    // for each light, perform shadowing and adding
    //
    for (const viewLight_t* vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        // do fogging later
        if (vLight->lightShader->IsFogLight()) {
            continue;
        }
        if (vLight->lightShader->IsBlendLight()) {
            continue;
        }

        if (vLight->localInteractions == NULL && vLight->globalInteractions == NULL && vLight->translucentInteractions == NULL) {
            continue;
        }

        const idMaterial* lightShader = vLight->lightShader;
        renderLog.OpenBlock(lightShader->GetName());

        // set the depth bounds for the whole light
        if (useLightDepthBounds) {
            GL_DepthBoundsTest(vLight->scissorRect.zmin, vLight->scissorRect.zmax);
        }

        // only need to clear the stencil buffer and perform stencil testing if there are shadows
        const bool performStencilTest = (vLight->globalShadows != NULL || vLight->localShadows != NULL);

        // mirror flips the sense of the stencil select, and I don't want to risk accidentally breaking it
        // in the normal case, so simply disable the stencil select in the mirror case
        const bool useLightStencilSelect = (r_useLightStencilSelect.GetBool() && backEnd.viewDef->isMirror == false);

        if (performStencilTest) {
            if (useLightStencilSelect) {
                // write a stencil mask for the visible light bounds to hi-stencil
                RB_StencilSelectLight(vLight);
            } else {
                // always clear whole S-Cull tiles
                idScreenRect rect;
                rect.x1 = (vLight->scissorRect.x1 + 0) & ~15;
                rect.y1 = (vLight->scissorRect.y1 + 0) & ~15;
                rect.x2 = (vLight->scissorRect.x2 + 15) & ~15;
                rect.y2 = (vLight->scissorRect.y2 + 15) & ~15;

                if (!backEnd.currentScissor.Equals(rect) && r_useScissor.GetBool()) {
                    GL_Scissor(backEnd.viewDef->viewport.x1 + rect.x1,
                        backEnd.viewDef->viewport.y1 + rect.y1,
                        rect.x2 + 1 - rect.x1,
                        rect.y2 + 1 - rect.y1);
                    backEnd.currentScissor = rect;
                }
                GL_State(GLS_DEFAULT); // make sure stencil mask passes for the clear
                GL_Clear(false, false, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f);
            }
        }

        if (vLight->globalShadows != NULL) {
            renderLog.OpenBlock("Global Light Shadows");
            RB_StencilShadowPass(vLight->globalShadows, vLight);
            renderLog.CloseBlock();
        }

        if (vLight->localInteractions != NULL) {
            renderLog.OpenBlock("Local Light Interactions");
            RB_RenderInteractions(vLight->localInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds);
            renderLog.CloseBlock();
        }

        if (vLight->localShadows != NULL) {
            renderLog.OpenBlock("Local Light Shadows");
            RB_StencilShadowPass(vLight->localShadows, vLight);
            renderLog.CloseBlock();
        }

        if (vLight->globalInteractions != NULL) {
            renderLog.OpenBlock("Global Light Interactions");
            RB_RenderInteractions(vLight->globalInteractions, vLight, GLS_DEPTHFUNC_EQUAL, performStencilTest, useLightDepthBounds);
            renderLog.CloseBlock();
        }

        if (vLight->translucentInteractions != NULL && !r_skipTranslucent.GetBool()) {
            renderLog.OpenBlock("Translucent Interactions");

            // Disable the depth bounds test because translucent surfaces don't work with
            // the depth bounds tests since they did not write depth during the depth pass.
            if (useLightDepthBounds) {
                GL_DepthBoundsTest(0.0f, 0.0f);
            }

            // The depth buffer wasn't filled in for translucent surfaces, so they
            // can never be constrained to perforated surfaces with the depthfunc equal.

            // Translucent surfaces do not receive shadows. This is a case where a
            // shadow buffer solution would work but stencil shadows do not because
            // stencil shadows only affect surfaces that contribute to the view depth
            // buffer and translucent surfaces do not contribute to the view depth buffer.

            RB_RenderInteractions(vLight->translucentInteractions, vLight, GLS_DEPTHFUNC_LESS, false, false);

            renderLog.CloseBlock();
        }

        renderLog.CloseBlock();
    }

    // disable stencil shadow test
    GL_State(GLS_DEFAULT);

    // unbind texture units
    for (int i = 0; i < 5; i++) {
        GL_SelectTexture(i);
        globalImages->BindNull();
    }
    GL_SelectTexture(0);

    // reset depth bounds
    if (useLightDepthBounds) {
        GL_DepthBoundsTest(0.0f, 0.0f);
    }

    renderLog.CloseBlock();
    renderLog.CloseMainBlock();
}

/*
=============================================================================================

NON-INTERACTION SHADER PASSES

=============================================================================================
*/

/*
=====================
RB_DrawShaderPasses

Draw non-light dependent passes

If we are rendering Guis, the drawSurf_t::sort value is a depth offset that can
be multiplied by guiEye for polarity and screenSeparation for scale.
=====================
*/
static int RB_DrawShaderPasses(const drawSurf_t* const* const drawSurfs, const int numDrawSurfs,
    const float guiStereoScreenOffset, const int stereoEye)
{
    VITA_RB_PASS(4);
    // only obey skipAmbient if we are rendering a view
    if (backEnd.viewDef->viewEntitys && r_skipAmbient.GetBool()) {
        return numDrawSurfs;
    }

    renderLog.OpenBlock("RB_DrawShaderPasses");

    GL_SelectTexture(1);
    globalImages->BindNull();

    GL_SelectTexture(0);

    backEnd.currentSpace = (const viewEntity_t*)1; // using NULL makes /analyze think surf->space needs to be checked...
    float currentGuiStereoOffset = 0.0f;

    int i = 0;
    for (; i < numDrawSurfs; i++) {
        const drawSurf_t* surf = drawSurfs[i];
        const idMaterial* shader = surf->material;

        if (!shader->HasAmbient()) {
            continue;
        }

        if (shader->IsPortalSky()) {
            continue;
        }

        // some deforms may disable themselves by setting numIndexes = 0
        if (surf->numIndexes == 0) {
            continue;
        }

        if (shader->SuppressInSubview()) {
            continue;
        }

        if (backEnd.viewDef->isXraySubview && surf->space->entityDef) {
            if (surf->space->entityDef->parms.xrayIndex != 2) {
                continue;
            }
        }

        // we need to draw the post process shaders after we have drawn the fog lights
        if (shader->GetSort() >= SS_POST_PROCESS && !backEnd.currentRenderCopied) {
            break;
        }

        // if we are rendering a 3D view and the surface's eye index doesn't match
        // the current view's eye index then we skip the surface
        // if the stereoEye value of a surface is 0 then we need to draw it for both eyes.
        const int shaderStereoEye = shader->GetStereoEye();
        const bool isEyeValid = stereoRender_swapEyes.GetBool() ? (shaderStereoEye == stereoEye) : (shaderStereoEye != stereoEye);
        if ((stereoEye != 0) && (shaderStereoEye != 0) && (isEyeValid)) {
            continue;
        }

        renderLog.OpenBlock(shader->GetName());

        // determine the stereoDepth offset
        // guiStereoScreenOffset will always be zero for 3D views, so the !=
        // check will never force an update due to the current sort value.
        const float thisGuiStereoOffset = guiStereoScreenOffset * surf->sort;

        // change the matrix and other space related vars if needed
        if (surf->space != backEnd.currentSpace || thisGuiStereoOffset != currentGuiStereoOffset) {
            backEnd.currentSpace = surf->space;
            currentGuiStereoOffset = thisGuiStereoOffset;

            const viewEntity_t* space = backEnd.currentSpace;

            if (guiStereoScreenOffset != 0.0f) {
                RB_SetMVPWithStereoOffset(space->mvp, currentGuiStereoOffset);
            } else {
                RB_SetMVP(space->mvp);
            }

            // set eye position in local space
            idVec4 localViewOrigin(1.0f);
            R_GlobalPointToLocal(space->modelMatrix, backEnd.viewDef->renderView.vieworg, localViewOrigin.ToVec3());
            SetVertexParm(RENDERPARM_LOCALVIEWORIGIN, localViewOrigin.ToFloatPtr());

            // set model Matrix
            float modelMatrixTranspose[16];
            R_MatrixTranspose(space->modelMatrix, modelMatrixTranspose);
            SetVertexParms(RENDERPARM_MODELMATRIX_X, modelMatrixTranspose, 4);

            // Set ModelView Matrix
            float modelViewMatrixTranspose[16];
            R_MatrixTranspose(space->modelViewMatrix, modelViewMatrixTranspose);
            SetVertexParms(RENDERPARM_MODELVIEWMATRIX_X, modelViewMatrixTranspose, 4);
        }

        // change the scissor if needed
        if (!backEnd.currentScissor.Equals(surf->scissorRect) && r_useScissor.GetBool()) {
            GL_Scissor(backEnd.viewDef->viewport.x1 + surf->scissorRect.x1,
                backEnd.viewDef->viewport.y1 + surf->scissorRect.y1,
                surf->scissorRect.x2 + 1 - surf->scissorRect.x1,
                surf->scissorRect.y2 + 1 - surf->scissorRect.y1);
            backEnd.currentScissor = surf->scissorRect;
        }

        // get the expressions for conditionals / color / texcoords
        const float* regs = surf->shaderRegisters;

        // set face culling appropriately
        if (surf->space->isGuiSurface) {
            GL_Cull(CT_TWO_SIDED);
        } else {
            GL_Cull(shader->GetCullType());
        }

        uint64 surfGLState = surf->extraGLState;

        // set polygon offset if necessary
        if (shader->TestMaterialFlag(MF_POLYGONOFFSET)) {
            GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
            surfGLState = GLS_POLYGON_OFFSET;
        }

        for (int stage = 0; stage < shader->GetNumStages(); stage++) {
            const shaderStage_t* pStage = shader->GetStage(stage);

            // check the enable condition
            if (regs[pStage->conditionRegister] == 0) {
                continue;
            }

            // skip the stages involved in lighting
            if (pStage->lighting != SL_AMBIENT) {
                continue;
            }

            uint64 stageGLState = surfGLState;
            if ((surfGLState & GLS_OVERRIDE) == 0) {
                stageGLState |= pStage->drawStateBits;
            }

            // skip if the stage is ( GL_ZERO, GL_ONE ), which is used for some alpha masks
            if ((stageGLState & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) == (GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE)) {
                continue;
            }

            // see if we are a new-style stage
            newShaderStage_t* newStage = pStage->newStage;
            if (newStage != NULL) {
                //--------------------------
                //
                // new style stages
                //
                //--------------------------
                if (r_skipNewAmbient.GetBool()) {
                    continue;
                }
                renderLog.OpenBlock("New Shader Stage");

                GL_State(stageGLState);

                // RB: CRITICAL BUGFIX: changed newStage->glslProgram to vertexProgram and fragmentProgram
                // otherwise it will result in an out of bounds crash in RB_DrawElementsWithCounters
                renderProgManager.BindShader(newStage->glslProgram, newStage->vertexProgram, newStage->fragmentProgram, false);
                // RB end

                for (int j = 0; j < newStage->numVertexParms; j++) {
                    float parm[4];
                    parm[0] = regs[newStage->vertexParms[j][0]];
                    parm[1] = regs[newStage->vertexParms[j][1]];
                    parm[2] = regs[newStage->vertexParms[j][2]];
                    parm[3] = regs[newStage->vertexParms[j][3]];
                    SetVertexParm((renderParm_t)(RENDERPARM_USER + j), parm);
                }

                // set rpEnableSkinning if the shader has optional support for skinning
                if (surf->jointCache && renderProgManager.ShaderHasOptionalSkinning()) {
                    const idVec4 skinningParm(1.0f);
                    SetVertexParm(RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr());
                }

                // bind texture units
                for (int j = 0; j < newStage->numFragmentProgramImages; j++) {
                    idImage* image = newStage->fragmentProgramImages[j];
                    if (image != NULL) {
                        GL_SelectTexture(j);
                        image->Bind();
                    }
                }

                // draw it
                RB_DrawElementsWithCounters(surf);

                // unbind texture units
                for (int j = 0; j < newStage->numFragmentProgramImages; j++) {
                    idImage* image = newStage->fragmentProgramImages[j];
                    if (image != NULL) {
                        GL_SelectTexture(j);
                        globalImages->BindNull();
                    }
                }

                // clear rpEnableSkinning if it was set
                if (surf->jointCache && renderProgManager.ShaderHasOptionalSkinning()) {
                    const idVec4 skinningParm(0.0f);
                    SetVertexParm(RENDERPARM_ENABLE_SKINNING, skinningParm.ToFloatPtr());
                }

                GL_SelectTexture(0);
                renderProgManager.Unbind();

                renderLog.CloseBlock();
                continue;
            }

            //--------------------------
            //
            // old style stages
            //
            //--------------------------

            // set the color
            float color[4];
            color[0] = regs[pStage->color.registers[0]];
            color[1] = regs[pStage->color.registers[1]];
            color[2] = regs[pStage->color.registers[2]];
            color[3] = regs[pStage->color.registers[3]];

            // skip the entire stage if an add would be black
            if ((stageGLState & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) == (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE)
                && color[0] <= 0 && color[1] <= 0 && color[2] <= 0) {
                continue;
            }

            // skip the entire stage if a blend would be completely transparent
            if ((stageGLState & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)) == (GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA)
                && color[3] <= 0) {
                continue;
            }

            stageVertexColor_t svc = pStage->vertexColor;

            renderLog.OpenBlock("Old Shader Stage");
            GL_Color(color);

            if (surf->space->isGuiSurface) {
                // Force gui surfaces to always be SVC_MODULATE
                svc = SVC_MODULATE;

                // use special shaders for bink cinematics
                if (pStage->texture.cinematic) {
                    if ((stageGLState & GLS_OVERRIDE) != 0) {
                        // This is a hack... Only SWF Guis set GLS_OVERRIDE
                        // Old style guis do not, and we don't want them to use the new GUI renederProg
                        renderProgManager.BindShader_BinkGUI();
                    } else {
                        renderProgManager.BindShader_Bink();
                    }
                } else {
                    if ((stageGLState & GLS_OVERRIDE) != 0) {
                        // This is a hack... Only SWF Guis set GLS_OVERRIDE
                        // Old style guis do not, and we don't want them to use the new GUI renderProg
                        renderProgManager.BindShader_GUI();
                    } else {
                        if (surf->jointCache) {
                            renderProgManager.BindShader_TextureVertexColorSkinned();
                        } else {
                            renderProgManager.BindShader_TextureVertexColor();
                        }
                    }
                }
            } else if ((pStage->texture.texgen == TG_SCREEN) || (pStage->texture.texgen == TG_SCREEN2)) {
                renderProgManager.BindShader_TextureTexGenVertexColor();
            } else if (pStage->texture.cinematic) {
                renderProgManager.BindShader_Bink();
            } else {
                if (surf->jointCache) {
                    renderProgManager.BindShader_TextureVertexColorSkinned();
                } else {
                    renderProgManager.BindShader_TextureVertexColor();
                }
            }

            RB_SetVertexColorParms(svc);

            // bind the texture
            RB_BindVariableStageImage(&pStage->texture, regs);

            // set privatePolygonOffset if necessary
            if (pStage->privatePolygonOffset) {
                GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset);
                stageGLState |= GLS_POLYGON_OFFSET;
            }

            // set the state
            GL_State(stageGLState);

            RB_PrepareStageTexturing(pStage, surf);

            // draw it
            RB_DrawElementsWithCounters(surf);

            RB_FinishStageTexturing(pStage, surf);

            // unset privatePolygonOffset if necessary
            if (pStage->privatePolygonOffset) {
                GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
            }
            renderLog.CloseBlock();
        }

        renderLog.CloseBlock();
    }

    GL_Cull(CT_FRONT_SIDED);
    GL_Color(1.0f, 1.0f, 1.0f);

    renderLog.CloseBlock();
    return i;
}

/*
=============================================================================================

BLEND LIGHT PROJECTION

=============================================================================================
*/

/*
=====================
RB_T_BlendLight
=====================
*/
static void RB_T_BlendLight(const drawSurf_t* drawSurfs, const viewLight_t* vLight)
{
    backEnd.currentSpace = NULL;

    for (const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight) {
        if (drawSurf->scissorRect.IsEmpty()) {
            continue; // !@# FIXME: find out why this is sometimes being hit!
                      // temporarily jump over the scissor and draw so the gl error callback doesn't get hit
        }

        if (!backEnd.currentScissor.Equals(drawSurf->scissorRect) && r_useScissor.GetBool()) {
            // change the scissor
            GL_Scissor(backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
                backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
                drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
                drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1);
            backEnd.currentScissor = drawSurf->scissorRect;
        }

        if (drawSurf->space != backEnd.currentSpace) {
            // change the matrix
            RB_SetMVP(drawSurf->space->mvp);

            // change the light projection matrix
            idPlane lightProjectInCurrentSpace[4];
            for (int i = 0; i < 4; i++) {
                R_GlobalPlaneToLocal(drawSurf->space->modelMatrix, vLight->lightProject[i], lightProjectInCurrentSpace[i]);
            }

            SetVertexParm(RENDERPARM_TEXGEN_0_S, lightProjectInCurrentSpace[0].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_0_T, lightProjectInCurrentSpace[1].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_0_Q, lightProjectInCurrentSpace[2].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_1_S, lightProjectInCurrentSpace[3].ToFloatPtr()); // falloff

            backEnd.currentSpace = drawSurf->space;
        }

        RB_DrawElementsWithCounters(drawSurf);
    }
}

/*
=====================
RB_BlendLight

Dual texture together the falloff and projection texture with a blend
mode to the framebuffer, instead of interacting with the surface texture
=====================
*/
static void RB_BlendLight(const drawSurf_t* drawSurfs, const drawSurf_t* drawSurfs2, const viewLight_t* vLight)
{
    if (drawSurfs == NULL) {
        return;
    }
    if (r_skipBlendLights.GetBool()) {
        return;
    }
    renderLog.OpenBlock(vLight->lightShader->GetName());

    const idMaterial* lightShader = vLight->lightShader;
    const float* regs = vLight->shaderRegisters;

    // texture 1 will get the falloff texture
    GL_SelectTexture(1);
    vLight->falloffImage->Bind();

    // texture 0 will get the projected texture
    GL_SelectTexture(0);

    renderProgManager.BindShader_BlendLight();

    for (int i = 0; i < lightShader->GetNumStages(); i++) {
        const shaderStage_t* stage = lightShader->GetStage(i);

        if (!regs[stage->conditionRegister]) {
            continue;
        }

        GL_State(GLS_DEPTHMASK | stage->drawStateBits | GLS_DEPTHFUNC_EQUAL);

        GL_SelectTexture(0);
        stage->texture.image->Bind();

        if (stage->texture.hasMatrix) {
            RB_LoadShaderTextureMatrix(regs, &stage->texture);
        }

        // get the modulate values from the light, including alpha, unlike normal lights
        float lightColor[4];
        lightColor[0] = regs[stage->color.registers[0]];
        lightColor[1] = regs[stage->color.registers[1]];
        lightColor[2] = regs[stage->color.registers[2]];
        lightColor[3] = regs[stage->color.registers[3]];
        GL_Color(lightColor);

        RB_T_BlendLight(drawSurfs, vLight);
        RB_T_BlendLight(drawSurfs2, vLight);
    }

    GL_SelectTexture(1);
    globalImages->BindNull();

    GL_SelectTexture(0);

    renderProgManager.Unbind();
    renderLog.CloseBlock();
}

/*
=========================================================================================================

FOG LIGHTS

=========================================================================================================
*/

/*
=====================
RB_T_BasicFog
=====================
*/
static void RB_T_BasicFog(const drawSurf_t* drawSurfs, const idPlane fogPlanes[4], const idRenderMatrix* inverseBaseLightProject)
{
    backEnd.currentSpace = NULL;

    for (const drawSurf_t* drawSurf = drawSurfs; drawSurf != NULL; drawSurf = drawSurf->nextOnLight) {
        if (drawSurf->scissorRect.IsEmpty()) {
            continue; // !@# FIXME: find out why this is sometimes being hit!
                      // temporarily jump over the scissor and draw so the gl error callback doesn't get hit
        }

        if (!backEnd.currentScissor.Equals(drawSurf->scissorRect) && r_useScissor.GetBool()) {
            // change the scissor
            GL_Scissor(backEnd.viewDef->viewport.x1 + drawSurf->scissorRect.x1,
                backEnd.viewDef->viewport.y1 + drawSurf->scissorRect.y1,
                drawSurf->scissorRect.x2 + 1 - drawSurf->scissorRect.x1,
                drawSurf->scissorRect.y2 + 1 - drawSurf->scissorRect.y1);
            backEnd.currentScissor = drawSurf->scissorRect;
        }

        if (drawSurf->space != backEnd.currentSpace) {
            idPlane localFogPlanes[4];
            if (inverseBaseLightProject == NULL) {
                RB_SetMVP(drawSurf->space->mvp);
                for (int i = 0; i < 4; i++) {
                    R_GlobalPlaneToLocal(drawSurf->space->modelMatrix, fogPlanes[i], localFogPlanes[i]);
                }
            } else {
                idRenderMatrix invProjectMVPMatrix;
                idRenderMatrix::Multiply(backEnd.viewDef->worldSpace.mvp, *inverseBaseLightProject, invProjectMVPMatrix);
                RB_SetMVP(invProjectMVPMatrix);
                for (int i = 0; i < 4; i++) {
                    inverseBaseLightProject->InverseTransformPlane(fogPlanes[i], localFogPlanes[i], false);
                }
            }

            SetVertexParm(RENDERPARM_TEXGEN_0_S, localFogPlanes[0].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_0_T, localFogPlanes[1].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_1_T, localFogPlanes[2].ToFloatPtr());
            SetVertexParm(RENDERPARM_TEXGEN_1_S, localFogPlanes[3].ToFloatPtr());

            backEnd.currentSpace = (inverseBaseLightProject == NULL) ? drawSurf->space : NULL;
        }

        if (drawSurf->jointCache) {
            renderProgManager.BindShader_FogSkinned();
        } else {
            renderProgManager.BindShader_Fog();
        }

        RB_DrawElementsWithCounters(drawSurf);
    }
}

/*
==================
RB_FogPass
==================
*/
static void RB_FogPass(const drawSurf_t* drawSurfs, const drawSurf_t* drawSurfs2, const viewLight_t* vLight)
{
    renderLog.OpenBlock(vLight->lightShader->GetName());

    // find the current color and density of the fog
    const idMaterial* lightShader = vLight->lightShader;
    const float* regs = vLight->shaderRegisters;
    // assume fog shaders have only a single stage
    const shaderStage_t* stage = lightShader->GetStage(0);

    float lightColor[4];
    lightColor[0] = regs[stage->color.registers[0]];
    lightColor[1] = regs[stage->color.registers[1]];
    lightColor[2] = regs[stage->color.registers[2]];
    lightColor[3] = regs[stage->color.registers[3]];

    GL_Color(lightColor);

    // calculate the falloff planes
    float a;

    // if they left the default value on, set a fog distance of 500
    if (lightColor[3] <= 1.0f) {
        a = -0.5f / DEFAULT_FOG_DISTANCE;
    } else {
        // otherwise, distance = alpha color
        a = -0.5f / lightColor[3];
    }

    // texture 0 is the falloff image
    GL_SelectTexture(0);
    globalImages->fogImage->Bind();

    // texture 1 is the entering plane fade correction
    GL_SelectTexture(1);
    globalImages->fogEnterImage->Bind();

    // S is based on the view origin
    const float s = vLight->fogPlane.Distance(backEnd.viewDef->renderView.vieworg);

    const float FOG_SCALE = 0.001f;

    idPlane fogPlanes[4];

    // S-0
    fogPlanes[0][0] = a * backEnd.viewDef->worldSpace.modelViewMatrix[0 * 4 + 2];
    fogPlanes[0][1] = a * backEnd.viewDef->worldSpace.modelViewMatrix[1 * 4 + 2];
    fogPlanes[0][2] = a * backEnd.viewDef->worldSpace.modelViewMatrix[2 * 4 + 2];
    fogPlanes[0][3] = a * backEnd.viewDef->worldSpace.modelViewMatrix[3 * 4 + 2] + 0.5f;

    // T-0
    fogPlanes[1][0] = 0.0f; // a * backEnd.viewDef->worldSpace.modelViewMatrix[0*4+0];
    fogPlanes[1][1] = 0.0f; // a * backEnd.viewDef->worldSpace.modelViewMatrix[1*4+0];
    fogPlanes[1][2] = 0.0f; // a * backEnd.viewDef->worldSpace.modelViewMatrix[2*4+0];
    fogPlanes[1][3] = 0.5f; // a * backEnd.viewDef->worldSpace.modelViewMatrix[3*4+0] + 0.5f;

    // T-1 will get a texgen for the fade plane, which is always the "top" plane on unrotated lights
    fogPlanes[2][0] = FOG_SCALE * vLight->fogPlane[0];
    fogPlanes[2][1] = FOG_SCALE * vLight->fogPlane[1];
    fogPlanes[2][2] = FOG_SCALE * vLight->fogPlane[2];
    fogPlanes[2][3] = FOG_SCALE * vLight->fogPlane[3] + FOG_ENTER;

    // S-1
    fogPlanes[3][0] = 0.0f;
    fogPlanes[3][1] = 0.0f;
    fogPlanes[3][2] = 0.0f;
    fogPlanes[3][3] = FOG_SCALE * s + FOG_ENTER;

    // draw it
    GL_State(GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL);
    RB_T_BasicFog(drawSurfs, fogPlanes, NULL);
    RB_T_BasicFog(drawSurfs2, fogPlanes, NULL);

    // the light frustum bounding planes aren't in the depth buffer, so use depthfunc_less instead
    // of depthfunc_equal
    GL_State(GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_LESS);
    GL_Cull(CT_BACK_SIDED);

    backEnd.zeroOneCubeSurface.space = &backEnd.viewDef->worldSpace;
    backEnd.zeroOneCubeSurface.scissorRect = backEnd.viewDef->scissor;
    RB_T_BasicFog(&backEnd.zeroOneCubeSurface, fogPlanes, &vLight->inverseBaseLightProject);

    GL_Cull(CT_FRONT_SIDED);

    GL_SelectTexture(1);
    globalImages->BindNull();

    GL_SelectTexture(0);

    renderProgManager.Unbind();

    renderLog.CloseBlock();
}

/*
==================
RB_FogAllLights
==================
*/
static void RB_FogAllLights()
{
    VITA_RB_PASS(5);
    if (r_skipFogLights.GetBool() || r_showOverDraw.GetInteger() != 0
        || backEnd.viewDef->isXraySubview /* don't fog in xray mode*/) {
        return;
    }
    renderLog.OpenMainBlock(MRB_FOG_ALL_LIGHTS);
    renderLog.OpenBlock("RB_FogAllLights");

    // force fog plane to recalculate
    backEnd.currentSpace = NULL;

    for (viewLight_t* vLight = backEnd.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        if (vLight->lightShader->IsFogLight()) {
            RB_FogPass(vLight->globalInteractions, vLight->localInteractions, vLight);
        } else if (vLight->lightShader->IsBlendLight()) {
            RB_BlendLight(vLight->globalInteractions, vLight->localInteractions, vLight);
        }
    }

    renderLog.CloseBlock();
    renderLog.CloseMainBlock();
}

/*
=========================================================================================================

BACKEND COMMANDS

=========================================================================================================
*/

/*
==================
RB_DrawViewInternal
==================
*/
void RB_DrawViewInternal(const viewDef_t* viewDef, const int stereoEye)
{
    renderLog.OpenBlock("RB_DrawViewInternal");
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    // every main 3D view drawn counts, warping effects or not: a copy is
    // "from the previous frame" only if no frame went by in between
    if (!viewDef->isSubview && !viewDef->is2Dgui) {
        vitaMainViewDraws++;
    }
#endif

    //-------------------------------------------------
    // guis can wind up referencing purged images that need to be loaded.
    // this used to be in the gui emit code, but now that it can be running
    // in a separate thread, it must not try to load images, so do it here.
    //-------------------------------------------------
    drawSurf_t** drawSurfs = (drawSurf_t**)&viewDef->drawSurfs[0];
    const int numDrawSurfs = viewDef->numDrawSurfs;

    for (int i = 0; i < numDrawSurfs; i++) {
        const drawSurf_t* ds = viewDef->drawSurfs[i];
        if (ds->material != NULL) {
            const_cast<idMaterial*>(ds->material)->EnsureNotPurged();
        }
    }

    //-------------------------------------------------
    // RB_BeginDrawingView
    //
    // Any mirrored or portaled views have already been drawn, so prepare
    // to actually render the visible surfaces for this view
    //
    // clear the z buffer, set the projection matrix, etc
    //-------------------------------------------------

    // set the window clipping
    GL_Viewport(viewDef->viewport.x1,
        viewDef->viewport.y1,
        viewDef->viewport.x2 + 1 - viewDef->viewport.x1,
        viewDef->viewport.y2 + 1 - viewDef->viewport.y1);

    // the scissor may be smaller than the viewport for subviews
    GL_Scissor(backEnd.viewDef->viewport.x1 + viewDef->scissor.x1,
        backEnd.viewDef->viewport.y1 + viewDef->scissor.y1,
        viewDef->scissor.x2 + 1 - viewDef->scissor.x1,
        viewDef->scissor.y2 + 1 - viewDef->scissor.y1);
    backEnd.currentScissor = viewDef->scissor;

    backEnd.glState.faceCulling = -1; // force face culling to set next time

    // ensures that depth writes are enabled for the depth clear
    GL_State(GLS_DEFAULT);

    // Clear the depth buffer and clear the stencil to 128 for stencil shadows as well as gui masking
    GL_Clear(false, true, true, STENCIL_SHADOW_TEST_VALUE, 0.0f, 0.0f, 0.0f, 0.0f);

    // normal face culling
    GL_Cull(CT_FRONT_SIDED);

#ifdef USE_CORE_PROFILE
    // bind one global Vertex Array Object (VAO)
    qglBindVertexArray(glConfig.global_vao);
#endif

    //------------------------------------
    // sets variables that can be used by all programs
    //------------------------------------
    {
        //
        // set eye position in global space
        //
        float parm[4];
        parm[0] = backEnd.viewDef->renderView.vieworg[0];
        parm[1] = backEnd.viewDef->renderView.vieworg[1];
        parm[2] = backEnd.viewDef->renderView.vieworg[2];
        parm[3] = 1.0f;

        SetVertexParm(RENDERPARM_GLOBALEYEPOS, parm); // rpGlobalEyePos

        // sets overbright to make world brighter
        // This value is baked into the specularScale and diffuseScale values so
        // the interaction programs don't need to perform the extra multiply,
        // but any other renderprogs that want to obey the brightness value
        // can reference this.
        float overbright = r_lightScale.GetFloat() * 0.5f;
        parm[0] = overbright;
        parm[1] = overbright;
        parm[2] = overbright;
        parm[3] = overbright;
        SetFragmentParm(RENDERPARM_OVERBRIGHT, parm);

        // Set Projection Matrix
        float projMatrixTranspose[16];
        R_MatrixTranspose(backEnd.viewDef->projectionMatrix, projMatrixTranspose);
        SetVertexParms(RENDERPARM_PROJMATRIX_X, projMatrixTranspose, 4);
    }

    //-------------------------------------------------
    // fill the depth buffer and clear color buffer to black except on subviews
    //-------------------------------------------------
    RB_FillDepthBufferFast(drawSurfs, numDrawSurfs);

    //-------------------------------------------------
    // main light renderer
    //-------------------------------------------------
    RB_DrawInteractions();

    //-------------------------------------------------
    // now draw any non-light dependent shading passes
    //-------------------------------------------------
    int processed = 0;
    if (!r_skipShaderPasses.GetBool()) {
        renderLog.OpenMainBlock(MRB_DRAW_SHADER_PASSES);
        float guiScreenOffset;
        if (viewDef->viewEntitys != NULL) {
            // guiScreenOffset will be 0 in non-gui views
            guiScreenOffset = 0.0f;
        } else {
            guiScreenOffset = stereoEye * viewDef->renderView.stereoScreenSeparation;
        }
        processed = RB_DrawShaderPasses(drawSurfs, numDrawSurfs, guiScreenOffset, stereoEye);
        renderLog.CloseMainBlock();
    }

    //-------------------------------------------------
    // fog and blend lights, drawn after emissive surfaces
    // so they are properly dimmed down
    //-------------------------------------------------
    RB_FogAllLights();

    //-------------------------------------------------
    // capture the depth for the motion blur before rendering any post process surfaces that may contribute to the depth
    //-------------------------------------------------
    if (r_motionBlur.GetInteger() > 0) {
        const idScreenRect& viewport = backEnd.viewDef->viewport;
        VITA_RB_PASS(6);
        globalImages->currentDepthImage->CopyDepthbuffer(viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight());
    }

    //-------------------------------------------------
    // now draw any screen warping post-process effects using _currentRender
    //-------------------------------------------------
    if (processed < numDrawSurfs && !r_skipPostProcess.GetBool()) {
        int x = backEnd.viewDef->viewport.x1;
        int y = backEnd.viewDef->viewport.y1;
        int w = backEnd.viewDef->viewport.x2 - backEnd.viewDef->viewport.x1 + 1;
        int h = backEnd.viewDef->viewport.y2 - backEnd.viewDef->viewport.y1 + 1;

        RENDERLOG_PRINTF("Resolve to %i x %i buffer\n", w, h);

        GL_SelectTexture(0);

        // resolve the screen
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
        // Vita (a cut, vita_currentRenderReuse): the main view copies the
        // screen into _currentRender at most every other frame; on the frame
        // between, its warping effects (heat haze, distortion) sample the
        // previous frame's copy. A copy ends the GPU scene being drawn, runs
        // the blit as a scene of its own and starts a new one - the main
        // thread spent 8-17 ms in it in the heaviest fight frames (run 36's
        // back end passes; 3-5 ms a frame on average in the fights).
        // Reused only when the last copy was this view's, from the previous
        // frame, at the same place and size (a subview's copy, a resolution
        // step or a skipped frame forces a fresh one).
        const bool vitaMainView = !backEnd.viewDef->isSubview && !backEnd.viewDef->is2Dgui;
        bool vitaReuse = false;
        uint64 vitaSignature = 0;
        if (vitaMainView && vita_currentRenderReuse.GetBool()) {
            vitaSignature = RB_VitaSceneSignature(backEnd.viewDef, drawSurfs, processed);
        }
        if (vitaMainView) {
            // reused for as long as nothing it shows changes (the signature
            // of the frame the copy was made in), any number of frames
            vitaReuse = vita_currentRenderReuse.GetBool() && vitaLastCopyMain
                        && vitaLastCopyDraw == vitaMainViewDraws - 1
                        && vitaLastCopyRect[0] == x && vitaLastCopyRect[1] == y && vitaLastCopyRect[2] == w && vitaLastCopyRect[3] == h;
            if (vitaReuse && (vitaSignature == 0 || vitaSignature != vitaLastCopySignature)) {
                vitaReuse = false;
                VITA_ZONE_COUNT("_currentRender reuse refused: something drawn before the copy changed (vita_screenCopyReuse2)");
            }
            // A copy one frame old only looks right while the view stands
            // still: moving or turning, glass and heat haze showed the
            // previous frame's picture every other frame (flicker, a low
            // update rate on anything transparent in motion).
            if (vitaReuse) {
                const renderView_t& rv = backEnd.viewDef->renderView;
                const bool moved = (rv.vieworg - vitaLastCopyOrigin).LengthSqr() > 0.0f;
                const bool turned = !rv.viewaxis.Compare(vitaLastCopyAxis);
                if (moved || turned) {
                    vitaReuse = false;
                    VITA_ZONE_COUNT("_currentRender reuse refused: the view moved (vita_currentRenderReuse)");
                } else if (vita_fullQuality) {
                    vitaReuse = false;
                    VITA_ZONE_COUNT("_currentRender reuse refused: cinematic (full quality)");
                }
            }
        }
        if (vitaReuse) {
            VITA_ZONE_COUNT("_currentRender copies reused from the previous frame (vita_currentRenderReuse)");
            vitaLastWasReuse = true;
            vitaLastCopyDraw = vitaMainViewDraws; // the copy stays this view's, one frame older
        } else {
            VITA_ZONE_COUNT("_currentRender copies made (screen blit, GPU scene split)");
            VITA_RB_PASS(6);
            globalImages->currentRenderImage->CopyFramebuffer(x, y, w, h);
            vitaLastWasReuse = false;
            vitaLastCopyMain = vitaMainView;
            vitaLastCopySignature = vitaMainView ? vitaSignature : 0;
            vitaLastCopyDraw = vitaMainView ? vitaMainViewDraws : -100;
            vitaLastCopyRect[0] = x;
            vitaLastCopyRect[1] = y;
            vitaLastCopyRect[2] = w;
            vitaLastCopyRect[3] = h;
            if (vitaMainView) {
                vitaLastCopyOrigin = backEnd.viewDef->renderView.vieworg;
                vitaLastCopyAxis = backEnd.viewDef->renderView.viewaxis;
            }
        }
#else
        {
            VITA_RB_PASS(6);
            globalImages->currentRenderImage->CopyFramebuffer(x, y, w, h);
        }
#endif
        backEnd.currentRenderCopied = true;

        // RENDERPARM_SCREENCORRECTIONFACTOR amd RENDERPARM_WINDOWCOORD overlap
        // diffuseScale and specularScale

        // screen power of two correction factor (no longer relevant now)
        float screenCorrectionParm[4];
        screenCorrectionParm[0] = 1.0f;
        screenCorrectionParm[1] = 1.0f;
        screenCorrectionParm[2] = 0.0f;
        screenCorrectionParm[3] = 1.0f;
        SetFragmentParm(RENDERPARM_SCREENCORRECTIONFACTOR, screenCorrectionParm); // rpScreenCorrectionFactor

        // window coord to 0.0 to 1.0 conversion
        float windowCoordParm[4];
        windowCoordParm[0] = 1.0f / w;
        windowCoordParm[1] = 1.0f / h;
        windowCoordParm[2] = 0.0f;
#if defined(__vita__)
        // The Vita's screen-space lookup (vitaVposToScreenPosTexCoord in
        // RenderProgs_GLSL.cpp) gets a top-origin fragment position r on the
        // whole display and computes v = w - r / h. The view's rows are GL
        // rows y .. y + h - 1 (bottom origin), i.e. display rows from
        // H - y - h down; v = (H - y - r) / h, so w = (H - y) / h. The scaled
        // 3D view sits in the TOP-left corner (y = H - h, w = 1): the old
        // H / h (a view at the bottom) pushed v past 1 over the top third of
        // the view, and the heat haze there smeared one row down the screen -
        // the striped blobs over the muzzle smoke.
        windowCoordParm[3] = (float)(renderSystem->GetHeight() - y) / (float)h;
#else
        windowCoordParm[3] = 1.0f;
#endif
        SetFragmentParm(RENDERPARM_WINDOWCOORD, windowCoordParm); // rpWindowCoord

        // render the remaining surfaces
        renderLog.OpenMainBlock(MRB_DRAW_SHADER_PASSES_POST);
        RB_DrawShaderPasses(drawSurfs + processed, numDrawSurfs - processed, 0.0f /* definitely not a gui */, stereoEye);
        renderLog.CloseMainBlock();
    }

    //-------------------------------------------------
    // render debug tools
    //-------------------------------------------------
    RB_RenderDebugTools(drawSurfs, numDrawSurfs);

    renderLog.CloseBlock();
}

/*
==================
RB_MotionBlur

Experimental feature
==================
*/
void RB_MotionBlur()
{
    if (!backEnd.viewDef->viewEntitys) {
        // 3D views only
        return;
    }
    if (r_motionBlur.GetInteger() <= 0) {
        return;
    }
    if (backEnd.viewDef->isSubview) {
        return;
    }

    GL_CheckErrors();

    // clear the alpha buffer and draw only the hands + weapon into it so
    // we can avoid blurring them
    qglClearColor(0, 0, 0, 1);
    GL_State(GLS_COLORMASK | GLS_DEPTHMASK);
    qglClear(GL_COLOR_BUFFER_BIT);
    GL_Color(0, 0, 0, 0);
    GL_SelectTexture(0);
    globalImages->blackImage->Bind();
    backEnd.currentSpace = NULL;

    drawSurf_t** drawSurfs = (drawSurf_t**)&backEnd.viewDef->drawSurfs[0];
    for (int surfNum = 0; surfNum < backEnd.viewDef->numDrawSurfs; surfNum++) {
        const drawSurf_t* surf = drawSurfs[surfNum];

        if (!surf->space->weaponDepthHack && !surf->space->skipMotionBlur && !surf->material->HasSubview()) {
            // Apply motion blur to this object
            continue;
        }

        const idMaterial* shader = surf->material;
        if (shader->Coverage() == MC_TRANSLUCENT) {
            // muzzle flash, etc
            continue;
        }

        // set mvp matrix
        if (surf->space != backEnd.currentSpace) {
            RB_SetMVP(surf->space->mvp);
            backEnd.currentSpace = surf->space;
        }

        // this could just be a color, but we don't have a skinned color-only prog
        if (surf->jointCache) {
            renderProgManager.BindShader_TextureVertexColorSkinned();
        } else {
            renderProgManager.BindShader_TextureVertexColor();
        }

        // draw it solid
        RB_DrawElementsWithCounters(surf);
    }
    GL_State(GLS_DEPTHFUNC_ALWAYS);

    // copy off the color buffer and the depth buffer for the motion blur prog
    // we use the viewport dimensions for copying the buffers in case resolution scaling is enabled.
    const idScreenRect& viewport = backEnd.viewDef->viewport;
    globalImages->currentRenderImage->CopyFramebuffer(viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight());

    // in stereo rendering, each eye needs to get a separate previous frame mvp
    int mvpIndex = (backEnd.viewDef->renderView.viewEyeBuffer == 1) ? 1 : 0;

    // derive the matrix to go from current pixels to previous frame pixels
    idRenderMatrix inverseMVP;
    idRenderMatrix::Inverse(backEnd.viewDef->worldSpace.mvp, inverseMVP);

    idRenderMatrix motionMatrix;
    idRenderMatrix::Multiply(backEnd.prevMVP[mvpIndex], inverseMVP, motionMatrix);

    backEnd.prevMVP[mvpIndex] = backEnd.viewDef->worldSpace.mvp;

    RB_SetMVP(motionMatrix);

    GL_State(GLS_DEPTHFUNC_ALWAYS);
    GL_Cull(CT_TWO_SIDED);

    renderProgManager.BindShader_MotionBlur();

    // let the fragment program know how many samples we are going to use
    idVec4 samples((float)(1 << r_motionBlur.GetInteger()));
    SetFragmentParm(RENDERPARM_OVERBRIGHT, samples.ToFloatPtr());

    GL_SelectTexture(0);
    globalImages->currentRenderImage->Bind();
    GL_SelectTexture(1);
    globalImages->currentDepthImage->Bind();

    RB_DrawElementsWithCounters(&backEnd.unitSquareSurface);
    GL_CheckErrors();
}

/*
==================
RB_DrawView

StereoEye will always be 0 in mono modes, or -1 / 1 in stereo modes.
If the view is a GUI view that is repeated for both eyes, the viewDef.stereoEye value
is 0, so the stereoEye parameter is not always the same as that.
==================
*/
void RB_DrawView(const void* data, const int stereoEye)
{
    VITA_RB_PASS(7);
    const drawSurfsCommand_t* cmd = (const drawSurfsCommand_t*)data;

    backEnd.viewDef = cmd->viewDef;

    // we will need to do a new copyTexSubImage of the screen
    // when a SS_POST_PROCESS material is used
    backEnd.currentRenderCopied = false;

    // if there aren't any drawsurfs, do nothing
    if (!backEnd.viewDef->numDrawSurfs) {
        return;
    }

    // skip render bypasses everything that has models, assuming
    // them to be 3D views, but leaves 2D rendering visible
    if (r_skipRender.GetBool() && backEnd.viewDef->viewEntitys) {
        return;
    }

    // skip render context sets the wgl context to NULL,
    // which should factor out the API cost, under the assumption
    // that all gl calls just return if the context isn't valid

    // RB: not really needed
    // if( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys )
    //{
    //	GLimp_DeactivateContext();
    //}
    // RB end

    backEnd.pc.c_surfaces += backEnd.viewDef->numDrawSurfs;

    RB_ShowOverdraw();

    // render the scene
    RB_DrawViewInternal(cmd->viewDef, stereoEye);

    RB_MotionBlur();

    // restore the context for 2D drawing if we were stubbing it out
    // RB: not really needed
    // if( r_skipRenderContext.GetBool() && backEnd.viewDef->viewEntitys )
    //{
    //	GLimp_ActivateContext();
    //	GL_SetDefaultState();
    //}
    // RB end

    // optionally draw a box colored based on the eye number
    if (r_drawEyeColor.GetBool()) {
        const idScreenRect& r = backEnd.viewDef->viewport;
        GL_Scissor((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2, 32, 32);
        switch (stereoEye) {
        case -1:
            GL_Clear(true, false, false, 0, 1.0f, 0.0f, 0.0f, 1.0f);
            break;
        case 1:
            GL_Clear(true, false, false, 0, 0.0f, 1.0f, 0.0f, 1.0f);
            break;
        default:
            GL_Clear(true, false, false, 0, 0.5f, 0.5f, 0.5f, 1.0f);
            break;
        }
    }
}

/*
==================
RB_CopyRender

Copy part of the current framebuffer to an image
==================
*/
void RB_CopyRender(const void* data)
{
    const copyRenderCommand_t* cmd = (const copyRenderCommand_t*)data;
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    vitaLastCopyMain = false; // any other screen copy may have written _currentRender
#endif

    if (r_skipCopyTexture.GetBool()) {
        return;
    }

    RENDERLOG_PRINTF("***************** RB_CopyRender *****************\n");

    if (cmd->image) {
        cmd->image->CopyFramebuffer(cmd->x, cmd->y, cmd->imageWidth, cmd->imageHeight);
    }

    if (cmd->clearColorAfterCopy) {
        GL_Clear(true, false, false, STENCIL_SHADOW_TEST_VALUE, 0, 0, 0, 0);
    }
}

/*
==================
RB_PostProcess

==================
*/
extern idCVar rs_enable;
// The verification's z-fail check (VitaVerify.cpp, shadowcheck): every shadow
// volume the view needs is drawn with Z-fail and its caps (Carmack's reverse
// everywhere) - the same stencil counts as the Z-pass / Z-fail mix the
// shadow jobs choose, so the same picture unless a cap or the far end at
// infinity is wrong. Read by the shadow volume jobs.
idCVar r_forceZFailStencilShadows("r_forceZFailStencilShadows", "0", CVAR_RENDERER | CVAR_BOOL, "draw every shadow volume with Z-fail and caps (a test: the picture must not change)");
bool R_ForceZFailStencilShadows()
{
    return r_forceZFailStencilShadows.GetBool(); // the shadow jobs keep to the idlib headers
}
// FXAA's constants (the shader: vita_fxaa, RenderProgs_GLSL.cpp; NVIDIA FXAA
// 3.11 console, its documented values): N 0.33 is its 'sharper' sub-pixel
// offset (0.50 default), sharpness 8 its 'sharper' default, threshold 0.125
// its default, minimum 0.05 its default
idCVar vita_fxaaN("vita_fxaaN", "0.33", CVAR_RENDERER | CVAR_FLOAT, "FXAA console: sub-pixel offset N (0.33 sharper, 0.50 default)");
idCVar vita_fxaaSharpness("vita_fxaaSharpness", "8", CVAR_RENDERER | CVAR_FLOAT, "FXAA console: edge sharpness (8 sharper - the default, 4 softer, 2 really soft)");
idCVar vita_fxaaThreshold("vita_fxaaThreshold", "0.125", CVAR_RENDERER | CVAR_FLOAT, "FXAA console: edge threshold (0.125 default, 0.25 sharper / more aliasing)");
idCVar vita_fxaaThresholdMin("vita_fxaaThresholdMin", "0.05", CVAR_RENDERER | CVAR_FLOAT, "FXAA console: edge threshold minimum - skips darks (0.04 slower, 0.05 default, 0.06 faster)");
#if defined(__vita__)
bool Vita_RenderScale(float& x, float& y);
#endif

// The stretch's sharpening and soft alpha-tested edges (RenderProgs_GLSL.cpp,
// vita_postFilter). Live.
#if defined(__vita__)
idCVar vita_sharpen("vita_sharpen", "0.5", CVAR_RENDERER | CVAR_FLOAT, "sharpening strength on the 3D view's stretch (LumaSharpen's diagonal 3-tap; 0 = off, 0.65 = LumaSharpen's default); grows with the stretch as dynamic resolution lowers the view; not on a PS TV", 0.0f, 2.0f);
idCVar vita_softAlphaEdges("vita_softAlphaEdges", "1", CVAR_RENDERER | CVAR_FLOAT, "soft edges for alpha-tested surfaces (grates, fences, foliage) on the 3D view's stretch, standing in for alpha to coverage (0 = off)", 0.0f, 1.0f);
#else
idCVar vita_sharpen("vita_sharpen", "0.5", CVAR_RENDERER | CVAR_FLOAT, "sharpening strength on the 3D view's stretch (vita_postFilter; LumaSharpen's diagonal 3-tap; 0 = off)", 0.0f, 2.0f);
idCVar vita_softAlphaEdges("vita_softAlphaEdges", "1", CVAR_RENDERER | CVAR_FLOAT, "soft edges for alpha-tested surfaces on the 3D view's stretch (vita_postFilter; 0 = off)", 0.0f, 1.0f);
#endif
idCVar vita_sharpenClamp("vita_sharpenClamp", "0.035", CVAR_RENDERER | CVAR_FLOAT, "the most the sharpening may change a pixel's brightness (0.035 = 9/255, LumaSharpen's default)", 0.001f, 0.25f);
idCVar vita_sharpenOnTV("vita_sharpenOnTV", "0", CVAR_RENDERER | CVAR_BOOL, "sharpen on a PS TV too (off: the TV scales the picture again)");
idCVar vita_softAlphaEdgesShow("vita_softAlphaEdgesShow", "0", CVAR_RENDERER | CVAR_BOOL, "paint the pixels the soft edges blur red (a check)");
idCVar vita_postFilterPlain("vita_postFilterPlain", "0", CVAR_RENDERER | CVAR_BOOL, "stretch the 3D view with the stock shader instead of the filter (no sharpening, no soft edges - the verification's cost check)");
extern bool r_vitaPostFilterBuilt;
// for the verification and the log: the last frame's map
int r_vitaAlphaEdgeSurfaces = 0;
int r_vitaAlphaEdgeFrames = 0; // frames with a map, since start

static bool RB_VitaOnTV()
{
#if defined(__vita__)
    static int tv = -1;
    if (tv < 0) {
        tv = sceKernelIsPSVitaTV() ? 1 : 0;
    }
    return tv == 1;
#else
    return false;
#endif
}

/*
==================
RB_VitaAlphaEdgeMask

Soft edges for alpha-tested surfaces (vita_softAlphaEdges): the user's
stand-in for alpha to coverage, which GXM does not have. Just before the
stretch copies the 3D view, the view's alpha channel - nothing reads it
past this point - becomes a map of where alpha-tested surfaces show:
zeroed over the view (a quad, alpha only: the colours are untouched), then
every alpha-tested stage is drawn again exactly as the depth pass drew it
(same shader, matrices, polygon offsets, culling and test), alpha only, with
depth EQUAL, writing its texture alpha where it passes the test. The
stretch's shader blurs where that map changes between neighbouring pixels -
the edges of the cut-outs, both sides - and sharpens everywhere else.

Returns the number of surfaces drawn: 0 (none in view) draws nothing, and
the shader is told to ignore alpha.
==================
*/
static int RB_VitaAlphaEdgeMask(const viewDef_t* viewDef)
{
    VITA_RB_PASS(8);
    if (viewDef == NULL || viewDef->viewEntitys == NULL || viewDef->numDrawSurfs <= 0 || viewDef->isSubview) {
        return 0;
    }
    // anything to draw?
    int candidates = 0;
    for (int i = 0; i < viewDef->numDrawSurfs && candidates == 0; i++) {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[i];
        if (drawSurf->material != NULL && drawSurf->material->Coverage() == MC_PERFORATED && drawSurf->material->GetSort() != SS_SUBVIEW) {
            candidates++;
        }
    }
    if (candidates == 0) {
        return 0;
    }
    renderLog.OpenBlock("RB_VitaAlphaEdgeMask");

    const idScreenRect& viewport = viewDef->viewport;
    GL_Viewport(viewport.x1, viewport.y1, viewport.x2 + 1 - viewport.x1, viewport.y2 + 1 - viewport.y1);
    GL_Scissor(viewport.x1, viewport.y1, viewport.x2 + 1 - viewport.x1, viewport.y2 + 1 - viewport.y1);

    // 1. alpha 0 over the view
    {
        float identity[16] = { 0 };
        identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
        renderProgManager.SetRenderParms(RENDERPARM_MVPMATRIX_X, identity, 4);
        GL_State(GLS_COLORMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS);
        GL_Cull(CT_TWO_SIDED);
        renderProgManager.BindShader_Color();
        GL_Color(0.0f, 0.0f, 0.0f, 0.0f);
        RB_DrawElementsWithCounters(&backEnd.unitSquareSurface);
    }

    // 2. the alpha-tested stages, as RB_FillDepthBufferGeneric draws them
    GL_Cull(CT_FRONT_SIDED);
    backEnd.currentSpace = NULL;
    int drawn = 0;
    for (int i = 0; i < viewDef->numDrawSurfs; i++) {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[i];
        const idMaterial* shader = drawSurf->material;
        if (shader == NULL || shader->Coverage() != MC_PERFORATED || shader->GetSort() == SS_SUBVIEW) {
            continue;
        }
        const float* regs = drawSurf->shaderRegisters;
        uint64 surfGLState = GLS_COLORMASK | GLS_DEPTHMASK | GLS_DEPTHFUNC_EQUAL;
        if (shader->TestMaterialFlag(MF_POLYGONOFFSET)) {
            surfGLState |= GLS_POLYGON_OFFSET;
            GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
        }
        bool spaceSet = false;
        for (int stage = 0; stage < shader->GetNumStages(); stage++) {
            const shaderStage_t* pStage = shader->GetStage(stage);
            if (!pStage->hasAlphaTest || regs[pStage->conditionRegister] == 0) {
                continue;
            }
            float color[4] = { 0.0f, 0.0f, 0.0f, regs[pStage->color.registers[3]] };
            if (color[3] <= 0.0f) {
                continue;
            }
            if (!spaceSet && drawSurf->space != backEnd.currentSpace) {
                RB_SetMVP(drawSurf->space->mvp);
                backEnd.currentSpace = drawSurf->space;
            }
            spaceSet = true;
            uint64 stageGLState = surfGLState;
            if (pStage->privatePolygonOffset) {
                GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * pStage->privatePolygonOffset);
                stageGLState |= GLS_POLYGON_OFFSET;
            }
            GL_Color(color);
#ifdef USE_CORE_PROFILE
            GL_State(stageGLState);
            idVec4 alphaTestValue(regs[pStage->alphaTestRegister]);
            SetFragmentParm(RENDERPARM_ALPHA_TEST, alphaTestValue.ToFloatPtr());
#else
            GL_State(stageGLState | GLS_ALPHATEST_FUNC_GREATER | GLS_ALPHATEST_MAKE_REF(idMath::Ftob(255.0f * regs[pStage->alphaTestRegister])));
#endif
            if (drawSurf->jointCache) {
                renderProgManager.BindShader_TextureVertexColorSkinned();
            } else {
                renderProgManager.BindShader_TextureVertexColor();
            }
            RB_SetVertexColorParms(SVC_IGNORE);
            GL_SelectTexture(0);
            pStage->texture.image->Bind();
            RB_PrepareStageTexturing(pStage, drawSurf);
            RB_DrawElementsWithCounters(drawSurf);
            RB_FinishStageTexturing(pStage, drawSurf);
            if (pStage->privatePolygonOffset) {
                GL_PolygonOffset(r_offsetFactor.GetFloat(), r_offsetUnits.GetFloat() * shader->GetPolygonOffset());
            }
            drawn++;
        }
    }
#ifdef USE_CORE_PROFILE
    SetFragmentParm(RENDERPARM_ALPHA_TEST, vec4_zero.ToFloatPtr());
#endif
    backEnd.currentSpace = NULL;
    renderLog.CloseBlock();
    return drawn;
}

void RB_PostProcess(const void* data)
{
    VITA_RB_PASS(9);

    // only do the post process step if resolution scaling is enabled. Prevents the unnecessary copying of the framebuffer and
    // corresponding full screen quad pass.
#if defined(__vita__)
    float vitaScaleX, vitaScaleY;
    if (rs_enable.GetInteger() == 0 && !Vita_RenderScale(vitaScaleX, vitaScaleY)) {
        return;
    }
#else
    if (rs_enable.GetInteger() == 0) {
        return;
    }
#endif

    // resolve the scaled rendering to a temporary texture
    postProcessCommand_t* cmd = (postProcessCommand_t*)data;
    const idScreenRect& viewport = cmd->viewDef->viewport;
#if defined(__vita__)
    {
        static int logged = 0;
        if (logged < 2) {
            logged++;
            Vita_Checkpoint("render scale: 3D view drawn at %dx%d in (%d,%d), stretched to %dx%d before the HUD/menus",
                viewport.GetWidth(), viewport.GetHeight(), viewport.x1, viewport.y1, renderSystem->GetWidth(), renderSystem->GetHeight());
        }
    }
#endif
    // the soft edges' map, in the view's alpha (before the copy)
    float softEdges = 0.0f;
    const bool vitaFilter = r_vitaPostFilterBuilt && !vita_postFilterPlain.GetBool();
    if (vitaFilter && vita_softAlphaEdges.GetFloat() > 0.0f) {
        r_vitaAlphaEdgeSurfaces = RB_VitaAlphaEdgeMask(cmd->viewDef);
        if (r_vitaAlphaEdgeSurfaces > 0) {
            softEdges = vita_softAlphaEdges.GetFloat();
            r_vitaAlphaEdgeFrames++;
        }
    } else {
        r_vitaAlphaEdgeSurfaces = 0;
    }

    globalImages->currentRenderImage->CopyFramebuffer(viewport.x1, viewport.y1, viewport.GetWidth(), viewport.GetHeight());

    GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO | GLS_DEPTHMASK | GLS_DEPTHFUNC_ALWAYS);
    GL_Cull(CT_TWO_SIDED);

    int screenWidth = renderSystem->GetWidth();
    int screenHeight = renderSystem->GetHeight();

    // set the window clipping
    GL_Viewport(0, 0, screenWidth, screenHeight);
    GL_Scissor(0, 0, screenWidth, screenHeight);

    GL_SelectTexture(0);
    globalImages->currentRenderImage->Bind();
    if (r_vitaPostFilterBuilt && !vitaFilter) {
        renderProgManager.BindShader_PostProcessPlain();
    } else {
        renderProgManager.BindShader_PostProcess();
    }
    const float copyW = (float)Max(1, globalImages->currentRenderImage->GetUploadWidth());
    const float copyH = (float)Max(1, globalImages->currentRenderImage->GetUploadHeight());
    if (vitaFilter) {
        // the sharpening and soft edges' constants (RenderProgs_GLSL.cpp):
        // the strength grows with the stretch - x1 at 720 wide, x1.32 at 544
        float strength = vita_sharpen.GetFloat() * Max(1.0f, (screenWidth / (float)Max(1, (int)viewport.GetWidth())) * (720.0f / 960.0f));
        if (RB_VitaOnTV() && !vita_sharpenOnTV.GetBool()) {
            strength = 0.0f;
        }
        const float clampTo = vita_sharpenClamp.GetFloat();
        const float k = strength * 1.5f * (0.5f / clampTo); // LumaSharpen's fast pattern x1.5, its clamp folded in
        const idVec4 p0(1.0f / copyW, 1.0f / copyH, 2.0f * clampTo, -clampTo);
        const idVec4 p1(0.2126f * k, 0.7152f * k, 0.0722f * k, 0.0f);
        const idVec4 p2(softEdges, vita_softAlphaEdgesShow.GetBool() ? 1.0f : 0.0f, 0.0f, 0.0f);
        SetFragmentParm(RENDERPARM_TEXGEN_0_S, p0.ToFloatPtr());
        SetFragmentParm(RENDERPARM_TEXGEN_0_T, p1.ToFloatPtr());
        SetFragmentParm(RENDERPARM_TEXGEN_0_Q, p2.ToFloatPtr());
        static int logged = 0;
        if (logged < 2) {
            logged++;
            common->Printf("vita: stretch %dx%d -> %dx%d: sharpening %.2f (clamp %.3f%s), soft alpha-tested edges %.2f (%d surfaces in the map)\n",
                viewport.GetWidth(), viewport.GetHeight(), screenWidth, screenHeight, strength, clampTo,
                RB_VitaOnTV() ? (vita_sharpenOnTV.GetBool() ? ", PS TV" : ", PS TV: off") : "", softEdges, r_vitaAlphaEdgeSurfaces);
        }
    } else {
        // FXAA's constants (unused by the plain stretch): the copy's texel size,
        // N, the edge sharpness, the threshold and its minimum
        const idVec4 fxaa0(1.0f / copyW, 1.0f / copyH, vita_fxaaN.GetFloat(), vita_fxaaSharpness.GetFloat());
        const idVec4 fxaa1(vita_fxaaThreshold.GetFloat(), vita_fxaaThresholdMin.GetFloat(), 0.0f, 0.0f);
        SetFragmentParm(RENDERPARM_TEXGEN_0_S, fxaa0.ToFloatPtr());
        SetFragmentParm(RENDERPARM_TEXGEN_0_T, fxaa1.ToFloatPtr());
    }

    // Draw
    RB_DrawElementsWithCounters(&backEnd.unitSquareSurface);

    renderLog.CloseBlock();
}
