/*
===========================================================================

Doom 3 BFG Edition GPL Source Code - PS Vita port: trophies.

The game's achievements are the PS3 edition's trophies (achievement_t ids
0-65 are the trophy ids of the PS3 set, NPWR03554_00), so the Vita gets that
set - its names, descriptions in six languages, grades and icons - as a
trophy pack of its own (tools/vita/make_trophy_pack.py, from the user's PS3
disc), unlocked through the system's trophy service (sceNpTrophy): the
Vita's own "trophy earned" notification, its Trophies application, the
platinum when all 65 are earned.

The way Rinnegatamante's ports do it (Ghostship, Super Mario 64): the pack
under sce_sys/trophy/DBFG00001_00/TROPHY.TRP in the VPK, the NoTrpDrm plugin
on the console (ur0:tai/NoTrpDrm.suprx under *main) to waive the checks of
the application's NP communication signature and of the pack's signature,
the setup dialog once at start-up, and the unlocks on a thread of their own
(sceNpTrophyUnlockTrophy waits for the notification). Without the plugin
the context is refused: the game runs without trophies, and says why in
console.txt.

A trophy is never unlocked by the benchmark or the full verification (L+R):
while either runs, unlock requests are recorded and logged instead (the
verification checks them) - the Vita has no way to lock a trophy again.

===========================================================================
*/

#include "precompiled.h"
#pragma hdrstop

#if defined(__vita__)

#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/appmgr.h> // sceAppMgrAppParamGetString (the title ID = the communication ID)
#include <psp2/kernel/modulemgr.h> // sceKernelGetSystemSwVersion
#include <psp2/common_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitaGL.h>

#include "vita_public.h"
#include "../sys_session_local.h"

// sceNpTrophy has no header in the VitaSDK: the calls as Ghostship uses them
// (src/port/mods/achievements/trophies.c) - the ones proven on hardware
extern "C" {
typedef struct {
    int sdkVersion;
    SceCommonDialogParam commonParam;
    int context;
    int options;
    uint8_t reserved[128];
} SceNpTrophySetupDialogParam;

typedef struct {
    uint32_t bits[4]; // a bit per trophy id
} SceNpTrophyUnlockState;

int sceNpTrophyInit(void* unk);
int sceNpTrophyCreateContext(int* context, char* commId, char* commSign, uint64_t options);
int sceNpTrophySetupDialogInit(SceNpTrophySetupDialogParam* param);
SceCommonDialogStatus sceNpTrophySetupDialogGetStatus();
int sceNpTrophySetupDialogTerm();
int sceNpTrophyCreateHandle(int* handle);
int sceNpTrophyDestroyHandle(int handle);
int sceNpTrophyUnlockTrophy(int ctx, int handle, int id, int* platId);
int sceNpTrophyGetTrophyUnlockState(int ctx, int handle, SceNpTrophyUnlockState* state, uint32_t* count);
}

#ifndef PSP2_SDK_VERSION
#define PSP2_SDK_VERSION 0x03570011
#endif

const char* Vita_TrophyWhy();
const char* Vita_FirmwareString();
static const char* VITA_TROPHY_COMM_ID = "DBFG00001"; // sce_sys/trophy/DBFG00001_00 (make_trophy_pack.py)
#define VITA_NUM_TROPHIES 66

struct vitaTrophy_t {
    int id;
    char grade; // P G S B
    bool hidden;
    const char* name;
};
static const vitaTrophy_t vitaTrophies[] = {
#include "vita_trophy_list.inc"
};

static struct {
    bool available;
    int initError;       // what refused the trophies (0: they are up)
    int ctx;
    uint32_t count;      // trophies in the pack, as the system reports
    SceNpTrophyUnlockState state;
    SceUID sema;
    SceUID lock;
    int queue[128];
    volatile unsigned int head, tail;
    volatile int unlockedThisRun;
    volatile int failedThisRun;
    // the benchmark / verification: requests recorded, not sent
    bool dryRun;
    bool dryHit[128];
    int dryHits;
} vt;

const char* Vita_TrophyName(int id, char* grade)
{
    for (int i = 0; i < (int)(sizeof(vitaTrophies) / sizeof(vitaTrophies[0])); i++) {
        if (vitaTrophies[i].id == id) {
            if (grade != NULL) {
                *grade = vitaTrophies[i].grade;
            }
            return vitaTrophies[i].name;
        }
    }
    if (grade != NULL) {
        *grade = '?';
    }
    return "?";
}

