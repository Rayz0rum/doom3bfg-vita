/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2014 Robert Beckebans

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

#include "tr_local.h"

#if defined(__vita__)
#include "../sys/vita/vita_public.h"
#endif

idCVar r_skipStripDeadCode("r_skipStripDeadCode", "0", CVAR_BOOL, "Skip stripping dead code");
idCVar r_useUniformArrays("r_useUniformArrays", "1", CVAR_BOOL, "");

// DG: the AMD drivers output a lot of useless warnings which are fscking annoying, added this CVar to suppress them
idCVar r_displayGLSLCompilerMessages("r_displayGLSLCompilerMessages", "1", CVAR_BOOL | CVAR_ARCHIVE, "Show info messages the GPU driver outputs when compiling the shaders");
// DG end

// RB begin
#if defined(__vita__)
// Converting every shader from Cg and writing three files per stage to the
// memory card on every boot and map load cost seconds each time. The Vita
// reuses the converted GLSL instead, as long as its signature line (see
// VitaGLSLSignature) says it was made by the same rewrites and settings.
idCVar r_alwaysExportGLSL("r_alwaysExportGLSL", "0", CVAR_BOOL, "");
#else
idCVar r_alwaysExportGLSL("r_alwaysExportGLSL", "1", CVAR_BOOL, "");
#endif

#if defined(__vita__)
idCVar vita_fastInteraction("vita_fastInteraction", "1", CVAR_RENDERER | CVAR_BOOL,
    "Vita: use the lighter per-pixel lighting shader (same look, cheaper on the GPU); applied at start");

static const char* vitaFastInteractionMain =
    "// Vita: the lighter interaction (vita_fastInteraction). Same lighting model as\n"
    "// the stock shader, cheaper to run on the Vita's GPU:\n"
    "//  - every texture read takes its coordinate straight from a varying: the\n"
    "//    falloff coordinate's w is always 1, so it is a plain tex2D; the\n"
    "//    projection's divisor is mirrored into .z by the vertex shader, so it is\n"
    "//    tex2Dproj on .xyz. Nothing is computed before the reads, so the GPU can\n"
    "//    fetch all five before the shader runs instead of stalling on them;\n"
    "//  - the specular exponent is 10, lowered by Toksvig where the normal\n"
    "//    map's mip level averaged disagreeing normals (blue, Toksvig.cpp);\n"
    "//  - the bump normal is 0.5 long by construction, so normalize() is a * 2;\n"
    "//  - the shading math runs in half precision; texture coordinates stay float.\n"
    "//    Multiplies are written as *= on purpose: vitaGL routes 'a * b' through\n"
    "//    its float-only vglMul() helper but passes *= to Cg untouched, so only\n"
    "//    the compound form stays half.\n"
    "void main( PS_IN fragment, out PS_OUT result ) {\n"
    "	VITA_H4 bumpMap =		tex2D( samp0, fragment.texcoord1.xy );\n"
    "	VITA_H4 lightFalloff =	tex2D( samp1, fragment.texcoord2.xy );\n"
    "	VITA_H4 lightProj =		tex2Dproj( samp2, fragment.texcoord3.xyz );\n"
    "	VITA_H4 YCoCG =			tex2D( samp3, fragment.texcoord4.xy );\n"
    "	VITA_H4 specMap =		tex2D( samp4, fragment.texcoord5.xy );\n"
    "\n"
    "	VITA_LIGHTVECTOR\n"
    "	VITA_H3 diffuseMap = ConvertYCoCgToRGB( YCoCG );\n"
    "\n"
    "	VITA_H3 localNormal;\n"
    "	localNormal.xy = bumpMap.wy - 0.5;\n"
    "	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );\n"
    "	localNormal *= 2.0;\n"
    "\n"
    "	VITA_H1 hDotN = max( dot( normalize( fragment.texcoord6.xyz ), localNormal ), 0.0 );\n"
    "	VITA_SPECULAR\n"
    "\n"
    "	VITA_H3 diffuseColor = diffuseMap;\n"
    "	diffuseColor *= rpDiffuseModifier.xyz;\n"
    "	VITA_H3 specularColor = specMap.xyz;\n"
    "	specularColor *= specular;\n"
    "	specularColor *= rpSpecularModifier.xyz;\n"
    "	VITA_H3 lightColor = lightProj.xyz;\n"
    "	lightColor *= lightFalloff.xyz;\n"
    "	lightColor *= dot( lightVector, localNormal );\n"
    "\n"
    "	VITA_H3 color = diffuseColor + specularColor;\n"
    "	color *= lightColor;\n"
    "	color *= fragment.color.xyz;\n"
    "	result.color.xyz = color;\n"
    "	result.color.w = 1.0;\n"
    "}\n";

// the specular term (VITA_SPECULAR above), by vita_toksvigShader
idCVar vita_toksvigShader("vita_toksvigShader", "1", CVAR_RENDERER | CVAR_BOOL,
    "Vita: the interaction's highlight reads Toksvig's exponent from the normal maps (0 = the fixed pow( x, 10 ), blue ignored - for measuring its cost); applied at start");

static const char* vitaSpecularToksvig =
    "// Toksvig (Toksvig.cpp): blue holds (10 - s') / 6, s' the exponent Toksvig\n"
    "	// gives the mip level's mean normal (10 - blue 0 - at full resolution and\n"
    "	// in unfiltered maps; 4 at the widest). pow( h, s' ) is computed as\n"
    "	// max( 1 - k * (1 - h), 0 )^8 with k = (0.75 + 0.9 s') / 8: a mad and\n"
    "	// three squarings, no log/exp/reciprocal on the GPU. Within 1.6/255 of\n"
    "	// pow( h, 10 ) at blue 0 and 1.9/255 at worst over s' 4..10\n"
    "	// (tools/vita/toksvig_pow_fit.py). The highlight is scaled by\n"
    "	// (s' + 1) / 11 as it widens.\n"
    "	VITA_H1 toksvigK = bumpMap.z;\n"
    "	toksvigK *= -0.675;\n"
    "	toksvigK += 1.21875;\n"
    "	VITA_H1 specular = hDotN - 1.0;\n"
    "	specular *= toksvigK;\n"
    "	specular += 1.0;\n"
    "	specular = max( specular, 0.0 );\n"
    "	specular *= specular;\n"
    "	specular *= specular;\n"
    "	specular *= specular;\n"
    "	VITA_H1 toksvigNorm = bumpMap.z;\n"
    "	toksvigNorm *= -0.5454545;\n"
    "	toksvigNorm += 1.0;\n"
    "	specular *= toksvigNorm;\n";

static const char* vitaSpecularFixed =
    "// the fixed exponent 10 (vita_toksvigShader 0): x^10 in four multiplies\n"
    "	VITA_H1 hDotN2 = hDotN;\n"
    "	hDotN2 *= hDotN;\n"
    "	VITA_H1 specular = hDotN2;\n"
    "	specular *= hDotN2;\n"
    "	specular *= specular;\n"
    "	specular *= hDotN2;\n";

static bool VitaIsInteractionProgram(const idStr& file, bool* ambient)
{
    const char* names[] = { "renderprogs/interaction", "renderprogs/interaction_skinned",
        "renderprogs/interactionAmbient", "renderprogs/interactionAmbient_skinned" };
    idStr base = file;
    base.StripFileExtension();
    for (int i = 0; i < 4; i++) {
        if (base.Icmp(names[i]) == 0) {
            *ambient = (i >= 2);
            return true;
        }
    }
    return false;
}
#endif
// RB end

// FXAA on the 3D view's stretch (RB_PostProcess): NVIDIA FXAA 3.11's console
// algorithm, generated from the official header by tools/vita/make_fxaa.py,
// replaces the postprocess shader's main() (a plain copy) when this is on.
// Applied when the shaders are built (start); its constants (vita_fxaaN,
// vita_fxaaSharpness, vita_fxaaThreshold, vita_fxaaThresholdMin) apply at once.
#if defined(__vita__)
idCVar vita_fxaa("vita_fxaa", "0", CVAR_RENDERER | CVAR_BOOL, "FXAA (NVIDIA FXAA 3.11, console) on the 3D view's stretch - OFF: dropped by the user's decision (2026-10-19, 2x MSAA + Toksvig + A2C instead); applied at start");
#else
idCVar vita_fxaa("vita_fxaa", "0", CVAR_RENDERER | CVAR_BOOL, "FXAA (NVIDIA FXAA 3.11, console) on the 3D view's stretch (needs rs_enable on the PC); applied at start");
#endif
static const char* vitaFxaaMain =
#include "vita_fxaa.inc"
    ;

// Sharpening + soft alpha-tested edges on the 3D view's stretch (RB_PostProcess):
// the whole postprocess program is replaced when vita_postFilter is on (the
// Vita's default; applied when the shaders are built). One pass, three texture
// reads, all at coordinates the vertex shader computed - the SGX fetches them
// before the fragment shader runs - and a handful of ALU:
//  - sharpening: LumaSharpen's fast pattern (CeeJay.dk, SweetFX), a diagonal
//    3-tap unsharp mask - two bilinear taps a third of a source texel out on
//    the diagonal make a 7-texel Gaussian blur; the luma of (picture - blur),
//    scaled and clamped, is added back. Luma only (no colour fringes), clamped
//    (no halos). Strength and clamp: vita_sharpen, vita_sharpenClamp; the
//    strength grows with the stretch as dynamic resolution lowers the view;
//  - soft edges for alpha-tested surfaces (vita_softAlphaEdges, standing in
//    for alpha to coverage, which GXM does not offer): the 3D view's alpha
//    holds a map of where alpha-tested surfaces show (RB_VitaAlphaEdgeMask);
//    where it changes between the centre and the two taps - a cut-out's edge,
//    either side of it - the blur replaces the picture.
// rpTexGen0S: 1 / the copy's size (xy), 2 * clamp, -clamp; rpTexGen0T: the
// luma weights times strength * 1.5 * 0.5 / clamp (xyz); rpTexGen0Q: x = soft
// edge strength (0 when the frame has no map), y = 1 to paint the softened
// pixels red (vita_softAlphaEdgesShow, the verification's check).
static const char* vitaPostFilterVertex =
    "#include \"global.inc\"\n"
    "\n"
    "struct VS_IN {\n"
    "	float4 position : POSITION;\n"
    "	float2 texcoord : TEXCOORD0;\n"
    "	float4 normal : NORMAL;\n"
    "	float4 tangent : TANGENT;\n"
    "	float4 color : COLOR0;\n"
    "};\n"
    "\n"
    "struct VS_OUT {\n"
    "	float4 position : POSITION;\n"
    "	float2 texcoord0 : TEXCOORD0;\n"
    "	float2 texcoord1 : TEXCOORD1;\n"
    "	float2 texcoord2 : TEXCOORD2;\n"
    "};\n"
    "\n"
    "void main( VS_IN vertex, out VS_OUT result ) {\n"
    "	result.position = vertex.position;\n"
    "	result.texcoord0 = vertex.texcoord;\n"
    "	float2 diagonal = rpTexGen0S.xy;\n"
    "	diagonal *= 0.33333333;\n"
    "	result.texcoord1 = vertex.texcoord + diagonal;\n"
    "	result.texcoord2 = vertex.texcoord - diagonal;\n"
    "}\n";

