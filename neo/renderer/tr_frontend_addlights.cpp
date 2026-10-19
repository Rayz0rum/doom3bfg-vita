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
#include "../sys/vita/vita_zones.h" // GD_SCOPE / GD_PASS: the game/draw thread's tree of passes (VitaBench.cpp)

extern idCVar r_useAreasConnectedForShadowCulling;
extern idCVar r_useParallelAddShadows;
extern idCVar r_forceShadowCaps;
extern idCVar r_useShadowPreciseInsideTest;
#if defined(__vita__)
// A pre-light shadow always draws its caps, so the precise inside test only
// picks the stencil mode: Z-fail is right whether or not the view is inside,
// Z-pass only when it is not, and both draw the same triangles. The test
// itself walks every vertex and triangle of the shadow (tens of thousands in
// the big rooms): the benchmark caught 134 pre-light jobs over 3 ms in one
// EnPro fight, for a result the GPU doesn't need. Off: Z-fail whenever the
// view is near enough to the shadow to be inside it.
idCVar vita_prelightInsideTest("vita_prelightInsideTest", "0", CVAR_RENDERER | CVAR_BOOL, "Vita: run the precise view-inside-shadow test for pre-light shadows too (it only chooses between Z-pass and Z-fail, which draw the same)");
#endif

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
// Vita: how far over budget the frames have been lately, whatever held them
// back (0 = holding 30 fps; common_frame.cpp steps it, up to 6): the light
// budget below tightens with it
int vita_loadStep = 0;
// Vita: the most lights with something to draw a view keeps (0 = no cap);
// common_frame.cpp lowers it while the render back end runs over budget and
// gives it back when the back end has time to spare
int vita_viewLightCap = 0;
int vita_viewLitLightsLast = 0; // lights with something to draw, last view
idCVar vita_lightCapAuto("vita_lightCapAuto", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "Vita: while the render back end runs over budget, the smallest lights on screen are dropped until it fits (the player's own lights, fog and blend lights stay)");
idCVar vita_lightMinScreenAreaPerLoadStep("vita_lightMinScreenAreaPerLoadStep", "0.002", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "Vita: added to vita_lightMinScreenArea for each load step (frames over budget lately: 0-6), so more small lights drop while 30 fps is not held");
idCVar vita_lightMinScreenArea("vita_lightMinScreenArea", "0.0025", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "Vita: lights covering less than this fraction of the view are dropped for the frame (the player's own lights always stay); 0 = keep all");
#endif
idCVar r_useAreasConnectedForShadowCulling("r_useAreasConnectedForShadowCulling", "2", CVAR_RENDERER | CVAR_INTEGER, "cull entities cut off by doors");
idCVar r_useParallelAddLights("r_useParallelAddLights", "1", CVAR_RENDERER | CVAR_BOOL, "aadd all lights in parallel with jobs");

/*
============================
R_ShadowBounds

Even though the extruded shadows are drawn projected to infinity, their effects are limited
to a fraction of the light's volume.  An extruded box would require 12 faces to specify and
be a lot of trouble, but an axial bounding box is quick and easy to determine.

If the light is completely contained in the view, there is no value in trying to cull the
shadows, as they will all pass.

Pure function.
============================
*/
void R_ShadowBounds(const idBounds& modelBounds, const idBounds& lightBounds, const idVec3& lightOrigin, idBounds& shadowBounds)
{
    for (int i = 0; i < 3; i++) {
        shadowBounds[0][i] = __fsels(modelBounds[0][i] - lightOrigin[i], modelBounds[0][i], lightBounds[0][i]);
        shadowBounds[1][i] = __fsels(lightOrigin[i] - modelBounds[1][i], modelBounds[1][i], lightBounds[1][i]);
    }
}

/*
============================
idRenderEntityLocal::IsDirectlyVisible()
============================
*/
bool idRenderEntityLocal::IsDirectlyVisible() const
{
    if (viewCount != tr.viewCount) {
        return false;
    }
    if (viewEntity->scissorRect.IsEmpty()) {
        // a viewEntity was created for shadow generation, but the
        // model global reference bounds isn't directly visible
        return false;
    }
    return true;
}

