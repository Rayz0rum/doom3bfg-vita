/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - multiplayer bots (PS Vita port).

The stock game has none, but its lobby kept the slots for them: a host can
add a lobby user marked as a bot (idLobby::AllocLobbyUserSlotForBot), every
peer sees it in the scoreboard and the game spawns a player for it like for
anyone else. What was missing is a brain. On the host, each bot's player
runs the command this file makes for it every tic (RunAllUserCmdsForPlayer)
instead of commands that would have come over the network.

THE BRAIN is Quake III Arena's (id Software, GPL v2 or later -
https://github.com/id-Software/Quake-III-Arena, code/game/ai_dmq3.c and
ai_main.c), ported to DOOM 3's players and weapons:
  - a character per difficulty: aim skill, aim accuracy, reaction time, fire
    throttle, attack skill, jumper, croucher, view factor and maximum view
    change, alertness (Q3's CHARACTERISTIC_*), five levels like Q3's skills
    (bot_skill: 1 I Can Win, 2 Bring It On, 3 Hurt Me Plenty, 4 Hardcore,
    5 Nightmare!);
  - BotFindEnemy: alertness sets how far it looks, a field of view that
    widens when the enemy is close, and all round when it is being hurt or
    the enemy is shooting; it lets an enemy that has not seen it go when it
    does not want to fight;
  - BotAimAtEnemy: the enemy's velocity remembered every half second, worse
    accuracy when the enemy changes direction, lead for projectiles (exact
    above aim skill 0.8, linear above 0.4), the ground at the enemy's feet for
    splash weapons above 0.6, random error scaled by (1 - accuracy) on the
    point, the direction and the weapon's spread; Q3's smooth view model
    (view factor, maximum change a second) turns the view;
  - BotCheckAttack: no shot before the reaction time after the enemy is
    seen, the fire throttle, a field of view of 50 degrees (120 close), the
    line of fire clear, never through a teammate, no splash shot at a wall
    under the bot's nose;
  - BotAttackMove: strafing with Q3's timings (direction kept 0.4 - 0.6 s,
    flipped at random, backing off now and then), moving to the weapon's
    ideal distance, jumping (jumper) and crouching (croucher); the lowest
    skills stand or walk straight at the enemy;
  - BotAggression / BotWantsToRetreat / BotWantsToChase: health, armour and
    the weapons and ammo held decide whether to fight, to fall back to items
    while shooting, or to chase an enemy that went out of sight;
  - BotChooseWeapon: Q3 weighs weapons with fuzzy logic files; here each
    DOOM 3 weapon has a weight by distance (shotguns close, rockets not
    under the bot's nose, the BFG far) and needs ammo;
  - long term goals: items valued by need (health when hurt, armour when
    low, weapons not held, powerups) over their path's length, the CTF flags
    (attackers and defenders by the bot's number), the last place an enemy
    was heard shooting, else roaming.

WHERE THEY CAN GO: Q3's bots plan on the map's AAS; DOOM 3's multiplayer maps
have none. So the host builds a waypoint graph (BotNav below): nodes at the
items and spawn points, linked where a walk between them is checked clear
(traces at knee and chest height, the floor sampled every 32 units - steps
up to 24, drops one way); and nodes and links learned from where every player
- people and bots - actually walks, jumps, rides and teleports. A* over it;
links that fail get dearer and are dropped. It is saved per map
(savepath botnav/<map>.nav) and grows from session to session. Until it
covers the map, a bot steers with feelers towards its goal.

On the host: "addBot [count] [skill]", "removeBots", bot_skill, bot_count
(the Match Settings' Bots and Bot Difficulty: the host keeps that many bots,
people first). The full verification fills lobbies with addBot.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Game_local.h"

idCVar bot_skill("bot_skill", "3", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "multiplayer bots' difficulty (Quake III's): 1 I Can Win, 2 Bring It On, 3 Hurt Me Plenty, 4 Hardcore, 5 Nightmare!", 1, 5);
idCVar bot_count("bot_count", "0", CVAR_GAME | CVAR_INTEGER | CVAR_ARCHIVE, "bots the host keeps in a multiplayer game it hosts (0-7; players take the slots first)", 0, 7);
idCVar bot_debug("bot_debug", "0", CVAR_GAME | CVAR_BOOL, "log the bots' decisions as they change");
idCVar bot_navLinksPerFrame("bot_navLinksPerFrame", "16", CVAR_GAME | CVAR_INTEGER, "waypoint pairs the host tests for a walkable link each frame (bots' navigation graph)", 0, 512);
idCVar bot_navExplorePerFrame("bot_navExplorePerFrame", "2", CVAR_GAME | CVAR_INTEGER, "waypoints the host explores around each frame - eight directions over open floor (bots' navigation graph)", 0, 64);

static idRandom vitaBotRandom(1234);

static const char* const vitaBotNames[] = { "Bot Kelly", "Bot Swann", "Bot Campbell", "Bot Betruger", "Bot McNeil", "Bot Bravo", "Bot Delta", "Bot Hayden" };

const char* VitaBot_SkillName(int skill)
{
    static const char* names[5] = { "1 - I Can Win", "2 - Bring It On", "3 - Hurt Me Plenty", "4 - Hardcore", "5 - Nightmare!" };
    return names[idMath::ClampInt(1, 5, skill) - 1];
}

static inline float VitaBot_Chance(float perTenth, float dtSec)
{
    // Q3 decides once per think (0.1 s); here every tic: the same odds a second
    return 1.0f - powf(1.0f - idMath::ClampFloat(0.0f, 1.0f, perTenth), dtSec / 0.1f);
}

/*
================================================================================

The character (Q3's CHARACTERISTIC_*), by skill

================================================================================
*/
struct botChar_t {
    float aimSkill;      // prediction, ground shots, aim delay
    float aimAccuracy;   // random error
    float reactionTime;  // s before the first shot at a newly seen enemy
    float fireThrottle;  // 1 = fires whenever it can
    float attackSkill;   // < 0.2 stands, <= 0.4 walks at / away, above strafes
    float jumper;
    float croucher;
    float viewFactor;    // how much of the remaining turn a tenth of a second takes
    float viewMaxChange; // degrees a second
    float alertness;     // how far it looks: 900 + alertness * 4000 units
};

static botChar_t VitaBot_Character(int skill)
{
    static const botChar_t table[5] = {
        // aimSkill aimAcc reaction throttle attack jumper croucher viewF viewMax alert
        { 0.15f, 0.25f, 0.90f, 0.45f, 0.15f, 0.05f, 0.00f, 0.12f, 240.0f, 0.25f },  // 1 I Can Win
        { 0.35f, 0.45f, 0.60f, 0.60f, 0.35f, 0.15f, 0.05f, 0.20f, 360.0f, 0.45f },  // 2 Bring It On
        { 0.60f, 0.65f, 0.40f, 0.75f, 0.60f, 0.30f, 0.10f, 0.35f, 600.0f, 0.65f },  // 3 Hurt Me Plenty
        { 0.85f, 0.82f, 0.25f, 0.90f, 0.85f, 0.45f, 0.15f, 0.55f, 900.0f, 0.85f },  // 4 Hardcore
        { 1.00f, 0.95f, 0.12f, 1.00f, 1.00f, 0.60f, 0.20f, 0.80f, 1500.0f, 1.00f }, // 5 Nightmare!
    };
    return table[idMath::ClampInt(1, 5, skill) - 1];
}

/*
================================================================================

Weapons (Q3's weaponinfo_t, from DOOM 3's entity defs)

================================================================================
*/
enum botWeaponKind_t { BW_NONE, BW_FISTS, BW_CHAINSAW, BW_PISTOL, BW_SHOTGUN, BW_SHOTGUN_DOUBLE, BW_MACHINEGUN, BW_CHAINGUN, BW_GRENADE, BW_PLASMA, BW_ROCKET, BW_BFG };

struct botWeapon_t {
    bool known;
    idStr def;           // def_weapon<n>
    int kind;
    float speed;         // projectile speed, 0 = instant
    float splashRadius;  // 0 = none
    float spread;        // degrees
    bool allowEmpty;
};
static botWeapon_t vitaBotWeapons[MAX_WEAPONS];
static bool vitaBotWeaponsReady = false;

static void VitaBot_LearnWeapons(idPlayer* p)
{
    if (vitaBotWeaponsReady) {
        return;
    }
    vitaBotWeaponsReady = true;
    idStr list;
    for (int i = 0; i < MAX_WEAPONS; i++) {
        botWeapon_t& w = vitaBotWeapons[i];
        w = botWeapon_t();
        w.def = p->spawnArgs.GetString(va("def_weapon%d", i));
        w.allowEmpty = p->spawnArgs.GetBool(va("weapon%d_allowempty", i));
        if (w.def.IsEmpty()) {
            continue;
        }
        w.known = true;
        const idStr& d = w.def;
        if (d.Find("fists", false) >= 0) {
            w.kind = BW_FISTS;
        } else if (d.Find("chainsaw", false) >= 0) {
            w.kind = BW_CHAINSAW;
        } else if (d.Find("pistol", false) >= 0) {
            w.kind = BW_PISTOL;
        } else if (d.Find("shotgun_double", false) >= 0) {
            w.kind = BW_SHOTGUN_DOUBLE;
        } else if (d.Find("shotgun", false) >= 0) {
            w.kind = BW_SHOTGUN;
        } else if (d.Find("machinegun", false) >= 0) {
            w.kind = BW_MACHINEGUN;
        } else if (d.Find("chaingun", false) >= 0) {
            w.kind = BW_CHAINGUN;
        } else if (d.Find("grenade", false) >= 0) {
            w.kind = BW_GRENADE;
        } else if (d.Find("plasma", false) >= 0) {
            w.kind = BW_PLASMA;
        } else if (d.Find("rocket", false) >= 0) {
            w.kind = BW_ROCKET;
        } else if (d.Find("bfg", false) >= 0) {
            w.kind = BW_BFG;
        } else {
            w.kind = BW_NONE; // flashlight, PDA, soul cube, grabber...: not for fighting
        }
        const idDeclEntityDef* wdef = gameLocal.FindEntityDef(d, false);
        if (wdef != NULL) {
            w.spread = wdef->dict.GetFloat("spread", "0");
            const char* projName = wdef->dict.GetString("def_projectile");
            const idDeclEntityDef* pdef = projName[0] ? gameLocal.FindEntityDef(projName, false) : NULL;
            if (pdef != NULL) {
                const float v = pdef->dict.GetVector("velocity", "0 0 0").Length();
                w.speed = (v > 0.0f && v < 4000.0f) ? v : 0.0f; // fast enough counts as instant
                const char* splash = pdef->dict.GetString("def_splash_damage");
                const idDeclEntityDef* sdef = splash[0] ? gameLocal.FindEntityDef(splash, false) : NULL;
                if (sdef != NULL) {
                    w.splashRadius = sdef->dict.GetFloat("radius", "0");
                }
            }
        }
        if (w.kind != BW_NONE) {
            list += va("%s%d %s (speed %.0f, splash %.0f, spread %.1f)", list.IsEmpty() ? "" : ", ", i, d.c_str(), w.speed, w.splashRadius, w.spread);
        }
    }
    gameLocal.Printf("bots: weapons %s\n", list.c_str());
}

// Q3's fuzzy weapon weights, by hand: how good a weapon is at a distance
static float VitaBot_WeaponWeight(int kind, float dist)
{
    switch (kind) {
    case BW_FISTS:
        return dist < 72.0f ? 0.35f : 0.02f;
    case BW_CHAINSAW:
        return dist < 96.0f ? 0.90f : 0.05f;
    case BW_PISTOL:
        return 0.20f;
    case BW_SHOTGUN:
        return dist < 350.0f ? 0.70f : 0.30f;
    case BW_SHOTGUN_DOUBLE:
        return dist < 300.0f ? 0.85f : 0.30f;
    case BW_MACHINEGUN:
        return 0.50f;
    case BW_CHAINGUN:
        return 0.72f;
    case BW_GRENADE:
        return (dist > 200.0f && dist < 700.0f) ? 0.35f : 0.10f;
    case BW_PLASMA:
        return 0.75f;
    case BW_ROCKET:
        return dist < 180.0f ? 0.25f : 0.86f;
    case BW_BFG:
        return dist < 250.0f ? 0.30f : 0.95f;
    }
    return -1.0f;
}

// Q3's IDEAL_ATTACKDIST and attack range, by weapon
static void VitaBot_AttackDistance(int kind, float& dist, float& range)
{
    switch (kind) {
    case BW_FISTS:
    case BW_CHAINSAW:
        dist = 0.0f;
        range = 0.0f;
        return;
    case BW_SHOTGUN:
    case BW_SHOTGUN_DOUBLE:
        dist = 160.0f;
        range = 60.0f;
        return;
    case BW_ROCKET:
    case BW_GRENADE:
        dist = 450.0f;
        range = 150.0f;
        return;
    case BW_BFG:
        dist = 500.0f;
        range = 150.0f;
        return;
    }
    dist = 350.0f;
    range = 120.0f;
}

static int VitaBot_Ammo(idPlayer* p, int weapon)
{
    const botWeapon_t& w = vitaBotWeapons[weapon];
    if (!w.known || !(p->inventory.weapons & (1 << weapon))) {
        return 0;
    }
    if (w.allowEmpty || w.kind == BW_FISTS || w.kind == BW_CHAINSAW) {
        return 999;
    }
    return p->inventory.HasAmmo(w.def, true, p);
}

/*
================================================================================

BotNav: the waypoint graph

================================================================================
*/
struct navLink_t {
    int to;
    float cost;
    byte fails;
    byte learned; // 0 tested walkable, 1 learned from a player, 2 teleport
};
struct navNode_t {
    idVec3 pos;
    bool seeded; // an item or a spawn point
    idList<navLink_t> links;
};

static const int BOTNAV_MAX_NODES = 1536;
static const int BOTNAV_VERSION = 1;

static struct {
    idStr map;
    idList<navNode_t> nodes;
    int pairI, pairJ;        // the linker's first sweep: the next pair
    bool linkerDone;         // that sweep is over (new nodes then go through "pending")
    idList<int> pending;     // nodes added since: linked against every other one
    int pendingJ;
    idList<int> explore;     // nodes to look around from (eight directions)
    int explored, exploreAdded;
    int tested, linkedWalk, linkedLearned, linkedTeleport, dropped;
    int lastNode[MAX_PLAYERS];
    int lastNodeMs[MAX_PLAYERS];
    idVec3 lastPos[MAX_PLAYERS];
    bool loaded;
    int lastReportMs;
    int plans, planFails;
} botNav;

static idVec3 BotNav_Floor(const idVec3& p, float above = 32.0f, float below = 256.0f)
{
    trace_t tr;
    gameLocal.clip.TracePoint(tr, p + idVec3(0, 0, above), p - idVec3(0, 0, below), MASK_PLAYERSOLID, NULL);
    return tr.fraction < 1.0f ? tr.endpos : p;
}

static int BotNav_Nearest(const idVec3& p, float maxDist, bool needSight = false)
{
    int best = -1;
    float bestD = maxDist * maxDist;
    for (int i = 0; i < botNav.nodes.Num(); i++) {
        const float d = (botNav.nodes[i].pos - p).LengthSqr();
        if (d < bestD && idMath::Fabs(botNav.nodes[i].pos.z - p.z) < 96.0f) {
            if (needSight) {
                trace_t tr;
                gameLocal.clip.TracePoint(tr, p + idVec3(0, 0, 32), botNav.nodes[i].pos + idVec3(0, 0, 32), MASK_PLAYERSOLID, NULL);
                if (tr.fraction < 1.0f) {
                    continue;
                }
            }
            best = i;
            bestD = d;
        }
    }
    return best;
}

static int BotNav_AddNode(const idVec3& at, bool seeded)
{
    if (botNav.nodes.Num() >= BOTNAV_MAX_NODES) {
        return -1;
    }
    const idVec3 p = BotNav_Floor(at);
    const int nearest = BotNav_Nearest(p, 64.0f);
    if (nearest >= 0) {
        botNav.nodes[nearest].seeded |= seeded;
        return nearest;
    }
    navNode_t& n = botNav.nodes.Alloc();
    n.pos = p;
    n.seeded = seeded;
    const int index = botNav.nodes.Num() - 1;
    if (botNav.linkerDone) {
        botNav.pending.Append(index); // linked against the others in the background
    }
    botNav.explore.Append(index);
    return index;
}

static navLink_t* BotNav_FindLink(int a, int b)
{
    idList<navLink_t>& l = botNav.nodes[a].links;
    for (int i = 0; i < l.Num(); i++) {
        if (l[i].to == b) {
            return &l[i];
        }
    }
    return NULL;
}

static void BotNav_Link(int a, int b, float cost, int learned)
{
    if (a < 0 || b < 0 || a == b) {
        return;
    }
    navLink_t* existing = BotNav_FindLink(a, b);
    if (existing != NULL) {
        if (learned != 0 && existing->fails > 0) {
            existing->fails = 0; // someone just made it: trust it again
            existing->cost = Min(existing->cost, cost);
        }
        return;
    }
    navLink_t& l = botNav.nodes[a].links.Alloc();
    l.to = b;
    l.cost = cost;
    l.fails = 0;
    l.learned = (byte)learned;
    if (learned == 0) {
        botNav.linkedWalk++;
    } else if (learned == 1) {
        botNav.linkedLearned++;
    } else {
        botNav.linkedTeleport++;
    }
}

// a walk from a to b: clear at knee and chest height, floor all the way,
// steps up of 24 at most (drops allowed - one way)
static bool BotNav_Walkable(const idVec3& a, const idVec3& b)
{
    idVec3 d = b - a;
    d.z = 0.0f;
    const float len = d.Normalize();
    if (len < 1.0f) {
        return true;
    }
    const idVec3 side(-d.y, d.x, 0.0f);
    trace_t tr;
    for (int h = 0; h < 2; h++) {
        for (int s = -1; s <= 1; s++) {
            const idVec3 off = idVec3(0, 0, h == 0 ? 22.0f : 58.0f) + side * (12.0f * s);
            gameLocal.clip.TracePoint(tr, a + off, idVec3(b.x, b.y, a.z) + off + idVec3(0, 0, Max(0.0f, b.z - a.z)), MASK_PLAYERSOLID, NULL);
            if (tr.fraction < 1.0f && s == 0) {
                return false;
            }
            if (tr.fraction < 0.9f) {
                return false;
            }
        }
    }
    float floorZ = a.z;
    const int steps = (int)(len / 32.0f);
    for (int i = 1; i <= steps; i++) {
        const idVec3 p = a + d * (32.0f * i);
        gameLocal.clip.TracePoint(tr, idVec3(p.x, p.y, floorZ + 40.0f), idVec3(p.x, p.y, floorZ - 220.0f), MASK_PLAYERSOLID, NULL);
        if (tr.fraction >= 1.0f || tr.c.normal.z < 0.7f) {
            return false; // a hole, or too steep
        }
        if (tr.endpos.z - floorZ > 24.0f) {
            return false; // a ledge to climb
        }
        floorZ = tr.endpos.z;
    }
    return idMath::Fabs(floorZ - b.z) < 40.0f;
}

static void BotNav_Clear(const char* map)
{
    botNav.map = map;
    botNav.nodes.Clear();
    botNav.pairI = 0;
    botNav.pairJ = 1;
    botNav.linkerDone = false;
    botNav.pending.Clear();
    botNav.pendingJ = 0;
    botNav.explore.Clear();
    botNav.explored = botNav.exploreAdded = 0;
    botNav.tested = botNav.linkedWalk = botNav.linkedLearned = botNav.linkedTeleport = botNav.dropped = 0;
    botNav.plans = botNav.planFails = 0;
    botNav.loaded = false;
    botNav.lastReportMs = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        botNav.lastNode[i] = -1;
        botNav.lastNodeMs[i] = 0;
        botNav.lastPos[i].Zero();
    }
}

static idStr BotNav_File(const char* map)
{
    idStr name = map;
    name.StripPath();
    name.StripFileExtension();
    return va("botnav/%s.nav", name.c_str());
}

static bool BotNav_Load(const char* map)
{
    void* buffer = NULL;
    const idStr path = BotNav_File(map);
    const int len = fileSystem->ReadFile(path, &buffer);
    if (len <= 0 || buffer == NULL) {
        return false;
    }
    idLexer src((const char*)buffer, len, path, LEXFL_NOERRORS | LEXFL_NOWARNINGS);
    bool ok = src.ExpectTokenString("botnav") && src.ParseInt() == BOTNAV_VERSION;
    const int numNodes = ok ? src.ParseInt() : 0;
    ok = ok && numNodes > 0 && numNodes <= BOTNAV_MAX_NODES;
    if (ok) {
        botNav.nodes.SetNum(numNodes);
        for (int i = 0; i < numNodes && ok; i++) {
            navNode_t& n = botNav.nodes[i];
            n.pos.x = src.ParseFloat();
            n.pos.y = src.ParseFloat();
            n.pos.z = src.ParseFloat();
            n.seeded = src.ParseInt() != 0;
            const int numLinks = src.ParseInt();
            n.links.Clear();
            for (int k = 0; k < numLinks; k++) {
                navLink_t l;
                l.to = src.ParseInt();
                l.cost = src.ParseFloat();
                l.learned = (byte)src.ParseInt();
                l.fails = 0;
                if (l.to >= 0 && l.to < numNodes) {
                    n.links.Append(l);
                }
            }
            ok = !src.HadError();
        }
    }
    fileSystem->FreeFile(buffer);
    if (!ok) {
        botNav.nodes.Clear();
        gameLocal.Warning("bots: %s is damaged - building the map's graph again", path.c_str());
        return false;
    }
    int links = 0;
    for (int i = 0; i < botNav.nodes.Num(); i++) {
        links += botNav.nodes[i].links.Num();
    }
    botNav.loaded = true;
    botNav.linkerDone = true; // what was saved was tested; new nodes go through "pending"
    botNav.explore.Clear();   // and was explored
    gameLocal.Printf("bots: navigation graph %s loaded - %d nodes, %d links (learned over earlier games)\n", path.c_str(), botNav.nodes.Num(), links);
    return true;
}

static void BotNav_Save()
{
    if (botNav.map.IsEmpty() || botNav.nodes.Num() == 0) {
        return;
    }
    const idStr path = BotNav_File(botNav.map);
    idFile* f = fileSystem->OpenFileWrite(path, "fs_savepath");
    if (f == NULL) {
        gameLocal.Warning("bots: could not write %s", path.c_str());
        return;
    }
    f->Printf("botnav %d %d\n", BOTNAV_VERSION, botNav.nodes.Num());
    int links = 0;
    for (int i = 0; i < botNav.nodes.Num(); i++) {
        const navNode_t& n = botNav.nodes[i];
        int kept = 0;
        for (int k = 0; k < n.links.Num(); k++) {
            kept += (n.links[k].fails < 3) ? 1 : 0;
        }
        f->Printf("%.1f %.1f %.1f %d %d", n.pos.x, n.pos.y, n.pos.z, n.seeded ? 1 : 0, kept);
        for (int k = 0; k < n.links.Num(); k++) {
            if (n.links[k].fails < 3) {
                f->Printf(" %d %.0f %d", n.links[k].to, n.links[k].cost, (int)n.links[k].learned);
            }
        }
        f->Printf("\n");
        links += kept;
    }
    fileSystem->CloseFile(f);
    gameLocal.Printf("bots: navigation graph saved to %s - %d nodes, %d links\n", path.c_str(), botNav.nodes.Num(), links);
}

// the map's items and spawn points: the first nodes
static void BotNav_Seed()
{
    int seeded = 0;
    for (idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next()) {
        if (ent->IsType(idPlayerStart::Type) || ent->IsType(idItem::Type)) {
            if (BotNav_AddNode(ent->GetPhysics()->GetOrigin(), true) >= 0) {
                seeded++;
            }
        }
    }
    gameLocal.Printf("bots: navigation graph for %s started from %d items and spawn points (%d nodes); walkable links are tested in the background and players' paths are learned\n",
        botNav.map.c_str(), seeded, botNav.nodes.Num());
}

static void BotNav_MapCheck()
{
    const char* map = gameLocal.GetMapFileName();
    if (map == NULL || !map[0]) {
        return;
    }
    if (botNav.map.Icmp(map) == 0) {
        return;
    }
    if (!botNav.map.IsEmpty()) {
        BotNav_Save();
    }
    BotNav_Clear(map);
    if (!BotNav_Load(map)) {
        BotNav_Seed();
    }
}

static void BotNav_TestPair(int i, int j)
{
    const idVec3& a = botNav.nodes[i].pos;
    const idVec3& b = botNav.nodes[j].pos;
    const float d2 = (b - a).ToVec2().LengthSqr();
    botNav.tested++;
    const float cost = idMath::Sqrt(d2) + idMath::Fabs(b.z - a.z);
    if (BotNav_FindLink(i, j) == NULL && BotNav_Walkable(a, b)) {
        BotNav_Link(i, j, cost, 0);
    }
    if (BotNav_FindLink(j, i) == NULL && BotNav_Walkable(b, a)) {
        BotNav_Link(j, i, cost, 0);
    }
}

static bool BotNav_PairWorthTesting(int i, int j)
{
    const idVec3& a = botNav.nodes[i].pos;
    const idVec3& b = botNav.nodes[j].pos;
    if ((b - a).ToVec2().LengthSqr() > 900.0f * 900.0f || idMath::Fabs(b.z - a.z) > 256.0f) {
        return false;
    }
    return BotNav_FindLink(i, j) == NULL || BotNav_FindLink(j, i) == NULL;
}

// a few pairs a frame: link nodes a walk joins
static void BotNav_Linker()
{
    if (botNav.linkerDone) {
        // nodes added since the first sweep, each against every other one
        int budget = bot_navLinksPerFrame.GetInteger();
        while (budget > 0 && botNav.pending.Num() > 0) {
            const int i = botNav.pending[0];
            if (i >= botNav.nodes.Num() || botNav.pendingJ >= botNav.nodes.Num()) {
                botNav.pending.RemoveIndex(0);
                botNav.pendingJ = 0;
                continue;
            }
            const int j = botNav.pendingJ++;
            if (j == i || !BotNav_PairWorthTesting(i, j)) {
                continue;
            }
            budget--;
            BotNav_TestPair(i, j);
        }
        return;
    }
    const int n = botNav.nodes.Num();
    int budget = bot_navLinksPerFrame.GetInteger();
    while (budget > 0) {
        if (botNav.pairI >= n - 1) {
            botNav.linkerDone = true;
            botNav.pairI = 0;
            botNav.pairJ = 1;
            int links = 0;
            for (int i = 0; i < n; i++) {
                links += botNav.nodes[i].links.Num();
            }
            gameLocal.Printf("bots: navigation graph for %s - every pair tested: %d nodes, %d links (%d walkable, %d learned from players, %d teleports; %d pairs tested)\n",
                botNav.map.c_str(), n, links, botNav.linkedWalk, botNav.linkedLearned, botNav.linkedTeleport, botNav.tested);
            return;
        }
        const int i = botNav.pairI, j = botNav.pairJ;
        if (++botNav.pairJ >= n) {
            botNav.pairI++;
            botNav.pairJ = botNav.pairI + 1;
        }
        if (!BotNav_PairWorthTesting(i, j)) {
            continue;
        }
        budget--;
        BotNav_TestPair(i, j);
    }
}

// looking around from nodes: eight directions over open floor, a node
// where there is room and none yet (big maps' items are far apart: walks
// between them alone leave the graph in pieces)
static void BotNav_Explore()
{
    int budget = bot_navExplorePerFrame.GetInteger();
    trace_t tr;
    while (budget > 0 && botNav.explore.Num() > 0 && botNav.nodes.Num() < BOTNAV_MAX_NODES - 64) {
        const int from = botNav.explore[0];
        botNav.explore.RemoveIndex(0);
        if (from < 0 || from >= botNav.nodes.Num()) {
            continue;
        }
        budget--;
        botNav.explored++;
        const idVec3 base = botNav.nodes[from].pos;
        for (int k = 0; k < 8; k++) {
            const idVec3 dir = idAngles(0.0f, k * 45.0f, 0.0f).ToForward();
            const idVec3 eye = base + idVec3(0, 0, 40);
            gameLocal.clip.TracePoint(tr, eye, eye + dir * 320.0f, MASK_PLAYERSOLID, NULL);
            const float room = tr.fraction * 320.0f;
            if (room < 176.0f) {
                continue;
            }
            const idVec3 at = base + dir * Min(room - 48.0f, 256.0f);
            gameLocal.clip.TracePoint(tr, at + idVec3(0, 0, 40), at - idVec3(0, 0, 72), MASK_PLAYERSOLID, NULL);
            if (tr.fraction >= 1.0f || tr.c.normal.z < 0.7f) {
                continue; // no floor (a drop) or too steep
            }
            const idVec3 floor = tr.endpos;
            if (BotNav_Nearest(floor, 128.0f) >= 0) {
                continue;
            }
            if (!BotNav_Walkable(base, floor)) {
                continue;
            }
            const int added = BotNav_AddNode(floor, false);
            if (added >= 0) {
                botNav.exploreAdded++;
                const float cost = (floor - base).Length();
                BotNav_Link(from, added, cost, 0);
                if (BotNav_Walkable(floor, base)) {
                    BotNav_Link(added, from, cost, 0);
                }
            }
        }
    }
}

// where everyone walks: nodes and links learned from players, people and bots
static void BotNav_Learn()
{
    const int now = gameLocal.time;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        idEntity* ent = gameLocal.entities[i];
        if (ent == NULL || !ent->IsType(idPlayer::Type)) {
            botNav.lastNode[i] = -1;
            continue;
        }
        idPlayer* p = static_cast<idPlayer*>(ent);
        if (p->health <= 0 || p->spectating) {
            botNav.lastNode[i] = -1;
            continue;
        }
        const idVec3 pos = p->GetPhysics()->GetOrigin();
        const bool jumpedFar = (pos - botNav.lastPos[i]).LengthSqr() > 300.0f * 300.0f && botNav.lastPos[i] != vec3_origin;
        botNav.lastPos[i] = pos;
        if (!p->GetPhysics()->HasGroundContacts()) {
            continue; // in the air: the landing will be linked
        }
        int node = BotNav_Nearest(pos, 96.0f);
        if (node < 0 && BotNav_Nearest(pos, 160.0f) < 0) {
            node = BotNav_AddNode(pos, false);
        }
        if (node < 0) {
            continue;
        }
        const int last = botNav.lastNode[i];
        if (last >= 0 && last != node && last < botNav.nodes.Num()) {
            const int took = now - botNav.lastNodeMs[i];
            if (jumpedFar && took < 1500) {
                BotNav_Link(last, node, 64.0f, 2); // a teleporter (or a respawn: those have no "last")
            } else if (took < 4000) {
                const float dist = (botNav.nodes[node].pos - botNav.nodes[last].pos).Length();
                BotNav_Link(last, node, Max(dist, took * 0.3f), 1);
            }
        }
        botNav.lastNode[i] = node;
        botNav.lastNodeMs[i] = now;
    }
}