static bool Vita_TrophyBit(int id)
{
    return id >= 0 && id < 128 && (vt.state.bits[id >> 5] & (1u << (id & 31))) != 0;
}

static const char* Vita_GradeName(char g)
{
    return g == 'P' ? "platinum" : g == 'G' ? "gold" : g == 'S' ? "silver" : "bronze";
}

static int Vita_TrophyThread(SceSize, void*)
{
    for (;;) {
        sceKernelWaitSema(vt.sema, 1, NULL);
        sceKernelLockMutex(vt.lock, 1, NULL);
        const int id = vt.queue[vt.tail % 128];
        vt.tail++;
        sceKernelUnlockMutex(vt.lock, 1);
        int handle = -1;
        int plat = -1;
        const uint64 t0 = Sys_Microseconds();
        int err = sceNpTrophyCreateHandle(&handle);
        if (err >= 0) {
            err = sceNpTrophyUnlockTrophy(vt.ctx, handle, id, &plat);
            sceNpTrophyDestroyHandle(handle);
        }
        char grade;
        const char* name = Vita_TrophyName(id, &grade);
        if (err >= 0) {
            vt.unlockedThisRun++;
            Vita_Checkpoint("trophy: EARNED #%d \"%s\" (%s) in %.0f ms%s", id, name, Vita_GradeName(grade), (Sys_Microseconds() - t0) / 1000.0,
                plat >= 0 ? va(" - and the platinum (#%d \"%s\")", plat, Vita_TrophyName(plat, NULL)) : "");
            if (plat >= 0 && plat < 128) {
                vt.state.bits[plat >> 5] |= 1u << (plat & 31);
            }
        } else {
            vt.failedThisRun++;
            Vita_Checkpoint("trophy: unlocking #%d \"%s\" FAILED (0x%08X)", id, name, (unsigned int)err);
        }
    }
    return 0;
}

