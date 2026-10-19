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

#ifndef __MODEL_LOCAL_H__
#define __MODEL_LOCAL_H__

/*
===============================================================================

        Static model

===============================================================================
*/

class idJointMat;
struct deformInfo_t;

class idRenderModelStatic : public idRenderModel {
public:
    // the inherited public interface
    static idRenderModel* Alloc();

    idRenderModelStatic();
    virtual ~idRenderModelStatic();

    virtual void InitFromFile(const char* fileName);
    virtual bool LoadBinaryModel(idFile* file, const ID_TIME_T sourceTimeStamp);
    virtual void WriteBinaryModel(idFile* file, ID_TIME_T* _timeStamp = NULL) const;
    virtual bool SupportsBinaryModel()
    {
        return true;
    }

    virtual void PartialInitFromFile(const char* fileName);
    virtual void PurgeModel();
    virtual void Reset() { };
    virtual void LoadModel();
    virtual bool IsLoaded();
    virtual void SetLevelLoadReferenced(bool referenced);
    virtual bool IsLevelLoadReferenced();
    virtual void TouchData();
    virtual void InitEmpty(const char* name);
    virtual void AddSurface(modelSurface_t surface);
    virtual void FinishSurfaces();
    virtual void FreeVertexCache();
    virtual const char* Name() const;
    virtual void Print() const;
    virtual void List() const;
    virtual int Memory() const;
    virtual ID_TIME_T Timestamp() const;
    virtual int NumSurfaces() const;
    virtual int NumBaseSurfaces() const;
    virtual const modelSurface_t* Surface(int surfaceNum) const;
    virtual srfTriangles_t* AllocSurfaceTriangles(int numVerts, int numIndexes) const;
    virtual void FreeSurfaceTriangles(srfTriangles_t* tris) const;
    virtual bool IsStaticWorldModel() const;
    virtual dynamicModel_t IsDynamicModel() const;
    virtual bool IsDefaultModel() const;
    virtual bool IsReloadable() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual int NumJoints() const;
    virtual const idMD5Joint* GetJoints() const;
    virtual jointHandle_t GetJointHandle(const char* name) const;
    virtual const char* GetJointName(jointHandle_t handle) const;
    virtual const idJointQuat* GetDefaultPose() const;
    virtual int NearestJoint(int surfaceNum, int a, int b, int c) const;
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;
    virtual void ReadFromDemoFile(class idDemoFile* f);
    virtual void WriteToDemoFile(class idDemoFile* f);
    virtual float DepthHack() const;

    virtual bool ModelHasDrawingSurfaces() const
    {
        return hasDrawingSurfaces;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return hasInteractingSurfaces;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return hasShadowCastingSurfaces;
    };

    void MakeDefaultModel();

    bool LoadASE(const char* fileName);
    bool LoadLWO(const char* fileName);
    bool LoadMA(const char* filename);

    bool ConvertASEToModelSurfaces(const struct aseModel_s* ase);
    bool ConvertLWOToModelSurfaces(const struct st_lwObject* lwo);
    bool ConvertMAToModelSurfaces(const struct maModel_s* ma);

    struct aseModel_s* ConvertLWOToASE(const struct st_lwObject* obj, const char* fileName);

    bool DeleteSurfaceWithId(int id);
    void DeleteSurfacesWithNegativeId();
    bool FindSurfaceWithId(int id, int& surfaceNum) const;

public:
    idList<modelSurface_t, TAG_MODEL> surfaces;
    idBounds bounds;
    int overlaysAdded;

    // when an md5 is instantiated, the inverted joints array is stored to allow GPU skinning
    int numInvertedJoints;
    idJointMat* jointsInverted;
    vertCacheHandle_t jointsInvertedBuffer;

protected:
    int lastModifiedFrame;
    int lastArchivedFrame;

