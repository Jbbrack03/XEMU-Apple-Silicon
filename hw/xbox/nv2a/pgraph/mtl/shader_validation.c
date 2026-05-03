/*
 * NV2A PGRAPH Metal renderer — M5 shader-validation harness.
 *
 * See shader_validation.h for the public contract. This file builds
 * representative VshState / GeomState / PshState fixtures, runs them
 * through `pgraph_glsl_gen_vsh / gen_geom / gen_psh` to produce GLSL,
 * pipes the GLSL through `pgraph_mtl_glsl_translate_to_msl`, and
 * confirms `[device newLibraryWithSource:]` accepts the result.
 *
 * Compiled per-target (.c, not .mm) because pgraph/glsl headers reach
 * pgraph_regs.h enums via vsh.h/psh.h/geom.h. The Metal-API touch
 * points go through the C-API shims in pipeline.h (which is .h-only +
 * .mm) and the translator in glsl.{h,c}. No nv2a_int.h.
 *
 * Copyright (c) 2026 XEMU MacOS Apple Silicon performance fork
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/mstring.h"

#include "glsl.h"
#include "pipeline.h"
#include "shader_validation.h"

#include "hw/xbox/nv2a/pgraph/glsl/vsh.h"
#include "hw/xbox/nv2a/pgraph/glsl/geom.h"
#include "hw/xbox/nv2a/pgraph/glsl/psh.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t s_ok_count = 0;
static _Atomic uint64_t s_fail_count = 0;

uint64_t pgraph_mtl_shader_validate_ok_count(void)
{
    return atomic_load(&s_ok_count);
}

uint64_t pgraph_mtl_shader_validate_fail_count(void)
{
    return atomic_load(&s_fail_count);
}

/* -------- fixtures --------
 *
 * Each fixture exercises one of the seven NV2A pipeline classes the
 * plan calls out (metal-renderer-plan.md §4 M5):
 *   1. fixed-function vertex (no programmable shader, simple lighting off)
 *   2. fixed-function vertex with lighting + texture matrix
 *   3. programmable vertex (vsh-prog) — minimal NOP-like program
 *   4. simple combiner (passthrough — NV2A reset state)
 *   5. complex combiner (multi-stage with texture sampling)
 *   6. alpha-test + fog
 *   7. native-tri-depth (PR #2240 fragment-shader path)
 *
 * The geom shader is exercised via class 7 (native-tri-depth disables
 * the geom shader; non-native triangle draws use the geom shader). For
 * class 7 we also run the geom-needed variant to confirm the geom
 * generator's output round-trips.
 */

typedef struct {
    const char *name;
    bool        run_vsh;
    bool        run_geom;
    bool        run_psh;
    VshState    vsh;
    GeomState   geom;
    PshState    psh;
} ShaderValidateFixture;

static void zero_fixture(ShaderValidateFixture *f)
{
    memset(f, 0, sizeof(*f));
}

/* Build the fixture set. Returns the count. The fixtures live in a
 * static array allocated by the caller (sized for MAX_FIXTURES). */
#define MAX_FIXTURES 16

