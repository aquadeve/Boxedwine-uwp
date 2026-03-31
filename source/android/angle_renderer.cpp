/*
 * Boxedwine Android Emulator - ANGLE/OpenGL ES 2.0 Renderer
 *
 * Implementation of the GLES2 fullscreen-quad renderer.
 * Uses only OpenGL ES 2.0 core calls so the code is portable between
 * ANGLE (D3D11 backend on UWP/Xbox) and native GLES2 (Android, Linux).
 *
 * Reference:
 *   referenceCode/Bridge/AppProcessAngle/SimpleRenderer.cpp
 */

#include "angle_renderer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Portable OpenGL ES 2.0 includes
 *
 * On UWP the headers come from the ANGLE distribution bundled with the
 * project.  On other platforms they come from the system SDK.
 * ---------------------------------------------------------------------- */
#ifdef _MSC_VER
#  define GL_GLEXT_PROTOTYPES
#  include <GLES2/gl2.h>
#  include <GLES2/gl2ext.h>
#else
/* Linux / Android / other: use system GLES2 headers */
#  define GL_GLEXT_PROTOTYPES
#  ifdef __ANDROID__
#    include <GLES2/gl2.h>
#    include <GLES2/gl2ext.h>
#  else
/* Desktop Linux with GLES2 headers (e.g. via mesa) */
#    include <GLES2/gl2.h>
#  endif
#endif

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */
struct AngleRenderer {
    /* Framebuffer dimensions (emulator resolution) */
    int fb_width;
    int fb_height;

    /* GLES2 resources */
    GLuint program;
    GLuint texture;
    GLuint vbo;

    /* Shader attribute/uniform locations */
    GLint a_position;
    GLint a_texcoord;
    GLint u_texture;

    /* Cursor line-drawing program */
    GLuint cursor_program;
    GLint  cursor_a_position;
    GLint  cursor_u_color;
    GLuint cursor_vbo;
};

/* -------------------------------------------------------------------------
 * Shader sources (GLES2 / GLSL ES 1.00)
 * ---------------------------------------------------------------------- */

static const char *vs_source =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

static const char *fs_source =
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
    "}\n";

/* Minimal cursor shaders (flat colour) */
static const char *cursor_vs_source =
    "attribute vec2 a_position;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *cursor_fs_source =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() {\n"
    "    gl_FragColor = u_color;\n"
    "}\n";

/* -------------------------------------------------------------------------
 * Helper: compile a GLES2 shader
 * ---------------------------------------------------------------------- */
static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "angle_renderer: shader compile error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* -------------------------------------------------------------------------
 * Helper: link a GLES2 program
 * ---------------------------------------------------------------------- */
static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "angle_renderer: program link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

/* -------------------------------------------------------------------------
 * angle_renderer_create
 * ---------------------------------------------------------------------- */
