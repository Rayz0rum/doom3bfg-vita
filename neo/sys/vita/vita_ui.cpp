/*
===========================================================================

PS Vita UI: button glyph atlas, the Vita control scheme and the Controller
Layout overlay. See vita_ui.h.

===========================================================================
*/
#pragma hdrstop
#include "../../idlib/precompiled.h"
#include "../../renderer/tr_local.h"
#include "../../renderer/Font.h"
#include "../../libs/zlib/zlib.h"

#include "vita_ui.h"

/*
================================================================================
Atlas image and material
================================================================================
*/

static void R_VitaUiAtlas(idImage* image)
{
    const int w = vitaUiAtlasWidth;
    const int h = vitaUiAtlasHeight;
    const uLongf expected = (uLongf)w * h * 4;
    byte* pic = (byte*)Mem_Alloc(expected, TAG_IMAGE);
    uLongf got = expected;
    const int err = uncompress(pic, &got, vitaUiAtlasCompressed, vitaUiAtlasCompressedSize);
    if (err != Z_OK || got != expected) {
        common->Warning("vita: UI atlas failed to inflate (zlib error %d, %lu of %lu bytes) - Vita button glyphs will be blank",
            err, (unsigned long)got, (unsigned long)expected);
        memset(pic, 0, expected);
    } else {
        common->Printf("vita: UI atlas %dx%d inflated (%u -> %lu bytes): %d glyphs, %d stock SWF prompts mapped to Vita buttons\n",
            w, h, vitaUiAtlasCompressedSize, (unsigned long)got, vitaUiNumGlyphs, vitaUiNumSwfGlyphs);
    }
    // uncompressed RGBA, no mips: the glyphs are drawn close to 1:1
    image->GenerateImage(pic, w, h, TF_LINEAR, TR_CLAMP, TD_LOOKUP_TABLE_RGBA);
    Mem_Free(pic);
}

void Vita_UI_CreateImages()
{
    globalImages->ImageFromFunction("_vitaUiAtlas", R_VitaUiAtlas);
}

const idMaterial* Vita_UI_Material()
{
    static const idMaterial* material = NULL;
    if (material == NULL && declManager != NULL) {
        // implicit material: "blend blend, colored, map _vitaUiAtlas, clamp"
        material = declManager->FindMaterial("_vitaUiAtlas");
    }
    return material;
}

/*
================================================================================
Glyph lookup
================================================================================
*/