// A* over the graph; the path from start to goal, start first
static bool BotNav_Path(int start, int goal, idList<int>& path)
{
    path.Clear();
    botNav.plans++;
    const int n = botNav.nodes.Num();
    if (start < 0 || goal < 0 || start >= n || goal >= n) {
        botNav.planFails++;
        return false;
    }
    if (start == goal) {
        path.Append(goal);
        return true;
    }
    static idList<float> g;
    static idList<int> from;
    static idList<byte> closed;
    static idList<int> heap; // node indices, ordered by f
    static idList<float> f;
    g.SetNum(n);
    from.SetNum(n);
    closed.SetNum(n);
    f.SetNum(n);
    for (int i = 0; i < n; i++) {
        g[i] = 1e30f;
        from[i] = -1;
        closed[i] = 0;
    }
    heap.SetNum(0);
    const idVec3 goalPos = botNav.nodes[goal].pos;
    auto push = [&](int v) {
        heap.Append(v);
        int c = heap.Num() - 1;
        while (c > 0) {
            const int parent = (c - 1) / 2;
            if (f[heap[parent]] <= f[heap[c]]) {
                break;
            }
            { const int t = heap[parent]; heap[parent] = heap[c]; heap[c] = t; }
            c = parent;
        }
    };
    auto pop = [&]() {
        const int top = heap[0];
        heap[0] = heap[heap.Num() - 1];
        heap.SetNum(heap.Num() - 1);
        int c = 0;
        while (true) {
            const int l = c * 2 + 1, r = l + 1;
            int m = c;
            if (l < heap.Num() && f[heap[l]] < f[heap[m]]) {
                m = l;
            }
            if (r < heap.Num() && f[heap[r]] < f[heap[m]]) {
                m = r;
            }
            if (m == c) {
                break;
            }
            { const int t = heap[m]; heap[m] = heap[c]; heap[c] = t; }
            c = m;
        }
        return top;
    };
    g[start] = 0.0f;
    f[start] = (botNav.nodes[start].pos - goalPos).Length();
    push(start);
    int expanded = 0;
    while (heap.Num() > 0 && expanded < 4000) {
        const int v = pop();
        if (closed[v]) {
            continue;
        }
        closed[v] = 1;
        expanded++;
        if (v == goal) {
            for (int c = goal; c >= 0; c = from[c]) {
                path.Insert(c, 0);
            }
            return true;
        }
        const idList<navLink_t>& links = botNav.nodes[v].links;
        for (int k = 0; k < links.Num(); k++) {
            const navLink_t& l = links[k];
            if (l.fails >= 3 || closed[l.to]) {
                continue;
            }
            const float ng = g[v] + l.cost * (1.0f + l.fails * 2.0f);
            if (ng < g[l.to]) {
                g[l.to] = ng;
                from[l.to] = v;
                f[l.to] = ng + (botNav.nodes[l.to].pos - goalPos).Length();
                push(l.to);
            }
        }
    }
    botNav.planFails++;
    return false;
}