/*
===================
R_AddSingleLight

May be run in parallel.

Sets vLight->removeFromList to true if the light should be removed from the list.
Builds a chain of entities that need to be added for shadows only off vLight->shadowOnlyViewEntities.
Allocates and fills in vLight->entityInteractionState.

Calc the light shader values, removing any light from the viewLight list
if it is determined to not have any visible effect due to being flashed off or turned off.

Add any precomputed shadow volumes.
===================
*/
static void R_AddSingleLight(viewLight_t* vLight)
{
    // until proven otherwise
    vLight->removeFromList = true;
    vLight->shadowOnlyViewEntities = NULL;
    vLight->preLightShadowVolumes = NULL;

    // globals we really should pass in...
    const viewDef_t* viewDef = tr.viewDef;

    const idRenderLightLocal* light = vLight->lightDef;
    const idMaterial* lightShader = light->lightShader;
    if (lightShader == NULL) {
        common->Error("R_AddSingleLight: NULL lightShader");
        return;
    }

    SCOPED_PROFILE_EVENT(lightShader->GetName());

    // see if we are suppressing the light in this view
    if (!r_skipSuppress.GetBool()) {
        if (light->parms.suppressLightInViewID && light->parms.suppressLightInViewID == viewDef->renderView.viewID) {
            return;
        }
        if (light->parms.allowLightInViewID && light->parms.allowLightInViewID != viewDef->renderView.viewID) {
            return;
        }
    }

    // evaluate the light shader registers
    VITA_ZONE_BEGIN(lightSetup, "light job: shader registers + scissor");
    float* lightRegs = (float*)R_FrameAlloc(lightShader->GetNumRegisters() * sizeof(float), FRAME_ALLOC_SHADER_REGISTER);
    {
        VITA_ZONE("light job: shader registers (EvaluateRegisters)");
        lightShader->EvaluateRegisters(lightRegs, light->parms.shaderParms, viewDef->renderView.shaderParms,
            tr.viewDef->renderView.time[0] * 0.001f, light->parms.referenceSound);
    }

    // if this is a purely additive light and no stage in the light shader evaluates
    // to a positive light value, we can completely skip the light
    if (!lightShader->IsFogLight() && !lightShader->IsBlendLight()) {
        int lightStageNum;
        for (lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++) {
            const shaderStage_t* lightStage = lightShader->GetStage(lightStageNum);

            // ignore stages that fail the condition
            if (!lightRegs[lightStage->conditionRegister]) {
                continue;
            }

            const int* registers = lightStage->color.registers;

            // snap tiny values to zero
            if (lightRegs[registers[0]] < 0.001f) {
                lightRegs[registers[0]] = 0.0f;
            }
            if (lightRegs[registers[1]] < 0.001f) {
                lightRegs[registers[1]] = 0.0f;
            }
            if (lightRegs[registers[2]] < 0.001f) {
                lightRegs[registers[2]] = 0.0f;
            }

            if (lightRegs[registers[0]] > 0.0f || lightRegs[registers[1]] > 0.0f || lightRegs[registers[2]] > 0.0f) {
                break;
            }
        }
        if (lightStageNum == lightShader->GetNumStages()) {
            // we went through all the stages and didn't find one that adds anything
            // remove the light from the viewLights list, and change its frame marker
            // so interaction generation doesn't think the light is visible and
            // create a shadow for it
            VITA_ZONE_END(lightSetup);
            return;
        }
    }

    //--------------------------------------------
    // copy data used by backend
    //--------------------------------------------
    vLight->globalLightOrigin = light->globalLightOrigin;
    vLight->lightProject[0] = light->lightProject[0];
    vLight->lightProject[1] = light->lightProject[1];
    vLight->lightProject[2] = light->lightProject[2];
    vLight->lightProject[3] = light->lightProject[3];

    // the fog plane is the light far clip plane
    idPlane fogPlane(light->baseLightProject[2][0] - light->baseLightProject[3][0],
        light->baseLightProject[2][1] - light->baseLightProject[3][1],
        light->baseLightProject[2][2] - light->baseLightProject[3][2],
        light->baseLightProject[2][3] - light->baseLightProject[3][3]);
    const float planeScale = idMath::InvSqrt(fogPlane.Normal().LengthSqr());
    vLight->fogPlane[0] = fogPlane[0] * planeScale;
    vLight->fogPlane[1] = fogPlane[1] * planeScale;
    vLight->fogPlane[2] = fogPlane[2] * planeScale;
    vLight->fogPlane[3] = fogPlane[3] * planeScale;

    // copy the matrix for deforming the 'zeroOneCubeModel' to exactly cover the light volume in world space
    vLight->inverseBaseLightProject = light->inverseBaseLightProject;

    vLight->falloffImage = light->falloffImage;
    vLight->lightShader = light->lightShader;
    vLight->shaderRegisters = lightRegs;

    if (r_useLightScissors.GetInteger() != 0) {
        // Calculate the matrix that projects the zero-to-one cube to exactly cover the
        // light frustum in clip space.
        idRenderMatrix invProjectMVPMatrix;
        idRenderMatrix::Multiply(viewDef->worldSpace.mvp, light->inverseBaseLightProject, invProjectMVPMatrix);

        // Calculate the projected bounds, either not clipped at all, near clipped, or fully clipped.
        idBounds projected;
        if (r_useLightScissors.GetInteger() == 1) {
            idRenderMatrix::ProjectedBounds(projected, invProjectMVPMatrix, bounds_zeroOneCube);
        } else if (r_useLightScissors.GetInteger() == 2) {
            idRenderMatrix::ProjectedNearClippedBounds(projected, invProjectMVPMatrix, bounds_zeroOneCube);
        } else {
            VITA_ZONE("light job: scissor bounds (fully clipped, r_useLightScissors 3)");
            idRenderMatrix::ProjectedFullyClippedBounds(projected, invProjectMVPMatrix, bounds_zeroOneCube);
        }

        if (projected[0][2] >= projected[1][2]) {
            // the light was culled to the view frustum
            VITA_ZONE_END(lightSetup);
            return;
        }

        float screenWidth = (float)viewDef->viewport.x2 - (float)viewDef->viewport.x1;
        float screenHeight = (float)viewDef->viewport.y2 - (float)viewDef->viewport.y1;

        idScreenRect lightScissorRect;
        lightScissorRect.x1 = idMath::Ftoi(projected[0][0] * screenWidth);
        lightScissorRect.x2 = idMath::Ftoi(projected[1][0] * screenWidth);
        lightScissorRect.y1 = idMath::Ftoi(projected[0][1] * screenHeight);
        lightScissorRect.y2 = idMath::Ftoi(projected[1][1] * screenHeight);
        lightScissorRect.Expand();

        vLight->scissorRect.Intersect(lightScissorRect);
        vLight->scissorRect.zmin = projected[0][2];
        vLight->scissorRect.zmax = projected[1][2];

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
        // Vita (a cut): a light covering less than vita_lightMinScreenArea
        // (a fraction of the view) is dropped this frame - its light job,
        // its interactions and shadows, its GPU passes. Lights the player
        // carries (flashlight, muzzle flashes: allowLightInViewID) stay.
        // Under load (vita_loadStep) the bar rises by
        // vita_lightMinScreenAreaPerLoadStep a step.
        extern bool vita_fullQuality;
        const float baseArea = vita_fullQuality ? 0.0f : vita_lightMinScreenArea.GetFloat();
        const float minArea = (baseArea > 0.0f) ? baseArea + vita_loadStep * vita_lightMinScreenAreaPerLoadStep.GetFloat() : 0.0f;
        // With hysteresis: a light on screen is only dropped once it is well
        // under the bar (60% of it), one dropped comes back at the bar - a
        // light at the bar no longer blinks on and off as the view moves.
        bool vitaSmallKeep = true;
        if (minArea > 0.0f && light->parms.allowLightInViewID == 0 && !lightShader->IsFogLight() && !lightShader->IsBlendLight()) {
            const float area = (float)vLight->scissorRect.GetArea();
            const float bar = (light->vitaSmallDropped ? 1.0f : 0.6f) * minArea * screenWidth * screenHeight;
            vitaSmallKeep = area >= bar;
            if (!viewDef->isSubview) {
                const_cast<idRenderLightLocal*>(light)->vitaSmallDropped = !vitaSmallKeep;
            }
        }
        if (!vitaSmallKeep) {
            const float area = (float)vLight->scissorRect.GetArea();
            {
                if (area < baseArea * screenWidth * screenHeight) {
                    VITA_ZONE_COUNT("lights dropped: too small on screen (vita_lightMinScreenArea)");
                } else {
                    VITA_ZONE_COUNT("lights dropped: small on screen while over budget (vita_loadStep x vita_lightMinScreenAreaPerLoadStep)");
                }
                VITA_ZONE_END(lightSetup);
                return;
            }
        }
#endif
    }

    // this one stays on the list
    vLight->removeFromList = false;
    VITA_ZONE_END(lightSetup);
    VITA_ZONE_BEGIN(lightEntities, "light job: entities in the light's areas (interactions, shadow-only entities)");
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    int vitaRefsStaticEmptySlow = 0, vitaRefsWeaponLight = 0, vitaRefsNoShadowHidden = 0, vitaRefsNoSurfaces = 0;
    int vitaEntityRefs = 0, vitaRefsSeen = 0, vitaRefsStaticEmpty = 0, vitaRefsCullTested = 0, vitaRefsVisible = 0, vitaRefsShadowTested = 0, vitaRefsShadowOnly = 0;
#endif

    //--------------------------------------------
    // create interactions with all entities the light may touch, and add viewEntities
    // that may cast shadows, even if they aren't directly visible.  Any real work
    // will be deferred until we walk through the viewEntities
    //--------------------------------------------
    const int renderViewID = viewDef->renderView.viewID;

    // this bool array will be set true whenever the entity will visibly interact with the light
    {
        // (Vita: timed and sized - one byte per entity slot of the map, cleared for every light)
        VITA_ZONE("light job: entity state array cleared (one byte per entity slot)");
        VITA_ZONE_ADD("light job: entity state bytes cleared", light->world->entityDefs.Num());
        vLight->entityInteractionState = (byte*)R_ClearedFrameAlloc(light->world->entityDefs.Num() * sizeof(vLight->entityInteractionState[0]), FRAME_ALLOC_INTERACTION_STATE);
    }

    const bool lightCastsShadows = light->LightCastsShadows();
    idInteraction** const interactionTableRow = light->world->interactionTable[light->index];

    for (areaReference_t* lref = light->references; lref != NULL; lref = lref->ownerNext) {
        portalArea_t* area = lref->area;

        // some lights have their center of projection outside the world, but otherwise
        // we want to ignore areas that are not connected to the light center due to a closed door
        if (light->areaNum != -1 && r_useAreasConnectedForShadowCulling.GetInteger() == 2) {
            if (!light->world->AreasAreConnected(light->areaNum, area->areaNum, PS_BLOCK_VIEW)) {
                // can't possibly be seen or shadowed
                continue;
            }
        }

        // check all the models in this area
        VITA_ZONE_SAMPLED("light job: one area's entity references (sampled)", ((uintptr_t)area >> 4) + tr.frameCount + light->index, 3);
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
        // Vita: the area's packed array (R_VitaPackAreaRefs, made before the
        // light jobs) in list order, instead of the linked references - a
        // walk of scattered nodes, ~1 us a reference (~3,800 a frame in a
        // fight). The checks most references end at read only the packed
        // entry: already checked through another area, a static model the
        // light statically has no interaction with, a model with no lit or
        // shadowing surfaces. The same tests on the same values, in the same
        // order; the rest of the checks are unchanged.
        const vitaAreaEntityRef_t* vitaPacked = area->vitaRefs;
        const int vitaNumPacked = area->vitaNumRefs;
        for (int vitaPackedNum = 0; vitaPackedNum < vitaNumPacked; vitaPackedNum++) {
            const vitaAreaEntityRef_t& vr = vitaPacked[vitaPackedNum];
            vitaEntityRefs++;
            {
                const int entityIndex = vr.index;
                if (vLight->entityInteractionState[entityIndex] != viewLight_t::INTERACTION_UNCHECKED) {
                    vitaRefsSeen++;
                    continue;
                }
                if (vr.staticModel && interactionTableRow[entityIndex] == INTERACTION_EMPTY) {
                    vLight->entityInteractionState[entityIndex] = viewLight_t::INTERACTION_NO;
                    vitaRefsStaticEmpty++;
                    continue;
                }
                // a model with neither lit nor shadow-casting surfaces ends at
                // the "doesn't accept lighting or cast shadows" check below;
                // every check before that one ends the same way (state NO,
                // next reference), so deciding it first changes nothing
                if (vr.noSurfaces) {
                    vLight->entityInteractionState[entityIndex] = viewLight_t::INTERACTION_NO;
                    vitaRefsNoSurfaces++;
                    continue;
                }
            }
            idRenderEntityLocal* edef = vr.edef;
#else
        for (areaReference_t* eref = area->entityRefs.areaNext; eref != &area->entityRefs; eref = eref->areaNext) {
            idRenderEntityLocal* edef = eref->entity;
#endif

            if (vLight->entityInteractionState[edef->index] != viewLight_t::INTERACTION_UNCHECKED) {
                continue;
            }
            // until proven otherwise
            vLight->entityInteractionState[edef->index] = viewLight_t::INTERACTION_NO;

            // The table is updated at interaction::AllocAndLink() and interaction::UnlinkAndFree()
            const idInteraction* inter = interactionTableRow[edef->index];

            const renderEntity_t& eParms = edef->parms;
            const idRenderModel* eModel = eParms.hModel;

            // a large fraction of static entity / light pairs will still have no interactions even though
            // they are both present in the same area(s)
            if (eModel != NULL && !eModel->IsDynamicModel() && inter == INTERACTION_EMPTY) {
                // the interaction was statically checked, and it didn't generate any surfaces,
                // so there is no need to force the entity onto the view list if it isn't
                // already there
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsStaticEmptySlow++;
#endif
                continue;
            }

            // We don't want the lights on weapons to illuminate anything else.
            // There are two assumptions here -- that allowLightInViewID is only
            // used for weapon lights, and that all weapons will have weaponDepthHack.
            // A more general solution would be to have an allowLightOnEntityID field.
            // HACK: the armor-mounted flashlight is a private spot light, which is probably
            // wrong -- you would expect to see them in multiplayer.
            if (light->parms.allowLightInViewID && light->parms.pointLight && !eParms.weaponDepthHack) {
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsWeaponLight++;
#endif
                continue;
            }

            // non-shadow casting entities don't need to be added if they aren't
            // directly visible
            if ((eParms.noShadow || (eModel && !eModel->ModelHasShadowCastingSurfaces())) && !edef->IsDirectlyVisible()) {
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsNoShadowHidden++;
#endif
                continue;
            }

            // if the model doesn't accept lighting or cast shadows, it doesn't need to be added
            if (eModel && !eModel->ModelHasInteractingSurfaces() && !eModel->ModelHasShadowCastingSurfaces()) {
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsNoSurfaces++;
#endif
                continue;
            }

            // no interaction present, so either the light or entity has moved
            // assert( lightHasMoved || edef->entityHasMoved );
            if (inter == NULL) {
                // some big outdoor meshes are flagged to not create any dynamic interactions
                // when the level designer knows that nearby moving lights shouldn't actually hit them
                if (eParms.noDynamicInteractions) {
                    continue;
                }

                // do a check of the entity reference bounds against the light frustum to see if they can't
                // possibly interact, despite sharing one or more world areas
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsCullTested++;
#endif
                if (R_CullModelBoundsToLight(light, edef->localReferenceBounds, edef->modelRenderMatrix)) {
                    continue;
                }
            }

            // we now know that the entity and light do overlap

            if (edef->IsDirectlyVisible()) {
                // entity is directly visible, so the interaction is definitely needed
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
                vitaRefsVisible++;
#endif
                vLight->entityInteractionState[edef->index] = viewLight_t::INTERACTION_YES;
                continue;
            }

            // the entity is not directly visible, but if we can tell that it may cast
            // shadows onto visible surfaces, we must make a viewEntity for it
            if (!lightCastsShadows) {
                // surfaces are never shadowed in this light
                continue;
            }
            // if we are suppressing its shadow in this view (player shadows, etc), skip
            if (!r_skipSuppress.GetBool()) {
                if (eParms.suppressShadowInViewID && eParms.suppressShadowInViewID == renderViewID) {
                    continue;
                }
                if (eParms.suppressShadowInLightID && eParms.suppressShadowInLightID == light->parms.lightId) {
                    continue;
                }
            }

            // should we use the shadow bounds from pre-calculated interactions?
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
            vitaRefsShadowTested++;
#endif
            idBounds shadowBounds;
            R_ShadowBounds(edef->globalReferenceBounds, light->globalLightBounds, light->globalLightOrigin, shadowBounds);

            // this test is pointless if we knew the light was completely contained
            // in the view frustum, but the entity would also be directly visible in most
            // of those cases.

            // this doesn't say that the shadow can't effect anything, only that it can't
            // effect anything in the view, so we shouldn't set up a view entity
            if (idRenderMatrix::CullBoundsToMVP(viewDef->worldSpace.mvp, shadowBounds)) {
                continue;
            }

            // debug tool to allow viewing of only one entity at a time
            if (r_singleEntity.GetInteger() >= 0 && r_singleEntity.GetInteger() != edef->index) {
                continue;
            }

            // we do need it for shadows
            vLight->entityInteractionState[edef->index] = viewLight_t::INTERACTION_YES;

            // we will need to create a viewEntity_t for it in the serial code section
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
            vitaRefsShadowOnly++;
#endif
            shadowOnlyEntity_t* shadEnt = (shadowOnlyEntity_t*)R_FrameAlloc(sizeof(shadowOnlyEntity_t), FRAME_ALLOC_SHADOW_ONLY_ENTITY);
            shadEnt->next = vLight->shadowOnlyViewEntities;
            shadEnt->edef = edef;
            vLight->shadowOnlyViewEntities = shadEnt;
        }
    }

    VITA_ZONE_END(lightEntities);
    VITA_ZONE_ADD("light job: entity references walked", vitaEntityRefs);
    VITA_ZONE_ADD("light job: references to an entity already checked (another area)", vitaRefsSeen);
    VITA_ZONE_ADD("light job: references to a static model with no interaction (no entity read)", vitaRefsStaticEmpty);
    VITA_ZONE_ADD("light job: references culled against the light (entity or light moved)", vitaRefsCullTested);
    VITA_ZONE_ADD("light job: references to a directly visible entity (interaction kept)", vitaRefsVisible);
    VITA_ZONE_ADD("light job: references given the shadow bounds test (entity not visible)", vitaRefsShadowTested);
    VITA_ZONE_ADD("light job: shadow-only entities added", vitaRefsShadowOnly);
    VITA_ZONE_ADD("light job: references to a static model with no interaction (after the entity read: callback models)", vitaRefsStaticEmptySlow);
    VITA_ZONE_ADD("light job: references skipped for a weapon light", vitaRefsWeaponLight);
    VITA_ZONE_ADD("light job: references to a hidden entity casting no shadow", vitaRefsNoShadowHidden);
    VITA_ZONE_ADD("light job: references to a model with no lit or shadowing surfaces", vitaRefsNoSurfaces);

    //--------------------------------------------
    // add the prelight shadows for the static world geometry
    //--------------------------------------------
    if (light->parms.prelightModel != NULL) {
        srfTriangles_t* tri = light->parms.prelightModel->Surface(0)->geometry;

        // these shadows will have valid bounds, and can be culled normally,
        // but they will typically cover most of the light's bounds
        if (idRenderMatrix::CullBoundsToMVP(viewDef->worldSpace.mvp, tri->bounds)) {
            return;
        }

        // prelight models should always have static data that never gets purged
        assert(vertexCache.CacheIsCurrent(tri->shadowCache));
        assert(vertexCache.CacheIsCurrent(tri->indexCache));

        drawSurf_t* shadowDrawSurf = (drawSurf_t*)R_FrameAlloc(sizeof(*shadowDrawSurf), FRAME_ALLOC_DRAW_SURFACE);

        shadowDrawSurf->frontEndGeo = tri;
        shadowDrawSurf->ambientCache = 0;
        shadowDrawSurf->indexCache = tri->indexCache;
        shadowDrawSurf->shadowCache = tri->shadowCache;
        shadowDrawSurf->jointCache = 0;
        shadowDrawSurf->numIndexes = 0;
        shadowDrawSurf->space = &viewDef->worldSpace;
        shadowDrawSurf->material = NULL;
        shadowDrawSurf->extraGLState = 0;
        shadowDrawSurf->shaderRegisters = NULL;
        shadowDrawSurf->scissorRect = vLight->scissorRect;     // default to the light scissor and light depth bounds
        shadowDrawSurf->shadowVolumeState = SHADOWVOLUME_DONE; // assume the shadow volume is done in case r_skipPrelightShadows is set

        if (!r_skipPrelightShadows.GetBool()) {
            preLightShadowVolumeParms_t* shadowParms = (preLightShadowVolumeParms_t*)R_FrameAlloc(sizeof(shadowParms[0]), FRAME_ALLOC_SHADOW_VOLUME_PARMS);

            shadowParms->verts = tri->preLightShadowVertexes;
            shadowParms->numVerts = tri->numVerts * 2;
            shadowParms->indexes = tri->indexes;
            shadowParms->numIndexes = tri->numIndexes;
            shadowParms->triangleBounds = tri->bounds;
            shadowParms->triangleMVP = viewDef->worldSpace.mvp;
            shadowParms->localLightOrigin = vLight->globalLightOrigin;
            shadowParms->localViewOrigin = viewDef->renderView.vieworg;
            shadowParms->zNear = r_znear.GetFloat();
            shadowParms->lightZMin = vLight->scissorRect.zmin;
            shadowParms->lightZMax = vLight->scissorRect.zmax;
            shadowParms->forceShadowCaps = r_forceShadowCaps.GetBool();
#if defined(__vita__)
            shadowParms->useShadowPreciseInsideTest = r_useShadowPreciseInsideTest.GetBool() && vita_prelightInsideTest.GetBool();
#else
            shadowParms->useShadowPreciseInsideTest = r_useShadowPreciseInsideTest.GetBool();
#endif
            shadowParms->useShadowDepthBounds = r_useShadowDepthBounds.GetBool();
            shadowParms->numShadowIndices = &shadowDrawSurf->numIndexes;
            shadowParms->renderZFail = &shadowDrawSurf->renderZFail;
            shadowParms->shadowZMin = &shadowDrawSurf->scissorRect.zmin;
            shadowParms->shadowZMax = &shadowDrawSurf->scissorRect.zmax;
            shadowParms->shadowVolumeState = &shadowDrawSurf->shadowVolumeState;

            // the pre-light shadow volume "_prelight_light_3297" in "d3xpdm2" is malformed in that it contains the light origin so the precise inside test always fails
            if (tr.primaryWorld->mapName.IcmpPath("maps/game/mp/d3xpdm2.map") == 0 && idStr::Icmp(light->parms.prelightModel->Name(), "_prelight_light_3297") == 0) {
                shadowParms->useShadowPreciseInsideTest = false;
            }

            shadowDrawSurf->shadowVolumeState = SHADOWVOLUME_UNFINISHED;

            shadowParms->next = vLight->preLightShadowVolumes;
            vLight->preLightShadowVolumes = shadowParms;
        }

        // actually link it in
        shadowDrawSurf->nextOnLight = vLight->globalShadows;
        vLight->globalShadows = shadowDrawSurf;
    }
}

REGISTER_PARALLEL_JOB(R_AddSingleLight, "R_AddSingleLight");

/*
=================
R_AddLights
=================
*/
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
/*
===================
R_VitaPackAreaRefs

Before the light jobs (they read the arrays in parallel): every area a light
of the view touches whose entity list changed since gets its packed array
remade, in list order.
===================
*/
static void R_VitaPackAreaRefs()
{
    VITA_ZONE("light jobs: pack the areas' entity references (changed areas)");
    int packed = 0, entries = 0;
    for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        for (areaReference_t* lref = vLight->lightDef->references; lref != NULL; lref = lref->ownerNext) {
            portalArea_t* area = lref->area;
            if (!area->vitaRefsDirty && area->vitaRefs != NULL) {
                continue;
            }
            int n = 0;
            for (areaReference_t* eref = area->entityRefs.areaNext; eref != &area->entityRefs; eref = eref->areaNext) {
                n++;
            }
            if (n > area->vitaRefsAlloced) {
                if (area->vitaRefs != NULL) {
                    Mem_Free(area->vitaRefs);
                }
                area->vitaRefsAlloced = n + 16;
                area->vitaRefs = (vitaAreaEntityRef_t*)Mem_Alloc(area->vitaRefsAlloced * sizeof(vitaAreaEntityRef_t), TAG_RENDER);
            } else if (area->vitaRefs == NULL) {
                area->vitaRefsAlloced = 16;
                area->vitaRefs = (vitaAreaEntityRef_t*)Mem_Alloc(area->vitaRefsAlloced * sizeof(vitaAreaEntityRef_t), TAG_RENDER);
            }
            int i = 0;
            for (areaReference_t* eref = area->entityRefs.areaNext; eref != &area->entityRefs; eref = eref->areaNext, i++) {
                vitaAreaEntityRef_t& e = area->vitaRefs[i];
                e.edef = eref->entity;
                e.index = eref->vitaEntityIndex;
                e.staticModel = eref->vitaStaticModel;
                e.noSurfaces = eref->vitaNoSurfaces;
            }
            area->vitaNumRefs = n;
            area->vitaRefsDirty = false;
            packed++;
            entries += n;
        }
    }
    VITA_ZONE_ADD("light jobs: areas packed (their entity list changed)", packed);
    VITA_ZONE_ADD("light jobs: entity references packed", entries);
}
#endif

