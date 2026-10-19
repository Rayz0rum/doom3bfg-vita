/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - the full verification (PS Vita port).

"verify" (L + R held at boot on the Vita) plays a long script through the
benchmark's machinery (VitaBench.cpp) whose steps check every part of the
game and give each check a verdict - PASS, FAIL, WARN or INFO - with what was
seen: the three campaigns map by map (loading, the warnings it raised, the
cutscene at its start, a fight with the map's own monsters, the weapons, the
ragdolls coming to rest on the floor, saving and loading), the videos, the
menus, every language (missing strings, broken text, glyphs the fonts lack,
English left behind, voice-overs), DOOM / DOOM II / No Rest for the Living,
the friends list and the network, multiplayer with bots, memory over the
whole session, and the benchmark's heavy scenes. Screenshots of what was
checked go to verify/ in the save folder.

Every check is appended to verify_checks.txt as it is made, and the step
being run to verify_progress.txt: a run that crashes or hangs is picked up at
the next step the next time it starts (L + R again), the step that died
counted as a FAIL. "verify fresh" starts over.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#include "Common_local.h"
#include "../sys/sys_net_friends.h"
#include "../sys/sys_net_online.h"
#include "../renderer/Font.h"
#include "../libs/jpeg-6/jpeglib.h"
#if defined(__vita__)
#include <psp2/ctrl.h> // the finish screen's SELECT
#endif

// VitaBench.cpp
void VitaBench_Log(VERIFY_FORMAT_STRING const char* fmt, ...);
void VitaBench_SetInput(int buttons, int forward, int right, float yawPerMs);
void VitaBench_SetImpulse(int impulse);
void VitaBench_SetLook(bool on, float yaw, float pitch);
void VitaBench_OpenSegment(const char* name);
void VitaBench_CloseSegment();
void VitaBench_SetMeasuring(bool on);
void VitaBench_SkipFrame();
void VitaBench_LastFrame(uint64* drawUs, uint64* backendUs);
void VitaBench_SetMapName(const char* name);
bool VitaBench_VerifyMode();
double VitaBench_Seconds();
void VitaBench_Memory(unsigned int& freeKB, unsigned int& cachedKB);
const char* VitaBench_Line(int i);

// d3xp/VitaVerify_game.cpp
struct vitaVerifyEnt_t {
    bool exists;
    bool isAI;
    bool hidden;
    bool active;
    bool hasEnemy;
    bool afActive;
    bool atRest;
    int health;
    idVec3 origin;
    float floorGap;
    float speed;
    float maxBodySpeed;
    float activeTime;
    int bodies;
    bool dormant;
};
bool Vita_VerifyInCinematic();
int Vita_VerifyGameTime();
bool Vita_VerifyPlayer(idVec3& origin, idAngles& angles, int& health, idStr& weapon);
int Vita_VerifyPlayerAmmo();
bool Vita_VerifyEntity(const char* name, vitaVerifyEnt_t& out, bool floorGap = false); // floorGap: a trace per body - only the corpse check asks (it cost ~5 ms a frame in the fights, inside the measured frames)
void Vita_VerifyMonsterClasses(idStrList& classes);
bool Vita_VerifySpawnNear(const char* cls, const char* name, float dist, idStr& why);
bool Vita_VerifyAimAt(const char* name, float& yaw, float& pitch);
bool Vita_VerifyGoOpen(float wantRoom, idStr& where);
void Vita_VerifyViewToCmd(float& yaw, float& pitch);
bool Vita_VerifyNoWeapons();
bool Vita_VerifyWeaponsUsable(idStr& why);
float Vita_VerifyPlayerSpeed();
void Vita_VerifyCountAI(int& alive, int& awake, int& ragdollsMoving);
bool Vita_VerifyShellActive();
int Vita_VerifyShellArea();
bool Vita_VerifyShellGoto(int area);
const char* Vita_VerifyShellAreaName(int area);
// Game_local.cpp (the benchmark's)
bool Vita_BenchGameReady(idStr& why);
void Vita_BenchPlayerSetup(bool god, bool noclip, bool notarget);
int Vita_BenchKill(const char* name);
bool Vita_BenchRemove(const char* name);
void Vita_BenchGameStats(idStr& out);
// renderer/Font.cpp
void Vita_RegisteredFontNames(idStrList& out);
// sys/common/session_local.cpp
void Session_VerifyFakeQuery(const char* id, const char* name, const char* hwIdHash, const netadr_t& from, bool friendRequest, bool invite);
void Session_VerifyFakeGame(const char* id, const char* name, const char* hwIdHash, const netadr_t& from, int numPlayers, int maxPlayers);
void Session_VerifyBrowse(bool start);
int Session_VerifyFindServer(const char* hostName, bool& isFriend, bool& joinable, int& players);
int Session_VerifyState();
int Session_SendInvites(const idStrList* onlyIds);
void Session_SendInviteTo(const netadr_t& addr);
void Session_JoinInvite(const netadr_t& addr);
extern int net_invitesSent;
extern int net_invitesReceived;
extern bool net_verifyLoopback;
extern int net_verifyLoopbackInvites;
// d3xp/VitaBots.cpp
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
int Vita_MPPlayers(vitaMPPlayer_t* out, int max);
int Vita_MPGameState();
bool Vita_MPAimAtNearest(float& yaw, float& pitch);
int VitaBot_Add(int count);
int VitaBot_RemoveAll();
void Vita_MPTimes(int& serverTime, int& nextSwitch, int& nextState);
// doomclassic/doom/vita_verify_classic.cpp
struct vitaClassicState_t {
    bool ready;
    int gamestate;
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
bool Vita_ClassicState(vitaClassicState_t& out);
void Vita_ClassicNewGame(int episode, int map);
void Vita_ClassicCheats();
void Vita_ClassicSetExpansion(int mission);
void I_MusicStats(int* songs, int* underruns, int* chunks, double* synthMsPerSecond); // i_sound_openal.cpp: the streamed music
#if defined(__vita__)
void Vita_ClassicZone(int* sizeMB, bool* cached); // z_zone.cpp
void Vita_ClassicZonePieces(int* pieces, int* cachedMB); // z_zone.cpp
#endif

// VitaVerify_game.cpp: an achievement through the game's own unlock path
// (idAchievementManager::LocalUser_CompleteAchievement): 0 sent, 1 the
// profile has it already, -1 no signed-in user
int Vita_VerifyTrophyChain(int id);

#if defined(__vita__)
// vita_trophies.cpp
bool Vita_TrophyStatus(int& count, int& earned, int& initError);
const char* Vita_TrophyWhy(); // vita_trophies.cpp: why the trophy system is not up
bool Vita_TrophyDryRunHit(int id);
const char* Vita_TrophyName(int id, char* grade);
#endif

#if defined(__vita__)
extern "C" int DISPLAY_STRIDE; // vitaGL (gxm.c)
// vita_avplayer.cpp
void VitaVideo_Watch(const char* pathEnd);
int VitaVideo_WatchedFrames(unsigned int* firstFrameMs);
void VitaVideo_Status(char* out, int outLen);
#endif

static const char* vitaVerifyScript =
#include "VitaVerify_script.inc"
    ;

const char* VitaVerify_BuiltInScript()
{
    return vitaVerifyScript;
}

/*
================================================================================

Verdicts

================================================================================
*/
enum { V_PASS, V_FAIL, V_WARN, V_INFO, V_NUM };
static const char* vResultNames[V_NUM] = { "PASS", "FAIL", "WARN", "INFO" };

struct verifyCheck_t {
    idStr suite;
    idStr name;
    idStr detail;
    int result;
};

static struct {
    idStr suite;
    idList<verifyCheck_t> checks;
    int counts[V_NUM];
    // warnings and errors the game printed (main thread)
    int warnings;
    int errors;
    idStrList recentWarnings; // the last 200
    idStr lastError;
    int warnMark;             // warnings when the current step began
    // memory at the menu and after each map load (the session's trend)
    idList<unsigned int> menuFreeKB, menuCachedKB;
    idStrList mapNames;
    idList<unsigned int> mapFreeKB, mapCachedKB;
    // the step that runs
    int op;
    int step;
    uint64 stepUs;
    uint64 opUs;
    idCmdArgs args;
    int lineNum;
    // screenshots
    int shotSerial;
    idStr shotName;
    volatile int shotState; // 0 none, 1 wanted, 2 captured (buffer full), 3 being written
    byte* shotRGB;
    int shotW, shotH;
} v;

static void V_AppendFile(const char* file, const char* text)
{
    idFile* f = fileSystem->OpenFileAppend(file, false, "fs_savepath");
    if (f != NULL) {
        f->Write(text, (int)strlen(text));
        delete f;
    }
}

static void V_Check(int result, const char* name, VERIFY_FORMAT_STRING const char* fmt, ...)
{
    char detail[2048];
    va_list argptr;
    va_start(argptr, fmt);
    idStr::vsnPrintf(detail, sizeof(detail) - 1, fmt, argptr);
    va_end(argptr);
    detail[sizeof(detail) - 1] = '\0';
    // one line per check: line breaks inside game strings ("CONNECTION
    // ERROR") split the language checks over several lines of verify_checks.txt
    for (char* p = detail; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            *p = ' ';
        }
    }
    verifyCheck_t c;
    c.suite = v.suite;
    c.name = name;
    c.detail = detail;
    c.result = result;
    v.checks.Append(c);
    v.counts[result]++;
    VitaBench_Log("bench CHECK %s [%s] %s: %s", vResultNames[result], v.suite.c_str(), name, detail);
    V_AppendFile("verify_checks.txt", va("%s\t%s\t%s\t%s\n", vResultNames[result], v.suite.c_str(), name, detail));
}

/*
================
The game's warnings and errors (Common_printf.cpp)
================
*/
void VitaVerify_NoteWarning(const char* msg)
{
    v.warnings++;
    if (v.recentWarnings.Num() >= 200) {
        v.recentWarnings.RemoveIndex(0);
    }
    v.recentWarnings.Append(msg);
}

void VitaVerify_NoteError(const char* msg)
{
    v.errors++;
    v.lastError = msg;
}

// the game data's own warnings: what a PC run of the same script printed too
// (tools/vita/make_known_warnings.py) - a missing GUI in a map, a joint a
// script asks for. Only the others can be the port's.
static const char* vKnownWarningsBlob =
#include "VitaVerify_known_warnings.inc"
    ;
static idStrList vKnownWarnings;

static bool V_KnownWarning(const char* w)
{
    if (vKnownWarnings.Num() == 0) {
        const char* p = vKnownWarningsBlob;
        while (*p) {
            const char* e = strchr(p, '\n');
            if (e == NULL) {
                break;
            }
            vKnownWarnings.Append(idStr(p, 0, (int)(e - p)));
            p = e + 1;
        }
    }
    // (a few hundred strings, a few warnings a map: a plain search)
    for (int i = 0; i < vKnownWarnings.Num(); i++) {
        if (vKnownWarnings[i].Cmp(w) == 0) {
            return true;
        }
    }
    return false;
}

// warnings that mean something is broken, not just untidy map data
static bool V_HardWarning(const char* w)
{
    if (V_KnownWarning(w)) {
        return false; // the data's own (the PC prints it too)
    }
    // stock data with no picture behind a name (the PC game says it too)
    if (strstr(w, "_emptyname") != NULL) {
        return false;
    }
    static const char* hard[] = { "out of memory", "Out of memory", "FAILED", "failed to", "overflow", "exceeded", "AllocStatic", "no .mp4",
        "couldn't load", "Couldn't load", "could not load", "Could not load", "couldn't find", "Couldn't find", "not found", "corrupt",
        "invalid mass", "GL error", "shader", "Shader", NULL };
    for (int i = 0; hard[i] != NULL; i++) {
        if (strstr(w, hard[i]) != NULL) {
            return true;
        }
    }
    return false;
}

// the warnings since mark: counted, the hard ones listed
static void V_WarningsSince(int mark, int& total, int& hard, idStr& hardList, idStr& allList)
{
    total = v.warnings - mark;
    hard = 0;
    const int have = Min(total, v.recentWarnings.Num());
    for (int i = v.recentWarnings.Num() - have; i < v.recentWarnings.Num(); i++) {
        const char* w = v.recentWarnings[i].c_str();
        if (allList.Length() < 1500) {
            allList += va("%s\"%s\"", allList.Length() ? "; " : "", w);
        }
        if (V_HardWarning(w)) {
            hard++;
            if (hardList.Length() < 1200) {
                hardList += va("%s\"%s\"", hardList.Length() ? "; " : "", w);
            }
        }
    }
}

static void V_Memory(unsigned int& freeKB, unsigned int& cachedKB)
{
    VitaBench_Memory(freeKB, cachedKB);
}

/*
================================================================================

Progress: picking up a run that died

================================================================================
*/
static const char* V_PROGRESS = "verify_progress.txt";

// steps that start from a known state (a map load, the menus...): where a
// resumed run starts
static bool V_IsCheckpointOp(const char* op)
{
    static const char* ops[] = { "suite", "mapcheck", "menus", "langcheck", "trophies", "videotest", "friends", "netpeer", "classic", "mp", "online", "memtrend", "map", NULL };
    for (int i = 0; ops[i] != NULL; i++) {
        if (!idStr::Icmp(op, ops[i])) {
            return true;
        }
    }
    return false;
}

void VitaVerify_LineStarted(int lineNum, const char* text)
{
    idCmdArgs a;
    a.TokenizeString(text, false);
    if (a.Argc() == 0 || !V_IsCheckpointOp(a.Argv(0))) {
        return;
    }
    const idStr p = va("at %d %s\n", lineNum, text);
    fileSystem->WriteFile(V_PROGRESS, p.c_str(), p.Length());
}

static int vResumeFromLine = 0; // 1-based line that died, 0 none
static idStr vResumeText;

void VitaVerify_Begin(bool resume)
{
    v.suite = "start";
    v.checks.Clear();
    memset(v.counts, 0, sizeof(v.counts));
    v.warnMark = v.warnings;
    v.menuFreeKB.Clear();
    v.menuCachedKB.Clear();
    v.mapNames.Clear();
    v.mapFreeKB.Clear();
    v.mapCachedKB.Clear();
    v.op = 0;
    v.shotState = 0;
    vResumeFromLine = 0;
    vResumeText.Clear();
    if (resume) {
        void* buffer = NULL;
        const int len = fileSystem->ReadFile(V_PROGRESS, &buffer);
        if (len > 0 && buffer != NULL) {
            idStr p = idStr((const char*)buffer).Left(len);
            fileSystem->FreeFile(buffer);
            if (!idStr::Icmpn(p.c_str(), "at ", 3)) {
                vResumeFromLine = atoi(p.c_str() + 3);
                const int sp = p.Find(' ', 3);
                vResumeText = (sp > 0) ? p.Mid(sp + 1, p.Length()) : "";
                vResumeText.StripTrailingWhitespace();
            }
        }
    }
    if (vResumeFromLine == 0) {
        // a fresh run: its own files
        fileSystem->WriteFile("verify_checks.txt", "", 0);
    }
    // test friends a run that died in the friends checks left behind (their
    // ids are the checks' own: 0000beef..., 0000feed...)
    {
        netFriends.Load();
        idStrList leftovers;
        for (int i = 0; i < netFriends.Num(); i++) {
            const idStr& id = netFriends.Get(i).id;
            if (id.Icmpn("0000beef", 8) == 0 || id.Icmpn("0000feed", 8) == 0) {
                leftovers.Append(id);
            }
        }
        for (int i = 0; i < leftovers.Num(); i++) {
            netFriends.Remove(leftovers[i]);
        }
        if (leftovers.Num() > 0) {
            VitaBench_Log("bench   %d test friends left by an earlier run removed", leftovers.Num());
        }
        for (int i = 0; i < 5; i++) {
            const idStr hw = va("feedface%08x", i); // the network checks' fake consoles
            if (netOnline.IsBanned(hw)) {
                netOnline.UnbanLocally(hw);
            }
        }
    }
    VitaBench_Log("bench ================================================================");
    VitaBench_Log("bench FULL VERIFICATION %s", vResumeFromLine ? "(resumed)" : "(fresh run)");
}

int VitaVerify_ResumeLine(int numLines)
{
    if (vResumeFromLine <= 0) {
        return 0;
    }
    v.suite = "resume";
    V_Check(V_FAIL, "previous run", "the previous run stopped during line %d (\"%s\") - a crash, a hang or a quit; carrying on after it", vResumeFromLine, vResumeText.c_str());
    // the next step that starts from a known state
    for (int i = vResumeFromLine; i < numLines; i++) { // (lines are 1-based: index vResumeFromLine is the next line)
        const char* l = VitaBench_Line(i);
        idCmdArgs a;
        a.TokenizeString(l, false);
        if (a.Argc() > 0 && V_IsCheckpointOp(a.Argv(0))) {
            VitaBench_Log("bench resuming at line %d: %s", i + 1, l);
            return i;
        }
    }
    return numLines;
}

/*
================================================================================

Screenshots

The Vita: vitaGL's display callback hands over each finished frame as it is
flipped (VitaBench.cpp: Bench_DisplayFlip -> VitaVerify_DisplayFrame), the
only time the frame exists in memory (GXM draws a whole scene at once). The
PC: read back before the swap. Either way the main thread writes it out as a
JPEG to verify/ in the save folder.

================================================================================
*/
static void V_ShotAlloc(int w, int h)
{
    if (v.shotRGB == NULL || v.shotW != w || v.shotH != h) {
        Mem_Free(v.shotRGB);
        v.shotRGB = (byte*)Mem_Alloc(w * h * 4, TAG_TEMP); // RGBA: id's libjpeg takes 4-byte pixels (RGB_PIXELSIZE 4)
        v.shotW = w;
        v.shotH = h;
    }
}

static void V_RequestShot(const char* label)
{
    idStr name = va("%03d_%s_%s", ++v.shotSerial, v.suite.c_str(), label);
    for (int i = 0; i < name.Length(); i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            name[i] = '_';
        }
    }
    v.shotName = name;
#if defined(__vita__)
    V_ShotAlloc(960, 544);
#endif
    __sync_synchronize();
    v.shotState = 1;
}

#if defined(__vita__)
// on vitaGL's display queue thread, as the frame goes on screen
void VitaVerify_DisplayFrame(void* framebuf)
{
    if (v.shotState != 1 || v.shotRGB == NULL || framebuf == NULL) {
        return;
    }
    // A8B8G8R8 is R, G, B, A in memory: the rows as they are
    const uint32* src = (const uint32*)framebuf;
    for (int y = 0; y < 544; y++) {
        memcpy(v.shotRGB + y * 960 * 4, src + y * DISPLAY_STRIDE, 960 * 4);
    }
    __sync_synchronize();
    v.shotState = 2;
}
#endif