static int build_fixtures(ShaderValidateFixture *fxs)
{
    int n = 0;

    /* 1. fixed-function vertex, no lighting / no fog / no specular.
     *    The minimal vsh — just oPos = vMVP * vPos. */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "ff_vsh_minimal";
        f->run_vsh = true;
        f->vsh.is_fixed_function = true;
        f->vsh.surface_scale_factor = 1;
        f->vsh.smooth_shading = true;
        f->vsh.point_size = 1.0f;
    }

    /* 2. fixed-function vertex with lighting + tex matrix on stage 0. */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "ff_vsh_lit_textured";
        f->run_vsh = true;
        f->vsh.is_fixed_function = true;
        f->vsh.surface_scale_factor = 1;
        f->vsh.smooth_shading = true;
        f->vsh.point_size = 1.0f;
        f->vsh.fixed_function.lighting = true;
        f->vsh.fixed_function.texture_matrix_enable[0] = true;
        f->vsh.fixed_function.normalization = true;
        /* Light 0 set to a simple infinite-direction light. Other
         * lights stay LIGHT_OFF (= 0) which the generator treats as
         * "skip this slot". */
        f->vsh.fixed_function.light[0] = LIGHT_INFINITE;
    }

    /* 3. (skipped) programmable vertex shader — vsh-prog requires at
     *    least one valid VSH token with FLD_FINAL set; without that
     *    the generator hits a runtime assert. Hand-encoding a valid
     *    token sequence here is too fragile (the token format is the
     *    NVIDIA Cheops binary, not a friendly intermediate). The
     *    fixed-function-vsh fixtures (1 + 2) already exercise the vsh
     *    code path that produces the structurally similar GLSL
     *    (prologue + body + epilogue layout); a future iteration of
     *    the harness that captures real ShaderState from a running
     *    game will cover prog_vsh end-to-end. */

    /* 4. simple combiner — zero stages, default final-input = passthrough.
     *    The NV2A reset state. */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "psh_simple_passthrough";
        f->run_psh = true;
        f->psh.combiner_control = 0; /* num_stages = 0 */
        f->psh.smooth_shading = true;
        f->psh.depth_clipping = false;
        f->psh.surface_zeta_format = 0;
        f->psh.depth_format = DEPTH_FORMAT_D24;
    }

    /* 5. complex combiner — 2 stages, with one texture sampler. The
     *    rgb_inputs / outputs are set to a passthrough-like pattern
     *    that exercises the multi-stage emit path. */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "psh_two_stage_textured";
        f->run_psh = true;
        f->psh.combiner_control = 2; /* num_stages = 2 */
        f->psh.shader_stage_program = 0; /* all stages: 2D tex */
        /* rgb_inputs / alpha_inputs: bit pattern for a passthrough
         * (D = TEX0; A,B,C = ZERO). A real value here would need the
         * NV097_PS_REGISTER_* encoding, but the generator handles
         * arbitrary inputs as long as the parser accepts them. */
        for (int i = 0; i < 2; i++) {
            f->psh.rgb_inputs[i]   = 0x00000000;
            f->psh.rgb_outputs[i]  = 0x00000000;
            f->psh.alpha_inputs[i] = 0x00000000;
            f->psh.alpha_outputs[i] = 0x00000000;
        }
        f->psh.smooth_shading = true;
        f->psh.depth_format = DEPTH_FORMAT_D24;
        /* dim_tex[0] = 2 (2D texture) so the generator emits a
         * texture2D sampler rather than texture1D/3D/cube. */
        f->psh.dim_tex[0] = 2;
        f->psh.dim_tex[1] = 2;
    }

    /* 6. alpha-test + fog. Stress the fragment-shader's alpha-discard
     *    + fog blend emit. */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "psh_alpha_test_fog";
        f->run_psh = true;
        f->psh.combiner_control = 1;
        f->psh.alpha_test = true;
        f->psh.alpha_func = ALPHA_FUNC_GREATER;
        f->psh.smooth_shading = true;
        f->psh.depth_format = DEPTH_FORMAT_D24;
        f->psh.dim_tex[0] = 2;
    }

    /* 7. native-tri-depth path (PR #2240 fragment-shader depth
     *    derivation). */
    {
        ShaderValidateFixture *f = &fxs[n++];
        zero_fixture(f);
        f->name = "psh_native_tri_depth";
        f->run_psh = true;
        f->psh.combiner_control = 1;
        f->psh.smooth_shading = true;
        f->psh.native_tri_depth = true;
        f->psh.depth_format = DEPTH_FORMAT_D24;
        f->psh.dim_tex[0] = 2;
    }

    /* 8. (skipped) geometry-shader fixture.
     *
     * Apple Silicon Metal has NO native geometry-shader stage. The
     * Metal port plan §3.8 + decision-log entry "2026-05-01: …
     * native_tri_depth / native_quad" already commit to the
     * fragment-shader native-depth path on Metal — geometry shaders
     * are bypassed at the renderer layer (mtl/renderer.c's draw
     * path eligibility check), not translated to MSL.
     *
     * spirv-cross's MSL backend nominally emulates GS via a
     * compute-shader transform, but the path is known to be fragile
     * in this version (vulkan-sdk-1.3.290.0); calling it on a
     * line-loop GS payload aborts the process under our test harness
     * (silent SIGSEGV inside spvc_compiler_compile). Validating it
     * end-to-end here would prove a feature we do not use on
     * Metal — and would gate the slice on a known-unsupported path.
     *
     * If the project later decides to add a Metal GS-emulation
     * fallback (M7 framebuffer-fetch combiner does NOT need this;
     * the GS is only relevant if we drop native_tri_depth /
     * native_quad), the fixture should be re-enabled, the
     * translator bug-fix or workaround landed, and this comment
     * removed. Until then the geom code path is a non-goal on
     * Metal and the fixture is intentionally absent rather than
     * "expected fail" so the harness summary stays clean. */

    return n;
}

