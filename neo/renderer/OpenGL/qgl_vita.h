/*
===========================================================================
Vita GL header layer for Chocolate DOOM-3-BFG.

The desktop build includes <GL/gl.h> plus the Khronos glext.h and resolves
every extension entry point at runtime into PFNGL*PROC function pointers.
Neither applies here:

  * <GL/gl.h> in the VitaSDK sysroot belongs to TinyGL, an unrelated software
    rasteriser, and shadows the real headers.
  * glext.h declares the whole desktop GL surface.
  * vitaGL is linked statically, so entry points resolve at link time and the
    qgl* dispatch layer can map straight onto them.

vitaGL already supplies every GL scalar type the engine names (GLclampf,
GLdouble, GLsync, GLuint64, ...) as macros, so none are redefined here.
===========================================================================
*/
#ifndef __QGL_VITA_H__
#define __QGL_VITA_H__

#include <vitaGL.h>
#include <assert.h>

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY*
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

/* ---- sync-object enums (vitaGL has no fence/sync support) ---------------- */
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_DEPTH_BOUNDS_TEST_EXT
#define GL_DEPTH_BOUNDS_TEST_EXT 0x8890
#endif

/* ---- ARB-suffixed names are the historical spelling of core entry points -- */
#define qglBindBufferARB                glBindBuffer
#define qglDeleteBuffersARB             glDeleteBuffers
#define qglGenBuffersARB                glGenBuffers
#define qglBufferDataARB                glBufferData
#define qglBufferSubDataARB             glBufferSubData
#define qglMapBufferARB                 glMapBuffer
#define qglUnmapBufferARB               glUnmapBuffer
#define qglGetBufferParameterivARB      glGetBufferParameteriv
#define qglVertexAttribPointerARB       glVertexAttribPointer
#define qglEnableVertexAttribArrayARB   glEnableVertexAttribArray
#define qglDisableVertexAttribArrayARB  glDisableVertexAttribArray
#define qglCompressedTexImage2DARB      glCompressedTexImage2D

/* ---- entry points vitaGL provides under their core names ----------------- */
#define qglBindBufferRange              glBindBufferRange
#define qglMapBufferRange               glMapBufferRange
#define qglGenVertexArrays              glGenVertexArrays
#define qglBindVertexArray              glBindVertexArray
#define qglDeleteVertexArrays           glDeleteVertexArrays
#define qglDrawElements                 glDrawElements
#define qglDrawElementsBaseVertex       glDrawElementsBaseVertex
#define qglCreateShader                 glCreateShader
#define qglDeleteShader                 glDeleteShader
#define qglShaderSource                 glShaderSource
#define qglCompileShader                glCompileShader
#define qglReleaseShaderCompiler        glReleaseShaderCompiler
#define qglGetShaderiv                  glGetShaderiv
#define qglGetShaderInfoLog             glGetShaderInfoLog
#define qglCreateProgram                glCreateProgram
#define qglDeleteProgram                glDeleteProgram
#define qglAttachShader                 glAttachShader
#define qglLinkProgram                  glLinkProgram
#define qglUseProgram                   glUseProgram
#define qglGetProgramiv                 glGetProgramiv
#define qglGetProgramInfoLog            glGetProgramInfoLog
#define qglBindAttribLocation           glBindAttribLocation
#define qglGetUniformLocation           glGetUniformLocation
#define qglGetActiveUniform             glGetActiveUniform
#define qglUniform1i                    glUniform1i
#define qglUniform4fv                   glUniform4fv
#define qglGetUniformBlockIndex         glGetUniformBlockIndex
#define qglUniformBlockBinding          glUniformBlockBinding
#define qglStencilOpSeparate            glStencilOpSeparate
#define qglStencilFuncSeparate          glStencilFuncSeparate

#ifdef __cplusplus
extern "C" {
#endif

/* ---- emulated / stubbed entry points ------------------------------------ *
 * Only those the engine actually calls are provided. Everything else that the
 * desktop qgl.h declares is simply absent, because nothing references it.    */

/* DSA-style multitexture bind: express it with the core equivalents. */
static inline void qglBindMultiTextureEXT(GLenum texunit, GLenum target, GLuint texture)
{
    glActiveTexture(texunit);
    glBindTexture(target, texture);
}

/* Depth-bounds test is an NV/EXT rasterisation optimisation with no GXM
   equivalent; skipping it costs some fill rate but is visually identical. */
static inline void qglDepthBoundsEXT(GLclampd zmin, GLclampd zmax)
{
    (void)zmin;
    (void)zmax;
}

/* vitaGL exposes glCompressedTexImage2D but not the sub-image variant. The
   engine's single use is a full-surface update, so re-upload the level. */
/* vitaGL has no glCompressedTexSubImage2D, so a sub-rectangle update has to be
   reissued as a full level upload. That is only equivalent when the update
   covers the whole mip level, i.e. xoffset and yoffset are zero.

   Every reachable caller satisfies that: idImage::SubImageUpload() is always
   invoked with x = y = 0 and the full mip dimensions (Image_load.cpp), and the
   one caller that would pass a real offset, idImage::SetPixel(), has no callers
   anywhere in the tree. Assert rather than silently pasting a sub-rectangle over
   the whole level if that ever changes. */
static inline void qglCompressedTexSubImage2DARB(GLenum target, GLint level,
        GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
        GLenum format, GLsizei imageSize, const void* data)
{
    assert(xoffset == 0 && yoffset == 0);
    (void)xoffset;
    (void)yoffset;
    glCompressedTexImage2D(target, level, format, width, height, 0, imageSize, data);
}

/* Fence/sync objects: GXM serialises the command list itself and vitaGL
   exposes no sync API, so report every fence as already signalled. */
static inline GLsync qglFenceSync(GLenum condition, GLbitfield flags)
{
    (void)condition;
    (void)flags;
    return (GLsync)1;
}
static inline GLboolean qglIsSync(GLsync sync) { return sync ? GL_TRUE : GL_FALSE; }
static inline void qglDeleteSync(GLsync sync) { (void)sync; }
static inline GLenum qglClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    (void)sync;
    (void)flags;
    (void)timeout;
    return GL_ALREADY_SIGNALED;
}