static void BotNav_Report()
{
    const int now = gameLocal.time;
    if (now - botNav.lastReportMs < 60000) {
        return;
    }
    botNav.lastReportMs = now;
    int links = 0, learnedNodes = 0;
    for (int i = 0; i < botNav.nodes.Num(); i++) {
        links += botNav.nodes[i].links.Num();
        learnedNodes += botNav.nodes[i].seeded ? 0 : 1;
    }
    gameLocal.Printf("bots: navigation graph %s - %d nodes (%d not items or spawns: %d explored over open floor, the rest learned from players), %d links (%d walkable, %d learned, %d teleports, %d dropped as failing); pairs tested %d%s, %d nodes waiting to be linked, %d to explore from; paths planned %d, %d found none\n",
        botNav.map.c_str(), botNav.nodes.Num(), learnedNodes, botNav.exploreAdded, links, botNav.linkedWalk, botNav.linkedLearned, botNav.linkedTeleport, botNav.dropped,
        botNav.tested, botNav.linkerDone ? " (first sweep done)" : "", botNav.pending.Num(), botNav.explore.Num(), botNav.plans, botNav.planFails);
}

/*
================================================================================

The bots

================================================================================
*/
enum botState_t { BS_ROAM, BS_ITEM, BS_FIGHT, BS_RETREAT, BS_CHASE, BS_FLAG, BS_DEAD, BS_NUM };
static const char* const botStateNames[BS_NUM] = { "roam", "items", "fight", "retreat", "chase", "flag", "dead" };

