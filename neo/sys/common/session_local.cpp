/*
================================================================================================
CONFIDENTIAL AND PROPRIETARY INFORMATION/NOT FOR DISCLOSURE WITHOUT WRITTEN PERMISSION
Copyright 2010 id Software LLC, a ZeniMax Media company. All Rights Reserved.
================================================================================================
*/

/*
================================================================================================

Contains the windows implementation of the network session

================================================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "../../framework/Common_local.h"
#include "../sys_session_local.h"
#include "../sys_stats.h"
#include "../sys_savegame.h"
#include "../sys_lobby_backend_direct.h"
#include "../sys_voicechat.h"
#include "achievements.h"
#include "../sys_net_friends.h"
#include "../sys_net_online.h"
#include "localuser.h"
// #include "win_local.h"

/*
========================
Global variables
========================
*/

extern idCVar net_port;
extern unsigned int NetGetVersionChecksum();
extern idCVar net_onlineVerbose;
extern void (*netOnlineSend)(const netadr_t& to, const byte* data, int len);

class idLobbyToSessionCBLocal;

/*
========================
idSessionLocalWin::idSessionLocalWin
========================
*/
class idSessionLocalWin : public idSessionLocal {
    friend class idLobbyToSessionCBLocal;

public:
    idSessionLocalWin();
    virtual ~idSessionLocalWin();

    // idSessionLocal interface
    virtual void Initialize();
    virtual void Shutdown();

    virtual void InitializeSoundRelatedSystems();
    virtual void ShutdownSoundRelatedSystems();

    virtual void PlatformPump();

    virtual void InviteFriends();
    virtual void InviteParty();
    virtual void ShowPartySessions();

    virtual void ShowSystemMarketplaceUI() const;

    virtual void ListServers(const idCallback& callback);
    virtual void CancelListServers();
    virtual int NumServers() const;
    virtual const serverInfo_t* ServerInfo(int i) const;
    virtual void ConnectToServer(int i);
    virtual void ShowServerGamerCardUI(int i);

    virtual void ShowLobbyUserGamerCardUI(lobbyUserID_t lobbyUserID);

    virtual void ShowOnlineSignin() { }
    virtual void UpdateRichPresence() { }
    virtual void CheckVoicePrivileges() { }

    virtual bool ProcessInputEvent(const sysEvent_t* ev);

    // System UI
    virtual bool IsSystemUIShowing() const;
    virtual void SetSystemUIShowing(bool show);

    // Invites
    virtual void HandleBootableInvite(int64 lobbyId = 0);
    virtual void ClearBootableInvite();
    virtual void ClearPendingInvite();

    virtual bool HasPendingBootableInvite();
    virtual void SetDiscSwapMPInvite(void* parm);
    virtual void* GetDiscSwapMPInviteParms();

    virtual void EnumerateDownloadableContent();

    virtual void HandleServerQueryRequest(lobbyAddress_t& remoteAddr, idBitMsg& msg, int msgType);
    virtual void HandleServerQueryAck(lobbyAddress_t& remoteAddr, idBitMsg& msg);

    // Leaderboards
    virtual void LeaderboardUpload(lobbyUserID_t lobbyUserID, const leaderboardDefinition_t* leaderboard, const column_t* stats, const idFile_Memory* attachment = NULL);
    virtual void LeaderboardDownload(int sessionUserIndex, const leaderboardDefinition_t* leaderboard, int startingRank, int numRows, const idLeaderboardCallback& callback);
    virtual void LeaderboardDownloadAttachment(int sessionUserIndex, const leaderboardDefinition_t* leaderboard, int64 attachmentID);

    // Scoring (currently just for TrueSkill)
    virtual void SetLobbyUserRelativeScore(lobbyUserID_t lobbyUserID, int relativeScore, int team) { }

    virtual void LeaderboardFlush();

    virtual idNetSessionPort& GetPort(bool dedicated = false);
    virtual idLobbyBackend* CreateLobbyBackend(const idMatchParameters& p, float skillLevel, idLobbyBackend::lobbyBackendType_t lobbyType);
    virtual idLobbyBackend* FindLobbyBackend(const idMatchParameters& p, int numPartyUsers, float skillLevel, idLobbyBackend::lobbyBackendType_t lobbyType);
    virtual idLobbyBackend* JoinFromConnectInfo(const lobbyConnectInfo_t& connectInfo, idLobbyBackend::lobbyBackendType_t lobbyType);
    virtual void DestroyLobbyBackend(idLobbyBackend* lobbyBackend);
    virtual void PumpLobbies();
    virtual void JoinAfterSwap(void* joinID);

    virtual bool GetLobbyAddressFromNetAddress(const netadr_t& netAddr, lobbyAddress_t& outAddr) const;
    virtual bool GetNetAddressFromLobbyAddress(const lobbyAddress_t& lobbyAddress, netadr_t& outNetAddr) const;

public:
    void Connect_f(const idCmdArgs& args);

    // game browser / game-specific friends (sys_net_friends.h)
    void QueryAddress(const netadr_t& addr, bool friendRequest, bool invite = false);
    void AddFriendByAddress(const char* text);
    void PromptAddFriendByAddress();
    void ToggleFriend(int serverIndex);
    void ConnectToAddress(const netadr_t& addr);
    bool IsServerFriend(int i) const;

private:
    void EnsurePort();
    void SendBrowserQueries();
    void WriteQuery(idBitMsg& msg, bool friendRequest, bool invite = false);

    struct browserEntry_t {
        lobbyAddress_t addr;
        serverInfo_t info;
        idStr hostId;
        idStr hostName;
        bool isFriend;
    };
    idList<browserEntry_t> browserList;
    idCallback* browserCallback;
    int browserNextQueryMs;
    int browserQueriesLeft;
    netadr_t pendingFriendAddr; // "Add Friend by Address": waiting for this one to answer
    int pendingFriendUntilMs;
    bool textInputActive;
    // online: the first-time questions (disclaimer, username) and the ban notice
    enum { ONLINE_ASK_NONE, ONLINE_ASK_DISCLAIMER, ONLINE_ASK_USERNAME } onlineAsking;
    bool onlineAskedThisSession;
    bool onlineBannedShown;
    bool textInputIsUsername;

public:
    void OnlineDisclaimerAnswer(bool accepted);
    void SendRawFromGamePort(const netadr_t& to, const byte* data, int len);

    idLobbyBackend* CreateLobbyInternal(idLobbyBackend::lobbyBackendType_t lobbyType);

    idArray<idLobbyBackend*, 3> lobbyBackends;

    idNetSessionPort port;
    bool canJoinLocalHost;

    idLobbyToSessionCBLocal* lobbyToSessionCB;
};

idSessionLocalWin sessionLocalWin;
idSession* session = &sessionLocalWin;

void Session_OnlineDisclaimerAnswer(bool accepted)
{
    sessionLocalWin.OnlineDisclaimerAnswer(accepted);
}

static void Session_NetOnlineSend(const netadr_t& to, const byte* data, int len)
{
    sessionLocalWin.SendRawFromGamePort(to, data, len);
}
static struct netOnlineSendInit_t {
    netOnlineSendInit_t()
    {
        netOnlineSend = Session_NetOnlineSend;
    }
} netOnlineSendInit;

/*
========================
idLobbyToSessionCBLocal
========================
*/
class idLobbyToSessionCBLocal : public idLobbyToSessionCB {
public:
    idLobbyToSessionCBLocal(idSessionLocalWin* sessionLocalWin_)
        : sessionLocalWin(sessionLocalWin_)
    {
    }

    virtual bool CanJoinLocalHost() const
    {
        sessionLocalWin->EnsurePort();
        return sessionLocalWin->canJoinLocalHost;
    }
    virtual class idLobbyBackend* GetLobbyBackend(idLobbyBackend::lobbyBackendType_t type) const
    {
        return sessionLocalWin->lobbyBackends[type];
    }

private:
    idSessionLocalWin* sessionLocalWin;
};

idLobbyToSessionCBLocal lobbyToSessionCBLocal(&sessionLocalWin);
idLobbyToSessionCB* lobbyToSessionCB = &lobbyToSessionCBLocal;

