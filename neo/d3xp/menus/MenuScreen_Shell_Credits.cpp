/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013 Robert Beckebans

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

static const int NUM_CREDIT_LINES = 16;

void idMenuScreen_Shell_Credits::SetupCreditList()
{

    class idRefreshCredits : public idSWFScriptFunction_RefCounted {
    public:
        idRefreshCredits(idMenuScreen_Shell_Credits* _screen)
            : screen(_screen)
        {
        }

        idSWFScriptVar Call(idSWFScriptObject* thisObject, const idSWFParmList& parms)
        {

            if (screen == NULL) {
                return idSWFScriptVar();
            }

            screen->UpdateCredits();
            return idSWFScriptVar();
        }

    private:
        idMenuScreen_Shell_Credits* screen;
    };

    if (GetSWFObject()) {
        GetSWFObject()->SetGlobal("updateCredits", new (TAG_SWF) idRefreshCredits(this));
    }

    creditList.Clear();
    // One block of text, a line per credit - type digit (or "-" for a gap)
    // then the text - read here, instead of 661 separate Append calls:
    // each call was ~9 relocations, and the Vita build sits at
    // vita-elf-create's relocation ceiling (~284k; it can not package past it).
    static const char vitaCredits[] =
        "3RBDOOM 3 BFG EDITION\n"
        "-\n"
        "1Project Owner\n"
        "0Robert Beckebans - Engine upgrades, Linux, TrenchBroom\n"
        "-\n"
        "1Additional Programming\n"
        "0Daniel Gibson - Tons of code cleanups, netcode++\n"
        "0Jonathan Young - Bugfixes, misc improvements\n"
        "0Felix Rueegg - Doomclassic Linux support\n"
        "0Carl Kenner - Bink video support\n"
        "0Pat Raynor - Compiler tools, QOL patches\n"
        "0Biel B. de Luna - Intro skipping, QOL patches\n"
        "0Dustin Land - Initial Vulkan backend\n"
        "0Stephen Saunders - macOS support, Vulkan bugfixes\n"
        "0Justin Marshall - QOL patches from IcedHellFire branch\n"
        "0Stephen Pridham - Major work on NVRHI backend\n"
        "0Harrie van Ginneken - glTF2 support, IcedHellFire++\n"
        "-\n"
        "-\n"
        "-\n"
        "0Thanks to everyone who provided valuable feedback on\n"
        "0GitHub and the id Tech 4 Discord\n"
        "-\n"
        "-\n"
        "-\n"
        "-\n"
        "3DOOM 3 BFG EDITION\n"
        "-\n"
        "2DEVELOPMENT TEAM\n"
        "-\n"
        "1Executive Producer\n"
        "0Eric Webb\n"
        "-\n"
        "1Lead Programmer\n"
        "0Brian Harris\n"
        "-\n"
        "1Lead Designer\n"
        "0Jerry Keehan\n"
        "-\n"
        "1Programming\n"
        "0Curtis Arink\n"
        "0Robert Duffy\n"
        "0Jeff Farrand\n"
        "0Ryan Gerleve\n"
        "0Billy Khan\n"
        "0Gloria Kennickell\n"
        "0Mike Maynard\n"
        "0John Roberts\n"
        "0Steven Serafin\n"
        "0Jan Paul Van Waveren\n"
        "-\n"
        "1Design\n"
        "0Steve Rescoe\n"
        "0Chris Voss\n"
        "0David Vargo\n"
        "-\n"
        "1Art\n"
        "0Kevin Cloud\n"
        "0Andy Chang\n"
        "0Pat Duffy\n"
        "-\n"
        "1Audio\n"
        "0Christian Antkow\n"
        "0Chris Hite\n"
        "-\n"
        "1Additional Support\n"
        "0Roger Berrones\n"
        "0Brad Bramlett\n"
        "0John Casey\n"
        "0Jeremy Cook\n"
        "0Chris Hays\n"
        "0Matt Hooper\n"
        "0Danny Keys\n"
        "0Jason Kim\n"
        "0Brian Kowalczyk\n"
        "0Dustin Land\n"
        "0Stephane Lebrun\n"
        "0Matt Nelson\n"
        "0John Pollard\n"
        "0Alan Rogers\n"
        "0Jah Raphael\n"
        "0Jarrod Showers\n"
        "0Shale Williams\n"
        "-\n"
        "1Studio Director\n"
        "0Tim Willits\n"
        "-\n"
        "1Studio President\n"
        "0Todd Hollenshead\n"
        "-\n"
        "1Technical Director\n"
        "0John Carmack\n"
        "-\n"
        "1Human Resources\n"
        "0Carrie Barcroft\n"
        "-\n"
        "1id Mom\n"
        "0Donna Jackson\n"
        "-\n"
        "1id Software IT\n"
        "0Duncan Welch\n"
        "0Michael Cave\n"
        "0Josh Shoemate\n"
        "0Michael Musick\n"
        "0Alex Brandt\n"
        "-\n"
        "3DOOM 3 DEVELOPED BY\n"
        "-\n"
        "2id Software\n"
        "-\n"
        "1Technical Director \n"
        "0John Carmack\n"
        "-\n"
        "1Lead Programmer\n"
        "0Robert A. Duffy\n"
        "-\n"
        "1Lead Designer\n"
        "0Tim Willits\n"
        "-\n"
        "1Lead Artist\n"
        "0Kenneth Scott\n"
        "-\n"
        "1Animation\n"
        "0James Houska\n"
        "0Fredrik Nilsson\n"
        "0Eric Webb\n"
        "-\n"
        "1Art\n"
        "0Adrian Carmack\n"
        "0Andy Chang\n"
        "0Kevin Cloud\n"
        "0Seneca Menard\n"
        "0Patrick Thomas\n"
        "-\n"
        "1Design\n"
        "0Mal Blackwell\n"
        "0Matt Hooper\n"
        "0Jerry Keehan\n"
        "0Steve Rescoe\n"
        "-\n"
        "1UI Artist\n"
        "0Pat Duffy\n"
        "-\n"
        "1Programming\n"
        "0Timothee Besset\n"
        "0Jim Dose\n"
        "0Jan Paul van Waveren\n"
        "0Jonathan Wright\n"
        "0Brian Harris\n"
        "-\n"
        "1Sound Design \n"
        "0Christian Antkow\n"
        "-\n"
        "1Dir. Business Development\n"
        "0Marty Stratton\n"
        "-\n"
        "1Development Assistant\n"
        "0Eric Webb\n"
        "-\n"
        "23rd PARTY\n"
        "-\n"
        "1DOOM 3: Ressurection of Evil Developed by\n"
        "0Nerve Software\n"
        "-\n"
        "1Additional Sound Design\n"
        "0Ed Lima\n"
        "0Danetracks, Inc.\n"
        "0Defacto Sound\n"
        "-\n"
        "1Theme for DOOM 3 Produced by\n"
        "0Chris Vrenna\n"
        "-\n"
        "1Theme for DOOM 3 Composed by\n"
        "0Clint Walsh\n"
        "-\n"
        "1Additional Story and Dialog\n"
        "0Matthew J. Costello\n"
        "-\n"
        "1Casting and Voice Direction\n"
        "0Margaret Tang - Womb Music\n"
        "-\n"
        "1Voice Editorial and Post\n"
        "0Rik Schaffer - Womb Music\n"
        "-\n"
        "1Voice Recording\n"
        "0Womb Music\n"
        "-\n"
        "1Voice Performers\n"
        "0Paul Eiding\n"
        "0Jim Cummings\n"
        "0Liam O'Brien\n"
        "0Nicholas Guest\n"
        "0Vanessa Marshall\n"
        "0Neill Ross\n"
        "0Phillip Clarke\n"
        "0Andy Chanley\n"
        "0Charles Dennis\n"
        "0Grey Delisle\n"
        "0Jennifer Hale\n"
        "0Grant Albrecht\n"
        "0Dee Baker\n"
        "0Michael Bell\n"
        "0Steve Blum\n"
        "0S. Scott Bullock\n"
        "0Cam Clarke\n"
        "0Robin Atkin Downes\n"
        "0Keith Ferguson\n"
        "0Jay Gordon\n"
        "0Michael Gough\n"
        "0Bill Harper\n"
        "0Nick Jameson\n"
        "0David Kaye\n"
        "0Phil La Marr\n"
        "0Scott Menville\n"
        "0Jim Meskimen\n"
        "0Matt Morton\n"
        "0Daran Norris\n"
        "0Rob Paulsen\n"
        "0Phil Proctor\n"
        "0Rino Romoano\n"
        "0Andre Sogliuzzo\n"
        "0Jim Ward\n"
        "0Wally Wingert\n"
        "0Edward Yin\n"
        "0Keone Young\n"
        "0Ryun Yu\n"
        "-\n"
        "1Additional User Interface\n"
        "0Double-Action Design\n"
        "-\n"
        "1Motion Capture Services\n"
        "0Janimation\n"
        "-\n"
        "1UAC Promotional Videos\n"
        "0Six Foot Studios\n"
        "-\n"
        "1Additional Multiplayer Design\n"
        "0Splash Damage, Ltd.\n"
        "-\n"
        "1Additional Programming\n"
        "0Graeme Devine\n"
        "-\n"
        "1Localization Services\n"
        "0Synthesis\n"
        "-\n"
        "-\n"
        "3BETHESDA SOFTWORKS\n"
        "-\n"
        "1Senior Producer\n"
        "0Laffy Taylor\n"
        "-\n"
        "1Sr. Producer, Submissions\n"
        "0Timothy Beggs\n"
        "-\n"
        "1VP, Product Development\n"
        "0Todd Vaughn\n"
        "-\n"
        "1External Technical Director\n"
        "0Jonathan Williams\n"
        "-\n"
        "1President\n"
        "0Vlatko Andonov\n"
        "-\n"
        "1VP, Public Relations and Marketing\n"
        "0Pete Hines\n"
        "-\n"
        "1Director of Global Marketing\n"
        "0Steve Perkins\n"
        "-\n"
        "1Director of Global PR\n"
        "0Tracey Thompson\n"
        "-\n"
        "1Senior Brand Manager\n"
        "0Rakhi Gupta\n"
        "-\n"
        "1Associate Brand Manager\n"
        "0David Clayman\n"
        "-\n"
        "1Event and Trade Show Director\n"
        "0Henry Mobley\n"
        "-\n"
        "1Senior Community Manager\n"
        "0Matthew Grandstaff\n"
        "-\n"
        "1Community Manager\n"
        "0Nick Breckon\n"
        "-\n"
        "1Senior PR Coordinator\n"
        "0Angela Ramsey-Chapman\n"
        "-\n"
        "1Marketing Assistant\n"
        "0Jenny McWhorter\n"
        "-\n"
        "1Video Production\n"
        "0Matt Killmon\n"
        "0Sal Goldenburg\n"
        "-\n"
        "1Graphic Design\n"
        "0Michael Wagner\n"
        "0Lindsay Westcott\n"
        "-\n"
        "1VP, Sales\n"
        "0Ron Seger\n"
        "-\n"
        "1Operations Director\n"
        "0Todd Curtis\n"
        "-\n"
        "1Sales and Operations Manager\n"
        "0Jill Bralove\n"
        "-\n"
        "1Sales Account Manager\n"
        "0Michelle Ferrara\n"
        "-\n"
        "1Regional Sales Manager\n"
        "0Michael Donnellan\n"
        "-\n"
        "1Channel Marketing Manager\n"
        "0Michelle Burgess\n"
        "-\n"
        "1Sales Assistants\n"
        "0Sara Simpson\n"
        "0Jason Snead\n"
        "0Jessica Williams\n"
        "-\n"
        "1Operations Assistant\n"
        "0Scott Mills\n"
        "-\n"
        "1Director of Quality Assurance\n"
        "0Darren Manes\n"
        "-\n"
        "1Quality Assurance Manager\n"
        "0Rob Gray\n"
        "-\n"
        "1Quality Assurance Leads\n"
        "0Terry Dunn\n"
        "0Matt Weil\n"
        "-\n"
        "1Quality Assurance\n"
        "0Dan Silva\n"
        "0Alan Webb\n"
        "0Cory Andrews\n"
        "0George Churchill\n"
        "0Jonathan DeVriendt\n"
        "0Spencer Gottlieb\n"
        "0Gary Powell\n"
        "0Samuel Papke\n"
        "0Amanda Sheehan\n"
        "0Philip Spangrud\n"
        "0Larry Waldman\n"
        "0Donald Anderson\n"
        "0James Audet\n"
        "0Hunter Calvert\n"
        "0Max Cameron\n"
        "0Donald Harris\n"
        "0Peter Garcia\n"
        "0Joseph Lopatta\n"
        "0Scott LoPresti\n"
        "0Collin Mackett\n"
        "0Gerard Nagy\n"
        "0William Pegus\n"
        "0Angela Rupinen\n"
        "0Gabriel Scaringello\n"
        "0Drew Slotkin\n"
        "0Kyle Wallace\n"
        "0Patrick Walsh\n"
        "0John Welkner\n"
        "0Jason Wilkin\n"
        "0John Benoit\n"
        "0Jacob Clayman\n"
        "0Colin McInerney\n"
        "-\n"
        "1Additional QA\n"
        "0James Costantino\n"
        "0Chris Krietz\n"
        "0Joseph Mueller\n"
        "0Andrew Scharf\n"
        "0Jen Tonon\n"
        "0Justin McSweeney\n"
        "0Sean Palomino\n"
        "0Erica Stead\n"
        "0Wil Cookman\n"
        "0Garrett Hohl\n"
        "0Tim Hartgrave\n"
        "0Heath Hollenshead\n"
        "0Brandon Korbel\n"
        "0Daniel Korecki\n"
        "0Max Morrison\n"
        "0Doug Ransley\n"
        "0Mauricio Rivera\n"
        "0Aaron Walsh\n"
        "0Daniel Wathen\n"
        "-\n"
        "1Special Thanks\n"
        "0Jason Bergman\n"
        "0Darren Chukitus\n"
        "0Matt Dickenson\n"
        "0Will Noble\n"
        "-\n"
        "-\n"
        "3ZENIMAX MEDIA\n"
        "-\n"
        "1Chairman & CEO\n"
        "0Robert Altman\n"
        "-\n"
        "1President\n"
        "0Ernie Del\n"
        "-\n"
        "1EVP & COO\n"
        "0Jamie Leder\n"
        "-\n"
        "1EVP & CFO\n"
        "0Cindy Tallent\n"
        "-\n"
        "1EVP Legal & Secretary\n"
        "0Grif Lesher\n"
        "-\n"
        "1SVP Finance & Controller\n"
        "0Denise Kidd\n"
        "-\n"
        "1Legal Lead\n"
        "0Joshua Gillespie\n"
        "-\n"
        "1Legal\n"
        "0Diana Bender\n"
        "0Adam Carter\n"
        "0Candice Garner-Groves\n"
        "0Marcia Mitnick\n"
        "0Amy Yeung\n"
        "-\n"
        "1VP, Information Technology\n"
        "0Steve Bloom\n"
        "-\n"
        "1Information Technology\n"
        "0Rob Havlovick\n"
        "0Nicholas Lea\n"
        "0Drew McCartney\n"
        "0Josh Mosby\n"
        "0Joseph Owens\n"
        "0Paul Tuttle\n"
        "0Keelian Wardle\n"
        "-\n"
        "1Director of Global HR\n"
        "0Tammy Boyd-Shumway\n"
        "-\n"
        "1Human Resources\n"
        "0Michelle Cool\n"
        "0Andrea Glinski\n"
        "0Katrina Lang\n"
        "0Valery Ridore\n"
        "-\n"
        "1Administration\n"
        "0Melissa Ayala\n"
        "0Brittany Bezawada-Joseph\n"
        "0Rosanna Campanile  \n"
        "0Katherine Edwards\n"
        "0Douglas Fredrick    \n"
        "0Jon Freund\n"
        "0Ken Garcia \n"
        "0Gerard Garnica\n"
        "0Betty Kouatelay\n"
        "0Ho Joong Lee\n"
        "0Barb Manning\n"
        "0Stephane Marquis\n"
        "0Michael Masciola\n"
        "0Tanuja Mistry\n"
        "0Rissa Monzano\n"
        "0Patrick Nolan \n"
        "0Patti Pulupa\n"
        "0Dave Rasmussen \n"
        "0Heather Spurrier\n"
        "0Claudia Umana\n"
        "0Melissa Washabaugh\n"
        "0Eric Weis\n"
        "-\n"
        "1Administrative Assistants\n"
        "0Bernice Guice\n"
        "0Paula Kasey\n"
        "0Shana Reed\n"
        "-\n"
        "1Executive Chef\n"
        "0Kenny McDonald\n"
        "-\n"
        "3ZENIMAX EUROPE\n"
        "-\n"
        "1European Managing Director\n"
        "0Sean Brennan\n"
        "-\n"
        "1European Marketing & PR Director\n"
        "0Sarah Seaby\n"
        "-\n"
        "1European Sales Director\n"
        "0Paul Oughton\n"
        "-\n"
        "1Sales Director\n"
        "0Greg Baverstock\n"
        "-\n"
        "1Finance Director\n"
        "0Robert Ford\n"
        "-\n"
        "1European Brand Marketing Manager\n"
        "0Thach Quach\n"
        "-\n"
        "1European Brand Marketing Manager\n"
        "0Alex Price\n"
        "-\n"
        "1European PR Manager\n"
        "0Alistair Hatch\n"
        "-\n"
        "1European Assistant PR Manager\n"
        "0Nicholas Heller\n"
        "-\n"
        "1International Marketing Manager\n"
        "0Rosemarie Dalton\n"
        "-\n"
        "1Manager Trade Shows/Events\n"
        "0Gareth Swann\n"
        "-\n"
        "1UK Marketing Manager\n"
        "0Gregory Weller\n"
        "-\n"
        "1UK Sales Manager\n"
        "0Gethyn Deakins\n"
        "-\n"
        "1Creative Services Artist\n"
        "0Morgan Gibbons\n"
        "-\n"
        "1Legal Counsel\n"
        "0Adam Carter\n"
        "-\n"
        "1Localisation Director\n"
        "0Harald Simon\n"
        "-\n"
        "1Localisation Project Manager\n"
        "0Ruth Granados Garcia\n"
        "-\n"
        "1Sales Administrator\n"
        "0Heather Clarke\n"
        "-\n"
        "1Operations Manager\n"
        "0Isabelle Midrouillet\n"
        "-\n"
        "1Operations Coordinator\n"
        "0David Gordon\n"
        "-\n"
        "1Financial Controller\n"
        "0Paul New\n"
        "-\n"
        "1Accounts Assistant\n"
        "0Charlotte Ovens\n"
        "-\n"
        "1Contracts Administrator/Paralegal\n"
        "0Katie Brooks\n"
        "-\n"
        "1IT Manager\n"
        "0Joseph Owens\n"
        "-\n"
        "1Office Manager\n"
        "0Angela Clement\n"
        "-\n"
        "3ZENIMAX FRANCE\n"
        "-\n"
        "1General Manager\n"
        "0Julie Chalmette\n"
        "-\n"
        "1Sales Manager\n"
        "0Yvan Rault\n"
        "-\n"
        "1Key Account Manager\n"
        "0Gaelle Gombert\n"
        "-\n"
        "1Trade Marketing Manager\n"
        "0Laurent Chatain\n"
        "-\n"
        "1Marketing Manager\n"
        "0Geraldine Mazot\n"
        "-\n"
        "1PR Manager\n"
        "0Jerome Firon\n"
        "-\n"
        "1Finance Manager\n"
        "0Cecile De Freitas\n"
        "-\n"
        "1Finance Assistant\n"
        "0Adeline Nonis\n"
        "-\n"
        "3ZENIMAX GERMANY\n"
        "-\n"
        "1Managing Director\n"
        "0Frank Matzke\n"
        "-\n"
        "1Sales Director\n"
        "0Thomas Huber\n"
        "-\n"
        "1Head of Marketing & PR\n"
        "0Marcel Jung\n"
        "-\n"
        "1Product Marketing Manager\n"
        "0Stefan Dettmering\n"
        "-\n"
        "1PR Manager\n"
        "0Peter Langhofer\n"
        "-\n"
        "1Trade Marketing Manager\n"
        "0Andrea Reuth\n"
        "-\n"
        "1Key Account Manager\n"
        "0Juergen Pahl\n"
        "-\n"
        "1Finance Manager\n"
        "0Joern Hoehling\n"
        "-\n"
        "1Sales and Office Administrator\n"
        "0Christiane Jauss\n"
        "-\n"
        "3ZENIMAX BENELUX\n"
        "-\n"
        "1General Manager\n"
        "0Menno Eijck\n"
        "-\n"
        "1Sales Manager\n"
        "0Stefan Koppers\n"
        "-\n"
        "1Marketing Manager\n"
        "0Jurgen Stirnweis\n"
        "-\n"
        "1PR Manager\n"
        "0Maikel van Dijk\n"
        "-\n"
        "1Sales and Marketing Administrator\n"
        "0Pam van Griethuysen\n"
        "-\n"
        "-\n"
        "3ZENIMAX ASIA\n"
        "-\n"
        "1General Manager\n"
        "0Tetsu Takahashi\n"
        "-\n"
        "1Localization Producer \n"
        "0Kei Iwamoto\n"
        "-\n"
        "1Associate Producer \n"
        "0Takayuki Tanaka\n"
        "-\n"
        "1Product Manager \n"
        "0Seigen Ko\n"
        "-\n"
        "1Localization Programmer\n"
        "0Masayuki Nagahashi\n"
        "-\n"
        "1Marketing and PR Manager \n"
        "0Eiichi Yaji\n"
        "-\n"
        "1Sales Manager\n"
        "0Hiroaki Yanagiguchi\n"
        "-\n"
        "1Office Manager\n"
        "0Myongsuk Rim\n"
        "-\n"
        "1Web Director\n"
        "0Tanaka Keisuke\n"
        "0Kosuke Fujita\n";
    for (const char* c = vitaCredits; *c != 0;) {
        const char* eol = strchr(c, '\n');
        if (eol == NULL) {
            break;
        }
        if (*c == '-') {
            creditList.Append(creditInfo_t());
        } else {
            idStr text;
            text.Append(c + 1, (int)(eol - c - 1));
            creditList.Append(creditInfo_t(*c - '0', text.c_str()));
        }
        c = eol + 1;
    }
};