struct vitaBot_t {
    bool active;
    bool managed;        // added by the host for bot_count (removed by it too)
    int skill;           // 0: bot_skill
    // view (Q3's viewangles / ideal_viewangles)
    float yaw, pitch;
    float idealYaw, idealPitch;
    // enemy
    int enemy;           // client number, -1 none
    int enemySightMs;    // when it was first seen (reaction time)
    int enemyLastSeenMs;
    idVec3 enemyLastPos;
    idVec3 enemyPosMemo;     // remembered every half second
    idVec3 enemyVelMemo;
    int enemyMemoMs;
    idVec3 enemyPrevPos;     // last tic: velocity now
    int enemyPrevMs;
    idVec3 aimTarget;
    int lastHealth;
    bool hurt;
    int nextEnemyScanMs;
    // attack
    int fireThrottleWaitMs, fireThrottleShootMs;
    int weaponChangeMs;
    bool strafeRight;
    float strafeTime;
    int backOffUntilMs;
    int crouchUntilMs, jumpUntilMs;
    int chaseUntilMs;
    // goals and paths
    int state;
    idVec3 goal;
    int goalNode;
    int goalUntilMs;
    idList<int> path;
    int pathIndex;
    int progressMs;
    float progressDist;
    int replanMs;
    int stuckCount;
    idVec3 lastPos;
    int lastMoveCheckMs;
    int respawnPress;
    int lastThinkMs;
    // stats (the log and the verification)
    float travelled;
    idVec3 prevPos;
    int shots;
    int stateMs[BS_NUM];
    int statsSinceMs;
    int paths, pathFails;
};

static vitaBot_t vitaBots[MAX_PLAYERS];

bool VitaBot_IsBot(int clientNum)
{
    if (!common->IsMultiplayer() || common->IsClient() || clientNum < 0 || clientNum >= MAX_PLAYERS) {
        return false;
    }
    const lobbyUserID_t& id = gameLocal.lobbyUserIDs[clientNum];
    return id.IsValid() && session->GetActingGameStateLobbyBase().GetLobbyUserIsBot(id);
}

static int VitaBot_Skill(const vitaBot_t& b)
{
    return b.skill > 0 ? b.skill : bot_skill.GetInteger();
}

static bool VitaBot_SameTeam(idPlayer* a, idPlayer* b)
{
    return gameLocal.mpGame.IsGametypeTeamBased() && a->team == b->team;
}

static idVec3 VitaBot_Center(idPlayer* p)
{
    return p->GetPhysics()->GetAbsBounds().GetCenter();
}

static bool VitaBot_Visible(idPlayer* self, idPlayer* other)
{
    const idVec3 eye = self->GetEyePosition();
    trace_t tr;
    const idVec3 points[2] = { VitaBot_Center(other), other->GetEyePosition() };
    for (int k = 0; k < 2; k++) {
        gameLocal.clip.TracePoint(tr, eye, points[k], MASK_SOLID | CONTENTS_OPAQUE, self);
        if (tr.fraction >= 1.0f || gameLocal.GetTraceEntity(tr) == other) {
            return true;
        }
    }
    return false;
}

static bool VitaBot_InFov(float viewYaw, float viewPitch, const idVec3& dir, float fov)
{
    const idAngles a = dir.ToAngles();
    return idMath::Fabs(idMath::AngleNormalize180(a.yaw - viewYaw)) < fov * 0.5f
        && idMath::Fabs(idMath::AngleNormalize180(a.pitch - viewPitch)) < fov * 0.5f;
}

// Q3's BotAggression
static float VitaBot_Aggression(idPlayer* self)
{
    const int health = self->health, armor = self->inventory.armor;
    if (health < 60) {
        return 0.0f;
    }
    if (health < 80 && armor < 40) {
        return 0.0f;
    }
    float best = 0.0f;
    for (int i = 0; i < MAX_WEAPONS; i++) {
        const int ammo = VitaBot_Ammo(self, i);
        if (ammo <= 0) {
            continue;
        }
        switch (vitaBotWeapons[i].kind) {
        case BW_BFG:
            best = Max(best, ammo > 1 ? 100.0f : 0.0f);
            break;
        case BW_ROCKET:
            best = Max(best, ammo > 5 ? 90.0f : 0.0f);
            break;
        case BW_PLASMA:
            best = Max(best, ammo > 40 ? 85.0f : 0.0f);
            break;
        case BW_CHAINGUN:
            best = Max(best, ammo > 50 ? 80.0f : 0.0f);
            break;
        case BW_SHOTGUN_DOUBLE:
            best = Max(best, ammo > 8 ? 70.0f : 0.0f);
            break;
        case BW_SHOTGUN:
            best = Max(best, ammo > 10 ? 50.0f : 0.0f);
            break;
        case BW_MACHINEGUN:
            best = Max(best, ammo > 40 ? 60.0f : 0.0f); // DOOM 3's spawn weapon: worth a fight (Q3's machinegun is not)
            break;
        case BW_PISTOL:
            best = Max(best, ammo > 20 ? 40.0f : 0.0f);
            break;
        }
    }
    return best;
}

// Q3's BotFindEnemy
static int VitaBot_FindEnemy(idPlayer* self, vitaBot_t& b, const botChar_t& ch)
{
    const idVec3 origin = self->GetPhysics()->GetOrigin();
    float curDist = 1e30f;
    if (b.enemy >= 0) {
        idEntity* cur = gameLocal.entities[b.enemy];
        if (cur != NULL && cur->IsType(idPlayer::Type)) {
            curDist = (cur->GetPhysics()->GetOrigin() - origin).LengthSqr();
        }
    }
    const float maxDist = 900.0f + ch.alertness * 4000.0f;
    int best = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        idEntity* ent = gameLocal.entities[i];
        if (ent == NULL || ent == self || !ent->IsType(idPlayer::Type)) {
            continue;
        }
        idPlayer* p = static_cast<idPlayer*>(ent);
        if (p->health <= 0 || p->spectating || p->IsHidden() || VitaBot_SameTeam(self, p)) {
            continue;
        }
        const bool shooting = (p->usercmd.buttons & BUTTON_ATTACK) != 0;
        if (p->PowerUpActive(INVISIBILITY) && !shooting) {
            continue;
        }
        const idVec3 dir = VitaBot_Center(p) - self->GetEyePosition();
        const float d2 = dir.LengthSqr();
        if (b.enemy >= 0 && d2 > curDist) {
            continue; // the current one is nearer
        }
        if (d2 > maxDist * maxDist) {
            continue;
        }
        float fov;
        if (b.enemy < 0 && (b.hurt || shooting)) {
            fov = 360.0f;
        } else {
            const float d = idMath::Sqrt(d2);
            fov = 120.0f + 60.0f * (1.0f - idMath::ClampFloat(0.0f, 1.0f, d / 810.0f));
        }
        if (fov < 360.0f && !VitaBot_InFov(b.yaw, b.pitch, dir, fov)) {
            continue;
        }
        if (!VitaBot_Visible(self, p)) {
            continue;
        }
        // far, not shooting, the bot not hurt and not in the enemy's view:
        // let it go if the bot does not want to fight (Q3)
        if (b.enemy < 0 && d2 > 100.0f * 100.0f && !b.hurt && !shooting) {
            const idVec3 back = self->GetEyePosition() - p->GetEyePosition();
            if (!VitaBot_InFov(p->viewAngles.yaw, p->viewAngles.pitch, back, 90.0f) && VitaBot_Aggression(self) < 50.0f) {
                continue;
            }
        }
        best = i;
        curDist = d2;
    }
    return best;
}

// Q3's BotChooseWeapon
static void VitaBot_ChooseWeapon(idPlayer* self, vitaBot_t& b, float dist)
{
    const int now = gameLocal.time;
    if (now - b.weaponChangeMs < 1500) {
        return;
    }
    const int cur = self->GetCurrentWeaponSlot();
    float curW = -1.0f;
    int best = -1;
    float bestW = -1.0f;
    for (int i = 0; i < MAX_WEAPONS; i++) {
        if (VitaBot_Ammo(self, i) <= 0) {
            continue;
        }
        const float w = VitaBot_WeaponWeight(vitaBotWeapons[i].kind, dist);
        if (i == cur) {
            curW = w;
        }
        if (w > bestW) {
            bestW = w;
            best = i;
        }
    }
    if (best >= 0 && best != cur && bestW > curW + 0.1f) {
        if (self->VitaBotSelectWeapon(best)) {
            b.weaponChangeMs = now;
            if (bot_debug.GetBool()) {
                gameLocal.Printf("bot %d: weapon %s (weight %.2f at %.0f units)\n", self->entityNumber, vitaBotWeapons[best].def.c_str(), bestW, dist);
            }
        }
    }
}