class idVoiceChatMgrWin : public idVoiceChatMgr {
public:
    virtual bool GetLocalChatDataInternal(int talkerIndex, byte* data, int& dataSize)
    {
        return false;
    }
    virtual void SubmitIncomingChatDataInternal(int talkerIndex, const byte* data, int dataSize) { }
    virtual bool TalkerHasData(int talkerIndex)
    {
        return false;
    }
    virtual bool RegisterTalkerInternal(int index)
    {
        return true;
    }
    virtual void UnregisterTalkerInternal(int index) { }
};

/*
========================
idSessionLocalWin::idSessionLocalWin
========================
*/
idSessionLocalWin::idSessionLocalWin()
{
    signInManager = new (TAG_SYSTEM) idSignInManagerWin;
    saveGameManager = new (TAG_SAVEGAMES) idSaveGameManager();
    voiceChat = new (TAG_SYSTEM) idVoiceChatMgrWin();
    lobbyToSessionCB = new (TAG_SYSTEM) idLobbyToSessionCBLocal(this);

    canJoinLocalHost = false;

    lobbyBackends.Zero();

    browserCallback = NULL;
    browserNextQueryMs = 0;
    browserQueriesLeft = 0;
    memset(&pendingFriendAddr, 0, sizeof(pendingFriendAddr));
    pendingFriendAddr.type = NA_BAD;
    pendingFriendUntilMs = 0;
    textInputActive = false;
    onlineAsking = ONLINE_ASK_NONE;
    onlineAskedThisSession = false;
    onlineBannedShown = false;
    textInputIsUsername = false;
}

/*
========================
idSessionLocalWin::idSessionLocalWin
========================
*/
idSessionLocalWin::~idSessionLocalWin()
{
    delete voiceChat;
    delete lobbyToSessionCB;
}

/*
========================
idSessionLocalWin::Initialize
========================
*/
void idSessionLocalWin::Initialize()
{
    idSessionLocal::Initialize();

    // The shipping path doesn't load title storage
    // Instead, we inject values through code which is protected through steam DRM
    titleStorageVars.Set("MAX_PLAYERS_ALLOWED", "8");
    titleStorageLoaded = true;

    // First-time check for downloadable content once game is launched
    EnumerateDownloadableContent();

    GetPartyLobby().Initialize(idLobby::TYPE_PARTY, sessionCallbacks);
    GetGameLobby().Initialize(idLobby::TYPE_GAME, sessionCallbacks);
    GetGameStateLobby().Initialize(idLobby::TYPE_GAME_STATE, sessionCallbacks);

#if defined(__vita__)
    // the PS3 set's trophies, through the Vita's trophy service (vita_trophies.cpp)
    extern idAchievementSystem* Vita_CreateAchievementSystem();
    achievementSystem = Vita_CreateAchievementSystem();
#else
    achievementSystem = new (TAG_SYSTEM) idAchievementSystemWin();
#endif
    achievementSystem->Init();
}

/*
========================
idSessionLocalWin::Shutdown
========================
*/
void idSessionLocalWin::Shutdown()
{
    NET_VERBOSE_PRINT("NET: Shutdown\n");
    idSessionLocal::Shutdown();

    MoveToMainMenu();

    // Wait until we fully shutdown
#if defined(USE_DOOMCLASSIC)
    while (localState != STATE_IDLE && localState != STATE_PRESS_START)
#else
    while (localState != STATE_IDLE)
#endif
    {
        Pump();
    }

    if (achievementSystem != NULL) {
        achievementSystem->Shutdown();
        delete achievementSystem;
        achievementSystem = NULL;
    }
}

/*
========================
idSessionLocalWin::InitializeSoundRelatedSystems
========================
*/
void idSessionLocalWin::InitializeSoundRelatedSystems()
{
    if (voiceChat != NULL) {
        voiceChat->Init(NULL);
    }
}

/*
========================
idSessionLocalWin::ShutdownSoundRelatedSystems
========================
*/
void idSessionLocalWin::ShutdownSoundRelatedSystems()
{
    if (voiceChat != NULL) {
        voiceChat->Shutdown();
    }
}

/*
========================
idSessionLocalWin::PlatformPump
========================
*/
void idSessionLocalWin::PlatformPump()
{
    const int now = Sys_Milliseconds();

    // the browser asks a few times: a broadcast or a friend's answer can be lost
    if (browserCallback != NULL && browserQueriesLeft > 0 && now >= browserNextQueryMs) {
        SendBrowserQueries();
    }

    // the on-screen keyboard: a friend's address, or the username
    if (textInputActive) {
        idStr text;
        if (Sys_TextInputPoll(text)) {
            textInputActive = false;
            if (textInputIsUsername) {
                textInputIsUsername = false;
                onlineAsking = ONLINE_ASK_NONE;
                netFriends.SetUsername(text.Length() > 0 ? text.c_str() : netFriends.LocalName());
                idLocalUserWin* user = static_cast<idLocalUserWin*>(GetSignInManager().GetMasterLocalUser());
                if (user != NULL) {
                    user->SetGamerTag(netFriends.LocalName());
                }
            } else if (text.Length() > 0) {
                AddFriendByAddress(text);
            }
        }
    }

    // Online play (sys_net_online.cpp): while a multiplayer lobby is up. The
    // first time: the disclaimer, then the username (offline-mode style: no
    // password, no sign-in).
    const bool inMultiplayer = GetPartyLobby().IsLobbyActive() || GetGameLobby().IsLobbyActive();
    if (inMultiplayer && !onlineAskedThisSession && onlineAsking == ONLINE_ASK_NONE) {
        onlineAskedThisSession = true;
        if (netOnline.IsThisConsoleBanned()) {
            if (!onlineBannedShown) {
                onlineBannedShown = true;
                class idSWFScriptFunction_CloseBanned : public idSWFScriptFunction_RefCounted {
                public:
                    idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
                    {
                        common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                        return idSWFScriptVar();
                    }
                };
                idStaticList<idSWFScriptFunction*, 4> callbacks;
                idStaticList<idStrId, 4> optionText;
                callbacks.Append(new (TAG_SWF) idSWFScriptFunction_CloseBanned());
                optionText.Append(idStrId("#STR_SWF_ACCEPT"));
                common->Dialog().AddDynamicDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, callbacks, optionText, false,
                    va("^1%s^0\n%s\n%s: %s", idLocalization::GetString("#str_vita_banned"), idLocalization::GetString("#str_vita_banned_notice"),
                        idLocalization::GetString("#str_vita_hw_id"), netOnline.HwIdHash()));
                idLib::Printf("net: this console is banned from online play (HW ID hash %s)\n", netOnline.HwIdHash());
            }
        } else if (netOnline.NeedsDisclaimer()) {
            onlineAsking = ONLINE_ASK_DISCLAIMER;
            class idSWFScriptFunction_OnlineDisclaimer : public idSWFScriptFunction_RefCounted {
            public:
                idSWFScriptFunction_OnlineDisclaimer(bool a) : accept(a) {}
                idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
                {
                    common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                    extern void Session_OnlineDisclaimerAnswer(bool accepted);
                    Session_OnlineDisclaimerAnswer(accept);
                    return idSWFScriptVar();
                }
                bool accept;
            };
            idStaticList<idSWFScriptFunction*, 4> callbacks;
            idStaticList<idStrId, 4> optionText;
            callbacks.Append(new (TAG_SWF) idSWFScriptFunction_OnlineDisclaimer(true));
            callbacks.Append(new (TAG_SWF) idSWFScriptFunction_OnlineDisclaimer(false));
            optionText.Append(idStrId("#STR_SWF_ACCEPT"));
            optionText.Append(idStrId("#STR_SWF_CANCEL"));
            common->Dialog().AddDynamicDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, callbacks, optionText, false, idLocalization::GetString("#str_vita_disclaimer"));
            idLib::Printf("net: online disclaimer shown\n");
        } else if (!netFriends.HasUsername()) {
            onlineAsking = ONLINE_ASK_USERNAME;
        }
    }
    if (onlineAsking == ONLINE_ASK_USERNAME && !textInputActive) {
        if (Sys_TextInputStart(idLocalization::GetString("#str_vita_username_title"), netFriends.LocalName())) {
            textInputActive = true;
            textInputIsUsername = true;
        } else {
            // no on-screen keyboard (PC): the system / ui_name one is kept
            onlineAsking = ONLINE_ASK_NONE;
            netFriends.SetUsername(netFriends.LocalName());
        }
    }
    if (!inMultiplayer) {
        onlineAskedThisSession = false;
    }
    if (inMultiplayer) {
        GetPort(); // the DHT talks from the game port
    }
    netOnline.Pump(inMultiplayer && onlineAsking == ONLINE_ASK_NONE);
    {
        idList<netadr_t> targets;
        netOnline.GetHelloTargets(targets);
        for (int i = 0; i < targets.Num(); i++) {
            QueryAddress(targets[i], false);
        }
    }

    if (pendingFriendAddr.type != NA_BAD && now > pendingFriendUntilMs) {
        idLib::Printf("net: nobody answered at %s - no friend added (is their game in the multiplayer menu, and UDP port %d forwarded to it?)\n",
            Sys_NetAdrToString(pendingFriendAddr), pendingFriendAddr.port);
        common->Dialog().AddDialog(GDM_CONNECTION_PROBLEMS, DIALOG_ACCEPT, NULL, NULL, false, __FUNCTION__, __LINE__, true);
        pendingFriendAddr.type = NA_BAD;
    }
}
void idSessionLocalWin::OnlineDisclaimerAnswer(bool accepted)
{
    onlineAsking = ONLINE_ASK_NONE;
    if (accepted) {
        netOnline.AcceptDisclaimer();
        if (!netFriends.HasUsername()) {
            onlineAsking = ONLINE_ASK_USERNAME;
        }
    } else {
        idLib::Printf("net: online disclaimer declined - staying offline (local network only) until the next time Multiplayer is opened\n");
    }
}

