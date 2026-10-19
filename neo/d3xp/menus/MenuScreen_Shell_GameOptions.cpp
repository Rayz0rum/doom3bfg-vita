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

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"
#include "../Game_local.h"

const static int NUM_GAME_OPTIONS_OPTIONS = 8;

const float MIN_FOV = 80.0f;
const float MAX_FOV = 100.0f;

const float MIN_FOV_GUN = 3.0f;
const float MAX_FOV_GUN = 0.0f;

/*
========================
idMenuScreen_Shell_GameOptions::Initialize
========================
*/
void idMenuScreen_Shell_GameOptions::Initialize(idMenuHandler* data)
{
    idMenuScreen::Initialize(data);

    if (data != NULL) {
        menuGUI = data->GetGUI();
    }

    SetSpritePath("menuGameOptions");

    options = new (TAG_SWF) idMenuWidget_DynamicList();
    options->SetNumVisibleOptions(NUM_GAME_OPTIONS_OPTIONS);
    options->SetSpritePath(GetSpritePath(), "info", "options");
    options->SetWrappingAllowed(true);
    options->SetControlList(true);
    options->Initialize(data);
    AddChild(options);

    btnBack = new (TAG_SWF) idMenuWidget_Button();
    btnBack->Initialize(data);
    btnBack->SetLabel("#str_swf_settings");
    btnBack->SetSpritePath(GetSpritePath(), "info", "btnBack");
    btnBack->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_GO_BACK);
    AddChild(btnBack);

    idMenuWidget_ControlButton* control;

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TEXT);
    control->SetLabel("#str_swf_fov");
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_FOV);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_swf_checkpoints");
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_CHECKPOINTS);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_02135"); // Auto Weapon Switch
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_AUTO_SWITCH);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_02134"); // Auto Weapon Reload
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_AUTO_RELOAD);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_swf_aim_assist"); // Aim Assist
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_AIM_ASSIST);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_04102"); // Always Run
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_ALWAYS_SPRINT);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

#if defined(__vita__)
    // Flashlight Shadows is not shown (held on, Vita_ApplyFixedSettings);
    // Language takes its row
    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TEXT);
    control->SetLabel("#str_vita_language");
    control->SetDescription("#str_vita_language_desc");
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_LANGUAGE);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);
#else
    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("#str_swf_flashlight_shadows");
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_FLASHLIGHT_SHADOWS);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);
#endif

    // online play on / off: off, multiplayer is the local network (and
    // friends added by address) only
    control = new (TAG_SWF) idMenuWidget_ControlButton();
    control->SetOptionType(OPTION_SLIDER_TOGGLE);
    control->SetLabel("Online Play");
    control->SetDataSource(&systemData, idMenuDataSource_GameSettings::GAME_FIELD_ONLINE);
    control->SetupEvents(DEFAULT_REPEAT_TIME, options->GetChildren().Num());
    control->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_PRESS_FOCUSED, options->GetChildren().Num());
    options->AddChild(control);

    options->AddEventAction(WIDGET_EVENT_SCROLL_DOWN).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_SCROLL_DOWN_START_REPEATER, WIDGET_EVENT_SCROLL_DOWN));
    options->AddEventAction(WIDGET_EVENT_SCROLL_UP).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_SCROLL_UP_START_REPEATER, WIDGET_EVENT_SCROLL_UP));
    options->AddEventAction(WIDGET_EVENT_SCROLL_DOWN_RELEASE).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_STOP_REPEATER, WIDGET_EVENT_SCROLL_DOWN_RELEASE));
    options->AddEventAction(WIDGET_EVENT_SCROLL_UP_RELEASE).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_STOP_REPEATER, WIDGET_EVENT_SCROLL_UP_RELEASE));
    options->AddEventAction(WIDGET_EVENT_SCROLL_DOWN_LSTICK).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_SCROLL_DOWN_START_REPEATER, WIDGET_EVENT_SCROLL_DOWN_LSTICK));
    options->AddEventAction(WIDGET_EVENT_SCROLL_UP_LSTICK).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_SCROLL_UP_START_REPEATER, WIDGET_EVENT_SCROLL_UP_LSTICK));
    options->AddEventAction(WIDGET_EVENT_SCROLL_DOWN_LSTICK_RELEASE).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_STOP_REPEATER, WIDGET_EVENT_SCROLL_DOWN_LSTICK_RELEASE));
    options->AddEventAction(WIDGET_EVENT_SCROLL_UP_LSTICK_RELEASE).Set(new (TAG_SWF) idWidgetActionHandler(options, WIDGET_ACTION_EVENT_STOP_REPEATER, WIDGET_EVENT_SCROLL_UP_LSTICK_RELEASE));
}

