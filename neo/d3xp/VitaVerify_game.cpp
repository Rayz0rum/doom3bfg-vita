/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - the full verification's view of the game
(PS Vita port; framework/VitaVerify.cpp drives it).

Called on the main thread between frames, while the game/draw thread is idle,
like the benchmark's own accessors (Game_local.cpp, Vita_Bench*).

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Game_local.h"
#include "menus/MenuHandler.h"
#include "menus/MenuScreen.h"

struct vitaVerifyEnt_t {
    bool exists;
    bool isAI;
    bool hidden;
    bool active;     // thinking
    bool hasEnemy;   // an idAI with an enemy
    bool afActive;   // a ragdoll
    bool atRest;
    int health;
    idVec3 origin;
    float floorGap;  // the lowest point of its bounds above the floor below it (-1 none found)
    float speed;
    float maxBodySpeed;  // a ragdoll: its fastest body
    float activeTime;    // a ragdoll: how long it has been simulated since it was last woken
    int bodies;
    bool dormant;        // closed off from the player: it does not think (stock behaviour)
};

bool Vita_VerifyInCinematic()
{
    return gameLocal.inCinematic;
}

int Vita_VerifyGameTime()
{
    return gameLocal.time;
}

bool Vita_VerifyPlayer(idVec3& origin, idAngles& angles, int& health, idStr& weapon)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    if (player == NULL) {
        return false;
    }
    origin = player->GetPhysics()->GetOrigin();
    angles = player->viewAngles;
    health = player->health;
    weapon = (player->weapon.GetEntity() != NULL) ? player->weapon.GetEntity()->GetName() : "";
    return true;
}

// the ammo the player has left in the current weapon's clip and in reserve
// (-1 when unknown); fired shots take from one of them
int Vita_VerifyPlayerAmmo()
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    if (player == NULL || player->weapon.GetEntity() == NULL) {
        return -1;
    }
    idWeapon* w = player->weapon.GetEntity();
    return w->AmmoInClip() + w->AmmoAvailable();
}

// a ragdoll: the smallest gap under any of its bodies - one body on the
// floor, a crate or a railing holds the whole corpse up
static float Vita_AFSupportGap(idAFEntity_Base* af)
{
    // each body's own shape swept straight down with its own clip mask (and
    // the world's solids): how far it could fall (grates, clip brushes and
    // props count, as they do for the ragdoll itself)
    idPhysics_AF* phys = af->GetAFPhysics();
    float best = -1.0f;
    for (int i = 0; i < phys->GetNumBodies(); i++) {
        idAFBody* body = phys->GetBody(i);
        if (body == NULL || body->GetClipModel() == NULL) {
            continue;
        }
        const int mask = body->GetClipMask() | phys->GetClipMask() | MASK_SOLID;
        const idVec3 o = body->GetWorldOrigin();
        trace_t tr;
        gameLocal.clip.Translation(tr, o, o - idVec3(0.0f, 0.0f, 256.0f), body->GetClipModel(), body->GetWorldAxis(), mask, af);
        const float gap = tr.fraction * 256.0f;
        if (best < 0.0f || gap < best) {
            best = gap;
        }
    }
    return best;
}

static float Vita_FloorGap(idEntity* ent)
{
    if (ent->IsType(idAFEntity_Base::Type) && static_cast<idAFEntity_Base*>(ent)->IsActiveAF() && ent->GetPhysics() == static_cast<idAFEntity_Base*>(ent)->GetAFPhysics()) {
        return Vita_AFSupportGap(static_cast<idAFEntity_Base*>(ent));
    }
    const idBounds b = ent->GetPhysics()->GetAbsBounds();
    if (b.IsCleared()) {
        return -1.0f;
    }
    const idVec3 c = b.GetCenter();
    trace_t tr;
    gameLocal.clip.TracePoint(tr, idVec3(c.x, c.y, b[0].z + 1.0f), idVec3(c.x, c.y, b[0].z - 1024.0f), MASK_SOLID, ent);
    if (tr.fraction >= 1.0f) {
        return -1.0f;
    }
    return b[0].z - tr.endpos.z;
}