void Session_OnlineDisclaimerAnswer(bool accepted);

void idSessionLocalWin::SendRawFromGamePort(const netadr_t& to, const byte* data, int len)
{
    lobbyAddress_t a;
    a.InitFromNetadr(to);
    GetPort().SendRawPacket(a, data, len);
}

/*
========================
idSessionLocalWin::InviteFriends
========================
*/
// invites sent and received (the full verification reads them: VitaVerify.cpp)
int net_invitesSent = 0;
int net_invitesReceived = 0;
idStr net_lastInviteFrom;
netadr_t net_lastInviteAddr;

bool net_verifyLoopback = false;
int net_verifyLoopbackInvites = 0;

// the invite to the friends with a known address (all of them, or only the
// ones listed: the verification's own)
int Session_SendInvites(const idStrList* onlyIds)
{
    netFriends.Load();
    int sent = 0;
    for (int i = 0; i < netFriends.Num(); i++) {
        const netFriend_t& f = netFriends.Get(i);
        if (onlyIds != NULL && onlyIds->FindIndex(f.id) < 0) {
            continue;
        }
        if (f.addr.type == NA_IP || f.addr.type == NA_LOOPBACK) {
            sessionLocalWin.QueryAddress(f.addr, false, true);
            sent++;
        }
    }
    net_invitesSent += sent;
    idLib::Printf("net: invite sent to %d friend(s) of %d\n", sent, netFriends.Num());
    return sent;
}

// the verification: an invite to an address of its choice (our own port)
void Session_SendInviteTo(const netadr_t& addr)
{
    sessionLocalWin.QueryAddress(addr, false, true);
}

void idSessionLocalWin::InviteFriends()
{
    // No platform overlay to pick friends in: every friend with a known
    // address gets the invite - a query carrying the invite flag in its tag.
    // Theirs shows "<name> invites you to join a game" (JOIN / DECLINE); a
    // friend in a game of their own sees it in the console only.
    const int sent = Session_SendInvites(NULL);
    if (common->Dialog().HasAnyActiveDialog()) {
        return;
    }
    class idSWFScriptFunction_CloseInvited : public idSWFScriptFunction_RefCounted {
    public:
        idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
        {
            common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
            return idSWFScriptVar();
        }
    };
    idStaticList<idSWFScriptFunction*, 4> callbacks;
    idStaticList<idStrId, 4> optionText;
    callbacks.Append(new (TAG_SWF) idSWFScriptFunction_CloseInvited());
    optionText.Append(idStrId("#STR_SWF_ACCEPT"));
    common->Dialog().AddDynamicDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, callbacks, optionText, false,
        sent > 0 ? va(idLocalization::GetString("#str_vita_invites_sent"), sent) : idLocalization::GetString("#str_vita_no_friends_to_invite"));
}

// a friend's invite accepted: off to their game
void Session_JoinInvite(const netadr_t& addr)
{
    idLib::Printf("net: joining the game at %s (invited)\n", Sys_NetAdrToString(addr));
    netFriends.NotePlayed("joined a friend's game (invited)");
    sessionLocalWin.ConnectToAddress(addr);
}

static void Session_ShowInvite(const netadr_t& from, const char* name)
{
    class idSWFScriptFunction_Invite : public idSWFScriptFunction_RefCounted {
    public:
        idSWFScriptFunction_Invite(const netadr_t& a, bool j) : addr(a), join(j) {}
        idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
        {
            common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
            if (join) {
                Session_JoinInvite(addr);
            } else {
                idLib::Printf("net: invite declined\n");
            }
            return idSWFScriptVar();
        }
        netadr_t addr;
        bool join;
    };
    idStaticList<idSWFScriptFunction*, 4> callbacks;
    idStaticList<idStrId, 4> optionText;
    callbacks.Append(new (TAG_SWF) idSWFScriptFunction_Invite(from, true));
    callbacks.Append(new (TAG_SWF) idSWFScriptFunction_Invite(from, false));
    optionText.Append(idStrId("#str_vita_join"));
    optionText.Append(idStrId("#str_vita_decline"));
    common->Dialog().AddDynamicDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, callbacks, optionText, false, va(idLocalization::GetString("#str_vita_invite_from"), name));
}

/*
========================
idSessionLocalWin::InviteParty
========================
*/
void idSessionLocalWin::InviteParty()
{
}

/*
========================
idSessionLocalWin::ShowPartySessions
========================
*/
void idSessionLocalWin::ShowPartySessions()
{
}

/*
========================
idSessionLocalWin::ShowSystemMarketplaceUI
========================
*/
void idSessionLocalWin::ShowSystemMarketplaceUI() const
{
}

/*
========================
idSessionLocalWin::ListServers
========================
*/
void idSessionLocalWin::ListServers(const idCallback& callback)
{
    Sys_RefreshNetworking();
    netFriends.Load();
    netFriends.LocalId();
    browserList.Clear();
    if (browserCallback != NULL) {
        delete browserCallback;
    }
    browserCallback = callback.Clone();
    browserQueriesLeft = 3;
    browserNextQueryMs = 0;
    SendBrowserQueries();
    // nothing heard is a valid answer too: the browser shows its list (and
    // the friends who are not hosting) at once, games join it as they answer
    browserCallback->Call();
}

/*
========================
idSessionLocalWin::WriteQuery

The stock query (the version checksum) and who is asking.
========================
*/
static const int NET_OWNER_BADGE_MAGIC = ('D' << 24) | ('3' << 16) | ('O' << 8) | 'B';

// the owner's console: its badge after the tag (older builds stop reading at the tag)
static void WriteOwnerBadgeTrailer(idBitMsg& msg)
{
    byte badge[512];
    const int len = netOnline.WriteOwnerBadge(badge, Min((int)sizeof(badge), msg.GetRemainingSpace() - 6));
    if (len > 0) {
        msg.WriteLong(NET_OWNER_BADGE_MAGIC);
        msg.WriteShort((short)len);
        msg.WriteData(badge, len);
    }
}

static void ReadOwnerBadgeTrailer(idBitMsg& msg, const netadr_t& from, const netPeerTag_t& tag)
{
    if (!tag.valid || msg.GetRemainingData() < 6) {
        return;
    }
    if (msg.ReadLong() != NET_OWNER_BADGE_MAGIC) {
        return;
    }
    const int len = (unsigned short)msg.ReadShort();
    if (len <= 0 || len > 512 || len > msg.GetRemainingData()) {
        return;
    }
    byte badge[512];
    msg.ReadData(badge, len);
    netOnline.OfferOwnerBadge(from, tag.name, tag.hwIdHash, badge, len);
}

