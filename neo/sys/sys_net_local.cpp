/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - PS Vita port: local multiplayer only.

The port's online play (finding players over the internet, accounts, bans)
is not published with the source; the official builds carry it. This file
stands in for it when the source tree has no neo/sys/online (CMakeLists.txt):
the same interfaces (sys_net_online.h, sys_net_friends.h), with

  - online play off for good (net_online does nothing, a lobby never looks
    beyond the local network);
  - friends kept in a plain file (friends_local.txt in the save folder):
    a player id, a username, the friends and the consoles banned from this
    host's games - found on the local network or added by address;
  - the tag games carry about their players in plain form (the official
    builds' tags are not readable here, nor these there: the two do not
    meet).

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"
#include "sys_net_online.h"
#include "sys_net_friends.h"
#include <time.h>

idCVar net_online("net_online", "0", CVAR_BOOL | CVAR_ARCHIVE | CVAR_NOCHEAT, "online play - not in this build: multiplayer is the local network (and friends added by address) only");
idCVar net_onlineVerbose("net_onlineVerbose", "0", CVAR_BOOL | CVAR_NOCHEAT, "log the multiplayer tags read and written");

idNetOnline netOnline;
// the session hands online code its socket (session_local.cpp); unused here
void (*netOnlineSend)(const netadr_t& to, const byte* data, int len) = NULL;
idNetFriends netFriends;

static const char* LOCAL_FILE = "friends_local.txt";
static const int MAX_FRIENDS = 200;
static const int MAX_PLAYED = 64;
static const int LOCAL_TAG_MAGIC = ('D' << 24) | ('3' << 16) | ('F' << 8) | 'L';
static const int LOCAL_TAG_VERSION = 1;

static idStr localHwId; // this install's id in the tags (random, not the hardware's)

static idStr RandomHex16()
{
    static idRandom2 rnd((unsigned int)time(NULL) ^ (unsigned int)Sys_Milliseconds() ^ (unsigned int)Sys_GetClockTicks());
    idStr s;
    for (int i = 0; i < 8; i++) {
        s += va("%02x", (int)(rnd.RandomInt() & 0xFF));
    }
    return s;
}

static idStr CleanName(const char* name)
{
    idStr n = name;
    n.Replace("\t", " ");
    n.Replace("\n", " ");
    n.Replace("\r", " ");
    n.StripLeading(' ');
    n.StripTrailing(' ');
    if (n.Length() > 24) {
        n = n.Left(24);
    }
    if (n.Length() == 0) {
        n = "Player";
    }
    return n;
}

static void Split(const idStr& line, idStrList& fields)
{
    fields.Clear();
    idStr f;
    for (int k = 0; k <= line.Length(); k++) {
        const char c = (k < line.Length()) ? line[k] : '\t';
        if (c == '\t') {
            fields.Append(f);
            f.Clear();
        } else {
            f += c;
        }
    }
}

/*
================================================================================
idNetFriends - the local version
================================================================================
*/
idNetFriends::idNetFriends()
    : disclaimerAccepted(true)
    , ownerRightsChecked(0)
    , loaded(false)
{
}

uint32 idNetFriends::OwnerRights()
{
    return 0;
}

bool idNetFriends::ImportOwnerAccount(const byte* data, int len)
{
    return false;
}

bool idNetFriends::LoadAccount()
{
    void* buffer = NULL;
    const int len = fileSystem->ReadFile(LOCAL_FILE, &buffer);
    if (len <= 0 || buffer == NULL) {
        return false;
    }
    idStr text((const char*)buffer);
    fileSystem->FreeFile(buffer);
    idStr line;
    idStrList fields;
    for (int i = 0; i <= text.Length(); i++) {
        const char c = (i < text.Length()) ? text[i] : '\n';
        if (c != '\n' && c != '\r') {
            line += c;
            continue;
        }
        Split(line, fields);
        line.Clear();
        if (fields.Num() < 2) {
            continue;
        }
        const idStr& k = fields[0];
        if (k == "name") {
            username = fields[1];
        } else if (k == "id") {
            playerId = fields[1];
        } else if (k == "hw") {
            localHwId = fields[1];
        } else if (k == "friend" && fields.Num() >= 3 && friends.Num() < MAX_FRIENDS) {
            netFriend_t f;
            f.id = fields[1];
            f.name = fields[2];
            memset(&f.addr, 0, sizeof(f.addr));
            f.addr.type = NA_BAD;
            if (fields.Num() > 3 && fields[3] != "-" && !Sys_StringToNetAdr(fields[3], &f.addr, false)) {
                f.addr.type = NA_BAD;
            }
            f.lastSeen = fields.Num() > 4 ? atoi(fields[4]) : 0;
            friends.Append(f);
        } else if (k == "played" && fields.Num() >= 3) {
            netPlayed_t p;
            p.when = atoi(fields[1]);
            p.what = fields[2];
            played.Append(p);
        } else if (k == "ban" && fields.Num() >= 3) {
            netBan_t b;
            b.hwIdHash = fields[1];
            b.name = fields[2];
            localBans.Append(b);
        }
    }
    return true;
}