// the PC's screenshots: before the swap, on the GL thread (GL_BlockingSwapBuffers)
void VitaVerify_BeforeSwap()
{
#if !defined(__vita__)
    if (v.shotState == 1) {
        const int w = renderSystem->GetWidth();
        const int h = renderSystem->GetHeight();
        V_ShotAlloc(w, h);
        byte* tmp = (byte*)Mem_Alloc(w * h * 4, TAG_TEMP);
        qglPixelStorei(GL_PACK_ALIGNMENT, 1);
        qglReadBuffer(GL_BACK);
        qglReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        for (int y = 0; y < h; y++) {
            memcpy(v.shotRGB + y * w * 4, tmp + (h - 1 - y) * w * 4, w * 4);
        }
        Mem_Free(tmp);
        v.shotState = 2;
    }
#endif
}

// libjpeg into memory
struct vJpegDest_t {
    jpeg_destination_mgr pub;
    idList<byte>* out;
    byte buf[16384];
};
static void V_JpegInit(j_compress_ptr cinfo)
{
    vJpegDest_t* d = (vJpegDest_t*)cinfo->dest;
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = sizeof(d->buf);
}
static boolean V_JpegEmpty(j_compress_ptr cinfo)
{
    vJpegDest_t* d = (vJpegDest_t*)cinfo->dest;
    const int n = d->out->Num();
    d->out->SetNum(n + (int)sizeof(d->buf));
    memcpy(d->out->Ptr() + n, d->buf, sizeof(d->buf));
    d->pub.next_output_byte = d->buf;
    d->pub.free_in_buffer = sizeof(d->buf);
    return TRUE;
}
static void V_JpegTerm(j_compress_ptr cinfo)
{
    vJpegDest_t* d = (vJpegDest_t*)cinfo->dest;
    const int used = (int)(sizeof(d->buf) - d->pub.free_in_buffer);
    const int n = d->out->Num();
    d->out->SetNum(n + used);
    memcpy(d->out->Ptr() + n, d->buf, used);
}

// on the main thread: a captured frame out to the card
static void V_WriteShotIfReady()
{
    if (v.shotState != 2) {
        return;
    }
    v.shotState = 3;
    const uint64 t0 = Sys_Microseconds();
    idList<byte> out;
    out.SetGranularity(64 * 1024);
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    vJpegDest_t* dest = (vJpegDest_t*)Mem_Alloc(sizeof(vJpegDest_t), TAG_TEMP);
    dest->pub.init_destination = V_JpegInit;
    dest->pub.empty_output_buffer = V_JpegEmpty;
    dest->pub.term_destination = V_JpegTerm;
    dest->out = &out;
    cinfo.dest = &dest->pub;
    cinfo.image_width = v.shotW;
    cinfo.image_height = v.shotH;
    cinfo.input_components = 4; // (RGB_PIXELSIZE 4: the fourth byte is skipped)
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = v.shotRGB + cinfo.next_scanline * v.shotW * 4;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    Mem_Free(dest);
    const idStr path = va("verify/%s.jpg", v.shotName.c_str());
    const int written = fileSystem->WriteFile(path, out.Ptr(), out.Num());
    VitaBench_Log("bench   screenshot %s: %d KB, %.0f ms to encode and write%s", path.c_str(), out.Num() / 1024, (Sys_Microseconds() - t0) / 1000.0, written > 0 ? "" : " - WRITE FAILED");
    v.shotState = 0;
    // the encode is the test's, not the game's: that frame is left out of the
    // frame-rate figures (it put a 250-370 ms "worst frame" in every map)
    VitaBench_SkipFrame();
}

// polled while a step waits for its picture
static bool V_ShotDone()
{
    V_WriteShotIfReady();
    return v.shotState == 0;
}

/*
================================================================================

The steps

================================================================================
*/
enum verifyOp_t {
    VOP_NONE,
    VOP_MAPCHECK,
    VOP_SHOTS,
    VOP_FIGHT,
    VOP_SAVE,
    VOP_MENUS,
    VOP_VIDEO,
    VOP_FRIENDS,
    VOP_SHOT,
    VOP_WAITMENU,
    VOP_CLASSIC,
    VOP_NETPEER,
    VOP_MP,
    VOP_ONLINE,
    VOP_SHADOW,
    VOP_POSTFILTER,
};

static uint64 V_Now()
{
    return Sys_Microseconds();
}

static double V_Since(uint64 t)
{
    return (V_Now() - t) / 1000000.0;
}

static void V_NextStep()
{
    v.step++;
    v.stepUs = V_Now();
}

/*
---------------- mapcheck <map> [intro] [timeout s] ----------------
*/
static struct {
    idStr map;
    bool intro;
    int timeoutS;
    uint64 requestUs;
    uint64 spawnedUs;
    bool sawCinematic;
    uint64 cinStartUs;
    int cinFrames;
    int warnAtRequest;
    int errorsAtRequest;
} vm;

static void V_MapCheckStart()
{
    vm.map = v.args.Argv(1);
    vm.intro = v.args.Argc() > 2 && !idStr::Icmp(v.args.Argv(2), "intro");
    vm.timeoutS = (v.args.Argc() > 3) ? atoi(v.args.Argv(3)) : 300;
    vm.requestUs = V_Now();
    vm.spawnedUs = 0;
    vm.sawCinematic = false;
    vm.cinFrames = 0;
    vm.warnAtRequest = v.warnings;
    vm.errorsAtRequest = v.errors;
    VitaBench_CloseSegment();
    VitaBench_SetInput(0, 0, 0, 0.0f);
    VitaBench_SetLook(false, 0.0f, 0.0f);
    VitaBench_SetMapName(vm.map.c_str());
    VitaBench_Log("bench ==== verify map %s: %s ====", vm.map.c_str(), vm.intro ? "a new game's start (map), its cutscene played through" : "devmap");
    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, va("%s %s\n", vm.intro ? "map" : "devmap", vm.map.c_str()));
}

static bool V_MapCheckPoll()
{
    const char* spawned = commonLocal.GetSpawnedMapName();
    const bool isSpawned = spawned != NULL && vm.map.Icmp(spawned) == 0;
    if (v.errors != vm.errorsAtRequest) {
        V_Check(V_FAIL, va("%s load", vm.map.c_str()), "the game stopped with an error: \"%s\"", v.lastError.c_str());
        return true;
    }
    if (!isSpawned) {
        if (V_Since(vm.requestUs) > vm.timeoutS) {
            V_Check(V_FAIL, va("%s load", vm.map.c_str()), "not loaded after %d s", vm.timeoutS);
            return true;
        }
        return false;
    }
    if (vm.spawnedUs == 0) {
        vm.spawnedUs = V_Now();
    }
    if (Vita_VerifyInCinematic()) {
        if (!vm.sawCinematic) {
            vm.sawCinematic = true;
            vm.cinStartUs = V_Now();
            VitaBench_Log("bench   %s: cutscene running (%.1f s after the load request)", vm.map.c_str(), V_Since(vm.requestUs));
            VitaBench_OpenSegment(va("%s: opening cutscene", vm.map.c_str()));
            VitaBench_SetMeasuring(true);
        }
        vm.cinFrames++;
        if (V_Since(vm.cinStartUs) > vm.timeoutS) {
            V_Check(V_FAIL, va("%s cutscene", vm.map.c_str()), "still running after %d s (%d frames) - stuck", vm.timeoutS, vm.cinFrames);
            VitaBench_CloseSegment();
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
            return true;
        }
        return false;
    }
    idStr why;
    if (!Vita_BenchGameReady(why)) {
        if (V_Since(vm.spawnedUs) > 60.0) {
            V_Check(V_FAIL, va("%s load", vm.map.c_str()), "loaded but not playable after 60 s: %s", why.c_str());
            return true;
        }
        return false;
    }
    // playable
    VitaBench_SetMeasuring(false);
    if (vm.sawCinematic) {
        const double secs = V_Since(vm.cinStartUs);
        VitaBench_CloseSegment();
        V_Check(secs > 1.0 && vm.cinFrames >= 10 ? V_PASS : V_FAIL, va("%s cutscene", vm.map.c_str()), "played through in %.1f s, %d frames (%.1f fps)", secs, vm.cinFrames, vm.cinFrames / Max(0.1, secs));
    } else if (vm.intro) {
        V_Check(V_INFO, va("%s cutscene", vm.map.c_str()), "no cutscene at this map's start");
    }
    unsigned int freeKB = 0, cachedKB = 0;
    V_Memory(freeKB, cachedKB);
    v.mapNames.Append(vm.map);
    v.mapFreeKB.Append(freeKB);
    v.mapCachedKB.Append(cachedKB);
    int total = 0, hard = 0;
    idStr hardList, allList;
    V_WarningsSince(vm.warnAtRequest, total, hard, hardList, allList);
    const double loadS = (vm.spawnedUs - vm.requestUs) / 1000000.0;
    V_Check(loadS < 180.0 ? V_PASS : V_WARN, va("%s load", vm.map.c_str()), "playable; %.1f s to load, free memory %u MB (cached heap %u MB)", loadS, freeKB / 1024, cachedKB / 1024);
    V_Check(hard ? V_FAIL : (total ? V_WARN : V_PASS), va("%s warnings", vm.map.c_str()), "%d warnings while loading, %d of them serious%s%s%s%s", total, hard,
        hard ? " - serious: " : "", hard ? hardList.c_str() : "", total ? " | all: " : "", total ? allList.c_str() : "");
    Vita_BenchPlayerSetup(true, false, false);
    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "give all\n");
    return true;
}

/*
---------------- shots <count> [label] ----------------
Turns the view in place and captures each direction (measured: "look around").
*/
static struct {
    int count;
    int i;
    float yaw0;
    idStr label;
} vs;

static void V_ShotsStart()
{
    vs.count = Max(1, atoi(v.args.Argv(1)));
    vs.label = (v.args.Argc() > 2) ? v.args.Argv(2) : "view";
    vs.i = 0;
    idVec3 o;
    idAngles a;
    int hp;
    idStr w;
    vs.yaw0 = Vita_VerifyPlayer(o, a, hp, w) ? a.yaw : 0.0f;
    VitaBench_OpenSegment(va("%s: looking around (%d directions)", commonLocal.GetCurrentMapName(), vs.count));
    VitaBench_SetMeasuring(true);
}

static bool V_ShotsPoll()
{
    switch (v.step) {
        case 0: { // face the next direction
            float yaw = vs.yaw0 + vs.i * 360.0f / vs.count, pitch = 0.0f;
            Vita_VerifyViewToCmd(yaw, pitch);
            VitaBench_SetLook(true, yaw, pitch);
            V_NextStep();
            return false;
        }
        case 1: // settle, then the picture
            if (V_Since(v.stepUs) < 0.8) {
                return false;
            }
            V_RequestShot(va("%s_%s_%d", commonLocal.GetCurrentMapName(), vs.label.c_str(), vs.i));
            V_NextStep();
            return false;
        case 2:
            if (!V_ShotDone()) {
                if (V_Since(v.stepUs) > 5.0) {
                    V_Check(V_FAIL, "screenshot", "%s: no frame captured in 5 s", v.shotName.c_str());
                    v.shotState = 0;
                } else {
                    return false;
                }
            }
            if (++vs.i < vs.count) {
                v.step = 0;
                return false;
            }
            break;
    }
    VitaBench_SetLook(false, 0.0f, 0.0f);
    VitaBench_SetMeasuring(false);
    VitaBench_CloseSegment();
    V_Check(V_PASS, va("%s views", commonLocal.GetCurrentMapName()), "%d directions rendered and captured", vs.count);
    return true;
}

static double V_PictureMean(const byte* rgba, int count);

/*
---------------- shadowcheck ----------------
Carmack's reverse on the Vita. The shadow jobs draw a volume with Z-pass
unless the view is inside it, then with Z-fail and its caps (one draw on the
Vita, r_useStencilShadowPreload 0). The same view is captured twice with
game time stopped: as the jobs choose, and with every volume drawn Z-fail
with caps (r_forceZFailStencilShadows). Both give the same stencil counts,
so the same picture - unless a cap is missing or the far end at infinity is
clipped. Then the precise inside test that picks between them (CPU job time)
is timed on and off: the front end and back end averaged over 45 frames each.
*/
static struct {
    byte* ref;
    int refW, refH;
    int frames;
    double drawMs[2], backMs[2];
    int n[2];
    bool stopTimeWas;
    bool preciseWas;
    bool hudWas, gunWas, overlaysHidden;
    byte* refZ; // the Z-fail picture
    int tries;
    int particlesWas;
} vsh;

// the two pictures show the world alone: the weapon's idle sway and the HUD's
// location text kept moving with game time stopped (the PC reference run's
// alphalabs2/3: 1.1-1.4% of the pixels, all of them hands and HUD text)
static void V_ShadowCheckHideOverlays(bool hide)
{
    if (hide) {
        vsh.hudWas = cvarSystem->GetCVarBool("g_showHud");
        vsh.gunWas = cvarSystem->GetCVarBool("ui_showGun");
        vsh.particlesWas = cvarSystem->GetCVarInteger("r_skipParticles");
        cvarSystem->SetCVarBool("g_showHud", false);
        cvarSystem->SetCVarBool("ui_showGun", false);
        cvarSystem->SetCVarInteger("r_skipParticles", 1);
    } else {
        cvarSystem->SetCVarBool("g_showHud", vsh.hudWas);
        cvarSystem->SetCVarBool("ui_showGun", vsh.gunWas);
        cvarSystem->SetCVarInteger("r_skipParticles", vsh.particlesWas);
    }
}

static void V_ShadowCheckStart()
{
    vsh.frames = 0;
    vsh.tries = 0;
    vsh.n[0] = vsh.n[1] = 0;
    vsh.drawMs[0] = vsh.drawMs[1] = vsh.backMs[0] = vsh.backMs[1] = 0.0;
    vsh.stopTimeWas = cvarSystem->GetCVarBool("g_stopTime");
    vsh.preciseWas = cvarSystem->GetCVarBool("r_useShadowPreciseInsideTest");
    cvarSystem->SetCVarBool("g_stopTime", true); // nothing moves between the two pictures
    cvarSystem->SetCVarBool("r_forceZFailStencilShadows", false);
    V_ShadowCheckHideOverlays(true);
    vsh.overlaysHidden = true;
}

static void V_ShadowCheckRestore()
{
    if (vsh.overlaysHidden) {
        V_ShadowCheckHideOverlays(false);
        vsh.overlaysHidden = false;
    }
    cvarSystem->SetCVarBool("r_forceZFailStencilShadows", false);
    cvarSystem->SetCVarBool("r_useShadowPreciseInsideTest", vsh.preciseWas);
    cvarSystem->SetCVarBool("g_stopTime", vsh.stopTimeWas);
}

static bool V_ShadowCheckPoll()
{
    const char* map = commonLocal.GetCurrentMapName();
    vsh.frames++;
    switch (v.step) {
        // Three pictures, time stopped: the jobs' own Z-pass/Z-fail mix (A),
        // every volume Z-fail with caps (Z), the mix again (B). Lights that
        // keep animating with game time stopped (sound-driven, real-time
        // flicker) changed a whole scene between two pictures once (PC,
        // alphalabs2: 11.9%): A against B is the scene's own change, and when
        // it is over 1% the three are taken again (three tries).
        case 0:
        case 2:
        case 4: // settle, then the picture
            if (v.step == 2) {
                cvarSystem->SetCVarBool("r_forceZFailStencilShadows", true);
            } else {
                cvarSystem->SetCVarBool("r_forceZFailStencilShadows", false);
            }
            if (vsh.frames < 8) {
                return false;
            }
            V_RequestShot(va("%s_shadow_%s", map, v.step == 0 ? "mixed" : (v.step == 2 ? "zfail" : "mixed_again")));
            V_NextStep();
            return false;
        case 1:
        case 3:
        case 5: {
            if (!V_ShotDone()) {
                if (V_Since(v.stepUs) > 5.0) {
                    V_Check(V_FAIL, va("%s z-fail shadows", map), "no frame captured in 5 s");
                    v.shotState = 0;
                    V_ShadowCheckRestore();
                    return true;
                }
                return false;
            }
            if (v.step != 5) {
                byte*& keep = (v.step == 1) ? vsh.ref : vsh.refZ;
                Mem_Free(keep);
                keep = (byte*)Mem_Alloc(v.shotW * v.shotH * 4, TAG_TEMP);
                if (keep != NULL) {
                    memcpy(keep, v.shotRGB, v.shotW * v.shotH * 4);
                }
                vsh.refW = v.shotW;
                vsh.refH = v.shotH;
                vsh.frames = 0;
                V_NextStep();
                return false;
            }
            const int total = vsh.refW * vsh.refH;
            const bool same = vsh.ref != NULL && vsh.refZ != NULL && v.shotW == vsh.refW && v.shotH == vsh.refH && total > 0;
            auto pctDiff = [&](const byte* a, const byte* b) {
                int differ = 0;
                for (int i = 0; i < total; i++) {
                    const byte* pa = a + i * 4;
                    const byte* pb = b + i * 4;
                    if (abs(pa[0] - pb[0]) > 24 || abs(pa[1] - pb[1]) > 24 || abs(pa[2] - pb[2]) > 24) {
                        differ++;
                    }
                }
                return 100.0f * differ / total;
            };
            const float noise = same ? pctDiff(vsh.ref, v.shotRGB) : 100.0f;
            const float zA = same ? pctDiff(vsh.refZ, vsh.ref) : 100.0f;
            const float zB = same ? pctDiff(vsh.refZ, v.shotRGB) : 100.0f;
            const float pct = Min(zA, zB);
            // a light that pulses (admin, r54: brightness 38.5 / 21.1 / 44.3 over
            // the three) changes every pixel a little - under the 24 above - and
            // the Z-fail picture a lot: the two mixed pictures' brightness must
            // agree too
            const double meanA = same ? V_PictureMean(vsh.ref, total) : 0.0;
            const double meanZ = same ? V_PictureMean(vsh.refZ, total) : 0.0;
            const double meanB = same ? V_PictureMean(v.shotRGB, total) : 0.0;
            const bool unsteady = noise > 1.0f || fabs(meanA - meanB) > 0.02 * Max(meanA, meanB) + 0.5;
            vsh.tries++;
            if (unsteady && vsh.tries < 3) {
                VitaBench_Log("bench   shadow check: the scene itself changed between the two mixed pictures (%.2f%% of the pixels, brightness %.1f -> %.1f) - taking the three again (try %d)", noise, meanA, meanB, vsh.tries + 1);
                vsh.frames = 0;
                v.step = 0;
                v.stepUs = V_Now();
                return false;
            }
            const int res = unsteady ? V_INFO : (pct < 1.0f ? V_PASS : (pct < 4.0f ? V_WARN : V_FAIL));
            V_Check(res, va("%s z-fail shadows", map),
                "Carmack's reverse everywhere vs the jobs' Z-pass/Z-fail mix, time stopped: %.2f%% of the pixels differ (the scene itself changed %.2f%% between the two mixed pictures; brightness mixed %.1f / Z-fail %.1f / mixed again %.1f; %d tries; shots %s_shadow_mixed / _zfail / _mixed_again)%s",
                pct, noise, meanA, meanZ, meanB, vsh.tries, map, unsteady ? " - the scene never held still (a pulsing or flickering light): no verdict" : (pct < 1.0f ? "" : " - a missing cap or a clipped far end would show here"));
            Mem_Free(vsh.ref);
            vsh.ref = NULL;
            Mem_Free(vsh.refZ);
            vsh.refZ = NULL;
            // the precise inside test, timed on and off (time running again,
            // the HUD and the weapon back: normal frames)
            cvarSystem->SetCVarBool("r_forceZFailStencilShadows", false);
            cvarSystem->SetCVarBool("g_stopTime", vsh.stopTimeWas);
            if (vsh.overlaysHidden) {
                V_ShadowCheckHideOverlays(false);
                vsh.overlaysHidden = false;
            }
            cvarSystem->SetCVarBool("r_useShadowPreciseInsideTest", true);
            vsh.frames = 0;
            V_NextStep();
            return false;
        }
        case 6:
        case 7: {
            const int k = v.step - 6;
            if (vsh.frames > 5) { // a few frames for the setting to reach the jobs
                uint64 drawUs, backUs;
                VitaBench_LastFrame(&drawUs, &backUs);
                vsh.drawMs[k] += drawUs / 1000.0;
                vsh.backMs[k] += backUs / 1000.0;
                vsh.n[k]++;
            }
            if (vsh.n[k] < 45) {
                return false;
            }
            if (k == 0) {
                cvarSystem->SetCVarBool("r_useShadowPreciseInsideTest", false);
                vsh.frames = 0;
                V_NextStep();
                return false;
            }
            const double d0 = vsh.drawMs[0] / vsh.n[0], b0 = vsh.backMs[0] / vsh.n[0];
            const double d1 = vsh.drawMs[1] / vsh.n[1], b1 = vsh.backMs[1] / vsh.n[1];
            V_Check(V_INFO, va("%s shadow inside test", map),
                "precise test on: front end %.2f ms + back end %.2f ms a frame; off (more volumes Z-fail with caps): %.2f + %.2f ms - %s %.2f ms",
                d0, b0, d1, b1, (d1 + b1) < (d0 + b0) ? "off saves" : "on saves", fabs((d1 + b1) - (d0 + b0)));
            V_ShadowCheckRestore();
            return true;
        }
    }
    V_ShadowCheckRestore();
    return true;
}