void idSessionLocalWin::WriteQuery(idBitMsg& msg, bool friendRequest, bool invite)
{
    msg.WriteLong(NetGetVersionChecksum());
    netFriends.WriteTag(msg, friendRequest, false, invite);
    WriteOwnerBadgeTrailer(msg);
}

/*
========================
idSessionLocalWin::QueryAddress
========================
*/
void idSessionLocalWin::QueryAddress(const netadr_t& addr, bool friendRequest, bool invite)
{
    byte buffer[idPacketProcessor::MAX_PACKET_SIZE - 2];
    idBitMsg msg(buffer, sizeof(buffer));
    WriteQuery(msg, friendRequest, invite);
    lobbyAddress_t address;
    address.InitFromNetadr(addr);
    GetPort();
    GetPartyLobby().SendConnectionLess(address, idLobby::OOB_MATCH_QUERY, msg.GetReadData(), msg.GetSize());
}

/*
========================
idSessionLocalWin::SendBrowserQueries

The local network (everyone at once) and every friend at their last address.
========================
*/
void idSessionLocalWin::SendBrowserQueries()
{
    browserQueriesLeft--;
    browserNextQueryMs = Sys_Milliseconds() + 700;

    netadr_t broadcast;
    memset(&broadcast, 0, sizeof(broadcast));
    broadcast.type = NA_BROADCAST;
    broadcast.port = net_port.GetInteger();
    QueryAddress(broadcast, false);

    // the subnet's own broadcast address too: some access points drop
    // 255.255.255.255
    for (int i = 0; i < Sys_GetLocalIPCount(); i++) {
        netadr_t local;
        if (!Sys_StringToNetAdr(Sys_GetLocalIP(i), &local, false) || local.type != NA_IP || local.ip[0] == 127) {
            continue;
        }
        unsigned int mask = 0;
        if (!Sys_GetLocalIPMask(i, mask) || mask == 0 || mask == 0xFFFFFFFF) {
            continue;
        }
        const unsigned int ip = (local.ip[0] << 24) | (local.ip[1] << 16) | (local.ip[2] << 8) | local.ip[3];
        const unsigned int bc = ip | ~mask;
        netadr_t subnet = local;
        subnet.ip[0] = (bc >> 24) & 255;
        subnet.ip[1] = (bc >> 16) & 255;
        subnet.ip[2] = (bc >> 8) & 255;
        subnet.ip[3] = bc & 255;
        subnet.port = net_port.GetInteger();
        QueryAddress(subnet, false);
    }

    int asked = 0;
    // everyone the DHT found (hellos go to them all the time; this asks now)
    {
        idList<netadr_t> all;
        netOnline.GetHelloTargets(all);
        for (int i = 0; i < all.Num(); i++) {
            QueryAddress(all[i], false);
            asked++;
        }
    }
    for (int i = 0; i < netFriends.Num(); i++) {
        const netFriend_t& f = netFriends.Get(i);
        if ((f.addr.type == NA_IP || f.addr.type == NA_LOOPBACK)) {
            QueryAddress(f.addr, false);
            asked++;
        }
    }
    idLib::Printf("net: looking for games: local network broadcast + %d friend address(es) (%d more round(s))\n", asked, browserQueriesLeft);
}
/*
========================
idSessionLocalWin::CancelListServers
========================
*/
void idSessionLocalWin::CancelListServers()
{
    if (browserCallback != NULL) {
        delete browserCallback;
        browserCallback = NULL;
    }
    browserQueriesLeft = 0;
}
/*
========================
idSessionLocalWin::NumServers
========================
*/
int idSessionLocalWin::NumServers() const
{
    return browserList.Num();
}
/*
========================
idSessionLocalWin::ServerInfo
========================
*/
const serverInfo_t* idSessionLocalWin::ServerInfo(int i) const
{
    if (i < 0 || i >= browserList.Num()) {
        return NULL;
    }
    return &browserList[i].info;
}

/*
========================
idSessionLocalWin::IsServerFriend
========================
*/
bool idSessionLocalWin::IsServerFriend(int i) const
{
    return i >= 0 && i < browserList.Num() && browserList[i].isFriend;
}
/*
========================
idSessionLocalWin::ConnectToServer
========================
*/
void idSessionLocalWin::ConnectToServer(int i)
{
    if (i < 0 || i >= browserList.Num()) {
        return;
    }
    idLib::Printf("net: joining %s's game at %s\n", browserList[i].hostName.c_str(), browserList[i].addr.ToString());
    netFriends.NotePlayed(va("joined %s's game", browserList[i].hostName.c_str()));
    ConnectToAddress(browserList[i].addr.netAddr);
}

/*
========================
idSessionLocalWin::ConnectToAddress
========================
*/
void idSessionLocalWin::ConnectToAddress(const netadr_t& addr)
{
    Cancel();

    if (signInManager->GetMasterLocalUser() == NULL) {
        signInManager->RegisterLocalUser(0);
    }

    lobbyConnectInfo_t connectInfo;
    connectInfo.netAddr = addr;
    if (connectInfo.netAddr.port == 0) {
        connectInfo.netAddr.port = 27015;
    }
    ConnectAndMoveToLobby(GetPartyLobby(), connectInfo, false);
}
/*
========================
idSessionLocalWin::Connect_f
========================
*/
void idSessionLocalWin::Connect_f(const idCmdArgs& args)
{
    if (args.Argc() < 2) {
        idLib::Printf("Usage: Connect to IP. Use IP:Port to specify port (e.g. 10.0.0.1:1234) \n");
        return;
    }

    Cancel();

    if (signInManager->GetMasterLocalUser() == NULL) {
        signInManager->RegisterLocalUser(0);
    }

    lobbyConnectInfo_t connectInfo;

    Sys_StringToNetAdr(args.Argv(1), &connectInfo.netAddr, true);
    // DG: don't use net_port to select port to connect to
    //     the port can be specified in the command, else the default port is used
    if (connectInfo.netAddr.port == 0) {
        connectInfo.netAddr.port = 27015;
    }
    // DG end

    ConnectAndMoveToLobby(GetPartyLobby(), connectInfo, false);
}

/*
========================
void Connect_f
========================
*/
CONSOLE_COMMAND(connect, "Connect to the specified IP", NULL)
{
    sessionLocalWin.Connect_f(args);
}

/*
========================
idSessionLocalWin::ShowServerGamerCardUI
========================
*/
void idSessionLocalWin::ShowServerGamerCardUI(int i)
{
    // no gamer cards here: the browser's Triangle adds / removes the host as
    // a friend instead
    ToggleFriend(i);
}

/*
========================
idSessionLocalWin::ToggleFriend
========================
*/
void idSessionLocalWin::ToggleFriend(int i)
{
    if (i < 0 || i >= browserList.Num()) {
        return;
    }
    browserEntry_t& e = browserList[i];
    if (e.hostId.Length() != 16) {
        idLib::Printf("net: %s runs an older build without friends support\n", e.addr.ToString());
        return;
    }
    if (e.isFriend) {
        netFriends.Remove(e.hostId);
        e.isFriend = false;
    } else {
        netFriends.Add(e.hostId, e.hostName, e.addr.netAddr);
        e.isFriend = true;
        QueryAddress(e.addr.netAddr, true); // "I added you": they add us back
    }
    if (browserCallback != NULL) {
        browserCallback->Call();
    }
}

/*
========================
idSessionLocalWin::AddFriendByAddress

"192.168.1.20", "203.0.113.7:27015" or a host name: asked directly; when a
game answers from there its host becomes a friend (and adds us back).
========================
*/
void idSessionLocalWin::AddFriendByAddress(const char* text)
{
    netadr_t addr;
    if (!Sys_StringToNetAdr(text, &addr, true) || (addr.type != NA_IP && addr.type != NA_LOOPBACK)) {
        idLib::Printf("net: '%s' is not an address\n", text);
        common->Dialog().AddDialog(GDM_CONNECTION_PROBLEMS, DIALOG_ACCEPT, NULL, NULL, false, __FUNCTION__, __LINE__, true);
        return;
    }
    if (addr.port == 0) {
        addr.port = 27015;
    }
    netFriends.Load();
    netFriends.LocalId();
    pendingFriendAddr = addr;
    pendingFriendUntilMs = Sys_Milliseconds() + 4000;
    idLib::Printf("net: asking %s to be friends\n", Sys_NetAdrToString(addr));
    QueryAddress(addr, true);
    QueryAddress(addr, true);
}

