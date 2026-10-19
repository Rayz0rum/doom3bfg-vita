/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.
Copyright (C) 2012 dhewg (dhewm3)
Copyright (C) 2012 Robert Beckebans
Copyright (C) 2013 Daniel Gibson

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.	If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "../../idlib/precompiled.h"

// DG: SDL.h somehow needs the following functions, so #undef those silly
//     "don't use" #defines from Str.h
#undef strncmp
#undef strcasecmp
#undef vsnprintf
// DG end

#include <SDL.h>

#include "renderer/tr_local.h"
#include "sdl_local.h"
#include "../posix/posix_public.h"

#if defined(__vita__)
#include <psp2/touch.h>

/*
Rear touch pad as two buttons: the left half is JOY15, the right half JOY16
(the Vita scheme binds them to the 180 degree quick turn and toggle sprint).
Each half covers its whole height; only its outer edge can be made dead, so a
finger resting on the rim of the pad while holding the Vita does nothing.

The pad is read straight from sceTouchPeek once per input poll rather than
through SDL's finger events, which stay ignored (they arrive continuously
and nothing else in the engine wants them).
*/
idCVar vita_rearTouch("vita_rearTouch", "1", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL,
    "Vita: the rear touch pad's left and right halves act as buttons JOY15 and JOY16");
idCVar vita_rearTouchLeftDeadzone("vita_rearTouchLeftDeadzone", "0.15", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT,
    "Vita: part of the rear pad's LEFT half, measured in from its outer (left) edge, that ignores touches. "
    "0 = the whole half counts, 0.5 = only the inner half of it, 0.9 = only a strip next to the centre", 0.0f, 0.9f);
idCVar vita_rearTouchRightDeadzone("vita_rearTouchRightDeadzone", "0.15", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_FLOAT,
    "Vita: part of the rear pad's RIGHT half, measured in from its outer (right) edge, that ignores touches. "
    "0 = the whole half counts, 0.5 = only the inner half of it, 0.9 = only a strip next to the centre", 0.0f, 0.9f);
idCVar vita_rearTouchSwap("vita_rearTouchSwap", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_BOOL,
    "Vita: mirror the rear pad left/right, should a unit report it mirrored");
idCVar vita_rearTouchDebug("vita_rearTouchDebug", "0", CVAR_SYSTEM | CVAR_BOOL,
    "Vita: log every rear touch report with its position, half and deadzone verdict");
#endif

#if !SDL_VERSION_ATLEAST(2, 0, 0)
#define SDL_Keycode SDLKey
#define SDLK_APPLICATION SDLK_COMPOSE
#define SDLK_SCROLLLOCK SDLK_SCROLLOCK
#define SDLK_LGUI SDLK_LSUPER
#define SDLK_RGUI SDLK_RSUPER
#define SDLK_KP_0 SDLK_KP0
#define SDLK_KP_1 SDLK_KP1
#define SDLK_KP_2 SDLK_KP2
#define SDLK_KP_3 SDLK_KP3
#define SDLK_KP_4 SDLK_KP4
#define SDLK_KP_5 SDLK_KP5
#define SDLK_KP_6 SDLK_KP6
#define SDLK_KP_7 SDLK_KP7
#define SDLK_KP_8 SDLK_KP8
#define SDLK_KP_9 SDLK_KP9
#define SDLK_NUMLOCKCLEAR SDLK_NUMLOCK
#define SDLK_PRINTSCREEN SDLK_PRINT
// DG: SDL1 doesn't seem to have defines for scancodes.. add the (only) one we need
#define SDL_SCANCODE_GRAVE 49 // in SDL2 this is 53.. but according to two different systems and keyboards this works for SDL1
// DG end
#endif

// DG: those are needed for moving/resizing windows
extern idCVar r_windowX;
extern idCVar r_windowY;
extern idCVar r_windowWidth;
extern idCVar r_windowHeight;
// DG end

const char* kbdNames[] = {
    "english", "french", "german", "italian", "spanish", "turkish", "norwegian", NULL
};

idCVar in_keyboard("in_keyboard", "english", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT, "keyboard layout", kbdNames, idCmdSystem::ArgCompletion_String<kbdNames>);

struct kbd_poll_t {
    int key;
    bool state;

    kbd_poll_t()
    {
    }

    kbd_poll_t(int k, bool s)
    {
        key = k;
        state = s;
    }
};

struct mouse_poll_t {
    int action;
    int value;

    mouse_poll_t()
    {
    }

    mouse_poll_t(int a, int v)
    {
        action = a;
        value = v;
    }
};

struct joy_poll_t {
    int action;
    int value;

    joy_poll_t()
    {
    }

    joy_poll_t(int a, int v)
    {
        action = a;
        value = v;
    }
};

static idList<kbd_poll_t> kbd_polls;
static idList<mouse_poll_t> mouse_polls;

#if SDL_VERSION_ATLEAST(2, 0, 0)

/*
================================================================================
Gamepad input

This backend previously left every Sys_*JoystickInputEvent* function as a stub,
which is survivable on a desktop and fatal on a console: the PS Vita has no
keyboard and no mouse, so the pad is the only input device there is.

SDL's game controller layer is used rather than the raw joystick API because it
normalises everything to the Xbox layout that the rest of the engine already
assumes - axes in -32768..32767 with +Y downward, triggers in 0..32767. That is
the same shape win_input.cpp gets out of XInput, so the J_* mapping below is a
straight transliteration of the joyRemap[] table there and the default binds in
the shipped .cfg files keep their meaning. SDL carries a built-in "PSVita
Controller" mapping, so this works on the Vita with no gamecontrollerdb entry.

Two consumers have to be fed, and they are not the same thing:
  - UsercmdGen reads the J_* action/value stream for gameplay (analog look/move)
  - menus and the console read SE_KEY events with K_JOY* keys

Buttons produce one of each. Axes produce an action/value pair every time they
move, plus a synthetic key press/release when they cross the threshold, so stick
and trigger input can drive menus. Sys_GetEvent() hands back a single event per
call, so those synthetic keys go through a small pending queue.
================================================================================
*/
static idList<joy_poll_t> joy_polls;
static SDL_GameController* gameController = NULL;

