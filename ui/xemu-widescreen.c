/*
 * xemu wide screen handler
 *
 * Copyright (c) 2023 Matt Borgerson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "xemu-widescreen.h"
#include "qemu/atomic.h"

static bool g_widescreen = false;
static unsigned int g_display_raster_width = 640;
static unsigned int g_display_raster_height = 480;

void xemu_set_widescreen(bool widescreen)
{
    g_widescreen = widescreen;
}

bool xemu_get_widescreen(void)
{
    return g_widescreen;
}

void xemu_set_display_raster(unsigned int width, unsigned int height)
{
    if (!width || !height) {
        return;
    }
    qatomic_set(&g_display_raster_width, width);
    qatomic_set(&g_display_raster_height, height);
}

#ifdef __ANDROID__
/* The OpenXR shell receives the guest frame as an AHardwareBuffer and does not
 * pass through xui's aspect-aware presenter.  Standard-definition Xbox modes
 * use a 4:3 raster that may be anamorphic, so their PM-GPIO aspect decision is
 * authoritative.  HD modes are 16:9 carriers, but a title may deliberately
 * place a normal-aspect 960x720 image in the center of 1280x720.  In that case
 * the XR shell crops only the standardized carrier pillarbox and presents the
 * complete 4:3 image; title safe-area pixels remain untouched. */
__attribute__((visibility("default")))
float xemu_xr_get_display_aspect(void)
{
    return g_widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);
}

/* Fraction to remove from each horizontal edge of an HD carrier when the
 * guest explicitly requests normal (4:3) presentation.  In addition to the
 * 160-pixel carrier bars, the only observed normal-in-HD title (Soul Calibur
 * II) places a symmetric 32x24 black safe-area matte around its 960x720
 * image.  Cropping 192/1280 horizontally and 24/720 vertically removes only
 * those black pixels and leaves an exact 896x672 (4:3) picture.  Other titles
 * do not enter this normal-aspect HD-carrier path. */
__attribute__((visibility("default")))
float xemu_xr_get_display_crop_x(void)
{
    unsigned int width = qatomic_read(&g_display_raster_width);
    unsigned int height = qatomic_read(&g_display_raster_height);
    bool hd_16_9 = width >= 1280 &&
                   (uint64_t)width * 9 == (uint64_t)height * 16;
    return hd_16_9 && !g_widescreen ? 0.15f : 0.0f;
}

__attribute__((visibility("default")))
float xemu_xr_get_display_crop_y(void)
{
    unsigned int width = qatomic_read(&g_display_raster_width);
    unsigned int height = qatomic_read(&g_display_raster_height);
    bool hd_16_9 = width >= 1280 &&
                   (uint64_t)width * 9 == (uint64_t)height * 16;
    return hd_16_9 && !g_widescreen ? (1.0f / 30.0f) : 0.0f;
}
#endif