/*
========================
idMenuScreen_Shell_GameOptions::Update
========================
*/
void idMenuScreen_Shell_GameOptions::Update()
{

    if (menuData != NULL) {
        idMenuWidget_CommandBar* cmdBar = menuData->GetCmdBar();
        if (cmdBar != NULL) {
            cmdBar->ClearAllButtons();
            idMenuWidget_CommandBar::buttonInfo_t* buttonInfo;
            buttonInfo = cmdBar->GetButton(idMenuWidget_CommandBar::BUTTON_JOY2);
            if (menuData->GetPlatform() != 2) {
                buttonInfo->label = "#str_00395";
            }
            buttonInfo->action.Set(WIDGET_ACTION_GO_BACK);

            buttonInfo = cmdBar->GetButton(idMenuWidget_CommandBar::BUTTON_JOY1);
            buttonInfo->action.Set(WIDGET_ACTION_PRESS_FOCUSED);
        }
    }

    idSWFScriptObject& root = GetSWFObject()->GetRootObject();
    if (BindSprite(root)) {
        idSWFTextInstance* heading = GetSprite()->GetScriptObject()->GetNestedText("info", "txtHeading");
        if (heading != NULL) {
            heading->SetText("#str_02129"); // SYSTEM SETTINGS
            heading->SetStrokeInfo(true, 0.75f, 1.75f);
        }

        idSWFSpriteInstance* gradient = GetSprite()->GetScriptObject()->GetNestedSprite("info", "gradient");
        if (gradient != NULL && heading != NULL) {
            gradient->SetXPos(heading->GetTextLength());
        }
    }

    if (btnBack != NULL) {
        btnBack->BindSprite(root);
    }

    idMenuScreen::Update();
}

/*
========================
idMenuScreen_Shell_GameOptions::ShowScreen
========================
*/
void idMenuScreen_Shell_GameOptions::ShowScreen(const mainMenuTransition_t transitionType)
{
    systemData.LoadData();
    idMenuScreen::ShowScreen(transitionType);
}

/*
========================
idMenuScreen_Shell_GameOptions::HideScreen
========================
*/
void idMenuScreen_Shell_GameOptions::HideScreen(const mainMenuTransition_t transitionType)
{
    if (systemData.IsDataChanged()) {
        systemData.CommitData();
    }
    idMenuScreen::HideScreen(transitionType);
}

/*
========================
idMenuScreen_Shell_GameOptions::HandleAction h
========================
*/
bool idMenuScreen_Shell_GameOptions::HandleAction(idWidgetAction& action, const idWidgetEvent& event, idMenuWidget* widget, bool forceHandled)
{

    if (menuData == NULL) {
        return true;
    }

    if (menuData->ActiveScreen() != SHELL_AREA_GAME_OPTIONS) {
        return false;
    }

    widgetAction_t actionType = action.GetType();
    const idSWFParmList& parms = action.GetParms();

    switch (actionType) {
    case WIDGET_ACTION_GO_BACK: {
        menuData->SetNextScreen(SHELL_AREA_SETTINGS, MENU_TRANSITION_SIMPLE);
        return true;
    }
    case WIDGET_ACTION_PRESS_FOCUSED: {

        if (options == NULL) {
            return true;
        }

        int selectionIndex = options->GetFocusIndex();
        if (parms.Num() > 0) {
            selectionIndex = parms[0].ToInteger();
        }

        if (selectionIndex != options->GetFocusIndex()) {
            options->SetViewIndex(options->GetViewOffset() + selectionIndex);
            options->SetFocusIndex(selectionIndex);
        }

        systemData.AdjustField(selectionIndex, 1);
        options->Update();

        return true;
    }
    case WIDGET_ACTION_START_REPEATER: {
        if (parms.Num() == 4) {
            int selectionIndex = parms[3].ToInteger();
            if (selectionIndex != options->GetFocusIndex()) {
                options->SetViewIndex(options->GetViewOffset() + selectionIndex);
                options->SetFocusIndex(selectionIndex);
            }
        }
        break;
    }
    }

    return idMenuWidget::HandleAction(action, event, widget, forceHandled);
}

/////////////////////////////////
// SCREEN SETTINGS
/////////////////////////////////