// Fraction of full deflection at which a stick or trigger counts as "pressed"
// for menu navigation. Matches the range argument win_input.cpp passes.
static const int JOY_KEY_THRESHOLD = 16384;

static const int MAX_PENDING_JOY_KEYS = 32;
static sysEvent_t pendingJoyKeys[MAX_PENDING_JOY_KEYS];
static int numPendingJoyKeys = 0;

// Tracks which K_JOY* keys are currently down so that an axis held past the
// threshold does not re-send a press every frame.
static bool joyKeyState[K_JOY_DPAD_RIGHT - K_JOY1 + 1];

// Last quantised value reported per axis, so SE_JOYSTICK is only sent on change.
static int joyAxisPercent[MAX_JOYSTICK_AXIS];

static void QueueJoyKey(int key, bool down)
{
    const int idx = key - K_JOY1;
    if (idx < 0 || idx >= (int)(sizeof(joyKeyState) / sizeof(joyKeyState[0]))) {
        return;
    }
    if (joyKeyState[idx] == down) {
        return;
    }
    joyKeyState[idx] = down;

    if (numPendingJoyKeys >= MAX_PENDING_JOY_KEYS) {
        return;
    }
    sysEvent_t& ev = pendingJoyKeys[numPendingJoyKeys++];
    ev.evType = SE_KEY;
    ev.evValue = key;
    ev.evValue2 = down ? 1 : 0;
    ev.evPtrLength = 0;
    ev.evPtr = NULL;
}

#if defined(__vita__)
static bool vitaRearDown[2] = { false, false };
static SceTouchPanelInfo vitaRearPanel;
static int vitaRearPanelState = 0; // 0 = not asked yet, 1 = ok, -1 = unavailable

static void PollVitaRearTouch()
{
    bool want[2] = { false, false };

    if (vita_rearTouch.GetBool() && vitaRearPanelState >= 0) {
        if (vitaRearPanelState == 0) {
            memset(&vitaRearPanel, 0, sizeof(vitaRearPanel));
            const int err = sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &vitaRearPanel);
            if (err < 0 || vitaRearPanel.maxAaX <= vitaRearPanel.minAaX || vitaRearPanel.maxAaY <= vitaRearPanel.minAaY) {
                common->Warning("vita: rear touch pad unavailable (sceTouchGetPanelInfo 0x%08X, area %d..%d x %d..%d) - rear touch controls off",
                    (unsigned)err, vitaRearPanel.minAaX, vitaRearPanel.maxAaX, vitaRearPanel.minAaY, vitaRearPanel.maxAaY);
                vitaRearPanelState = -1;
                return;
            }
            vitaRearPanelState = 1;
            common->Printf("vita: rear touch pad area x %d..%d, y %d..%d: left half = JOY15 (%s), right half = JOY16 (%s); "
                           "outer deadzones %.0f%% / %.0f%% of each half%s\n",
                vitaRearPanel.minAaX, vitaRearPanel.maxAaX, vitaRearPanel.minAaY, vitaRearPanel.maxAaY,
                idKeyInput::GetBinding(K_JOY15), idKeyInput::GetBinding(K_JOY16),
                100.0f * vita_rearTouchLeftDeadzone.GetFloat(), 100.0f * vita_rearTouchRightDeadzone.GetFloat(),
                vita_rearTouchSwap.GetBool() ? ", mirrored" : "");
        }

        SceTouchData data;
        memset(&data, 0, sizeof(data));
        if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &data, 1) >= 0) {
            const float x0 = vitaRearPanel.minAaX;
            const float w = (float)(vitaRearPanel.maxAaX - vitaRearPanel.minAaX);
            const float y0 = vitaRearPanel.minAaY;
            const float h = (float)(vitaRearPanel.maxAaY - vitaRearPanel.minAaY);
            const int reports = Min((int)data.reportNum, (int)(sizeof(data.report) / sizeof(data.report[0])));
            for (int i = 0; i < reports; i++) {
                float nx = idMath::ClampFloat(0.0f, 1.0f, (data.report[i].x - x0) / w);
                const float ny = idMath::ClampFloat(0.0f, 1.0f, (data.report[i].y - y0) / h);
                if (vita_rearTouchSwap.GetBool()) {
                    nx = 1.0f - nx;
                }
                const int half = (nx < 0.5f) ? 0 : 1;
                // 0 at the half's outer edge, 1 at the centre line
                const float inward = (half == 0) ? nx * 2.0f : (1.0f - nx) * 2.0f;
                float dead = (half == 0) ? vita_rearTouchLeftDeadzone.GetFloat() : vita_rearTouchRightDeadzone.GetFloat();
                // a little hysteresis: a held touch drifting on the edge of the
                // deadzone must not stutter the button (which would re-toggle sprint)
                if (vitaRearDown[half]) {
                    dead -= 0.04f;
                }
                const bool counts = inward >= dead;
                if (counts) {
                    want[half] = true;
                }
                if (vita_rearTouchDebug.GetBool()) {
                    common->Printf("vita: rear touch id %d at (%d,%d) = (%.2f,%.2f): %s half, %.0f%% in from its edge, deadzone %.0f%% -> %s\n",
                        data.report[i].id, data.report[i].x, data.report[i].y, nx, ny, half == 0 ? "left" : "right",
                        100.0f * inward, 100.0f * dead, counts ? "PRESS" : "ignored");
                }
            }
        }
    }

    for (int h = 0; h < 2; h++) {
        if (want[h] == vitaRearDown[h]) {
            continue;
        }
        vitaRearDown[h] = want[h];
        const int key = (h == 0) ? K_JOY15 : K_JOY16;
        // gameplay reads joy_polls (usercmd buttons such as _speed), menus and
        // command binds (vita_quickTurn) read the SE_KEY event
        joy_polls.Append(joy_poll_t((h == 0) ? J_ACTION15 : J_ACTION16, want[h] ? 1 : 0));
        QueueJoyKey(key, want[h]);
        common->Printf("vita: rear touch %s half %s (%s)\n", h == 0 ? "left" : "right", want[h] ? "pressed" : "released",
            idKeyInput::GetBinding(key));
    }
}
#endif