/*
---------------- postcheck ----------------
The 3D view's stretch filter (vita_postFilter): sharpening and soft edges for
alpha-tested surfaces. Three pictures with game time stopped and the HUD and
weapon hidden: the filter as set (A), the softened pixels painted red
(vita_softAlphaEdgesShow - the soft edges' map at work), and the filter off
(sharpening and soft edges 0). A against off: the filter must stay a light
touch (FAIL past 5% of the pixels changed by more than 24, or a picture much
darker or brighter - a broken map or stretch); the red count proves the map
reached the stretch (WARN when alpha-tested surfaces were drawn into it and
nothing turned red: the copy lost the alpha). Then the costs, 45 frames each
with the view still: the filter as set, the filter without the soft edges'
map, and the stock stretch (vita_postFilterPlain) - the frame, the back end,
its stretch pass and the wait for the GPU averaged (an INFO line): the map's
cost is the first against the second, the sharpening pass's the second
against the third.
*/
uint64 VitaBench_LastFrameGpuWait();
extern int r_vitaAlphaEdgeSurfaces;
extern bool r_vitaPostFilterBuilt;
static struct {
    byte* ref;
    int refW, refH;
    int frames;
    int surfaces; // alpha-tested surfaces in the map while A was taken
    bool stopTimeWas, hudWas, gunWas;
    int particlesWas;
    float sharpenWas, softWas;
    bool showWas;
    double redPct;
    double backMs[3], gpuMs[3], frameMs[3];
    int n[3];
    bool plainWas;
    int tries;
    uint64 lastUs;
} vpf;

static void V_PostCheckRestore()
{
    cvarSystem->SetCVarBool("vita_softAlphaEdgesShow", vpf.showWas);
    cvarSystem->SetCVarFloat("vita_sharpen", vpf.sharpenWas);
    cvarSystem->SetCVarFloat("vita_softAlphaEdges", vpf.softWas);
    cvarSystem->SetCVarBool("g_showHud", vpf.hudWas);
    cvarSystem->SetCVarBool("ui_showGun", vpf.gunWas);
    cvarSystem->SetCVarInteger("r_skipParticles", vpf.particlesWas);
    cvarSystem->SetCVarBool("g_stopTime", vpf.stopTimeWas);
    cvarSystem->SetCVarBool("vita_postFilterPlain", vpf.plainWas);
    Mem_Free(vpf.ref);
    vpf.ref = NULL;
}

static void V_PostCheckStart()
{
    vpf.frames = 0;
    vpf.surfaces = 0;
    vpf.redPct = 0.0;
    vpf.ref = NULL;
    vpf.tries = 0;
    for (int k = 0; k < 3; k++) {
        vpf.n[k] = 0;
        vpf.backMs[k] = vpf.gpuMs[k] = vpf.frameMs[k] = 0.0;
    }
    vpf.plainWas = cvarSystem->GetCVarBool("vita_postFilterPlain");
    cvarSystem->SetCVarBool("vita_postFilterPlain", false);
    vpf.stopTimeWas = cvarSystem->GetCVarBool("g_stopTime");
    vpf.hudWas = cvarSystem->GetCVarBool("g_showHud");
    vpf.gunWas = cvarSystem->GetCVarBool("ui_showGun");
    vpf.particlesWas = cvarSystem->GetCVarInteger("r_skipParticles");
    vpf.sharpenWas = cvarSystem->GetCVarFloat("vita_sharpen");
    vpf.softWas = cvarSystem->GetCVarFloat("vita_softAlphaEdges");
    vpf.showWas = cvarSystem->GetCVarBool("vita_softAlphaEdgesShow");
    cvarSystem->SetCVarBool("g_stopTime", true);
    cvarSystem->SetCVarBool("g_showHud", false);
    cvarSystem->SetCVarBool("ui_showGun", false);
    cvarSystem->SetCVarInteger("r_skipParticles", 1);
}

// mean of R, G and B over the picture
static double V_PictureMean(const byte* rgba, int count)
{
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        sum += rgba[i * 4 + 0] + rgba[i * 4 + 1] + rgba[i * 4 + 2];
    }
    return count > 0 ? sum / (3.0 * count) : 0.0;
}

static bool V_PostCheckPoll()
{
    const char* map = commonLocal.GetCurrentMapName();
    vpf.frames++;
    if (!r_vitaPostFilterBuilt) {
        V_Check(V_INFO, va("%s stretch filter", map), "not built (vita_postFilter 0, or its shader failed - see the log): nothing to check");
        V_PostCheckRestore();
        return true;
    }
    switch (v.step) {
        case 0:
        case 2:
        case 4: // settle, then the picture
            if (vpf.frames < 8) {
                return false;
            }
            if (v.step == 0) {
                vpf.surfaces = r_vitaAlphaEdgeSurfaces;
            }
            V_RequestShot(va("%s_filter_%s", map, v.step == 0 ? "on" : (v.step == 2 ? "edges_red" : "off")));
            V_NextStep();
            return false;
        case 1:
        case 3:
        case 5: {
            if (!V_ShotDone()) {
                if (V_Since(v.stepUs) > 5.0) {
                    V_Check(V_FAIL, va("%s stretch filter", map), "no frame captured in 5 s");
                    v.shotState = 0;
                    V_PostCheckRestore();
                    return true;
                }
                return false;
            }
            const int total = v.shotW * v.shotH;
            if (v.step == 1) {
                Mem_Free(vpf.ref);
                vpf.ref = (byte*)Mem_Alloc(total * 4, TAG_TEMP);
                if (vpf.ref != NULL) {
                    memcpy(vpf.ref, v.shotRGB, total * 4);
                }
                vpf.refW = v.shotW;
                vpf.refH = v.shotH;
                cvarSystem->SetCVarBool("vita_softAlphaEdgesShow", true);
                vpf.frames = 0;
                V_NextStep();
                return false;
            }
            const bool same = vpf.ref != NULL && v.shotW == vpf.refW && v.shotH == vpf.refH && total > 0;
            if (v.step == 3) {
                // pixels the red paint took: strongly red here, not in A
                int red = 0;
                if (same) {
                    for (int i = 0; i < total; i++) {
                        const byte* p = v.shotRGB + i * 4;
                        const byte* q = vpf.ref + i * 4;
                        if (p[0] > 180 && p[1] < 70 && p[2] < 70 && !(q[0] > 180 && q[1] < 70 && q[2] < 70)) {
                            red++;
                        }
                    }
                }
                vpf.redPct = same ? 100.0 * red / total : 0.0;
                cvarSystem->SetCVarBool("vita_softAlphaEdgesShow", false);
                cvarSystem->SetCVarFloat("vita_sharpen", 0.0f);
                cvarSystem->SetCVarFloat("vita_softAlphaEdges", 0.0f);
                vpf.frames = 0;
                V_NextStep();
                return false;
            }
            int differ = 0;
            double absSum = 0.0;
            if (same) {
                for (int i = 0; i < total; i++) {
                    const byte* a = vpf.ref + i * 4;
                    const byte* b = v.shotRGB + i * 4;
                    const int d = Max(abs(a[0] - b[0]), Max(abs(a[1] - b[1]), abs(a[2] - b[2])));
                    absSum += abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]);
                    if (d > 24) {
                        differ++;
                    }
                }
            }
            const double pct = same ? 100.0 * differ / total : 100.0;
            const double meanAbs = same ? absSum / (3.0 * total) : 0.0;
            const double meanOn = same ? V_PictureMean(vpf.ref, total) : 0.0;
            const double meanOff = V_PictureMean(v.shotRGB, total);
            const bool brightnessOff = meanOff > 4.0 && (meanOn < meanOff * 0.8 || meanOn > meanOff * 1.25);
            // the filter keeps the picture's brightness (sharpening adds and
            // takes evenly); a big change is a light changing between the
            // pictures (admin, r54: 6.4 then black) - or a broken stretch, which
            // stays when they are taken again
            const bool lightChanged = same && fabs(meanOn - meanOff) > 0.1 * Max(meanOn, meanOff) + 1.0;
            vpf.tries++;
            if (lightChanged && vpf.tries < 3) {
                VitaBench_Log("bench   stretch filter check: the picture's brightness changed %.1f -> %.1f - a light? taking the three again (try %d)", meanOn, meanOff, vpf.tries + 1);
                cvarSystem->SetCVarFloat("vita_sharpen", vpf.sharpenWas);
                cvarSystem->SetCVarFloat("vita_softAlphaEdges", vpf.softWas);
                vpf.frames = 0;
                v.step = 0;
                v.stepUs = V_Now();
                return false;
            }
            // a map softened somewhere this run proves the copy keeps the alpha:
            // after that, surfaces drawn into the map with nothing softened are
            // hidden behind others or too small (r54: comm1, 4 surfaces, 0.00%)
            static bool softenedThisRun = false;
            if (vpf.redPct > 0.0) {
                softenedThisRun = true;
            }
            int res = V_PASS;
            const char* why = "";
            if (!same) {
                res = V_FAIL;
                why = " - the pictures could not be compared";
            } else if (lightChanged) {
                res = V_WARN;
                why = " - the brightness changed in all three tries: a light that never held still, or a broken stretch (see the shots)";
            } else if (pct > 5.0 || brightnessOff) {
                res = V_FAIL;
                why = " - more than a light touch: a broken map or stretch would show like this";
            } else if (vpf.surfaces > 0 && vpf.redPct <= 0.0) {
                res = softenedThisRun ? V_INFO : V_WARN;
                why = softenedThisRun ? " - nothing softened here: the surfaces are hidden or too small (other maps this run were softened)"
                                      : " - alpha-tested surfaces were drawn into the map but nothing was softened: the copy may have lost the alpha";
            }
            V_Check(res, va("%s stretch filter", map),
                "filter as set vs off: %.2f%% of the pixels differ by more than 24, mean change %.2f/255, brightness %.1f vs %.1f; soft edges: %d alpha-tested surfaces in the map, %.2f%% of the pixels softened (painted red); shots %s_filter_on / _edges_red / _off%s",
                pct, meanAbs, meanOn, meanOff, vpf.surfaces, vpf.redPct, map, why);
            // the cost: the filter as set first
            cvarSystem->SetCVarFloat("vita_sharpen", vpf.sharpenWas);
            cvarSystem->SetCVarFloat("vita_softAlphaEdges", vpf.softWas);
            vpf.frames = 0;
            V_NextStep();
            return false;
        }
        case 6:
        case 7:
        case 8: {
            const int k = v.step - 6;
            const uint64 now = Sys_Microseconds();
            if (vpf.frames > 5) { // a few frames for the setting to take
                uint64 drawUs, backUs;
                VitaBench_LastFrame(&drawUs, &backUs);
                vpf.backMs[k] += backUs / 1000.0;
                vpf.gpuMs[k] += VitaBench_LastFrameGpuWait() / 1000.0;
                vpf.frameMs[k] += (now - vpf.lastUs) / 1000.0;
                vpf.n[k]++;
            }
            vpf.lastUs = now;
            if (vpf.n[k] < 45) {
                return false;
            }
            if (k == 0) {
                cvarSystem->SetCVarFloat("vita_softAlphaEdges", 0.0f); // the filter without the map
                vpf.frames = 0;
                V_NextStep();
                return false;
            }
            if (k == 1) {
                cvarSystem->SetCVarBool("vita_postFilterPlain", true); // the stock stretch
                vpf.frames = 0;
                V_NextStep();
                return false;
            }
            double f[3], b[3], g[3];
            for (int i = 0; i < 3; i++) {
                f[i] = vpf.frameMs[i] / vpf.n[i];
                b[i] = vpf.backMs[i] / vpf.n[i];
                g[i] = vpf.gpuMs[i] / vpf.n[i];
            }
            V_Check(V_INFO, va("%s stretch filter cost", map),
                "frame / back end / GPU wait ms (45 frames each, view still): filter as set (soft edges' map of %d alpha-tested surfaces) %.2f / %.2f / %.2f; without the map %.2f / %.2f / %.2f; stock stretch %.2f / %.2f / %.2f - the soft edges cost the back end %+.2f ms and the GPU wait %+.2f ms; the sharpening pass costs the GPU wait %+.2f ms",
                vpf.surfaces, f[0], b[0], g[0], f[1], b[1], g[1], f[2], b[2], g[2], b[0] - b[1], g[0] - g[1], g[1] - g[2]);
            V_PostCheckRestore();
            return true;
        }
    }
    V_PostCheckRestore();
    return true;
}

/*
---------------- fightcheck [count] [fire seconds] ----------------
Monsters of the map's own kinds in front of the player: they must wake and go
for him, the weapon must fire and kill, and every corpse must come to rest on
the floor (not hang in the air, not keep sliding).
*/
static struct {
    int count;
    float fireS;
    idStrList names;
    idStrList classes;
    int ammo0;
    idStr weapon0;
    bool firing;
    int weaponTry;
    float nextTryS;
    float lastLog;
    int target;
    bool woke[8];
    int killedByFire;
} vf;

static void V_FightStart()
{
    vf.count = idMath::ClampInt(1, 8, (v.args.Argc() > 1) ? atoi(v.args.Argv(1)) : 3);
    vf.fireS = (v.args.Argc() > 2) ? atof(v.args.Argv(2)) : 8.0f;
    vf.names.Clear();
    vf.classes.Clear();
    memset(vf.woke, 0, sizeof(vf.woke));
    vf.killedByFire = 0;
    vf.target = 0;
}