bool Vita_VerifyEntity(const char* name, vitaVerifyEnt_t& out, bool floorGap)
{
    memset(&out, 0, sizeof(out));
    idEntity* ent = gameLocal.FindEntity(name);
    if (ent == NULL) {
        return false;
    }
    out.exists = true;
    out.isAI = ent->IsType(idAI::Type);
    out.hidden = ent->IsHidden();
    out.active = ent->IsActive();
    out.health = ent->health;
    out.origin = ent->GetPhysics()->GetOrigin();
    out.atRest = ent->GetPhysics()->IsAtRest();
    out.dormant = ent->fl.isDormant;
    out.speed = ent->GetPhysics()->GetLinearVelocity().Length();
    if (out.isAI) {
        out.hasEnemy = static_cast<idAI*>(ent)->GetEnemy() != NULL;
    }
    if (ent->IsType(idAFEntity_Base::Type)) {
        idAFEntity_Base* af = static_cast<idAFEntity_Base*>(ent);
        // a ragdoll: the figure is what moves the entity (a living monster's
        // figure is "active" too, as its hit boxes; one that burns away keeps it so)
        out.afActive = af->IsActiveAF() && ent->GetPhysics() == af->GetAFPhysics();
        if (out.afActive) {
            idPhysics_AF* phys = af->GetAFPhysics();
            out.bodies = phys->GetNumBodies();
            out.activeTime = phys->VitaActivateTime();
            for (int i = 0; i < phys->GetNumBodies(); i++) {
                out.maxBodySpeed = Max(out.maxBodySpeed, phys->GetBody(i)->GetLinearVelocity().Length());
            }
        }
    }
    out.floorGap = floorGap ? Vita_FloorGap(ent) : -1.0f; // (a trace per body: only when asked)
    return true;
}

// the monster classes the map already has (their assets are loaded), the
// common ones first; bosses, scripted characters and turrets left out
void Vita_VerifyMonsterClasses(idStrList& classes)
{
    static const char* skip[] = { "boss", "cinematic", "cyberdemon", "guardian", "sabaoth", "maledict", "hunter", "vagary", "sentry", "turret", "_spawner", "_dummy", "flying", "cherub_hand", NULL };
    for (idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next()) {
        if (!ent->IsType(idAI::Type)) {
            continue;
        }
        const char* cls = ent->spawnArgs.GetString("classname");
        if (idStr::Icmpn(cls, "monster_", 8) != 0) {
            continue;
        }
        bool bad = false;
        for (int i = 0; skip[i] != NULL && !bad; i++) {
            bad = idStr::FindText(cls, skip[i], false) >= 0;
        }
        if (!bad && classes.FindIndex(cls) < 0) {
            classes.Append(cls);
        }
    }
}