int Vita_UI_FindGlyph(const char* name)
{
    for (int i = 0; i < vitaUiNumGlyphs; i++) {
        if (idStr::Icmp(vitaUiGlyphs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

const vitaUiGlyph_t* Vita_UI_Glyph(int index)
{
    if (index < 0 || index >= vitaUiNumGlyphs) {
        return NULL;
    }
    return &vitaUiGlyphs[index];
}

void Vita_UI_GlyphST(int index, float& s1, float& t1, float& s2, float& t2)
{
    const vitaUiGlyph_t* g = Vita_UI_Glyph(index);
    if (g == NULL) {
        s1 = t1 = s2 = t2 = 0.0f;
        return;
    }
    // glyphs sit 1 px apart on a transparent background, so the exact
    // rectangle is safe to sample with bilinear filtering
    s1 = (float)g->x / vitaUiAtlasWidth;
    t1 = (float)g->y / vitaUiAtlasHeight;
    s2 = (float)(g->x + g->w) / vitaUiAtlasWidth;
    t2 = (float)(g->y + g->h) / vitaUiAtlasHeight;
}

int Vita_UI_SwfGlyph(const char* swfName, int x, int y, int w, int h)
{
    for (int i = 0; i < vitaUiNumSwfGlyphs; i++) {
        const vitaUiSwfGlyph_t& s = vitaUiSwfGlyphs[i];
        if (s.x == x && s.y == y && s.w == w && s.h == h && idStr::Icmp(s.swf, swfName) == 0) {
            return Vita_UI_FindGlyph(s.glyph);
        }
    }
    return -1;
}

int Vita_UI_GlyphForKey(int keyNum)
{
    static int cache[K_LAST_KEY];
    static bool cached = false;
    if (!cached) {
        for (int i = 0; i < K_LAST_KEY; i++) {
            cache[i] = -1;
        }
        // SDL's PSVita mapping: A = Cross, B = Circle, X = Square, Y = Triangle
        cache[K_JOY1] = Vita_UI_FindGlyph("face_cross");
        cache[K_JOY2] = Vita_UI_FindGlyph("face_circle");
        cache[K_JOY3] = Vita_UI_FindGlyph("face_square");
        cache[K_JOY4] = Vita_UI_FindGlyph("face_triangle");
        cache[K_JOY5] = Vita_UI_FindGlyph("l_49x35");
        cache[K_JOY6] = Vita_UI_FindGlyph("r_49x35");
        cache[K_JOY_TRIGGER1] = Vita_UI_FindGlyph("l_43x43");
        cache[K_JOY_TRIGGER2] = Vita_UI_FindGlyph("r_43x43");
        cache[K_JOY9] = Vita_UI_FindGlyph("start_88x55");
        cache[K_JOY10] = Vita_UI_FindGlyph("select_88x55");
        cache[K_JOY15] = Vita_UI_FindGlyph("rear_left");
        cache[K_JOY16] = Vita_UI_FindGlyph("rear_right");
        cache[K_JOY_DPAD_UP] = Vita_UI_FindGlyph("dpad_up");
        cache[K_JOY_DPAD_DOWN] = Vita_UI_FindGlyph("dpad_down");
        cache[K_JOY_DPAD_LEFT] = Vita_UI_FindGlyph("dpad_left");
        cache[K_JOY_DPAD_RIGHT] = Vita_UI_FindGlyph("dpad_right");
        const int ls = Vita_UI_FindGlyph("lstick_52x52");
        const int rs = Vita_UI_FindGlyph("rstick_52x52");
        cache[K_JOY_STICK1_UP] = cache[K_JOY_STICK1_DOWN] = cache[K_JOY_STICK1_LEFT] = cache[K_JOY_STICK1_RIGHT] = ls;
        cache[K_JOY_STICK2_UP] = cache[K_JOY_STICK2_DOWN] = cache[K_JOY_STICK2_LEFT] = cache[K_JOY_STICK2_RIGHT] = rs;
        cached = true;
    }
    if (keyNum < 0 || keyNum >= K_LAST_KEY) {
        return -1;
    }
    return cache[keyNum];
}

int Vita_UI_GlyphForKeyName(const char* keyName)
{
    const keyNum_t key = idKeyInput::StringToKeyNum(keyName);
    if (key == K_NONE) {
        return -1;
    }
    return Vita_UI_GlyphForKey(key);
}

/*
================================================================================
The Vita control scheme

Left stick      move                     Right stick   camera / aim
R               primary fire / melee     L             armour flashlight
D-pad up        quick-slot 1 (grenades)  D-pad down    quick-slot 2 (BFG 9000)
D-pad left      previous weapon          D-pad right   next weapon
Cross           jump                     Circle        crouch
Square          reload                   Triangle      use / interact
Start           pause menu               Select        PDA / logs
Rear touch, right half: toggle sprint    left half:    180 degree quick turn

This is the definitive scheme: it is bound whenever the profile loads or its
config is executed, and the menus offer no way to change it (no layout
selector, no lefty flip, no key bindings screen). The quick slots are the
stock BFG D-pad slots, so they keep the stock weapons (grenades up, BFG down).
================================================================================
*/

void Vita_ApplyControlScheme()
{
    // nothing of the 360 layout (stick clicks, triggers, ...) may survive
    for (int k = K_JOY1; k <= K_JOY_DPAD_RIGHT; k++) {
        idKeyInput::SetBinding(k, "");
    }

    const char* stick1[4] = { "_forward", "_back", "_moveLeft", "_moveRight" };
    const char* stick2[4] = { "_lookUp", "_lookDown", "_left", "_right" };

    const struct {
        int key;
        const char* binding;
    } scheme[] = {
        { K_JOY6, "_attack" },
        { K_JOY5, "_impulse16" },
        // a PS TV with a DualShock has L2/R2 as well: same actions
        { K_JOY_TRIGGER2, "_attack" },
        { K_JOY_TRIGGER1, "_impulse16" },
        { K_JOY_DPAD_UP, "_impulse7" },
        { K_JOY_DPAD_DOWN, "_impulse10" },
        { K_JOY_DPAD_LEFT, "_impulse15" },
        { K_JOY_DPAD_RIGHT, "_impulse14" },
        { K_JOY1, "_moveUp" },
        { K_JOY2, "_moveDown" },
        { K_JOY3, "_impulse13" },
        { K_JOY4, "_use" },
        // Start is hard-wired to the pause menu by the engine
        { K_JOY10, "_impulse19" },
        { K_JOY16, "_speed" },
        { K_JOY15, "vita_quickTurn" },
        { K_JOY_STICK1_UP, stick1[0] },
        { K_JOY_STICK1_DOWN, stick1[1] },
        { K_JOY_STICK1_LEFT, stick1[2] },
        { K_JOY_STICK1_RIGHT, stick1[3] },
        { K_JOY_STICK2_UP, stick2[0] },
        { K_JOY_STICK2_DOWN, stick2[1] },
        { K_JOY_STICK2_LEFT, stick2[2] },
        { K_JOY_STICK2_RIGHT, stick2[3] },
    };
    for (int i = 0; i < (int)(sizeof(scheme) / sizeof(scheme[0])); i++) {
        idKeyInput::SetBinding(scheme[i].key, scheme[i].binding);
    }
    common->Printf("vita: pad bound to the Vita scheme (R fire, L flashlight, X jump, O crouch toggle, [] reload, /\\ use, "
                   "d-pad quick slots / weapon cycle, Select PDA, rear right sprint, rear left quick turn)\n");
}

/*
================================================================================
Labels
================================================================================
*/

extern idCVar vita_toggleSprint;
extern idCVar vita_toggleCrouch;

static idStr Vita_BindingLabel(int key)
{
    const char* binding = idKeyInput::GetBinding(key);
    if (binding == NULL || binding[0] == '\0') {
        if (key == K_JOY9) {
            return "#str_vita_pause_menu";
        }
        return "";
    }
    const struct {
        const char* binding;
        const char* label;
    } labels[] = {
        { "_attack", "#str_vita_fire_melee" },
        { "_impulse16", "#str_vita_flashlight" },
        { "_impulse13", "#str_vita_reload" },
        { "_impulse14", "#str_vita_next_weapon" },
        { "_impulse15", "#str_vita_prev_weapon" },
        { "_impulse7", "#str_vita_quick1" },
        { "_impulse10", "#str_vita_quick2" },
        { "_impulse0", "#str_vita_fists" },
        { "_impulse12", "#str_vita_soulcube" },
        { "_moveUp", "#str_vita_jump" },
        { "_use", "#str_vita_use" },
        { "_impulse19", "#str_vita_pda" },
        { "_impulse18", "#str_vita_center_view" },
        { "_zoom", "#str_vita_zoom" },
        { "_forward", "#str_vita_move" },
        { "_lookUp", "#str_vita_camera" },
        { "vita_quickTurn", "#str_vita_quickturn" },
    };
    if (idStr::Icmp(binding, "_speed") == 0) {
        return vita_toggleSprint.GetBool() ? "#str_vita_sprint_toggle" : "#str_vita_sprint_hold";
    }
    if (idStr::Icmp(binding, "_moveDown") == 0) {
        return vita_toggleCrouch.GetBool() ? "#str_vita_crouch_toggle" : "#str_vita_crouch_hold";
    }
    for (int i = 0; i < (int)(sizeof(labels) / sizeof(labels[0])); i++) {
        if (idStr::Icmp(binding, labels[i].binding) == 0) {
            return labels[i].label;
        }
    }
    // anything else: the stock action name if there is one, else the raw bind
    const char* loc = idLocalization::GetString(va("#str_swf_action%s", binding));
    if (loc != NULL && loc[0] != '\0' && loc[0] != '#') {
        return loc;
    }
    return binding;
}

/*
DOOM / DOOM II on the same pad (doomlib.cpp InitControlRemap, g_game.cpp
G_BuildTiccmd): the DOOM 3 usercmd drives fire, use, the weapon cycle, run and
turning; Square / Select / Triangle / Circle / Cross / Start arrive as the
classic keys (automap, map zoom, menu confirm / back). L and the D-pad up/down
quick slots do nothing in game there - D-pad up/down move through the menus.
*/
static idStr Vita_ClassicBindingLabel(int key)
{
    switch (key) {
    case K_JOY6:
        return "#str_vita_c_fire";
    case K_JOY5:
        return "-";
    case K_JOY_DPAD_UP:
    case K_JOY_DPAD_DOWN:
        return "#str_vita_c_menu_nav";
    case K_JOY_DPAD_LEFT:
        return "#str_vita_prev_weapon";
    case K_JOY_DPAD_RIGHT:
        return "#str_vita_next_weapon";
    case K_JOY_STICK1_UP:
        return "#str_vita_move";
    case K_JOY_STICK2_UP:
        return "#str_vita_c_turn";
    case K_JOY4:
        return "#str_vita_c_use_zoomout";
    case K_JOY2:
        return "#str_vita_c_back_zoomin";
    case K_JOY3:
        return "#str_vita_c_automap";
    case K_JOY1:
        return "#str_vita_c_use_confirm";
    case K_JOY10:
        return "#str_vita_c_automap";
    case K_JOY9:
        return "#str_vita_c_main_menu";
    case K_JOY15:
        return "#str_vita_quickturn";
    case K_JOY16:
        return vita_toggleSprint.GetBool() ? "#str_vita_c_run_toggle" : "#str_vita_c_run_hold";
    }
    return "";
}

/*
================================================================================
Controller Layout overlay

Drawn in post-transform screen pixels (the space idSWF::RenderSprite uses)
over the whole screen, fading with the stock screen it replaces.
================================================================================
*/

static const idMaterial* vitaWhite = NULL;
static bool vitaLayoutLogged = false;
static int vitaLayoutGame = 0; // 0 DOOM 3: BFG, 1 DOOM I & II

void Vita_UI_ControllerLayoutShown()
{
    vitaLayoutLogged = false;
    vitaLayoutGame = 0;
}

void Vita_UI_SetControllerLayoutGame(int game)
{
    game = idMath::ClampInt(0, 1, game);
    if (game != vitaLayoutGame) {
        vitaLayoutGame = game;
        vitaLayoutLogged = false; // log the other scheme's rows once too
        common->Printf("vita: controller layout shows the %s scheme\n", game == 0 ? "DOOM 3: BFG" : "DOOM I & II");
    }
}

int Vita_UI_GetControllerLayoutGame()
{
    return vitaLayoutGame;
}

static void DrawQuadPx(idSWF* swf, float x, float y, float w, float h, float s1, float t1, float s2, float t2, const idMaterial* m)
{
    swf->VitaDrawQuad(idVec2(x, y), idVec2(x + w, y), idVec2(x + w, y + h), idVec2(x, y + h), s1, t1, s2, t2, m);
}

static void DrawGlyphPx(idSWF* swf, int glyph, float x, float y, float w, float h)
{
    const idMaterial* m = Vita_UI_Material();
    if (glyph < 0 || m == NULL) {
        return;
    }
    float s1, t1, s2, t2;
    Vita_UI_GlyphST(glyph, s1, t1, s2, t2);
    DrawQuadPx(swf, x, y, w, h, s1, t1, s2, t2, m);
}

static void DrawLinePx(idSWF* swf, const idVec2& a, const idVec2& b, float width)
{
    idVec2 d = b - a;
    const float len = d.Length();
    if (len < 0.5f || vitaWhite == NULL) {
        return;
    }
    idVec2 n(-d.y / len * width * 0.5f, d.x / len * width * 0.5f);
    swf->VitaDrawQuad(a + n, b + n, b - n, a - n, 0, 0, 1, 1, vitaWhite);
}

static bool FontHasGlyph(const idFont* font, uint32 c)
{
    // a missing glyph comes back as the asterisk
    scaledGlyphInfo_t g, star;
    font->GetScaledGlyph(1.0f, c, g);
    font->GetScaledGlyph(1.0f, '*', star);
    return c == '*' || g.s1 != star.s1 || g.t1 != star.t1;
}

static float TextWidthPx(const idFont* font, float scale, const char* s)
{
    // (UTF-8: the translated labels have accents)
    float w = 0.0f;
    const int len = idStr::Length(s);
    for (int i = 0; i < len;) {
        scaledGlyphInfo_t g;
        font->GetScaledGlyph(scale, idStr::UTF8Char(s, i), g);
        w += g.xSkip;
    }
    return w;
}

// text with a soft black drop shadow; y is the top of the line
static void DrawTextPx(idSWF* swf, const idFont* font, float scale, float x, float y, const char* s, const idVec4& color)
{
    const float baseline = y + font->GetAscender(scale);
    for (int pass = 0; pass < 2; pass++) {
        const float off = (pass == 0) ? Max(1.0f, scale * 2.0f) : 0.0f;
        renderSystem->SetColor(pass == 0 ? idVec4(0.0f, 0.0f, 0.0f, color.w * 0.85f) : color);
        float px = x;
        const int len = idStr::Length(s);
        for (int i = 0; i < len;) {
            scaledGlyphInfo_t g;
            font->GetScaledGlyph(scale, idStr::UTF8Char(s, i), g);
            DrawQuadPx(swf, px + g.left + off, baseline - g.top + off, g.width + 1.0f, g.height + 1.0f, g.s1, g.t1, g.s2, g.t2, g.material);
            px += g.xSkip;
        }
    }
}

// A port string (#str_vita_...) in the game's language - or in English when
// this font can not draw it (the layout's Latin fonts have no Japanese).
// Anything else (a raw binding name) is returned as it is.
extern const char* Vita_PortStringEnglish(const char* key);
static idStr VitaLocalLabel(const idFont* font, const idStr& keyOrText)
{
    if (keyOrText.Icmpn("#str_", 5) != 0) {
        return keyOrText;
    }
    const char* loc = idLocalization::GetString(keyOrText);
    const int len = idStr::Length(loc);
    for (int i = 0; i < len;) {
        const uint32 c = idStr::UTF8Char(loc, i);
        if (c > 0x7F && c != 0xB0 && !FontHasGlyph(font, c)) {
            return Vita_PortStringEnglish(keyOrText);
        }
    }
    return loc;
}

struct vitaLayoutRow_t {
    int key;
    float u, v;     // the button on the controller picture, 0..1 (measured on the 2000x903 artwork)
    idStr label;
    int glyph;
    idStr lines[2]; // the label, wrapped to at most two lines
    int numLines;
};

// One line if it fits, else two: prefer breaking right after a ':' or '/' (the
// punctuation ends the first line, "Quick-slot 1:" / "Grenades"), then splits
// that do not leave a single word alone on line two, then the most balanced.
static int SplitLabel(const idFont* font, float scale, const idStr& text, float maxW, idStr lines[2])
{
    if (TextWidthPx(font, scale, text) <= maxW) {
        lines[0] = text;
        return 1;
    }
    int bestAt = -1;
    int bestPunct = 2, bestLone = 2;
    float bestW = 1e9f;
    for (int i = 0; i < text.Length(); i++) {
        if (text[i] != ' ') {
            continue;
        }
        idStr a = text.Left(i);
        idStr b = text.Right(text.Length() - i - 1);
        a.StripTrailingWhitespace();
        b.StripLeading(' ');
        const float w = Max(TextWidthPx(font, scale, a), TextWidthPx(font, scale, b));
        const char last = a.Length() ? a[a.Length() - 1] : 0;
        const int punct = ((last == ':' || last == '/') && w <= maxW) ? 0 : 1;
        const int lone = (b.Find(' ') < 0) ? 1 : 0;
        if (punct < bestPunct || (punct == bestPunct && (lone < bestLone || (lone == bestLone && w < bestW)))) {
            bestPunct = punct;
            bestLone = lone;
            bestW = w;
            bestAt = i;
        }
    }
    if (bestAt < 0) {
        lines[0] = text;
        return 1;
    }
    lines[0] = text.Left(bestAt);
    lines[1] = text.Right(text.Length() - bestAt - 1);
    lines[0].StripTrailingWhitespace();
    lines[1].StripLeading(' ');
    return 2;
}

static void FillPx(idSWF* swf, float x0, float y0, float x1, float y1, const idVec4& color)
{
    renderSystem->SetColor(color);
    DrawQuadPx(swf, x0, y0, x1 - x0, y1 - y0, 0, 0, 1, 1, vitaWhite);
}

/*
========================
Vita_UI_DrawControllerLayout

The Controller Layout screen, full screen: it takes over the whole display
while that menu screen is up (the stock screen underneath keeps handling the
buttons - Circle goes back). Laid out from the screen height so it reads the
same at any render resolution. tools/vita/layout_mock.py is the reference
mock-up of exactly this layout, drawn with the game's own fonts.
========================
*/
void Vita_UI_DrawControllerLayout(idSWF* swf, float alpha)
{
    if (swf == NULL || alpha <= 0.01f || Vita_UI_Material() == NULL) {
        return;
    }
    if (vitaWhite == NULL) {
        vitaWhite = declManager->FindMaterial("_white");
    }
    static const idFont* font = NULL;
    static const idFont* headFont = NULL;
    if (font == NULL) {
        font = renderSystem->RegisterFont("Sarasori Rg");
        if (font == NULL) {
            font = renderSystem->RegisterFont("Arial Narrow");
        }
    }
    if (headFont == NULL) {
        headFont = renderSystem->RegisterFont("BankGothic Md BT");
        if (headFont == NULL) {
            headFont = font;
        }
    }
    const int controllerGlyph = Vita_UI_FindGlyph("controller");
    const vitaUiGlyph_t* cg = Vita_UI_Glyph(controllerGlyph);
    if (font == NULL || cg == NULL || vitaWhite == NULL) {
        return;
    }

    const float W = (float)renderSystem->GetWidth();
    const float H = (float)renderSystem->GetHeight();

    // ---- what is bound where -----------------------------------------------------
    vitaLayoutRow_t left[] = {
        { K_JOY5, 0.125f, 0.020f },
        { K_JOY_DPAD_UP, 0.0955f, 0.249f },
        { K_JOY_DPAD_LEFT, 0.0555f, 0.338f },
        { K_JOY_DPAD_RIGHT, 0.1355f, 0.338f },
        { K_JOY_DPAD_DOWN, 0.0955f, 0.426f },
        { K_JOY_STICK1_UP, 0.1225f, 0.599f },
    };
    vitaLayoutRow_t right[] = {
        { K_JOY6, 0.875f, 0.020f },
        { K_JOY4, 0.904f, 0.241f },
        { K_JOY2, 0.9485f, 0.3355f },
        { K_JOY3, 0.859f, 0.3355f },
        { K_JOY1, 0.904f, 0.4297f },
        { K_JOY_STICK2_UP, 0.8775f, 0.599f },
        { K_JOY10, 0.861f, 0.792f },
        { K_JOY9, 0.916f, 0.792f },
    };
    vitaLayoutRow_t rear[] = {
        { K_JOY15, 0.30f, 1.0f },
        { K_JOY16, 0.70f, 1.0f },
    };
    const int numLeft = sizeof(left) / sizeof(left[0]);
    const int numRight = sizeof(right) / sizeof(right[0]);
    const bool degree = FontHasGlyph(font, 0xB0);
    for (int pass = 0; pass < 3; pass++) {
        vitaLayoutRow_t* rows = (pass == 0) ? left : (pass == 1) ? right : rear;
        const int n = (pass == 0) ? numLeft : (pass == 1) ? numRight : 2;
        for (int i = 0; i < n; i++) {
            rows[i].label = VitaLocalLabel(font, (vitaLayoutGame == 1) ? Vita_ClassicBindingLabel(rows[i].key) : Vita_BindingLabel(rows[i].key));
            if (!degree) {
                rows[i].label.Replace("\xC2\xB0", "");
                rows[i].label.Replace("\xB0", "");
            }
            rows[i].glyph = Vita_UI_GlyphForKey(rows[i].key);
        }
    }

    // ---- layout: shrink the text until both columns fit (normally 4.6% of H) -------
    const float margin = 0.045f * W;
    const float pw = 0.40f * W;
    const float ph = pw * cg->h / cg->w;
    // (the bottom strip holds the game stepper and the BACK prompt)
    const float contentTop = 0.20f * H, contentBot = 0.86f * H;
    float th = 0.046f * H;
    float icon = 0, gap = 0, lead = 0, colText = 0, lineH = 0, rearH = 0, blockH = 0;
    float colH[2] = { 0, 0 };
    for (int attempt = 0; attempt < 30; attempt++) {
        const float scale = th / 48.0f;
        icon = 1.35f * th;
        gap = 0.35f * th;
        lead = 1.1f * th;
        colText = (W - 2.0f * margin - pw) * 0.5f - lead - icon - gap;
        lineH = 1.08f * th;
        rearH = 2.4f * th;
        blockH = ph + 0.7f * th + rearH + 0.2f * th + 2.0f * lineH;
        float widest = 0.0f;
        for (int side = 0; side < 2; side++) {
            vitaLayoutRow_t* rows = side == 0 ? left : right;
            const int n = side == 0 ? numLeft : numRight;
            colH[side] = 0.0f;
            for (int i = 0; i < n; i++) {
                rows[i].numLines = SplitLabel(font, scale, rows[i].label, colText, rows[i].lines);
                for (int k = 0; k < rows[i].numLines; k++) {
                    widest = Max(widest, TextWidthPx(font, scale, rows[i].lines[k]));
                }
                colH[side] += lineH * rows[i].numLines + 0.62f * th;
            }
        }
        for (int i = 0; i < 2; i++) {
            rear[i].numLines = SplitLabel(font, scale, rear[i].label, 0.36f * pw, rear[i].lines);
        }
        if (widest <= colText && Max(colH[0], colH[1]) <= contentBot - contentTop && blockH <= contentBot - contentTop) {
            break;
        }
        th *= 0.95f;
    }
    const float scale = th / 48.0f;
    const float px0 = (W - pw) * 0.5f;
    const float py0 = contentTop + (contentBot - contentTop - blockH) * 0.5f;

    if (!vitaLayoutLogged) {
        vitaLayoutLogged = true;
        common->Printf("vita: controller layout [%s] (full screen %.0fx%.0f): text %.1f px (%.1f%% of the screen), picture %.0fx%.0f at (%.0f,%.0f), alpha %.2f\n",
            vitaLayoutGame == 0 ? "DOOM 3: BFG" : "DOOM I & II", W, H, th, 100.0f * th / H, pw, ph, px0, py0, alpha);
        for (int pass = 0; pass < 3; pass++) {
            const vitaLayoutRow_t* rows = (pass == 0) ? left : (pass == 1) ? right : rear;
            const int n = (pass == 0) ? numLeft : (pass == 1) ? numRight : 2;
            for (int i = 0; i < n; i++) {
                common->Printf("vita:   %-16s -> %-18s \"%s\"\n", idKeyInput::KeyNumToString((keyNum_t)rows[i].key),
                    idKeyInput::GetBinding(rows[i].key), rows[i].label.c_str());
            }
        }
    }

    renderSystem->SetGLState(GLS_OVERRIDE | GLS_DEPTHFUNC_LESS | GLS_DEPTHMASK | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

    // ---- background: the whole screen, near-black with a faint warm lift at the top
    FillPx(swf, 0, 0, W, H, idVec4(0.04f, 0.035f, 0.035f, alpha));
    for (int i = 0; i < 12; i++) {
        const float y0 = H * 0.5f * i / 12.0f, y1 = H * 0.5f * (i + 1) / 12.0f;
        FillPx(swf, 0, y0, W, y1, idVec4(0.10f, 0.05f, 0.03f, alpha * 0.22f * (1.0f - i / 12.0f)));
    }

    // ---- header: white heading over an orange plate fading to the right, then a rule
    const float hy0 = 0.05f * H, hy1 = 0.155f * H;
    for (int i = 0; i < 32; i++) {
        const float a = idMath::Pow(1.0f - i / 32.0f, 1.6f);
        FillPx(swf, W * 0.62f * i / 32.0f, hy0, W * 0.62f * (i + 1) / 32.0f, hy1, idVec4(0.59f, 0.17f, 0.05f, alpha * 0.92f * a));
    }
    const float headTh = 0.072f * H;
    DrawTextPx(swf, headFont, headTh / 48.0f, margin, (hy0 + hy1) * 0.5f - headTh * 0.62f, VitaLocalLabel(headFont, "#str_vita_layout_title").c_str(), idVec4(1, 1, 1, alpha));
    const float ruleY = hy1 + 0.006f * H;
    FillPx(swf, 0, ruleY, W * 0.75f, ruleY + Max(1.0f, 0.004f * H), idVec4(0.97f, 0.58f, 0.12f, alpha * 0.8f));

    // ---- the Vita
    renderSystem->SetColor(idVec4(1.0f, 1.0f, 1.0f, alpha));
    DrawGlyphPx(swf, controllerGlyph, px0, py0, pw, ph);

    // ---- the label columns, with orange leaders elbowing in to each button ------
    const idVec4 textColor(0.95f, 0.95f, 0.95f, alpha);
    const idVec4 lineColor(0.97f, 0.58f, 0.12f, 0.95f * alpha);
    const idVec4 dotColor(1.0f, 0.63f, 0.16f, alpha);
    const float lw = Max(1.0f, 0.09f * th);
    for (int side = 0; side < 2; side++) {
        const vitaLayoutRow_t* rows = side == 0 ? left : right;
        const int n = side == 0 ? numLeft : numRight;
        const float picCy = py0 + ph * 0.5f;
        float y = Max(contentTop, Min(picCy - colH[side] * 0.5f, contentBot - colH[side]));
        for (int i = 0; i < n; i++) {
            const vitaLayoutRow_t& r = rows[i];
            const float rowH = lineH * r.numLines + 0.62f * th;
            const float cy = y + rowH * 0.5f;
            const idVec2 target(px0 + r.u * pw, py0 + r.v * ph);
            float ix, lx, edge;
            if (side == 0) {
                ix = px0 - lead - icon;
                lx = ix + icon + 0.12f * th;
                edge = Min(px0 + 0.02f * pw, target.x);
            } else {
                ix = px0 + pw + lead;
                lx = ix - 0.12f * th;
                edge = Max(px0 + 0.98f * pw, target.x);
            }
            renderSystem->SetColor(lineColor);
            DrawLinePx(swf, idVec2(lx, cy), idVec2(edge, cy), lw);
            DrawLinePx(swf, idVec2(edge, cy), target, lw);
            const float dot = Max(3.0f, 0.32f * th);
            FillPx(swf, target.x - dot * 0.5f, target.y - dot * 0.5f, target.x + dot * 0.5f, target.y + dot * 0.5f, dotColor);

            if (r.glyph >= 0) {
                const vitaUiGlyph_t* g = Vita_UI_Glyph(r.glyph);
                const float a = (float)g->w / g->h;
                const float gw = a >= 1.0f ? icon : icon * a;
                const float gh = a >= 1.0f ? icon / a : icon;
                renderSystem->SetColor(idVec4(1.0f, 1.0f, 1.0f, alpha));
                DrawGlyphPx(swf, r.glyph, ix + 0.5f * (icon - gw), cy - 0.5f * gh, gw, gh);
            }
            const float ty = cy - r.numLines * lineH * 0.5f;
            for (int k = 0; k < r.numLines; k++) {
                const float tw = TextWidthPx(font, scale, r.lines[k]);
                const float tx = (side == 0) ? (ix - gap - tw) : (ix + icon + gap);
                DrawTextPx(swf, font, scale, tx, ty + k * lineH - 0.08f * th, r.lines[k], textColor);
            }
            y += rowH;
        }
    }

    // ---- rear touch: the two halves under the picture, labels kept inside it ----
    const float rearTop = py0 + ph + 0.7f * th;
    for (int i = 0; i < 2; i++) {
        const vitaLayoutRow_t& r = rear[i];
        const float gh = rearH, gw = rearH * 1.6f;
        const float cx = px0 + r.u * pw;
        renderSystem->SetColor(idVec4(1.0f, 1.0f, 1.0f, alpha));
        DrawGlyphPx(swf, r.glyph, cx - 0.5f * gw, rearTop, gw, gh);
        for (int k = 0; k < r.numLines; k++) {
            const float tw = TextWidthPx(font, scale, r.lines[k]);
            DrawTextPx(swf, font, scale, cx - 0.5f * tw, rearTop + gh + 0.2f * th + k * lineH, r.lines[k], textColor);
        }
    }

    // ---- the game stepper, centred under everything: < DOOM 3: BFG >  -------------
    {
        const float sth = 0.050f * H;
        const float sscale = sth / 48.0f;
        const float sy = 0.905f * H;
        static const char* names[2] = { "DOOM 3: BFG", "DOOM I & II" };
        float nameW = 0.0f;
        for (int i = 0; i < 2; i++) {
            nameW = Max(nameW, TextWidthPx(headFont, sscale, names[i]));
        }
        const float boxW = nameW + 2.0f * sth;
        const float cx = W * 0.5f;
        FillPx(swf, cx - boxW * 0.5f - 1.6f * sth, sy - 0.15f * sth, cx + boxW * 0.5f + 1.6f * sth, sy + 1.05f * sth,
            idVec4(0.0f, 0.0f, 0.0f, alpha * 0.45f));
        const char* name = names[vitaLayoutGame];
        const float tw = TextWidthPx(headFont, sscale, name);
        DrawTextPx(swf, headFont, sscale, cx - 0.5f * tw, sy, name, idVec4(1, 1, 1, alpha));
        // arrows: lit when that way leads somewhere
        const float aw = 0.45f * sth, ah = 0.7f * sth;
        const float ay = sy + 0.45f * sth;
        for (int side = 0; side < 2; side++) {
            const bool live = (side == 0) ? (vitaLayoutGame > 0) : (vitaLayoutGame < 1);
            const idVec4 c = live ? idVec4(1.0f, 0.63f, 0.16f, alpha) : idVec4(0.5f, 0.5f, 0.5f, alpha * 0.45f);
            const float tipX = (side == 0) ? cx - boxW * 0.5f - 0.9f * sth : cx + boxW * 0.5f + 0.9f * sth;
            const float baseX = (side == 0) ? tipX + aw : tipX - aw;
            renderSystem->SetColor(c);
            swf->VitaDrawQuad(idVec2(tipX, ay), idVec2(baseX, ay - ah * 0.5f), idVec2(baseX, ay + ah * 0.5f), idVec2(tipX, ay), 0, 0, 1, 1, vitaWhite);
        }
        // page dots
        const float d = 0.16f * sth;
        for (int i = 0; i < 2; i++) {
            const float dx = cx + (i - 0.5f) * 0.6f * sth;
            FillPx(swf, dx - d * 0.5f, sy + 1.2f * sth - d * 0.5f, dx + d * 0.5f, sy + 1.2f * sth + d * 0.5f,
                i == vitaLayoutGame ? idVec4(1.0f, 0.63f, 0.16f, alpha) : idVec4(0.6f, 0.6f, 0.6f, alpha * 0.6f));
        }
    }

    // ---- footer prompt, like the menus' command bar (this screen covers theirs) --
    const float fth = 0.05f * H;
    const float fy = 0.93f * H;
    const int circle = Vita_UI_FindGlyph("face_circle");
    renderSystem->SetColor(idVec4(1.0f, 1.0f, 1.0f, alpha));
    DrawGlyphPx(swf, circle, margin, fy - fth * 0.62f, fth * 1.25f * 38.0f / 34.0f, fth * 1.25f);
    DrawTextPx(swf, headFont, fth / 48.0f, margin + fth * 1.55f, fy - fth * 0.55f, VitaLocalLabel(headFont, "#str_vita_back").c_str(), idVec4(1, 1, 1, alpha));

    renderSystem->SetColor(idVec4(1.0f, 1.0f, 1.0f, 1.0f));
    renderSystem->SetGLState(0);
}