/*
========================
idMenuScreen_Shell_Credits::Initialize
========================
*/
void idMenuScreen_Shell_Credits::Initialize(idMenuHandler* data)
{
    idMenuScreen::Initialize(data);

    if (data != NULL) {
        menuGUI = data->GetGUI();
    }

    SetSpritePath("menuCredits");

    btnBack = new (TAG_SWF) idMenuWidget_Button();
    btnBack->Initialize(data);
    btnBack->SetLabel("#str_02305");
    btnBack->SetSpritePath(GetSpritePath(), "info", "btnBack");
    btnBack->AddEventAction(WIDGET_EVENT_PRESS).Set(WIDGET_ACTION_GO_BACK);
    AddChild(btnBack);

    SetupCreditList();
}

/*
========================
idMenuScreen_Shell_Credits::Update
========================
*/
void idMenuScreen_Shell_Credits::Update()
{

    if (menuData != NULL) {
        idMenuWidget_CommandBar* cmdBar = menuData->GetCmdBar();
        if (cmdBar != NULL) {
            cmdBar->ClearAllButtons();

            idMenuHandler_Shell* shell = dynamic_cast<idMenuHandler_Shell*>(menuData);
            bool complete = false;
            if (shell != NULL) {
                complete = shell->GetGameComplete();
            }

            idMenuWidget_CommandBar::buttonInfo_t* buttonInfo;
            if (!complete) {
                buttonInfo = cmdBar->GetButton(idMenuWidget_CommandBar::BUTTON_JOY2);
                if (menuData->GetPlatform() != 2) {
                    buttonInfo->label = "#str_00395";
                }
                buttonInfo->action.Set(WIDGET_ACTION_GO_BACK);
            } else {
                buttonInfo = cmdBar->GetButton(idMenuWidget_CommandBar::BUTTON_JOY1);
                if (menuData->GetPlatform() != 2) {
                    buttonInfo->label = "#str_swf_continue";
                }
                buttonInfo->action.Set(WIDGET_ACTION_GO_BACK);
            }
        }
    }

    idSWFScriptObject& root = GetSWFObject()->GetRootObject();
    if (BindSprite(root)) {
        idSWFTextInstance* heading = GetSprite()->GetScriptObject()->GetNestedText("info", "txtHeading");
        if (heading != NULL) {
            heading->SetText("#str_02218");
            heading->SetStrokeInfo(true, 0.75f, 1.75f);
        }
    }

    if (btnBack != NULL) {
        btnBack->BindSprite(root);
    }

    idMenuScreen::Update();
}