static bool V_FightPoll()
{
    const char* map = commonLocal.GetCurrentMapName();
    switch (v.step) {
        case 0: {
            idStrList classes;
            Vita_VerifyMonsterClasses(classes);
            {
                int alive = 0, awake = 0, moving = 0;
                Vita_VerifyCountAI(alive, awake, moving);
                VitaBench_Log("bench   fight: the map has %d monsters alive (%d awake), %d kinds to spawn", alive, awake, classes.Num());
            }
            if (classes.Num() == 0) {
                V_Check(V_INFO, va("%s fight", map), "the map has no monsters of its own to fight with - skipped");
                return true;
            }
            idStr why;
            static int serial = 0;
            for (int i = 0; i < vf.count; i++) {
                const idStr name = va("verify_monster_%d", ++serial);
                const char* cls = classes[i % classes.Num()].c_str();
                if (Vita_VerifySpawnNear(cls, name.c_str(), 320.0f, why)) {
                    vf.names.Append(name);
                    vf.classes.Append(cls);
                } else {
                    VitaBench_Log("bench   fight: %s not spawned: %s", cls, why.c_str());
                }
            }
            if (vf.names.Num() == 0) {
                V_Check(V_WARN, va("%s fight", map), "no room to spawn a monster in sight of the player (%s)", why.c_str());
                return true;
            }
            idStr list;
            for (int i = 0; i < vf.names.Num(); i++) {
                list += va("%s%s", i ? ", " : "", vf.classes[i].c_str());
            }
            VitaBench_Log("bench   fight: %d monsters spawned in front of the player: %s", vf.names.Num(), list.c_str());
            vf.ammo0 = -1;
            vf.firing = false;
            vf.weaponTry = 0;
            vf.nextTryS = 2.2f;
            vf.lastLog = 0.0f;
            VitaBench_OpenSegment(va("%s: fight (%s)", map, list.c_str()));
            VitaBench_SetMeasuring(true);
            V_NextStep();
            return false;
        }
        case 1: { // fire at them, one after the other
            vitaVerifyEnt_t e;
            int alive = 0;
            for (int i = 0; i < vf.names.Num(); i++) {
                if (Vita_VerifyEntity(vf.names[i].c_str(), e) && e.health > 0) {
                    alive++;
                    if (e.hasEnemy && !e.dormant) {
                        vf.woke[i] = true;
                    }
                }
            }
            // the target: the first one alive
            int t = -1;
            for (int i = 0; i < vf.names.Num() && t < 0; i++) {
                if (Vita_VerifyEntity(vf.names[i].c_str(), e) && e.health > 0) {
                    t = i;
                }
            }
            float yaw = 0.0f, pitch = 0.0f;
            const bool fireNow = V_Since(v.stepUs) > 1.5; // the weapon raised
            // a weapon with no ammo in hand (the slot asked for is not there,
            // or the map takes the guns away): the other guns, one by one
            if (fireNow && vf.ammo0 < 0 && V_Since(v.stepUs) > vf.nextTryS && vf.weaponTry < 7) {
                static const int guns[7] = { 5, 4, 3, 7, 2, 8, 9 };
                VitaBench_SetImpulse(guns[vf.weaponTry++]);
                vf.nextTryS = (float)V_Since(v.stepUs) + 0.8f;
            }
            if (fireNow && (!vf.firing || vf.ammo0 < 0)) {
                // the ammo of the weapon in hand as the firing starts (again
                // while it reads as none: the weapon still being raised)
                idVec3 o;
                idAngles a;
                int hp;
                Vita_VerifyPlayer(o, a, hp, vf.weapon0);
                vf.ammo0 = Vita_VerifyPlayerAmmo();
                vf.firing = true;
            }
            if (t >= 0 && Vita_VerifyAimAt(vf.names[t].c_str(), yaw, pitch)) {
                VitaBench_SetLook(true, yaw, pitch);
                VitaBench_SetInput(fireNow ? BUTTON_ATTACK : 0, 0, 0, 0.0f);
            } else {
                VitaBench_SetInput(0, 0, 0, 0.0f);
            }
            if (V_Since(v.stepUs) - vf.lastLog >= 1.0) {
                vf.lastLog = (float)V_Since(v.stepUs);
                idVec3 o;
                idAngles a;
                int hp;
                idStr w;
                Vita_VerifyPlayer(o, a, hp, w);
                vitaVerifyEnt_t te;
                const bool haveT = t >= 0 && Vita_VerifyEntity(vf.names[t].c_str(), te);
                VitaBench_Log("bench   fight %.0f s: %d alive, target %s (health %d, %.0f units away), aim yaw %.0f pitch %.0f, view yaw %.0f pitch %.0f, %s ammo %d, player health %d",
                    vf.lastLog, alive, t >= 0 ? vf.classes[t].c_str() : "-", haveT ? te.health : 0, haveT ? (te.origin - o).Length() : 0.0f,
                    yaw, pitch, a.yaw, a.pitch, w.c_str(), Vita_VerifyPlayerAmmo(), hp);
            }
            if (alive > 0 && V_Since(v.stepUs) < vf.fireS + 1.5) {
                return false;
            }
            VitaBench_SetInput(0, 0, 0, 0.0f);
            VitaBench_SetLook(false, 0.0f, 0.0f);
            int woke = 0;
            for (int i = 0; i < vf.names.Num(); i++) {
                woke += vf.woke[i] ? 1 : 0;
            }
            idStr weapon1;
            {
                idVec3 o;
                idAngles a;
                int hp;
                Vita_VerifyPlayer(o, a, hp, weapon1);
            }
            const int ammo1 = (weapon1 == vf.weapon0) ? Vita_VerifyPlayerAmmo() : -1;
            int dead = 0;
            for (int i = 0; i < vf.names.Num(); i++) {
                if (!Vita_VerifyEntity(vf.names[i].c_str(), e) || e.health <= 0) {
                    dead++;
                }
            }
            vf.killedByFire = dead;
            int dormantNow = 0;
            for (int i = 0; i < vf.names.Num(); i++) {
                if (!vf.woke[i] && Vita_VerifyEntity(vf.names[i].c_str(), e) && e.dormant) {
                    dormantNow++;
                }
            }
            V_Check(woke == vf.names.Num() ? V_PASS : (woke + dormantNow == vf.names.Num() ? V_WARN : V_FAIL), va("%s monsters", map), "%d of %d spawned monsters woke up and went for the player%s", woke, vf.names.Num(),
                dormantNow ? va("; %d stayed dormant - the map counts their spot as closed off from the player (stock behaviour)", dormantNow) : "");
            idStr noGun;
            const bool usable = Vita_VerifyWeaponsUsable(noGun);
            if (!usable) {
                V_Check(V_INFO, va("%s weapon", map), "%s: nothing to fire", noGun.c_str());
            } else
            V_Check(ammo1 >= 0 && ammo1 < vf.ammo0 ? V_PASS : V_FAIL, va("%s weapon", map), "%s: ammo %d -> %d while firing%s", vf.weapon0.c_str(), vf.ammo0, ammo1, ammo1 < 0 ? va(" (the weapon changed to %s)", weapon1.c_str()) : "");
            V_Check(dead > 0 ? V_PASS : (!usable ? V_INFO : V_WARN), va("%s kills", map), "%d of %d monsters killed by the player's fire in %.0f s", dead, vf.names.Num(), vf.fireS);
            // the rest, killed outright
            for (int i = 0; i < vf.names.Num(); i++) {
                Vita_BenchKill(vf.names[i].c_str());
            }
            V_NextStep();
            return false;
        }
        case 2: { // the corpses at rest
            vitaVerifyEnt_t e;
            bool moving = false;
            for (int i = 0; i < vf.names.Num(); i++) {
                if (Vita_VerifyEntity(vf.names[i].c_str(), e) && e.afActive && !e.atRest) {
                    moving = true; // (only ragdolls: a burning corpse never rests)
                }
            }
            if (moving && V_Since(v.stepUs) < 8.0) {
                return false;
            }
            int ok = 0, gone = 0, floating = 0, sliding = 0, burning = 0;
            idStr bad;
            for (int i = 0; i < vf.names.Num(); i++) {
                if (!Vita_VerifyEntity(vf.names[i].c_str(), e, true)) {
                    gone++; // burned away / gibbed
                    continue;
                }
                if (!e.afActive) {
                    // no ragdoll: a death that burns away (imps, wraiths...),
                    // held where the monster died until it is gone
                    burning++;
                    continue;
                }
                if (e.dormant) {
                    // closed off from the player: it simulates once he can see it again
                    bad += va(" %s dormant (closed off from the player);", vf.classes[i].c_str());
                    continue;
                }
                if (!e.atRest) {
                    sliding++;
                    bad += va(" %s still moving (fastest of its %d bodies %.0f u/s, simulated for %.1f s; thinking %s, health %d%s);", vf.classes[i].c_str(), e.bodies, e.maxBodySpeed, e.activeTime, e.active ? "yes" : "NO", e.health, e.hidden ? ", hidden" : "");
                } else if (e.floorGap > 24.0f) {
                    floating++;
                    bad += va(" %s at rest with nothing within %.0f units under any of its bodies (at %.0f %.0f %.0f);", vf.classes[i].c_str(), e.floorGap, e.origin.x, e.origin.y, e.origin.z);
                } else {
                    ok++;
                }
            }
            V_Check(floating == 0 && sliding == 0 ? V_PASS : V_FAIL, va("%s ragdolls", map), "%d ragdolls at rest on the floor, %d floating, %d still moving after %.1f s; %d corpses burning away (no ragdoll), %d gone%s",
                ok, floating, sliding, V_Since(v.stepUs), burning, gone, bad.c_str());
            VitaBench_SetMeasuring(false);
            VitaBench_CloseSegment();
            V_RequestShot(va("%s_fight", map));
            V_NextStep();
            return false;
        }
        case 3:
            if (!V_ShotDone() && V_Since(v.stepUs) < 5.0) {
                return false;
            }
            for (int i = 0; i < vf.names.Num(); i++) {
                Vita_BenchRemove(vf.names[i].c_str());
            }
            return true;
    }
    return true;
}

/*
---------------- savecheck ----------------
Saves, loads the save back, and compares where the player stands.
*/
static struct {
    idVec3 origin;
    idStr map;
    uint64 loadUs;
    int errors0;
    float speed;
} vsave;

static bool V_SavePoll()
{
    switch (v.step) {
        case 0: {
            idAngles a;
            int hp;
            idStr w;
            if (!Vita_VerifyPlayer(vsave.origin, a, hp, w)) {
                V_Check(V_FAIL, "save", "no player to save");
                return true;
            }
            vsave.map = commonLocal.GetCurrentMapName();
            vsave.errors0 = v.errors;
            vsave.speed = Vita_VerifyPlayerSpeed();
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "saveGame verify_check\n");
            V_NextStep();
            return false;
        }
        case 1: // the save threads finish
            if (V_Since(v.stepUs) < 1.0 || (session->GetSaveGameManager().IsWorking() && V_Since(v.stepUs) < 60.0)) {
                return false;
            }
            if (session->GetSaveGameManager().IsWorking()) {
                V_Check(V_FAIL, va("%s save", vsave.map.c_str()), "still saving after 60 s");
                return true;
            }
            V_Check(V_PASS, va("%s save", vsave.map.c_str()), "saved in %.1f s", V_Since(v.stepUs));
            vsave.loadUs = V_Now();
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "loadGame verify_check\n");
            V_NextStep();
            return false;
        case 2: {
            if (V_Since(v.stepUs) < 2.0) {
                return false;
            }
            if (v.errors != vsave.errors0) {
                V_Check(V_FAIL, va("%s load save", vsave.map.c_str()), "the game stopped with an error: \"%s\"", v.lastError.c_str());
                return true;
            }
            idStr why;
            const char* spawned = commonLocal.GetSpawnedMapName();
            if (spawned == NULL || vsave.map.Icmp(spawned) != 0 || !Vita_BenchGameReady(why)) {
                if (V_Since(v.stepUs) > 240.0) {
                    V_Check(V_FAIL, va("%s load save", vsave.map.c_str()), "not back in the game after 240 s (%s)", why.c_str());
                    return true;
                }
                return false;
            }
            idVec3 o;
            idAngles a;
            int hp;
            idStr w;
            Vita_VerifyPlayer(o, a, hp, w);
            const float d = (o - vsave.origin).Length();
            V_Check(d < 2.0f ? V_PASS : (d < 48.0f ? V_WARN : V_FAIL), va("%s load save", vsave.map.c_str()), "loaded back in %.1f s; the player %.1f units from where he was saved (%.0f %.0f %.0f; he was moving at %.0f u/s when saved)%s", V_Since(vsave.loadUs), d, o.x - vsave.origin.x, o.y - vsave.origin.y, o.z - vsave.origin.z, vsave.speed,
                (d >= 2.0f && d < 48.0f) ? " (a few units: standing on something that moves, or settling onto the floor)" : "");
            Vita_BenchPlayerSetup(true, false, false);
            return true;
        }
    }
    return true;
}

/*
---------------- menus ----------------
Out to the main menu, then every screen of it, each pictured.
*/

static struct {
    idList<int> areas;
    int i;
    int warn0;
} vmenu;

// the shell's screens (d3xp/menus/MenuHandler.h, shellAreas_t), as numbers
// to keep the game's headers out of here: root 1, campaign 3, load 4,
// new game 6, game options 7, system options 8, stereoscopics 11, settings 13,
// controls 17, controller layout 18, gamepad 19, difficulty 23, resolution 24,
// credits 28
static void V_MenusStart()
{
    static const int areas[] = { 1, 3, 6, 23, 4, 13, 7, 8, 24, 17, 19, 18, 11, 28, 1 };
    vmenu.areas.Clear();
    for (int i = 0; i < (int)(sizeof(areas) / sizeof(areas[0])); i++) {
        vmenu.areas.Append(areas[i]);
    }
    vmenu.i = 0;
    vmenu.warn0 = v.warnings;
    VitaBench_CloseSegment();
    VitaBench_SetMapName("menus");
    if (commonLocal.GetSpawnedMapName() != NULL && commonLocal.GetSpawnedMapName()[0]) {
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
    }
}

static bool V_MenusPoll()
{
    switch (v.step) {
        case 0: // the shell up
            if (!Vita_VerifyShellActive() || V_Since(v.stepUs) < 2.0) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, "main menu", "the main menu did not come up in 60 s");
                    return true;
                }
                return false;
            }
            {
                unsigned int freeKB = 0, cachedKB = 0;
                V_Memory(freeKB, cachedKB);
                v.menuFreeKB.Append(freeKB);
                v.menuCachedKB.Append(cachedKB);
                V_Check(V_PASS, "main menu", "up; free memory %u MB (cached heap %u MB)", freeKB / 1024, cachedKB / 1024);
            }
            V_NextStep();
            return false;
        case 1:
            Vita_VerifyShellGoto(vmenu.areas[vmenu.i]);
            V_NextStep();
            return false;
        case 2:
            if (V_Since(v.stepUs) < 1.5) {
                return false;
            }
            {
                const int want = vmenu.areas[vmenu.i];
                const int have = Vita_VerifyShellArea();
                V_Check(have == want ? V_PASS : V_FAIL, va("menu %s", Vita_VerifyShellAreaName(want)), have == want ? "shown" : "asked for it, the menu shows '%s'", Vita_VerifyShellAreaName(have));
                V_RequestShot(va("menu_%s", Vita_VerifyShellAreaName(want)));
            }
            V_NextStep();
            return false;
        case 3:
            if (!V_ShotDone() && V_Since(v.stepUs) < 5.0) {
                return false;
            }
            if (++vmenu.i < vmenu.areas.Num()) {
                v.step = 1;
                return false;
            }
            break;
    }
    int total = 0, hard = 0;
    idStr hardList, allList;
    V_WarningsSince(vmenu.warn0, total, hard, hardList, allList);
    V_Check(hard ? V_FAIL : (total ? V_WARN : V_PASS), "menu warnings", "%d warnings going through the menus, %d serious%s%s", total, hard, total ? ": " : "", allList.c_str());
    return true;
}

/*
---------------- videotest <file.mp4> [seconds] ----------------
A video as the game plays one: a material with a videoMap stage drawn over
the screen every frame (VitaVerify_Draw, from idCommonLocal::Draw), so the
renderer asks its cinematic for the picture as it does for a PDA disc or a
wall screen - on the Vita, the hardware H.264 / AAC player (sceAvPlayer).

The file first: an MP4 (ftyp), its index (moov) ahead of the data (the
player reads it before the first frame), an H.264 picture track (avc1), an
AAC sound track (mp4a). Then its first frame must come - 15 s at most:
another video may have to give up its player first - and it must play on at
10 fps or more; a screenshot of it playing goes to verify/. It is stopped
only once it plays (or the 15 s are up), and closed a few frames after it
was last drawn.

(2026-10-18, the first Vita run: the test drove an idCinematic of its own
from the swap, named the .bik, and gave each video 4 s - the first player,
closed before its first frame, wedged the video worker for the rest of the
run and all 51 failed. The .mp4 is named now and played the game's way.)
*/
static struct {
    idStr file;
    idStr stem;
    float seconds;
    const idMaterial* mat;
    int phase;         // 0 waiting for the first frame, 1 playing, 2 stopped (closing), 3 done
    double firstAt;    // seconds into the step of its first frame
    unsigned int firstMs; // the first frame, from the player's open (Vita)
    bool shotAsked;
    int stopPolls;
    int frames;
    double playedS;
    int lengthMs;
    bool failed;
    idStr why;
    bool pcRoq;        // the PC: its .roq (the Vita's software stream) drawn instead
    float pcMean;      // the screenshot's mean brightness in the middle
} vvid;
static const idMaterial* volatile vVideoDraw = NULL; // VitaVerify_Draw draws it while set

/*
---------------- the finish screen ----------------
When the benchmark or the verification ends (VitaBench.cpp, Bench_Finish) the
game goes back to the main menu; this panel stays over it with the verdicts
until SELECT is pressed (or another run starts), so a finished run is plain
to see. One screenshot of it goes to verify/ a second after it appears.
*/
enum { VB_WHITE, VB_GREEN, VB_RED, VB_YELLOW, VB_GREY };
static struct {
    idSysMutex lock;
    idStrList lines;
    idList<int> colors;
    bool shown;
    int frames;
    bool shotAsked;
    bool selectWas;
    bool sawMenu; // drawn once the game is back at the menu; a map started afterwards closes it
} vban;

void VitaVerify_BannerClear()
{
    idScopedCriticalSection cs(vban.lock);
    vban.lines.Clear();
    vban.colors.Clear();
    vban.shown = false;
}

// color: 0 white, 1 green, 2 red, 3 yellow, 4 grey; the first line is the title
void VitaVerify_BannerAdd(int color, VERIFY_FORMAT_STRING const char* fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    idStr::vsnPrintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    idScopedCriticalSection cs(vban.lock);
    vban.lines.Append(text);
    vban.colors.Append(color);
}

void VitaVerify_BannerShow()
{
    idScopedCriticalSection cs(vban.lock);
    vban.shown = vban.lines.Num() > 0;
    vban.frames = 0;
    vban.shotAsked = false;
    vban.selectWas = true; // a SELECT already held does not close it
    vban.sawMenu = false;
}

static void V_BannerDraw()
{
    idScopedCriticalSection cs(vban.lock);
    if (!vban.shown) {
        return;
    }
    // (the run ends inside a map; the disconnect follows a frame or two later.
    // The session's state, not mapSpawned: that stays set at the menu)
    const idSession::sessionState_t st = session->GetState();
    const bool inMap = st == idSession::LOADING || st == idSession::INGAME;
    const bool atMenu = st <= idSession::GAME_LOBBY;
    if (!vban.sawMenu) {
        if (!atMenu) {
            return;
        }
        vban.sawMenu = true;
        common->Printf("bench   finish screen shown over the menu\n");
    } else if (inMap) {
        vban.shown = false;
        VitaBench_Log("bench   finish screen closed (a map was started)");
        return;
    }
#if defined(__vita__)
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const bool select = (pad.buttons & SCE_CTRL_SELECT) != 0;
    if (select && !vban.selectWas && vban.frames > 30) {
        vban.shown = false;
        VitaBench_Log("bench   finish screen closed (SELECT)");
        return;
    }
    vban.selectWas = select;
#endif
    vban.frames++;
    static const idVec4 colors[] = { idVec4(1, 1, 1, 1), idVec4(0.35f, 1, 0.35f, 1), idVec4(1, 0.3f, 0.3f, 1), idVec4(1, 0.85f, 0.25f, 1), idVec4(0.7f, 0.7f, 0.7f, 1) };
    const float vw = renderSystem->GetVirtualWidth();
    const float vh = renderSystem->GetVirtualHeight();
    const int n = vban.lines.Num();
    // the console font: the title 16 x 16, the rest 8 x 16 - lines 18 apart
    int widest = 0;
    for (int i = 0; i < n; i++) {
        widest = Max(widest, vban.lines[i].LengthWithoutColors() * (i == 0 ? BIGCHAR_WIDTH : SMALLCHAR_WIDTH));
    }
    const float boxW = Min(vw - 16.0f, (float)widest + 32.0f);
    const float boxH = Min(vh - 16.0f, 24.0f + n * 18.0f + 8.0f);
    const float x0 = (vw - boxW) * 0.5f;
    const float y0 = (vh - boxH) * 0.5f;
    const idMaterial* white = declManager->FindMaterial("_white");
    renderSystem->SetColor4(0, 0, 0, 0.88f);
    renderSystem->DrawStretchPic(x0, y0, boxW, boxH, 0, 0, 1, 1, white);
    const idVec4& edge = colors[vban.colors[0]];
    renderSystem->SetColor4(edge.x, edge.y, edge.z, 1.0f);
    renderSystem->DrawStretchPic(x0, y0, boxW, 2, 0, 0, 1, 1, white);
    renderSystem->DrawStretchPic(x0, y0 + boxH - 2, boxW, 2, 0, 0, 1, 1, white);
    float y = y0 + 12.0f;
    for (int i = 0; i < n && y < y0 + boxH - 16.0f; i++) {
        const int len = vban.lines[i].LengthWithoutColors() * (i == 0 ? BIGCHAR_WIDTH : SMALLCHAR_WIDTH);
        const int x = (int)(i == 0 ? (vw - len) * 0.5f : x0 + 16.0f);
        if (i == 0) {
            renderSystem->DrawBigStringExt(x, (int)y, vban.lines[i].c_str(), colors[vban.colors[i]], true);
            y += 24.0f;
        } else {
            renderSystem->DrawSmallStringExt(x, (int)y, vban.lines[i].c_str(), colors[vban.colors[i]], true);
            y += 18.0f;
        }
    }
    renderSystem->SetColor4(1, 1, 1, 1);
    // a record of it in verify/, a second after it appears
    if (!vban.shotAsked && vban.frames == 60 && v.shotState == 0) {
        vban.shotAsked = true;
        v.suite = "finished";
        V_RequestShot("screen");
    }
}