// Maps an SDL controller button to the engine's J_* action, and to the K_JOY*
// key the menus listen for. Returns false for buttons the engine has no slot for.
static bool JoyButtonToAction(Uint8 sdlButton, int& action, int& key)
{
    switch (sdlButton) {
    case SDL_CONTROLLER_BUTTON_A:             action = J_ACTION1;  break;
    case SDL_CONTROLLER_BUTTON_B:             action = J_ACTION2;  break;
    case SDL_CONTROLLER_BUTTON_X:             action = J_ACTION3;  break;
    case SDL_CONTROLLER_BUTTON_Y:             action = J_ACTION4;  break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  action = J_ACTION5;  break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: action = J_ACTION6;  break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     action = J_ACTION7;  break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    action = J_ACTION8;  break;
    case SDL_CONTROLLER_BUTTON_START:         action = J_ACTION9;  break;
    case SDL_CONTROLLER_BUTTON_BACK:          action = J_ACTION10; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       action = J_DPAD_UP;    key = K_JOY_DPAD_UP;    return true;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     action = J_DPAD_DOWN;  key = K_JOY_DPAD_DOWN;  return true;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     action = J_DPAD_LEFT;  key = K_JOY_DPAD_LEFT;  return true;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    action = J_DPAD_RIGHT; key = K_JOY_DPAD_RIGHT; return true;
    default:
        return false;
    }
    key = K_JOY1 + (action - J_ACTION1);
    return true;
}

// Maps an SDL controller axis to the engine's J_AXIS_* action plus the pair of
// K_JOY* keys its negative and positive halves stand for. Triggers only have a
// positive half, so negKey comes back as 0.
static bool JoyAxisToAction(Uint8 sdlAxis, int& action, int& negKey, int& posKey)
{
    switch (sdlAxis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
        action = J_AXIS_LEFT_X;  negKey = K_JOY_STICK1_LEFT; posKey = K_JOY_STICK1_RIGHT; return true;
    case SDL_CONTROLLER_AXIS_LEFTY:
        action = J_AXIS_LEFT_Y;  negKey = K_JOY_STICK1_UP;   posKey = K_JOY_STICK1_DOWN;  return true;
    case SDL_CONTROLLER_AXIS_RIGHTX:
        action = J_AXIS_RIGHT_X; negKey = K_JOY_STICK2_LEFT; posKey = K_JOY_STICK2_RIGHT; return true;
    case SDL_CONTROLLER_AXIS_RIGHTY:
        action = J_AXIS_RIGHT_Y; negKey = K_JOY_STICK2_UP;   posKey = K_JOY_STICK2_DOWN;  return true;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        action = J_AXIS_LEFT_TRIG;  negKey = 0; posKey = K_JOY_TRIGGER1; return true;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        action = J_AXIS_RIGHT_TRIG; negKey = 0; posKey = K_JOY_TRIGGER2; return true;
    default:
        return false;
    }
}

static void OpenGameController()
{
    if (gameController != NULL) {
        return;
    }
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        gameController = SDL_GameControllerOpen(i);
        if (gameController != NULL) {
            common->Printf("Gamepad: %s\n", SDL_GameControllerName(gameController));
            return;
        }
    }
}

static void CloseGameController()
{
    if (gameController != NULL) {
        SDL_GameControllerClose(gameController);
        gameController = NULL;
    }
    memset(joyKeyState, 0, sizeof(joyKeyState));
    numPendingJoyKeys = 0;
}

#endif // SDL2