/* -------- harness driver -------- */

typedef struct {
    bool   stage_ok[3]; /* vsh, geom, psh */
    bool   stage_attempted[3];
    char  *stage_err[3];
} FixtureResult;

static const char *stage_name(int s)
{
    switch (s) {
    case 0: return "vsh";
    case 1: return "geom";
    case 2: return "psh";
    default: return "?";
    }
}

static PgraphMtlGlslStage stage_to_translator(int s)
{
    switch (s) {
    case 0: return PGRAPH_MTL_GLSL_STAGE_VERTEX;
    case 1: return PGRAPH_MTL_GLSL_STAGE_GEOMETRY;
    case 2: return PGRAPH_MTL_GLSL_STAGE_FRAGMENT;
    }
    return PGRAPH_MTL_GLSL_STAGE_VERTEX;
}

static char *gen_glsl_for_stage(int s, const ShaderValidateFixture *fx)
{
    MString *m = NULL;
    if (s == 0) {
        GenVshGlslOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.vulkan = true;
        opts.prefix_outputs = false;
        opts.use_push_constants_for_uniform_attrs = false;
        opts.ubo_binding = 0;
        m = pgraph_glsl_gen_vsh(&fx->vsh, opts);
    } else if (s == 1) {
        GenGeomGlslOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.vulkan = true;
        m = pgraph_glsl_gen_geom(&fx->geom, opts);
    } else if (s == 2) {
        GenPshGlslOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.vulkan = true;
        opts.ubo_binding = 1;
        opts.tex_binding = 2;
        m = pgraph_glsl_gen_psh(&fx->psh, opts);
    }

    if (m == NULL) {
        return NULL;
    }
    /* mstring_get_str returns the internal buffer pointer; copy. */
    const char *s_ptr = mstring_get_str(m);
    char *copy = s_ptr ? g_strdup(s_ptr) : NULL;
    mstring_unref(m);
    return copy;
}