void idNetFriends::LoadOldFriendsFile()
{
}

void idNetFriends::Load()
{
    if (loaded) {
        return;
    }
    loaded = true;
    friends.Clear();
    if (!LoadAccount()) {
        idLib::Printf("net: no %s yet - one is made now (local multiplayer only: this build has no online play)\n", LOCAL_FILE);
    }
    bool changed = false;
    if (playerId.Length() != 16) {
        playerId = RandomHex16();
        changed = true;
    }
    if (localHwId.Length() != 16) {
        localHwId = RandomHex16();
        changed = true;
    }
    if (changed) {
        Save();
    }
    idLib::Printf("net: local player '%s' (player id %s), %d friend(s), %d game(s) remembered, %d console(s) banned from my games - online play is not in this build\n",
        username.c_str(), playerId.c_str(), friends.Num(), played.Num(), localBans.Num());
}

void idNetFriends::Save()
{
    idStr text;
    text += "# DOOM 3 BFG (PS Vita port, local multiplayer build): this player and its friends\n";
    text += va("name\t%s\n", username.c_str());
    text += va("id\t%s\n", playerId.c_str());
    text += va("hw\t%s\n", localHwId.c_str());
    for (int i = 0; i < friends.Num(); i++) {
        const netFriend_t& f = friends[i];
        text += va("friend\t%s\t%s\t%s\t%d\n", f.id.c_str(), f.name.c_str(), f.addr.type == NA_BAD ? "-" : Sys_NetAdrToString(f.addr), f.lastSeen);
    }
    for (int i = 0; i < played.Num(); i++) {
        text += va("played\t%d\t%s\n", played[i].when, played[i].what.c_str());
    }
    for (int i = 0; i < localBans.Num(); i++) {
        text += va("ban\t%s\t%s\n", localBans[i].hwIdHash.c_str(), localBans[i].name.c_str());
    }
    idFile* f = fileSystem->OpenFileWrite(LOCAL_FILE, "fs_savepath");
    if (f == NULL) {
        idLib::Warning("net: could not write %s", LOCAL_FILE);
        return;
    }
    f->Write(text.c_str(), text.Length());
    fileSystem->CloseFile(f);
}

const char* idNetFriends::LocalId()
{
    Load();
    return playerId.c_str();
}

const char* idNetFriends::LocalName()
{
    Load();
    if (username.Length() > 0) {
        return username.c_str();
    }
    const char* uiName = cvarSystem->GetCVarString("ui_name");
    if (uiName[0] != '\0') {
        return uiName;
    }
    const char* sys = Sys_UserName();
    if (sys[0] != '\0') {
        return sys;
    }
    idLocalUser* user = session != NULL ? session->GetSignInManager().GetMasterLocalUser() : NULL;
    if (user != NULL && user->GetGamerTag()[0] != '\0') {
        return user->GetGamerTag();
    }
    return "Player";
}

bool idNetFriends::HasUsername()
{
    Load();
    return username.Length() > 0;
}

void idNetFriends::SetUsername(const char* name)
{
    Load();
    username = CleanName(name);
    idLib::Printf("net: username set to '%s'\n", username.c_str());
    Save();
    extern void Sys_SetLocalUserNames(const char* name);
    Sys_SetLocalUserNames(username.c_str());
}

void idNetFriends::NotePlayed(const char* what)
{
    Load();
    netPlayed_t p;
    p.when = (int)time(NULL);
    p.what = what;
    p.what.Replace("\t", " ");
    p.what.Replace("\n", " ");
    played.Append(p);
    while (played.Num() > MAX_PLAYED) {
        played.RemoveIndex(0);
    }
    idLib::Printf("net: played: %s\n", p.what.c_str());
    Save();
}

int idNetFriends::Find(const char* id) const
{
    for (int i = 0; i < friends.Num(); i++) {
        if (friends[i].id.Icmp(id) == 0) {
            return i;
        }
    }
    return -1;
}

bool idNetFriends::IsFriendAddress(const netadr_t& addr)
{
    Load();
    for (int i = 0; i < friends.Num(); i++) {
        if (friends[i].addr.type != NA_BAD && Sys_CompareNetAdrBase(friends[i].addr, addr)) {
            return true;
        }
    }
    return false;
}