// RB begin
static int SDL_KeyToDoom3Key(SDL_Keycode key, bool& isChar)
{
    isChar = false;

    if (key >= SDLK_SPACE && key < SDLK_DELETE) {
        isChar = true;
        // return key;// & 0xff;
    }

    switch (key) {
    case SDLK_ESCAPE:
        return K_ESCAPE;

    case SDLK_SPACE:
        return K_SPACE;

        // case SDLK_EXCLAIM:
        /*
        SDLK_QUOTEDBL:
        SDLK_HASH:
        SDLK_DOLLAR:
        SDLK_AMPERSAND:
        SDLK_QUOTE		= 39,
        SDLK_LEFTPAREN		= 40,
        SDLK_RIGHTPAREN		= 41,
        SDLK_ASTERISK		= 42,
        SDLK_PLUS		= 43,
        SDLK_COMMA		= 44,
        SDLK_MINUS		= 45,
        SDLK_PERIOD		= 46,
        SDLK_SLASH		= 47,
        */
    case SDLK_0:
        return K_0;

    case SDLK_1:
        return K_1;

    case SDLK_2:
        return K_2;

    case SDLK_3:
        return K_3;

    case SDLK_4:
        return K_4;

    case SDLK_5:
        return K_5;

    case SDLK_6:
        return K_6;

    case SDLK_7:
        return K_7;

    case SDLK_8:
        return K_8;

    case SDLK_9:
        return K_9;

        // DG: add some missing keys..
    case SDLK_UNDERSCORE:
        return K_UNDERLINE;

    case SDLK_MINUS:
        return K_MINUS;

    case SDLK_COMMA:
        return K_COMMA;

    case SDLK_COLON:
        return K_COLON;

    case SDLK_SEMICOLON:
        return K_SEMICOLON;

    case SDLK_PERIOD:
        return K_PERIOD;

    case SDLK_AT:
        return K_AT;

    case SDLK_EQUALS:
        return K_EQUALS;
        // DG end

        /*
        SDLK_COLON		= 58,
        SDLK_SEMICOLON		= 59,
        SDLK_LESS		= 60,
        SDLK_EQUALS		= 61,
        SDLK_GREATER		= 62,
        SDLK_QUESTION		= 63,
        SDLK_AT			= 64,
        */
        /*
           Skip uppercase letters
         */
        /*
        SDLK_LEFTBRACKET	= 91,
        SDLK_BACKSLASH		= 92,
        SDLK_RIGHTBRACKET	= 93,
        SDLK_CARET		= 94,
        SDLK_UNDERSCORE		= 95,
        SDLK_BACKQUOTE		= 96,
        */

    case SDLK_a:
        return K_A;

    case SDLK_b:
        return K_B;

    case SDLK_c:
        return K_C;

    case SDLK_d:
        return K_D;

    case SDLK_e:
        return K_E;

    case SDLK_f:
        return K_F;

    case SDLK_g:
        return K_G;

    case SDLK_h:
        return K_H;

    case SDLK_i:
        return K_I;

    case SDLK_j:
        return K_J;

    case SDLK_k:
        return K_K;

    case SDLK_l:
        return K_L;

    case SDLK_m:
        return K_M;

    case SDLK_n:
        return K_N;

    case SDLK_o:
        return K_O;

    case SDLK_p:
        return K_P;

    case SDLK_q:
        return K_Q;

    case SDLK_r:
        return K_R;

    case SDLK_s:
        return K_S;

    case SDLK_t:
        return K_T;

    case SDLK_u:
        return K_U;

    case SDLK_v:
        return K_V;

    case SDLK_w:
        return K_W;

    case SDLK_x:
        return K_X;

    case SDLK_y:
        return K_Y;

    case SDLK_z:
        return K_Z;

    case SDLK_RETURN:
        return K_ENTER;

    case SDLK_BACKSPACE:
        return K_BACKSPACE;

    case SDLK_PAUSE:
        return K_PAUSE;

        // DG: add tab key support
    case SDLK_TAB:
        return K_TAB;
        // DG end

        // case SDLK_APPLICATION:
        //	return K_COMMAND;
    case SDLK_CAPSLOCK:
        return K_CAPSLOCK;

    case SDLK_SCROLLLOCK:
        return K_SCROLL;

    case SDLK_POWER:
        return K_POWER;

    case SDLK_UP:
        return K_UPARROW;

    case SDLK_DOWN:
        return K_DOWNARROW;

    case SDLK_LEFT:
        return K_LEFTARROW;

    case SDLK_RIGHT:
        return K_RIGHTARROW;

    case SDLK_LGUI:
        return K_LWIN;

    case SDLK_RGUI:
        return K_RWIN;
        // case SDLK_MENU:
        //	return K_MENU;

    case SDLK_LALT:
        return K_LALT;

    case SDLK_RALT:
        return K_RALT;

    case SDLK_RCTRL:
        return K_RCTRL;

    case SDLK_LCTRL:
        return K_LCTRL;

    case SDLK_RSHIFT:
        return K_RSHIFT;

    case SDLK_LSHIFT:
        return K_LSHIFT;

    case SDLK_INSERT:
        return K_INS;

    case SDLK_DELETE:
        return K_DEL;

    case SDLK_PAGEDOWN:
        return K_PGDN;

    case SDLK_PAGEUP:
        return K_PGUP;

    case SDLK_HOME:
        return K_HOME;

    case SDLK_END:
        return K_END;

    case SDLK_F1:
        return K_F1;

    case SDLK_F2:
        return K_F2;

    case SDLK_F3:
        return K_F3;

    case SDLK_F4:
        return K_F4;

    case SDLK_F5:
        return K_F5;

    case SDLK_F6:
        return K_F6;

    case SDLK_F7:
        return K_F7;

    case SDLK_F8:
        return K_F8;

    case SDLK_F9:
        return K_F9;

    case SDLK_F10:
        return K_F10;

    case SDLK_F11:
        return K_F11;

    case SDLK_F12:
        return K_F12;
        // K_INVERTED_EXCLAMATION;

    case SDLK_F13:
        return K_F13;

    case SDLK_F14:
        return K_F14;

    case SDLK_F15:
        return K_F15;

    case SDLK_KP_7:
        return K_KP_7;

    case SDLK_KP_8:
        return K_KP_8;

    case SDLK_KP_9:
        return K_KP_9;

    case SDLK_KP_4:
        return K_KP_4;

    case SDLK_KP_5:
        return K_KP_5;

    case SDLK_KP_6:
        return K_KP_6;

    case SDLK_KP_1:
        return K_KP_1;

    case SDLK_KP_2:
        return K_KP_2;

    case SDLK_KP_3:
        return K_KP_3;

    case SDLK_KP_ENTER:
        return K_KP_ENTER;

    case SDLK_KP_0:
        return K_KP_0;

    case SDLK_KP_PERIOD:
        return K_KP_DOT;

    case SDLK_KP_DIVIDE:
        return K_KP_SLASH;
        // K_SUPERSCRIPT_TWO;

    case SDLK_KP_MINUS:
        return K_KP_MINUS;
        // K_ACUTE_ACCENT;

    case SDLK_KP_PLUS:
        return K_KP_PLUS;

    case SDLK_NUMLOCKCLEAR:
        return K_NUMLOCK;

    case SDLK_KP_MULTIPLY:
        return K_KP_STAR;

    case SDLK_KP_EQUALS:
        return K_KP_EQUALS;

        // K_MASCULINE_ORDINATOR;
        // K_GRAVE_A;
        // K_AUX1;
        // K_CEDILLA_C;
        // K_GRAVE_E;
        // K_AUX2;
        // K_AUX3;
        // K_AUX4;
        // K_GRAVE_I;
        // K_AUX5;
        // K_AUX6;
        // K_AUX7;
        // K_AUX8;
        // K_TILDE_N;
        // K_GRAVE_O;
        // K_AUX9;
        // K_AUX10;
        // K_AUX11;
        // K_AUX12;
        // K_AUX13;
        // K_AUX14;
        // K_GRAVE_U;
        // K_AUX15;
        // K_AUX16;

    case SDLK_PRINTSCREEN:
        return K_PRINTSCREEN;

    case SDLK_MODE:
        return K_RALT;
    }

    return 0;
}
// RB end

static void PushConsoleEvent(const char* s)
{
    char* b;
    size_t len;

    len = strlen(s) + 1;
    b = (char*)Mem_Alloc(len, TAG_EVENTS);
    strcpy(b, s);

    SDL_Event event;

    event.type = SDL_USEREVENT;
    event.user.code = SE_CONSOLE;
    event.user.data1 = (void*)len;
    event.user.data2 = b;

    SDL_PushEvent(&event);
}