// on the main thread every frame, a run or not (VitaBench_Frame): the finish
// screen's screenshot out to the card
void VitaVerify_Idle()
{
    if (vban.shotAsked && v.shotState == 2) {
        V_WriteShotIfReady();
        Mem_Free(v.shotRGB);
        v.shotRGB = NULL;
        vban.shotAsked = false;
    }
}

// from idCommonLocal::Draw (the game / draw thread), over everything but the console
void VitaVerify_Draw()
{
    V_BannerDraw();
    const idMaterial* mat = vVideoDraw;
    static const idMaterial* logged = NULL;
    if (mat != NULL && mat != logged) {
        logged = mat;
        VitaBench_Log("bench   video draw: %s, %d stage(s), sort %.0f, coverage %d, %dx%d virtual", mat->GetName(), mat->GetNumStages(), mat->GetSort(), (int)mat->Coverage(),
            renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight());
    }
    if (mat != NULL) {
        renderSystem->SetColor4(1, 1, 1, 1);
        renderSystem->DrawStretchPic(0, 0, renderSystem->GetVirtualWidth(), renderSystem->GetVirtualHeight(), 0, 0, 1, 1, mat);
    }
}

static idCinematic* V_VideoCinematic(const idMaterial* mat)
{
    if (mat == NULL) {
        return NULL;
    }
    for (int i = 0; i < mat->GetNumStages(); i++) {
        const shaderStage_t* st = mat->GetStage(i);
        if (st != NULL && st->texture.cinematic != NULL) {
            return st->texture.cinematic;
        }
    }
    return NULL;
}

static bool V_HasTag(const byte* buf, int len, const char* tag)
{
    for (int i = 0; i + 4 <= len; i++) {
        if (buf[i] == (byte)tag[0] && !memcmp(buf + i, tag, 4)) {
            return true;
        }
    }
    return false;
}

// the file: an .mp4 the Vita's player takes; true when it is there to play
static bool V_VideoCheckFile(const char* file)
{
    const char* label = va("video %s file", file);
    idStr ext;
    idStr(file).ExtractFileExtension(ext);
    if (ext.Icmp("mp4") != 0) {
        V_Check(V_FAIL, label, "not an .mp4: the Vita plays H.264 + AAC .mp4 videos in hardware, there is no Bink decoder");
        return false;
    }
    idFile* f = fileSystem->OpenFileRead(file);
    if (f == NULL) {
#if defined(__vita__)
        V_Check(V_FAIL, label, "missing (tools/vita/convert_video.py makes the .mp4 of each of the game's videos)");
#else
        V_Check(V_INFO, label, "not in this PC's data (it has the .bik only) - the Vita plays the .mp4");
#endif
        return false;
    }
    const int len = f->Length();
    const int readLen = Min(len, 512 * 1024);
    byte* buf = (byte*)Mem_Alloc(Max(readLen, 16), TAG_TEMP);
    const int got = f->Read(buf, readLen);
    fileSystem->CloseFile(f);
    bool ftyp = false;
    int moovAt = -1, moovEnd = -1, mdatAt = -1;
    for (int pos = 0; pos + 8 <= got;) {
        const uint32 size = ((uint32)buf[pos] << 24) | ((uint32)buf[pos + 1] << 16) | ((uint32)buf[pos + 2] << 8) | buf[pos + 3];
        if (!memcmp(buf + pos + 4, "ftyp", 4)) {
            ftyp = true;
        } else if (!memcmp(buf + pos + 4, "moov", 4) && moovAt < 0) {
            moovAt = pos;
            moovEnd = pos + (int)size;
        } else if (!memcmp(buf + pos + 4, "mdat", 4) && mdatAt < 0) {
            mdatAt = pos;
        }
        if (size < 8) {
            break; // 0: to the end of the file, 1: a 64-bit size - the last box looked at
        }
        pos += (int)size;
    }
    const bool wholeIndex = moovAt >= 0 && moovEnd <= got;
    const int idxLen = wholeIndex ? moovEnd - moovAt : (moovAt >= 0 ? got - moovAt : 0);
    const bool avc = moovAt >= 0 && V_HasTag(buf + moovAt, idxLen, "avc1");
    const bool aac = moovAt >= 0 && V_HasTag(buf + moovAt, idxLen, "mp4a");
    Mem_Free(buf);
    if (!ftyp) {
        V_Check(V_FAIL, label, "not an MP4 (no ftyp box at the start; %d bytes)", len);
        return false;
    }
    if (moovAt < 0 || (mdatAt >= 0 && mdatAt < moovAt)) {
        V_Check(V_WARN, label, "MP4, %.1f MB, but its index is after the data (made without -movflags +faststart): the player must read to the end before the first frame", len / 1048576.0);
        return true;
    }
    if (!avc) {
        V_Check(wholeIndex ? V_FAIL : V_WARN, label, "MP4, %.1f MB, %s", len / 1048576.0,
            wholeIndex ? "but no H.264 picture track (avc1) - the Vita's player decodes H.264 only" : "no H.264 track seen in its first 512 KB of index");
        return wholeIndex ? false : true;
    }
    V_Check(V_PASS, label, "MP4, H.264 picture (avc1)%s, %.1f MB, index at the front", aac ? " + AAC sound (mp4a)" : ", no sound track", len / 1048576.0);
    return true;
}

static void V_VideoStart()
{
    vvid.file = v.args.Argv(1);
    vvid.seconds = (v.args.Argc() > 2) ? atof(v.args.Argv(2)) : 4.0f;
    vvid.stem = vvid.file;
    vvid.stem.StripPath();
    vvid.stem.StripFileExtension();
    vvid.mat = NULL;
    vvid.phase = 3;
    vvid.firstAt = 0.0;
    vvid.firstMs = 0;
    vvid.shotAsked = false;
    vvid.stopPolls = 0;
    vvid.frames = 0;
    vvid.playedS = 0.0;
    vvid.lengthMs = 0;
    vvid.failed = false;
    vvid.why.Clear();
    vvid.pcRoq = false;
    vvid.pcMean = 0.0f;
    const bool haveFile = V_VideoCheckFile(vvid.file.c_str());
#if defined(__vita__)
    if (!haveFile) {
        return;
    }
#endif
    // its own material, as a GUI or a map gives a video (made on the PC too,
    // with no file to play: the first Vita run crashed making it - a stock
    // bug in idDeclManagerLocal::CreateNewDecl - which no PC run had done)
    idStr name = "_verify/";
    name += vvid.file;
    name.StripFileExtension();
    idDecl* decl = declManager->CreateNewDecl(DECL_MATERIAL, name.c_str(), "materials/_verify_videos.mtr");
    // written as the game's own (materials/video.mtr: "translucent { videomap
    // ... linear }") - an opaque stage is not drawn by the GUI pass at all
    decl->SetText(va("material %s\n{\n\ttranslucent\n\t{\n\t\tvideoMap %s\n\t\tlinear\n\t}\n}\n", name.c_str(), vvid.file.c_str()));
    decl->Invalidate();
    vvid.mat = declManager->FindMaterial(name.c_str(), false);
    if (V_VideoCinematic(vvid.mat) == NULL) {
        V_Check(V_FAIL, va("video %s", vvid.file.c_str()), "its material (videoMap %s) has no video stage", vvid.file.c_str());
        vvid.mat = NULL;
        return;
    }
#if !defined(__vita__)
    // the PC build has no H.264 decoder: the file and the material are all
    // that is checked - unless the video has a .roq, the Vita's software
    // stream for when its hardware player is taken (Cinematic.cpp), which the
    // PC's RoQ decoder (the same code) then draws: proof the converted
    // RoQ decodes in this engine
    idStr roq = vvid.file;
    roq.SetFileExtension("roq");
    idFile* roqFile = fileSystem->OpenFileRead(roq);
    if (roqFile != NULL) {
        fileSystem->CloseFile(roqFile);
        vvid.pcRoq = true;
        V_VideoCinematic(vvid.mat)->ResetTime(-1); // from its start, at the next draw
        vvid.phase = 0;
        __sync_synchronize();
        vVideoDraw = vvid.mat;
        VitaBench_Log("bench   video %s: its software stream %s drawn through material %s", vvid.file.c_str(), roq.c_str(), name.c_str());
        return;
    }
    V_Check(V_INFO, va("video %s", vvid.file.c_str()), "material %s made (a videoMap stage); not played on the PC (no H.264 decoder in this build) - the Vita plays it in hardware", name.c_str());
    V_VideoCinematic(vvid.mat)->Close();
    vvid.mat = NULL;
    return;
#else
    VitaVideo_Watch(va("/%s", vvid.file.c_str()));
    vvid.phase = 0;
    __sync_synchronize();
    vVideoDraw = vvid.mat;
    VitaBench_Log("bench   video %s: drawn through material %s from now", vvid.file.c_str(), name.c_str());
#endif
}

static void V_VideoStop(double t)
{
    vVideoDraw = NULL;
    vvid.playedS = (vvid.phase == 1) ? t - vvid.firstAt : 0.0;
    idCinematic* cin = V_VideoCinematic(vvid.mat);
    vvid.lengthMs = (cin != NULL) ? cin->AnimationLength() : 0;
    vvid.phase = 2;
    vvid.stopPolls = 0;
}

static bool V_VideoPoll()
{
    if (vvid.phase == 3) {
        return true;
    }
    const double t = V_Since(v.stepUs);
    const char* label = va("video %s", vvid.file.c_str());
#if defined(__vita__)
    unsigned int firstMs = 0;
    const int frames = VitaVideo_WatchedFrames(&firstMs);
#else
    const unsigned int firstMs = 0;
    const int frames = 0;
#endif
#if !defined(__vita__)
    if (vvid.pcRoq && vvid.phase == 0) {
        // a shot at 1.5 s; one more at 4 s if that frame was black (dropship
        // is black for its first 3 s, as the original; monoflyby ends at 2.7 s)
        const double shotAt = (vvid.stopPolls == 0) ? 1.5 : 4.0;
        if (!vvid.shotAsked && t >= shotAt) {
            V_RequestShot(va("video_%s_roq", vvid.stem.c_str()));
            vvid.shotAsked = true;
        } else if (vvid.shotAsked) {
            V_WriteShotIfReady();
            if (v.shotState == 0 && v.shotRGB != NULL) {
                // the middle half of the screen, where the video is drawn
                double sum = 0.0;
                int n = 0;
                for (int y = v.shotH / 4; y < v.shotH * 3 / 4; y += 2) {
                    const byte* row = v.shotRGB + y * v.shotW * 4;
                    for (int x = v.shotW / 4; x < v.shotW * 3 / 4; x += 2) {
                        sum += 0.299 * row[x * 4] + 0.587 * row[x * 4 + 1] + 0.114 * row[x * 4 + 2];
                        n++;
                    }
                }
                vvid.pcMean = Max(vvid.pcMean, n ? (float)(sum / n) : 0.0f);
                if (vvid.pcMean <= 2.0f && vvid.stopPolls == 0) {
                    vvid.stopPolls = 1; // (reset by V_VideoStop) the second shot
                    vvid.shotAsked = false;
                } else {
                    V_VideoStop(t);
                }
            } else if (t > 10.0) {
                V_VideoStop(t);
            }
        }
        return false;
    }
#endif
    if (vvid.phase == 0) {
        if (frames > 0) {
            vvid.phase = 1;
            vvid.firstAt = t;
            vvid.firstMs = firstMs;
        } else if (t > 15.0) {
            vvid.failed = true;
#if defined(__vita__)
            char status[512];
            VitaVideo_Status(status, sizeof(status));
            vvid.why = status;
#endif
            V_VideoStop(t);
        }
        return false;
    }
    if (vvid.phase == 1) {
        vvid.frames = frames;
        if (!vvid.shotAsked && t - vvid.firstAt >= 1.5) {
            V_RequestShot(va("video_%s", vvid.stem.c_str()));
            vvid.shotAsked = true;
        }
        if (vvid.shotAsked) {
            V_WriteShotIfReady();
        }
        if (t - vvid.firstAt >= vvid.seconds && (!vvid.shotAsked || v.shotState == 0)) {
            V_VideoStop(t);
        }
        return false;
    }
    // stopped: closed once no frame being drawn can still ask it for its
    // picture (a few frames), so its player and its sound stop now
    if (++vvid.stopPolls < 4) {
        return false;
    }
    idCinematic* cin = V_VideoCinematic(vvid.mat);
    if (cin != NULL) {
        cin->Close();
    }
#if defined(__vita__)
    VitaVideo_Watch("");
#endif
    vvid.phase = 3;
    if (vvid.pcRoq) {
        V_Check(vvid.pcMean > 2.0f ? V_PASS : V_FAIL, label, "its software stream (.roq, the Vita's when the hardware player is taken) drawn by the RoQ decoder: mean brightness %.0f/255 in the middle of the screen (screenshot video_%s_roq)",
            vvid.pcMean, vvid.stem.c_str());
        return true;
    }
    if (vvid.failed) {
        V_Check(V_FAIL, label, "no frame in 15 s on screen | video system: %s", vvid.why.c_str());
        return true;
    }
    // a video shorter than the time watched is judged over its length
    double overS = vvid.playedS;
    if (vvid.lengthMs > 0 && vvid.lengthMs / 1000.0 < overS) {
        overS = Max(0.1, vvid.lengthMs / 1000.0 - vvid.firstMs / 1000.0);
    }
    const float fps = (overS > 0.0) ? (float)(vvid.frames / overS) : 0.0f;
    V_Check(fps >= 10.0f || (vvid.lengthMs > 0 && vvid.lengthMs < 1500) ? V_PASS : V_FAIL, label,
        "hardware H.264 player: first frame %u ms after it was opened (%.1f s after the video came on screen), then %d frames in %.1f s (%.1f fps); %d ms long",
        vvid.firstMs, vvid.firstAt, vvid.frames, vvid.playedS, fps, vvid.lengthMs);
    return true;
}

/*
---------------- trophies ----------------
The trophies: the PS3 set as the Vita's own (sce_sys/trophy/DBFG00001_00,
vita_trophies.cpp). The trophy service must be up with all 66 trophies of
the pack; then every achievement (1-65; the platinum is the system's) is
sent through the game's own unlock path, and must reach the trophy system.
Nothing is unlocked: while the benchmark or the verification runs, unlock
requests are recorded, not sent (the Vita cannot lock a trophy again). The
achievements the run's own play asked for are listed too.
*/
static void V_TrophyCheck()
{
#if defined(__vita__)
    int count = 0, earned = 0, initError = 0;
    const bool up = Vita_TrophyStatus(count, earned, initError);
    if (!up) {
        V_Check(V_FAIL, "trophy system", "not up (0x%08X): %s", (unsigned int)initError, Vita_TrophyWhy());
        return;
    }
    V_Check(count == 66 ? V_PASS : V_FAIL, "trophy system", "up: set DBFG00001_00, %d trophies in the pack (66: the PS3 set - 1 platinum, 1 gold, 64 bronze), %d earned on this Vita", count, earned);
    idStr natural;
    int naturalCount = 0;
    for (int id = 1; id < 66; id++) {
        if (Vita_TrophyDryRunHit(id)) {
            natural += va("%s#%d %s", naturalCount ? ", " : "", id, Vita_TrophyName(id, NULL));
            naturalCount++;
        }
    }
    V_Check(V_INFO, "trophies earned by the run's play", "%d (recorded, not unlocked)%s%s", naturalCount, naturalCount ? ": " : "", natural.c_str());
    int reached = 0, already = 0, noUser = 0;
    idStr missing;
    for (int id = 1; id < 66; id++) {
        const int r = Vita_VerifyTrophyChain(id);
        if (r < 0) {
            noUser++;
        } else if (r == 1) {
            already++;
        } else if (Vita_TrophyDryRunHit(id)) {
            reached++;
        } else {
            missing += va("%s#%d %s", missing.Length() ? ", " : "", id, Vita_TrophyName(id, NULL));
        }
    }
    if (noUser > 0) {
        V_Check(V_FAIL, "trophy unlock path", "no signed-in user to earn them");
        return;
    }
    V_Check(missing.Length() == 0 ? V_PASS : V_FAIL, "trophy unlock path", "%d of %d achievements reached the trophy system through the game's unlock path (dry run, nothing unlocked); %d earned on this Vita already%s%s",
        reached, 65 - already, already, missing.Length() ? " | never reached it: " : "", missing.c_str());
#else
    V_Check(V_INFO, "trophy system", "the PC build has no trophies (the Vita: vita_trophies.cpp)");
#endif
}

/*
---------------- langcheck ----------------
Every language's text, without switching the game's own: the strings each
language lacks (they would show as #str_ codes), text that is not valid
UTF-8, characters none of that language's fonts can draw, strings left in
English, and its voice-over pack.
*/
static bool V_Utf8Next(const char*& p, uint32& cp)
{
    const byte c = (byte)*p++;
    if (c < 0x80) {
        cp = c;
        return true;
    }
    int n;
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        return false;
    }
    for (int i = 0; i < n; i++) {
        const byte d = (byte)*p;
        if ((d & 0xC0) != 0x80) {
            return false;
        }
        p++;
        cp = (cp << 6) | (d & 0x3F);
    }
    return true;
}

static bool V_LoadLang(const char* lang, idLangDict& dict)
{
    idFileList* files = fileSystem->ListFilesTree("strings", ".lang", true);
    int loaded = 0;
    for (int i = 0; i < files->GetNumFiles(); i++) {
        idStr f = files->GetFile(i);
        idStr base = f;
        base.StripPath();
        if (idStr::Icmpn(base.c_str(), lang, (int)strlen(lang)) != 0) {
            continue;
        }
        const byte* buffer = NULL;
        const int len = fileSystem->ReadFile(f, (void**)&buffer);
        if (len > 0) {
            dict.Load(buffer, len, f);
            loaded++;
        }
        fileSystem->FreeFile((void*)buffer);
    }
    fileSystem->FreeFileList(files);
    if (loaded > 0) {
        // the port's own strings and its fixes to the game's, as the game adds them
        extern void Vita_AddPortStringsTo(idLangDict & dict, int column, bool log);
        extern int Vita_PortStringColumn(const char* lang);
        Vita_AddPortStringsTo(dict, Vita_PortStringColumn(lang), false);
    }
    return loaded > 0;
}

