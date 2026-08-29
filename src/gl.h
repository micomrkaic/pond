/* pond — a numerically honest wave tank as screen candy
 * Copyright (C) 2026 Mico <https://github.com/micomrkaic>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* gl.h — the slice of OpenGL 3.3 core / OpenGL ES 3.0 this program uses.
 *
 * Desktop: no system GL headers, no libGL link; every entry point is a
 * function pointer fetched through SDL_GL_GetProcAddress (works with the
 * OpenGL framework on macOS and with Mesa/NVIDIA on Linux).
 * Emscripten: the real GLES3 header, calls go straight to WebGL2.
 */
#ifndef POND_GL_H
#define POND_GL_H

#ifdef __EMSCRIPTEN__

#include <GLES3/gl3.h>
static inline int gl_load(void) { return 0; }

#else

#include <stddef.h>

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef unsigned int  GLbitfield;
typedef float         GLfloat;
typedef char          GLchar;
typedef unsigned char GLubyte;
typedef ptrdiff_t     GLsizeiptr;
typedef ptrdiff_t     GLintptr;
typedef void          GLvoid;

#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NO_ERROR 0
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LESS 0x0201
#define GL_LEQUAL 0x0203
#define GL_ALWAYS 0x0207
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_RGBA8 0x8058
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MULTISAMPLE 0x809D
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_R8 0x8229
#define GL_R32F 0x822E

#define GL_FUNCTIONS(X) \
    X(void,          glClear,                   (GLbitfield)) \
    X(void,          glClearColor,              (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,          glEnable,                  (GLenum)) \
    X(void,          glDisable,                 (GLenum)) \
    X(void,          glBlendFunc,               (GLenum, GLenum)) \
    X(void,          glDepthMask,               (GLboolean)) \
    X(void,          glDepthFunc,               (GLenum)) \
    X(void,          glCullFace,                (GLenum)) \
    X(void,          glFrontFace,               (GLenum)) \
    X(void,          glViewport,                (GLint, GLint, GLsizei, GLsizei)) \
    X(GLenum,        glGetError,                (void)) \
    X(const GLubyte*,glGetString,               (GLenum)) \
    X(void,          glPixelStorei,             (GLenum, GLint)) \
    X(void,          glReadPixels,              (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)) \
    X(void,          glGenBuffers,              (GLsizei, GLuint*)) \
    X(void,          glDeleteBuffers,           (GLsizei, const GLuint*)) \
    X(void,          glBindBuffer,              (GLenum, GLuint)) \
    X(void,          glBufferData,              (GLenum, GLsizeiptr, const void*, GLenum)) \
    X(void,          glGenVertexArrays,         (GLsizei, GLuint*)) \
    X(void,          glDeleteVertexArrays,      (GLsizei, const GLuint*)) \
    X(void,          glBindVertexArray,         (GLuint)) \
    X(void,          glEnableVertexAttribArray, (GLuint)) \
    X(void,          glVertexAttribPointer,     (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void,          glDrawArrays,              (GLenum, GLint, GLsizei)) \
    X(void,          glDrawElements,            (GLenum, GLsizei, GLenum, const void*)) \
    X(GLuint,        glCreateShader,            (GLenum)) \
    X(void,          glShaderSource,            (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
    X(void,          glCompileShader,           (GLuint)) \
    X(void,          glGetShaderiv,             (GLuint, GLenum, GLint*)) \
    X(void,          glGetShaderInfoLog,        (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,          glDeleteShader,            (GLuint)) \
    X(GLuint,        glCreateProgram,           (void)) \
    X(void,          glAttachShader,            (GLuint, GLuint)) \
    X(void,          glLinkProgram,             (GLuint)) \
    X(void,          glBindAttribLocation,      (GLuint, GLuint, const GLchar*)) \
    X(void,          glGetProgramiv,            (GLuint, GLenum, GLint*)) \
    X(void,          glGetProgramInfoLog,       (GLuint, GLsizei, GLsizei*, GLchar*)) \
    X(void,          glUseProgram,              (GLuint)) \
    X(void,          glDeleteProgram,           (GLuint)) \
    X(GLint,         glGetUniformLocation,      (GLuint, const GLchar*)) \
    X(void,          glUniform1i,               (GLint, GLint)) \
    X(void,          glUniform1f,               (GLint, GLfloat)) \
    X(void,          glUniform2f,               (GLint, GLfloat, GLfloat)) \
    X(void,          glUniform2i,               (GLint, GLint, GLint)) \
    X(void,          glUniform3f,               (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void,          glUniform4f,               (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,          glUniformMatrix4fv,        (GLint, GLsizei, GLboolean, const GLfloat*)) \
    X(void,          glGenTextures,             (GLsizei, GLuint*)) \
    X(void,          glDeleteTextures,          (GLsizei, const GLuint*)) \
    X(void,          glBindTexture,             (GLenum, GLuint)) \
    X(void,          glActiveTexture,           (GLenum)) \
    X(void,          glTexImage2D,              (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void,          glTexSubImage2D,           (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void,          glTexParameteri,           (GLenum, GLenum, GLint))

#define GL_DECL(ret, name, args) extern ret (*name) args;
GL_FUNCTIONS(GL_DECL)
#undef GL_DECL

/* Resolve every pointer above through SDL_GL_GetProcAddress. 0 on success. */
int gl_load(void);

#endif /* __EMSCRIPTEN__ */
#endif