/*
=================
Sys_InitInput
=================
*/
void Sys_InitInput()
{
    kbd_polls.SetGranularity(64);
    mouse_polls.SetGranularity(64);

#if SDL_VERSION_ATLEAST(2, 0, 0)
    joy_polls.SetGranularity(64);
    memset(joyKeyState, 0, sizeof(joyKeyState));
    memset(joyAxisPercent, 0, sizeof(joyAxisPercent));
    numPendingJoyKeys = 0;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        common->Warning("Could not init SDL game controller subsystem: %s", SDL_GetError());
    } else {
        OpenGameController();
    }

    // The Vita's front and rear touchpads emit these continuously and nothing
    // in the engine reads them, so keep them out of the queue entirely rather
    // than paying to enqueue, gesture-process and then discard each one.
    //
    // The raw SDL_JOY* events are NOT suppressed this way even though they are
    // pure duplicates of the GameController ones: SDL_PrivateJoystickAxis()
    // checks SDL_GetEventState() before it pushes, and the GameController
    // layer is an event *watcher* on that push, so ignoring the raw event
    // would stop the translated one from ever being generated. They are
    // swallowed in the Sys_GetEvent() switch instead.
    static const Uint32 unusedEvents[] = {
        SDL_FINGERDOWN, SDL_FINGERUP, SDL_FINGERMOTION,
        SDL_MULTIGESTURE, SDL_DOLLARGESTURE, SDL_DOLLARRECORD
    };
    for (size_t i = 0; i < sizeof(unusedEvents) / sizeof(unusedEvents[0]); i++) {
        SDL_EventState(unusedEvents[i], SDL_IGNORE);
    }
#if defined(__vita__)
    // and no fake mouse clicks from either touch panel: K_MOUSE1 is bound to
    // _attack by default.cfg, so a palm on the screen would fire the gun
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
#endif

#if !SDL_VERSION_ATLEAST(2, 0, 0)
    SDL_EnableUNICODE(1);
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL);
#endif

    in_keyboard.SetModified();
}

/*
=================
Sys_ShutdownInput
=================
*/
void Sys_ShutdownInput()
{
    kbd_polls.Clear();
    mouse_polls.Clear();

#if SDL_VERSION_ATLEAST(2, 0, 0)
    CloseGameController();
    joy_polls.Clear();
#endif
}

/*
===========
Sys_InitScanTable
===========
*/
// Windows has its own version due to the tools
#ifndef _WIN32
void Sys_InitScanTable()
{
}
#endif

char* Sys_GetClipboardData()
{
    char* txt = SDL_GetClipboardText();

    if (txt == NULL) {
        return NULL;
    } else if (txt[0] == '\0') {
        SDL_free(txt);
        return NULL;
    }

    char* ret = Mem_CopyString(txt);
    SDL_free(txt);

    return ret;
}

void Sys_SetClipboardData(const char* string)
{
    SDL_SetClipboardText(string);
}

/*
===============
Sys_GetConsoleKey
===============
*/
unsigned char Sys_GetConsoleKey(bool shifted)
{
    static unsigned char keys[2] = { '`', '~' };

    if (in_keyboard.IsModified()) {
        idStr lang = in_keyboard.GetString();

        if (lang.Length()) {
            if (!lang.Icmp("french")) {
                keys[0] = '<';
                keys[1] = '>';
            } else if (!lang.Icmp("german")) {
                keys[0] = '^';
                keys[1] = 176; // °
            } else if (!lang.Icmp("italian")) {
                keys[0] = '\\';
                keys[1] = '|';
            } else if (!lang.Icmp("spanish")) {
                keys[0] = 186; // º
                keys[1] = 170; // ª
            } else if (!lang.Icmp("turkish")) {
                keys[0] = '"';
                keys[1] = 233; // é
            } else if (!lang.Icmp("norwegian")) {
                keys[0] = 124; // |
                keys[1] = 167; // §
            }
        }

        in_keyboard.ClearModified();
    }

    return shifted ? keys[1] : keys[0];
}

/*
===============
Sys_MapCharForKey
===============
*/
unsigned char Sys_MapCharForKey(int key)
{
    return key & 0xff;
}

/*
===============
Sys_GrabMouseCursor
===============
*/
void Sys_GrabMouseCursor(bool grabIt)
{
    int flags;

    if (grabIt) {
        // DG: disabling the cursor is now done once in GLimp_Init() because it should always be disabled
        flags = GRAB_ENABLE | GRAB_SETSTATE;
        // DG end
    } else {
        flags = GRAB_SETSTATE;
    }

    GLimp_GrabInput(flags);
}

/*
================
Sys_GetEvent
================
*/
// Deferred events: produced while handling some other SDL event, and handed
// back on a later call because Sys_GetEvent() can only return one at a time.
// These live at file scope so PopDeferredEvent() can drain them both before
// and after the SDL queue is polled.
#if SDL_VERSION_ATLEAST(2, 0, 0)
static char* pendingChars = NULL; // SDL_TEXTINPUT string, one SE_CHAR per call
static size_t pendingCharPos = 0;
static int mwheelRel = 0;         // synthetic "mousewheel released" for SDL2
#endif
static byte pendingChar = 0;      // single SE_CHAR (console key, backspace)

/*
================
PopDeferredEvent

Returns true and fills res if a synthetic event is waiting. The order matches
the sequence the original inline checks ran in.
================
*/
static bool PopDeferredEvent(sysEvent_t& res)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
    if (pendingChars) {
        res.evType = SE_CHAR;
        res.evValue = pendingChars[pendingCharPos];

        pendingCharPos++;
        if (!pendingChars[pendingCharPos]) {
            free(pendingChars);
            pendingChars = NULL;
            pendingCharPos = 0;
        }
        return true;
    }

    // DG: fake a "mousewheel not pressed anymore" event for SDL2
    // so scrolling in menus stops after one step
    if (mwheelRel) {
        res.evType = SE_KEY;
        res.evValue = mwheelRel;
        res.evValue2 = 0; // "not pressed anymore"
        mwheelRel = 0;
        return true;
    }
    // DG end

    // Synthetic K_JOY* presses from axis crossings.
    if (numPendingJoyKeys > 0) {
        res = pendingJoyKeys[0];
        numPendingJoyKeys--;
        for (int i = 0; i < numPendingJoyKeys; i++) {
            pendingJoyKeys[i] = pendingJoyKeys[i + 1];
        }
        return true;
    }
#endif

    if (pendingChar) {
        res.evType = SE_CHAR;
        res.evValue = pendingChar;
        pendingChar = 0;
        return true;
    }

    return false;
}

