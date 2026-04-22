#pragma once

#include <GL/gl.h>

// Call once from a thread with an active GL context.
void texture_init();

// 0 until texture_init() succeeds.
GLuint texture_id_unheld();
GLuint texture_id_held();
GLuint texture_id_ring();