extern idCVar ui_autoSwitch;
extern idCVar ui_autoReload;
extern idCVar aa_targetAimAssistEnable;
extern idCVar in_alwaysRun;
extern idCVar g_checkpoints;
extern idCVar g_weaponShadows;
extern idCVar net_online; // sys_net_online.cpp (or its local-only stand-in)

#if defined(__vita__)
extern idCVar vita_language;
extern idCVar sys_lang;

// The languages this install has strings for (strings/<language>*.lang), in
// the engine's order, and their own names.
static idStrList vitaLanguages;
static const char* VitaLanguageName(const char* lang)
{
    static const char* names[][2] = {
        { ID_LANG_ENGLISH, "English" },
        { ID_LANG_FRENCH, "Fran\xC3\xA7" "ais" },
        { ID_LANG_ITALIAN, "Italiano" },
        { ID_LANG_GERMAN, "Deutsch" },
        { ID_LANG_SPANISH, "Espa\xC3\xB1" "ol" },
        { ID_LANG_JAPANESE, "Japanese" },
    };
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (idStr::Icmp(lang, names[i][0]) == 0) {
            return names[i][1];
        }
    }
    return lang;
}

static void VitaFindLanguages()
{
    if (vitaLanguages.Num() > 0) {
        return;
    }
    idFileList* files = fileSystem->ListFilesTree("strings", ".lang", true);
    for (int l = 0; l < Sys_NumLangs(); l++) {
        const char* lang = Sys_Lang(l);
        for (int i = 0; i < files->GetNumFiles(); i++) {
            idStr name = files->GetFile(i);
            name.StripPath();
            if (idStr::Icmpn(name.c_str(), lang, idStr::Length(lang)) == 0) {
                vitaLanguages.Append(lang);
                break;
            }
        }
    }
    fileSystem->FreeFileList(files);
    if (vitaLanguages.Num() == 0) {
        vitaLanguages.Append(sys_lang.GetString());
    }
    idStr list;
    for (int i = 0; i < vitaLanguages.Num(); i++) {
        list += va("%s%s", i ? ", " : "", vitaLanguages[i].c_str());
    }
    common->Printf("vita: languages with strings installed: %s (running in %s)\n", list.c_str(), sys_lang.GetString());
}

// the language the next start uses
static const char* VitaPendingLanguage()
{
    return vita_language.GetString()[0] ? vita_language.GetString() : sys_lang.GetString();
}
#endif

/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_AudioSettings::idMenuDataSource_AudioSettings
========================
*/
idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::idMenuDataSource_GameSettings()
{
    fields.SetNum(MAX_GAME_FIELDS);
    originalFields.SetNum(MAX_GAME_FIELDS);
}

/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_AudioSettings::LoadData
========================
*/
void idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::LoadData()
{
    fields[GAME_FIELD_FOV].SetInteger(g_fov.GetFloat());
    fields[GAME_FIELD_CHECKPOINTS].SetBool(g_checkpoints.GetBool());
    fields[GAME_FIELD_AUTO_SWITCH].SetBool(ui_autoSwitch.GetBool());
    fields[GAME_FIELD_AUTO_RELOAD].SetBool(ui_autoReload.GetBool());
    fields[GAME_FIELD_AIM_ASSIST].SetBool(aa_targetAimAssistEnable.GetBool());
    fields[GAME_FIELD_ALWAYS_SPRINT].SetBool(in_alwaysRun.GetBool());
    fields[GAME_FIELD_FLASHLIGHT_SHADOWS].SetBool(g_weaponShadows.GetBool());
    fields[GAME_FIELD_ONLINE].SetBool(net_online.GetBool());
#if defined(__vita__)
    VitaFindLanguages();
    vitaPickedLanguage = VitaPendingLanguage();
    SetVitaLanguageField();
#endif
    originalFields = fields;
}