sysEvent_t Sys_GetEvent()
{
    SDL_Event ev;
    sysEvent_t res = {};
    int key;

    static const sysEvent_t res_none = { SE_NONE, 0, 0, 0, NULL };

    if (PopDeferredEvent(res)) {
        return res;
    }

    // This used to poll a single SDL event per call and return SE_NONE for any
    // event the engine has no use for - raw joystick duplicates, touchpad
    // motion, an axis that moved but not far enough to change the quantised
    // value the menus read. idEventLoop::RunEventLoop() stops the moment it
    // sees SE_NONE, so every one of those cost a whole frame of draining: the
    // pad pushes hundreds of events a second and the engine removed about one
    // per frame. The backlog grew without bound, which is why input started
    // out responsive and got steadily worse the longer the game ran.
    //
    // Now the uninteresting cases continue instead, so SE_NONE is only
    // returned once the SDL queue is genuinely empty.
    bool deferred = false;

    while (!deferred && SDL_PollEvent(&ev)) {
        switch (ev.type) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
        case SDL_WINDOWEVENT:
            switch (ev.window.event) {
            case SDL_WINDOWEVENT_FOCUS_GAINED: {
                // unset modifier, in case alt-tab was used to leave window and ALT is still set
                // as that can cause fullscreen-toggling when pressing enter...
                SDL_Keymod currentmod = SDL_GetModState();
                int newmod = KMOD_NONE;
                if (currentmod & KMOD_CAPS) // preserve capslock
                    newmod |= KMOD_CAPS;

                SDL_SetModState((SDL_Keymod)newmod);

                // DG: un-pause the game when focus is gained, that also re-grabs the input
                //     disabling the cursor is now done once in GLimp_Init() because it should always be disabled
                cvarSystem->SetCVarBool("com_pause", false);
                // DG end
                break;
            }

            case SDL_WINDOWEVENT_FOCUS_LOST:
                // DG: pause the game when focus is lost, that also un-grabs the input
                cvarSystem->SetCVarBool("com_pause", true);
                // DG end
                break;

                // DG: handle resizing and moving of window
            case SDL_WINDOWEVENT_RESIZED: {
                int w = ev.window.data1;
                int h = ev.window.data2;
                r_windowWidth.SetInteger(w);
                r_windowHeight.SetInteger(h);

                glConfig.nativeScreenWidth = w;
                glConfig.nativeScreenHeight = h;
                break;
            }

            case SDL_WINDOWEVENT_MOVED: {
                int x = ev.window.data1;
                int y = ev.window.data2;
                r_windowX.SetInteger(x);
                r_windowY.SetInteger(y);
                break;
            }
                // DG end
            }

            continue;
#else
        case SDL_ACTIVEEVENT: {
            // DG: (un-)pause the game when focus is gained, that also (un-)grabs the input
            bool pause = true;

            if (ev.active.gain) {

                pause = false;

                // unset modifier, in case alt-tab was used to leave window and ALT is still set
                // as that can cause fullscreen-toggling when pressing enter...
                SDLMod currentmod = SDL_GetModState();
                int newmod = KMOD_NONE;
                if (currentmod & KMOD_CAPS) // preserve capslock
                    newmod |= KMOD_CAPS;

                SDL_SetModState((SDLMod)newmod);
            }

            cvarSystem->SetCVarBool("com_pause", pause);
        }

            continue;

        case SDL_VIDEOEXPOSE:
            continue;

            // DG: handle resizing and moving of window
        case SDL_VIDEORESIZE: {
            int w = ev.resize.w;
            int h = ev.resize.h;
            r_windowWidth.SetInteger(w);
            r_windowHeight.SetInteger(h);

            glConfig.nativeScreenWidth = w;
            glConfig.nativeScreenHeight = h;
            // for some reason this needs a vid_restart in SDL1 but not SDL2 so GLimp_SetScreenParms() is called
            PushConsoleEvent("vid_restart");
            continue;
        }
        // DG end
#endif

        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT) > 0) {
                // DG: go to fullscreen on current display, instead of always first display
                int fullscreen = 0;
                if (!renderSystem->IsFullScreen()) {
                    // this will be handled as "fullscreen on current window"
                    // r_fullscreen 1 means "fullscreen on first window" in d3 bfg
                    fullscreen = -2;
                }
                cvarSystem->SetCVarInteger("r_fullscreen", fullscreen);
                // DG end
                PushConsoleEvent("vid_restart");
                continue;
            }

            // DG: ctrl-g to un-grab mouse - yeah, left ctrl shoots, then just use right ctrl :)
            if (ev.key.keysym.sym == SDLK_g && (ev.key.keysym.mod & KMOD_CTRL) > 0) {
                bool grab = cvarSystem->GetCVarBool("in_nograb");
                grab = !grab;
                cvarSystem->SetCVarBool("in_nograb", grab);
                continue;
            }
            // DG end

#if !SDL_VERSION_ATLEAST(2, 0, 0)
            // DG: only do this for key-down, don't care about isChar from SDL_KeyToDoom3Key.
            //     if unicode is not 0 and is translatable to ASCII it should work..
            if (ev.key.state == SDL_PRESSED && (ev.key.keysym.unicode & 0xff80) == 0) {
                pendingChar = ev.key.keysym.unicode & 0x7f;
            }
            // DG end