void R_AddLights()
{
    SCOPED_PROFILE_EVENT("R_AddLights");
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    R_VitaPackAreaRefs();
#endif

    //-------------------------------------------------
    // check each light individually, possibly in parallel
    //-------------------------------------------------

    GD_PASS_BEGIN("light jobs (R_AddSingleLight per light)");
    if (r_useParallelAddLights.GetBool()) {
        for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
            tr.frontEndJobList->AddJob((jobRun_t)R_AddSingleLight, vLight);
        }
        tr.frontEndJobList->Submit();
        tr.frontEndJobList->Wait();
    } else {
        for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
            R_AddSingleLight(vLight);
        }
    }

    //-------------------------------------------------
    // cull lights from the list if they turned out to not be needed
    //-------------------------------------------------

    GD_PASS("light list, shadow-only entities, pre-light shadow job queue");
    tr.pc.c_viewLights = 0;
    viewLight_t** ptr = &tr.viewDef->viewLights;
    while (*ptr != NULL) {
        viewLight_t* vLight = *ptr;

        if (vLight->removeFromList) {
            vLight->lightDef->viewCount = -1; // this probably doesn't matter with current code
            *ptr = vLight->next;
            continue;
        }

        ptr = &vLight->next;

        // serial work
        tr.pc.c_viewLights++;

        for (shadowOnlyEntity_t* shadEnt = vLight->shadowOnlyViewEntities; shadEnt != NULL; shadEnt = shadEnt->next) {
            // this will add it to the viewEntities list, but with an empty scissor rect
            R_SetEntityDefViewEntity(shadEnt->edef);
        }

        if (r_showLightScissors.GetBool()) {
            R_ShowColoredScreenRect(vLight->scissorRect, vLight->lightDef->index);
        }
    }

    //-------------------------------------------------
    // Add jobs to setup pre-light shadow volumes.
    //-------------------------------------------------

    if (r_useParallelAddShadows.GetInteger() == 1) {
        for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
            for (preLightShadowVolumeParms_t* shadowParms = vLight->preLightShadowVolumes; shadowParms != NULL; shadowParms = shadowParms->next) {
                tr.frontEndJobList->AddJob((jobRun_t)PreLightShadowVolumeJob, shadowParms);
            }
            vLight->preLightShadowVolumes = NULL;
        }
    } else {
        int start = Sys_Microseconds();

        for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
            for (preLightShadowVolumeParms_t* shadowParms = vLight->preLightShadowVolumes; shadowParms != NULL; shadowParms = shadowParms->next) {
                PreLightShadowVolumeJob(shadowParms);
            }
            vLight->preLightShadowVolumes = NULL;
        }

        int end = Sys_Microseconds();
        backEnd.pc.shadowMicroSec += end - start;
    }
    GD_PASS_END();
}

