/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/
#ifndef __SYS_NET_ONLINE_H__
#define __SYS_NET_ONLINE_H__

/*
================================================================================
Online multiplayer (PS Vita port)

The official builds' online code is not published. This interface is what
the rest of the engine uses; neo/sys/sys_net_local.cpp implements it for
builds made from this source tree (local network multiplayer only).
================================================================================
*/

class idNetOnline {
public:
    // called from idSessionLocalWin::PlatformPump: runs the DHT and the hellos
    // while a multiplayer lobby is up (active = the party lobby exists)
    void Pump(bool active);

    // a raw UDP packet on the game port: true when it was DHT traffic (eaten)
    bool HandleRawPacket(const netadr_t& from, const byte* data, int size);

    // players heard from lately (their game's answer to our hello)
    int NumPlayersOnline() const;

    // the first time online: the disclaimer must be accepted
    bool NeedsDisclaimer() const;
    void AcceptDisclaimer();

    // this console
    const char* HwIdHash();  // 16 hex digits
    bool IsThisConsoleBanned();

    // bans: a console's HW ID hash
    bool IsBanned(const char* hwIdHash);            // local list or the owner's list
    bool IsOwnerBanned(const char* hwIdHash);       // the owner's list only (banned from online play)
    bool IsAddressBanned(const netadr_t& addr);     // a peer we know the HW ID of
    void BanLocally(const char* hwIdHash, const char* name);
    void UnbanLocally(const char* hwIdHash);
    // the owner's console only (official builds)
    bool GlobalBan(const char* hwIdHash, bool ban);
    bool CanGlobalBan();

    // the owner's badge (official builds only)
    int WriteOwnerBadge(byte* out, int maxLen);  // 0 when this is not the owner's console (or not signed yet)
    void OfferOwnerBadge(const netadr_t& from, const char* name, const char* hwIdHash, const byte* badge, int len);
    bool IsOwnerName(const char* name);          // a player met lately showed a valid owner badge


    // a peer told us who it is (from its tag)
    void NotePeer(const netadr_t& addr, const char* id, const char* name, const char* hwIdHash);
    const char* HwIdOfName(const char* name); // a lobby member's HW ID hash, "" when unknown

    // the owner's signed ban list
    uint32 BanListVersion() const;
    int WriteBanList(byte* out, int maxLen) const;     // the signed blob, 0 when none
    void OfferBanList(const byte* blob, int len, const char* fromWhom);

    // what the session asks / answers: endpoints to say hello to
    void GetHelloTargets(idList<netadr_t>& out);

    void Status(); // net_online console command
    // the full verification's view (framework/VitaVerify.cpp)
    struct verifyInfo_t {
        bool active;
        int packetsIn, packetsOut;
        int lookupsDone;
        bool haveExternal;
        idStr external;
        int portMapExternal; // 0 none
        int portMapState;    // 0 idle, 1 asking, 2 done
        idStr nat;           // "symmetric", "cone", "unknown"
        int peers, peersReached;
    };
    void VerifyInfo(verifyInfo_t& out);
};

extern idNetOnline netOnline;

#endif // !__SYS_NET_ONLINE_H__