AngleRenderer *angle_renderer_create(int width, int height)
{
    AngleRenderer *r = (AngleRenderer *)calloc(1, sizeof(AngleRenderer));
    if (!r) return NULL;

    r->fb_width  = width;
    r->fb_height = height;

    /* ---- Fullscreen quad program ---- */
    r->program = link_program(vs_source, fs_source);
    if (!r->program) { free(r); return NULL; }

    r->a_position = glGetAttribLocation(r->program, "a_position");
    r->a_texcoord = glGetAttribLocation(r->program, "a_texcoord");
    r->u_texture  = glGetUniformLocation(r->program, "u_texture");

    /* ---- Fullscreen quad VBO (2 triangles, interleaved pos+uv) ---- */
    /* NDC coordinates: (-1,-1) to (1,1), UV: (0,1) to (1,0) for OpenGL */
    const float quad[] = {
        /* pos.x  pos.y  uv.u  uv.v */
        -1.0f, -1.0f,   0.0f, 1.0f,
         1.0f, -1.0f,   1.0f, 1.0f,
        -1.0f,  1.0f,   0.0f, 0.0f,
         1.0f,  1.0f,   1.0f, 0.0f,
    };
    glGenBuffers(1, &r->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /* ---- Framebuffer texture ---- */
    glGenTextures(1, &r->texture);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* Allocate storage (initially black) */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* ---- Cursor program ---- */
    r->cursor_program = link_program(cursor_vs_source, cursor_fs_source);
    if (r->cursor_program) {
        r->cursor_a_position = glGetAttribLocation(r->cursor_program, "a_position");
        r->cursor_u_color    = glGetUniformLocation(r->cursor_program, "u_color");
    }

    /* Cursor VBO (rewritten each frame) */
    glGenBuffers(1, &r->cursor_vbo);

    return r;
}

/* -------------------------------------------------------------------------
 * angle_renderer_upload
 * ---------------------------------------------------------------------- */
void angle_renderer_upload(AngleRenderer *r, const uint8_t *pixels)
{
    if (!r || !pixels) return;
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    r->fb_width, r->fb_height,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* -------------------------------------------------------------------------
 * angle_renderer_draw
 * ---------------------------------------------------------------------- */
void angle_renderer_draw(AngleRenderer *r, int screen_w, int screen_h)
{
    if (!r) return;

    glViewport(0, 0, screen_w, screen_h);
    glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glUseProgram(r->program);

    /* Bind the framebuffer texture */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    glUniform1i(r->u_texture, 0);

    /* Bind the fullscreen quad VBO */
    glBindBuffer(GL_ARRAY_BUFFER, r->vbo);

    glEnableVertexAttribArray(r->a_position);
    glVertexAttribPointer(r->a_position, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void *)0);

    glEnableVertexAttribArray(r->a_texcoord);
    glVertexAttribPointer(r->a_texcoord, 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(float), (void *)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(r->a_position);
    glDisableVertexAttribArray(r->a_texcoord);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

/* -------------------------------------------------------------------------
 * angle_renderer_draw_cursor
 * ---------------------------------------------------------------------- */
void angle_renderer_draw_cursor(AngleRenderer *r, float x, float y,
                                int screen_w, int screen_h)
{
    if (!r || !r->cursor_program) return;

    /* Convert pixel coords to NDC */
    float nx = (x / (float)screen_w) * 2.0f - 1.0f;
    float ny = 1.0f - (y / (float)screen_h) * 2.0f;

    /* Crosshair size in NDC */
    float sx = 16.0f / (float)screen_w;
    float sy = 16.0f / (float)screen_h;

    float lines[] = {
        /* Horizontal line */
        nx - sx, ny,
        nx + sx, ny,
        /* Vertical line */
        nx, ny - sy,
        nx, ny + sy,
    };

    glUseProgram(r->cursor_program);
    glUniform4f(r->cursor_u_color, 1.0f, 1.0f, 1.0f, 0.7f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindBuffer(GL_ARRAY_BUFFER, r->cursor_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lines), lines, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(r->cursor_a_position);
    glVertexAttribPointer(r->cursor_a_position, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

    glDrawArrays(GL_LINES, 0, 4);

    glDisableVertexAttribArray(r->cursor_a_position);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_BLEND);
    glUseProgram(0);
}

/* -------------------------------------------------------------------------
 * angle_renderer_resize
 * ---------------------------------------------------------------------- */
void angle_renderer_resize(AngleRenderer *r, int new_width, int new_height)
{
    if (!r) return;
    r->fb_width  = new_width;
    r->fb_height = new_height;

    glBindTexture(GL_TEXTURE_2D, r->texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, new_width, new_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* -------------------------------------------------------------------------
 * angle_renderer_destroy
 * ---------------------------------------------------------------------- */
void angle_renderer_destroy(AngleRenderer *r)
{
    if (!r) return;

    if (r->program)        glDeleteProgram(r->program);
    if (r->cursor_program) glDeleteProgram(r->cursor_program);
    if (r->texture)        glDeleteTextures(1, &r->texture);
    if (r->vbo)            glDeleteBuffers(1, &r->vbo);
    if (r->cursor_vbo)     glDeleteBuffers(1, &r->cursor_vbo);

    free(r);
}