/*
========================
idSessionLocalWin::PromptAddFriendByAddress
========================
*/
void idSessionLocalWin::PromptAddFriendByAddress()
{
    if (Sys_TextInputStart(idLocalization::GetString("#str_vita_friend_addr_title"), "")) {
        textInputActive = true;
    } else {
        idLib::Printf("net: no on-screen keyboard here - use: net_addFriend <ip[:port]>\n");
    }
}

CONSOLE_COMMAND(net_addFriend, "Add a friend by the address of their game: net_addFriend <ip[:port]>", NULL)
{
    if (args.Argc() < 2) {
        sessionLocalWin.PromptAddFriendByAddress();
        return;
    }
    sessionLocalWin.AddFriendByAddress(args.Argv(1));
}

CONSOLE_COMMAND(net_hostParty, "Host a multiplayer party, as entering the multiplayer menu does (testing)", NULL)
{
    if (session->GetSignInManager().GetMasterLocalUser() == NULL) {
        session->GetSignInManager().RegisterLocalUser(0);
    }
    idMatchParameters matchParameters;
    matchParameters.matchFlags = DefaultPartyFlags;
    session->CreatePartyLobby(matchParameters);
}

CONSOLE_COMMAND(net_showProfile, "Show a party member's profile, as Square in the lobby does: net_showProfile [ordinal] (testing)", NULL)
{
    idLobbyBase& lobby = session->GetPartyLobbyBase();
    const int ordinal = args.Argc() > 1 ? atoi(args.Argv(1)) : 0;
    lobbyUserID_t luid = lobby.GetLobbyUserIdByOrdinal(ordinal);
    if (!luid.IsValid()) {
        idLib::Printf("net: no party member %d (net_hostParty first)\n", ordinal);
        return;
    }
    session->ShowLobbyUserGamerCardUI(luid);
}

CONSOLE_COMMAND(net_listGames, "Look for games on the local network and at friends' addresses (testing)", NULL)
{
    class idListGamesDone : public idCallback {
    public:
        virtual void Call()
        {
            for (int i = 0; i < session->NumServers(); i++) {
                const serverInfo_t* s = session->ServerInfo(i);
                idLib::Printf("net:   [%d] %s %d/%d\n", i, s->serverName.c_str(), s->numPlayers, s->maxPlayers);
            }
        }
        virtual idCallback* Clone() const
        {
            return new idListGamesDone();
        }
    };
    session->ListServers(idListGamesDone());
}

CONSOLE_COMMAND(net_addFriendPrompt,"Ask for a friend's address on the on-screen keyboard", NULL)
{
    sessionLocalWin.PromptAddFriendByAddress();
}

CONSOLE_COMMAND(net_removeFriend, "Remove a friend: net_removeFriend <id or name>", NULL)
{
    if (args.Argc() < 2) {
        return;
    }
    netFriends.Load();
    for (int i = 0; i < netFriends.Num(); i++) {
        const netFriend_t& f = netFriends.Get(i);
        if (f.id.Icmp(args.Argv(1)) == 0 || f.name.Icmp(args.Argv(1)) == 0) {
            const idStr id = f.id;
            netFriends.Remove(id);
            return;
        }
    }
    idLib::Printf("net: no friend '%s'\n", args.Argv(1));
}

CONSOLE_COMMAND(net_listFriends, "List the game-specific friends", NULL)
{
    netFriends.Load();
    idLib::Printf("net: you are %s (%s)\n", netFriends.LocalName(), netFriends.LocalId());
    for (int i = 0; i < netFriends.Num(); i++) {
        const netFriend_t& f = netFriends.Get(i);
        idLib::Printf("  %s  %-20s %s\n", f.id.c_str(), f.name.c_str(), f.addr.type == NA_BAD ? "-" : Sys_NetAdrToString(f.addr));
    }
}

CONSOLE_COMMAND(net_joinFriend, "Join a friend's game: net_joinFriend <id or name>", NULL)
{
    if (args.Argc() < 2) {
        return;
    }
    netFriends.Load();
    for (int i = 0; i < netFriends.Num(); i++) {
        const netFriend_t& f = netFriends.Get(i);
        if ((f.id.Icmp(args.Argv(1)) == 0 || f.name.Icmp(args.Argv(1)) == 0) && (f.addr.type == NA_IP || f.addr.type == NA_LOOPBACK)) {
            sessionLocalWin.ConnectToAddress(f.addr);
            return;
        }
    }
    idLib::Printf("net: no friend '%s' with an address\n", args.Argv(1));
}
/*
========================
idSessionLocalWin::ShowLobbyUserGamerCardUI(
========================
*/
void idSessionLocalWin::ShowLobbyUserGamerCardUI(lobbyUserID_t lobbyUserID)
{
    // No platform gamer cards: Square on a lobby member shows this game's own
    // profile - for yourself your name, player id and friends; for anyone
    // else their name and whether they are a friend.
    class idSWFScriptFunction_CloseProfile : public idSWFScriptFunction_RefCounted {
    public:
        idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
        {
            common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
            return idSWFScriptVar();
        }
    };

    idLobbyBase& lobby = GetActivePlatformLobbyBase();
    const char* name = lobby.GetLobbyUserName(lobbyUserID);
    const bool isLocal = lobby.GetLocalUserFromLobbyUser(lobbyUserID) != NULL;
    netFriends.Load();

    // Four lines - title, name, player id, HW ID (the layout chosen on the
    // console) - plus one for a ban notice: the game's dialog draws nothing at all past
    // five lines (measured on PC: 5 show, 6 leave an empty panel that still
    // takes the buttons - the "Square does nothing and locks" bug, the old
    // profile was 8 lines). The OWNER mark goes on the name line.
    idStr text;
    if (isLocal) {
        idStr nameLine = netFriends.LocalName();
        if (netFriends.IsOwner()) {
            nameLine += va(" ^3%s^0", idLocalization::GetString("#str_vita_owner_tag"));
        }
        // banned from online play by the owner: said plainly, in red (bans
        // from single players' games are theirs and are not shown)
        const bool banned = netOnline.IsThisConsoleBanned();
        if (banned) {
            nameLine += va(" ^1%s^0", idLocalization::GetString("#str_vita_banned"));
        }
        if (!banned) {
            text = va("%s\n%s\n%s: %s\n%s: %s", idLocalization::GetString("#str_vita_profile_title"), nameLine.c_str(),
                idLocalization::GetString("#str_vita_player_id"), netFriends.LocalId(), idLocalization::GetString("#str_vita_hw_id"), netOnline.HwIdHash());
        } else {
            // the five-line limit counts wrapped lines too: both ids share a
            // line so the appeal notice may take two in any language
            text = va("%s\n%s\n%s: %s   %s: %s\n^1%s^0", idLocalization::GetString("#str_vita_profile_title"), nameLine.c_str(),
                idLocalization::GetString("#str_vita_player_id"), netFriends.LocalId(), idLocalization::GetString("#str_vita_hw_id"), netOnline.HwIdHash(),
                idLocalization::GetString("#str_vita_banned_notice"));
        }
    } else {
        bool isFriend = false;
        for (int i = 0; i < netFriends.Num(); i++) {
            if (name != NULL && netFriends.Get(i).name.Icmp(name) == 0) {
                isFriend = true;
            }
        }
        const char* hw = netOnline.HwIdOfName(name != NULL ? name : "");
        idStr nameLine = name != NULL ? name : "?";
        if (isFriend) {
            nameLine += va(" %s", idLocalization::GetString("#str_vita_friend_suffix"));
        }
        if (name != NULL && netOnline.IsOwnerName(name)) {
            nameLine += va(" ^3%s^0", idLocalization::GetString("#str_vita_owner_tag"));
        }
        text = va("%s\n%s\n%s: %s", idLocalization::GetString("#str_vita_profile_title"), nameLine.c_str(), idLocalization::GetString("#str_vita_hw_id"),
            hw[0] ? hw : idLocalization::GetString("#str_vita_unknown"));
        if (hw[0] && netOnline.IsOwnerBanned(hw)) {
            text += va("\n^1%s^0", idLocalization::GetString("#str_vita_banned"));
        } else if (hw[0] && netOnline.IsBanned(hw)) {
            text += va(" %s", idLocalization::GetString("#str_vita_banned_from_yours"));
        }
    }
    if (common->Dialog().HasDialogMsg(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, NULL)) {
        // another of the port's dialogs is up (the disclaimer...): the dialog
        // system drops a second one of the same kind silently
        idLib::Printf("net: profile not shown - another dialog is open\n");
        return;
    }
    idLib::Printf("net: profile shown: %s\n", text.c_str());

    idStaticList<idSWFScriptFunction*, 4> callbacks;
    idStaticList<idStrId, 4> optionText;
    callbacks.Append(new (TAG_SWF) idSWFScriptFunction_CloseProfile());
    optionText.Append(idStrId("#STR_SWF_ACCEPT"));
    // another player whose console is known: ban it from my games
    if (!isLocal && name != NULL && netOnline.HwIdOfName(name)[0] != '\0') {
        class idSWFScriptFunction_BanPlayer : public idSWFScriptFunction_RefCounted {
        public:
            idSWFScriptFunction_BanPlayer(const char* h, const char* n) : hw(h), who(n) {}
            idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
            {
                common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                if (netOnline.IsBanned(hw) && !netOnline.IsOwnerBanned(hw)) {
                    netOnline.UnbanLocally(hw);
                } else if (!netOnline.IsBanned(hw)) {
                    netOnline.BanLocally(hw, who);
                }
                return idSWFScriptVar();
            }
            idStr hw, who;
        };
        const char* hw = netOnline.HwIdOfName(name);
        callbacks.Append(new (TAG_SWF) idSWFScriptFunction_BanPlayer(hw, name));
        optionText.Append(idStrId((netOnline.IsBanned(hw) && !netOnline.IsOwnerBanned(hw)) ? "#str_vita_unban" : "#str_vita_ban"));
        // the owner's console: ban from (or let back into) online play altogether
        if (netOnline.CanGlobalBan()) {
            class idSWFScriptFunction_GlobalBan : public idSWFScriptFunction_RefCounted {
            public:
                idSWFScriptFunction_GlobalBan(const char* h, bool b) : hw(h), ban(b) {}
                idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
                {
                    common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                    netOnline.GlobalBan(hw, ban);
                    return idSWFScriptVar();
                }
                idStr hw;
                bool ban;
            };
            const bool globallyBanned = netOnline.IsOwnerBanned(hw);
            callbacks.Append(new (TAG_SWF) idSWFScriptFunction_GlobalBan(hw, !globallyBanned));
            optionText.Append(idStrId(globallyBanned ? "#str_vita_global_unban" : "#str_vita_global_ban"));
        }
    }
    common->Dialog().AddDynamicDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL, callbacks, optionText, false, text.c_str());
}