/*
=====================
R_OptimizeViewLightsList
=====================
*/
void R_OptimizeViewLightsList()
{
    // go through each visible light
    int numViewLights = 0;
    for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        numViewLights++;
        // If the light didn't have any lit surfaces visible, there is no need to
        // draw any of the shadows.  We still keep the vLight for debugging draws.
        if (!vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions) {
            vLight->localShadows = NULL;
            vLight->globalShadows = NULL;
        }
    }

    if (r_useShadowSurfaceScissor.GetBool()) {
        // shrink the light scissor rect to only intersect the surfaces that will actually be drawn.
        // This doesn't seem to actually help, perhaps because the surface scissor
        // rects aren't actually the surface, but only the portal clippings.
        for (viewLight_t* vLight = tr.viewDef->viewLights; vLight; vLight = vLight->next) {
            drawSurf_t* surf;
            idScreenRect surfRect;

            if (!vLight->lightShader->LightCastsShadows()) {
                continue;
            }

            surfRect.Clear();

            for (surf = vLight->globalInteractions; surf != NULL; surf = surf->nextOnLight) {
                surfRect.Union(surf->scissorRect);
            }
            for (surf = vLight->localShadows; surf != NULL; surf = surf->nextOnLight) {
                surf->scissorRect.Intersect(surfRect);
            }

            for (surf = vLight->localInteractions; surf != NULL; surf = surf->nextOnLight) {
                surfRect.Union(surf->scissorRect);
            }
            for (surf = vLight->globalShadows; surf != NULL; surf = surf->nextOnLight) {
                surf->scissorRect.Intersect(surfRect);
            }

            for (surf = vLight->translucentInteractions; surf != NULL; surf = surf->nextOnLight) {
                surfRect.Union(surf->scissorRect);
            }

            vLight->scissorRect.Intersect(surfRect);
        }
    }

    // sort the viewLights list so the largest lights come first, which will reduce
    // the chance of GPU pipeline bubbles
    struct sortLight_t {
        viewLight_t* vLight;
        int screenArea;
        static int sort(const void* a, const void* b)
        {
            return ((sortLight_t*)a)->screenArea - ((sortLight_t*)b)->screenArea;
        }
    };
    sortLight_t* sortLights = (sortLight_t*)_alloca(sizeof(sortLight_t) * numViewLights);
    int numSortLightsFilled = 0;
    for (viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next) {
        sortLights[numSortLightsFilled].vLight = vLight;
        sortLights[numSortLightsFilled].screenArea = vLight->scissorRect.GetArea();
        numSortLightsFilled++;
    }

    qsort(sortLights, numSortLightsFilled, sizeof(sortLights[0]), sortLight_t::sort);

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    // Vita (a cut): the light cap (vita_viewLightCap, set from the back end's
    // time in common_frame.cpp). The lights with something to draw beyond
    // the cap - the smallest on screen first - are left out of the view:
    // their interactions and shadows never reach the back end, whose time
    // is draw calls (~20 us each; a light is ~30-40 of them in the fights).
    // The player's own lights, fog and blend lights always stay.
    int vitaLit = 0;
    for (int i = 0; i < numSortLightsFilled; i++) {
        const viewLight_t* vl = sortLights[i].vLight;
        if (vl->localInteractions || vl->globalInteractions || vl->translucentInteractions) {
            vitaLit++;
        }
    }
    if (!tr.viewDef->isSubview) {
        vita_viewLitLightsLast = vitaLit;
    }
    extern bool vita_fullQuality;
    int vitaToDrop = (vita_viewLightCap > 0 && !tr.viewDef->isSubview && !vita_fullQuality) ? vitaLit - vita_viewLightCap : 0;
    int vitaDropped = 0, vitaDrawsDropped = 0;