#endif

            // fall through
        case SDL_KEYUP: {
            bool isChar;

            // DG: special case for SDL_SCANCODE_GRAVE - the console key under Esc
            if (ev.key.keysym.scancode == SDL_SCANCODE_GRAVE) {
                key = K_GRAVE;
                pendingChar = K_BACKSPACE; // bad hack to get empty console inputline..
            } // DG end, the original code is in the else case
            else {
                key = SDL_KeyToDoom3Key(ev.key.keysym.sym, isChar);

                if (key == 0) {
#if SDL_VERSION_ATLEAST(2, 0, 0)
                    // SDL2 has no ev.key.keysym.unicode anymore.. but the scancode should work well enough for console
                    if (ev.type == SDL_KEYDOWN) // FIXME: don't complain if this was an ASCII char and the console is open?
                        common->Warning("unmapped SDL key %d scancode %d", ev.key.keysym.sym, ev.key.keysym.scancode);

                    continue;
#else
                    unsigned char uc = ev.key.keysym.unicode & 0xff;
                    // check if its an unmapped console key
                    if (uc == Sys_GetConsoleKey(false) || uc == Sys_GetConsoleKey(true)) {
                        key = K_GRAVE;
                        pendingChar = K_BACKSPACE; // bad hack to get empty console inputline..
                    } else {
                        if (ev.type == SDL_KEYDOWN) // FIXME: don't complain if this was an ASCII char and the console is open?
                            common->Warning("unmapped SDL key %d (0x%x) scancode %d", ev.key.keysym.sym, ev.key.keysym.unicode, ev.key.keysym.scancode);
                        continue;
                    }
#endif
                }
            }

            res.evType = SE_KEY;
            res.evValue = key;
            res.evValue2 = ev.key.state == SDL_PRESSED ? 1 : 0;

            kbd_polls.Append(kbd_poll_t(key, ev.key.state == SDL_PRESSED));

            if (key == K_BACKSPACE && ev.key.state == SDL_PRESSED)
                pendingChar = key;

            return res;
        }

#if SDL_VERSION_ATLEAST(2, 0, 0)
        case SDL_TEXTINPUT:
            if (ev.text.text && *ev.text.text) {
                if (!ev.text.text[1])
                    pendingChar = *ev.text.text;
                else
                    pendingChars = strdup(ev.text.text);

                // A second SDL_TEXTINPUT in the same drain would overwrite
                // (and leak) what was just stashed, so stop here and let the
                // post-loop PopDeferredEvent() hand this one back first.
                deferred = true;
            }

            continue;
#endif

        case SDL_MOUSEMOTION:
            // DG: return event with absolute mouse-coordinates when in menu
            // to fix cursor problems in windowed mode
            if (game && game->Shell_IsActive()) {
                res.evType = SE_MOUSE_ABSOLUTE;
                res.evValue = ev.motion.x;
                res.evValue2 = ev.motion.y;
            } else // this is the old, default behavior
            {
                res.evType = SE_MOUSE;
                res.evValue = ev.motion.xrel;
                res.evValue2 = ev.motion.yrel;
            }
            // DG end

            mouse_polls.Append(mouse_poll_t(M_DELTAX, ev.motion.xrel));
            mouse_polls.Append(mouse_poll_t(M_DELTAY, ev.motion.yrel));

            return res;

#if SDL_VERSION_ATLEAST(2, 0, 0)
        case SDL_MOUSEWHEEL:
            res.evType = SE_KEY;

            if (ev.wheel.y > 0) {
                res.evValue = K_MWHEELUP;
                mouse_polls.Append(mouse_poll_t(M_DELTAZ, 1));
            } else {
                res.evValue = K_MWHEELDOWN;
                mouse_polls.Append(mouse_poll_t(M_DELTAZ, -1));
            }

            // DG: remember mousewheel direction to issue a "not pressed anymore" event
            mwheelRel = res.evValue;
            // DG end

            res.evValue2 = 1;

            return res;