bool idNetFriends::Add(const char* id, const char* name, const netadr_t& addr)
{
    Load();
    if (idStr::Length(id) != 16 || idStr::Icmp(id, LocalId()) == 0) {
        return false;
    }
    if (Find(id) >= 0) {
        Seen(id, name, addr);
        return false;
    }
    if (friends.Num() >= MAX_FRIENDS) {
        idLib::Warning("net: the friends list is full (%d)", MAX_FRIENDS);
        return false;
    }
    netFriend_t f;
    f.id = id;
    f.name = CleanName(name);
    f.addr = addr;
    f.lastSeen = (int)time(NULL);
    friends.Append(f);
    Save();
    idLib::Printf("net: friend added: %s (%s) at %s\n", f.name.c_str(), f.id.c_str(), addr.type == NA_BAD ? "-" : Sys_NetAdrToString(addr));
    return true;
}

void idNetFriends::Remove(const char* id)
{
    Load();
    const int i = Find(id);
    if (i >= 0) {
        idLib::Printf("net: friend removed: %s (%s)\n", friends[i].name.c_str(), friends[i].id.c_str());
        friends.RemoveIndex(i);
        Save();
    }
}

void idNetFriends::Seen(const char* id, const char* name, const netadr_t& addr)
{
    Load();
    const int i = Find(id);
    if (i < 0) {
        return;
    }
    netFriend_t& f = friends[i];
    const idStr clean = CleanName(name);
    const bool moved = (addr.type != NA_BAD) && (f.addr.type == NA_BAD || !Sys_CompareNetAdrBase(f.addr, addr) || f.addr.port != addr.port);
    const bool renamed = (clean != f.name);
    f.lastSeen = (int)time(NULL);
    if (moved) {
        f.addr = addr;
    }
    if (renamed) {
        f.name = clean;
    }
    if (moved || renamed) {
        idLib::Printf("net: friend %s (%s) now at %s\n", f.name.c_str(), f.id.c_str(), Sys_NetAdrToString(f.addr));
        Save();
    }
}

void idNetFriends::WriteTag(idBitMsg& msg, bool friendRequest, bool wantBanList, bool invite)
{
    Load();
    WriteTagAs(msg, LocalId(), LocalName(), localHwId.c_str(), friendRequest, wantBanList, invite);
}

void idNetFriends::WriteTagAs(idBitMsg& msg, const char* id, const char* name, const char* hwIdHash, bool friendRequest, bool wantBanList, bool invite)
{
    msg.WriteLong(LOCAL_TAG_MAGIC);
    msg.WriteByte(LOCAL_TAG_VERSION);
    msg.WriteString(id);
    msg.WriteString(name);
    msg.WriteString(hwIdHash);
    msg.WriteBool(friendRequest);
    msg.WriteBool(invite);
    if (net_onlineVerbose.GetBool()) {
        idLib::Printf("net: tag written for %s (%s)%s%s\n", name, id, friendRequest ? ", friend request" : "", invite ? ", invite" : "");
    }
}

netPeerTag_t idNetFriends::ReadTag(idBitMsg& msg)
{
    netPeerTag_t tag;
    tag.valid = false;
    tag.friendRequest = false;
    tag.wantBanList = false;
    tag.banListVersion = 0;
    tag.invite = false;
    if (msg.GetRemainingData() < 5) {
        return tag;
    }
    if (msg.ReadLong() != LOCAL_TAG_MAGIC || msg.ReadByte() != LOCAL_TAG_VERSION) {
        return tag;
    }
    char buf[128];
    msg.ReadString(buf, sizeof(buf));
    tag.id = buf;
    msg.ReadString(buf, sizeof(buf));
    tag.name = buf;
    msg.ReadString(buf, sizeof(buf));
    tag.hwIdHash = buf;
    tag.friendRequest = msg.ReadBool();
    tag.invite = msg.ReadBool();
    tag.valid = (tag.id.Length() == 16 && tag.hwIdHash.Length() == 16);
    return tag;
}

void Sys_HardwareId(byte out[16])
{
    // this build never reads the hardware's id: the install's random one
    netFriends.Load();
    memset(out, 0, 16);
    for (int i = 0; i < 8 && i * 2 + 1 < localHwId.Length(); i++) {
        char two[3] = { localHwId[i * 2], localHwId[i * 2 + 1], 0 };
        out[i] = (byte)strtol(two, NULL, 16);
    }
}

/*
================================================================================
idNetOnline - off: no online play in this build
================================================================================
*/
struct localPeer_t {
    netadr_t addr;
    idStr id, name, hw;
};
static idList<localPeer_t> localPeers;

void idNetOnline::Pump(bool active)
{
    static bool said = false;
    if (active && net_online.GetBool() && !said) {
        said = true;
        idLib::Printf("net: online play is not part of this build (built from the published source) - the lobby is the local network and friends added by address\n");
    }
}