/*
========================
idMenuScreen_Shell_Credits::ShowScreen
========================
*/
void idMenuScreen_Shell_Credits::ShowScreen(const mainMenuTransition_t transitionType)
{

    if (menuData != NULL) {
        idMenuHandler_Shell* shell = dynamic_cast<idMenuHandler_Shell*>(menuData);
        bool complete = false;
        if (shell != NULL) {
            complete = shell->GetGameComplete();
        }

        if (complete) {
            menuData->PlaySound(GUI_SOUND_MUSIC);
        }
    }

    idMenuScreen::ShowScreen(transitionType);
    creditIndex = 0;
    UpdateCredits();
}

/*
========================
idMenuScreen_Shell_Credits::HideScreen
========================
*/
void idMenuScreen_Shell_Credits::HideScreen(const mainMenuTransition_t transitionType)
{
    idMenuScreen::HideScreen(transitionType);
}

/*
========================
idMenuScreen_Shell_Credits::HandleAction
========================
*/
bool idMenuScreen_Shell_Credits::HandleAction(idWidgetAction& action, const idWidgetEvent& event, idMenuWidget* widget, bool forceHandled)
{

    if (menuData == NULL) {
        return true;
    }

    if (menuData->ActiveScreen() != SHELL_AREA_CREDITS) {
        return false;
    }

    widgetAction_t actionType = action.GetType();
    switch (actionType) {
    case WIDGET_ACTION_GO_BACK: {

        idMenuHandler_Shell* shell = dynamic_cast<idMenuHandler_Shell*>(menuData);
        bool complete = false;
        if (shell != NULL) {
            complete = shell->GetGameComplete();
        }

        if (complete) {
            cmdSystem->BufferCommandText(CMD_EXEC_NOW, "disconnect");
        } else {
            menuData->SetNextScreen(SHELL_AREA_ROOT, MENU_TRANSITION_SIMPLE);
        }

        return true;
    }
    }

    return idMenuWidget::HandleAction(action, event, widget, forceHandled);
}