// a spot on the floor at about dist units from the player, as close to the
// direction he looks as the map allows, in plain sight; then the monster
bool Vita_VerifySpawnNear(const char* cls, const char* name, float dist, idStr& why)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    if (player == NULL) {
        why = "no player";
        return false;
    }
    const idVec3 eye = player->GetEyePosition();
    const idBounds box(idVec3(-20.0f, -20.0f, 0.0f), idVec3(20.0f, 20.0f, 72.0f));
    static const float yaws[] = { 0.0f, 25.0f, -25.0f, 50.0f, -50.0f, 80.0f, -80.0f, 120.0f, -120.0f, 180.0f };
    static const float dists[] = { 1.0f, 0.75f, 0.5f };
    int blocked = 0, low = 0, noFloor = 0, solid = 0, height = 0, closedOff = 0;
    // the player's potentially visible set, now (the game's own is freed at
    // the end of each frame, and this runs between frames)
    const pvsHandle_t pvs = gameLocal.pvs.SetupCurrentPVS(player->GetPVSAreas(), player->GetNumPVSAreas());
    // and the areas connected to his through open portals: a monster outside
    // them goes dormant (idEntity::DoDormantTests), seen or not
    const pvsHandle_t connected = gameLocal.pvs.SetupCurrentPVS(player->GetPVSAreas(), player->GetNumPVSAreas(), PVS_CONNECTED_AREAS);
    for (int d = 0; d < 3; d++) {
        for (int y = 0; y < 10; y++) {
            const float yaw = player->viewAngles.yaw + yaws[y];
            const idVec3 dir = idAngles(0.0f, yaw, 0.0f).ToForward();
            idVec3 want = eye + dir * (dist * dists[d]);
            trace_t tr;
            // in sight from the eye: as far as the walls allow, keeping 40
            // units off them (the player may stand in a small room)
            gameLocal.clip.TracePoint(tr, eye, want, MASK_SOLID, player);
            if (tr.fraction < 1.0f) {
                const float free = dist * dists[d] * tr.fraction - 40.0f;
                if (free < 80.0f) {
                    blocked++;
                    continue;
                }
                want = eye + dir * free;
            }
            // down to the floor, from just above the player's feet (from the
            // eye, a monster's box would start in the ceiling)
            idVec3 from = want;
            from.z = player->GetPhysics()->GetOrigin().z + 24.0f;
            gameLocal.clip.TracePoint(tr, want, from, MASK_SOLID, player);
            if (tr.fraction < 1.0f) {
                low++;
                continue;
            }
            gameLocal.clip.TraceBounds(tr, from, from - idVec3(0.0f, 0.0f, 512.0f), box, MASK_MONSTERSOLID, player);
            if (tr.fraction >= 1.0f || tr.fraction <= 0.0f) {
                noFloor++;
                continue;
            }
            const idVec3 feet = tr.endpos + idVec3(0.0f, 0.0f, 1.0f);
            if (gameLocal.clip.Contents(feet, NULL, mat3_identity, MASK_MONSTERSOLID, player) != 0) {
                solid++;
                continue;
            }
            if (idMath::Fabs(feet.z - player->GetPhysics()->GetOrigin().z) > 160.0f) {
                height++;
                continue; // far below or above: not a fight in front of him
            }
            // in sight through glass or a grate is not enough: closed off from
            // the player (not in his potentially visible set) it would stay
            // dormant - not think, not fight, not fall as a ragdoll
            const idBounds spot = idBounds(feet).Expand(16.0f);
            if (!gameLocal.pvs.InCurrentPVS(pvs, spot) || !gameLocal.pvs.InCurrentPVS(connected, spot)) {
                closedOff++;
                continue;
            }
            idDict args;
            args.Set("classname", cls);
            args.Set("name", name);
            args.SetVector("origin", feet);
            args.SetFloat("angle", yaw + 180.0f); // facing the player
            idEntity* ent = NULL;
            gameLocal.pvs.FreeCurrentPVS(pvs);
            gameLocal.pvs.FreeCurrentPVS(connected);
            if (!gameLocal.SpawnEntityDef(args, &ent) || ent == NULL) {
                why = va("the game refused to spawn %s", cls);
                return false;
            }
            return true;
        }
    }
    gameLocal.pvs.FreeCurrentPVS(pvs);
    gameLocal.pvs.FreeCurrentPVS(connected);
    why = va("no free floor in sight near the player (of 30 spots tried: %d with a wall closer than 120 units, %d blocked below the eye, %d no floor or starting in solid, %d in something solid, %d too far up or down, %d closed off from the player)", blocked, low, noFloor, solid, height, closedOff);
    return false;
}

