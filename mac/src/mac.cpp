// What the menu of the arcade launcher needs on a Mac in place of the Pi (see shim/GLES2/gl2.h).
#define LAUNCHER_MAC_ITSELF
#include <GLES2/gl2.h>
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>

// The menu writes its shaders in GLSL ES 1.00. The Mac's OpenGL 2.1 reads GLSL 1.20, which
// has the same words apart from the precision statements of GLSL ES. Those go, and the words
// for a precision in front of a variable stand for nothing.
void mac_shader_source(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths) {
    std::string text;
    for (GLsizei i = 0; i < count; i++)
        text += (lengths && lengths[i] >= 0) ? std::string(strings[i], (size_t)lengths[i]) : std::string(strings[i]);
    static const std::regex precision(R"(precision\s+(lowp|mediump|highp)\s+\w+\s*;)");
    text = "#version 120\n#define lowp\n#define mediump\n#define highp\n" + std::regex_replace(text, precision, "");
    const GLchar* source = text.c_str();
    glShaderSource(shader, 1, &source, nullptr);
}

// The menu asks for OpenGL ES 2.0. The Mac gives it OpenGL 2.1 in its place.
int mac_gl_set_attribute(SDL_GLattr attribute, int value) {
    switch (attribute) {
    case SDL_GL_CONTEXT_PROFILE_MASK: return SDL_GL_SetAttribute(attribute, 0);
    case SDL_GL_CONTEXT_MAJOR_VERSION: return SDL_GL_SetAttribute(attribute, 2);
    case SDL_GL_CONTEXT_MINOR_VERSION: return SDL_GL_SetAttribute(attribute, 1);
    default: return SDL_GL_SetAttribute(attribute, value);
    }
}

// On the Pi the window covers the screen. Here it is a window of 16:9 in the middle of the
// screen, as large as fits with some room around it, at most 1920 x 1080 points.
// LAUNCHER_MAC_SIZE=1280x720 asks for another size. The menu fits its picture of 1920 x 1080
// into the window it gets.
SDL_Window* mac_create_window(const char* title, int, int, int, int, Uint32 flags) {
    int w = 1600, h = 900;
    SDL_Rect room;
    if (SDL_GetDisplayUsableBounds(0, &room) == 0) {
        w = std::min(1920, room.w * 9 / 10);
        h = w * 9 / 16;
        if (h > room.h * 9 / 10) {
            h = room.h * 9 / 10;
            w = h * 16 / 9;
        }
    }
    if (const char* size = getenv("LAUNCHER_MAC_SIZE"))
        sscanf(size, "%dx%d", &w, &h);
    return SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                            flags | SDL_WINDOW_ALLOW_HIGHDPI);
}

// The Pi hides the mouse pointer. On the Mac it stays in view.
int mac_show_cursor(int) {
    return SDL_ShowCursor(SDL_QUERY);
}
