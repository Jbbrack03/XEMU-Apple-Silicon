/*
 * S3TC Texture Decompression
 *
 * Copyright (c) 2020 Wilhelm Kovatch
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

#ifndef HW_XBOX_NV2A_PGRAPH_S3TC_H
#define HW_XBOX_NV2A_PGRAPH_S3TC_H

#include <stdint.h>
#include <stdbool.h>

/* Test hook (see s3tc.c): force scalar path for NEON-vs-scalar self-test. */
extern bool s3tc_disable_neon;

enum S3TC_DECOMPRESS_FORMAT {
    S3TC_DECOMPRESS_FORMAT_DXT1,
    S3TC_DECOMPRESS_FORMAT_DXT3,
    S3TC_DECOMPRESS_FORMAT_DXT5,
};

uint8_t *s3tc_decompress_3d(enum S3TC_DECOMPRESS_FORMAT color_format,
                            const uint8_t *data, unsigned int width,
                            unsigned int height, unsigned int depth);

uint8_t *s3tc_decompress_2d(enum S3TC_DECOMPRESS_FORMAT color_format,
                            const uint8_t *data, unsigned int width,
                            unsigned int height);

/* Decode directly into a caller-provided RGBA8 buffer (width*height*depth*4
 * bytes), avoiding the intermediate allocation + copy on the upload path. */
void s3tc_decompress_3d_to(uint8_t *out,
                           enum S3TC_DECOMPRESS_FORMAT color_format,
                           const uint8_t *data, unsigned int width,
                           unsigned int height, unsigned int depth);

void s3tc_decompress_2d_to(uint8_t *out,
                           enum S3TC_DECOMPRESS_FORMAT color_format,
                           const uint8_t *data, unsigned int width,
                           unsigned int height);

/* NEON-vs-scalar bit-exactness self-test (see s3tc.c). Returns true if all
 * checks pass; optionally reports the number of checks/failures run. */
bool pgraph_texture_selftest(int *out_checks, int *out_failures);

#endif
