/*
===========================================================================

Classic DOOM / DOOM II as the full verification sees them (PS Vita port;
neo/framework/VitaVerify.cpp drives it). Called on the main thread between
frames.

===========================================================================
*/

#include "Precompiled.h"
#include "globaldata.h"

#include "doomdef.h"
#include "doomlib.h"
#include "g_game.h"
#include "d_player.h"

struct vitaClassicState_t {
    bool ready;
    int gamestate; // GS_LEVEL 0, GS_INTERMISSION 1, GS_FINALE 2, GS_DEMOSCREEN 3
    int episode;
    int map;
    bool demo;
    int gametic;
    int leveltime;
    int health;
    int kills;
    int totalkills;
    int mission;
    int menuactive;
    int ammo;
    int weapon;
};

bool Vita_ClassicState(vitaClassicState_t& out)
{
    memset(&out, 0, sizeof(out));
    if (DoomLib::GetGlobalData(0) == NULL) {
        return false;
    }
    const int old = DoomLib::GetPlayer();
    DoomLib::SetPlayer(0);
    if (::g == NULL) {
        DoomLib::SetPlayer(old);
        return false;
    }
    out.ready = true;
    out.gamestate = (int)::g->gamestate;
    out.episode = ::g->gameepisode;
    out.map = ::g->gamemap;
    out.demo = ::g->demoplayback != 0;
    out.gametic = ::g->gametic;
    out.leveltime = ::g->leveltime;
    out.mission = (int)::g->gamemission;
    out.menuactive = ::g->menuactive;
    out.totalkills = ::g->totalkills;
    const player_t& p = ::g->players[::g->consoleplayer];
    out.health = p.health;
    out.kills = p.killcount;
    out.weapon = (int)p.readyweapon;
    if (p.readyweapon >= 0 && p.readyweapon < NUMWEAPONS) {
        const ammotype_t at = weaponinfo[p.readyweapon].ammo;
        out.ammo = (at < NUMAMMO) ? p.ammo[at] : -1;
    }
    DoomLib::SetPlayer(old);
    return true;
}

// a new game straight onto a map (the menus skipped), medium skill
void Vita_ClassicNewGame(int episode, int map)
{
    const int old = DoomLib::GetPlayer();
    DoomLib::SetPlayer(0);
    if (::g != NULL) {
        ::g->menuactive = 0;
        G_DeferedInitNew(sk_medium, episode, map);
    }
    DoomLib::SetPlayer(old);
}

// god mode, every weapon and full ammo, the strongest weapon in hand
void Vita_ClassicCheats()
{
    const int old = DoomLib::GetPlayer();
    DoomLib::SetPlayer(0);
    if (::g != NULL && ::g->gamestate == GS_LEVEL) {
        player_t& p = ::g->players[::g->consoleplayer];
        p.cheats |= CF_GODMODE;
        if (p.mo != NULL) {
            p.health = p.mo->health = 100;
        }
        for (int i = 0; i < NUMWEAPONS; i++) {
            p.weaponowned[i] = true;
        }
        for (int i = 0; i < NUMAMMO; i++) {
            p.ammo[i] = p.maxammo[i];
        }
        p.pendingweapon = wp_chaingun;
    }
    DoomLib::SetPlayer(old);
}

// DOOM II's expansion: 1 Hell on Earth, 5 No Rest for the Living (GameMission_t)
void Vita_ClassicSetExpansion(int mission)
{
    DoomLib::SetIdealExpansion(mission);
    DoomLib::SetCurrentExpansion(mission);
}