/* GPU timer queries drive the r_speeds readout only; report zero elapsed. */
static inline void qglGenQueriesARB(GLsizei n, GLuint* ids)
{
    for (GLsizei i = 0; i < n; i++) {
        ids[i] = 1;
    }
}
static inline void qglDeleteQueriesARB(GLsizei n, const GLuint* ids) { (void)n; (void)ids; }
static inline void qglBeginQueryARB(GLenum target, GLuint id) { (void)target; (void)id; }
static inline void qglEndQueryARB(GLenum target) { (void)target; }
static inline void qglGetQueryObjectui64vEXT(GLuint id, GLenum pname, GLuint64* params)
{
    (void)id;
    (void)pname;
    if (params) {
        *params = 0;
    }
}

#ifdef __cplusplus
}
#endif


/* ---- constants absent from GLES2 / vitaGL -------------------------------- *
 * Historical ARB/EXT spellings first, then plain desktop GL enum values. Most
 * are only reached through code paths gated off by the capability flags set in
 * R_CheckPortableExtensions(); they need to compile, not to function.        */
#define GL_ARRAY_BUFFER_ARB             GL_ARRAY_BUFFER
#define GL_ELEMENT_ARRAY_BUFFER_ARB     GL_ELEMENT_ARRAY_BUFFER
#define GL_DYNAMIC_DRAW_ARB             GL_DYNAMIC_DRAW
#define GL_STREAM_DRAW_ARB              GL_STREAM_DRAW
#define GL_TEXTURE0_ARB                 GL_TEXTURE0
#define GL_MAX_TEXTURE_IMAGE_UNITS_ARB  GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_COORDS_ARB       GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_TEXTURE_CUBE_MAP_EXT         GL_TEXTURE_CUBE_MAP
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT GL_TEXTURE_CUBE_MAP_POSITIVE_X
#define GL_TIME_ELAPSED_EXT             GL_TIME_ELAPSED

#define GLintptrARB                     GLintptr
#define GLsizeiptrARB                   GLsizeiptr
#define GLuint64EXT                     GLuint64

#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER              0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR         0x1004
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL            0x813D
#endif
#ifndef GL_TEXTURE_LOD_BIAS_EXT
#define GL_TEXTURE_LOD_BIAS_EXT         0x8501
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS    0x884F
#endif
#ifndef GL_INTENSITY16
#define GL_INTENSITY16                  0x804D
#endif
#ifndef GL_LUMINANCE16_ALPHA16
#define GL_LUMINANCE16_ALPHA16          0x8048
#endif
#ifndef GL_R8
#define GL_R8                           0x8229
#endif
#ifndef GL_RG8
#define GL_RG8                          0x822B
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB             0x8DB9
#endif
#ifndef GL_MULTISAMPLE_ARB
#define GL_MULTISAMPLE_ARB              0x809D
#endif
#ifndef GL_BACK_LEFT
#define GL_BACK_LEFT                    0x0402
#endif
#ifndef GL_BACK_RIGHT
#define GL_BACK_RIGHT                   0x0403
#endif
#ifndef GL_STENCIL_INDEX
#define GL_STENCIL_INDEX                0x1901
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT                 0x8866
#endif
#ifndef GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT
#define GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT 0x8A34
#endif
#ifndef GL_UNPACK_SWAP_BYTES
#define GL_UNPACK_SWAP_BYTES            0x0CF0
#endif
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R            0x8E42
#define GL_TEXTURE_SWIZZLE_G            0x8E43
#define GL_TEXTURE_SWIZZLE_B            0x8E44
#define GL_TEXTURE_SWIZZLE_A            0x8E45
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB
#define GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB 0x8242
#endif
#ifndef GL_DEBUG_SEVERITY_LOW_ARB
#define GL_DEBUG_SEVERITY_LOW_ARB       0x9148
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GL 1.x fixed-function entry points GLES2 removed -------------------- *
 * qgl_linked.h maps qgl* straight onto these names, so they must exist. They
 * are only referenced by the debug/rendertools paths.                        */
static inline void glArrayElement(GLint i) { (void)i; }
static inline void glDrawBuffer(GLenum mode) { (void)mode; }
static inline void glReadBuffer(GLenum mode) { (void)mode; }
static inline void glRasterPos2f(GLfloat x, GLfloat y) { (void)x; (void)y; }
static inline void glDrawPixels(GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* px)
{
    (void)w; (void)h; (void)fmt; (void)type; (void)px;
}
static inline void glTexParameterfv(GLenum target, GLenum pname, const GLfloat* params)
{
    /* Only reached for GL_TEXTURE_BORDER_COLOR, which GXM has no equivalent for. */
    (void)target; (void)pname; (void)params;
}
static inline void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* params)
{
    (void)target; (void)level; (void)pname;
    if (params) { *params = 0; }
}

#ifdef __cplusplus
}
#endif

#endif /* __QGL_VITA_H__ */
