// The menu of the arcade launcher includes <GLES2/gl2.h> for OpenGL ES 2, which macOS lacks.
// The Mac build finds this file in its place. It gives the menu the Mac's own OpenGL 2.1,
// which has every function and constant the menu uses, and it sends four calls of the menu
// through src/mac.cpp, which adapts them to the Mac.
#pragma once

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

// Linux names the time of a file's last change st_mtim, macOS names it st_mtimespec.
#define st_mtim st_mtimespec

#if defined(__cplusplus) && !defined(LAUNCHER_MAC_ITSELF)
#include <SDL.h>

void mac_shader_source(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths);
int mac_gl_set_attribute(SDL_GLattr attribute, int value);
SDL_Window* mac_create_window(const char* title, int x, int y, int w, int h, Uint32 flags);
int mac_show_cursor(int toggle);

#define glShaderSource mac_shader_source
#define SDL_GL_SetAttribute mac_gl_set_attribute
#define SDL_CreateWindow mac_create_window
#define SDL_ShowCursor mac_show_cursor
#endif