/*
========================
idMenuScreen_Shell_Credits::UpdateCredits
========================
*/
void idMenuScreen_Shell_Credits::UpdateCredits()
{

    if (menuData == NULL || GetSWFObject() == NULL) {
        return;
    }

    if (menuData->ActiveScreen() != SHELL_AREA_CREDITS && menuData->NextScreen() != SHELL_AREA_CREDITS) {
        return;
    }

    if (creditIndex >= creditList.Num() + NUM_CREDIT_LINES) {
        idMenuHandler_Shell* shell = dynamic_cast<idMenuHandler_Shell*>(menuData);
        bool complete = false;
        if (shell != NULL) {
            complete = shell->GetGameComplete();
        }

        if (complete) {
            cmdSystem->BufferCommandText(CMD_EXEC_NOW, "disconnect");
        } else {
            menuData->SetNextScreen(SHELL_AREA_ROOT, MENU_TRANSITION_SIMPLE);
        }
        return;
    }

    idSWFScriptObject* options = GetSWFObject()->GetRootObject().GetNestedObj("menuCredits", "info", "options");
    if (options != NULL) {
        for (int i = 15; i >= 0; --i) {
            int curIndex = creditIndex - i;
            idSWFTextInstance* heading = options->GetNestedText(va("item%d", 15 - i), "heading");
            idSWFTextInstance* subHeading = options->GetNestedText(va("item%d", 15 - i), "subHeading");
            idSWFTextInstance* title = options->GetNestedText(va("item%d", 15 - i), "title");
            idSWFTextInstance* txtEntry = options->GetNestedText(va("item%d", 15 - i), "entry");

            if (curIndex >= 0 && curIndex < creditList.Num()) {

                int type = creditList[curIndex].type;
                idStr entry = creditList[curIndex].entry;

                if (heading) {
                    heading->SetText(type == 3 ? entry : "");
                    heading->SetStrokeInfo(true);
                }

                if (subHeading) {
                    subHeading->SetText(type == 2 ? entry : "");
                    subHeading->SetStrokeInfo(true);
                }

                if (title) {
                    title->SetText(type == 1 ? entry : "");
                    title->SetStrokeInfo(true);
                }

                if (txtEntry) {
                    txtEntry->SetText(type == 0 ? entry : "");
                    txtEntry->SetStrokeInfo(true);
                }

            } else {

                if (heading) {
                    heading->SetText("");
                }

                if (subHeading) {
                    subHeading->SetText("");
                }

                if (txtEntry) {
                    txtEntry->SetText("");
                }

                if (title) {
                    title->SetText("");
                }
            }
        }
        if (options->GetSprite()) {
            options->GetSprite()->PlayFrame("roll");
        }
    }

    creditIndex++;
}