/*
========================
idSessionLocalWin::ProcessInputEvent
========================
*/
bool idSessionLocalWin::ProcessInputEvent(const sysEvent_t* ev)
{
    if (GetSignInManager().ProcessInputEvent(ev)) {
        return true;
    }
    return false;
}

/*
========================
idSessionLocalWin::IsSystemUIShowing
========================
*/
bool idSessionLocalWin::IsSystemUIShowing() const
{
    // DG: pausing here when window is out of focus like originally done on windows is hacky
    // it's done with com_pause now.
    return isSysUIShowing;
}

/*
========================
idSessionLocalWin::SetSystemUIShowing
========================
*/
void idSessionLocalWin::SetSystemUIShowing(bool show)
{
    isSysUIShowing = show;
}

/*
========================
idSessionLocalWin::HandleServerQueryRequest
========================
*/
void idSessionLocalWin::HandleServerQueryRequest(lobbyAddress_t& remoteAddr, idBitMsg& msg, int msgType)
{
    NET_VERBOSE_PRINT("HandleServerQueryRequest from %s\n", remoteAddr.ToString());

    const unsigned int remoteChecksum = msg.ReadLong();
    const netPeerTag_t asker = idNetFriends::ReadTag(msg);

    if (asker.valid && asker.id.Icmp(netFriends.LocalId()) == 0) {
        // our own broadcast - or the verification's invite sent to our own
        // port, through the real socket and back (VitaVerify.cpp)
        extern bool net_verifyLoopback;
        extern int net_verifyLoopbackInvites;
        if (net_verifyLoopback && asker.invite) {
            net_verifyLoopbackInvites++;
        }
        return;
    }
    if (asker.valid) {
        netOnline.NotePeer(remoteAddr.netAddr, asker.id, asker.name, asker.hwIdHash);
        ReadOwnerBadgeTrailer(msg, remoteAddr.netAddr, asker);
    }
    if (asker.valid) {
        netFriends.Load();
        if (asker.friendRequest) {
            if (netFriends.Add(asker.id, asker.name, remoteAddr.netAddr)) {
                idLib::Printf("net: %s added you as a friend - added back\n", asker.name.c_str());
            }
        } else {
            netFriends.Seen(asker.id, asker.name, remoteAddr.netAddr);
        }
        // a friend's invite to their game
        if (asker.invite) {
            if (netFriends.Find(asker.id) < 0) {
                idLib::Printf("net: invite from %s ignored (not a friend)\n", asker.name.c_str());
            } else if (netOnline.IsBanned(asker.hwIdHash)) {
                idLib::Printf("net: invite from %s ignored (banned from your games)\n", asker.name.c_str());
            } else {
                net_invitesReceived++;
                net_lastInviteFrom = asker.name;
                net_lastInviteAddr = remoteAddr.netAddr;
                const char* playing = commonLocal.GetSpawnedMapName();
                if ((playing != NULL && playing[0] != '\0') || common->Dialog().HasAnyActiveDialog()) {
                    idLib::Printf("net: %s invites you to their game at %s - you are busy; join it from Play With Friends\n", asker.name.c_str(), remoteAddr.ToString());
                } else {
                    idLib::Printf("net: %s invites you to their game at %s\n", asker.name.c_str(), remoteAddr.ToString());
                    Session_ShowInvite(remoteAddr.netAddr, asker.name.c_str());
                }
            }
        }
    }

    // Is there a game here to join? Anyone in the multiplayer menus hosts a
    // party others can join (a peer in someone else's party does not)
    bool canJoin = true;
    const char* why = "";
    idLocalUser* masterUser = GetSignInManager().GetMasterLocalUser();
    if (remoteChecksum != NetGetVersionChecksum()) {
        canJoin = false;
        why = " (another version of the game)";
    } else if (!GetPartyLobby().IsHost()) {
        canJoin = false;
        why = " (not hosting)";
    } else if (GetActivePlatformLobby() == NULL) {
        canJoin = false;
        why = " (no lobby)";
    } else if (GetPartyLobby().NumFreeSlots() == 0 || (GetGameLobby().IsLobbyActive() && GetGameLobby().NumFreeSlots() == 0)) {
        canJoin = false;
        why = " (full)";
    } else if (MatchTypeInviteOnly(GetPartyLobby().parms.matchFlags) && !(asker.valid && netFriends.Find(asker.id) >= 0)
               && !Sys_IsLANAddress(remoteAddr.netAddr)) {
        // invite only (the party's default): friends and the local network
        // still see it, strangers over the internet do not
        canJoin = false;
        why = " (invite only)";
    } else if (masterUser == NULL) {
        canJoin = false;
        why = " (nobody signed in)";
    } else if (asker.valid && netOnline.IsBanned(asker.hwIdHash)) {
        canJoin = false;
        why = " (that console is banned)";
    }

    byte buffer[idPacketProcessor::MAX_PACKET_SIZE - 2];
    idBitMsg retmsg(buffer, sizeof(buffer));
    retmsg.WriteBool(canJoin);
    if (canJoin) {
        serverInfo_t serverInfo;
        serverInfo.joinable = true;
        serverInfo.serverName = masterUser->GetGamerTag();
        if (GetGameLobby().IsLobbyActive()) {
            serverInfo.gameMap = GetGameLobby().parms.gameMap;
            serverInfo.gameMode = GetGameLobby().parms.gameMode;
        } else {
            serverInfo.gameMap = -1;
            serverInfo.gameMode = -1;
        }
        serverInfo.numPlayers = GetActivePlatformLobby()->GetNumLobbyUsers();
        serverInfo.maxPlayers = GetActivePlatformLobby()->parms.numSlots;
        serverInfo.Write(retmsg);
        for (int i = 0; i < serverInfo.numPlayers; i++) {
            retmsg.WriteString(GetActivePlatformLobby()->GetLobbyUserName(GetActivePlatformLobby()->GetLobbyUserIdByOrdinal(i)));
        }
    }
    netFriends.WriteTag(retmsg, false);
    // the owner's ban list goes on to anyone holding an older one
    int banBytes = 0;
    if (asker.valid && asker.banListVersion < netOnline.BanListVersion()) {
        byte blob[1100];
        banBytes = netOnline.WriteBanList(blob, Min((int)sizeof(blob), retmsg.GetRemainingSpace() - 8));
        if (banBytes > 0) {
            retmsg.WriteLong(banBytes);
            retmsg.WriteData(blob, banBytes);
        }
    }
    // then the owner badge, when there is room (a ban list goes first; the
    // badge comes with the next answer). Its trailer needs the ban list's
    // length before it, 0 when none.
    {
        byte badge[512];
        if (netOnline.WriteOwnerBadge(badge, sizeof(badge)) > 0) {
            if (banBytes == 0) {
                retmsg.WriteLong(0);
            }
            WriteOwnerBadgeTrailer(retmsg);
        }
    }
    GetPartyLobby().SendConnectionLess(remoteAddr, idLobby::OOB_MATCH_QUERY_ACK, retmsg.GetReadData(), retmsg.GetSize());
    if (net_onlineVerbose.GetBool() || asker.friendRequest || banBytes > 0 || !asker.valid || idStr::Length(why) > 0 && idStr::Icmp(why, " (not hosting)") != 0) {
        idLib::Printf("net: game query from %s (%s)%s - answered %s%s%s\n", remoteAddr.ToString(), asker.valid ? asker.name.c_str() : "no tag",
            asker.friendRequest ? " [friend request]" : "", canJoin ? "joinable" : "not joinable", why, banBytes > 0 ? va(" + the ban list (%d bytes)", banBytes) : "");
    }
}
/*
========================
idSessionLocalWin::HandleServerQueryAck
========================
*/
void idSessionLocalWin::HandleServerQueryAck(lobbyAddress_t& remoteAddr, idBitMsg& msg)
{
    NET_VERBOSE_PRINT("HandleServerQueryAck from %s\n", remoteAddr.ToString());

    const bool canJoin = msg.ReadBool();
    serverInfo_t info;
    idStrList players;
    if (canJoin) {
        info.Read(msg);
        for (int i = 0; i < info.numPlayers && i < 16; i++) {
            idStr user;
            msg.ReadString(user);
            players.Append(user);
        }
    }
    const netPeerTag_t host = idNetFriends::ReadTag(msg);
    if (host.valid && host.id.Icmp(netFriends.LocalId()) == 0) {
        return; // our own answer to our own broadcast
    }
    if (host.valid) {
        netOnline.NotePeer(remoteAddr.netAddr, host.id, host.name, host.hwIdHash);
        if (msg.GetRemainingData() >= 4) {
            const int banBytes = msg.ReadLong();
            if (banBytes > 0 && banBytes <= msg.GetRemainingData() && banBytes <= 1100) {
                byte blob[1100];
                msg.ReadData(blob, banBytes);
                netOnline.OfferBanList(blob, banBytes, host.name);
            }
            ReadOwnerBadgeTrailer(msg, remoteAddr.netAddr, host);
        }
    }

    netFriends.Load();
    bool isFriend = false;
    if (host.valid) {
        // "Add Friend by Address": this is the one we were waiting for
        if (pendingFriendAddr.type != NA_BAD && Sys_CompareNetAdrBase(pendingFriendAddr, remoteAddr.netAddr)) {
            pendingFriendAddr.type = NA_BAD;
            netFriends.Add(host.id, host.name, remoteAddr.netAddr);
            idLib::Printf("net: %s is now a friend\n", host.name.c_str());
        }
        isFriend = netFriends.Find(host.id) >= 0;
        if (isFriend) {
            netFriends.Seen(host.id, host.name, remoteAddr.netAddr);
        }
    }

    if (browserCallback == NULL) {
        return;
    }

    int found = -1;
    for (int i = 0; i < browserList.Num(); i++) {
        if (browserList[i].addr.Compare(remoteAddr, true) || (host.valid && browserList[i].hostId.Icmp(host.id) == 0)) {
            found = i;
            break;
        }
    }
    if (!canJoin) {
        // not hosting a game we may join: listed as a player online
        if (!host.valid) {
            if (found >= 0) {
                browserList.RemoveIndex(found);
                browserCallback->Call();
            }
            return;
        }
        info.serverName = host.name;
        info.joinable = false;
        info.gameMap = -1;
        info.gameMode = -1;
        info.numPlayers = 0;
        info.maxPlayers = 0;
    }
    const bool newEntry = (found < 0);
    browserEntry_t& e = (found >= 0) ? browserList[found] : browserList.Alloc();
    e.addr = remoteAddr;
    e.info = info;
    e.hostId = host.valid ? host.id : "";
    e.hostName = host.valid ? host.name : info.serverName;
    e.isFriend = isFriend;
    if (e.info.serverName.IsEmpty()) {
        e.info.serverName = remoteAddr.ToString();
    }
    if (isFriend) {
        e.info.serverName = va("%s %s", e.hostName.c_str(), idLocalization::GetString("#str_vita_friend_suffix"));
    }
    // the game's owner (a verified badge): marked after the name (and after
    // the friend suffix, which the browser matches friends by)
    if (host.valid && netOnline.IsOwnerName(host.name)) {
        e.info.serverName += va(" ^3%s^0", idLocalization::GetString("#str_vita_owner_tag"));
    }
    if (newEntry) {
        idLib::Printf("net: %s found: %s at %s, %d/%d players%s\n", canJoin ? "game" : "player online", e.info.serverName.c_str(), remoteAddr.ToString(), info.numPlayers, info.maxPlayers,
            host.valid ? "" : " (older build: no friends support)");
    }
    browserCallback->Call();
}
/*
========================
idSessionLocalWin::ClearBootableInvite
========================
*/
void idSessionLocalWin::ClearBootableInvite()
{
}