// Q3's BotAimAtEnemy: sets the ideal view
static void VitaBot_AimAtEnemy(idPlayer* self, vitaBot_t& b, const botChar_t& ch, idPlayer* e, bool visible)
{
    const int now = gameLocal.time;
    float aimSkill = ch.aimSkill, aimAccuracy = ch.aimAccuracy;
    if (aimSkill > 0.95f && now - b.enemySightMs < (int)(500.0f * ch.reactionTime)) {
        return; // don't aim too early
    }
    const int weapon = self->GetCurrentWeaponSlot();
    const botWeapon_t& w = vitaBotWeapons[idMath::ClampInt(0, MAX_WEAPONS - 1, weapon)];
    if (e->PowerUpActive(INVISIBILITY) && vitaBotRandom.RandomFloat() > 0.1f) {
        aimAccuracy *= 0.4f;
    }
    const idVec3 enemyPos = e->GetPhysics()->GetOrigin();
    // the enemy's velocity now, and remembered every half second
    idVec3 vel(0, 0, 0);
    if (b.enemyPrevMs > 0 && now > b.enemyPrevMs) {
        vel = (enemyPos - b.enemyPrevPos) * (1000.0f / (now - b.enemyPrevMs));
    }
    b.enemyPrevPos = enemyPos;
    b.enemyPrevMs = now;
    if (b.enemyMemoMs < now) {
        b.enemyMemoMs = now + 500;
        b.enemyVelMemo = vel;
        b.enemyPosMemo = enemyPos;
    }
    if (aimSkill < 0.9f && (enemyPos - b.enemyPosMemo).LengthSqr() > 48.0f * 48.0f && (b.enemyVelMemo * vel) < 0.0f) {
        aimAccuracy *= 0.7f; // the enemy changed direction
    }
    const idVec3 eye = self->GetEyePosition();
    idVec3 best;
    if (visible) {
        best = VitaBot_Center(e);
        // projectiles: lead the enemy
        if (w.speed > 0.0f) {
            const float dist = (best - eye).Length();
            const bool smallSteps = dist > 100.0f && (enemyPos - b.enemyPosMemo).LengthSqr() < 32.0f * 32.0f;
            if (!smallSteps) {
                if (aimSkill > 0.8f) {
                    // where it will be when the shot gets there (two rounds)
                    float t = dist / w.speed;
                    for (int k = 0; k < 2; k++) {
                        const idVec3 at = best + vel * t;
                        t = (at - eye).Length() / w.speed;
                    }
                    idVec3 lead = vel * t;
                    if (e->GetPhysics()->HasGroundContacts()) {
                        lead.z = 0.0f;
                    }
                    best += lead;
                } else if (aimSkill > 0.4f) {
                    idVec3 flat = vel;
                    flat.z = 0.0f;
                    best += flat * (dist / w.speed);
                }
            }
        }
        // splash: the ground at its feet
        if (aimSkill > 0.6f && w.splashRadius > 0.0f && enemyPos.z < self->GetPhysics()->GetOrigin().z + 16.0f) {
            trace_t tr;
            gameLocal.clip.TracePoint(tr, enemyPos + idVec3(0, 0, 8), enemyPos - idVec3(0, 0, 64), MASK_SHOT_RENDERMODEL, e);
            const idVec3 ground(best.x, best.y, tr.fraction < 1.0f ? tr.endpos.z + 2.0f : enemyPos.z);
            trace_t line;
            gameLocal.clip.TracePoint(line, eye, ground, MASK_SHOT_RENDERMODEL, self);
            if (idMath::Fabs(line.endpos.z - ground.z) < 50.0f && (line.endpos - ground).LengthSqr() < 60.0f * 60.0f
                && (line.endpos - eye).LengthSqr() > Square(Max(100.0f, w.splashRadius))) {
                best = ground;
            }
        }
        best.x += 20.0f * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy);
        best.y += 20.0f * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy);
        best.z += 10.0f * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy);
    } else {
        best = b.enemyLastPos + idVec3(0, 0, 40);
        if (aimSkill > 0.5f && w.splashRadius > 0.0f) {
            aimAccuracy = 1.0f; // a shot round the corner, where it went
        }
    }
    b.aimTarget = best;
    idVec3 dir = best - eye;
    if (w.kind == BW_MACHINEGUN || w.kind == BW_CHAINGUN || w.kind == BW_SHOTGUN || w.kind == BW_SHOTGUN_DOUBLE || w.kind == BW_PISTOL) {
        const float dist = Min(dir.Length(), 150.0f);
        aimAccuracy *= 0.6f + dist / 150.0f * 0.4f;
    }
    if (aimAccuracy < 0.8f) {
        dir.Normalize();
        for (int k = 0; k < 3; k++) {
            dir[k] += 0.3f * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy);
        }
    }
    const idAngles a = dir.ToAngles();
    const float spread = Max(w.spread, 1.0f);
    b.idealPitch = idMath::AngleNormalize180(a.pitch + 6.0f * spread * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy) * 0.25f);
    b.idealYaw = idMath::AngleNormalize180(a.yaw + 6.0f * spread * vitaBotRandom.CRandomFloat() * (1.0f - aimAccuracy) * 0.25f);
}

// Q3's BotChangeViewAngles (the smooth "challenge" model), per tic
static void VitaBot_ChangeView(vitaBot_t& b, const botChar_t& ch, bool fighting, float dt)
{
    const float factor = fighting ? ch.viewFactor : 0.25f;
    const float maxChange = Max(fighting ? ch.viewMaxChange : 360.0f, 240.0f) * dt;
    const float share = 1.0f - powf(1.0f - factor, dt / 0.1f); // factor per tenth of a second
    float* view[2] = { &b.pitch, &b.yaw };
    const float ideal[2] = { b.idealPitch, b.idealYaw };
    for (int i = 0; i < 2; i++) {
        const float diff = idMath::AngleNormalize180(ideal[i] - *view[i]);
        const float step = idMath::ClampFloat(-maxChange, maxChange, diff * share);
        *view[i] = idMath::AngleNormalize180(*view[i] + step);
    }
    b.pitch = idMath::ClampFloat(-89.0f, 89.0f, b.pitch);
}

// Q3's BotCheckAttack: true to fire this tic
static bool VitaBot_CheckAttack(idPlayer* self, vitaBot_t& b, const botChar_t& ch, idPlayer* e)
{
    const int now = gameLocal.time;
    if (now - b.enemySightMs < (int)(1000.0f * ch.reactionTime)) {
        return false;
    }
    if (now - b.weaponChangeMs < 100) {
        return false;
    }
    // fire throttle
    if (b.fireThrottleWaitMs > now) {
        return false;
    }
    if (b.fireThrottleShootMs < now) {
        if (vitaBotRandom.RandomFloat() > ch.fireThrottle) {
            b.fireThrottleWaitMs = now + (int)(1000.0f * ch.fireThrottle);
            b.fireThrottleShootMs = 0;
            return false;
        }
        b.fireThrottleShootMs = now + (int)(1000.0f * (1.0f - ch.fireThrottle)) + 100;
        b.fireThrottleWaitMs = 0;
    }
    const idVec3 eye = self->GetEyePosition();
    const idVec3 dir = b.aimTarget - eye;
    const int weapon = self->GetCurrentWeaponSlot();
    const botWeapon_t& w = vitaBotWeapons[idMath::ClampInt(0, MAX_WEAPONS - 1, weapon)];
    if ((w.kind == BW_FISTS || w.kind == BW_CHAINSAW) && dir.LengthSqr() > 80.0f * 80.0f) {
        return false;
    }
    const float fov = dir.LengthSqr() < 100.0f * 100.0f ? 120.0f : 50.0f;
    if (!VitaBot_InFov(b.yaw, b.pitch, dir, fov)) {
        return false;
    }
    trace_t tr;
    gameLocal.clip.TracePoint(tr, eye, b.aimTarget, MASK_SOLID, self);
    if (tr.fraction < 1.0f && gameLocal.GetTraceEntity(tr) != e) {
        return false;
    }
    // along the view: no teammate in the way, no splash in the bot's face
    const idVec3 forward = idAngles(b.pitch, b.yaw, 0.0f).ToForward();
    gameLocal.clip.TracePoint(tr, eye - forward * 12.0f, eye + forward * 1000.0f, MASK_SHOT_RENDERMODEL, self);
    idEntity* hit = gameLocal.GetTraceEntity(tr);
    if (hit != NULL && hit != e && hit->IsType(idPlayer::Type) && VitaBot_SameTeam(self, static_cast<idPlayer*>(hit))) {
        return false;
    }
    if (hit != e && w.splashRadius > 0.0f && tr.fraction * 1000.0f < w.splashRadius) {
        return false;
    }
    return true;
}

// a direction on the floor the bot may take (no wall close, no drop)
static float VitaBot_Feel(idPlayer* self, float yaw, float dist)
{
    const idVec3 o = self->GetPhysics()->GetOrigin();
    const idVec3 dir = idAngles(0.0f, yaw, 0.0f).ToForward();
    trace_t tr;
    float best = dist;
    for (int h = 0; h < 2; h++) {
        const idVec3 from = o + idVec3(0.0f, 0.0f, h == 0 ? 24.0f : 56.0f);
        gameLocal.clip.TracePoint(tr, from, from + dir * dist, MASK_PLAYERSOLID, self);
        best = Min(best, tr.fraction * dist);
    }
    const idVec3 ahead = o + dir * Min(best, 96.0f) + idVec3(0.0f, 0.0f, 16.0f);
    gameLocal.clip.TracePoint(tr, ahead, ahead - idVec3(0.0f, 0.0f, 160.0f), MASK_PLAYERSOLID, self);
    if (tr.fraction >= 1.0f) {
        best = Min(best, 32.0f);
    }
    return best;
}

// a world direction into the command's forward / right moves
static void VitaBot_MoveDir(const vitaBot_t& b, const idVec3& dir, usercmd_t& cmd)
{
    idVec3 flat(dir.x, dir.y, 0.0f);
    if (flat.Normalize() < 0.001f) {
        return;
    }
    const idVec3 fwd = idAngles(0.0f, b.yaw, 0.0f).ToForward();
    const idVec3 right(fwd.y, -fwd.x, 0.0f);
    cmd.forwardmove = (signed char)idMath::ClampInt(-127, 127, (int)(127.0f * (flat * fwd)));
    cmd.rightmove = (signed char)idMath::ClampInt(-127, 127, (int)(127.0f * (flat * right)));
}