static void V_LangCheck()
{
    static const char* langs[] = { "english", "french", "german", "italian", "spanish", "japanese", NULL };
    static const char* packs[] = { "en", "fr", "gr", "it", "sp", "jp", NULL };
    idLangDict english;
    if (!V_LoadLang("english", english)) {
        V_Check(V_FAIL, "english", "no english string files");
        return;
    }
    idStrList baseFonts;
    Vita_RegisteredFontNames(baseFonts);
    for (int l = 0; langs[l] != NULL; l++) {
        idLangDict dict;
        if (!V_LoadLang(langs[l], dict)) {
            V_Check(V_FAIL, langs[l], "no string files");
            continue;
        }
        // keys english has and this language lacks
        int missing = 0;
        idStr missingList;
        for (int i = 0; i < english.GetNumKeyVals(); i++) {
            const idLangKeyValue* kv = english.GetKeyVal(i);
            if (idStr::Icmpn(kv->key, "#font_", 6) == 0) {
                continue; // font names, not text
            }
            if (dict.FindString(kv->key) == NULL) {
                missing++;
                if (missingList.Length() < 400) {
                    missingList += va(" %s", kv->key);
                }
            }
        }
        V_Check(missing ? V_FAIL : V_PASS, va("%s strings", langs[l]), "%d strings, %d of english's %d missing%s", dict.GetNumKeyVals(), missing, english.GetNumKeyVals(), missingList.c_str());
        // the fonts this language draws with
        idList<idFont*> fonts;
        for (int f = 0; f < baseFonts.Num(); f++) {
            const char* remap = dict.FindString(va("#font_%s", baseFonts[f].c_str()));
            if (remap == NULL) {
                remap = dict.FindString("#font_*");
            }
            idFont* font = renderSystem->RegisterFont(remap != NULL ? remap : baseFonts[f].c_str());
            if (font != NULL) {
                fonts.AddUnique(font);
            }
        }
        int badUtf8 = 0, noGlyph = 0, sameAsEnglish = 0;
        idStr badList, glyphList, sameList;
        idList<uint32> reported;
        for (int i = 0; i < dict.GetNumKeyVals(); i++) {
            const idLangKeyValue* kv = dict.GetKeyVal(i);
            if (idStr::Icmpn(kv->key, "#font_", 6) == 0) {
                continue;
            }
            const char* p = kv->value;
            bool bad = false;
            while (*p) {
                uint32 cp;
                if (!V_Utf8Next(p, cp)) {
                    bad = true;
                    break;
                }
                if (cp < 32 || cp == 0xA0 || reported.FindIndex(cp) >= 0 || fonts.Num() == 0) {
                    continue;
                }
                bool drawn = false;
                for (int f = 0; f < fonts.Num() && !drawn; f++) {
                    drawn = fonts[f]->HasGlyph(cp);
                }
                if (!drawn) {
                    noGlyph++;
                    reported.Append(cp);
                    if (glyphList.Length() < 400) {
                        glyphList += va(" U+%04X (%s)", cp, kv->key);
                    }
                }
            }
            if (bad) {
                badUtf8++;
                if (badList.Length() < 300) {
                    badList += va(" %s", kv->key);
                }
            }
            if (l > 0) {
                const char* en = english.FindString(kv->key);
                int letters = 0;
                for (const char* q = kv->value; *q; q++) {
                    letters += ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z')) ? 1 : 0;
                }
                if (en != NULL && letters >= 6 && strchr(kv->value, ' ') != NULL && idStr::Cmp(en, kv->value) == 0) {
                    sameAsEnglish++;
                    if (sameList.Length() < 500) {
                        sameList += va(" %s=\"%s\"", kv->key, idStr(kv->value).Left(40).c_str());
                    }
                }
            }
        }
        V_Check(badUtf8 ? V_FAIL : V_PASS, va("%s text encoding", langs[l]), "%d strings that are not valid UTF-8%s", badUtf8, badList.c_str());
        V_Check(noGlyph ? V_FAIL : V_PASS, va("%s glyphs", langs[l]), "%d characters none of its %d fonts can draw%s", noGlyph, fonts.Num(), glyphList.c_str());
        if (l > 0) {
            V_Check(V_INFO, va("%s translation", langs[l]), "%d sentences identical to english (names and titles can be)%s", sameAsEnglish, sameList.c_str());
        }
        // its voice-over pack
        const idStr pack = va("_sound_pc_%s.resources", packs[l]);
        idFile* f = fileSystem->OpenFileRead(pack);
        V_Check(f != NULL && f->Length() > 1024 * 1024 ? V_PASS : V_FAIL, va("%s voice", langs[l]), "%s %s", pack.c_str(), f != NULL ? va("present, %d MB", f->Length() / (1024 * 1024)) : "missing");
        delete f;
    }
}

/*
---------------- friends [count] ----------------
The friends list at capacity: added, found, renamed, saved and read back,
the overflow refused, then all removed - the player's own friends untouched.
*/
static void V_FriendsCheck(int count)
{
    netFriends.Load();
    const int before = netFriends.Num();
    idStrList beforeIds;
    for (int i = 0; i < before; i++) {
        beforeIds.Append(netFriends.Get(i).id);
    }
    const uint64 t0 = V_Now();
    idStrList ids;
    int added = 0;
    for (int i = 0; i < count; i++) {
        const idStr id = va("0000beef%08x", i);
        netadr_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.type = NA_IP; // loopback: anything sent to them stays on the console
        addr.ip[0] = 127;
        addr.ip[3] = 1;
        addr.port = (unsigned short)(28000 + i);
        if (netFriends.Add(id, va("VerifyFriend%03d", i), addr)) {
            added++;
            ids.Append(id);
        } else {
            break; // full
        }
    }
    const double addS = V_Since(t0);
    int found = 0;
    for (int i = 0; i < ids.Num(); i++) {
        found += netFriends.Find(ids[i]) >= 0 ? 1 : 0;
    }
    V_Check(added > 0 && found == added ? V_PASS : V_FAIL, "add friends", "%d friends added (asked for %d; the list had %d) in %.1f s, %d found again", added, count, before, addS, found);
    // the list's limit
    if (added < count) {
        netadr_t addr;
        memset(&addr, 0, sizeof(addr));
        const bool overflow = netFriends.Add("0000beeffffffff0", "VerifyOverflow", addr);
        V_Check(!overflow ? V_PASS : V_FAIL, "friends list full", "%d friends: one more %s", netFriends.Num(), overflow ? "was accepted (no limit)" : "is refused, as it should be");
        if (overflow) {
            ids.Append("0000beeffffffff0");
        }
    }
    // a friend seen again under a new name
    if (ids.Num() > 0) {
        netadr_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.type = NA_IP;
        addr.ip[0] = 127;
        addr.ip[3] = 1;
        addr.port = 28999;
        netFriends.Seen(ids[0], "VerifyRenamed", addr);
        const int k = netFriends.Find(ids[0]);
        V_Check(k >= 0 && netFriends.Get(k).name.Cmp("VerifyRenamed") == 0 ? V_PASS : V_FAIL, "friend renamed", "a friend heard from under a new name and address is updated (%s)", k >= 0 ? netFriends.Get(k).name.c_str() : "lost");
    }
    // the file read back
    netFriends.Save();
    idNetFriends reread;
    reread.Load();
    int kept = 0;
    for (int i = 0; i < ids.Num(); i++) {
        kept += reread.Find(ids[i]) >= 0 ? 1 : 0;
    }
    V_Check(kept == ids.Num() ? V_PASS : V_FAIL, "friends saved", "%d of %d friends read back from the save folder", kept, ids.Num());
    // all removed again
    const uint64 t1 = V_Now();
    for (int i = 0; i < ids.Num(); i++) {
        netFriends.Remove(ids[i]);
    }
    int left = 0;
    for (int i = 0; i < ids.Num(); i++) {
        left += netFriends.Find(ids[i]) >= 0 ? 1 : 0;
    }
    int ownKept = 0;
    for (int i = 0; i < beforeIds.Num(); i++) {
        ownKept += netFriends.Find(beforeIds[i]) >= 0 ? 1 : 0;
    }
    V_Check(left == 0 && ownKept == before && netFriends.Num() == before ? V_PASS : V_FAIL, "remove friends", "%d removed in %.1f s, %d left behind; the player's own %d friends: %d kept", ids.Num() - left, V_Since(t1), left, before, ownKept);
}


/*
---------------- classic <doom|doom2|nerve> <attract s> <maps...> ----------------
DOOM (maps as episode-map pairs), DOOM II or No Rest for the Living (map
numbers): out of DOOM 3 into the game, its attract demos played for a while,
then a game started on each map - god mode, every weapon - fired, pictured;
then back to DOOM 3's menu.
*/
static struct {
    idStr game;
    int attractS;
    idList<int> episodes, maps;
    int i;
    bool sawDemo;
    int tic0;
    int ammo0;
    int leveltime0;
    int kills0;
    uint64 levelUs;
} vc;

static void V_ClassicStart()
{
    vc.game = v.args.Argv(1);
    vc.attractS = (v.args.Argc() > 2) ? atoi(v.args.Argv(2)) : 30;
    vc.episodes.Clear();
    vc.maps.Clear();
    const bool pairs = !vc.game.Icmp("doom");
    for (int a = 3; a < v.args.Argc(); a += pairs ? 2 : 1) {
        if (pairs && a + 1 < v.args.Argc()) {
            vc.episodes.Append(atoi(v.args.Argv(a)));
            vc.maps.Append(atoi(v.args.Argv(a + 1)));
        } else if (!pairs) {
            vc.episodes.Append(1);
            vc.maps.Append(atoi(v.args.Argv(a)));
        }
    }
    vc.i = 0;
    vc.sawDemo = false;
    VitaBench_CloseSegment();
    VitaBench_SetMapName(va("classic %s", vc.game.c_str()));
    if (commonLocal.GetSpawnedMapName() != NULL && commonLocal.GetSpawnedMapName()[0]) {
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
    }
}

static const char* V_ClassicName()
{
    return !vc.game.Icmp("doom") ? "DOOM" : (!vc.game.Icmp("nerve") ? "No Rest for the Living" : "DOOM II");
}

static bool V_ClassicPoll()
{
    vitaClassicState_t st;
    switch (v.step) {
        case 0: // DOOM 3's menu first
            if (common->GetCurrentGame() == DOOM3_BFG && !Vita_VerifyShellActive()) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, V_ClassicName(), "DOOM 3's menu did not come up to start from");
                    return true;
                }
                return false;
            }
            common->SwitchToGame(!vc.game.Icmp("doom") ? DOOM_CLASSIC : DOOM2_CLASSIC);
            V_NextStep();
            return false;
        case 1: // the classic game up
            if (common->GetCurrentGame() == DOOM3_BFG || !Vita_ClassicState(st)) {
                if (V_Since(v.stepUs) > 30.0) {
                    V_Check(V_FAIL, V_ClassicName(), "the game did not start in 30 s");
                    common->SwitchToGame(DOOM3_BFG);
                    return true;
                }
                return false;
            }
            if (!vc.game.Icmp("nerve") && st.mission != 5) {
                Vita_ClassicSetExpansion(5); // pack_nerve: the engine restarts on NERVE.WAD
                V_NextStep();
                v.step = 1; // wait for it again
                if (V_Since(v.opUs) > 60.0) {
                    V_Check(V_FAIL, V_ClassicName(), "NERVE.WAD did not load (mission %d)", st.mission);
                    common->SwitchToGame(DOOM3_BFG);
                    return true;
                }
                return false;
            }
            V_Check(V_PASS, va("%s start", V_ClassicName()), "running (mission %d) %.1f s after the switch", st.mission, V_Since(v.opUs));
#if defined(__vita__)
            {
                // the zone holds the screens and every cached lump: in the
                // uncached pool the attract screens ran at 4.8 fps (run 7)
                int zoneMB = 0;
                bool zoneCached = false;
                Vita_ClassicZone(&zoneMB, &zoneCached);
                int pieces = 0, cachedMB = 0;
                Vita_ClassicZonePieces(&pieces, &cachedMB);
                V_Check(zoneCached ? V_PASS : (cachedMB >= 8 ? V_WARN : V_FAIL), va("%s memory", V_ClassicName()), "the zone is %d MB in %d piece%s, %d MB of it cached RAM%s", zoneMB, pieces, pieces == 1 ? "" : "s", cachedMB,
                    zoneCached ? "" : " - the rest is UNCACHED memory: what the renderer touches there is slow");
            }
#endif
            vc.tic0 = st.gametic;
            VitaBench_OpenSegment(va("%s: attract demos", V_ClassicName()));
            VitaBench_SetMeasuring(true);
            V_NextStep();
            return false;
        case 2: // the attract loop: title, demos
            Vita_ClassicState(st);
            vc.sawDemo |= st.demo;
            if (V_Since(v.stepUs) < vc.attractS) {
                return false;
            }
            VitaBench_SetMeasuring(false);
            VitaBench_CloseSegment();
            V_Check(vc.sawDemo && st.gametic > vc.tic0 ? V_PASS : V_FAIL, va("%s demos", V_ClassicName()), "%d tics in %d s of the attract loop, a demo %s", st.gametic - vc.tic0, vc.attractS, vc.sawDemo ? "played" : "never played");
            V_RequestShot(va("classic_%s_attract", vc.game.c_str()));
            V_NextStep();
            return false;
        case 3:
            if (!V_ShotDone() && V_Since(v.stepUs) < 5.0) {
                return false;
            }
            if (vc.i >= vc.maps.Num()) {
                v.step = 7;
                return false;
            }
            Vita_ClassicNewGame(vc.episodes[vc.i], vc.maps[vc.i]);
            V_NextStep();
            return false;
        case 4: // on the map
            Vita_ClassicState(st);
            if (!(st.gamestate == 0 && st.map == vc.maps[vc.i] && (vc.game.Icmp("doom") || st.episode == vc.episodes[vc.i]) && !st.demo)) {
                if (V_Since(v.stepUs) > 30.0) {
                    V_Check(V_FAIL, va("%s E%dM%d", V_ClassicName(), vc.episodes[vc.i], vc.maps[vc.i]), "not on the map after 30 s (state %d, E%dM%d)", st.gamestate, st.episode, st.map);
                    vc.i++;
                    v.step = 3;
                }
                return false;
            }
            Vita_ClassicCheats();
            vc.levelUs = V_Now();
            VitaBench_OpenSegment(va("%s E%dM%d: firing and turning", V_ClassicName(), vc.episodes[vc.i], vc.maps[vc.i]));
            VitaBench_SetMeasuring(true);
            V_NextStep();
            return false;
        case 5: // the weapon raised, then fire and turn
            Vita_ClassicState(st);
            if (V_Since(v.stepUs) < 1.0) {
                vc.ammo0 = st.ammo;
                vc.leveltime0 = st.leveltime;
                vc.kills0 = st.kills;
                return false;
            }
            VitaBench_SetInput(BUTTON_ATTACK, 0, 0, 0.06f);
            if (V_Since(v.stepUs) < 6.0) {
                return false;
            }
            VitaBench_SetInput(0, 0, 0, 0.0f);
            VitaBench_SetMeasuring(false);
            VitaBench_CloseSegment();
            {
                const bool doom1 = !vc.game.Icmp("doom");
                const idStr name = doom1 ? va("%s E%dM%d", V_ClassicName(), vc.episodes[vc.i], vc.maps[vc.i]) : va("%s MAP%02d", V_ClassicName(), vc.maps[vc.i]);
                const bool ok = st.health > 0 && st.leveltime > vc.leveltime0 && st.gamestate == 0;
                V_Check(ok ? V_PASS : V_FAIL, name.c_str(), "played %.1f s (%d tics of level time), health %d, ammo %d -> %d (firing), %d kills", V_Since(vc.levelUs), st.leveltime - vc.leveltime0, st.health, vc.ammo0, st.ammo, st.kills - vc.kills0);
                V_Check(st.ammo < vc.ammo0 ? V_PASS : V_WARN, va("%s weapon", name.c_str()), "ammo %d -> %d", vc.ammo0, st.ammo);
            }
            V_RequestShot(va("classic_%s_%d_%d", vc.game.c_str(), vc.episodes[vc.i], vc.maps[vc.i]));
            V_NextStep();
            return false;
        case 6:
            if (!V_ShotDone() && V_Since(v.stepUs) < 5.0) {
                return false;
            }
            vc.i++;
            v.step = 3;
            return false;
        case 7: // back to DOOM 3
            {
                // the music is synthesised as it plays (i_sound_openal.cpp)
                int songs = 0, underruns = 0, chunks = 0;
                double synthMs = 0.0;
                I_MusicStats(&songs, &underruns, &chunks, &synthMs);
#if defined(__vita__)
                V_Check(songs > 0 && underruns == 0 ? V_PASS : (songs > 0 ? V_WARN : V_FAIL), va("%s music", V_ClassicName()),
                    "%d songs this session, %d chunks synthesised as they played, %.1f ms of synthesis per second of music, %d underruns (gaps)", songs, chunks, synthMs, underruns);
#else
                V_Check(V_INFO, va("%s music", V_ClassicName()), "%d songs, %d chunks, %.1f ms of synthesis per second, %d underruns (0s: the PC's XAudio2 path plays whole songs)", songs, chunks, synthMs, underruns);
#endif
            }
            common->SwitchToGame(DOOM3_BFG);
            V_NextStep();
            return false;
        case 8:
            if (common->GetCurrentGame() != DOOM3_BFG || !Vita_VerifyShellActive()) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, va("%s exit", V_ClassicName()), "DOOM 3's menu did not come back in 60 s");
                    return true;
                }
                return false;
            }
            V_Check(V_PASS, va("%s exit", V_ClassicName()), "back in DOOM 3's menu %.1f s later", V_Since(v.stepUs));
            return true;
    }
    return true;
}


/*
---------------- netpeer ----------------
Friends over the network, with fake peers (other consoles' packets built by
the real code and handed to the real handlers, from loopback addresses where
nobody listens): a friend request, invites from a friend / a stranger / a
banned friend, sending invites, an invite through the real UDP socket and
back, a friend's game and a stranger's in the browser, joining the friend's
game, accepting an invite. The player's own friends are never written to.
*/
static struct {
    idStrList ids;
    idStrList names;
    idStrList hws;
    idList<netadr_t> addrs;
    int loop0;
    int friendIndex;
} vn;

static netadr_t V_Loopback(int port)
{
    netadr_t a;
    memset(&a, 0, sizeof(a));
    a.type = NA_IP;
    a.ip[0] = 127;
    a.ip[3] = (byte)(1 + (port & 0x3f)); // one loopback address each: the browser tells hosts apart by address
    a.port = (unsigned short)port;
    return a;
}

static void V_NetPeerCleanup()
{
    net_verifyLoopback = false;
    Session_VerifyBrowse(false);
    for (int i = 0; i < vn.ids.Num(); i++) {
        netFriends.Remove(vn.ids[i]);
        netOnline.UnbanLocally(vn.hws[i]);
    }
    if (common->Dialog().HasAnyActiveDialog()) {
        common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
    }
}