/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_AudioSettings::CommitData
========================
*/
void idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::CommitData()
{

    g_fov.SetFloat(fields[GAME_FIELD_FOV].ToFloat());
    g_gun_x.SetFloat(Lerp(MIN_FOV_GUN, MAX_FOV_GUN, (fields[GAME_FIELD_FOV].ToFloat() - MIN_FOV) / (MAX_FOV - MIN_FOV)));

    g_checkpoints.SetBool(fields[GAME_FIELD_CHECKPOINTS].ToBool());
    ui_autoSwitch.SetBool(fields[GAME_FIELD_AUTO_SWITCH].ToBool());
    ui_autoReload.SetBool(fields[GAME_FIELD_AUTO_RELOAD].ToBool());
    aa_targetAimAssistEnable.SetBool(fields[GAME_FIELD_AIM_ASSIST].ToBool());
    in_alwaysRun.SetBool(fields[GAME_FIELD_ALWAYS_SPRINT].ToBool());
    g_weaponShadows.SetBool(fields[GAME_FIELD_FLASHLIGHT_SHADOWS].ToBool());
    if (net_online.GetBool() != fields[GAME_FIELD_ONLINE].ToBool()) {
        net_online.SetBool(fields[GAME_FIELD_ONLINE].ToBool());
        common->Printf("net: online play switched %s in Game Options (%s)\n", net_online.GetBool() ? "ON" : "off",
            net_online.GetBool() ? "the multiplayer lobbies look for players over the internet" : "multiplayer is the local network and friends added by address only");
    }
#if defined(__vita__)
    if (idStr::Icmp(vitaPickedLanguage, VitaPendingLanguage()) != 0) {
        vita_language.SetString(vitaPickedLanguage);
        common->Printf("vita: language %s picked in Game Options (running in %s; applies at the next start)\n", vitaPickedLanguage.c_str(), sys_lang.GetString());
    }
#endif

    cvarSystem->SetModifiedFlags(CVAR_ARCHIVE);

    // make the committed fields into the backup fields
    originalFields = fields;
}

/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_AudioSettings::AdjustField
========================
*/
void idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::AdjustField(const int fieldIndex, const int adjustAmount)
{
    if (fieldIndex == GAME_FIELD_FOV) {
        fields[fieldIndex].SetInteger(idMath::ClampInt(MIN_FOV, MAX_FOV, fields[fieldIndex].ToInteger() + adjustAmount * 5));
#if defined(__vita__)
    } else if (fieldIndex == GAME_FIELD_LANGUAGE) {
        VitaFindLanguages();
        int cur = 0;
        for (int i = 0; i < vitaLanguages.Num(); i++) {
            if (idStr::Icmp(vitaLanguages[i], vitaPickedLanguage) == 0) {
                cur = i;
            }
        }
        const int n = vitaLanguages.Num();
        cur = ((cur + adjustAmount) % n + n) % n;
        vitaPickedLanguage = vitaLanguages[cur];
        SetVitaLanguageField();
#endif
    } else {
        fields[fieldIndex].SetBool(!fields[fieldIndex].ToBool());
    }
}

#if defined(__vita__)
/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::SetVitaLanguageField

The picked language's own name, marked when it is not the one running now.
========================
*/
void idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::SetVitaLanguageField()
{
    const char* name = VitaLanguageName(vitaPickedLanguage);
    if (idStr::Icmp(vitaPickedLanguage, sys_lang.GetString()) != 0) {
        fields[GAME_FIELD_LANGUAGE].SetString(va("%s %s", name, idLocalization::GetString("#str_vita_after_restart")));
    } else {
        fields[GAME_FIELD_LANGUAGE].SetString(name);
    }
}
#endif

/*
========================
idMenuScreen_Shell_GameOptions::idMenuDataSource_AudioSettings::IsDataChanged
========================
*/
bool idMenuScreen_Shell_GameOptions::idMenuDataSource_GameSettings::IsDataChanged() const
{

    if (fields[GAME_FIELD_FOV].ToInteger() != originalFields[GAME_FIELD_FOV].ToInteger()) {
        return true;
    }

    if (fields[GAME_FIELD_CHECKPOINTS].ToBool() != originalFields[GAME_FIELD_CHECKPOINTS].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_AUTO_SWITCH].ToBool() != originalFields[GAME_FIELD_AUTO_SWITCH].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_AUTO_RELOAD].ToBool() != originalFields[GAME_FIELD_AUTO_RELOAD].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_AIM_ASSIST].ToBool() != originalFields[GAME_FIELD_AIM_ASSIST].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_ALWAYS_SPRINT].ToBool() != originalFields[GAME_FIELD_ALWAYS_SPRINT].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_FLASHLIGHT_SHADOWS].ToBool() != originalFields[GAME_FIELD_FLASHLIGHT_SHADOWS].ToBool()) {
        return true;
    }

    if (fields[GAME_FIELD_ONLINE].ToBool() != originalFields[GAME_FIELD_ONLINE].ToBool()) {
        return true;
    }

#if defined(__vita__)
    if (idStr::Icmp(vitaPickedLanguage, VitaPendingLanguage()) != 0) {
        return true;
    }
#endif

    return false;
}