/*
========================
Vita_TrophyStart

On the GL thread at start-up, after the renderer is up: the trophy
service, the pack's context and its setup dialog (the system shows it over
the screen the first time; the frame is swapped meanwhile, as Ghostship
does), the state of the trophies and the unlock thread.
========================
*/
void Vita_TrophyStart()
{
    const uint64 t0 = Sys_Microseconds();
    Vita_Checkpoint("trophy: reported system software %s (a spoofed version shows here; NoTrpDrm, which the homebrew pack needs, knows the real 3.60, 3.65, 3.67 and 3.68 SceShell)", Vita_FirmwareString());
    vt.initError = sceSysmoduleLoadModule(SCE_SYSMODULE_NP_TROPHY);
    if (vt.initError < 0) {
        Vita_Checkpoint("trophy: the trophy module did not load (0x%08X) - no trophies this run", (unsigned int)vt.initError);
        return;
    }
    sceNpTrophyInit(NULL);
    // The communication ID is the app's own title ID, read back from its
    // param.sfo, and the pack sits in sce_sys/trophy/<title ID>_00 - how
    // Rinnegatamante's ports (NoTrpDrm's author) do it. r55 used DBFG00001
    // with the title ID DOOM3BFG0 and was refused; the title ID is now
    // DBFG00001 (CMakeLists.txt).
    char commId[16];
    memset(commId, 0, sizeof(commId));
    const int appParam = sceAppMgrAppParamGetString(0, 12, commId, sizeof(commId));
    if (appParam < 0 || commId[0] == '\0') {
        idStr::Copynz(commId, VITA_TROPHY_COMM_ID, sizeof(commId));
    }
    Vita_Checkpoint("trophy: communication ID %s (%s), pack sce_sys/trophy/%s_00/TROPHY.TRP", commId,
        appParam < 0 ? va("title ID not read, 0x%08X - the built-in one", (unsigned int)appParam) : "the app's title ID", commId);
    // an NP communication signature of the form the service expects; NoTrpDrm
    // waives its check (there is no real one for a homebrew set)
    static char signature[160] = { (char)0xb9, (char)0xdd, (char)0xe1, (char)0x3b, (char)0x01, (char)0x00 };
    vt.initError = sceNpTrophyCreateContext(&vt.ctx, commId, signature, 0);
    if (vt.initError < 0) {
        Vita_Checkpoint("trophy: sceNpTrophyCreateContext(%s) refused (0x%08X) - no trophies this run. %s",
            commId, (unsigned int)vt.initError, Vita_TrophyWhy());
        return;
    }
    SceNpTrophySetupDialogParam setup;
    memset(&setup, 0, sizeof(setup));
    _sceCommonDialogSetMagicNumber(&setup.commonParam);
    setup.sdkVersion = PSP2_SDK_VERSION;
    setup.options = 0;
    setup.context = vt.ctx;
    const int dlg = sceNpTrophySetupDialogInit(&setup);
    int frames = 0;
    if (dlg >= 0) {
        while (sceNpTrophySetupDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
            frames++;
        }
        sceNpTrophySetupDialogTerm();
    }
    vt.sema = sceKernelCreateSema("d3bfg_trophy_requests", 0, 0, 128, NULL);
    vt.lock = sceKernelCreateMutex("d3bfg_trophy_queue", 0, 0, NULL);
    const SceUID thread = sceKernelCreateThread("d3bfg_trophies", Vita_TrophyThread, 0x10000100 + 20, 0x10000, 0,
        Vita_Core3Available() ? SCE_KERNEL_CPU_MASK_SYSTEM : 0, NULL);
    if (vt.sema < 0 || vt.lock < 0 || thread < 0) {
        vt.initError = (thread < 0) ? thread : -1;
        Vita_Checkpoint("trophy: no unlock thread (0x%08X) - no trophies this run", (unsigned int)thread);
        return;
    }
    sceKernelStartThread(thread, 0, NULL);
    Vita_RegisterThread(thread);
    Vita_NameThread(thread, "d3bfg_trophies (trophy unlocks)", Vita_Core3Available() ? 3 : -1);

    int handle = -1;
    memset(&vt.state, 0, sizeof(vt.state));
    if (sceNpTrophyCreateHandle(&handle) >= 0) {
        sceNpTrophyGetTrophyUnlockState(vt.ctx, handle, &vt.state, &vt.count);
        sceNpTrophyDestroyHandle(handle);
    }
    int earned = 0;
    for (int i = 0; i < 128; i++) {
        earned += Vita_TrophyBit(i) ? 1 : 0;
    }
    vt.available = true;
    // the profile loaded before the trophies were up: its bits from the system's
    idLocalUser* user = session->GetSignInManager().GetMasterLocalUser();
    if (user != NULL && user->GetProfile() != NULL) {
        session->GetAchievementSystem().SyncAchievementBits(user);
    }
    Vita_Checkpoint("trophy: up - set %s_00, %u trophies in the pack (%d expected), %d earned so far; setup dialog %s (%d frames), %.0f ms",
        commId, (unsigned int)vt.count, VITA_NUM_TROPHIES, earned, dlg >= 0 ? "done" : va("refused 0x%08X", (unsigned int)dlg), frames,
        (Sys_Microseconds() - t0) / 1000.0);
}

// the firmware as the system reports it (a spoofed version shows as spoofed)
const char* Vita_FirmwareString()
{
    static char text[40] = "";
    if (text[0] == '\0') {
        SceKernelSystemSwVersion v;
        memset(&v, 0, sizeof(v));
        v.size = sizeof(v);
        if (sceKernelGetSystemSwVersion(&v) >= 0) {
            idStr::snPrintf(text, sizeof(text), "%s (0x%08X)", v.versionString, (unsigned int)v.version);
        } else {
            idStr::Copynz(text, "unknown", sizeof(text));
        }
    }
    return text;
}

/*
========================
Vita_TrophyPluginConfig

Where NoTrpDrm should be: ur0:tai is the place to install plugins (the port's
instructions), ux0:tai is looked at too. For both config.txt files: whether
it exists, whether it lists NoTrpDrm, under which section (it must be *main -
SceShell), and whether the file that line names is there; and whether
NoTrpDrm.suprx sits in either folder. (taiHEN reads ux0:tai/config.txt
instead of ur0's when that file exists - said when it applies.)
========================
*/
static void Vita_TrophyConfigOne(const char* path, idStr& out)
{
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        out += va("%s: none", path);
        return;
    }
    const int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    char* text = (char*)Mem_ClearedAlloc(Max(size, 0) + 1, TAG_TEMP);
    if (text != NULL && size > 0) {
        sceIoRead(fd, text, size);
    }
    sceIoClose(fd);
    if (text == NULL) {
        out += va("%s: unreadable", path);
        return;
    }
    // line by line: the section in force, and the NoTrpDrm line
    idStr section = "(none)";
    idStr found, foundSection;
    for (char* line = text; line != NULL && *line != '\0';) {
        char* next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        idStr l = line;
        l.StripTrailingWhitespace();
        l.StripLeading(' ');
        l.StripLeading('\t');
        if (l.Length() > 0 && l[0] == '*') {
            section = l;
        } else if (l.Length() > 0 && l[0] != '#' && l.Find("notrpdrm", false) >= 0 && found.IsEmpty()) {
            found = l;
            foundSection = section;
        }
        line = next;
    }
    Mem_Free(text);
    if (found.IsEmpty()) {
        out += va("%s: does NOT list NoTrpDrm", path);
        return;
    }
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    const bool exists = sceIoGetstat(found.c_str(), &st) >= 0;
    out += va("%s: \"%s\" under %s%s, that file %s", path, found.c_str(), foundSection.c_str(),
        foundSection.Icmp("*main") == 0 ? "" : " (it must be under *main)", exists ? "is there" : "is MISSING");
}