// Q3's BotAttackMove
static void VitaBot_AttackMove(idPlayer* self, vitaBot_t& b, const botChar_t& ch, idPlayer* e, float dt, usercmd_t& cmd)
{
    const int now = gameLocal.time;
    if (ch.attackSkill < 0.2f) {
        return; // really stupid: stands
    }
    idVec3 forward = e->GetPhysics()->GetOrigin() - self->GetPhysics()->GetOrigin();
    forward.z = 0.0f;
    const float dist = forward.Normalize();
    const idVec3 backward = -forward;
    // walk, crouch or jump
    if (b.crouchUntilMs < now - 1000) {
        if (vitaBotRandom.RandomFloat() < VitaBot_Chance(ch.jumper, dt) && b.jumpUntilMs < now) {
            cmd.buttons |= BUTTON_JUMP;
            b.jumpUntilMs = now + 1000;
        } else if (vitaBotRandom.RandomFloat() < VitaBot_Chance(ch.croucher, dt)) {
            b.crouchUntilMs = now + (int)(ch.croucher * 5000.0f);
        }
    }
    if (b.crouchUntilMs > now) {
        cmd.buttons |= BUTTON_CROUCH;
    }
    float attackDist, attackRange;
    VitaBot_AttackDistance(vitaBotWeapons[idMath::ClampInt(0, MAX_WEAPONS - 1, self->GetCurrentWeaponSlot())].kind, attackDist, attackRange);
    if (ch.attackSkill <= 0.4f) {
        // stupid: just walk to or away from the enemy
        if (dist > attackDist + attackRange) {
            VitaBot_MoveDir(b, forward, cmd);
        } else if (dist < attackDist - attackRange) {
            VitaBot_MoveDir(b, backward, cmd);
        }
        return;
    }
    // strafe: the direction is kept a while, then flipped at random
    b.strafeTime += dt;
    float strafeChange = 0.4f + (1.0f - ch.attackSkill) * 0.2f;
    if (ch.attackSkill > 0.7f) {
        strafeChange += vitaBotRandom.CRandomFloat() * 0.2f;
    }
    if (b.strafeTime > strafeChange && vitaBotRandom.RandomFloat() < VitaBot_Chance(0.065f, dt)) {
        b.strafeRight = !b.strafeRight;
        b.strafeTime = 0.0f;
    }
    if (b.backOffUntilMs < now && vitaBotRandom.RandomFloat() < VitaBot_Chance(0.1f, dt)) {
        b.backOffUntilMs = now + 300; // randomly go back a little
    }
    for (int i = 0; i < 2; i++) {
        idVec3 sideward(-forward.y, forward.x, 0.0f);
        if (b.strafeRight) {
            sideward = -sideward;
        }
        if (b.backOffUntilMs > now) {
            sideward += backward;
        } else if (dist > attackDist + attackRange) {
            sideward += forward;
        } else if (dist < attackDist - attackRange) {
            sideward += backward;
        }
        sideward.Normalize();
        if (VitaBot_Feel(self, sideward.ToYaw(), 64.0f) >= 40.0f) {
            VitaBot_MoveDir(b, sideward, cmd);
            return;
        }
        // movement failed: flip the strafe direction
        b.strafeRight = !b.strafeRight;
        b.strafeTime = 0.0f;
    }
}

/*
--------------------------------------------------------------------------------
Long term goals
--------------------------------------------------------------------------------
*/
static float VitaBot_ItemValue(idPlayer* self, idEntity* ent)
{
    const idDict& args = ent->spawnArgs;
    float v = 0.0f;
    const int health = args.GetInt("inv_health");
    if (health > 0 && self->health < 100) {
        v = Max(v, (100 - self->health) / 100.0f + Min(health, 100) / 300.0f);
    }
    const int armor = args.GetInt("inv_armor");
    if (armor > 0 && self->inventory.armor < self->inventory.maxarmor) {
        v = Max(v, 0.7f * (self->inventory.maxarmor - self->inventory.armor) / (float)Max(1, self->inventory.maxarmor) + 0.1f);
    }
    const char* weapon = args.GetString("inv_weapon");
    if (weapon[0]) {
        for (int i = 0; i < MAX_WEAPONS; i++) {
            if (vitaBotWeapons[i].known && vitaBotWeapons[i].def.Icmp(weapon) == 0) {
                if (!(self->inventory.weapons & (1 << i))) {
                    v = Max(v, 0.3f + VitaBot_WeaponWeight(vitaBotWeapons[i].kind, 400.0f));
                } else if (VitaBot_Ammo(self, i) < 20) {
                    v = Max(v, 0.3f);
                }
            }
        }
    }
    if (args.MatchPrefix("inv_ammo_") != NULL && v < 0.15f) {
        v = Max(v, 0.15f);
    }
    if (ent->IsType(idItemPowerup::Type)) {
        v = Max(v, 0.9f);
    }
    return v;
}

static void VitaBot_SetGoal(idPlayer* self, vitaBot_t& b, const idVec3& goal, int state, int forMs)
{
    b.goal = goal;
    b.state = state;
    b.goalUntilMs = gameLocal.time + forMs;
    b.goalNode = BotNav_Nearest(goal, 256.0f);
    b.path.Clear();
    b.pathIndex = 0;
    b.replanMs = 0;
}

static bool VitaBot_PickItemGoal(idPlayer* self, vitaBot_t& b, bool needHealth)
{
    const idVec3 pos = self->GetPhysics()->GetOrigin();
    idEntity* bestEnt = NULL;
    float best = 0.05f;
    for (idEntity* ent = gameLocal.spawnedEntities.Next(); ent != NULL; ent = ent->spawnNode.Next()) {
        if (!ent->IsType(idItem::Type) || ent->IsType(idItemTeam::Type) || ent->IsHidden()) {
            continue;
        }
        float v = VitaBot_ItemValue(self, ent);
        if (needHealth && ent->spawnArgs.GetInt("inv_health") <= 0 && ent->spawnArgs.GetInt("inv_armor") <= 0) {
            v *= 0.3f;
        }
        const float d = (ent->GetPhysics()->GetOrigin() - pos).Length();
        const float u = v / (1.0f + d / 800.0f);
        if (u > best) {
            best = u;
            bestEnt = ent;
        }
    }
    if (bestEnt == NULL) {
        return false;
    }
    VitaBot_SetGoal(self, b, bestEnt->GetPhysics()->GetOrigin(), BS_ITEM, 15000);
    return true;
}

// CTF: the flag to take, to bring home or to defend (true when it set a goal)
static bool VitaBot_FlagGoal(idPlayer* self, vitaBot_t& b)
{
    if (!gameLocal.mpGame.IsGametypeFlagBased()) {
        return false;
    }
    const int myTeam = self->team, enemyTeam = 1 - self->team;
    idItemTeam* myFlag = gameLocal.mpGame.GetTeamFlag(myTeam);
    idItemTeam* enemyFlag = gameLocal.mpGame.GetTeamFlag(enemyTeam);
    if (myFlag == NULL || enemyFlag == NULL) {
        return false;
    }
    if (self->carryingFlag) {
        // home with it (Q3: always retreat with the flag)
        VitaBot_SetGoal(self, b, myFlag->GetPhysics()->GetOrigin(), BS_FLAG, 20000);
        return true;
    }
    const bool attacker = (self->entityNumber % 2) == 0;
    if (myFlag->carried) {
        // our flag is away: go for its carrier
        const int carrier = gameLocal.mpGame.GetFlagCarrier(enemyTeam);
        if (carrier >= 0 && gameLocal.entities[carrier] != NULL) {
            VitaBot_SetGoal(self, b, gameLocal.entities[carrier]->GetPhysics()->GetOrigin(), BS_FLAG, 4000);
            return true;
        }
    }
    if (myFlag->dropped) {
        VitaBot_SetGoal(self, b, myFlag->GetPhysics()->GetOrigin(), BS_FLAG, 8000); // touch it: it goes home
        return true;
    }
    if (attacker && !enemyFlag->carried) {
        VitaBot_SetGoal(self, b, enemyFlag->GetPhysics()->GetOrigin(), BS_FLAG, 20000);
        return true;
    }
    if (!attacker) {
        // a defender: near the own flag
        const idVec3 base = myFlag->GetPhysics()->GetOrigin();
        if ((self->GetPhysics()->GetOrigin() - base).LengthSqr() > 600.0f * 600.0f) {
            VitaBot_SetGoal(self, b, base, BS_FLAG, 10000);
            return true;
        }
    }
    return false;
}

static void VitaBot_PickGoal(idPlayer* self, vitaBot_t& b)
{
    if (VitaBot_FlagGoal(self, b)) {
        return;
    }
    // an enemy heard shooting
    const idVec3 pos = self->GetPhysics()->GetOrigin();
    int heard = -1;
    float heardD = 1500.0f * 1500.0f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        idEntity* ent = gameLocal.entities[i];
        if (ent == NULL || ent == self || !ent->IsType(idPlayer::Type)) {
            continue;
        }
        idPlayer* p = static_cast<idPlayer*>(ent);
        if (p->health > 0 && !p->spectating && !VitaBot_SameTeam(self, p) && (p->usercmd.buttons & BUTTON_ATTACK)) {
            const float d = (p->GetPhysics()->GetOrigin() - pos).LengthSqr();
            if (d < heardD) {
                heardD = d;
                heard = i;
            }
        }
    }
    const bool wantFight = VitaBot_Aggression(self) >= 50.0f;
    if (heard >= 0 && wantFight) {
        VitaBot_SetGoal(self, b, gameLocal.entities[heard]->GetPhysics()->GetOrigin(), BS_ROAM, 6000);
        return;
    }
    // hunting (a deathmatch is about meeting): now and then towards an
    // enemy's rough whereabouts - the more so the higher the skill
    if (wantFight && vitaBotRandom.RandomFloat() < 0.25f + 0.08f * VitaBot_Skill(b)) {
        idList<idVec3> enemies;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            idEntity* ent = gameLocal.entities[i];
            if (ent == NULL || ent == self || !ent->IsType(idPlayer::Type)) {
                continue;
            }
            idPlayer* p = static_cast<idPlayer*>(ent);
            if (p->health > 0 && !p->spectating && !VitaBot_SameTeam(self, p)) {
                enemies.Append(p->GetPhysics()->GetOrigin() + idVec3(vitaBotRandom.CRandomFloat() * 256.0f, vitaBotRandom.CRandomFloat() * 256.0f, 0.0f));
            }
        }
        if (enemies.Num() > 0) {
            VitaBot_SetGoal(self, b, enemies[vitaBotRandom.RandomInt(enemies.Num())], BS_ROAM, 8000);
            return;
        }
    }
    if (VitaBot_PickItemGoal(self, b, !wantFight)) {
        return;
    }
    // roam: a node or a spawn point somewhere else
    if (botNav.nodes.Num() > 0) {
        for (int tries = 0; tries < 8; tries++) {
            const idVec3& p = botNav.nodes[vitaBotRandom.RandomInt(botNav.nodes.Num())].pos;
            if ((p - pos).LengthSqr() > 400.0f * 400.0f) {
                VitaBot_SetGoal(self, b, p, BS_ROAM, 12000);
                return;
            }
        }
    }
    VitaBot_SetGoal(self, b, pos + idAngles(0.0f, vitaBotRandom.RandomFloat() * 360.0f, 0.0f).ToForward() * 512.0f, BS_ROAM, 6000);
}