static const char* vitaPostFilterPixel =
    "#include \"global.inc\"\n"
    "\n"
    "uniform sampler2D samp0 : register(s0);\n"
    "\n"
    "struct PS_IN {\n"
    "	float4 position : VPOS;\n"
    "	float2 texcoord0 : TEXCOORD0_centroid;\n"
    "	float2 texcoord1 : TEXCOORD1_centroid;\n"
    "	float2 texcoord2 : TEXCOORD2_centroid;\n"
    "};\n"
    "\n"
    "struct PS_OUT {\n"
    "	float4 color : COLOR;\n"
    "};\n"
    "\n"
    "void main( PS_IN fragment, out PS_OUT result ) {\n"
    "	float4 ori = tex2D( samp0, fragment.texcoord0 );\n"
    "	float4 tapA = tex2D( samp0, fragment.texcoord1 );\n"
    "	float4 tapB = tex2D( samp0, fragment.texcoord2 );\n"
    "	float4 blur = tapA + tapB;\n"
    "	blur *= 0.5;\n"
    "	// sharpening: the luma of (picture - blur), scaled, clamped to +-clamp\n"
    "	float3 detail = ori.xyz - blur.xyz;\n"
    "	float sharpLuma = saturate( dot( detail, rpTexGen0T.xyz ) + 0.5 );\n"
    "	sharpLuma *= rpTexGen0S.z;\n"
    "	sharpLuma += rpTexGen0S.w;\n"
    "	float3 color = ori.xyz + sharpLuma;\n"
    "	// soft edges: alpha is twice the alpha-tested surfaces' texture alpha\n"
    "	// where they show, 0 elsewhere; the edge is where centre and taps differ\n"
    "	float maskC = saturate( ori.w * 2.0 );\n"
    "	float edge = abs( maskC - saturate( tapA.w * 2.0 ) ) + abs( maskC - saturate( tapB.w * 2.0 ) );\n"
    "	edge = saturate( edge * 2.0 );\n"
    "	edge *= rpTexGen0Q.x;\n"
    "	color = lerp( color, blur.xyz, edge );\n"
    "	result.color.xyz = lerp( color, float3( 1.0, 0.0, 0.0 ), edge * rpTexGen0Q.y );\n"
    "	result.color.w = 1.0;\n"
    "}\n";

#if defined(__vita__)
idCVar vita_postFilter("vita_postFilter", "1", CVAR_RENDERER | CVAR_BOOL, "sharpening + soft edges for alpha-tested surfaces on the 3D view's stretch (one pass; strengths vita_sharpen, vita_softAlphaEdges); applied at start");
#else
idCVar vita_postFilter("vita_postFilter", "0", CVAR_RENDERER | CVAR_BOOL, "sharpening + soft edges for alpha-tested surfaces on the 3D view's stretch, as on the Vita (needs rs_enable on the PC); applied at start");
#endif
// set when the postprocess program was built with the filter (the back end
// then sets its constants and draws the soft edges' map)
bool r_vitaPostFilterBuilt = false;

static bool VitaIsPostProcess(const idStr& file)
{
    idStr base = file;
    base.StripFileExtension();
    return base.Icmp("renderprogs/postprocess") == 0;
}

static bool VitaIsPostProcessFragment(const idStr& file, GLenum target)
{
    idStr base = file;
    base.StripFileExtension();
    return target == GL_FRAGMENT_SHADER && base.Icmp("renderprogs/postprocess") == 0;
}

#if defined(__vita__)
idCVar vita_compoundAssign("vita_compoundAssign", "0", CVAR_RENDERER | CVAR_INTEGER,
    "Vita: how to emit 'x *= y' in generated GLSL - 0 = decided by the boot self-test, 1 = keep, 2 = expand");

/*
========================
VitaExpandCompoundMultiplies

vitaGL's GLSL-to-Cg pass rewrites every '*' into a call to its overloaded
vglMul(), and skips the operator outright when it is part of a '*=' - see the
FIXME in utils/glsl_utils.c, glsl_inject_mul(). The statement is then dropped on
the floor with no diagnostic, so anything that relied on it silently computes
the wrong thing.

Rewrite 'lhs *= rhs ;' into 'lhs = lhs * ( rhs ) ;' before the source ever
reaches vitaGL. The parentheses matter: '*=' has the precedence of an
assignment, so 'a *= b + c' means 'a = a * (b + c)', not 'a = a * b + c'.

Statements only, never a for-clause: parenDepth guards against rewriting
'for ( ; ; i *= 2 )', where the trailing ';' this looks for does not exist.
========================
*/
static int VitaExpandCompoundMultiplies(idStr& program)
{
    idStr out;
    const int len = program.Length();
    int stmtStart = 0;
    int parenDepth = 0;
    int expanded = 0;
    bool rewriting = false;

    for (int i = 0; i < len; i++) {
        const char c = program[i];

        if (c == '(') {
            parenDepth++;
            out.Append(c);
            continue;
        }
        if (c == ')') {
            parenDepth--;
            out.Append(c);
            continue;
        }

        if (parenDepth == 0 && (c == ';' || c == '{' || c == '}')) {
            if (rewriting && c == ';') {
                out.Append(" ) ;");
                rewriting = false;
            } else {
                out.Append(c);
            }
            stmtStart = i + 1;
            continue;
        }

        if (c == '*' && !rewriting && parenDepth == 0
            && (i + 1) < len && program[i + 1] == '=') {
            const idStr lead = program.Mid(stmtStart, i - stmtStart);
            int b = 0;
            int e = lead.Length();
            while (b < e && (lead[b] == ' ' || lead[b] == '\t' || lead[b] == '\n' || lead[b] == '\r')) {
                b++;
            }
            while (e > b && (lead[e - 1] == ' ' || lead[e - 1] == '\t' || lead[e - 1] == '\n' || lead[e - 1] == '\r')) {
                e--;
            }
            if (e > b) {
                out.Append("= ");
                out.Append(lead.Mid(b, e - b));
                out.Append(" * ( ");
                rewriting = true;
                expanded++;
                i++; // step over the '='
                continue;
            }
        }

        out.Append(c);
    }

    if (expanded > 0 && !rewriting) {
        program = out;
    }
    return rewriting ? 0 : expanded;
}
#endif

#define VERTEX_UNIFORM_ARRAY_NAME "_va_"
#define FRAGMENT_UNIFORM_ARRAY_NAME "_fa_"

static const int AT_VS_IN = BIT(1);
static const int AT_VS_OUT = BIT(2);
static const int AT_PS_IN = BIT(3);
static const int AT_PS_OUT = BIT(4);
// RB begin
static const int AT_VS_OUT_RESERVED = BIT(5);
static const int AT_PS_IN_RESERVED = BIT(6);
static const int AT_PS_OUT_RESERVED = BIT(7);
// RB end

struct idCGBlock {
    idStr prefix;  // tokens that comes before the name
    idStr name;    // the name
    idStr postfix; // tokens that comes after the name
    bool used;     // whether or not this block is referenced anywhere
};

/*
================================================
attribInfo_t
================================================
*/
struct attribInfo_t {
    const char* type;
    const char* name;
    const char* semantic;
    const char* glsl;
    int bind;
    int flags;
    int vertexMask;
};

/*
================================================
vertexMask_t

NOTE: There is a PS3 dependency between the bit flag specified here and the vertex
attribute index and attribute semantic specified in DeclRenderProg.cpp because the
stored render prog vertexMask is initialized with cellCgbGetVertexConfiguration().
The ATTRIB_INDEX_ defines are used to make sure the vertexMask_t and attrib assignment
in DeclRenderProg.cpp are in sync.

Even though VERTEX_MASK_XYZ_SHORT and VERTEX_MASK_ST_SHORT are not real attributes,
they come before the VERTEX_MASK_MORPH to reduce the range of vertex program
permutations defined by the vertexMask_t bits on the Xbox 360 (see MAX_VERTEX_DECLARATIONS).
================================================
*/
enum vertexMask_t {
    VERTEX_MASK_XYZ = BIT(PC_ATTRIB_INDEX_VERTEX),
    VERTEX_MASK_ST = BIT(PC_ATTRIB_INDEX_ST),
    VERTEX_MASK_NORMAL = BIT(PC_ATTRIB_INDEX_NORMAL),
    VERTEX_MASK_COLOR = BIT(PC_ATTRIB_INDEX_COLOR),
    VERTEX_MASK_TANGENT = BIT(PC_ATTRIB_INDEX_TANGENT),
    VERTEX_MASK_COLOR2 = BIT(PC_ATTRIB_INDEX_COLOR2),
};