// view angles into the angles the input (the usercmd) has to carry: the
// player's view is the input's angles plus his delta angles (set when he is
// teleported or spawned); normalized to -180..180
void Vita_VerifyViewToCmd(float& yaw, float& pitch)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    if (player != NULL) {
        yaw -= player->GetDeltaViewAngles().yaw;
        pitch -= player->GetDeltaViewAngles().pitch;
    }
    yaw = idMath::AngleNormalize180(yaw);
    pitch = idMath::AngleNormalize180(pitch);
}

// the input angles that look from the player's eye at the entity's middle
bool Vita_VerifyAimAt(const char* name, float& yaw, float& pitch)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    idEntity* ent = gameLocal.FindEntity(name);
    if (player == NULL || ent == NULL) {
        return false;
    }
    const idVec3 to = ent->GetPhysics()->GetAbsBounds().GetCenter() - player->GetEyePosition();
    const idAngles a = to.ToAngles();
    yaw = a.yaw;
    pitch = a.pitch;
    Vita_VerifyViewToCmd(yaw, pitch);
    return true;
}

void Vita_VerifyCountAI(int& alive, int& awake, int& ragdollsMoving)
{
    alive = awake = ragdollsMoving = 0;
    for (idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next()) {
        if (ent->IsType(idAI::Type) && ent->health > 0 && !ent->IsHidden()) {
            alive++;
            if (ent->IsActive()) {
                awake++;
            }
        }
        if (ent->IsType(idAFEntity_Base::Type) && static_cast<idAFEntity_Base*>(ent)->IsActiveAF() && !ent->GetPhysics()->IsAtRest()) {
            ragdollsMoving++;
        }
    }
}

// the main menu (the shell): the screen it shows, and asking for another
bool Vita_VerifyShellActive()
{
    return gameLocal.shellHandler != NULL && gameLocal.shellHandler->IsActive();
}

int Vita_VerifyShellArea()
{
    return gameLocal.shellHandler != NULL ? gameLocal.shellHandler->ActiveScreen() : -1;
}

bool Vita_VerifyShellGoto(int area)
{
    if (gameLocal.shellHandler == NULL) {
        return false;
    }
    gameLocal.shellHandler->SetNextScreen(area, MENU_TRANSITION_SIMPLE);
    return true;
}

const char* Vita_VerifyShellAreaName(int area)
{
    static const char* names[] = { "start", "root", "dev", "campaign", "load", "save", "new game", "game options", "system options", "multiplayer", "game lobby", "stereoscopics", "party lobby", "settings", "audio", "video", "keyboard", "controls", "controller layout", "gamepad", "pause", "leaderboards", "playstation", "difficulty", "resolution", "match settings", "mode select", "browser", "credits" };
    if (area < 0 || area >= (int)(sizeof(names) / sizeof(names[0]))) {
        return "?";
    }
    return names[area];
}