static void Vita_TrophyPluginConfig(char* out, int outSize)
{
    idStr text;
    Vita_TrophyConfigOne("ur0:tai/config.txt", text);
    text += " | ";
    Vita_TrophyConfigOne("ux0:tai/config.txt", text);
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    const bool inUr0 = sceIoGetstat("ur0:tai/NoTrpDrm.suprx", &st) >= 0;
    memset(&st, 0, sizeof(st));
    const bool inUx0 = sceIoGetstat("ux0:tai/NoTrpDrm.suprx", &st) >= 0;
    memset(&st, 0, sizeof(st));
    const bool ux0Config = sceIoGetstat("ux0:tai/config.txt", &st) >= 0;
    text += va(" | NoTrpDrm.suprx: ur0:tai %s, ux0:tai %s", inUr0 ? "yes" : "no", inUx0 ? "yes" : "no");
    if (ux0Config) {
        text += " | note: taiHEN reads ux0:tai/config.txt instead of ur0:tai/config.txt while it exists";
    }
    idStr::Copynz(out, text.c_str(), outSize);
}

// why the trophy system is not up, in words (the SceNpTrophy error names
// are Vita3K's table, modules/SceNpTrophy)
const char* Vita_TrophyWhy()
{
    static char text[1024];
    const unsigned int e = (unsigned int)vt.initError;
    const char* fw = Vita_FirmwareString();
    if (e == 0x8055160Cu) {
        // NoTrpDrm (Rinnegatamante) patches SceShell for the 3.60, 3.65, 3.67
        // and 3.68 builds of SceShell (it tells them apart by the module, so
        // a spoofed version number does not matter); when it runs, this check
        // never fails
        char config[640];
        Vita_TrophyPluginConfig(config, sizeof(config));
        idStr::snPrintf(text, sizeof(text), "NPCOMMSIGN_VERIFICATION_FAILURE: the system checked the pack's signature, so NoTrpDrm is not active in SceShell. "
                        "Install it as ur0:tai/NoTrpDrm.suprx, list that path under *main in ur0:tai/config.txt and reboot (real firmware 3.60, 3.65, 3.67 or 3.68; reported version %s, possibly spoofed) - %s", fw, config);
    } else if (e == 0x80551617u || e == 0x8055161Au || e == 0x8055161Bu) {
        idStr::snPrintf(text, sizeof(text), "the trophy pack was refused (TRP verification / format, 0x%08X): NoTrpDrm waives the TRP signature too - firmware %s", e, fw);
    } else if (e == 0x80551619u) {
        idStr::snPrintf(text, sizeof(text), "TRP_FILE_NOT_FOUND: sce_sys/trophy/%s_00/TROPHY.TRP is missing from the installed app - firmware %s", VITA_TROPHY_COMM_ID, fw);
    } else if (e == 0x8055160Au) {
        idStr::snPrintf(text, sizeof(text), "INVALID_NPCOMMID: the ID must be 4 capital letters + 5 digits - firmware %s", fw);
    } else {
        idStr::snPrintf(text, sizeof(text), "error 0x%08X - the Vita takes a homebrew trophy pack only with the NoTrpDrm plugin (ur0:tai/NoTrpDrm.suprx under *main in ur0:tai/config.txt; real firmware 3.60/3.65/3.67/3.68) - reported version %s", e, fw);
    }
    return text;
}

// the verification: what the trophy system looks like
bool Vita_TrophyStatus(int& count, int& earned, int& initError)
{
    count = (int)vt.count;
    initError = vt.initError;
    earned = 0;
    for (int i = 0; i < 128; i++) {
        earned += Vita_TrophyBit(i) ? 1 : 0;
    }
    return vt.available;
}