/*
========================
idSessionLocalWin::ClearPendingInvite
========================
*/
void idSessionLocalWin::ClearPendingInvite()
{
}

/*
========================
idSessionLocalWin::HandleBootableInvite
========================
*/
void idSessionLocalWin::HandleBootableInvite(int64 lobbyId)
{
}

/*
========================
idSessionLocalWin::HasPendingBootableInvite
========================
*/
bool idSessionLocalWin::HasPendingBootableInvite()
{
    return false;
}

/*
========================
idSessionLocal::SetDiscSwapMPInvite
========================
*/
void idSessionLocalWin::SetDiscSwapMPInvite(void* parm)
{
}

/*
========================
idSessionLocal::GetDiscSwapMPInviteParms
========================
*/
void* idSessionLocalWin::GetDiscSwapMPInviteParms()
{
    return NULL;
}

/*
========================
idSessionLocalWin::EnumerateDownloadableContent
========================
*/
void idSessionLocalWin::EnumerateDownloadableContent()
{
}

/*
========================
idSessionLocalWin::LeaderboardUpload
========================
*/
void idSessionLocalWin::LeaderboardUpload(lobbyUserID_t lobbyUserID, const leaderboardDefinition_t* leaderboard, const column_t* stats, const idFile_Memory* attachment)
{
}

/*
========================
idSessionLocalWin::LeaderboardFlush
========================
*/
void idSessionLocalWin::LeaderboardFlush()
{
}

/*
========================
idSessionLocalWin::LeaderboardDownload
========================
*/
void idSessionLocalWin::LeaderboardDownload(int sessionUserIndex, const leaderboardDefinition_t* leaderboard, int startingRank, int numRows, const idLeaderboardCallback& callback)
{
}