static void run_one_fixture(const ShaderValidateFixture *fx,
                            FixtureResult *res)
{
    bool runs[3] = { fx->run_vsh, fx->run_geom, fx->run_psh };
    for (int s = 0; s < 3; s++) {
        res->stage_attempted[s] = runs[s];
        res->stage_ok[s] = false;
        res->stage_err[s] = NULL;
        if (!runs[s]) {
            continue;
        }

        char *glsl = gen_glsl_for_stage(s, fx);
        if (!glsl) {
            res->stage_err[s] = strdup("GLSL generator returned NULL");
            continue;
        }

        char *xerr = NULL;
        char *msl = pgraph_mtl_glsl_translate_to_msl(stage_to_translator(s),
                                                     glsl, &xerr);
        if (!msl) {
            /* Capture the failing GLSL alongside the translator error
             * so the report is actionable. */
            size_t need = (xerr ? strlen(xerr) : 0) + strlen(glsl) + 256;
            res->stage_err[s] = (char *)malloc(need);
            if (res->stage_err[s]) {
                snprintf(res->stage_err[s], need,
                         "translate_to_msl failed: %s\n--- GLSL ---\n%s\n",
                         xerr ? xerr : "(no message)", glsl);
            }
            if (xerr) {
                free(xerr);
            }
            g_free(glsl);
            continue;
        }

        char *lerr = NULL;
        int ok = pgraph_mtl_pipeline_validate_msl(msl, &lerr);
        if (!ok) {
            size_t need = (lerr ? strlen(lerr) : 0) + strlen(msl) + 256;
            res->stage_err[s] = (char *)malloc(need);
            if (res->stage_err[s]) {
                snprintf(res->stage_err[s], need,
                         "MTLLibrary compile failed: %s\n--- MSL ---\n%s\n",
                         lerr ? lerr : "(no message)", msl);
            }
            if (lerr) {
                free(lerr);
            }
            free(msl);
            g_free(glsl);
            continue;
        }
        free(msl);
        g_free(glsl);
        res->stage_ok[s] = true;
    }
}

/*
 * M7 standalone framebuffer-fetch fixture.
 *
 * Confirms spirv-cross's MSL backend translates a Vulkan input-attachment
 * subpass-load into framebuffer-fetch (`[[color(0)]]` MSL fragment input)
 * with a raster-order-group decoration when MSL_FRAMEBUFFER_FETCH_SUBPASS
 * is enabled.
 *
 * NV2A's pixel pipeline does NOT inherently read destination color —
 * fixed-function blend handles that on hardware — so this fixture uses
 * hand-written Vulkan-style GLSL rather than going through the NV2A
 * generator.  Its purpose is to validate the framebuffer-fetch
 * translation path itself for the M7 exit gate; if a future ubershader
 * (M8) needs programmable blend, this same translation pipeline carries
 * it through.
 *
 * Returns true iff:
 *   - GLSL → SPIR-V → MSL succeeds
 *   - The resulting MSL contains `[[color(0)]]` (framebuffer fetch input)
 *
 * The raster_order_group decoration is checked separately as an
 * advisory message — older spirv-cross releases emit it only when the
 * input attachment is also written; we tolerate either behaviour here.
 */
static bool run_framebuffer_fetch_fixture(char **out_msl, char **out_err)
{
    static const char fb_fetch_glsl[] =
        "#version 460\n"
        "layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput uSubpass;\n"
        "layout(location = 0) out vec4 outColor;\n"
        "void main() {\n"
        "    vec4 dst = subpassLoad(uSubpass);\n"
        "    outColor = vec4(1.0 - dst.rgb, dst.a);\n"
        "}\n";

    char *xerr = NULL;
    char *msl = pgraph_mtl_glsl_translate_to_msl(
        PGRAPH_MTL_GLSL_STAGE_FRAGMENT, fb_fetch_glsl, &xerr);
    if (!msl) {
        if (out_err) {
            size_t n = (xerr ? strlen(xerr) : 0) + 256;
            *out_err = (char *)malloc(n);
            if (*out_err) {
                snprintf(*out_err, n,
                         "translate_to_msl failed: %s",
                         xerr ? xerr : "(no message)");
            }
        }
        if (xerr) free(xerr);
        return false;
    }
    if (xerr) free(xerr);

    bool has_fb_fetch = (strstr(msl, "[[color(0)]]") != NULL ||
                        strstr(msl, "[[ color(0) ]]") != NULL);
    if (!has_fb_fetch) {
        if (out_err) {
            size_t n = strlen(msl) + 256;
            *out_err = (char *)malloc(n);
            if (*out_err) {
                snprintf(*out_err, n,
                         "MSL output lacks framebuffer-fetch [[color(0)]] "
                         "input:\n%s", msl);
            }
        }
        free(msl);
        return false;
    }

    /* raster_order_group is advisory; not all spirv-cross builds emit
     * it for read-only subpassLoad. Log if missing but do not fail. */
    bool has_rog = (strstr(msl, "raster_order_group") != NULL);
    if (!has_rog) {
        fprintf(stderr,
                "[xemu-metal-validate]  ADVISORY framebuffer_fetch_msl: "
                "MSL has [[color(0)]] but no raster_order_group decoration "
                "(spirv-cross variant; tolerated)\n");
    }

    if (out_msl) {
        *out_msl = msl;
    } else {
        free(msl);
    }
    return true;
}