/*
--------------------------------------------------------------------------------
Moving to the goal: the path, else feelers
--------------------------------------------------------------------------------
*/
static idVec3 VitaBot_NextTarget(idPlayer* self, vitaBot_t& b)
{
    const int now = gameLocal.time;
    const idVec3 pos = self->GetPhysics()->GetOrigin();
    if (b.path.Num() == 0 && now >= b.replanMs && b.goalNode >= 0) {
        b.replanMs = now + 1500;
        const int start = BotNav_Nearest(pos, 400.0f, true);
        if (start >= 0 && BotNav_Path(start, b.goalNode, b.path)) {
            b.pathIndex = 0;
            b.paths++;
            b.progressMs = now;
            b.progressDist = 1e30f;
        } else {
            b.path.Clear();
            b.pathFails++;
        }
    }
    while (b.pathIndex < b.path.Num()) {
        const idVec3& n = botNav.nodes[b.path[b.pathIndex]].pos;
        const idVec2 d2 = (n - pos).ToVec2();
        if (d2.LengthSqr() < 40.0f * 40.0f && idMath::Fabs(n.z - pos.z) < 72.0f) {
            b.pathIndex++;
            b.progressMs = now;
            b.progressDist = 1e30f;
            continue;
        }
        // no progress towards the next node: the link is dearer, or dropped
        const float dist = d2.Length();
        if (dist < b.progressDist - 24.0f) {
            b.progressDist = dist;
            b.progressMs = now;
        } else if (now - b.progressMs > 2000) {
            if (b.pathIndex > 0) {
                navLink_t* l = BotNav_FindLink(b.path[b.pathIndex - 1], b.path[b.pathIndex]);
                if (l != NULL && ++l->fails == 3) {
                    botNav.dropped++;
                }
            }
            b.path.Clear();
            b.stuckCount++;
            break;
        }
        return n;
    }
    if (b.path.Num() > 0 && b.pathIndex >= b.path.Num()) {
        b.path.Clear(); // at the goal's node: straight on
    }
    return b.goal;
}

static void VitaBot_MoveTowards(idPlayer* self, vitaBot_t& b, const idVec3& target, usercmd_t& cmd, bool lookThere)
{
    const idVec3 pos = self->GetPhysics()->GetOrigin();
    const float goalYaw = (target - pos).ToYaw();
    // the clearest of a few directions around the target's
    static const float offsets[] = { 0.0f, 25.0f, -25.0f, 50.0f, -50.0f, 80.0f, -80.0f, 120.0f, -120.0f, 180.0f };
    float bestYaw = goalYaw, bestScore = -1.0f;
    const float want = Min(160.0f, (target - pos).ToVec2().Length() + 16.0f);
    for (int i = 0; i < 10; i++) {
        const float y = goalYaw + offsets[i];
        const float room = VitaBot_Feel(self, y, want);
        const float score = room - idMath::Fabs(offsets[i]) * 0.6f;
        if (room >= Min(48.0f, want - 8.0f) && score > bestScore) {
            bestScore = score;
            bestYaw = y;
            if (offsets[i] == 0.0f) {
                break; // straight ahead is clear
            }
        }
    }
    if (lookThere) {
        b.idealYaw = bestYaw;
        b.idealPitch = 0.0f;
    }
    VitaBot_MoveDir(b, idAngles(0.0f, bestYaw, 0.0f).ToForward(), cmd);
    // a step up ahead: jump it
    if (target.z - pos.z > 24.0f && (target - pos).ToVec2().LengthSqr() < 128.0f * 128.0f) {
        cmd.buttons |= BUTTON_JUMP;
    }
}

/*
================================================================================

VitaBot_Think: one tic of a bot

================================================================================
*/
void VitaBot_FragsDeaths(int client, int& frags, int& deaths)
{
    frags = gameLocal.mpGame.playerState[client].fragCount;
    deaths = gameLocal.mpGame.playerState[client].deaths;
}

static void VitaBot_Stats(idPlayer* self, vitaBot_t& b, int dtMs)
{
    b.stateMs[idMath::ClampInt(0, BS_NUM - 1, b.state)] += dtMs;
    const int now = gameLocal.time;
    if (now - b.statsSinceMs < 60000) {
        return;
    }
    int total = 0;
    for (int i = 0; i < BS_NUM; i++) {
        total += b.stateMs[i];
    }
    idStr states;
    for (int i = 0; i < BS_NUM; i++) {
        if (b.stateMs[i] > 0) {
            states += va("%s%s %.0f%%", states.IsEmpty() ? "" : ", ", botStateNames[i], 100.0f * b.stateMs[i] / Max(1, total));
        }
        b.stateMs[i] = 0;
    }
    const int c = self->entityNumber;
    int frags = 0, deaths = 0;
    VitaBot_FragsDeaths(c, frags, deaths);
    gameLocal.Printf("bot %s (%d, skill %s): last %.0f s - %s | frags %d, deaths %d | firing %d tics | paths %d planned, %d none | stuck %d | travelled %.0f units\n",
        session->GetActingGameStateLobbyBase().GetLobbyUserName(gameLocal.lobbyUserIDs[c]), c, VitaBot_SkillName(VitaBot_Skill(b)),
        (now - b.statsSinceMs) / 1000.0f, states.c_str(), frags, deaths,
        b.shots, b.paths, b.pathFails, b.stuckCount, b.travelled);
    b.statsSinceMs = now;
}

void VitaBot_Think(idPlayer* self, usercmd_t& cmd)
{
    const int clientNum = self->entityNumber;
    vitaBot_t& b = vitaBots[clientNum];
    const int now = gameLocal.time;
    memset(&cmd, 0, sizeof(cmd));
    cmd.clientGameMilliseconds = now;
    cmd.serverGameMilliseconds = now;
    VitaBot_LearnWeapons(self);
    if (!b.active) {
        const bool managed = b.managed;
        const int skill = b.skill;
        b.path.Clear();
        b = vitaBot_t();
        b.managed = managed;
        b.skill = skill;
        b.active = true;
        b.lastPos = b.prevPos = self->GetPhysics()->GetOrigin();
        b.lastMoveCheckMs = now;
        b.yaw = b.idealYaw = self->viewAngles.yaw;
        b.enemy = -1;
        b.lastHealth = self->health;
        b.lastThinkMs = now;
        b.statsSinceMs = now;
        b.state = BS_ROAM;
        b.goalUntilMs = 0;
        gameLocal.Printf("bot %s (%d) thinking, skill %s\n", session->GetActingGameStateLobbyBase().GetLobbyUserName(gameLocal.lobbyUserIDs[clientNum]), clientNum, VitaBot_SkillName(VitaBot_Skill(b)));
    }
    const int dtMs = idMath::ClampInt(1, 250, now - b.lastThinkMs);
    const float dt = dtMs / 1000.0f;
    b.lastThinkMs = now;
    const botChar_t ch = VitaBot_Character(VitaBot_Skill(b));

    if (self->wantSpectate) {
        // joining the game: what a player's "join" message does on the host
        self->wantSpectate = false;
        self->forceRespawn = true;
    }
    if (self->health <= 0 || self->spectating) {
        b.respawnPress = !b.respawnPress;
        if (b.respawnPress) {
            cmd.buttons |= BUTTON_ATTACK; // respawn: fire pressed and released
        }
        b.enemy = -1;
        b.path.Clear();
        b.goalUntilMs = 0;
        b.state = BS_DEAD;
        b.lastPos = b.prevPos = self->GetPhysics()->GetOrigin();
        b.lastHealth = self->health;
        b.enemySightMs = now; // the reaction time starts again (Q3's teleport_time)
        VitaBot_Stats(self, b, dtMs);
        return;
    }
    if (b.state == BS_DEAD) {
        b.state = BS_ROAM;
    }

    const idVec3 pos = self->GetPhysics()->GetOrigin();
    const float step = (pos - b.prevPos).Length();
    if (step < 200.0f) {
        b.travelled += step;
    }
    b.prevPos = pos;
    b.hurt = self->health < b.lastHealth;
    b.lastHealth = self->health;

    // the enemy: looked for every tenth of a second (Q3's think time),
    // the bots taking turns
    if (now >= b.nextEnemyScanMs || b.hurt) {
        b.nextEnemyScanMs = now + 100 + (clientNum % 3) * 7;
        const int found = VitaBot_FindEnemy(self, b, ch);
        if (found >= 0 && found != b.enemy) {
            b.enemySightMs = (b.enemy >= 0) ? now - 2000 : now; // a switch: no new reaction time (Q3)
            b.enemyPrevMs = 0;
            b.enemyMemoMs = 0;
            if (bot_debug.GetBool()) {
                gameLocal.Printf("bot %d: enemy %d\n", clientNum, found);
            }
        }
        if (found >= 0) {
            b.enemy = found;
            b.enemyLastSeenMs = now;
            b.enemyLastPos = gameLocal.entities[found]->GetPhysics()->GetOrigin();
        }
    }
    idPlayer* enemy = NULL;
    bool visible = false;
    if (b.enemy >= 0) {
        idEntity* ent = gameLocal.entities[b.enemy];
        if (ent == NULL || !ent->IsType(idPlayer::Type) || static_cast<idPlayer*>(ent)->health <= 0 || static_cast<idPlayer*>(ent)->spectating) {
            b.enemy = -1;
        } else {
            enemy = static_cast<idPlayer*>(ent);
            visible = (now - b.enemyLastSeenMs) < 150;
            if (visible) {
                b.enemyLastPos = enemy->GetPhysics()->GetOrigin();
            }
        }
    }

    const float aggression = VitaBot_Aggression(self);
    const bool carryingFlag = self->carryingFlag;
    if (enemy != NULL && visible) {
        const float dist = (VitaBot_Center(enemy) - self->GetEyePosition()).Length();
        VitaBot_ChooseWeapon(self, b, dist);
        VitaBot_AimAtEnemy(self, b, ch, enemy, true);
        if (VitaBot_CheckAttack(self, b, ch, enemy)) {
            cmd.buttons |= BUTTON_ATTACK;
            b.shots++;
        }
        const bool retreat = carryingFlag || (aggression < 50.0f && VitaBot_Skill(b) >= 2);
        if (retreat) {
            // Q3's battle retreat: to items (or home with the flag), shooting
            if (b.state != BS_RETREAT && b.state != BS_FLAG) {
                if (!(carryingFlag && VitaBot_FlagGoal(self, b))) {
                    if (VitaBot_PickItemGoal(self, b, true)) {
                        b.state = BS_RETREAT;
                    }
                }
            }
            if (b.state == BS_RETREAT || b.state == BS_FLAG) {
                VitaBot_MoveTowards(self, b, VitaBot_NextTarget(self, b), cmd, false);
            } else {
                VitaBot_AttackMove(self, b, ch, enemy, dt, cmd);
            }
        } else {
            b.state = BS_FIGHT;
            VitaBot_AttackMove(self, b, ch, enemy, dt, cmd);
        }
        b.chaseUntilMs = now + 3000 + (int)(ch.attackSkill * 5000.0f);
    } else if (enemy != NULL && now < b.chaseUntilMs && aggression >= 50.0f && !carryingFlag) {
        // Q3's BotWantsToChase: to where it was last seen, aiming there
        if (b.state != BS_CHASE) {
            VitaBot_SetGoal(self, b, b.enemyLastPos, BS_CHASE, b.chaseUntilMs - now);
        }
        VitaBot_AimAtEnemy(self, b, ch, enemy, false);
        VitaBot_MoveTowards(self, b, VitaBot_NextTarget(self, b), cmd, false);
        if ((b.enemyLastPos - pos).ToVec2().LengthSqr() < 64.0f * 64.0f) {
            b.enemy = -1; // got there: nothing
        }
    } else {
        if (b.state == BS_FIGHT || b.state == BS_CHASE || b.state == BS_RETREAT) {
            b.goalUntilMs = 0;
            b.enemy = -1;
        }
        if (now >= b.goalUntilMs || (b.goal - pos).ToVec2().LengthSqr() < 64.0f * 64.0f) {
            VitaBot_PickGoal(self, b);
            if (bot_debug.GetBool()) {
                gameLocal.Printf("bot %d: %s goal (%.0f %.0f %.0f), node %d\n", clientNum, botStateNames[b.state], b.goal.x, b.goal.y, b.goal.z, b.goalNode);
            }
        }
        VitaBot_MoveTowards(self, b, VitaBot_NextTarget(self, b), cmd, true);
        // the best weapon held for a fight at a middle distance
        VitaBot_ChooseWeapon(self, b, 400.0f);
    }

    // stuck: it meant to move and did not
    if (now - b.lastMoveCheckMs >= 700) {
        const float moved = (pos - b.lastPos).Length();
        b.lastPos = pos;
        b.lastMoveCheckMs = now;
        if ((cmd.forwardmove != 0 || cmd.rightmove != 0) && moved < 24.0f) {
            b.stuckCount++;
            b.idealYaw += 90.0f + vitaBotRandom.RandomFloat() * 180.0f;
            b.path.Clear();
            if (b.enemy < 0) {
                b.goalUntilMs = 0;
            }
            cmd.buttons |= BUTTON_JUMP;
        }
    }

    VitaBot_ChangeView(b, ch, enemy != NULL, dt);
    cmd.buttons |= BUTTON_RUN;
    const idAngles& delta = self->GetDeltaViewAngles();
    cmd.angles[YAW] = ANGLE2SHORT(b.yaw - delta.yaw);
    cmd.angles[PITCH] = ANGLE2SHORT(b.pitch - delta.pitch);
    cmd.angles[ROLL] = 0;
    VitaBot_Stats(self, b, dtMs);
}