#endif

    // rebuild the linked list in order
    tr.viewDef->viewLights = NULL;
    for (int i = 0; i < numSortLightsFilled; i++) {
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
        // (ascending area: the smallest come first)
        if (vitaToDrop > 0) {
            viewLight_t* vl = sortLights[i].vLight;
            const bool lit = (vl->localInteractions || vl->globalInteractions || vl->translucentInteractions);
            if (lit && vl->lightDef->parms.allowLightInViewID == 0 && !vl->lightShader->IsFogLight() && !vl->lightShader->IsBlendLight()) {
                for (const drawSurf_t* s = vl->localInteractions; s != NULL; s = s->nextOnLight) {
                    vitaDrawsDropped++;
                }
                for (const drawSurf_t* s = vl->globalInteractions; s != NULL; s = s->nextOnLight) {
                    vitaDrawsDropped++;
                }
                for (const drawSurf_t* s = vl->translucentInteractions; s != NULL; s = s->nextOnLight) {
                    vitaDrawsDropped++;
                }
                for (const drawSurf_t* s = vl->localShadows; s != NULL; s = s->nextOnLight) {
                    vitaDrawsDropped++;
                }
                for (const drawSurf_t* s = vl->globalShadows; s != NULL; s = s->nextOnLight) {
                    vitaDrawsDropped++;
                }
                vitaToDrop--;
                vitaDropped++;
                continue;
            }
        }
#endif
        sortLights[i].vLight->next = tr.viewDef->viewLights;
        tr.viewDef->viewLights = sortLights[i].vLight;
    }
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    VITA_ZONE_ADD("lights dropped: over the light cap (render back end over budget)", vitaDropped);
    VITA_ZONE_ADD("light cap: draws (interactions + shadows) dropped with them", vitaDrawsDropped);
    VITA_ZONE_ADD("light cap: lights with something to draw (main view)", tr.viewDef->isSubview ? 0 : vitaLit);
#endif
}