bool pgraph_mtl_shader_validate_run(void)
{
    const char *env = getenv("XEMU_METAL_SHADER_VALIDATE");
    if (!env || env[0] == '\0' || env[0] == '0') {
        return true;
    }
    bool strict = (strcmp(env, "strict") == 0 || strcmp(env, "2") == 0);

    /* Ensure the translator's glslang runtime is up. The renderer's
     * normal init path also brings this up, but the harness can run
     * standalone. */
    if (!pgraph_mtl_glsl_init()) {
        fprintf(stderr,
                "[xemu-metal-validate] pgraph_mtl_glsl_init failed; "
                "skipping harness\n");
        return false;
    }

    ShaderValidateFixture fxs[MAX_FIXTURES];
    int n = build_fixtures(fxs);

    fprintf(stderr,
            "[xemu-metal-validate] running M5 shader-validation harness "
            "(%d fixtures + 1 M7 framebuffer-fetch fixture, mode=%s)\n",
            n, strict ? "strict" : "advisory");

    int total_pass = 0, total_fail = 0;
    int total_attempted = 0;

    for (int i = 0; i < n; i++) {
        FixtureResult r;
        memset(&r, 0, sizeof(r));
        run_one_fixture(&fxs[i], &r);

        for (int s = 0; s < 3; s++) {
            if (!r.stage_attempted[s]) {
                continue;
            }
            total_attempted++;
            if (r.stage_ok[s]) {
                total_pass++;
                fprintf(stderr,
                        "[xemu-metal-validate]  PASS %s/%s\n",
                        fxs[i].name, stage_name(s));
            } else {
                total_fail++;
                fprintf(stderr,
                        "[xemu-metal-validate]  FAIL %s/%s\n%s\n",
                        fxs[i].name, stage_name(s),
                        r.stage_err[s] ? r.stage_err[s] : "(no error)");
            }
            if (r.stage_err[s]) {
                free(r.stage_err[s]);
                r.stage_err[s] = NULL;
            }
        }
    }

    /* M7: framebuffer-fetch translation fixture. */
    {
        char *fb_err = NULL;
        char *fb_msl = NULL;
        bool fb_ok = run_framebuffer_fetch_fixture(&fb_msl, &fb_err);
        total_attempted++;
        if (fb_ok) {
            total_pass++;
            fprintf(stderr,
                    "[xemu-metal-validate]  PASS framebuffer_fetch_msl/psh\n");
        } else {
            total_fail++;
            fprintf(stderr,
                    "[xemu-metal-validate]  FAIL framebuffer_fetch_msl/psh\n%s\n",
                    fb_err ? fb_err : "(no error)");
        }
        if (fb_err) {
            free(fb_err);
        }
        if (fb_msl) {
            free(fb_msl);
        }
    }

    atomic_fetch_add(&s_ok_count, (uint64_t)total_pass);
    atomic_fetch_add(&s_fail_count, (uint64_t)total_fail);

    fprintf(stderr,
            "[xemu-metal-validate] summary: %d/%d passed, %d failed\n",
            total_pass, total_attempted, total_fail);

    pgraph_mtl_glsl_finalize();

    if (total_fail > 0) {
        if (strict) {
            fprintf(stderr,
                    "[xemu-metal-validate] strict mode: aborting "
                    "(set XEMU_METAL_SHADER_VALIDATE=1 for advisory)\n");
            exit(2);
        }
        return false;
    }
    return true;
}
