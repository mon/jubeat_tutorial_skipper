#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include <GL/gl.h>

// GL 1.2 enum, not always defined by mingw's gl.h.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "log.hpp"
#include "texture.hpp"

#include "unheld_dims.h"
#include "held_dims.h"

static constexpr uint8_t unheld_rgba[] = {
    #embed "unheld.rgba"
};
static_assert(sizeof(unheld_rgba) == UNHELD_RGBA_SIZE);
static_assert(sizeof(unheld_rgba) == UNHELD_W * UNHELD_H * 4);

static constexpr uint8_t held_rgba[] = {
    #embed "held.rgba"
};
static_assert(sizeof(held_rgba) == HELD_RGBA_SIZE);
static_assert(sizeof(held_rgba) == HELD_W * HELD_H * 4);

static GLuint tex_unheld = 0;
static GLuint tex_held = 0;
static GLuint tex_ring = 0;

static GLuint upload(const uint8_t *rgba, unsigned w, unsigned h) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return id;
}

// Ring cross-section: alpha peaks at the radial midline and fades at
// inner/outer edges so the ring reads as a soft glowing tube. Width is 1
// (no angular variation) — the texture is stretched around the full ring
// via UVs at draw time. Color comes from glColor; the texture is RGB white.
static constexpr int RING_TEX_W = 1;
static constexpr int RING_TEX_H = 32;
static uint8_t ring_rgba[RING_TEX_W * RING_TEX_H * 4];

static void build_ring_texture() {
    for(int y = 0; y < RING_TEX_H; y++) {
        float v = (float)y / (float)(RING_TEX_H - 1);
        float d = v - 0.5f;
        float a = 1.0f - 4.0f * d * d;  // parabolic peak at v=0.5
        if(a < 0.0f) a = 0.0f;
        a = a * a;  // sharper falloff at the edges
        for(int x = 0; x < RING_TEX_W; x++) {
            uint8_t *p = &ring_rgba[(y * RING_TEX_W + x) * 4];
            p[0] = 0xFF; p[1] = 0xFF; p[2] = 0xFF;
            p[3] = (uint8_t)(a * 255.0f + 0.5f);
        }
    }
}

void texture_init() {
    if(tex_unheld) return;

    tex_unheld = upload(unheld_rgba, UNHELD_W, UNHELD_H);
    tex_held = upload(held_rgba, HELD_W, HELD_H);
    build_ring_texture();
    tex_ring = upload(ring_rgba, RING_TEX_W, RING_TEX_H);

    log_misc("texture: overlays uploaded (unheld %ux%u id=%u, held %ux%u id=%u, ring %ux%u id=%u)",
             UNHELD_W, UNHELD_H, tex_unheld, HELD_W, HELD_H, tex_held,
             RING_TEX_W, RING_TEX_H, tex_ring);
}

GLuint texture_id_unheld() { return tex_unheld; }
GLuint texture_id_held() { return tex_held; }
GLuint texture_id_ring() { return tex_ring; }