    idStr name;
    bool isStaticWorldModel;
    bool defaulted;
    bool purged;              // eventually we will have dynamic reloading
    bool fastLoad;            // don't generate tangents and shadow data
    bool reloadable;          // if not, reloadModels won't check timestamp
    bool levelLoadReferenced; // for determining if it needs to be freed
    bool hasDrawingSurfaces;
    bool hasInteractingSurfaces;
    bool hasShadowCastingSurfaces;
    ID_TIME_T timeStamp;

    static idCVar r_mergeModelSurfaces; // combine model surfaces with the same material
    static idCVar r_slopVertex;         // merge xyz coordinates this far apart
    static idCVar r_slopTexCoord;       // merge texture coordinates this far apart
    static idCVar r_slopNormal;         // merge normals that dot less than this
};

/*
===============================================================================

        MD5 animated model

===============================================================================
*/

class idMD5Mesh {
    friend class idRenderModelMD5;

public:
    idMD5Mesh();
    ~idMD5Mesh();

    void ParseMesh(idLexer& parser, int numJoints, const idJointMat* joints);

    int NumVerts() const
    {
        return numVerts;
    }
    int NumTris() const
    {
        return numTris;
    }

    void UpdateSurface(const struct renderEntity_s* ent, const idJointMat* joints,
        const idJointMat* entJointsInverted, modelSurface_t* surf);
    void CalculateBounds(const idJointMat* entJoints, idBounds& bounds) const;
    int NearestJoint(int a, int b, int c) const;

private:
    const idMaterial* shader; // material applied to mesh
    int numVerts;             // number of vertices
    int numTris;              // number of triangles
    byte* meshJoints;         // the joints used by this mesh
    int numMeshJoints;        // number of mesh joints
    float maxJointVertDist;   // maximum distance a vertex is separated from a joint
    deformInfo_t* deformInfo; // used to create srfTriangles_t from base frames and new vertexes
    int surfaceNum;           // number of the static surface created for this mesh
#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    mutable float vitaWeightSumMax; // the largest sum of a vertex's skinning weights (bytes / 255, can top 1), < 0 not measured yet
    // render model traces: the triangles in groups, by the heaviest joint of
    // their first vertex, each with the joints its vertices hang on and how
    // far from those joints they reach (VitaBuildTraceGroups)
    void VitaBuildTraceGroups(const idJointMat* invertedDefaultPose) const;
    void VitaFreeTraceGroups() const;
    mutable const deformInfo_t* vitaGroupsOf; // the deformInfo they were built from, NULL not built
    mutable int vitaNumGroups;
    mutable struct vitaTraceGroup_t* vitaGroups;
    mutable byte* vitaGroupJoints;
    mutable triIndex_t* vitaGroupVerts;
    mutable triIndex_t* vitaGroupIndexes;
    mutable int* vitaGroupTriPos;
#endif
#if defined(__vita__)
    // the base vertices' unit normals and tangents for the skin, made the
    // first time the mesh is skinned (8 floats a vertex; Model_md5.cpp)
    const float* VitaUnitNormals() const;
    mutable float* vitaUnitNT;
#endif
};

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
struct localTrace_t; // tr_local.h
struct vitaTraceGroup_t {
    int firstJoint, numJoints;   // into vitaGroupJoints
    int firstVert, numVerts;     // into vitaGroupVerts: the mesh vertices its triangles use, each once
    int firstTri, numTris;       // into vitaGroupIndexes (three each) and vitaGroupTriPos
    float dist;                  // the farthest any of its vertices is from one of its joints, in the base pose
};
#endif

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
// Vita: an animated model's posed positions, for render model traces
// (idRenderModelMD5::VitaPoseForTrace, RenderWorld.cpp)
static const int VITA_POSED_MAX_MESHES = 32;
struct vitaPosedMesh_t {
    const idMaterial* shader;  // the mesh's own material, as the snapshot's surface holds it
    const triIndex_t* indexes;
    int numIndexes;
    int numVerts;
    int firstVert;             // into vitaPosedModel_t::xyz
    int mesh;                  // the mesh's index in the model
    bool reachValid;           // false: no joints to bound it by (never skipped)
    idBounds reach;            // holds every position the mesh can have in this pose
    int firstGroup;            // its trace groups, into vitaPosedModel_t::groups
    int numGroups;
};
struct vitaPosedGroup_t {
    bool posed;                // its vertices' positions are in xyz (VitaTraceMesh)
    bool reachValid;           // false: no joints to bound it by (never skipped)
    idBounds reach;           // holds every position its vertices can have in this pose, known before posing them
    idBounds bounds;           // of its posed positions (once posed)
};
struct vitaPosedModel_t {
    int numMeshes;
    vitaPosedMesh_t meshes[VITA_POSED_MAX_MESHES];
    idVec3* xyz;
    int maxVerts;
    idJointMat* joints;        // the pose's skinning matrices (the snapshot's jointsInverted)
    int maxJoints;
    vitaPosedGroup_t* groups;
    int maxGroups;
};
#endif