// while the benchmark / verification runs: record, do not unlock
void Vita_TrophyDryRun(bool on)
{
    if (on != vt.dryRun) {
        Vita_Checkpoint("trophy: %s", on ? "dry run - unlock requests are recorded, nothing is unlocked (the benchmark / verification)" : "dry run over - trophies unlock again");
    }
    vt.dryRun = on;
    if (on) {
        memset(vt.dryHit, 0, sizeof(vt.dryHit));
        vt.dryHits = 0;
    }
}

bool Vita_TrophyDryRunHit(int id)
{
    return id >= 0 && id < 128 && vt.dryHit[id];
}

/*
================================================
idAchievementSystemVita
================================================
*/
class idAchievementSystemVita : public idAchievementSystem {
public:
    bool IsInitialized()
    {
        return vt.available;
    }
    void AchievementUnlock(idLocalUser* user, const int achievementID);
    void AchievementLock(idLocalUser* user, const int achievementID);
    void AchievementLockAll(idLocalUser* user, const int maxId);
    void Pump() { }
    bool GetAchievementDescription(idLocalUser* user, const int id, achievementDescription_t& data) const;
    bool GetAchievementState(idLocalUser* user, idArray<bool, idAchievementSystem::MAX_ACHIEVEMENTS>& achievements) const;
};

void idAchievementSystemVita::AchievementUnlock(idLocalUser* user, const int id)
{
    if (id < 0 || id >= VITA_NUM_TROPHIES) {
        return;
    }
    char grade;
    const char* name = Vita_TrophyName(id, &grade);
    if (vt.dryRun) {
        if (!vt.dryHit[id]) {
            vt.dryHit[id] = true;
            vt.dryHits++;
        }
        common->Printf("trophy: #%d \"%s\" (%s) would be earned here - dry run, not sent\n", id, name, Vita_GradeName(grade));
        return;
    }
    // the profile's bit: the game asks for an achievement only once
    if (user != NULL && user->GetProfile() != NULL) {
        user->GetProfile()->SetAchievement(id);
    }
    if (id == 0) {
        return; // the platinum: the system gives it with the last of the others
    }
    if (!vt.available) {
        common->Printf("trophy: #%d \"%s\" earned, but the trophy system is not up (0x%08X)\n", id, name, (unsigned int)vt.initError);
        return;
    }
    if (Vita_TrophyBit(id)) {
        return;
    }
    vt.state.bits[id >> 5] |= 1u << (id & 31);
    sceKernelLockMutex(vt.lock, 1, NULL);
    vt.queue[vt.head % 128] = id;
    vt.head++;
    sceKernelUnlockMutex(vt.lock, 1);
    sceKernelSignalSema(vt.sema, 1);
    common->Printf("trophy: #%d \"%s\" (%s) earned - unlocking\n", id, name, Vita_GradeName(grade));
}

void idAchievementSystemVita::AchievementLock(idLocalUser* user, const int id)
{
    common->Printf("trophy: #%d stays earned - the Vita cannot lock a trophy again\n", id);
}

void idAchievementSystemVita::AchievementLockAll(idLocalUser* user, const int maxId)
{
    common->Printf("trophy: the trophies stay earned - the Vita cannot lock a trophy again\n");
}

bool idAchievementSystemVita::GetAchievementDescription(idLocalUser* user, const int id, achievementDescription_t& data) const
{
    if (id < 0 || id >= VITA_NUM_TROPHIES) {
        return false;
    }
    for (int i = 0; i < (int)(sizeof(vitaTrophies) / sizeof(vitaTrophies[0])); i++) {
        if (vitaTrophies[i].id == id) {
            idStr::Copynz(data.name, vitaTrophies[i].name, sizeof(data.name));
            data.description[0] = '\0';
            data.hidden = vitaTrophies[i].hidden;
            return true;
        }
    }
    return false;
}

bool idAchievementSystemVita::GetAchievementState(idLocalUser* user, idArray<bool, idAchievementSystem::MAX_ACHIEVEMENTS>& achievements) const
{
    if (!vt.available) {
        return false;
    }
    for (int i = 0; i < achievements.Num(); i++) {
        achievements[i] = Vita_TrophyBit(i);
    }
    return true;
}

idAchievementSystem* Vita_CreateAchievementSystem()
{
    return new (TAG_SYSTEM) idAchievementSystemVita();
}

#endif // __vita__