attribInfo_t attribsPC[] = {
    // vertex attributes
    { "float4", "position", "POSITION", "in_Position", PC_ATTRIB_INDEX_VERTEX, AT_VS_IN, VERTEX_MASK_XYZ },
    { "float2", "texcoord", "TEXCOORD0", "in_TexCoord", PC_ATTRIB_INDEX_ST, AT_VS_IN, VERTEX_MASK_ST },
    { "float4", "normal", "NORMAL", "in_Normal", PC_ATTRIB_INDEX_NORMAL, AT_VS_IN, VERTEX_MASK_NORMAL },
    { "float4", "tangent", "TANGENT", "in_Tangent", PC_ATTRIB_INDEX_TANGENT, AT_VS_IN, VERTEX_MASK_TANGENT },
    { "float4", "color", "COLOR0", "in_Color", PC_ATTRIB_INDEX_COLOR, AT_VS_IN, VERTEX_MASK_COLOR },
    { "float4", "color2", "COLOR1", "in_Color2", PC_ATTRIB_INDEX_COLOR2, AT_VS_IN, VERTEX_MASK_COLOR2 },

    // pre-defined vertex program output
    { "float4", "position", "POSITION", "gl_Position", 0, AT_VS_OUT | AT_VS_OUT_RESERVED, 0 },
    { "float", "clip0", "CLP0", "gl_ClipDistance[0]", 0, AT_VS_OUT, 0 },
    { "float", "clip1", "CLP1", "gl_ClipDistance[1]", 0, AT_VS_OUT, 0 },
    { "float", "clip2", "CLP2", "gl_ClipDistance[2]", 0, AT_VS_OUT, 0 },
    { "float", "clip3", "CLP3", "gl_ClipDistance[3]", 0, AT_VS_OUT, 0 },
    { "float", "clip4", "CLP4", "gl_ClipDistance[4]", 0, AT_VS_OUT, 0 },
    { "float", "clip5", "CLP5", "gl_ClipDistance[5]", 0, AT_VS_OUT, 0 },

    // pre-defined fragment program input
    { "float4", "position", "WPOS", "gl_FragCoord", 0, AT_PS_IN | AT_PS_IN_RESERVED, 0 },
    { "half4", "hposition", "WPOS", "gl_FragCoord", 0, AT_PS_IN | AT_PS_IN_RESERVED, 0 },
    { "float", "facing", "FACE", "gl_FrontFacing", 0, AT_PS_IN | AT_PS_IN_RESERVED, 0 },

    // fragment program output
#if USE_GLES2
    { "float4", "color", "COLOR", "gl_FragColor", 0, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
    { "half4", "hcolor", "COLOR", "gl_FragColor", 0, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
    { "float4", "color0", "COLOR0", "gl_FragColor", 0, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
    { "float4", "color1", "COLOR1", "gl_FragColor", 1, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
    { "float4", "color2", "COLOR2", "gl_FragColor", 2, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
    { "float4", "color3", "COLOR3", "gl_FragColor", 3, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },
#else
    { "float4", "color", "COLOR", "fo_FragColor", 0, AT_PS_OUT, 0 }, // GLSL version 1.2 doesn't allow for custom color name mappings
    { "half4", "hcolor", "COLOR", "fo_FragColor", 0, AT_PS_OUT, 0 },
    { "float4", "color0", "COLOR0", "fo_FragColor", 0, AT_PS_OUT, 0 },
    { "float4", "color1", "COLOR1", "fo_FragColor", 1, AT_PS_OUT, 0 },
    { "float4", "color2", "COLOR2", "fo_FragColor", 2, AT_PS_OUT, 0 },
    { "float4", "color3", "COLOR3", "fo_FragColor", 3, AT_PS_OUT, 0 },
#endif
    { "float", "depth", "DEPTH", "gl_FragDepth", 4, AT_PS_OUT | AT_PS_OUT_RESERVED, 0 },

// vertex to fragment program pass through
#if 0 // defined(USE_GLES2)
	{ "float4",		"color",		"COLOR",		"vofi_FrontColor",			0,	AT_VS_OUT | AT_VS_OUT_RESERVED,	0 },
	{ "float4",		"color0",		"COLOR0",		"vofi_Color",				0,	AT_VS_OUT | AT_VS_OUT_RESERVED,	0 },
	{ "float4",		"color1",		"COLOR1",		"vofi_FrontSecondaryColor",	0,	AT_VS_OUT | AT_VS_OUT_RESERVED,	0 },
#else
    { "float4", "color", "COLOR", "vofi_Color", 0, AT_VS_OUT, 0 },
    { "float4", "color0", "COLOR0", "vofi_Color", 0, AT_VS_OUT, 0 },
    { "float4", "color1", "COLOR1", "vofi_SecondaryColor", 0, AT_VS_OUT, 0 },
#endif

    { "float4", "color", "COLOR", "vofi_Color", 0, AT_PS_IN, 0 },
    { "float4", "color0", "COLOR0", "vofi_Color", 0, AT_PS_IN, 0 },
    { "float4", "color1", "COLOR1", "vofi_SecondaryColor", 0, AT_PS_IN, 0 },

    { "half4", "hcolor", "COLOR", "vofi_Color", 0, AT_PS_IN, 0 },
    { "half4", "hcolor0", "COLOR0", "vofi_Color", 0, AT_PS_IN, 0 },
    { "half4", "hcolor1", "COLOR1", "vofi_SecondaryColor", 0, AT_PS_IN, 0 },

    { "float4", "texcoord0", "TEXCOORD0_centroid", "vofi_TexCoord0", 0, AT_PS_IN, 0 },
    { "float4", "texcoord1", "TEXCOORD1_centroid", "vofi_TexCoord1", 0, AT_PS_IN, 0 },
    { "float4", "texcoord2", "TEXCOORD2_centroid", "vofi_TexCoord2", 0, AT_PS_IN, 0 },
    { "float4", "texcoord3", "TEXCOORD3_centroid", "vofi_TexCoord3", 0, AT_PS_IN, 0 },
    { "float4", "texcoord4", "TEXCOORD4_centroid", "vofi_TexCoord4", 0, AT_PS_IN, 0 },
    { "float4", "texcoord5", "TEXCOORD5_centroid", "vofi_TexCoord5", 0, AT_PS_IN, 0 },
    { "float4", "texcoord6", "TEXCOORD6_centroid", "vofi_TexCoord6", 0, AT_PS_IN, 0 },
    { "float4", "texcoord7", "TEXCOORD7_centroid", "vofi_TexCoord7", 0, AT_PS_IN, 0 },
    { "float4", "texcoord8", "TEXCOORD8_centroid", "vofi_TexCoord8", 0, AT_PS_IN, 0 },
    { "float4", "texcoord9", "TEXCOORD9_centroid", "vofi_TexCoord9", 0, AT_PS_IN, 0 },

    { "float4", "texcoord0", "TEXCOORD0", "vofi_TexCoord0", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord1", "TEXCOORD1", "vofi_TexCoord1", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord2", "TEXCOORD2", "vofi_TexCoord2", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord3", "TEXCOORD3", "vofi_TexCoord3", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord4", "TEXCOORD4", "vofi_TexCoord4", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord5", "TEXCOORD5", "vofi_TexCoord5", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord6", "TEXCOORD6", "vofi_TexCoord6", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord7", "TEXCOORD7", "vofi_TexCoord7", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord8", "TEXCOORD8", "vofi_TexCoord8", 0, AT_VS_OUT | AT_PS_IN, 0 },
    { "float4", "texcoord9", "TEXCOORD9", "vofi_TexCoord9", 0, AT_VS_OUT | AT_PS_IN, 0 },

    { "half4", "htexcoord0", "TEXCOORD0", "vofi_TexCoord0", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord1", "TEXCOORD1", "vofi_TexCoord1", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord2", "TEXCOORD2", "vofi_TexCoord2", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord3", "TEXCOORD3", "vofi_TexCoord3", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord4", "TEXCOORD4", "vofi_TexCoord4", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord5", "TEXCOORD5", "vofi_TexCoord5", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord6", "TEXCOORD6", "vofi_TexCoord6", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord7", "TEXCOORD7", "vofi_TexCoord7", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord8", "TEXCOORD8", "vofi_TexCoord8", 0, AT_PS_IN, 0 },
    { "half4", "htexcoord9", "TEXCOORD9", "vofi_TexCoord9", 0, AT_PS_IN, 0 },
    { "float", "fog", "FOG", "gl_FogFragCoord", 0, AT_VS_OUT, 0 },
    { "float4", "fog", "FOG", "gl_FogFragCoord", 0, AT_PS_IN, 0 },
    { NULL, NULL, NULL, NULL, 0, 0, 0 }
};

const char* types[] = {
    "int",
    "float",
    "half",
    "fixed",
    "bool",
    "cint",
    "cfloat",
    "void"
};
static const int numTypes = sizeof(types) / sizeof(types[0]);

const char* typePosts[] = {
    "1", "2", "3", "4",
    "1x1", "1x2", "1x3", "1x4",
    "2x1", "2x2", "2x3", "2x4",
    "3x1", "3x2", "3x3", "3x4",
    "4x1", "4x2", "4x3", "4x4"
};
static const int numTypePosts = sizeof(typePosts) / sizeof(typePosts[0]);

const char* prefixes[] = {
    "static",
    "const",
    "uniform",
    "struct",

    "sampler",

    "sampler1D",
    "sampler2D",
    "sampler3D",
    "samplerCUBE",

    "sampler1DShadow",   // GLSL
    "sampler2DShadow",   // GLSL
    "sampler3DShadow",   // GLSL
    "samplerCubeShadow", // GLSL

    "sampler2DMS", // GLSL
};
static const int numPrefixes = sizeof(prefixes) / sizeof(prefixes[0]);

// For GLSL we need to have the names for the renderparms so we can look up their run time indices within the renderprograms
static const char* GLSLParmNames[] = {
    "rpScreenCorrectionFactor",
    "rpWindowCoord",
    "rpDiffuseModifier",
    "rpSpecularModifier",

    "rpLocalLightOrigin",
    "rpLocalViewOrigin",

    "rpLightProjectionS",
    "rpLightProjectionT",
    "rpLightProjectionQ",
    "rpLightFalloffS",

    "rpBumpMatrixS",
    "rpBumpMatrixT",

    "rpDiffuseMatrixS",
    "rpDiffuseMatrixT",

    "rpSpecularMatrixS",
    "rpSpecularMatrixT",

    "rpVertexColorModulate",
    "rpVertexColorAdd",

    "rpColor",
    "rpViewOrigin",
    "rpGlobalEyePos",

    "rpMVPmatrixX",
    "rpMVPmatrixY",
    "rpMVPmatrixZ",
    "rpMVPmatrixW",

    "rpModelMatrixX",
    "rpModelMatrixY",
    "rpModelMatrixZ",
    "rpModelMatrixW",

    "rpProjectionMatrixX",
    "rpProjectionMatrixY",
    "rpProjectionMatrixZ",
    "rpProjectionMatrixW",

    "rpModelViewMatrixX",
    "rpModelViewMatrixY",
    "rpModelViewMatrixZ",
    "rpModelViewMatrixW",

    "rpTextureMatrixS",
    "rpTextureMatrixT",

    "rpTexGen0S",
    "rpTexGen0T",
    "rpTexGen0Q",
    "rpTexGen0Enabled",

    "rpTexGen1S",
    "rpTexGen1T",
    "rpTexGen1Q",
    "rpTexGen1Enabled",

    "rpWobbleSkyX",
    "rpWobbleSkyY",
    "rpWobbleSkyZ",

    "rpOverbright",
    "rpEnableSkinning",
    "rpAlphaTest"
};

/*
========================
StripDeadCode
========================
*/
idStr StripDeadCode(const idStr& in, const char* name)
{
    if (r_skipStripDeadCode.GetBool()) {
        return in;
    }

    // idLexer src( LEXFL_NOFATALERRORS );
    idParser src(LEXFL_NOFATALERRORS);
    src.LoadMemory(in.c_str(), in.Length(), name);
    src.AddDefine("PC");

    switch (glConfig.driverType) {
    case GLDRV_OPENGL_ES2:
    case GLDRV_OPENGL_ES3:
        src.AddDefine("GLES2");
        break;
    }

    if (glConfig.gpuSkinningAvailable) {
        src.AddDefine("USE_GPU_SKINNING");
    }

    idList<idCGBlock> blocks;

    blocks.SetNum(100);

    idToken token;
    while (!src.EndOfFile()) {
        idCGBlock& block = blocks.Alloc();
        // read prefix
        while (src.ReadToken(&token)) {
            bool found = false;
            for (int i = 0; i < numPrefixes; i++) {
                if (token == prefixes[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (int i = 0; i < numTypes; i++) {
                    if (token == types[i]) {
                        found = true;
                        break;
                    }
                    int typeLen = idStr::Length(types[i]);
                    if (token.Cmpn(types[i], typeLen) == 0) {
                        for (int j = 0; j < numTypePosts; j++) {
                            if (idStr::Cmp(token.c_str() + typeLen, typePosts[j]) == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (found) {
                            break;
                        }
                    }
                }
            }
            if (found) {
                if (block.prefix.Length() > 0 && token.WhiteSpaceBeforeToken()) {
                    block.prefix += ' ';
                }
                block.prefix += token;
            } else {
                src.UnreadToken(&token);
                break;
            }
        }
        if (!src.ReadToken(&token)) {
            blocks.SetNum(blocks.Num() - 1);
            break;
        }
        block.name = token;

        if (src.PeekTokenString("=") || src.PeekTokenString(":") || src.PeekTokenString("[")) {
            src.ReadToken(&token);
            block.postfix = token;
            while (src.ReadToken(&token)) {
                if (token == ";") {
                    block.postfix += ';';
                    break;
                } else {
                    if (token.WhiteSpaceBeforeToken()) {
                        block.postfix += ' ';
                    }
                    block.postfix += token;
                }
            }
        } else if (src.PeekTokenString("(")) {
            idStr parms, body;
            src.ParseBracedSection(parms, -1, true, '(', ')');
            if (src.CheckTokenString(";")) {
                block.postfix = parms + ";";
            } else {
                src.ParseBracedSection(body, -1, true, '{', '}');
                block.postfix = parms + " " + body;
            }
        } else if (src.PeekTokenString("{")) {
            src.ParseBracedSection(block.postfix, -1, true, '{', '}');
            if (src.CheckTokenString(";")) {
                block.postfix += ';';
            }
        } else if (src.CheckTokenString(";")) {
            block.postfix = idStr(';');
        } else {
            src.Warning("Could not strip dead code -- unknown token %s\n", token.c_str());
            return in;
        }
    }

    idList<int, TAG_RENDERPROG> stack;
    for (int i = 0; i < blocks.Num(); i++) {
        blocks[i].used = ((blocks[i].name == "main")
            || blocks[i].name.Right(4) == "_ubo");

        if (blocks[i].name == "include") {
            blocks[i].used = true;
            blocks[i].name = ""; // clear out the include tag
        }

        if (blocks[i].used) {
            stack.Append(i);
        }
    }

    while (stack.Num() > 0) {
        int i = stack[stack.Num() - 1];
        stack.SetNum(stack.Num() - 1);

        idLexer src(LEXFL_NOFATALERRORS);
        src.LoadMemory(blocks[i].postfix.c_str(), blocks[i].postfix.Length(), name);
        while (src.ReadToken(&token)) {
            for (int j = 0; j < blocks.Num(); j++) {
                if (!blocks[j].used) {
                    if (token == blocks[j].name) {
                        blocks[j].used = true;
                        stack.Append(j);
                    }
                }
            }
        }
    }

    idStr out;

    for (int i = 0; i < blocks.Num(); i++) {
        if (blocks[i].used) {
            out += blocks[i].prefix;
            out += ' ';
            out += blocks[i].name;
            out += ' ';
            out += blocks[i].postfix;
            out += '\n';
        }
    }

    return out;
}

struct typeConversion_t {
    const char* typeCG;
    const char* typeGLSL;
} typeConversion[] = {
    { "void", "void" },

    { "fixed", "float" },

    { "float", "float" },
    { "float2", "vec2" },
    { "float3", "vec3" },
    { "float4", "vec4" },

    { "half", "float" },
    { "half2", "vec2" },
    { "half3", "vec3" },
    { "half4", "vec4" },

    { "int", "int" },
    { "int2", "ivec2" },
    { "int3", "ivec3" },
    { "int4", "ivec4" },

    { "bool", "bool" },
    { "bool2", "bvec2" },
    { "bool3", "bvec3" },
    { "bool4", "bvec4" },

    { "float2x2", "mat2x2" },
    { "float2x3", "mat2x3" },
    { "float2x4", "mat2x4" },

    { "float3x2", "mat3x2" },
    { "float3x3", "mat3x3" },
    { "float3x4", "mat3x4" },

    { "float4x2", "mat4x2" },
    { "float4x3", "mat4x3" },
    { "float4x4", "mat4x4" },

    { "sampler1D", "sampler1D" },
    { "sampler2D", "sampler2D" },
    { "sampler3D", "sampler3D" },
    { "samplerCUBE", "samplerCube" },

    { "sampler1DShadow", "sampler1DShadow" },
    { "sampler2DShadow", "sampler2DShadow" },
    { "sampler3DShadow", "sampler3DShadow" },
    { "samplerCubeShadow", "samplerCubeShadow" },

    { "sampler2DMS", "sampler2DMS" },

    { NULL, NULL }
};

// RB begin
#if defined(USE_GLES2)
const char* vertexInsert_GLSL_ES_1_0 = {
    "#version 100\n"
    "#define GLES2\n"
    "#define PC\n"
    "precision mediump float;\n"
    "\n"
#if defined(__vita__)
    // vitaGL translates this GLSL to Cg and hands it to ShaccCg, and Cg already
    // provides saturate(), clip(), tex2D(), texCUBE() and tex2Dproj() as system
    // functions. Redeclaring them is an error there ("declaration of overloaded
    // function clashes with a system-declared function"), and the shim signatures
    // additionally name a parameter "sampler", which is a reserved Cg keyword.
    // The shader bodies call these by exactly the names Cg uses, so dropping the
    // shims lets the built-ins serve instead.
#else
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
#endif
    //"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    "\n"
};
#else
const char* vertexInsert_GLSL_ES_1_0 = {
    "#version 100\n"

#if !defined(USE_MESA)
    "#define GLES2\n"
#endif

    "#define PC\n"

#if defined(USE_GPU_SKINNING) && !defined(__ANDROID__)
    "#extension GL_ARB_gpu_shader5 : enable\n"
#endif

#if 1 // defined(__ANDROID__)
    "precision mediump float;\n"
#else
    "precision highp float;\n"
#endif

    "\n"
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    //"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    "\n"
};
#endif // #if defined(USE_GLES2)

const char* vertexInsert_GLSL_1_50 = {
    "#version 140\n"
    "#define PC\n"
    "\n"
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    "\n"
};

#if defined(USE_GLES2)
const char* fragmentInsert_GLSL_ES_1_0 = {
    "#version 100\n"
    "#define GLES2\n"
    "#define PC\n"
#if !defined(__vita__)
    // vitaGL translates GLSL to Cg and hands the result to ShaccCg. Its
    // translator strips #version and precision statements but not #extension,
    // and Cg's preprocessor rejects the directive outright. Nothing in the
    // builtin shader table uses dFdx/dFdy: only simpleshade.pixel does, and
    // that entry is commented out in RenderProgs.cpp.
    "#extension GL_OES_standard_derivatives : enable\n"
#endif
    "precision mediump float;\n"
    "\n"
#if defined(__vita__)
    // See the note in vertexInsert_GLSL_ES_1_0: Cg supplies clip(), saturate(),
    // tex2D(), texCUBE() and tex2Dproj() itself, so these shims collide with the
    // system declarations, and "sampler" is a reserved keyword there so the
    // parameter name is a syntax error on top of that.
#else
    "void clip( float v ) { if ( v < 0.0 ) { discard; } }\n"
    "void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }\n"
    "\n"
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "\n"
    "vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture2D( sampler, texcoord.xy ); }\n"
    //"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }\n"
    "\n"
    //"vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }\n"
    //"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }\n"
    //"\n"
    "vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return textureCube( sampler, texcoord.xyz ); }\n"
    //"vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( textureCube( sampler, texcoord.xyzw ) ); }\n"
    "\n"
    //"vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return texture2DProj( sampler, texcoord ); }\n"
#endif
    //"vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "\n"
    //"vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }\n"
    //"vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }\n"
    //"vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"\n"
    //"vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }\n"
    //"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    //"vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"\n"
};
#else
const char* fragmentInsert_GLSL_ES_1_0 = {
    "#version 100\n"

#if !defined(USE_MESA)
    "#define GLES2\n"
#endif

    "#define PC\n"

#if 1 // defined(__ANDROID__)
    "precision mediump float;\n"
//"#extension GL_OES_standard_derivatives : enable\n"
#else
    "precision highp float;\n"
#endif
    "\n"
    "void clip( float v ) { if ( v < 0.0 ) { discard; } }\n"
    "void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }\n"
    "\n"
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "\n"
    "vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture2D( sampler, texcoord.xy ); }\n"
    //"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }\n"
    "\n"
    //"vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }\n"
    //"vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }\n"
    //"\n"
    "vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return textureCube( sampler, texcoord.xyz ); }\n"
    //"vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( textureCube( sampler, texcoord.xyzw ) ); }\n"
    "\n"
    //"vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return texture2DProj( sampler, texcoord ); }\n"
    //"vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "\n"
    //"vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }\n"
    //"vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }\n"
    //"vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"\n"
    //"vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }\n"
    //"vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    //"vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    //"\n"
};
#endif // #if defined(USE_GLES2)

const char* fragmentInsert_GLSL_1_50 = {
    "#version 140\n"
    "#define PC\n"
    "\n"
    "void clip( float v ) { if ( v < 0.0 ) { discard; } }\n"
    "void clip( vec2 v ) { if ( any( lessThan( v, vec2( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec3 v ) { if ( any( lessThan( v, vec3( 0.0 ) ) ) ) { discard; } }\n"
    "void clip( vec4 v ) { if ( any( lessThan( v, vec4( 0.0 ) ) ) ) { discard; } }\n"
    "\n"
    "float saturate( float v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec2 saturate( vec2 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec3 saturate( vec3 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "vec4 saturate( vec4 v ) { return clamp( v, 0.0, 1.0 ); }\n"
    "\n"
    "vec4 tex2D( sampler2D sampler, vec2 texcoord ) { return texture( sampler, texcoord.xy ); }\n"
    "vec4 tex2D( sampler2DShadow sampler, vec3 texcoord ) { return vec4( texture( sampler, texcoord.xyz ) ); }\n"
    "\n"
    "vec4 tex2D( sampler2D sampler, vec2 texcoord, vec2 dx, vec2 dy ) { return textureGrad( sampler, texcoord.xy, dx, dy ); }\n"
    "vec4 tex2D( sampler2DShadow sampler, vec3 texcoord, vec2 dx, vec2 dy ) { return vec4( textureGrad( sampler, texcoord.xyz, dx, dy ) ); }\n"
    "\n"
    "vec4 texCUBE( samplerCube sampler, vec3 texcoord ) { return texture( sampler, texcoord.xyz ); }\n"
    "vec4 texCUBE( samplerCubeShadow sampler, vec4 texcoord ) { return vec4( texture( sampler, texcoord.xyzw ) ); }\n"
    "\n"
    "vec4 tex1Dproj( sampler1D sampler, vec2 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "vec4 tex2Dproj( sampler2D sampler, vec3 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "vec4 tex3Dproj( sampler3D sampler, vec4 texcoord ) { return textureProj( sampler, texcoord ); }\n"
    "\n"
    "vec4 tex1Dbias( sampler1D sampler, vec4 texcoord ) { return texture( sampler, texcoord.x, texcoord.w ); }\n"
    "vec4 tex2Dbias( sampler2D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xy, texcoord.w ); }\n"
    "vec4 tex3Dbias( sampler3D sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    "vec4 texCUBEbias( samplerCube sampler, vec4 texcoord ) { return texture( sampler, texcoord.xyz, texcoord.w ); }\n"
    "\n"
    "vec4 tex1Dlod( sampler1D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.x, texcoord.w ); }\n"
    "vec4 tex2Dlod( sampler2D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xy, texcoord.w ); }\n"
    "vec4 tex3Dlod( sampler3D sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    "vec4 texCUBElod( samplerCube sampler, vec4 texcoord ) { return textureLod( sampler, texcoord.xyz, texcoord.w ); }\n"
    "\n"
};
// RB end

struct builtinConversion_t {
    const char* nameCG;
    const char* nameGLSL;
} builtinConversion[] = {
    { "frac", "fract" },
    { "lerp", "mix" },
    { "rsqrt", "inversesqrt" },
    { "ddx", "dFdx" },
    { "ddy", "dFdy" },

    { NULL, NULL }
};

struct inOutVariable_t {
    idStr type;
    idStr nameCg;
    idStr nameGLSL;
    bool declareInOut;
};

/*
========================
ParseInOutStruct
========================
*/
void ParseInOutStruct(idLexer& src, int attribType, int attribIgnoreType, idList<inOutVariable_t>& inOutVars)
{
    src.ExpectTokenString("{");

    while (!src.CheckTokenString("}")) {
        inOutVariable_t var;

        idToken token;
        src.ReadToken(&token);
        var.type = token;
        src.ReadToken(&token);
        var.nameCg = token;

        if (!src.CheckTokenString(":")) {
            src.SkipUntilString(";");
            continue;
        }

        src.ReadToken(&token);
        var.nameGLSL = token;
        src.ExpectTokenString(";");

        // convert the type
        for (int i = 0; typeConversion[i].typeCG != NULL; i++) {
            if (var.type.Cmp(typeConversion[i].typeCG) == 0) {
                var.type = typeConversion[i].typeGLSL;
                break;
            }
        }

        // convert the semantic to a GLSL name
        for (int i = 0; attribsPC[i].semantic != NULL; i++) {
            if ((attribsPC[i].flags & attribType) != 0) {
                if (var.nameGLSL.Cmp(attribsPC[i].semantic) == 0) {
                    var.nameGLSL = attribsPC[i].glsl;
                    break;
                }
            }
        }

        // check if it was defined previously
        var.declareInOut = true;
        for (int i = 0; i < inOutVars.Num(); i++) {
            if (var.nameGLSL == inOutVars[i].nameGLSL) {
                var.declareInOut = false;
                break;
            }
        }

        // RB: ignore reserved builtin gl_ uniforms
        for (int i = 0; attribsPC[i].semantic != NULL; i++) {
            if (var.nameGLSL.Cmp(attribsPC[i].glsl) == 0) {
                if ((attribsPC[i].flags & attribIgnoreType) != 0) {
                    var.declareInOut = false;
                    break;
                }
            }
        }
        // RB end

        inOutVars.Append(var);
    }

    src.ExpectTokenString(";");
}

/*
========================
ConvertCG2GLSL
========================
*/
idStr ConvertCG2GLSL(const idStr& in, const char* name, bool isVertexProgram, idStr& uniforms)
{
    idStr program;
    program.ReAllocate(in.Length() * 2, false);

    idList<inOutVariable_t, TAG_RENDERPROG> varsIn;
    idList<inOutVariable_t, TAG_RENDERPROG> varsOut;
    idList<idStr> uniformList;

    idLexer src(LEXFL_NOFATALERRORS);
    src.LoadMemory(in.c_str(), in.Length(), name);

    bool inMain = false;
    const char* uniformArrayName = isVertexProgram ? VERTEX_UNIFORM_ARRAY_NAME : FRAGMENT_UNIFORM_ARRAY_NAME;
    char newline[128] = { "\n" };

    idToken token;
    while (src.ReadToken(&token)) {

        // check for uniforms
        while (token == "uniform" && src.CheckTokenString("float4")) {
            src.ReadToken(&token);
            uniformList.Append(token);

            // strip ': register()' from uniforms
            if (src.CheckTokenString(":")) {
                if (src.CheckTokenString("register")) {
                    src.SkipUntilString(";");
                }
            }

            src.ReadToken(&token);
        }

        // convert the in/out structs
        if (token == "struct") {
            if (src.CheckTokenString("VS_IN")) {
                ParseInOutStruct(src, AT_VS_IN, 0, varsIn);
                program += "\n\n";
                for (int i = 0; i < varsIn.Num(); i++) {
                    if (varsIn[i].declareInOut) {
                        // RB begin
                        switch (glConfig.driverType) {
                        case GLDRV_OPENGL_ES2:
                        case GLDRV_OPENGL_ES3:
                        case GLDRV_OPENGL_MESA: {
                            program += "attribute " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
                            break;
                        }

                        default: {
                            program += "in " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
                            break;
                        }
                        }
                        // RB end
                    }
                }
                continue;
            } else if (src.CheckTokenString("VS_OUT")) {
                // RB begin
                ParseInOutStruct(src, AT_VS_OUT, AT_VS_OUT_RESERVED, varsOut);
                // RB end

                program += "\n";
                for (int i = 0; i < varsOut.Num(); i++) {
                    if (varsOut[i].declareInOut) {
                        // RB begin
                        switch (glConfig.driverType) {
                        case GLDRV_OPENGL_ES2:
                        case GLDRV_OPENGL_ES3:
                        case GLDRV_OPENGL_MESA: {
                            program += "varying " + varsOut[i].type + " " + varsOut[i].nameGLSL + ";\n";
                            break;
                        }

                        default: {
                            program += "out " + varsOut[i].type + " " + varsOut[i].nameGLSL + ";\n";
                            break;
                        }
                        }
                        // RB end
                    }
                }
                continue;
            } else if (src.CheckTokenString("PS_IN")) {
                ParseInOutStruct(src, AT_PS_IN, AT_PS_IN_RESERVED, varsIn);
                program += "\n\n";
                for (int i = 0; i < varsIn.Num(); i++) {
                    if (varsIn[i].declareInOut) {
                        // RB begin
                        switch (glConfig.driverType) {
                        case GLDRV_OPENGL_ES2:
                        case GLDRV_OPENGL_ES3:
                        case GLDRV_OPENGL_MESA: {
                            program += "varying " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
                            break;
                        }

                        default:
                            program += "in " + varsIn[i].type + " " + varsIn[i].nameGLSL + ";\n";
                            break;
                        }
                        // RB end
                    }
                }
                inOutVariable_t var;
                var.type = "vec4";
                var.nameCg = "position";
                var.nameGLSL = "gl_FragCoord";
                varsIn.Append(var);
                continue;
            } else if (src.CheckTokenString("PS_OUT")) {
                // RB begin
                ParseInOutStruct(src, AT_PS_OUT, AT_PS_OUT_RESERVED, varsOut);
                // RB end

                program += "\n";
                for (int i = 0; i < varsOut.Num(); i++) {
                    if (varsOut[i].declareInOut) {
                        program += "out " + varsOut[i].type + " " + varsOut[i].nameGLSL + ";\n";
                    }
                }
                continue;
            }
        }

        // strip 'static'
        if (token == "static") {
            program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
            src.SkipWhiteSpace(true); // remove white space between 'static' and the next token
            continue;
        }

        // strip ': register()' from uniforms
        if (token == ":") {
            if (src.CheckTokenString("register")) {
                src.SkipUntilString(";");
                program += ";";
                continue;
            }
        }

        // strip in/program parameters from main
        if (token == "void" && src.CheckTokenString("main")) {
            src.ExpectTokenString("(");
            while (src.ReadToken(&token)) {
                if (token == ")") {
                    break;
                }
            }

            program += "\nvoid main()";
            inMain = true;
            continue;
        }

        // strip 'const' from local variables in main()
        if (token == "const" && inMain) {
            program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
            src.SkipWhiteSpace(true); // remove white space between 'const' and the next token
            continue;
        }

        // maintain indentation
        if (token == "{") {
            program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
            program += "{";

            int len = Min(idStr::Length(newline) + 1, (int)sizeof(newline) - 1);
            newline[len - 1] = '\t';
            newline[len - 0] = '\0';
            continue;
        }
        if (token == "}") {
            int len = Max(idStr::Length(newline) - 1, 0);
            newline[len] = '\0';

            program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
            program += "}";
            continue;
        }

        // check for a type conversion
        bool foundType = false;
        for (int i = 0; typeConversion[i].typeCG != NULL; i++) {
            if (token.Cmp(typeConversion[i].typeCG) == 0) {
                program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                program += typeConversion[i].typeGLSL;
                foundType = true;
                break;
            }
        }
        if (foundType) {
            continue;
        }

        if (r_useUniformArrays.GetBool()) {
            // check for uniforms that need to be converted to the array
            bool isUniform = false;
            for (int i = 0; i < uniformList.Num(); i++) {
                if (token == uniformList[i]) {
                    program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                    program += va("%s[%d /* %s */]", uniformArrayName, i, uniformList[i].c_str());
                    isUniform = true;
                    break;
                }
            }
            if (isUniform) {
                continue;
            }
        }

        // check for input/output parameters
        if (src.CheckTokenString(".")) {

            if (token == "vertex" || token == "fragment") {
                idToken member;
                src.ReadToken(&member);

                bool foundInOut = false;
                for (int i = 0; i < varsIn.Num(); i++) {
                    if (member.Cmp(varsIn[i].nameCg) == 0) {
                        program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                        program += varsIn[i].nameGLSL;
                        foundInOut = true;
                        break;
                    }
                }
                if (!foundInOut) {
                    src.Error("invalid input parameter %s.%s", token.c_str(), member.c_str());
                    program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                    program += token;
                    program += ".";
                    program += member;
                }
                continue;
            }

            if (token == "result") {
                idToken member;
                src.ReadToken(&member);

                bool foundInOut = false;
                for (int i = 0; i < varsOut.Num(); i++) {
                    if (member.Cmp(varsOut[i].nameCg) == 0) {
                        program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                        program += varsOut[i].nameGLSL;
                        foundInOut = true;
                        break;
                    }
                }
                if (!foundInOut) {
                    src.Error("invalid output parameter %s.%s", token.c_str(), member.c_str());
                    program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                    program += token;
                    program += ".";
                    program += member;
                }
                continue;
            }

            program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
            program += token;
            program += ".";
            continue;
        }

        // check for a function conversion
        bool foundFunction = false;
        for (int i = 0; builtinConversion[i].nameCG != NULL; i++) {
            if (token.Cmp(builtinConversion[i].nameCG) == 0) {
                program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
                program += builtinConversion[i].nameGLSL;
                foundFunction = true;
                break;
            }
        }
        if (foundFunction) {
            continue;
        }

        program += (token.linesCrossed > 0) ? newline : (token.WhiteSpaceBeforeToken() > 0 ? " " : "");
        program += token;
    }

    idStr out;

    // RB: changed to allow multiple versions of GLSL
    if (isVertexProgram) {
        switch (glConfig.driverType) {
        case GLDRV_OPENGL_ES2:
        case GLDRV_OPENGL_ES3:
        case GLDRV_OPENGL_MESA: {
            out.ReAllocate(idStr::Length(vertexInsert_GLSL_ES_1_0) + in.Length() * 2, false);
            out += vertexInsert_GLSL_ES_1_0;
            break;
        }

        default: {
            out.ReAllocate(idStr::Length(vertexInsert_GLSL_1_50) + in.Length() * 2, false);
            out += vertexInsert_GLSL_1_50;
            break;
        }
        }

    } else {
        switch (glConfig.driverType) {
        case GLDRV_OPENGL_ES2:
        case GLDRV_OPENGL_ES3:
        case GLDRV_OPENGL_MESA: {
            out.ReAllocate(idStr::Length(fragmentInsert_GLSL_ES_1_0) + in.Length() * 2, false);
            out += fragmentInsert_GLSL_ES_1_0;
            break;
        }

        default: {
            out.ReAllocate(idStr::Length(fragmentInsert_GLSL_1_50) + in.Length() * 2, false);
            out += fragmentInsert_GLSL_1_50;
            break;
        }
        }
    }
    // RB end

    if (uniformList.Num() > 0) {
        if (r_useUniformArrays.GetBool()) {
            out += va("\nuniform vec4 %s[%d];\n", uniformArrayName, uniformList.Num());
        } else {
            out += "\n";
            for (int i = 0; i < uniformList.Num(); i++) {
                out += "uniform vec4 ";
                out += uniformList[i];
                out += ";\n";
            }
        }
    }

    out += program;

    for (int i = 0; i < uniformList.Num(); i++) {
        uniforms += uniformList[i];
        uniforms += "\n";
    }
    uniforms += "\n";

    return out;
}

/*
================================================================================================
idRenderProgManager::LoadGLSLShader
================================================================================================
*/
GLuint idRenderProgManager::LoadGLSLShader(GLenum target, const char* name, idList<int>& uniforms)
{

    idStr inFile;
    idStr outFileHLSL;
    idStr outFileGLSL;
    idStr outFileUniforms;

    // RB: replaced backslashes
    inFile.Format("renderprogs/%s", name);
    inFile.StripFileExtension();
    // the plain stretch (BUILTIN_POSTPROCESS_PLAIN) is postprocess's source
    // under its own name - its own cache files, and never the filter's swap
    idStr srcFile = inFile;
    if (srcFile.Icmp("renderprogs/postprocess_plain") == 0) {
        srcFile = "renderprogs/postprocess";
    }
    outFileHLSL.Format("renderprogs/hlsl/%s", name);
    outFileHLSL.StripFileExtension();

    switch (glConfig.driverType) {
    case GLDRV_OPENGL_ES2:
    case GLDRV_OPENGL_ES3:
    case GLDRV_OPENGL_MESA: {
        outFileGLSL.Format("renderprogs/glsles-1.0/%s", name);
        outFileUniforms.Format("renderprogs/glsles-1.0/%s", name);
        break;
    }

    default: {
        outFileGLSL.Format("renderprogs/glsl-1.50/%s", name);
        outFileUniforms.Format("renderprogs/glsl-1.50/%s", name);
    }
    }

    outFileGLSL.StripFileExtension();
    outFileUniforms.StripFileExtension();
    // RB end

    if (target == GL_FRAGMENT_SHADER && VitaIsPostProcess(inFile)) {
        // (whichever way it is built - converted now or from the cache)
        r_vitaPostFilterBuilt = vita_postFilter.GetBool() && !vita_fxaa.GetBool();
    }
    if (target == GL_FRAGMENT_SHADER) {
        inFile += ".pixel";
        srcFile += ".pixel";
        outFileHLSL += "_fragment.hlsl";
        outFileGLSL += "_fragment.glsl";
        outFileUniforms += "_fragment.uniforms";
    } else {
        inFile += ".vertex";
        srcFile += ".vertex";
        outFileHLSL += "_vertex.hlsl";
        outFileGLSL += "_vertex.glsl";
        outFileUniforms += "_vertex.uniforms";
    }

    // first check whether we already have a valid GLSL file and compare it to the hlsl timestamp;
    ID_TIME_T hlslTimeStamp;
    int hlslFileLength = fileSystem->ReadFile(srcFile.c_str(), NULL, &hlslTimeStamp);

    ID_TIME_T glslTimeStamp;
    int glslFileLength = fileSystem->ReadFile(outFileGLSL.c_str(), NULL, &glslTimeStamp);

    // if the glsl file doesn't exist or we have a newer HLSL file we need to recreate the glsl file.
    idStr programGLSL;
#if defined(__vita__)
    bool vitaFastFragment = false; // the lighter interaction replaced main()
    // Everything the Vita rewrites below depend on. Bump the version when one
    // of them changes; a cached file without this exact last line is redone.
    const idStr vitaSignature = va("\n// vita-glsl 3 fast%d tk%d ca%d st%d%d%d fx%d pf%d\n",
        vita_fastInteraction.GetBool() ? 1 : 0, vita_toksvigShader.GetBool() ? 1 : 0, vita_compoundAssign.GetInteger(),
        vitaSelfTest.ran ? 1 : 0, vitaSelfTest.ycocgCompound, vitaSelfTest.ycocgExpanded,
        (vita_fxaa.GetBool() && VitaIsPostProcessFragment(inFile, target)) ? 1 : 0,
        (vita_postFilter.GetBool() && !vita_fxaa.GetBool() && VitaIsPostProcess(inFile)) ? 1 : 0);
    bool vitaStale = false;
    if (glslFileLength > 0 && !(hlslTimeStamp > glslTimeStamp) && !r_alwaysExportGLSL.GetBool()) {
        void* cached = NULL;
        const int cachedLen = fileSystem->ReadFile(outFileGLSL.c_str(), &cached);
        if (cachedLen > vitaSignature.Length()
            && idStr::Cmp((const char*)cached + cachedLen - vitaSignature.Length(), vitaSignature.c_str()) == 0) {
            programGLSL = (const char*)cached;
        } else {
            vitaStale = true;
        }
        if (cached != NULL) {
            Mem_Free(cached);
        }
        if (!vitaStale) {
            bool ambient = false;
            vitaFastFragment = (target == GL_FRAGMENT_SHADER) && vita_fastInteraction.GetBool() && VitaIsInteractionProgram(inFile, &ambient);
        }
    }
#endif
    idStr programUniforms;
#if defined(__vita__)
    if ((glslFileLength <= 0) || (hlslTimeStamp > glslTimeStamp) || r_alwaysExportGLSL.GetBool() || vitaStale) {
#else
    if ((glslFileLength <= 0) || (hlslTimeStamp > glslTimeStamp) || r_alwaysExportGLSL.GetBool()) {
#endif
        if (hlslFileLength <= 0) {
            // hlsl file doesn't even exist bail out
            idLib::Warning("Couldn't load shader source '%s'", inFile.c_str());
            return INVALID_PROGID;
        }

        void* hlslFileBuffer = NULL;
        int len = fileSystem->ReadFile(srcFile.c_str(), &hlslFileBuffer);
        if (len <= 0) {
            return INVALID_PROGID;
        }
        idStr hlslCode((const char*)hlslFileBuffer);
#if defined(__vita__)
        // Screen-space lookups of _currentRender (the heat haze shaders) turn
        // the fragment's window position into a 0..1 texcoord with
        //
        //     vpos.xy * rpWindowCoord.xy        (global.inc)
        //
        // which assumes OpenGL's bottom-left origin for gl_FragCoord. vitaGL
        // passes GXM's WPOS through, and on the display that counts rows from
        // the top, so every lookup came back upside down - the mirrored pause
        // menu over the ceiling. The copy in _currentRender is bottom-up like
        // GL's, so flip the position instead: substitute a Vita version of the
        // helper that measures y from the bottom. Done on the source text
        // before StripDeadCode resolves the include, so the original
        // global.inc is left alone.
        if (target == GL_FRAGMENT_SHADER && hlslCode.Find("vposToScreenPosTexCoord(", true) >= 0) {
            const int inc = hlslCode.Find("#include \"global.inc\"", true);
            const int eol = (inc >= 0) ? hlslCode.Find('\n', inc) : -1;
            if (eol >= 0) {
                hlslCode.Replace("vposToScreenPosTexCoord(", "vitaVposToScreenPosTexCoord(");
                // rpWindowCoord.w is (display height - view's GL y) / view
                // height: 1 for the scaled 3D view in the display's top-left
                // corner, and at full resolution (tr_backend_draw.cpp)
                hlslCode.Insert("static float2 vitaVposToScreenPosTexCoord( float2 vpos ) { return float2( vpos.x * rpWindowCoord.x, rpWindowCoord.w - vpos.y * rpWindowCoord.y ); }\n", eol + 1);
                common->Printf("vita: %s reads gl_FragCoord for a screen lookup; y measured from the bottom as GL expects\n", inFile.c_str());
            } else {
                common->Warning("vita: %s uses vposToScreenPosTexCoord but has no #include \"global.inc\" line; screen lookups will be upside down", inFile.c_str());
            }
        }
#endif
#if defined(__vita__)
        bool vitaAmbient = false;
        if (vita_fastInteraction.GetBool() && VitaIsInteractionProgram(inFile, &vitaAmbient)) {
            if (target == GL_VERTEX_SHADER) {
                // mirror the projection's divisor into .z so the fragment shader can
                // feed .xyz to tex2Dproj untouched; .xyw (the stock reads) is unchanged
                if (hlslCode.Find("result.texcoord3.z = 0.0f;", true) >= 0 && hlslCode.Find("rpLightProjectionQ );", true) >= 0) {
                    hlslCode.Replace("result.texcoord3.z = 0.0f;", "");
                    hlslCode.Replace("rpLightProjectionQ );", "rpLightProjectionQ );\n\tresult.texcoord3.z = result.texcoord3.w;");
                    common->Printf("vita: %s: projection divisor mirrored into texcoord3.z for the lighter interaction\n", inFile.c_str());
                } else {
                    common->Warning("vita: %s does not look like the stock interaction vertex shader; left alone", inFile.c_str());
                }
            } else {
                const int mainAt = hlslCode.Find("void main(", true);
                if (mainAt >= 0) {
                    idStr body = vitaFastInteractionMain;
                    body.Replace("VITA_LIGHTVECTOR", vitaAmbient
                        ? "VITA_H3 lightVector = normalize( VITA_H3( 0.5, 9.5 - 0.385, 0.8925 ) );"
                        : "VITA_H3 lightVector = normalize( fragment.texcoord0.xyz );");
                    body.Replace("VITA_SPECULAR", vita_toksvigShader.GetBool() ? vitaSpecularToksvig : vitaSpecularFixed);
                    hlslCode.CapLength(mainAt);
                    hlslCode += body;
                    vitaFastFragment = true;
                    common->Printf("vita: %s: lighter interaction shader%s, %s highlight\n", inFile.c_str(), vitaAmbient ? " (ambient)" : "",
                        vita_toksvigShader.GetBool() ? "Toksvig" : "fixed pow( x, 10 ) (vita_toksvigShader 0)");
                } else {
                    common->Warning("vita: %s has no main(); stock interaction shader kept", inFile.c_str());
                }
            }
        }
#endif
        bool vitaFxaaFragment = false; // (logged; the compile's fallback tests vita_fxaa itself)
        if (vita_fxaa.GetBool() && VitaIsPostProcessFragment(inFile, target)) {
            const int mainAt = hlslCode.Find("void main(", true);
            if (mainAt >= 0) {
                hlslCode.CapLength(mainAt);
                hlslCode += vitaFxaaMain;
                vitaFxaaFragment = true;
                common->Printf("vita: %s: FXAA (NVIDIA FXAA 3.11, console) on the 3D view's stretch\n", inFile.c_str());
            } else {
                common->Warning("vita: %s has no main(); no FXAA", inFile.c_str());
            }
        }
        if (!vitaFxaaFragment && vita_postFilter.GetBool() && !vita_fxaa.GetBool() && VitaIsPostProcess(inFile)) {
            hlslCode = (target == GL_FRAGMENT_SHADER) ? vitaPostFilterPixel : vitaPostFilterVertex;
            common->Printf("vita: %s: sharpening + soft alpha-tested edges on the 3D view's stretch (%s)\n", inFile.c_str(),
                target == GL_FRAGMENT_SHADER ? "fragment" : "vertex: the taps' coordinates");
        }
        idStr programHLSL = StripDeadCode(hlslCode, inFile);
        programGLSL = ConvertCG2GLSL(programHLSL, inFile, target == GL_VERTEX_SHADER, programUniforms);
#if defined(__vita__)
        if (vitaFastFragment) {
            // placeholders that survive the Cg->GLSL pass; Cg (what vitaGL compiles
            // in the end) has half types natively
            programGLSL.Replace("VITA_H4", "half4");
            programGLSL.Replace("VITA_H3", "half3");
            programGLSL.Replace("VITA_H1", "half");
        }
#endif

#if defined(__vita__)
        // Compound multiply-assign, e.g. ConvertYCoCgToRGB's
        //
        //     YCoCg. xy *= YCoCg. z ;
        //
        // vitaGL's GLSL-to-Cg pass leaves '*=' alone (utils/glsl_utils.c,
        // glsl_inject_mul: "FIXME: *= still not handled") and hands it to
        // ShaccCg, which accepts it as ordinary Cg. Whether ShaccCg then
        // computes it correctly is measured rather than assumed: the boot
        // self-test runs this exact decode both ways and reads the pixels
        // back. Keep the statement unless the self-test proved the compound
        // form wrong and the expanded form right. An earlier build expanded it
        // unconditionally and the whole world rendered black, without either
        // form ever having been measured.
        {
            int count = 0;
            for (int at = programGLSL.Find("*=", true); at >= 0; at = programGLSL.Find("*=", true, at + 2)) {
                count++;
            }
            if (count > 0) {
                const int forced = vita_compoundAssign.GetInteger();
                bool expand = false;
                const char* why;
                if (forced == 1) {
                    why = "kept: vita_compoundAssign 1";
                } else if (forced == 2) {
                    expand = true;
                    why = "expanded: vita_compoundAssign 2";
                } else if (!vitaSelfTest.ran) {
                    why = "kept: self-test did not run";
                } else if (vitaSelfTest.ycocgCompound == 0 && vitaSelfTest.ycocgExpanded == 1) {
                    expand = true;
                    why = "expanded: self-test measured the compound form WRONG and the expanded form right";
                } else if (vitaSelfTest.ycocgCompound == 1) {
                    why = "kept: self-test measured the compound form correct";
                } else {
                    why = "kept: self-test inconclusive (see selftest lines)";
                }
                int done = 0;
                if (expand) {
                    done = VitaExpandCompoundMultiplies(programGLSL);
                }
                common->Printf("vita: %s has %d compound multipl%s, %s%s\n", outFileGLSL.c_str(), count,
                    count == 1 ? "y" : "ies", why, (expand && done != count) ? " (some inside for-clauses left alone)" : "");
            }
        }
#endif

#if defined(__vita__)
        // the intermediate HLSL is only of use when debugging the converter
        if (r_alwaysExportGLSL.GetBool()) {
            fileSystem->WriteFile(outFileHLSL, programHLSL.c_str(), programHLSL.Length(), "fs_basepath");
        }
        programGLSL += vitaSignature; // a trailing comment: the compiler ignores it
#else
        fileSystem->WriteFile(outFileHLSL, programHLSL.c_str(), programHLSL.Length(), "fs_basepath");
#endif
        fileSystem->WriteFile(outFileGLSL, programGLSL.c_str(), programGLSL.Length(), "fs_basepath");
        if (r_useUniformArrays.GetBool()) {
            fileSystem->WriteFile(outFileUniforms, programUniforms.c_str(), programUniforms.Length(), "fs_basepath");
        }
    } else {
#if defined(__vita__)
        // programGLSL was read (and its signature checked) above
#else
        // read in the glsl file
        void* fileBufferGLSL = NULL;
        int lengthGLSL = fileSystem->ReadFile(outFileGLSL.c_str(), &fileBufferGLSL);
        if (lengthGLSL <= 0) {
            idLib::Error("GLSL file %s could not be loaded and may be corrupt", outFileGLSL.c_str());
        }
        programGLSL = (const char*)fileBufferGLSL;
        Mem_Free(fileBufferGLSL);
#endif

        if (r_useUniformArrays.GetBool()) {
            // read in the uniform file
            void* fileBufferUniforms = NULL;
            int lengthUniforms = fileSystem->ReadFile(outFileUniforms.c_str(), &fileBufferUniforms);
            if (lengthUniforms <= 0) {
                idLib::Error("uniform file %s could not be loaded and may be corrupt", outFileUniforms.c_str());
            }
            programUniforms = (const char*)fileBufferUniforms;
            Mem_Free(fileBufferUniforms);
        }
    }

    // find the uniforms locations in either the vertex or fragment uniform array
    if (r_useUniformArrays.GetBool()) {
        uniforms.Clear();

        idLexer src(programUniforms, programUniforms.Length(), "uniforms");
        idToken token;
        while (src.ReadToken(&token)) {
            int index = -1;
            for (int i = 0; i < RENDERPARM_TOTAL && index == -1; i++) {
                const char* parmName = GetGLSLParmName(i);
                if (token == parmName) {
                    index = i;
                }
            }
            for (int i = 0; i < MAX_GLSL_USER_PARMS && index == -1; i++) {
                const char* parmName = GetGLSLParmName(RENDERPARM_USER + i);
                if (token == parmName) {
                    index = RENDERPARM_USER + i;
                }
            }
            if (index == -1) {
                idLib::Error("couldn't find uniform %s for %s", token.c_str(), outFileGLSL.c_str());
            }
            uniforms.Append(index);
        }
    }

    // A material parsed on the game/draw thread (one first seen during play)
    // asks for its programs from there. Everything up to here is CPU work and
    // fills in 'uniforms'; the GL part happens when the back end first binds
    // the program (idRenderProgManager::BindShader), on the GL thread - vitaGL
    // does not survive GL calls from anywhere else.
    if (!R_IsGLThread()) {
        static int deferred = 0;
        if (deferred++ < 8) {
            common->Printf("%s shader '%s' requested off the GL thread: compiled at its first use\n",
                (target == GL_FRAGMENT_SHADER) ? "fragment" : "vertex", inFile.c_str());
        }
        return INVALID_PROGID;
    }

    // create and compile the shader
    const GLuint shader = qglCreateShader(target);
    if (shader) {
        const char* source[1] = { programGLSL.c_str() };

        qglShaderSource(shader, 1, source, NULL);
        qglCompileShader(shader);

        int infologLength = 0;
        qglGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infologLength);
        if (infologLength > 1) {
            idTempArray<char> infoLog(infologLength);
            int charsWritten = 0;
            qglGetShaderInfoLog(shader, infologLength, &charsWritten, infoLog.Ptr());

            // catch the strings the ATI and Intel drivers output on success
            if (strstr(infoLog.Ptr(), "successfully compiled to run on hardware") != NULL || strstr(infoLog.Ptr(), "No errors.") != NULL) {
                // idLib::Printf( "%s program %s from %s compiled to run on hardware\n", typeName, GetName(), GetFileName() );
            } else if (r_displayGLSLCompilerMessages.GetBool()) // DG:  check for the CVar I added above
            {
                idLib::Printf("While compiling %s program %s\n", (target == GL_FRAGMENT_SHADER) ? "fragment" : "vertex", inFile.c_str());

                const char separator = '\n';
                idList<idStr> lines;
                lines.Clear();
                idStr source(programGLSL);
                lines.Append(source);
                for (int index = 0, ofs = lines[index].Find(separator); ofs != -1; index++, ofs = lines[index].Find(separator)) {
                    lines.Append(lines[index].c_str() + ofs + 1);
                    lines[index].CapLength(ofs);
                }

                idLib::Printf("-----------------\n");
                for (int i = 0; i < lines.Num(); i++) {
                    idLib::Printf("%3d: %s\n", i + 1, lines[i].c_str());
                }
                idLib::Printf("-----------------\n");

                idLib::Printf("%s\n", infoLog.Ptr());
            }
        }

        GLint compiled = GL_FALSE;
        qglGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE) {
            // Upstream discards this silently and only complains once linking
            // fails, which loses which stage was at fault.
            idLib::Warning("FAILED to compile %s shader '%s'",
                (target == GL_FRAGMENT_SHADER) ? "fragment" : "vertex", inFile.c_str());
            if (vita_postFilter.GetBool() && !vita_fxaa.GetBool() && VitaIsPostProcess(inFile)) {
                // the plain stretch rather than no picture: say why
                char log[1024] = "";
                qglGetShaderInfoLog(shader, sizeof(log), NULL, log);
                qglDeleteShader(shader);
                common->Warning("vita: the sharpening / soft edge shader did not compile for %s, using the plain stretch. Compiler said: %s", inFile.c_str(), log[0] ? log : "(nothing)");
                vita_postFilter.SetBool(false);
                r_vitaPostFilterBuilt = false;
                return LoadGLSLShader(target, name, uniforms);
            }
            if (vita_fxaa.GetBool() && VitaIsPostProcessFragment(inFile, target)) {
                // the stretch without FXAA rather than no picture: say why
                char log[1024] = "";
                qglGetShaderInfoLog(shader, sizeof(log), NULL, log);
                qglDeleteShader(shader);
                common->Warning("vita: the FXAA shader did not compile for %s, using the plain stretch. Compiler said: %s", inFile.c_str(), log[0] ? log : "(nothing)");
                vita_fxaa.SetBool(false);
                return LoadGLSLShader(target, name, uniforms);
            }
#if defined(__vita__)
            if (vitaFastFragment) {
                // never leave the world unlit over an optimisation: say why and
                // build the stock shader instead (its .xyw reads do not mind the
                // vertex shader's mirrored .z)
                char log[1024] = "";
                qglGetShaderInfoLog(shader, sizeof(log), NULL, log);
                qglDeleteShader(shader);
                common->Warning("vita: the lighter interaction shader did not compile for %s, using the stock one. Compiler said: %s",
                    inFile.c_str(), log[0] ? log : "(nothing)");
                vita_fastInteraction.SetBool(false);
                const GLuint stock = LoadGLSLShader(target, name, uniforms);
                vita_fastInteraction.SetBool(true);
                return stock;
            }
#endif
            qglDeleteShader(shader);
            return INVALID_PROGID;
        }
    }

    return shader;
}
/*
================================================================================================
idRenderProgManager::FindGLSLProgram
================================================================================================
*/
int idRenderProgManager::FindGLSLProgram(const char* name, int vIndex, int fIndex)
{

    for (int i = 0; i < glslPrograms.Num(); ++i) {
        if ((glslPrograms[i].vertexShaderIndex == vIndex) && (glslPrograms[i].fragmentShaderIndex == fIndex)) {
            LoadGLSLProgram(i, vIndex, fIndex);
            return i;
        }
    }

    glslProgram_t program;
    program.name = name;
    int index = glslPrograms.Append(program);
    LoadGLSLProgram(index, vIndex, fIndex);
    return index;
}

/*
================================================================================================
idRenderProgManager::GetGLSLParmName
================================================================================================
*/
const char* idRenderProgManager::GetGLSLParmName(int rp) const
{
    if (rp >= RENDERPARM_USER) {
        int userParmIndex = rp - RENDERPARM_USER;
        return va("rpUser%d", userParmIndex);
    }
    assert(rp < RENDERPARM_TOTAL);
    return GLSLParmNames[rp];
}

/*
================================================================================================
idRenderProgManager::SetUniformValue
================================================================================================
*/
void idRenderProgManager::SetUniformValue(const renderParm_t rp, const float* value)
{
    for (int i = 0; i < 4; i++) {
        glslUniforms[rp][i] = value[i];
    }
}

/*
================================================================================================
idRenderProgManager::CommitUnforms
================================================================================================
*/
void idRenderProgManager::CommitUniforms()
{
    const int progID = GetGLSLCurrentProgram();
    const glslProgram_t& prog = glslPrograms[progID];

    if (r_useUniformArrays.GetBool()) {
        ALIGNTYPE16 idVec4 localVectors[RENDERPARM_USER + MAX_GLSL_USER_PARMS];

        if (prog.vertexShaderIndex >= 0) {
            const idList<int>& vertexUniforms = vertexShaders[prog.vertexShaderIndex].uniforms;
            if (prog.vertexUniformArray != -1 && vertexUniforms.Num() > 0) {
                for (int i = 0; i < vertexUniforms.Num(); i++) {
                    localVectors[i] = glslUniforms[vertexUniforms[i]];
                }
                qglUniform4fv(prog.vertexUniformArray, vertexUniforms.Num(), localVectors->ToFloatPtr());
            }
        }

        if (prog.fragmentShaderIndex >= 0) {
            const idList<int>& fragmentUniforms = fragmentShaders[prog.fragmentShaderIndex].uniforms;
            if (prog.fragmentUniformArray != -1 && fragmentUniforms.Num() > 0) {
                for (int i = 0; i < fragmentUniforms.Num(); i++) {
                    localVectors[i] = glslUniforms[fragmentUniforms[i]];
                }
                qglUniform4fv(prog.fragmentUniformArray, fragmentUniforms.Num(), localVectors->ToFloatPtr());
            }
        }
    } else {
        for (int i = 0; i < prog.uniformLocations.Num(); i++) {
            const glslUniformLocation_t& uniformLocation = prog.uniformLocations[i];
            qglUniform4fv(uniformLocation.uniformIndex, 1, glslUniforms[uniformLocation.parmIndex].ToFloatPtr());
        }
    }
}

class idSort_QuickUniforms : public idSort_Quick<glslUniformLocation_t, idSort_QuickUniforms> {
public:
    int Compare(const glslUniformLocation_t& a, const glslUniformLocation_t& b) const
    {
        return a.uniformIndex - b.uniformIndex;
    }
};

/*
================================================================================================
idRenderProgManager::LoadGLSLProgram
================================================================================================
*/
void idRenderProgManager::LoadGLSLProgram(const int programIndex, const int vertexShaderIndex, const int fragmentShaderIndex)
{
    glslProgram_t& prog = glslPrograms[programIndex];

    if (prog.progId != INVALID_PROGID) {
        return; // Already loaded
    }
    if (!R_IsGLThread()) {
        return; // linked when the back end first binds it (see LoadGLSLShader)
    }

    GLuint vertexProgID = (vertexShaderIndex != -1) ? vertexShaders[vertexShaderIndex].progId : INVALID_PROGID;
    GLuint fragmentProgID = (fragmentShaderIndex != -1) ? fragmentShaders[fragmentShaderIndex].progId : INVALID_PROGID;

    const GLuint program = qglCreateProgram();
    if (program) {

        if (vertexProgID != INVALID_PROGID) {
            qglAttachShader(program, vertexProgID);
        }

        if (fragmentProgID != INVALID_PROGID) {
            qglAttachShader(program, fragmentProgID);
        }

        // bind vertex attribute locations
        for (int i = 0; attribsPC[i].glsl != NULL; i++) {
            if ((attribsPC[i].flags & AT_VS_IN) != 0) {
                qglBindAttribLocation(program, attribsPC[i].bind, attribsPC[i].glsl);
            }
        }

        qglLinkProgram(program);

        int infologLength = 0;
        qglGetProgramiv(program, GL_INFO_LOG_LENGTH, &infologLength);
        if (infologLength > 1) {
            char* infoLog = (char*)malloc(infologLength);
            int charsWritten = 0;
            qglGetProgramInfoLog(program, infologLength, &charsWritten, infoLog);

            // catch the strings the ATI and Intel drivers output on success
            if (strstr(infoLog, "Vertex shader(s) linked, fragment shader(s) linked.") != NULL || strstr(infoLog, "No errors.") != NULL) {
                // idLib::Printf( "render prog %s from %s linked\n", GetName(), GetFileName() );
            } else {
                idLib::Printf("While linking GLSL program %d with vertexShader %s and fragmentShader %s\n",
                    programIndex,
                    (vertexShaderIndex >= 0) ? vertexShaders[vertexShaderIndex].name.c_str() : "<Invalid>",
                    (fragmentShaderIndex >= 0) ? fragmentShaders[fragmentShaderIndex].name.c_str() : "<Invalid>");
                idLib::Printf("%s\n", infoLog);
            }

            free(infoLog);
        }
    }

    int linked = GL_FALSE;
    qglGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        qglDeleteProgram(program);
        idLib::Error("While linking GLSL program %d with vertexShader %s and fragmentShader %s\n",
            programIndex,
            (vertexShaderIndex >= 0) ? vertexShaders[vertexShaderIndex].name.c_str() : "<Invalid>",
            (fragmentShaderIndex >= 0) ? fragmentShaders[fragmentShaderIndex].name.c_str() : "<Invalid>");
        return;
    }

    if (r_useUniformArrays.GetBool()) {
        prog.vertexUniformArray = qglGetUniformLocation(program, VERTEX_UNIFORM_ARRAY_NAME);
        prog.fragmentUniformArray = qglGetUniformLocation(program, FRAGMENT_UNIFORM_ARRAY_NAME);

        assert(prog.vertexUniformArray != -1 || vertexShaderIndex < 0 || vertexShaders[vertexShaderIndex].uniforms.Num() == 0);
        assert(prog.fragmentUniformArray != -1 || fragmentShaderIndex < 0 || fragmentShaders[fragmentShaderIndex].uniforms.Num() == 0);

    } else {
        // store the uniform locations after we have linked the GLSL program
        prog.uniformLocations.Clear();
        for (int i = 0; i < RENDERPARM_TOTAL; i++) {
            const char* parmName = GetGLSLParmName(i);
            GLint loc = qglGetUniformLocation(program, parmName);
            if (loc != -1) {
                glslUniformLocation_t uniformLocation;
                uniformLocation.parmIndex = i;
                uniformLocation.uniformIndex = loc;
                prog.uniformLocations.Append(uniformLocation);
            }
        }

        // store the USER uniform locations
        for (int i = 0; i < MAX_GLSL_USER_PARMS; i++) {
            const char* parmName = GetGLSLParmName(RENDERPARM_USER + i);
            GLint loc = qglGetUniformLocation(program, parmName);
            if (loc != -1) {
                glslUniformLocation_t uniformLocation;
                uniformLocation.parmIndex = RENDERPARM_USER + i;
                uniformLocation.uniformIndex = loc;
                prog.uniformLocations.Append(uniformLocation);
            }
        }

        // sort the uniforms based on index
        prog.uniformLocations.SortWithTemplate(idSort_QuickUniforms());
    }

    // RB: only load joint uniform buffers if available
    if (glConfig.gpuSkinningAvailable) {
        // get the uniform buffer binding for skinning joint matrices
        GLint blockIndex = qglGetUniformBlockIndex(program, "matrices_ubo");
        if (blockIndex != -1) {
            qglUniformBlockBinding(program, blockIndex, 0);
        }
    }
    // RB end

    // set the texture unit locations once for the render program. We only need to do this once since we only link the program once
    qglUseProgram(program);
#if defined(__vita__)
    idStr vitaSamplers;
#endif
    for (int i = 0; i < MAX_PROG_TEXTURE_PARMS; ++i) {
        GLint loc = qglGetUniformLocation(program, va("samp%d", i));
        if (loc != -1) {
            qglUniform1i(loc, i);
#if defined(__vita__)
            vitaSamplers += va(" samp%d->unit%d", i, i);
#endif
        }
    }
#if defined(__vita__)
    // One line per program: what vitaGL actually kept after ShaccCg. A
    // sampler or uniform array that silently fell out of the compiled program
    // (location -1) means the engine's values never reach the shader, which
    // renders as black or flat colour with no error anywhere.
    {
        GLint nUnif = 0;
        qglGetProgramiv(program, GL_ACTIVE_UNIFORMS, &nUnif);
        idStr names;
        for (GLint u = 0; u < nUnif && u < 24; u++) {
            char name[64] = { 0 };
            GLint size = 0;
            GLenum type = 0;
            qglGetActiveUniform(program, u, sizeof(name) - 1, NULL, &size, &type, name);
            names += va(" %s[%d]", name, size);
        }
        Vita_Checkpoint("program %d '%s' + '%s': %d active uniforms:%s | vertex array loc %d, fragment array loc %d |%s",
            programIndex,
            (vertexShaderIndex >= 0) ? vertexShaders[vertexShaderIndex].name.c_str() : "<none>",
            (fragmentShaderIndex >= 0) ? fragmentShaders[fragmentShaderIndex].name.c_str() : "<none>",
            nUnif, names.c_str(), prog.vertexUniformArray, prog.fragmentUniformArray,
            vitaSamplers.Length() ? vitaSamplers.c_str() : " no samplers");
    }
#endif

    idStr programName = vertexShaders[vertexShaderIndex].name;
    programName.StripFileExtension();
    prog.name = programName;
    prog.progId = program;
    prog.fragmentShaderIndex = fragmentShaderIndex;
    prog.vertexShaderIndex = vertexShaderIndex;
}

/*
================================================================================================
idRenderProgManager::ZeroUniforms
================================================================================================
*/
void idRenderProgManager::ZeroUniforms()
{
    memset(glslUniforms.Ptr(), 0, glslUniforms.Allocated());
}