class idRenderModelMD5 : public idRenderModelStatic {
public:
    virtual void InitFromFile(const char* fileName);
    virtual bool LoadBinaryModel(idFile* file, const ID_TIME_T sourceTimeStamp);
    virtual void WriteBinaryModel(idFile* file, ID_TIME_T* _timeStamp = NULL) const;
    virtual dynamicModel_t IsDynamicModel() const;
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;
    virtual void Print() const;
    virtual void List() const;
    virtual void TouchData();
    virtual void PurgeModel();
    virtual void LoadModel();
    virtual int Memory() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual int NumJoints() const;
    virtual const idMD5Joint* GetJoints() const;
    virtual jointHandle_t GetJointHandle(const char* name) const;
    virtual const char* GetJointName(jointHandle_t handle) const;
    virtual const idJointQuat* GetDefaultPose() const;
    virtual int NearestJoint(int surfaceNum, int a, int b, int c) const;

    virtual bool SupportsBinaryModel()
    {
        return true;
    }

#if defined(__vita__) || defined(ID_VITA_PHYSCHECK)
    bool VitaPoseForTrace(const struct renderEntity_s* ent, vitaPosedModel_t& posed) const;
    localTrace_t VitaTraceMesh(vitaPosedModel_t& posed, int posedMesh, const idVec3& localStart, const idVec3& localEnd, float radius) const;
    int VitaMeshNearestJoint(int mesh, int a, int b, int c) const;
#endif

private:
    idList<idMD5Joint, TAG_MODEL> joints;
    idList<idJointQuat, TAG_MODEL> defaultPose;
    idList<idJointMat, TAG_MODEL> invertedDefaultPose;
    idList<idMD5Mesh, TAG_MODEL> meshes;

    void DrawJoints(const renderEntity_t* ent, const viewDef_t* view) const;
    void ParseJoint(idLexer& parser, idMD5Joint* joint, idJointQuat* defaultPose);
};

/*
===============================================================================

        MD3 animated model

===============================================================================
*/

struct md3Header_s;
struct md3Surface_s;

class idRenderModelMD3 : public idRenderModelStatic {
public:
    virtual void InitFromFile(const char* fileName);
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual dynamicModel_t IsDynamicModel() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

private:
    int index;               // model = tr.models[model->index]
    int dataSize;            // just for listing purposes
    struct md3Header_s* md3; // only if type == MOD_MESH
    int numLods;

    void LerpMeshVertexes(srfTriangles_t* tri, const struct md3Surface_s* surf, const float backlerp, const int frame, const int oldframe) const;
};

/*
===============================================================================

        Liquid model

===============================================================================
*/

class idRenderModelLiquid : public idRenderModelStatic {
public:
    idRenderModelLiquid();

    virtual void InitFromFile(const char* fileName);
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual dynamicModel_t IsDynamicModel() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

    virtual void Reset();
    void IntersectBounds(const idBounds& bounds, float displacement);

private:
    modelSurface_t GenerateSurface(float lerp);
    void WaterDrop(int x, int y, float* page);
    void Update();

