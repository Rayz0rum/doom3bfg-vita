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

===========================================================================
*/
#ifndef __SYS_NET_FRIENDS_H__
#define __SYS_NET_FRIENDS_H__

/*
================================================================================
Game-specific friends and the local game browser

No platform service (PSN, Steam) is involved: every copy of the game has its
own player id (net_playerId, made up the first time it is needed) and name
(ui_name, else the system's user name), and the friends list is a plain file
in the save folder (friends.txt) holding, for each friend, their id, name and
the address they were last seen at.

Finding games ("Play With Friends" -> the game browser) is peer to peer: a
query is broadcast on the local network and sent straight to every friend's
last address; every game host that hears it answers with its game (the stock
BFG system-link query, OOB_MATCH_QUERY / _ACK) followed by a small block of
our own: who the host is. The query carries the same block for the asker, so
both sides learn each other's id; adding a host as a friend tells the host,
which adds the asker back - friendship is mutual and only lives in this game.

Over the internet a friend's game is reached at the address saved for them
(or one typed in with "Add Friend by Address"): that needs the host's UDP port
(net_port, 27015) forwarded to it, as there is no relay or matchmaking server.
================================================================================
*/

struct netFriend_t {
    idStr id;       // 16 hex digits
    idStr name;
    netadr_t addr;  // last address heard from (type NA_BAD when none)
    int lastSeen;   // time() when last heard from, 0 never
};

// What a query / an answer carries after the stock fields.
struct netPeerTag_t {
    idStr id;
    idStr name;
    idStr hwIdHash;     // 16 hex digits
    bool friendRequest; // query: "I added you" / answer: unused
    bool wantBanList;   // query: "send me your ban list" (yours is newer)
    uint32 banListVersion;
    bool invite;        // query: "come and join my game" (a friend's invite; read only when present)
    bool valid;
};

struct netBan_t {
    idStr hwIdHash;
    idStr name;
};

struct netPlayed_t {
    int when; // time()
    idStr what;
};

/*
The player and its friends (see sys_net_local.cpp for this source tree's version).
*/
class idNetFriends {
public:
    idNetFriends();

    void Load();
    void Save();

    int Num() const
    {
        return friends.Num();
    }
    const netFriend_t& Get(int i) const
    {
        return friends[i];
    }
    int Find(const char* id) const;
    // a friend was last heard from this IP (any port)
    bool IsFriendAddress(const netadr_t& addr);

    // adds or refreshes; returns true when the friend is new
    bool Add(const char* id, const char* name, const netadr_t& addr);
    void Remove(const char* id);
    // a friend was heard from at this address
    void Seen(const char* id, const char* name, const netadr_t& addr);

    // this player
    const char* LocalId();
    const char* LocalName();
    bool HasUsername();
    void SetUsername(const char* name);

    // what was played with whom (the last 64 things)
    void NotePlayed(const char* what);
    int NumPlayed() const
    {
        return played.Num();
    }
    const netPlayed_t& GetPlayed(int i) const
    {
        return played[i];
    }

    // the account's other parts (used by idNetOnline)
    idList<netBan_t>& LocalBans()
    {
        return localBans;
    }
    idStrList& DhtNodes()
    {
        return dhtNodes;
    }
    idList<byte>& BanListBlob()
    {
        return banListBlob;
    }
    bool disclaimerAccepted;

    // the game owner's console (official builds only)
    enum { OWNER_RIGHT_OWNER = 1, OWNER_RIGHT_ADMIN = 2 };
    uint32 OwnerRights();
    bool IsOwner()
    {
        return OwnerRights() != 0;
    }
    const idList<byte>& OwnerKey() const
    {
        return ownerKey;
    }

    void WriteTag(idBitMsg& msg, bool friendRequest, bool wantBanList = false, bool invite = false);
    // the same for another identity (the full verification's fake peers)
    void WriteTagAs(idBitMsg& msg, const char* id, const char* name, const char* hwIdHash, bool friendRequest, bool wantBanList, bool invite);
    static netPeerTag_t ReadTag(idBitMsg& msg);

private:
    void LoadOldFriendsFile();
    bool LoadAccount();
    bool ImportOwnerAccount(const byte* data, int len);
    idList<byte> ownerCert; // signed part + signature
    idList<byte> ownerKey;  // private exponent, big-endian
    int ownerRightsChecked; // -1: not checked yet
    idList<netFriend_t> friends;
    idList<netPlayed_t> played;
    idList<netBan_t> localBans;
    idStrList dhtNodes; // DHT nodes that answered lately ("ip:port")
    idList<byte> banListBlob;
    idStr username;
    idStr playerId;
    bool loaded;
};

extern idNetFriends netFriends;

// Vita: the system's user name ("" elsewhere / when it cannot be read)
const char* Sys_UserName();

// The console's hardware id: the Vita's OpenPSID (16 bytes); elsewhere a
// hash of the machine's name. Never leaves the console as is - only a hash.
void Sys_HardwareId(byte out[16]);

// Typed-in addresses (the Vita's on-screen keyboard): starts asking for text,
// then poll until it returns true; the text is "" when cancelled. Without an
// on-screen keyboard (PC) Start returns false - use the console command.
bool Sys_TextInputStart(const char* title, const char* initial);
// accepted (optional): Enter was pressed - the text may then be empty on
// purpose (everything erased); false when cancelled
bool Sys_TextInputPoll(idStr& text, bool* accepted = NULL);

#endif // !__SYS_NET_FRIENDS_H__