// the player moved to the most open walkable spot near him (the AAS's areas,
// dropped to the floor), facing his longest clear line of sight: maps start
// in elevators and corridors, where there is nothing to look at and no room
// for a fight
bool Vita_VerifyGoOpen(float wantRoom, idStr& where)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    idAAS* aas = gameLocal.GetAAS("aas48");
    for (int i = 0; i < gameLocal.NumAAS() && aas == NULL; i++) {
        aas = gameLocal.GetAAS(i);
    }
    if (player == NULL || aas == NULL) {
        where = (player == NULL) ? "no player" : "the map has no AAS";
        return false;
    }
    const idVec3 start = player->GetPhysics()->GetOrigin();
    const idBounds bounds = player->GetPhysics()->GetBounds();
    const float eyeHeight = player->EyeHeight();
    const int numAreas = aas->NumAreas();
    const int stride = Max(1, numAreas / 800);
    float bestScore = -1.0f, bestYaw = 0.0f, bestRoom = 0.0f;
    idVec3 bestFeet = start;
    int tried = 0;
    for (int a = 1; a < numAreas; a += stride) {
        if ((aas->AreaFlags(a) & AREA_REACHABLE_WALK) == 0) {
            continue;
        }
        const idVec3 c = aas->AreaCenter(a);
        const float away = (c - start).Length();
        if (away > 3000.0f) {
            continue;
        }
        trace_t tr;
        gameLocal.clip.TraceBounds(tr, c + idVec3(0.0f, 0.0f, 16.0f), c - idVec3(0.0f, 0.0f, 256.0f), bounds, MASK_PLAYERSOLID, player);
        if (tr.fraction >= 1.0f || tr.fraction <= 0.0f) {
            continue;
        }
        const idVec3 feet = tr.endpos + idVec3(0.0f, 0.0f, 1.0f);
        if (gameLocal.clip.Contents(feet, player->GetPhysics()->GetClipModel(), mat3_identity, MASK_PLAYERSOLID, player) != 0) {
            continue;
        }
        tried++;
        const idVec3 eye = feet + idVec3(0.0f, 0.0f, eyeHeight);
        float room = 0.0f, yaw = 0.0f, around = 0.0f;
        for (int k = 0; k < 8; k++) {
            const idVec3 dir = idAngles(0.0f, k * 45.0f, 0.0f).ToForward();
            gameLocal.clip.TracePoint(tr, eye, eye + dir * 1024.0f, MASK_SOLID, player);
            const float len = tr.fraction * 1024.0f;
            around += len;
            if (len > room) {
                room = len;
                yaw = k * 45.0f;
            }
        }
        if (room < wantRoom) {
            continue;
        }
        const float score = room + around * 0.25f - away * 0.5f;
        if (score > bestScore) {
            bestScore = score;
            bestFeet = feet;
            bestYaw = yaw;
            bestRoom = room;
        }
    }
    if (bestScore < 0.0f) {
        where = va("no spot with %.0f units of room within 3000 units (%d spots tried)", wantRoom, tried);
        return false;
    }
    player->Teleport(bestFeet, idAngles(0.0f, bestYaw, 0.0f), NULL);
    where = va("(%.0f %.0f %.0f) facing %.0f: %.0f units clear ahead, %.0f units from where the map started him (%d spots tried)",
        bestFeet.x, bestFeet.y, bestFeet.z, bestYaw, bestRoom, (bestFeet - start).Length(), tried);
    return true;
}

// the map takes the player's weapons away (worldspawn no_Weapons: Mars City,
// phobos1...)
bool Vita_VerifyNoWeapons()
{
    return gameLocal.world != NULL && gameLocal.world->spawnArgs.GetBool("no_Weapons");
}

// the player can fire at all: the map has weapons, its script has not taken
// them away, and one is in hand
bool Vita_VerifyWeaponsUsable(idStr& why)
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    if (player == NULL) {
        why = "no player";
        return false;
    }
    if (gameLocal.world != NULL && gameLocal.world->spawnArgs.GetBool("no_Weapons")) {
        why = "the map takes the player's weapons away (no_Weapons)";
        return false;
    }
    if (!player->VitaWeaponEnabled()) {
        why = "the map's script has the weapons disabled here";
        return false;
    }
    if (player->hiddenWeapon) {
        why = "the weapon is hidden here";
        return false;
    }
    return true;
}

// the player's speed (a save made while he slides or rides something)
float Vita_VerifyPlayerSpeed()
{
    idPlayer* player = gameLocal.GetLocalPlayer();
    return player != NULL ? player->GetPhysics()->GetLinearVelocity().Length() : 0.0f;
}

// the verification's trophy check: an achievement through the game's own
// unlock path (0 sent, 1 the profile has it already, -1 no signed-in user)
int Vita_VerifyTrophyChain(int id)
{
    idLocalUser* user = session->GetSignInManager().GetMasterLocalUser();
    if (user == NULL || user->GetProfile() == NULL) {
        return -1;
    }
    if (user->GetProfile()->GetAchievement(id)) {
        return 1;
    }
    idAchievementManager::LocalUser_CompleteAchievement((achievement_t)id);
    return 0;
}