bool idNetOnline::HandleRawPacket(const netadr_t& from, const byte* data, int size)
{
    return false;
}

int idNetOnline::NumPlayersOnline() const
{
    return 0;
}

bool idNetOnline::NeedsDisclaimer() const
{
    return false;
}

void idNetOnline::AcceptDisclaimer()
{
}

const char* idNetOnline::HwIdHash()
{
    netFriends.Load();
    return localHwId.c_str();
}

bool idNetOnline::IsThisConsoleBanned()
{
    return false;
}

bool idNetOnline::IsBanned(const char* hwIdHash)
{
    idList<netBan_t>& bans = netFriends.LocalBans();
    for (int i = 0; i < bans.Num(); i++) {
        if (bans[i].hwIdHash.Icmp(hwIdHash) == 0) {
            return true;
        }
    }
    return false;
}

bool idNetOnline::IsOwnerBanned(const char* hwIdHash)
{
    return false;
}

bool idNetOnline::IsAddressBanned(const netadr_t& addr)
{
    for (int i = 0; i < localPeers.Num(); i++) {
        if (Sys_CompareNetAdrBase(localPeers[i].addr, addr) && localPeers[i].addr.port == addr.port) {
            return IsBanned(localPeers[i].hw);
        }
    }
    return false;
}

void idNetOnline::BanLocally(const char* hwIdHash, const char* name)
{
    if (IsBanned(hwIdHash)) {
        return;
    }
    netBan_t b;
    b.hwIdHash = hwIdHash;
    b.name = name;
    netFriends.LocalBans().Append(b);
    netFriends.Save();
    idLib::Printf("net: %s (%s) banned from my games\n", name, hwIdHash);
}

void idNetOnline::UnbanLocally(const char* hwIdHash)
{
    idList<netBan_t>& bans = netFriends.LocalBans();
    for (int i = 0; i < bans.Num(); i++) {
        if (bans[i].hwIdHash.Icmp(hwIdHash) == 0) {
            idLib::Printf("net: %s (%s) unbanned\n", bans[i].name.c_str(), hwIdHash);
            bans.RemoveIndex(i);
            netFriends.Save();
            return;
        }
    }
}

bool idNetOnline::GlobalBan(const char* hwIdHash, bool ban)
{
    return false;
}

bool idNetOnline::CanGlobalBan()
{
    return false;
}

int idNetOnline::WriteOwnerBadge(byte* out, int maxLen)
{
    return 0;
}

void idNetOnline::OfferOwnerBadge(const netadr_t& from, const char* name, const char* hwIdHash, const byte* badge, int len)
{
}

bool idNetOnline::IsOwnerName(const char* name)
{
    return false;
}

void idNetOnline::NotePeer(const netadr_t& addr, const char* id, const char* name, const char* hwIdHash)
{
    for (int i = 0; i < localPeers.Num(); i++) {
        if (localPeers[i].id.Icmp(id) == 0) {
            localPeers[i].addr = addr;
            localPeers[i].name = name;
            localPeers[i].hw = hwIdHash;
            return;
        }
    }
    if (localPeers.Num() >= 64) {
        localPeers.RemoveIndex(0);
    }
    localPeer_t& p = localPeers.Alloc();
    p.addr = addr;
    p.id = id;
    p.name = name;
    p.hw = hwIdHash;
}

const char* idNetOnline::HwIdOfName(const char* name)
{
    for (int i = localPeers.Num() - 1; i >= 0; i--) {
        if (localPeers[i].name.Icmp(name) == 0) {
            return localPeers[i].hw.c_str();
        }
    }
    return "";
}

uint32 idNetOnline::BanListVersion() const
{
    return 0;
}

int idNetOnline::WriteBanList(byte* out, int maxLen) const
{
    return 0;
}

void idNetOnline::OfferBanList(const byte* blob, int len, const char* fromWhom)
{
}

void idNetOnline::GetHelloTargets(idList<netadr_t>& out)
{
    out.Clear();
}

void idNetOnline::Status()
{
    netFriends.Load();
    idLib::Printf("net: local multiplayer build - no online play; player '%s' (%s), %d friend(s), %d console(s) banned from my games, %d player(s) met this session\n",
        netFriends.LocalName(), netFriends.LocalId(), netFriends.Num(), netFriends.LocalBans().Num(), localPeers.Num());
}

void idNetOnline::VerifyInfo(verifyInfo_t& out)
{
    out = verifyInfo_t();
    out.active = false;
    out.nat = "none (local multiplayer build)";
}

CONSOLE_COMMAND(net_onlineStatus, "multiplayer: this build's player, friends and bans (local network only)", NULL)
{
    netOnline.Status();
}