static void V_NetPeerStart()
{
    vn.ids.Clear();
    vn.names.Clear();
    vn.hws.Clear();
    vn.addrs.Clear();
    for (int i = 0; i < 5; i++) {
        vn.ids.Append(va("0000feed%08x", i));
        vn.names.Append(va("VerifyPeer%d", i));
        vn.hws.Append(va("feedface%08x", i));
        vn.addrs.Append(V_Loopback(27101 + i));
    }
    netFriends.Load();
    if (commonLocal.GetSpawnedMapName() != NULL && commonLocal.GetSpawnedMapName()[0]) {
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
    }
}

static bool V_NetPeerPoll()
{
    switch (v.step) {
        case 0: // the menus up
            if (!Vita_VerifyShellActive() || V_Since(v.stepUs) < 1.0) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, "network friends", "the main menu did not come up");
                    return true;
                }
                return false;
            }
            // into the multiplayer menus, as a player is when friends reach
            // them: the party lobby (online play only runs there)
            Vita_VerifyShellGoto(1);
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "net_hostParty\n");
            V_NextStep();
            return false;
        case 1:
            if (Session_VerifyState() != idSession::PARTY_LOBBY) {
                if (V_Since(v.stepUs) > 15.0) {
                    V_Check(V_FAIL, "multiplayer menus", "the party lobby did not open in 15 s (session state %d)", Session_VerifyState());
                    V_NetPeerCleanup();
                    return true;
                }
                return false;
            }
            if (V_Since(v.stepUs) < 1.5) {
                return false;
            }
            V_Check(V_PASS, "multiplayer menus", "party lobby open, hosting (%.1f s)", V_Since(v.stepUs));
            V_NextStep();
            return false;
        case 2: {
            if (common->Dialog().HasAnyActiveDialog()) {
                // the first visit's online notice (accepted on the player's console already)
                extern void Session_OnlineDisclaimerAnswer(bool accepted);
                common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                if (netOnline.NeedsDisclaimer()) {
                    Session_OnlineDisclaimerAnswer(true);
                }
            }
            // 0: a stranger who adds us - added back
            Session_VerifyFakeQuery(vn.ids[0], vn.names[0], vn.hws[0], vn.addrs[0], true, false);
            V_Check(netFriends.Find(vn.ids[0]) >= 0 ? V_PASS : V_FAIL, "friend request", "a console that added this one as a friend %s", netFriends.Find(vn.ids[0]) >= 0 ? "is added back" : "was NOT added back");
            netFriends.Add(vn.ids[1], vn.names[1], vn.addrs[1]);
            netFriends.Add(vn.ids[2], vn.names[2], vn.addrs[2]);
            // an invite from a friend, in the menus: the JOIN / DECLINE prompt
            int r0 = net_invitesReceived;
            Session_VerifyFakeQuery(vn.ids[0], vn.names[0], vn.hws[0], vn.addrs[0], false, true);
            const bool shown = common->Dialog().HasAnyActiveDialog();
            V_Check(net_invitesReceived == r0 + 1 && shown ? V_PASS : V_FAIL, "invite received", "a friend's invite %s, %s", net_invitesReceived == r0 + 1 ? "arrived" : "was LOST", shown ? "the JOIN / DECLINE prompt is up" : "NO prompt shown");
            V_NextStep();
            return false;
        }
        case 3: // the prompt drawn, then pictured
            if (V_Since(v.stepUs) < 0.6) {
                return false;
            }
            V_RequestShot("invite_prompt");
            V_NextStep();
            return false;
        case 4: {
            if (!V_ShotDone() && V_Since(v.stepUs) < 5.0) {
                return false;
            }
            common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL); // declined
            // a stranger's invite: ignored
            int r0 = net_invitesReceived;
            Session_VerifyFakeQuery(vn.ids[4], vn.names[4], vn.hws[4], vn.addrs[4], false, true);
            V_Check(net_invitesReceived == r0 && !common->Dialog().HasAnyActiveDialog() ? V_PASS : V_FAIL, "invite from a stranger", "%s", net_invitesReceived == r0 ? "ignored, as it should be" : "ACCEPTED from someone not on the friends list");
            netFriends.Remove(vn.ids[4]); // (a query from a stranger may note them as seen only)
            // a friend banned from my games: ignored
            netOnline.BanLocally(vn.hws[1], vn.names[1]);
            r0 = net_invitesReceived;
            Session_VerifyFakeQuery(vn.ids[1], vn.names[1], vn.hws[1], vn.addrs[1], false, true);
            V_Check(net_invitesReceived == r0 ? V_PASS : V_FAIL, "invite from a banned friend", "%s", net_invitesReceived == r0 ? "ignored" : "ACCEPTED from a banned console");
            netOnline.UnbanLocally(vn.hws[1]);
            if (common->Dialog().HasAnyActiveDialog()) {
                common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
            }
            // sending: to the fake friends only
            idStrList mine;
            mine.Append(vn.ids[0]);
            mine.Append(vn.ids[1]);
            mine.Append(vn.ids[2]);
            const int sent = Session_SendInvites(&mine);
            V_Check(sent == 3 ? V_PASS : V_FAIL, "invites sent", "%d of 3 friends invited", sent);
            // through the real socket: an invite to our own port comes back
            net_verifyLoopback = true;
            vn.loop0 = net_verifyLoopbackInvites;
            {
                netadr_t self = V_Loopback(cvarSystem->GetCVarInteger("net_port"));
                self.ip[3] = 1;
                Session_SendInviteTo(self);
            }
            V_NextStep();
            return false;
        }
        case 5:
            if (net_verifyLoopbackInvites == vn.loop0 && V_Since(v.stepUs) < 3.0) {
                return false;
            }
            V_Check(net_verifyLoopbackInvites > vn.loop0 ? V_PASS : V_FAIL, "invite over UDP", "an invite sent to this console's own port %s", net_verifyLoopbackInvites > vn.loop0 ? va("came back through the socket and decoded (%.0f ms)", V_Since(v.stepUs) * 1000.0) : "never came back (socket, port or encryption)");
            net_verifyLoopback = false;
            // the browser: a friend's game, a stranger's
            Session_VerifyBrowse(true);
            Session_VerifyFakeGame(vn.ids[1], vn.names[1], vn.hws[1], vn.addrs[1], 3, 8);
            Session_VerifyFakeGame(vn.ids[3], vn.names[3], vn.hws[3], vn.addrs[3], 2, 8);
            V_NextStep();
            return false;
        case 6: {
            if (V_Since(v.stepUs) < 0.3) {
                return false;
            }
            bool fr = false, joinable = false;
            int players = 0;
            vn.friendIndex = Session_VerifyFindServer(vn.names[1], fr, joinable, players);
            V_Check(vn.friendIndex >= 0 && fr && joinable && players == 3 ? V_PASS : V_FAIL, "friend's game listed", "%s", vn.friendIndex >= 0 ? va("in the browser: friend %s, joinable %s, %d players", fr ? "yes" : "NO", joinable ? "yes" : "NO", players) : "NOT in the browser");
            const int si = Session_VerifyFindServer(vn.names[3], fr, joinable, players);
            V_Check(si >= 0 && !fr ? V_PASS : V_FAIL, "stranger's game listed", "%s", si >= 0 ? va("in the browser, friend %s", fr ? "YES (wrong)" : "no") : "NOT in the browser");
            V_RequestShot("browser");
            if (vn.friendIndex >= 0) {
                session->ConnectToServer(vn.friendIndex);
            }
            V_NextStep();
            return false;
        }
        case 7: {
            V_ShotDone();
            const int st = Session_VerifyState();
            if (st != idSession::CONNECTING && V_Since(v.stepUs) < 3.0) {
                return false;
            }
            V_Check(st == idSession::CONNECTING ? V_PASS : V_FAIL, "join friend's game", "joining the friend's game %s", st == idSession::CONNECTING ? "started (connecting to its address)" : va("did not start (session state %d)", st));
            session->Cancel();
            V_NextStep();
            return false;
        }
        case 8:
            if (Session_VerifyState() == idSession::CONNECTING && V_Since(v.stepUs) < 10.0) {
                return false;
            }
            Session_JoinInvite(vn.addrs[0]); // an invite accepted
            V_NextStep();
            return false;
        case 9: {
            const int st = Session_VerifyState();
            if (st != idSession::CONNECTING && V_Since(v.stepUs) < 3.0) {
                return false;
            }
            V_Check(st == idSession::CONNECTING ? V_PASS : V_FAIL, "accept invite", "accepting an invite %s", st == idSession::CONNECTING ? "connects to the friend's game" : va("did nothing (session state %d)", st));
            session->Cancel();
            V_NextStep();
            return false;
        }
        case 10:
            if (Session_VerifyState() == idSession::CONNECTING && V_Since(v.stepUs) < 10.0) {
                return false;
            }
            V_NetPeerCleanup();
            session->Cancel(); // out of the party lobby
            {
                int left = 0;
                for (int i = 0; i < vn.ids.Num(); i++) {
                    left += netFriends.Find(vn.ids[i]) >= 0 ? 1 : 0;
                }
                V_Check(left == 0 ? V_PASS : V_FAIL, "network friends cleanup", "%d test friends left behind", left);
            }
            return true;
    }
    return true;
}


/*
---------------- mp <map> <dm|tourney|tdm|lms|ctf> <bots> <seconds> ----------------
A private match hosted from the multiplayer menus, as a player makes one:
the party lobby, the match created on the map and mode, started, bots added
until the lobby is as full as asked, then played - the local player aiming
at the nearest enemy and firing. The bots must join, move, shoot and score;
the scoreboard must name everyone; the match must be on. Then out to the menus.
*/
static struct {
    idStr map;
    idStr mode;
    int modeNum;
    int bots;
    float seconds;
    int mapIndex;
    uint64 startUs;
    int maxState;
    float lastLog;
    bool shotTaken;
} vmp;

static int V_ModeNum(const char* m)
{
    static const char* modes[] = { "dm", "tourney", "tdm", "lms", "ctf", NULL };
    for (int i = 0; modes[i] != NULL; i++) {
        if (!idStr::Icmp(m, modes[i])) {
            return i;
        }
    }
    return 0;
}

static void V_MPStart()
{
    vmp.map = v.args.Argv(1);
    vmp.mode = (v.args.Argc() > 2) ? v.args.Argv(2) : "dm";
    vmp.modeNum = V_ModeNum(vmp.mode);
    vmp.bots = (v.args.Argc() > 3) ? atoi(v.args.Argv(3)) : 3;
    vmp.seconds = (v.args.Argc() > 4) ? atof(v.args.Argv(4)) : 45.0f;
    vmp.mapIndex = -1;
    vmp.maxState = -1;
    vmp.lastLog = 0.0f;
    vmp.shotTaken = false;
    const idList<mpMap_t>& maps = common->GetMapList();
    idStr want = vmp.map;
    want.StripPath();
    for (int i = 0; i < maps.Num(); i++) {
        idStr f = maps[i].mapFile;
        f.StripPath();
        f.StripFileExtension();
        if (!f.Icmp(want)) {
            vmp.mapIndex = i;
        }
    }
    VitaBench_CloseSegment();
    VitaBench_SetMapName(vmp.map.c_str());
    if (commonLocal.GetSpawnedMapName() != NULL && commonLocal.GetSpawnedMapName()[0]) {
        cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
    }
}

static const char* V_MPName()
{
    return va("MP %s %s", vmp.map.c_str(), vmp.mode.c_str());
}

static bool V_MPPoll()
{
    switch (v.step) {
        case 0:
            if (vmp.mapIndex < 0) {
                idStr list;
                const idList<mpMap_t>& maps = common->GetMapList();
                for (int i = 0; i < maps.Num(); i++) {
                    list += va(" %s", maps[i].mapFile.c_str());
                }
                V_Check(V_FAIL, V_MPName(), "the map is not in the game's multiplayer map list:%s", list.c_str());
                return true;
            }
            if (!Vita_VerifyShellActive() || V_Since(v.stepUs) < 1.0) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, V_MPName(), "the main menu did not come up");
                    return true;
                }
                return false;
            }
            Vita_VerifyShellGoto(1);
            if (Session_VerifyState() != idSession::PARTY_LOBBY) {
                cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "net_hostParty\n");
            }
            V_NextStep();
            return false;
        case 1: // the party lobby, then the match
            if (Session_VerifyState() != idSession::PARTY_LOBBY) {
                if (V_Since(v.stepUs) > 15.0) {
                    V_Check(V_FAIL, V_MPName(), "the party lobby did not open (session state %d)", Session_VerifyState());
                    return true;
                }
                return false;
            }
            if (common->Dialog().HasAnyActiveDialog()) {
                extern void Session_OnlineDisclaimerAnswer(bool accepted);
                common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                if (netOnline.NeedsDisclaimer()) {
                    Session_OnlineDisclaimerAnswer(true);
                }
            }
            {
                idMatchParameters parms = idMatchParameters(session->GetPartyLobbyBase().GetMatchParms());
                parms.matchFlags = DefaultPartyFlags | DefaultPrivateGameFlags;
                parms.gameMap = vmp.mapIndex;
                parms.gameMode = vmp.modeNum;
                cvarSystem->MoveCVarsToDict(CVAR_SERVERINFO, parms.serverInfo);
                session->CreateMatch(parms);
            }
            V_NextStep();
            return false;
        case 2:
            if (Session_VerifyState() != idSession::GAME_LOBBY) {
                if (V_Since(v.stepUs) > 20.0) {
                    V_Check(V_FAIL, V_MPName(), "the game lobby did not open (session state %d)", Session_VerifyState());
                    session->Cancel();
                    return true;
                }
                return false;
            }
            session->StartMatch();
            vmp.startUs = V_Now();
            V_NextStep();
            return false;
        case 3: { // in the match
            const char* spawned = commonLocal.GetSpawnedMapName();
            if (Session_VerifyState() != idSession::INGAME || spawned == NULL || !spawned[0] || Vita_MPGameState() < 0) {
                if (V_Since(v.stepUs) > 240.0) {
                    V_Check(V_FAIL, V_MPName(), "the match did not start in 240 s (session state %d)", Session_VerifyState());
                    session->Cancel();
                    return true;
                }
                return false;
            }
            V_Check(V_PASS, va("%s start", V_MPName()), "hosting on %s, %.1f s after the start", spawned, V_Since(vmp.startUs));
            const int added = VitaBot_Add(vmp.bots);
            V_Check(added == vmp.bots ? V_PASS : V_FAIL, va("%s bots added", V_MPName()), "%d of %d bots took a lobby slot", added, vmp.bots);
            VitaBench_OpenSegment(va("%s: warm-up and countdown", V_MPName()));
            VitaBench_SetMeasuring(true);
            V_NextStep();
            return false;
        }
        case 4: // warm-up and the countdown (g_countDown), then the match is on
            if (Vita_MPGameState() < 3) {
                {
                    float y = 0.0f, pt = 0.0f;
                    Vita_MPAimAtNearest(y, pt); // (the local player joins in from spectating)
                }
                if (V_Since(v.stepUs) - vmp.lastLog >= 5.0) {
                    vmp.lastLog = (float)V_Since(v.stepUs);
                    vitaMPPlayer_t pl[16];
                    const int n = Vita_MPPlayers(pl, 16);
                    idStr line;
                    for (int i = 0; i < n; i++) {
                        line += va(" | %s%s: health %d%s%s", pl[i].name, pl[i].bot ? " (bot)" : "", pl[i].health, pl[i].spectating ? ", spectating" : "", pl[i].wantSpectate ? ", wants to spectate" : "");
                    }
                    int st = 0, sw = 0, ns = 0;
                    Vita_MPTimes(st, sw, ns);
                    VitaBench_Log("bench   %s waiting for the match: game state %d (next %d at server time %d, now %d; engine %d Hz, game frame %d, timescale %.2f, %d ms)%s", V_MPName(), Vita_MPGameState(), ns, sw, st, cvarSystem->GetCVarInteger("com_engineHz"), common->GetGameFrame(), cvarSystem->GetCVarFloat("timescale"), Sys_Milliseconds(), line.c_str());
                }
                if (V_Since(v.stepUs) > 120.0) {
                    V_Check(V_FAIL, va("%s match on", V_MPName()), "still in warm-up / countdown (state %d) after 120 s", Vita_MPGameState());
                    VitaBot_RemoveAll();
                    cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
                    v.step = 6;
                    v.stepUs = V_Now();
                }
                return false;
            }
            VitaBench_Log("bench   %s: the match is on, %.1f s after the bots joined", V_MPName(), V_Since(v.stepUs));
            vmp.lastLog = 0.0f;
            VitaBench_OpenSegment(va("%s: %d bots", V_MPName(), vmp.bots));
            VitaBench_SetMeasuring(true);
            V_NextStep();
            return false;
        case 5: { // play: the local player fights too (fire also respawns him)
            vmp.maxState = Max(vmp.maxState, Vita_MPGameState());
            float yaw = 0.0f, pitch = 0.0f;
            if (Vita_MPAimAtNearest(yaw, pitch)) {
                VitaBench_SetLook(true, yaw, pitch);
                VitaBench_SetInput(BUTTON_ATTACK, 0, 0, 0.0f);
            } else {
                VitaBench_SetLook(false, 0.0f, 0.0f);
                VitaBench_SetInput(((int)(V_Since(v.stepUs) * 2.0) & 1) ? BUTTON_ATTACK : 0, 0, 0, 0.05f);
            }
            if (V_Since(v.stepUs) - vmp.lastLog >= 5.0) {
                vmp.lastLog = (float)V_Since(v.stepUs);
                vitaMPPlayer_t pl[16];
                const int n = Vita_MPPlayers(pl, 16);
                idStr line;
                for (int i = 0; i < n; i++) {
                    line += va(" | %s%s: %d frags %d deaths, %.0f u moved, %d shots, health %d%s%s", pl[i].name, pl[i].bot ? " (bot)" : "", pl[i].frags, pl[i].deaths, pl[i].travelled, pl[i].shots, pl[i].health,
                        pl[i].spectating ? ", spectating" : "", pl[i].wantSpectate ? ", wants to spectate" : "");
                }
                int st = 0, sw = 0, ns = 0;
                Vita_MPTimes(st, sw, ns);
                VitaBench_Log("bench   %s %.0f s: game state %d (next %d at server time %d, now %d)%s", V_MPName(), vmp.lastLog, Vita_MPGameState(), ns, sw, st, line.c_str());
            }
            if (!vmp.shotTaken && V_Since(v.stepUs) >= vmp.seconds * 0.5 && v.shotState == 0) {
                vmp.shotTaken = true;
                V_RequestShot(va("mp_%s_%s", vmp.map.c_str(), vmp.mode.c_str()));
            }
            V_WriteShotIfReady();
            if (V_Since(v.stepUs) < vmp.seconds) {
                return false;
            }
            VitaBench_SetInput(0, 0, 0, 0.0f);
            VitaBench_SetLook(false, 0.0f, 0.0f);
            VitaBench_SetMeasuring(false);
            VitaBench_CloseSegment();
            vitaMPPlayer_t pl[16];
            const int n = Vita_MPPlayers(pl, 16);
            int bots = 0, moving = 0, shooting = 0, frags = 0, deaths = 0, named = 0;
            idStr list;
            for (int i = 0; i < n; i++) {
                bots += pl[i].bot ? 1 : 0;
                moving += (pl[i].bot && pl[i].travelled > 150.0f) ? 1 : 0;
                shooting += (pl[i].bot && pl[i].shots > 0) ? 1 : 0;
                frags += pl[i].frags;
                deaths += pl[i].deaths;
                named += pl[i].name[0] ? 1 : 0;
                list += va("%s%s%s %d/%d", i ? ", " : "", pl[i].name, pl[i].bot ? "(bot)" : "", pl[i].frags, pl[i].deaths);
            }
            V_Check(bots == vmp.bots ? V_PASS : V_FAIL, va("%s bots in game", V_MPName()), "%d players, %d of them bots (asked for %d)", n, bots, vmp.bots);
            V_Check(bots > 0 && moving == bots ? V_PASS : V_FAIL, va("%s bots move", V_MPName()), "%d of %d bots moved about", moving, bots);
            V_Check(bots > 0 && shooting > 0 ? V_PASS : V_WARN, va("%s bots fight", V_MPName()), "%d of %d bots fired at someone%s", shooting, bots, shooting == 0 ? " (they did not meet in time on this map)" : "");
            V_Check(frags > 0 || deaths > 0 ? V_PASS : V_WARN, va("%s kills", V_MPName()), "%d frags, %d deaths in %.0f s: %s", frags, deaths, vmp.seconds, list.c_str());
            V_Check(named == n ? V_PASS : V_FAIL, va("%s scoreboard", V_MPName()), "%d of %d players have a name", named, n);
            V_Check(vmp.maxState >= 3 ? V_PASS : V_WARN, va("%s match on", V_MPName()), "game state reached %d (3 = on: past warm-up and countdown)", vmp.maxState);
            VitaBot_RemoveAll();
            cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "disconnect\n");
            V_NextStep();
            return false;
        }
        case 6: // back in the menus, out of the lobby
            V_WriteShotIfReady();
            if (commonLocal.GetSpawnedMapName() != NULL && commonLocal.GetSpawnedMapName()[0]) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, va("%s leave", V_MPName()), "still in the match 60 s after leaving");
                    return true;
                }
                return false;
            }
            session->Cancel();
            V_NextStep();
            return false;
        case 7:
            if (!Vita_VerifyShellActive() || V_Since(v.stepUs) < 1.0) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, va("%s leave", V_MPName()), "the menus did not come back");
                    return true;
                }
                return false;
            }
            V_Check(V_PASS, va("%s leave", V_MPName()), "back in the menus (session state %d)", Session_VerifyState());
            return true;
    }
    return true;
}


