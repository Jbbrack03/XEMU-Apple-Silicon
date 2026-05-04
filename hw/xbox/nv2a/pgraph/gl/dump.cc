/*
 * NV2A PGRAPH OpenGL renderer — per-draw color RT dump (W4).
 *
 * Apple Silicon performance fork. Provides a tiny C-callable shim
 * around fpng so the GL renderer's draw.c (C, not C++) can dump the
 * post-MSAA-resolve color buffer of a draw to a PNG without dragging
 * fpng's <vector>-flavored C++ surface area into the .c file.
 *
 * The caller (gl/draw.c) is responsible for:
 *   - Resolving the MSAA buffer (surface_resolve_msaa or equivalent).
 *   - Reading the pixels via glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE)
 *     into a host buffer.
 *   - Calling pgraph_gl_dump_rgba8_png_to_file() with that buffer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <cstdint>
#include <cstdio>

#include <fpng.h>

extern "C" bool pgraph_gl_dump_rgba8_png_to_file(const char *filename,
                                                 const uint8_t *rgba,
                                                 uint32_t w,
                                                 uint32_t h)
{
    static bool s_inited = false;
    if (!s_inited) {
        fpng::fpng_init();
        s_inited = true;
    }
    if (filename == nullptr || rgba == nullptr || w == 0 || h == 0) {
        return false;
    }
    return fpng::fpng_encode_image_to_file(filename, rgba, w, h, 4, 0);
}