#endif

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            res.evType = SE_KEY;

            switch (ev.button.button) {
            case SDL_BUTTON_LEFT:
                res.evValue = K_MOUSE1;
                mouse_polls.Append(mouse_poll_t(M_ACTION1, ev.button.state == SDL_PRESSED ? 1 : 0));
                break;
            case SDL_BUTTON_MIDDLE:
                res.evValue = K_MOUSE3;
                mouse_polls.Append(mouse_poll_t(M_ACTION3, ev.button.state == SDL_PRESSED ? 1 : 0));
                break;
            case SDL_BUTTON_RIGHT:
                res.evValue = K_MOUSE2;
                mouse_polls.Append(mouse_poll_t(M_ACTION2, ev.button.state == SDL_PRESSED ? 1 : 0));
                break;

#if !SDL_VERSION_ATLEAST(2, 0, 0)
            case SDL_BUTTON_WHEELUP:
                res.evValue = K_MWHEELUP;
                if (ev.button.state == SDL_PRESSED)
                    mouse_polls.Append(mouse_poll_t(M_DELTAZ, 1));
                break;
            case SDL_BUTTON_WHEELDOWN:
                res.evValue = K_MWHEELDOWN;
                if (ev.button.state == SDL_PRESSED)
                    mouse_polls.Append(mouse_poll_t(M_DELTAZ, -1));
                break;
#endif
            }

            res.evValue2 = ev.button.state == SDL_PRESSED ? 1 : 0;

            return res;

        case SDL_CONTROLLERDEVICEADDED:
            OpenGameController();
            continue;

        case SDL_CONTROLLERDEVICEREMOVED:
            if (gameController != NULL
                && ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gameController))) {
                CloseGameController();
            }
            continue;

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            int action = 0;
            int key = 0;
            if (!JoyButtonToAction(ev.cbutton.button, action, key)) {
                continue;
            }

            const bool down = (ev.cbutton.state == SDL_PRESSED);
            joy_polls.Append(joy_poll_t(action, down ? 1 : 0));

            const int idx = key - K_JOY1;
            if (idx >= 0 && idx < (int)(sizeof(joyKeyState) / sizeof(joyKeyState[0]))) {
                joyKeyState[idx] = down;
            }

            // UsercmdGen picks the button up from joy_polls via Joystick();
            // this event is what the menus and console read.
            res.evType = SE_KEY;
            res.evValue = key;
            res.evValue2 = down ? 1 : 0;
            return res;
        }

        case SDL_CONTROLLERAXISMOTION: {
            int action = 0;
            int negKey = 0;
            int posKey = 0;
            if (!JoyAxisToAction(ev.caxis.axis, action, negKey, posKey)) {
                continue;
            }

            const int value = ev.caxis.value;

            // idUsercmdGenLocal::Joystick() handles an axis poll as a plain
            // "set this axis to this value", so position in the list does not
            // matter and only the newest sample of each axis survives anyway.
            // Overwriting the existing entry keeps the list at six entries
            // instead of however many samples a frame's worth of stick jitter
            // produced. Buttons are never merged - Key() has edge semantics,
            // so a press and its release have to stay as two ordered entries.
            int existing = -1;
            for (int i = joy_polls.Num() - 1; i >= 0; i--) {
                if (joy_polls[i].action == action) {
                    existing = i;
                    break;
                }
            }
            if (existing >= 0) {
                joy_polls[existing].value = value;
            } else {
                joy_polls.Append(joy_poll_t(action, value));
            }

            // Let the stick drive menus as well. These are queued rather than
            // returned because one axis move can release one key and press another.
            if (negKey != 0) {
                QueueJoyKey(negKey, value < -JOY_KEY_THRESHOLD);
            }
            QueueJoyKey(posKey, value > JOY_KEY_THRESHOLD);

            // Menus read SE_JOYSTICK and scale evValue2 by 1/32 (SWF_Events.cpp),
            // so it has to carry the same quantised -16..16 that win_input.cpp
            // sends, not the raw axis. Only emit when that quantised value
            // actually changes, otherwise a resting stick floods the queue.
            const int axis = action - J_AXIS_MIN;
            const int percent = (value * 16) / 32768;
            if (axis >= 0 && axis < MAX_JOYSTICK_AXIS && joyAxisPercent[axis] != percent) {
                joyAxisPercent[axis] = percent;
                res.evType = SE_JOYSTICK;
                res.evValue = axis;
                res.evValue2 = percent;
                return res;
            }
            continue;
        }

        // SDL reports every pad movement twice: once through the GameController
        // layer we actually read above, and once as the raw joystick event
        // underneath it. Swallow the raw ones. Warning about them costs a full
        // open/write/close of ux0:data/doom3bfg/console.txt per axis sample,
        // which is hundreds of file operations a second on a resting stick.
        case SDL_JOYAXISMOTION:
        case SDL_JOYBALLMOTION:
        case SDL_JOYHATMOTION:
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
            continue;

        // The Vita's front and rear touchpads emit these continuously, and the
        // engine does not use them. Falling through to the default case below
        // logged one warning per event - hundreds of console.txt writes a
        // second - and the event loop spent its time blocked on the SD card
        // instead of delivering input, which is why the controller felt
        // massively delayed. Same trap as the joystick axes above.
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_FINGERMOTION:
        case SDL_MULTIGESTURE:
        case SDL_DOLLARGESTURE:
        case SDL_DOLLARRECORD:
            continue;

        case SDL_QUIT:
            PushConsoleEvent("quit");
            continue;

        case SDL_USEREVENT:
            switch (ev.user.code) {
            case SE_CONSOLE:
                res.evType = SE_CONSOLE;
                res.evPtrLength = (intptr_t)ev.user.data1;
                res.evPtr = ev.user.data2;
                return res;
            default:
                common->Warning("unknown user event %u", ev.user.code);
                continue;
            }
        default:
            // Rate-limited deliberately. Every warning is a separate
            // open/write/close of console.txt on this platform, so an event
            // type that arrives every frame turns a diagnostic into a hang.
            // Report each type once and then stay quiet.
            {
                static idList<unsigned int> reported;
                if (reported.Find(ev.type) == NULL && reported.Num() < 16) {
                    reported.Append(ev.type);
                    common->Warning("unknown event %u (further reports of this type suppressed)", ev.type);
                }
            }
            continue;
        }
    }

    // Anything queued while draining (text input, joystick threshold keys) is
    // delivered here instead of waiting for the next call.
    if (PopDeferredEvent(res)) {
        return res;
    }

    return res_none;
}

/*
================
Sys_ClearEvents
================
*/
void Sys_ClearEvents()
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev))
        ;

    kbd_polls.SetNum(0);
    mouse_polls.SetNum(0);
}

/*
================
Sys_GenerateEvents
================
*/
void Sys_GenerateEvents()
{
    char* s = Posix_ConsoleInput();

    if (s)
        PushConsoleEvent(s);

    SDL_PumpEvents();
}

/*
================
Sys_PollKeyboardInputEvents
================
*/
int Sys_PollKeyboardInputEvents()
{
    return kbd_polls.Num();
}

/*
================
Sys_ReturnKeyboardInputEvent
================
*/
int Sys_ReturnKeyboardInputEvent(const int n, int& key, bool& state)
{
    if (n >= kbd_polls.Num())
        return 0;

    key = kbd_polls[n].key;
    state = kbd_polls[n].state;
    return 1;
}

/*
================
Sys_EndKeyboardInputEvents
================
*/
void Sys_EndKeyboardInputEvents()
{
    kbd_polls.SetNum(0);
}

/*
================
Sys_PollMouseInputEvents
================
*/
int Sys_PollMouseInputEvents(int mouseEvents[MAX_MOUSE_EVENTS][2])
{
    int numEvents = mouse_polls.Num();

    if (numEvents > MAX_MOUSE_EVENTS) {
        numEvents = MAX_MOUSE_EVENTS;
    }

    for (int i = 0; i < numEvents; i++) {
        const mouse_poll_t& mp = mouse_polls[i];

        mouseEvents[i][0] = mp.action;
        mouseEvents[i][1] = mp.value;
    }

    mouse_polls.SetNum(0);

    return numEvents;
}

//=====================================================================================
//	Joystick Input Handling
//=====================================================================================

void Sys_SetRumble(int device, int low, int hi)
{
#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (gameController != NULL) {
        // The engine passes 0..65535 per motor, which is what SDL wants too.
        SDL_GameControllerRumble(gameController, (Uint16)low, (Uint16)hi, 100);
    }
#endif
}

int Sys_PollJoystickInputEvents(int deviceNum)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
    // Only one pad is tracked; the Vita has exactly one and the desktop builds
    // have never used more.
    if (deviceNum != 0 || gameController == NULL) {
        return 0;
    }
#if defined(__vita__)
    PollVitaRearTouch();
#endif
    return joy_polls.Num();
#else
    return 0;
#endif
}

int Sys_ReturnJoystickInputEvent(const int n, int& action, int& value)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
    if (n < 0 || n >= joy_polls.Num()) {
        return 0;
    }
    action = joy_polls[n].action;
    value = joy_polls[n].value;
    return 1;
#else
    return 0;
#endif
}

void Sys_EndJoystickInputEvents()
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
    joy_polls.SetNum(0);
#endif
}