/*
========================
idSessionLocalWin::LeaderboardDownloadAttachment
========================
*/
void idSessionLocalWin::LeaderboardDownloadAttachment(int sessionUserIndex, const leaderboardDefinition_t* leaderboard, int64 attachmentID)
{
}

/*
========================
idSessionLocalWin::EnsurePort
========================
*/
void idSessionLocalWin::EnsurePort()
{
    // Init the port using reqular sockets
    if (port.IsOpen()) {
        return; // Already initialized
    }

    if (port.InitPort(net_port.GetInteger(), false)) {
        // TODO: what about canJoinLocalHost when running two instances with different net_port values?
        canJoinLocalHost = false;
    } else {
        // Assume this is another instantiation on the same machine, and just init using any available port
        port.InitPort(PORT_ANY, false);
        canJoinLocalHost = true;
    }
}

/*
========================
idSessionLocalWin::GetPort
========================
*/
idNetSessionPort& idSessionLocalWin::GetPort(bool dedicated)
{
    EnsurePort();
    return port;
}

/*
========================
idSessionLocalWin::CreateLobbyBackend
========================
*/
idLobbyBackend* idSessionLocalWin::CreateLobbyBackend(const idMatchParameters& p, float skillLevel, idLobbyBackend::lobbyBackendType_t lobbyType)
{
    idLobbyBackend* lobbyBackend = CreateLobbyInternal(lobbyType);
    lobbyBackend->StartHosting(p, skillLevel, lobbyType);
    return lobbyBackend;
}

/*
========================
idSessionLocalWin::FindLobbyBackend
========================
*/
idLobbyBackend* idSessionLocalWin::FindLobbyBackend(const idMatchParameters& p, int numPartyUsers, float skillLevel, idLobbyBackend::lobbyBackendType_t lobbyType)
{
    idLobbyBackend* lobbyBackend = CreateLobbyInternal(lobbyType);
    lobbyBackend->StartFinding(p, numPartyUsers, skillLevel);
    return lobbyBackend;
}

/*
========================
idSessionLocalWin::JoinFromConnectInfo
========================
*/
idLobbyBackend* idSessionLocalWin::JoinFromConnectInfo(const lobbyConnectInfo_t& connectInfo, idLobbyBackend::lobbyBackendType_t lobbyType)
{
    idLobbyBackend* lobbyBackend = CreateLobbyInternal(lobbyType);
    lobbyBackend->JoinFromConnectInfo(connectInfo);
    return lobbyBackend;
}

/*
========================
idSessionLocalWin::DestroyLobbyBackend
========================
*/
void idSessionLocalWin::DestroyLobbyBackend(idLobbyBackend* lobbyBackend)
{
    assert(lobbyBackend != NULL);
    assert(lobbyBackends[lobbyBackend->GetLobbyType()] == lobbyBackend);

    lobbyBackends[lobbyBackend->GetLobbyType()] = NULL;

    lobbyBackend->Shutdown();
    delete lobbyBackend;
}

/*
========================
idSessionLocalWin::PumpLobbies
========================
*/
void idSessionLocalWin::PumpLobbies()
{
    assert(lobbyBackends[idLobbyBackend::TYPE_PARTY] == NULL || lobbyBackends[idLobbyBackend::TYPE_PARTY]->GetLobbyType() == idLobbyBackend::TYPE_PARTY);
    assert(lobbyBackends[idLobbyBackend::TYPE_GAME] == NULL || lobbyBackends[idLobbyBackend::TYPE_GAME]->GetLobbyType() == idLobbyBackend::TYPE_GAME);
    assert(lobbyBackends[idLobbyBackend::TYPE_GAME_STATE] == NULL || lobbyBackends[idLobbyBackend::TYPE_GAME_STATE]->GetLobbyType() == idLobbyBackend::TYPE_GAME_STATE);

    // Pump lobbyBackends
    for (int i = 0; i < lobbyBackends.Num(); i++) {
        if (lobbyBackends[i] != NULL) {
            lobbyBackends[i]->Pump();
        }
    }
}

/*
========================
idSessionLocalWin::CreateLobbyInternal
========================
*/
idLobbyBackend* idSessionLocalWin::CreateLobbyInternal(idLobbyBackend::lobbyBackendType_t lobbyType)
{
    EnsurePort();
    idLobbyBackend* lobbyBackend = new (TAG_NETWORKING) idLobbyBackendDirect();

    lobbyBackend->SetLobbyType(lobbyType);

    assert(lobbyBackends[lobbyType] == NULL);
    lobbyBackends[lobbyType] = lobbyBackend;

    return lobbyBackend;
}

/*
========================
idSessionLocalWin::JoinAfterSwap
========================
*/
void idSessionLocalWin::JoinAfterSwap(void* joinID)
{
}

/*
========================
idSessionLocalWin::GetLobbyAddressFromNetAddress
========================
*/
bool idSessionLocalWin::GetLobbyAddressFromNetAddress(const netadr_t& netAddr, lobbyAddress_t& outAddr) const
{
    return false;
}

/*
========================
idSessionLocalWin::GetNetAddressFromLobbyAddress
========================
*/
bool idSessionLocalWin::GetNetAddressFromLobbyAddress(const lobbyAddress_t& lobbyAddress, netadr_t& outNetAddr) const
{
    return false;
}

/*
================================================================================

The full verification's fake peers (framework/VitaVerify.cpp)

What another console would send - a game query (with a friend request or an
invite in its tag), a game's answer - built with the real code and handed to
the handlers as if it had come in from the given address. Answers go out
through the real port to that address (a loopback one, nobody listens).

================================================================================
*/
void Session_VerifyFakeQuery(const char* id, const char* name, const char* hwIdHash, const netadr_t& from, bool friendRequest, bool invite)
{
    sessionLocalWin.GetPort();
    byte buffer[idPacketProcessor::MAX_PACKET_SIZE - 2];
    idBitMsg msg(buffer, sizeof(buffer));
    msg.WriteLong(NetGetVersionChecksum());
    netFriends.WriteTagAs(msg, id, name, hwIdHash, friendRequest, false, invite);
    idBitMsg in;
    in.InitRead(buffer, msg.GetSize());
    lobbyAddress_t a;
    a.InitFromNetadr(from);
    sessionLocalWin.HandleServerQueryRequest(a, in, idLobby::OOB_MATCH_QUERY);
}

void Session_VerifyFakeGame(const char* id, const char* name, const char* hwIdHash, const netadr_t& from, int numPlayers, int maxPlayers)
{
    byte buffer[idPacketProcessor::MAX_PACKET_SIZE - 2];
    idBitMsg msg(buffer, sizeof(buffer));
    msg.WriteBool(true);
    serverInfo_t info;
    info.joinable = true;
    info.serverName = name;
    info.gameMap = 0;
    info.gameMode = 0;
    info.numPlayers = numPlayers;
    info.maxPlayers = maxPlayers;
    info.Write(msg);
    for (int i = 0; i < numPlayers; i++) {
        msg.WriteString(va("%s_mate%d", name, i));
    }
    netFriends.WriteTagAs(msg, id, name, hwIdHash, false, false, false);
    msg.WriteLong(0); // no ban list
    idBitMsg in;
    in.InitRead(buffer, msg.GetSize());
    lobbyAddress_t a;
    a.InitFromNetadr(from);
    sessionLocalWin.HandleServerQueryAck(a, in);
}

static void Session_VerifyBrowserCallback()
{
}

void Session_VerifyBrowse(bool start)
{
    if (start) {
        session->ListServers(idCallbackStatic(Session_VerifyBrowserCallback));
    } else {
        session->CancelListServers();
    }
}

// the browser's entry for a host name: -1 none; friend / joinable
int Session_VerifyFindServer(const char* hostName, bool& isFriend, bool& joinable, int& players)
{
    for (int i = 0; i < session->NumServers(); i++) {
        const serverInfo_t* s = session->ServerInfo(i);
        if (s != NULL && idStr::FindText(s->serverName, hostName, false) == 0) {
            isFriend = sessionLocalWin.IsServerFriend(i);
            joinable = s->joinable;
            players = s->numPlayers;
            return i;
        }
    }
    return -1;
}

int Session_VerifyState()
{
    return (int)session->GetState();
}