    int verts_x;
    int verts_y;
    float scale_x;
    float scale_y;
    int time;
    int liquid_type;
    int update_tics;
    int seed;

    idRandom random;

    const idMaterial* shader;
    deformInfo_t* deformInfo; // used to create srfTriangles_t from base frames
    // and new vertexes

    float density;
    float drop_height;
    int drop_radius;
    float drop_delay;

    idList<float, TAG_MODEL> pages;
    float* page1;
    float* page2;

    idList<idDrawVert, TAG_MODEL> verts;

    int nextDropTime;
};

/*
===============================================================================

        PRT model

===============================================================================
*/

class idRenderModelPrt : public idRenderModelStatic {
public:
    idRenderModelPrt();

    virtual void InitFromFile(const char* fileName);
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual void TouchData();
    virtual dynamicModel_t IsDynamicModel() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;
    virtual float DepthHack() const;
    virtual int Memory() const;

    // with the addModels2 arrangement we could have light accepting and
    // shadowing dynamic models, but the original game never did
    virtual bool ModelHasDrawingSurfaces() const
    {
        return true;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return false;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return false;
    };

private:
    const idDeclParticle* particleSystem;
};

/*
===============================================================================

        Beam model

===============================================================================
*/

class idRenderModelBeam : public idRenderModelStatic {
public:
    virtual dynamicModel_t IsDynamicModel() const;
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual bool IsLoaded() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

    // with the addModels2 arrangement we could have light accepting and
    // shadowing dynamic models, but the original game never did
    virtual bool ModelHasDrawingSurfaces() const
    {
        return true;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return false;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return false;
    };
};

/*
===============================================================================

        Beam model

===============================================================================
*/
#define MAX_TRAIL_PTS 20

struct Trail_t {
    int lastUpdateTime;
    int duration;

    idVec3 pts[MAX_TRAIL_PTS];
    int numPoints;
};

class idRenderModelTrail : public idRenderModelStatic {
    idList<Trail_t, TAG_MODEL> trails;
    int numActive;
    idBounds trailBounds;

public:
    idRenderModelTrail();

    virtual dynamicModel_t IsDynamicModel() const;
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual bool IsLoaded() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

    // with the addModels2 arrangement we could have light accepting and
    // shadowing dynamic models, but the original game never did
    virtual bool ModelHasDrawingSurfaces() const
    {
        return true;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return false;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return false;
    };

    int NewTrail(idVec3 pt, int duration);
    void UpdateTrail(int index, idVec3 pt);
    void DrawTrail(int index, const struct renderEntity_s* ent, srfTriangles_t* tri, float globalAlpha);
};

/*
===============================================================================

        Lightning model

===============================================================================
*/

class idRenderModelLightning : public idRenderModelStatic {
public:
    virtual dynamicModel_t IsDynamicModel() const;
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual bool IsLoaded() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

    // with the addModels2 arrangement we could have light accepting and
    // shadowing dynamic models, but the original game never did
    virtual bool ModelHasDrawingSurfaces() const
    {
        return true;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return false;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return false;
    };
};

/*
================================================================================

        idRenderModelSprite

================================================================================
*/
class idRenderModelSprite : public idRenderModelStatic {
public:
    virtual dynamicModel_t IsDynamicModel() const;
    virtual bool SupportsBinaryModel()
    {
        return false;
    }
    virtual bool IsLoaded() const;
    virtual idRenderModel* InstantiateDynamicModel(const struct renderEntity_s* ent, const viewDef_t* view, idRenderModel* cachedModel);
    virtual idBounds Bounds(const struct renderEntity_s* ent) const;

    // with the addModels2 arrangement we could have light accepting and
    // shadowing dynamic models, but the original game never did
    virtual bool ModelHasDrawingSurfaces() const
    {
        return true;
    };
    virtual bool ModelHasInteractingSurfaces() const
    {
        return false;
    };
    virtual bool ModelHasShadowCastingSurfaces() const
    {
        return false;
    };
};

#endif /* !__MODEL_LOCAL_H__ */