void VitaBot_Reset(int clientNum)
{
    if (clientNum >= 0 && clientNum < MAX_PLAYERS) {
        vitaBots[clientNum].active = false;
    }
}

static int VitaBot_AddOne(int skill, bool managed)
{
    if (!common->IsMultiplayer() || common->IsClient()) {
        gameLocal.Printf("addBot: only the host of a multiplayer game can add bots\n");
        return 0;
    }
    idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
    const char* name = NULL;
    for (int n = 0; n < (int)(sizeof(vitaBotNames) / sizeof(vitaBotNames[0])) && name == NULL; n++) {
        bool used = false;
        for (int u = 0; u < lobby.GetNumLobbyUsers(); u++) {
            if (idStr::Icmp(lobby.GetLobbyUserName(lobby.GetLobbyUserIdByOrdinal(u)), vitaBotNames[n]) == 0) {
                used = true;
            }
        }
        if (!used) {
            name = vitaBotNames[n];
        }
    }
    if (name == NULL) {
        name = va("Bot %d", lobby.GetNumLobbyUsers() + 1);
    }
    const lobbyUserID_t id = lobby.AllocLobbyUserSlotForBot(name);
    if (!id.IsValid()) {
        gameLocal.Printf("addBot: no free slot for %s\n", name);
        return 0;
    }
    // the brain's settings, by the client slot the lobby gave it
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (gameLocal.lobbyUserIDs[i] == id) {
            vitaBots[i].managed = managed;
            vitaBots[i].skill = skill;
            vitaBots[i].active = false;
        }
    }
    gameLocal.Printf("addBot: %s joins (skill %s%s)\n", name, VitaBot_SkillName(skill > 0 ? skill : bot_skill.GetInteger()), managed ? ", kept by the Match Settings' Bots" : "");
    return 1;
}

int VitaBot_Add(int count)
{
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (VitaBot_AddOne(0, false) == 0) {
            break;
        }
        added++;
    }
    return added;
}

int VitaBot_RemoveAll()
{
    int removed = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (VitaBot_IsBot(i)) {
            session->GetActingGameStateLobbyBase().RemoveBotFromLobbyUserList(gameLocal.lobbyUserIDs[i]);
            VitaBot_Reset(i);
            vitaBots[i].managed = false;
            removed++;
        }
    }
    return removed;
}

/*
========================
VitaBot_HostFrame

Every game frame on a multiplayer host: the navigation graph (map change,
linking, learning, the log) and the Match Settings' bot count.
========================
*/
void VitaBot_HostFrame()
{
    if (!common->IsMultiplayer() || common->IsClient()) {
        return;
    }
    BotNav_MapCheck();
    BotNav_Explore();
    BotNav_Linker();
    BotNav_Learn();
    BotNav_Report();

    // bot_count: the host keeps that many bots it added itself (people first)
    static int nextCheckMs = 0;
    const int now = gameLocal.time;
    if (now < nextCheckMs) {
        return;
    }
    nextCheckMs = now + 2000;
    const int state = (int)gameLocal.mpGame.GetGameState();
    if (state != idMultiplayerGame::WARMUP && state != idMultiplayerGame::GAMEON) {
        return;
    }
    idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
    int managed = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (VitaBot_IsBot(i) && vitaBots[i].managed) {
            managed++;
        }
    }
    const int want = bot_count.GetInteger();
    const int freeSlots = (int)lobby.GetMatchParms().numSlots - lobby.GetNumLobbyUsers();
    if (managed < want && freeSlots > 0) {
        VitaBot_AddOne(0, true);
    } else if (managed > want) {
        for (int i = MAX_PLAYERS - 1; i >= 0; i--) {
            if (VitaBot_IsBot(i) && vitaBots[i].managed) {
                gameLocal.Printf("bots: %s leaves (Match Settings: %d bots)\n", lobby.GetLobbyUserName(gameLocal.lobbyUserIDs[i]), want);
                lobby.RemoveBotFromLobbyUserList(gameLocal.lobbyUserIDs[i]);
                VitaBot_Reset(i);
                vitaBots[i].managed = false;
                break;
            }
        }
    }
}

void VitaBot_MapShutdown()
{
    if (!botNav.map.IsEmpty()) {
        BotNav_Save();
        botNav.map.Clear();
        botNav.nodes.Clear();
    }
    vitaBotWeaponsReady = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        vitaBots[i].active = false;
        vitaBots[i].path.Clear();
    }
}

CONSOLE_COMMAND(addBot, "adds bots to the multiplayer game you host: addBot [count] [skill 1-5]", NULL)
{
    const int count = args.Argc() > 1 ? Max(1, atoi(args.Argv(1))) : 1;
    const int skill = args.Argc() > 2 ? idMath::ClampInt(1, 5, atoi(args.Argv(2))) : 0;
    for (int i = 0; i < count; i++) {
        if (VitaBot_AddOne(skill, false) == 0) {
            break;
        }
    }
}

CONSOLE_COMMAND(removeBots, "removes the bots from the multiplayer game you host", NULL)
{
    gameLocal.Printf("removeBots: %d removed\n", VitaBot_RemoveAll());
}

CONSOLE_COMMAND(botNavStatus, "the bots' navigation graph for this map", NULL)
{
    botNav.lastReportMs = -1000000;
    BotNav_Report();
}

/*
================
The verification's view of a multiplayer game (framework/VitaVerify.cpp)
================
*/
struct vitaMPPlayer_t {
    char name[40];
    bool bot;
    bool local;
    bool spectating;
    bool wantSpectate;
    int health;
    int frags;
    int deaths;
    int team;
    float travelled;
    int shots;
};

int Vita_MPPlayers(vitaMPPlayer_t* out, int max)
{
    if (!common->IsMultiplayer()) {
        return 0;
    }
    idLobbyBase& lobby = session->GetActingGameStateLobbyBase();
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS && n < max; i++) {
        idEntity* ent = gameLocal.entities[i];
        if (ent == NULL || !ent->IsType(idPlayer::Type)) {
            continue;
        }
        idPlayer* p = static_cast<idPlayer*>(ent);
        vitaMPPlayer_t& o = out[n++];
        memset(&o, 0, sizeof(o));
        idStr::Copynz(o.name, lobby.GetLobbyUserName(gameLocal.lobbyUserIDs[i]), sizeof(o.name));
        o.bot = VitaBot_IsBot(i);
        o.local = (i == gameLocal.GetLocalClientNum());
        o.spectating = p->spectating;
        o.wantSpectate = p->wantSpectate;
        o.health = p->health;
        o.frags = gameLocal.mpGame.playerState[i].fragCount;
        o.deaths = gameLocal.mpGame.playerState[i].deaths;
        o.team = p->team;
        o.travelled = vitaBots[i].travelled;
        o.shots = vitaBots[i].shots;
    }
    return n;
}

int Vita_MPGameState()
{
    return common->IsMultiplayer() ? (int)gameLocal.mpGame.GetGameState() : -1;
}

// the local player's aim at the nearest other player in sight (input angles)
bool Vita_MPAimAtNearest(float& yaw, float& pitch)
{
    idPlayer* self = gameLocal.GetLocalPlayer();
    if (self != NULL && self->wantSpectate) {
        // the test's player joins in (the menu's "join" does this)
        self->wantSpectate = false;
        self->forceRespawn = true;
    }
    if (self == NULL || self->health <= 0) {
        return false;
    }
    // the nearest visible enemy (the test's own aim, not a bot's brain)
    const idVec3 eye = self->GetEyePosition();
    int bestI = -1;
    float bestD = 1e30f;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        idEntity* ent = gameLocal.entities[i];
        if (ent == NULL || ent == self || !ent->IsType(idPlayer::Type)) {
            continue;
        }
        idPlayer* p = static_cast<idPlayer*>(ent);
        if (p->health <= 0 || p->spectating || VitaBot_SameTeam(self, p)) {
            continue;
        }
        const float d = (VitaBot_Center(p) - eye).LengthSqr();
        if (d < bestD && VitaBot_Visible(self, p)) {
            bestD = d;
            bestI = i;
        }
    }
    if (bestI < 0) {
        return false;
    }
    const idVec3 to = VitaBot_Center(static_cast<idPlayer*>(gameLocal.entities[bestI])) - eye;
    const idAngles a = to.ToAngles();
    yaw = idMath::AngleNormalize180(a.yaw - self->GetDeltaViewAngles().yaw);
    pitch = idMath::AngleNormalize180(a.pitch - self->GetDeltaViewAngles().pitch);
    return true;
}

void Vita_MPTimes(int& serverTime, int& nextSwitch, int& nextState)
{
    serverTime = gameLocal.serverTime;
    nextSwitch = gameLocal.mpGame.nextStateSwitch;
    nextState = (int)gameLocal.mpGame.nextState;
}