/*
---------------- online [seconds] ----------------
Online play as a player meets it: into the multiplayer menus (the party
lobby), where the console goes online - the public BitTorrent DHT, the router
asked to forward the game port (NAT-PMP / UPnP), the internet's view of this
console - and finds other players. Judged after the given time.
*/
static struct {
    float seconds;
    float lastLog;
} von;

static bool V_OnlinePoll()
{
    switch (v.step) {
        case 0:
            if (!cvarSystem->GetCVarBool("net_online")) {
                V_Check(V_INFO, "online", "online play is switched off on this console (net_online 0) - not checked");
                return true;
            }
            if (!Vita_VerifyShellActive() || V_Since(v.stepUs) < 1.0) {
                if (V_Since(v.stepUs) > 60.0) {
                    V_Check(V_FAIL, "online", "the main menu did not come up");
                    return true;
                }
                return false;
            }
            Vita_VerifyShellGoto(1);
            if (Session_VerifyState() != idSession::PARTY_LOBBY) {
                cmdSystem->BufferCommandText(CMD_EXEC_APPEND, "net_hostParty\n");
            }
            V_NextStep();
            return false;
        case 1:
            if (Session_VerifyState() != idSession::PARTY_LOBBY) {
                if (V_Since(v.stepUs) > 15.0) {
                    V_Check(V_FAIL, "online", "the party lobby did not open (session state %d)", Session_VerifyState());
                    return true;
                }
                return false;
            }
            if (common->Dialog().HasAnyActiveDialog()) {
                extern void Session_OnlineDisclaimerAnswer(bool accepted);
                common->Dialog().ClearDialog(GDM_XBOX_DEPLOYMENT_TYPE_FAIL);
                if (netOnline.NeedsDisclaimer()) {
                    Session_OnlineDisclaimerAnswer(true);
                }
            }
            von.lastLog = 0.0f;
            V_NextStep();
            return false;
        case 2: {
            idNetOnline::verifyInfo_t info;
            netOnline.VerifyInfo(info);
            if (V_Since(v.stepUs) - von.lastLog >= 5.0) {
                von.lastLog = (float)V_Since(v.stepUs);
                VitaBench_Log("bench   online %.0f s: %s, DHT packets %d in / %d out, %d lookups, public address %s, router forwarding %s, NAT %s, %d players listed (%d reached)",
                    von.lastLog, info.active ? "on" : "off", info.packetsIn, info.packetsOut, info.lookupsDone, info.haveExternal ? info.external.c_str() : "unknown",
                    info.portMapExternal ? va("port %d", info.portMapExternal) : (info.portMapState == 1 ? "being asked" : "none"), info.nat.c_str(), info.peers, info.peersReached);
            }
            if (V_Since(v.stepUs) < von.seconds) {
                return false;
            }
            V_Check(info.active ? V_PASS : V_FAIL, "online on", "%s in the multiplayer menus", info.active ? "online" : "NOT online");
            V_Check(info.packetsIn > 0 ? V_PASS : V_FAIL, "DHT reached", "%d packets from the BitTorrent DHT (%d sent), %d lookups done", info.packetsIn, info.packetsOut, info.lookupsDone);
            V_Check(info.haveExternal ? V_PASS : V_WARN, "public address", "%s", info.haveExternal ? va("the internet sees this console at %s", info.external.c_str()) : "not learned from the DHT yet");
            V_Check(info.portMapExternal ? V_PASS : V_WARN, "router port forwarding", "%s", info.portMapExternal ? va("the router forwards public port %d to this console", info.portMapExternal) : (info.portMapState == 2 ? "the router refused or does not speak NAT-PMP / UPnP: others may not reach this console" : "no answer from the router yet"));
            V_Check(V_INFO, "NAT type", "%s", info.nat.c_str());
            V_Check(V_INFO, "players online", "%d players listed by the DHT, %d of them reached", info.peers, info.peersReached);
            session->Cancel(); // out of the party lobby
            return true;
        }
    }
    return true;
}

/*
---------------- memtrend ----------------
*/
static void V_MemTrend()
{
#if !defined(__vita__)
    V_Check(V_INFO, "memory", "the PC build has no memory budget to judge: %d menu visits, %d maps loaded", v.menuFreeKB.Num(), v.mapFreeKB.Num());
    return;
#endif
    if (v.menuFreeKB.Num() >= 2) {
        const int first = (int)v.menuFreeKB[0], last = (int)v.menuFreeKB[v.menuFreeKB.Num() - 1];
        int lowest = first;
        for (int i = 0; i < v.menuFreeKB.Num(); i++) {
            lowest = Min(lowest, (int)v.menuFreeKB[i]);
        }
        const int lostMB = (first - last) / 1024;
        V_Check(lostMB > 24 ? V_FAIL : (lostMB > 8 ? V_WARN : V_PASS), "memory at the menu", "free at the main menu: %d MB at the first visit, %d MB at the last (%d visits), lowest %d MB - %d MB not given back over the session",
            first / 1024, last / 1024, v.menuFreeKB.Num(), lowest / 1024, lostMB);
        const int c0 = (int)v.menuCachedKB[0], c1 = (int)v.menuCachedKB[v.menuCachedKB.Num() - 1];
        V_Check((c0 - c1) / 1024 > 16 ? V_WARN : V_PASS, "cached heap at the menu", "%d MB -> %d MB", c0 / 1024, c1 / 1024);
    } else {
        V_Check(V_INFO, "memory at the menu", "fewer than two visits to the menu to compare");
    }
    if (v.mapFreeKB.Num() > 0) {
        int lowest = 0x7fffffff, at = 0;
        for (int i = 0; i < v.mapFreeKB.Num(); i++) {
            if ((int)v.mapFreeKB[i] < lowest) {
                lowest = v.mapFreeKB[i];
                at = i;
            }
        }
        V_Check(lowest / 1024 < 16 ? V_FAIL : (lowest / 1024 < 32 ? V_WARN : V_PASS), "memory in maps", "lowest free right after a map load: %d MB (%s), over %d maps", lowest / 1024, v.mapNames[at].c_str(), v.mapFreeKB.Num());
    }
}

/*
---------------- boot ----------------
*/
static void V_Boot()
{
    unsigned int freeKB = 0, cachedKB = 0;
    V_Memory(freeKB, cachedKB);
    V_Check(V_INFO, "build", "%s %s; script line %d", __DATE__, __TIME__, v.lineNum);
    V_Check(v.errors == 0 ? V_PASS : V_FAIL, "errors so far", "%d errors since the game started%s%s", v.errors, v.errors ? ", the last: " : "", v.lastError.c_str());
    idStr all;
    for (int i = 0; i < v.recentWarnings.Num() && all.Length() < 1500; i++) {
        all += va("%s\"%s\"", i ? "; " : "", v.recentWarnings[i].c_str());
    }
    V_Check(v.warnings == 0 ? V_PASS : V_WARN, "warnings so far", "%d warnings since the game started%s%s", v.warnings, v.warnings ? ": " : "", all.c_str());
    V_Check(V_INFO, "memory", "free %u MB, cached heap %u MB", freeKB / 1024, cachedKB / 1024);
    idStrList fonts;
    Vita_RegisteredFontNames(fonts);
    V_Check(fonts.Num() > 0 ? V_PASS : V_FAIL, "fonts", "%d fonts loaded", fonts.Num());
}

/*
================================================================================

The script's commands

================================================================================
*/
int VitaVerify_Op(const idCmdArgs& args, int lineNum)
{
    const char* op = args.Argv(0);
    v.args = args;
    v.lineNum = lineNum;
    v.step = 0;
    v.stepUs = V_Now();
    v.opUs = V_Now();
    v.warnMark = v.warnings;
    if (!idStr::Icmp(op, "suite")) {
        v.suite = args.Args();
        VitaBench_Log("bench ######## suite: %s ########", v.suite.c_str());
        return 1;
    }
    if (!idStr::Icmp(op, "boot")) {
        V_Boot();
        return 1;
    }
    if (!idStr::Icmp(op, "mapcheck")) {
        V_MapCheckStart();
        v.op = VOP_MAPCHECK;
        return 2;
    }
    if (!idStr::Icmp(op, "shots")) {
        V_ShotsStart();
        v.op = VOP_SHOTS;
        return 2;
    }
    if (!idStr::Icmp(op, "shadowcheck")) {
        V_ShadowCheckStart();
        v.op = VOP_SHADOW;
        return 2;
    }
    if (!idStr::Icmp(op, "postcheck")) {
        V_PostCheckStart();
        v.op = VOP_POSTFILTER;
        return 2;
    }
    if (!idStr::Icmp(op, "shot")) {
        V_RequestShot(args.Argc() > 1 ? args.Argv(1) : "shot");
        v.op = VOP_SHOT;
        return 2;
    }
    if (!idStr::Icmp(op, "fightcheck")) {
        if (commonLocal.GetSpawnedMapName() == NULL || !commonLocal.GetSpawnedMapName()[0]) {
            V_Check(V_FAIL, "fight", "no map loaded");
            return 1;
        }
        V_FightStart();
        v.op = VOP_FIGHT;
        return 2;
    }
    if (!idStr::Icmp(op, "savecheck")) {
        if (commonLocal.GetSpawnedMapName() == NULL || !commonLocal.GetSpawnedMapName()[0]) {
            V_Check(V_FAIL, "save", "no map loaded");
            return 1;
        }
        v.op = VOP_SAVE;
        return 2;
    }
    if (!idStr::Icmp(op, "menus")) {
        V_MenusStart();
        v.op = VOP_MENUS;
        return 2;
    }
    if (!idStr::Icmp(op, "langcheck")) {
        V_LangCheck();
        return 1;
    }
    if (!idStr::Icmp(op, "trophies")) {
        V_TrophyCheck();
        return 1;
    }
    if (!idStr::Icmp(op, "videotest")) {
        V_VideoStart();
        v.op = VOP_VIDEO;
        return 2;
    }
    if (!idStr::Icmp(op, "friends")) {
        V_FriendsCheck(args.Argc() > 1 ? atoi(args.Argv(1)) : 300);
        return 1;
    }
    if (!idStr::Icmp(op, "online")) {
        von.seconds = (args.Argc() > 1) ? atof(args.Argv(1)) : 60.0f;
        v.op = VOP_ONLINE;
        return 2;
    }
    if (!idStr::Icmp(op, "mp")) {
        V_MPStart();
        v.op = VOP_MP;
        return 2;
    }
    if (!idStr::Icmp(op, "netpeer")) {
        V_NetPeerStart();
        v.op = VOP_NETPEER;
        return 2;
    }
    if (!idStr::Icmp(op, "classic")) {
        V_ClassicStart();
        v.op = VOP_CLASSIC;
        return 2;
    }
    if (!idStr::Icmp(op, "memtrend")) {
        V_MemTrend();
        return 1;
    }
    if (!idStr::Icmp(op, "openspot")) {
        idStr where;
        const float room = (args.Argc() > 1) ? atof(args.Argv(1)) : 500.0f;
        if (Vita_VerifyGoOpen(room, where)) {
            VitaBench_Log("bench   the player moved to an open spot: %s", where.c_str());
        } else {
            V_Check(V_INFO, va("%s open spot", commonLocal.GetCurrentMapName()), "staying where the map put him: %s", where.c_str());
        }
        return 1;
    }
    if (!idStr::Icmp(op, "note")) {
        VitaBench_Log("bench   note: %s", args.Args());
        return 1;
    }
    return 0;
}

bool VitaVerify_Poll(uint64 now)
{
    V_WriteShotIfReady();
    bool done = true;
    switch (v.op) {
        case VOP_MAPCHECK:
            done = V_MapCheckPoll();
            break;
        case VOP_SHOTS:
            done = V_ShotsPoll();
            break;
        case VOP_SHADOW:
            done = V_ShadowCheckPoll();
            break;
        case VOP_POSTFILTER:
            done = V_PostCheckPoll();
            break;
        case VOP_SHOT:
            done = V_ShotDone() || V_Since(v.stepUs) > 5.0;
            if (done && v.shotState != 0) {
                V_Check(V_FAIL, "screenshot", "%s: no frame captured in 5 s", v.shotName.c_str());
                v.shotState = 0;
            }
            break;
        case VOP_FIGHT:
            done = V_FightPoll();
            break;
        case VOP_SAVE:
            done = V_SavePoll();
            break;
        case VOP_MENUS:
            done = V_MenusPoll();
            break;
        case VOP_VIDEO:
            done = V_VideoPoll();
            break;
        case VOP_CLASSIC:
            done = V_ClassicPoll();
            break;
        case VOP_NETPEER:
            done = V_NetPeerPoll();
            break;
        case VOP_MP:
            done = V_MPPoll();
            break;
        case VOP_ONLINE:
            done = V_OnlinePoll();
            break;
        default:
            break;
    }
    if (done) {
        v.op = VOP_NONE;
    }
    return done;
}

/*
================================================================================

The report

================================================================================
*/
void VitaVerify_Report()
{
    if (VitaBench_VerifyMode()) {
        fileSystem->WriteFile(V_PROGRESS, "done\n", 5);
    }
    VitaBench_Log("bench ================================================================");
    VitaBench_Log("bench FULL VERIFICATION REPORT: %d checks - %d PASS, %d FAIL, %d WARN, %d INFO (%.0f minutes)", v.checks.Num(), v.counts[V_PASS], v.counts[V_FAIL], v.counts[V_WARN], v.counts[V_INFO], VitaBench_Seconds() / 60.0);
    // by suite
    idStrList suites;
    for (int i = 0; i < v.checks.Num(); i++) {
        suites.AddUnique(v.checks[i].suite);
    }
    for (int s = 0; s < suites.Num(); s++) {
        int c[V_NUM] = { 0, 0, 0, 0 };
        for (int i = 0; i < v.checks.Num(); i++) {
            if (v.checks[i].suite == suites[s]) {
                c[v.checks[i].result]++;
            }
        }
        VitaBench_Log("bench   %-40s %s: %d pass, %d fail, %d warn", suites[s].c_str(), c[V_FAIL] ? "FAIL" : (c[V_WARN] ? "warn" : "PASS"), c[V_PASS], c[V_FAIL], c[V_WARN]);
    }
    VitaBench_Log("bench failures:");
    for (int i = 0; i < v.checks.Num(); i++) {
        if (v.checks[i].result == V_FAIL) {
            VitaBench_Log("bench   FAIL [%s] %s: %s", v.checks[i].suite.c_str(), v.checks[i].name.c_str(), v.checks[i].detail.c_str());
        }
    }
    VitaBench_Log("bench warnings:");
    for (int i = 0; i < v.checks.Num(); i++) {
        if (v.checks[i].result == V_WARN) {
            VitaBench_Log("bench   WARN [%s] %s: %s", v.checks[i].suite.c_str(), v.checks[i].name.c_str(), v.checks[i].detail.c_str());
        }
    }
    VitaBench_Log("bench VERDICT full verification: %s", v.counts[V_FAIL] == 0 ? "PASS - everything checked works" : "FAIL - see the failures above");
    // the finish screen (VitaBench.cpp shows it)
    VitaVerify_BannerAdd(v.counts[V_FAIL] == 0 ? VB_GREEN : VB_RED, "Verification: %s - %d checks: %d pass, %d fail, %d warn, %d info",
        v.counts[V_FAIL] == 0 ? "PASS" : "FAIL", v.checks.Num(), v.counts[V_PASS], v.counts[V_FAIL], v.counts[V_WARN], v.counts[V_INFO]);
    for (int s = 0; s < suites.Num(); s++) {
        int c[V_NUM] = { 0, 0, 0, 0 };
        for (int i = 0; i < v.checks.Num(); i++) {
            if (v.checks[i].suite == suites[s]) {
                c[v.checks[i].result]++;
            }
        }
        VitaVerify_BannerAdd(c[V_FAIL] ? VB_RED : (c[V_WARN] ? VB_YELLOW : VB_GREEN), "  %-22s %s  (%d pass, %d fail, %d warn)", suites[s].c_str(),
            c[V_FAIL] ? "FAIL" : (c[V_WARN] ? "warn" : "PASS"), c[V_PASS], c[V_FAIL], c[V_WARN]);
    }
    Mem_Free(v.shotRGB);
    v.shotRGB = NULL;
}
