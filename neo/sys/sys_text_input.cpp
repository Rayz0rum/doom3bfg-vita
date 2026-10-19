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
#pragma hdrstop
#include "precompiled.h"
#include "sys_net_friends.h"

#if defined(__vita__)
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#endif

// The system's user name and the on-screen keyboard (Vita): used by the
// multiplayer menus (the username, "Add Friend by Address"), online or not.

/*
========================
Sys_UserName
========================
*/
const char* Sys_UserName()
{
#if defined(__vita__)
    static char name[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE * 2] = { 0 };
    static bool tried = false;
    if (!tried) {
        tried = true;
        SceAppUtilInitParam init;
        SceAppUtilBootParam boot;
        memset(&init, 0, sizeof(init));
        memset(&boot, 0, sizeof(boot));
        const int initErr = sceAppUtilInit(&init, &boot); // fails harmlessly when already done
        const int err = sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8*)name, sizeof(name) - 1);
        if (err < 0) {
            name[0] = '\0';
        }
        idLib::Printf("net: system user name '%s' (sceAppUtilInit 0x%08X, get 0x%08X)\n", name, (unsigned int)initErr, (unsigned int)err);
    }
    return name;
#else
    return "";
#endif
}

/*
========================
Sys_TextInputStart / Sys_TextInputPoll
========================
*/
#if defined(__vita__)
static bool imeRunning = false;
static SceWChar16 imeTitle[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
// console commands can be long (net_globalBan <hash>, a map name and cvars)
#define VITA_IME_MAX 128
static SceWChar16 imeInitial[VITA_IME_MAX + 1];
static SceWChar16 imeText[VITA_IME_MAX + 1];

static void ToUTF16(const char* s, SceWChar16* out, int max)
{
    int n = 0;
    int i = 0;
    const int len = idStr::Length(s);
    while (i < len && n < max - 1) {
        uint32 c = idStr::UTF8Char(s, i);
        out[n++] = (SceWChar16)(c < 0x10000 ? c : '?');
    }
    out[n] = 0;
}
#endif

bool Sys_TextInputStart(const char* title, const char* initial)
{
#if defined(__vita__)
    if (imeRunning) {
        return true;
    }
    ToUTF16(title, imeTitle, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    ToUTF16(initial, imeInitial, VITA_IME_MAX + 1);
    memset(imeText, 0, sizeof(imeText));
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_FALSE;
    param.type = SCE_IME_TYPE_BASIC_LATIN;
    param.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    param.title = imeTitle;
    param.maxTextLength = VITA_IME_MAX;
    param.initialText = imeInitial;
    param.inputTextBuffer = imeText;
    const int err = sceImeDialogInit(&param);
    idLib::Printf("net: on-screen keyboard for '%s' (sceImeDialogInit 0x%08X)\n", title, (unsigned int)err);
    imeRunning = (err >= 0);
    return imeRunning;
#else
    return false;
#endif
}

bool Sys_TextInputPoll(idStr& text, bool* accepted)
{
    if (accepted != NULL) {
        *accepted = false;
    }
#if defined(__vita__)
    if (!imeRunning) {
        text.Clear();
        return true;
    }
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        return false;
    }
    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    text.Clear();
    const bool enter = (result.button == SCE_IME_DIALOG_BUTTON_ENTER);
    if (enter) {
        for (int i = 0; imeText[i] != 0 && i < VITA_IME_MAX; i++) {
            text.AppendUTF8Char(imeText[i]);
        }
    }
    if (accepted != NULL) {
        *accepted = enter;
    }
    sceImeDialogTerm();
    imeRunning = false;
    idLib::Printf("net: on-screen keyboard closed (%s): '%s'\n", enter ? "Enter" : "cancelled", text.c_str());
    return true;
#else
    text.Clear();
    return true;
#endif
}
