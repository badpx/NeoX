/* ===========================================================================
 * gl_api.h — gl.c 使用的 GL 入口
 *
 * macOS：OpenGL.framework 静态导出全部符号，本头只引入系统 gl3.h，
 *         nx_gl_load 为空操作（docs/linux-port.md W2）。
 * Linux：不包含系统 GL 头（CI 运行时包不必带开发头），类型与常量就地
 *         定义；函数指针由 gl_load.c 经 nx_gl_ctx_get_proc 填充。
 * 平台分支只出现在本适配头，不进入 gl.c。
 * =========================================================================*/
#ifndef NEOX_GL_API_H
#define NEOX_GL_API_H

#include "gl_ctx.h"

#if defined(__APPLE__)

#include <OpenGL/gl3.h>

static inline int nx_gl_load(nx_gl_ctx *ctx)
{
    (void)ctx;
    return 1;
}
static inline const char *nx_gl_load_last_error(void) { return NULL; }

#else /* Linux / 其他 EGL 桌面 GL */

#include <stddef.h>

typedef unsigned int   GLenum;
typedef unsigned int   GLbitfield;
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef ptrdiff_t      GLsizeiptr;
typedef void           GLvoid;

#define GL_FALSE                    0
#define GL_TRUE                     1
#define GL_NO_ERROR                 0
#define GL_ZERO                     0
#define GL_ONE                      1
#define GL_TRIANGLES                0x0004
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_DEPTH_TEST               0x0B71
#define GL_STENCIL_TEST             0x0B90
#define GL_DITHER                   0x0BD0
#define GL_BLEND                    0x0BE2
#define GL_SCISSOR_TEST             0x0C11
#define GL_CULL_FACE                0x0B44
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_PACK_ALIGNMENT           0x0D05
#define GL_MAX_TEXTURE_SIZE         0x0D33
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNSIGNED_BYTE            0x1401
#define GL_FLOAT                    0x1406
#define GL_RED                      0x1903
#define GL_RGBA                     0x1908
#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_NEAREST                  0x2600
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_FUNC_ADD                 0x8006
#define GL_RGBA8                    0x8058
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_MAJOR_VERSION            0x821B
#define GL_MINOR_VERSION            0x821C
#define GL_R32F                     0x822E
#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE1                 0x84C1
#define GL_TEXTURE2                 0x84C2
#define GL_ARRAY_BUFFER             0x8892
#define GL_STREAM_DRAW              0x88E0
#define GL_ONE_MINUS_SRC1_ALPHA     0x88FB
#define GL_MAX_DUAL_SOURCE_DRAW_BUFFERS 0x88FC
#define GL_RGBA32F                  0x8814
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_READ_FRAMEBUFFER         0x8CA8
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#define GL_COLOR_ATTACHMENT0        0x8CE0
#define GL_STENCIL_ATTACHMENT       0x8D20
#define GL_STENCIL_INDEX8           0x8D48
#define GL_FRAMEBUFFER              0x8D40
#define GL_RENDERBUFFER             0x8D41
#define GL_FRAMEBUFFER_SRGB         0x8DB9

/* 第三参用 const char **，与 gl.c 里 `const char *src; glShaderSource(..., &src, ...)`
 * 的实参类型一致（const char *const * 会在 -Wextra 下告不兼容）。 */
#define NX_GL_PROCS(X) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glBlendEquation, (GLenum mode)) \
    X(void, glBlendFuncSeparate, (GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glClearColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint *buffers)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
    X(void, glDeleteProgram, (GLuint program)) \
    X(void, glDeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(void, glDeleteTextures, (GLsizei n, const GLuint *textures)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glFinish, (void)) \
    X(void, glFlush, (void)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetIntegerv, (GLenum pname, GLint *data)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(GLint, glGetUniformLocation, (GLuint program, const char *name)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)) \
    X(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const char **string, const GLint *length)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glTexSubImage2D, (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
    X(void, glUniform2i, (GLint location, GLint v0, GLint v1)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniformMatrix3fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat *value)) \
    X(void, glUseProgram, (GLuint program)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height))

#define NX_GL_DECL(ret, name, args) \
    typedef ret (*PFN_##name) args; \
    extern PFN_##name name;
NX_GL_PROCS(NX_GL_DECL)
#undef NX_GL_DECL

int         nx_gl_load(nx_gl_ctx *ctx);
const char *nx_gl_load_last_error(void);

#endif /* __APPLE__ */
#endif /* NEOX_GL_API_H */
