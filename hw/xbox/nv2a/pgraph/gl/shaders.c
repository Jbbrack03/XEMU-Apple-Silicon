/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"
#include "qemu/timer.h"

#include "xemu-version.h"
#include "ui/xemu-settings.h"
#include "hw/xbox/nv2a/pgraph/util.h"
#include "debug.h"
#include "renderer.h"

static GLenum get_gl_primitive_mode(const GeomState *geom)
{
    enum ShaderPolygonMode polygon_mode = geom->polygon_front_mode;
    enum ShaderPrimitiveMode primitive_mode = geom->primitive_mode;
    bool native_quad =
        geom->native_quad &&
        pgraph_glsl_native_quad_supported(primitive_mode,
                                          geom->polygon_front_mode,
                                          geom->polygon_back_mode,
                                          geom->smooth_shading);

    switch (primitive_mode) {
    case PRIM_TYPE_POINTS: return GL_POINTS;
    case PRIM_TYPE_LINES: return GL_LINES;
    case PRIM_TYPE_LINE_LOOP: return GL_LINE_LOOP;
    case PRIM_TYPE_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TYPE_TRIANGLES: return GL_TRIANGLES;
    case PRIM_TYPE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    case PRIM_TYPE_QUADS:
        return native_quad ? GL_TRIANGLES : GL_LINES_ADJACENCY;
    case PRIM_TYPE_QUAD_STRIP:
        return native_quad ? GL_TRIANGLES : GL_LINE_STRIP_ADJACENCY;
    case PRIM_TYPE_POLYGON:
        if (polygon_mode == POLY_MODE_LINE) {
            return GL_LINE_LOOP;
        } else if (polygon_mode == POLY_MODE_FILL) {
            return GL_TRIANGLE_FAN;
        }

        assert(!"PRIM_TYPE_POLYGON with invalid polygon_mode");
        return 0;
    default:
        assert(!"Invalid primitive_mode");
        return 0;
    }
}

static GLuint create_gl_shader(GLenum gl_shader_type,
                               const char *code,
                               const char *name)
{
    GLint compiled = 0;

    NV2A_GL_DGROUP_BEGIN("Creating new %s", name);

    NV2A_DPRINTF("compile new %s, code:\n%s\n", name, code);

    GLuint shader = glCreateShader(gl_shader_type);
    glShaderSource(shader, 1, &code, 0);
    glCompileShader(shader);

    /* Check it compiled */
    compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar* log;
        GLint log_length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
        log = g_malloc(log_length * sizeof(GLchar));
        glGetShaderInfoLog(shader, log_length, NULL, log);
        fprintf(stderr, "%s\n\n" "nv2a: %s compilation failed: %s\n", code, name, log);
        g_free(log);

        NV2A_GL_DGROUP_END();
        abort();
    }

    NV2A_GL_DGROUP_END();

    return shader;
}

static void set_texture_sampler_uniforms(ShaderBinding *binding)
{
    for (int i = 0; i < NV2A_MAX_TEXTURES; i++) {
        char samplerName[16];
        snprintf(samplerName, sizeof(samplerName), "texSamp%d", i);
        GLint texSampLoc =
            glGetUniformLocation(binding->gl_program, samplerName);
        if (texSampLoc >= 0) {
            glUniform1i(texSampLoc, i);
        }
    }
}

static void update_shader_uniform_locs(ShaderBinding *binding)
{
    char tmp[64];

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.vsh); i++) {
        const char *name = VshUniformInfo[i].name;
        if (VshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.vsh[i] = glGetUniformLocation(binding->gl_program, name);
    }

    for (int i = 0; i < ARRAY_SIZE(binding->uniform_locs.psh); i++) {
        const char *name = PshUniformInfo[i].name;
        if (PshUniformInfo[i].count > 1) {
            snprintf(tmp, sizeof(tmp), "%s[0]", name);
            name = tmp;
        }
        binding->uniform_locs.psh[i] = glGetUniformLocation(binding->gl_program, name);
    }
}

static void shader_module_cache_entry_init(Lru *lru, LruNode *node,
                                           const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    memcpy(&module->key, key, sizeof(ShaderModuleCacheKey));

    const char *kind_str;
    MString *code;

    switch (module->key.kind) {
    case GL_VERTEX_SHADER:
        kind_str = "vertex shader";
        code = pgraph_glsl_gen_vsh(&module->key.vsh.state,
                                   module->key.vsh.glsl_opts);
        break;
    case GL_GEOMETRY_SHADER:
        kind_str = "geometry shader";
        code = pgraph_glsl_gen_geom(&module->key.geom.state,
                                    module->key.geom.glsl_opts);
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_SHADER_MODULE_GEN);
        break;
    case GL_FRAGMENT_SHADER:
        kind_str = "fragment shader";
        code = pgraph_glsl_gen_psh(&module->key.psh.state,
                                   module->key.psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        kind_str = "unknown";
        code = NULL;
    }

    module->gl_shader =
        create_gl_shader(module->key.kind, mstring_get_str(code), kind_str);
    mstring_unref(code);
}

static void shader_module_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    glDeleteShader(module->gl_shader);
}

static bool shader_module_cache_entry_compare(Lru *lru, LruNode *node,
                                              const void *key)
{
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return memcmp(&module->key, key, sizeof(ShaderModuleCacheKey));
}

static GLuint get_shader_module_for_key(PGRAPHGLState *r,
                                        const ShaderModuleCacheKey *key)
{
    uint64_t hash = fast_hash((void *)key, sizeof(ShaderModuleCacheKey));
    LruNode *node = lru_lookup(&r->shader_module_cache, hash, key);
    ShaderModuleCacheEntry *module =
        container_of(node, ShaderModuleCacheEntry, node);
    return module->gl_shader;
}

/* Counter increment that is safe from the async-compile worker thread.
 * The worker may race against the renderer's per-frame counter reset; using
 * atomic add keeps individual counter writes well-defined, and the worst
 * outcome is one event being attributed to an adjacent frame. */
static inline void nv2a_profile_inc_counter_atomic(
    enum NV2A_PROF_COUNTERS_ENUM cnt)
{
    qatomic_inc(&g_nv2a_stats.frame_working.counters[cnt]);
}

static inline void nv2a_profile_add_counter_atomic(
    enum NV2A_PROF_COUNTERS_ENUM cnt, int delta)
{
    qatomic_add(&g_nv2a_stats.frame_working.counters[cnt], delta);
}

/* Compile + link the program described by binding->state. Safe to call from
 * either the renderer or the async-compile worker; the caller must ensure
 * the appropriate GloContext is current on this thread. Module-cache access
 * is locked so the renderer's synchronous path and the worker do not race
 * on shader_module_cache. Counters are incremented atomically so async
 * worker accumulation does not corrupt the renderer's per-frame counter
 * reset.
 *
 * validate_program: glValidateProgram against the currently-bound GL state
 * (a debug-only sanity check). The async-compile worker context has no
 * VAO bound, so validation fails there with "No vertex array object
 * bound." Pass false from the worker; the link itself catches the
 * compile-level errors that matter, and any runtime state errors will
 * surface on the renderer's first glUseProgram + draw. */
static void generate_shaders(PGRAPHGLState *r, ShaderBinding *binding,
                             bool validate_program)
{
    /* Time the entire compile+link path. On Apple's GL-on-Metal driver
     * glLinkProgram synchronously translates GLSL to MSL and compiles, which
     * is the dominant per-shader-bind cost (and the source of Crimson Skies'
     * worst-frame stutter). The counter is microseconds accumulated per
     * frame; interval averages roll up via the existing profile aggregation. */
    int64_t compile_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    GLuint program = glCreateProgram();

    ShaderState *state = &binding->state;
    ShaderModuleCacheKey key;

    bool need_geometry_shader = pgraph_glsl_need_geom(&state->geom);

    qemu_mutex_lock(&r->shader_module_cache_lock);
    if (need_geometry_shader) {
        nv2a_profile_inc_counter_atomic(NV2A_PROF_GEOM_SHADER_PROGRAM_GEN);
        memset(&key, 0, sizeof(key));
        key.kind = GL_GEOMETRY_SHADER;
        key.geom.state = state->geom;
        glAttachShader(program, get_shader_module_for_key(r, &key));
    }

    /* create the vertex shader */
    memset(&key, 0, sizeof(key));
    key.kind = GL_VERTEX_SHADER;
    key.vsh.state = state->vsh;
    key.vsh.glsl_opts.prefix_outputs = need_geometry_shader;
    glAttachShader(program, get_shader_module_for_key(r, &key));

    /* generate a fragment shader from register combiners */
    memset(&key, 0, sizeof(key));
    key.kind = GL_FRAGMENT_SHADER;
    key.psh.state = state->psh;
    glAttachShader(program, get_shader_module_for_key(r, &key));
    qemu_mutex_unlock(&r->shader_module_cache_lock);

    /* link the program */
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if(!linked) {
        GLchar log[2048];
        glGetProgramInfoLog(program, 2048, NULL, log);
        fprintf(stderr, "nv2a: shader linking failed: %s\n", log);
        abort();
    }

    glUseProgram(program);

    binding->gl_program = program;
    binding->gl_primitive_mode = get_gl_primitive_mode(&state->geom);
    binding->has_geometry_shader = need_geometry_shader;

    set_texture_sampler_uniforms(binding);

    /* validate the program (skipped on the async-compile worker because its
     * context has no VAO bound). */
    if (validate_program) {
        GLint valid = 0;
        glValidateProgram(program);
        glGetProgramiv(program, GL_VALIDATE_STATUS, &valid);
        if (!valid) {
            GLchar log[1024];
            glGetProgramInfoLog(program, 1024, NULL, log);
            fprintf(stderr, "nv2a: shader validation failed: %s\n", log);
            abort();
        }
    }

    update_shader_uniform_locs(binding);

    /* Push the worker's GL command stream so the renderer thread can pick
     * up the linked program. glFlush is sufficient on shared-name-space
     * contexts and avoids the p999 regression that glFinish caused by
     * serializing against Apple's GL command queue (1,345 ms worst-frame
     * unchanged but p999 went 104 ms -> 382 ms in the 2026-05-01 paired
     * Crimson runs). The renderer's later glUseProgram on the same
     * GLuint will pick up the worker's writes either way. */
    glFlush();

    /* Publish the binding only after all compile work is observable, so
     * the renderer's check on binding->initialized synchronizes with the
     * worker's writes. */
    smp_wmb();
    qatomic_set(&binding->initialized, true);

    int64_t compile_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    nv2a_profile_inc_counter_atomic(NV2A_PROF_SHADER_COMPILE_COUNT);
    nv2a_profile_add_counter_atomic(NV2A_PROF_SHADER_COMPILE_US_TOTAL,
                                    (int)(compile_end_us - compile_start_us));
}

static const char *shader_gl_vendor = NULL;

static void shader_create_cache_folder(void)
{
    char *shader_path = g_strdup_printf("%sshaders", xemu_settings_get_base_path());
    qemu_mkdir(shader_path);
    g_free(shader_path);
}

static char *shader_get_lru_cache_path(void)
{
    return g_strdup_printf("%s/shader_cache_list", xemu_settings_get_base_path());
}

static void shader_write_lru_list_entry_to_disk(Lru *lru, LruNode *node, void *opaque)
{
    FILE *lru_list_file = (FILE*) opaque;
    size_t written = fwrite(&node->hash, sizeof(uint64_t), 1, lru_list_file);
    if (written != 1) {
        fprintf(stderr, "nv2a: Failed to write shader list entry %llx to disk\n",
                (unsigned long long) node->hash);
    }
}

void pgraph_gl_shader_write_cache_reload_list(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!g_config.perf.cache_shaders) {
        qatomic_set(&r->shader_cache_writeback_pending, false);
        qemu_event_set(&r->shader_cache_writeback_complete);
        return;
    }

    char *shader_lru_path = shader_get_lru_cache_path();
    qemu_thread_join(&r->shader_disk_thread);

    FILE *lru_list = qemu_fopen(shader_lru_path, "wb");
    g_free(shader_lru_path);
    if (!lru_list) {
        fprintf(stderr, "nv2a: Failed to open shader LRU cache for writing\n");
        return;
    }

    lru_visit_active(&r->shader_cache, shader_write_lru_list_entry_to_disk, lru_list);
    fclose(lru_list);

    lru_flush(&r->shader_cache);

    qatomic_set(&r->shader_cache_writeback_pending, false);
    qemu_event_set(&r->shader_cache_writeback_complete);
}

bool pgraph_gl_shader_load_from_memory(ShaderBinding *binding)
{
    assert(glGetError() == GL_NO_ERROR);

    if (!binding->program) {
        return false;
    }

    /* Time the disk-cached binary load. On Apple's GL-on-Metal driver
     * glProgramBinary may still translate the cached binary to a Metal
     * function and compile, so this counter belongs alongside the cold
     * compile path even though the cost is typically lower. */
    int64_t compile_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    GLuint gl_program = glCreateProgram();
    glProgramBinary(gl_program, binding->program_format, binding->program,
                    binding->program_size);
    GLint gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: GL error code %d\n",
            gl_error);
        glDeleteProgram(gl_program);
        return false;
    }

    GLint link_status = GL_FALSE;
    glGetProgramiv(gl_program, GL_LINK_STATUS, &link_status);
    if (!link_status) {
        NV2A_DPRINTF(
            "failed to load shader binary from disk: link status is FALSE\n");
        glDeleteProgram(gl_program);
        return false;
    }

    glUseProgram(gl_program);

    g_free(binding->program);

    binding->program = NULL;
    binding->gl_program = gl_program;
    binding->gl_primitive_mode = get_gl_primitive_mode(&binding->state.geom);
    binding->initialized = true;

    set_texture_sampler_uniforms(binding);

    glValidateProgram(gl_program);
    GLint valid = 0;
    glGetProgramiv(gl_program, GL_VALIDATE_STATUS, &valid);
    if (!valid) {
        GLchar log[1024];
        glGetProgramInfoLog(gl_program, 1024, NULL, log);
        NV2A_DPRINTF("failed to load shader binary from disk: %s\n", log);
        glDeleteProgram(gl_program);
        return false;
    }

    update_shader_uniform_locs(binding);

    int64_t compile_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    nv2a_profile_inc_counter(NV2A_PROF_SHADER_COMPILE_COUNT);
    nv2a_profile_add_counter(NV2A_PROF_SHADER_COMPILE_US_TOTAL,
                             (int)(compile_end_us - compile_start_us));

    return true;
}

static char *shader_get_bin_directory(uint64_t hash)
{
    const char *cfg_dir = xemu_settings_get_base_path();
    char *shader_bin_dir =
        g_strdup_printf("%s/shaders/%04x", cfg_dir, (uint32_t)(hash >> 48));
    return shader_bin_dir;
}

static char *shader_get_binary_path(const char *shader_bin_dir, uint64_t hash)
{
    uint64_t bin_mask = (uint64_t)0xffff << 48;
    return g_strdup_printf("%s/%012" PRIx64, shader_bin_dir, hash & ~bin_mask);
}

static void shader_load_from_disk(PGRAPHState *pg, uint64_t hash)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    char *shader_bin_dir = shader_get_bin_directory(hash);
    char *shader_path = shader_get_binary_path(shader_bin_dir, hash);
    char *cached_xemu_version = NULL;
    char *cached_gl_vendor = NULL;
    void *program_buffer = NULL;

    uint64_t cached_xemu_version_len;
    uint64_t gl_vendor_len;
    GLenum program_binary_format;
    ShaderState state;
    size_t shader_size;

    g_free(shader_bin_dir);

    qemu_mutex_lock(&r->shader_cache_lock);
    if (lru_contains_hash(&r->shader_cache, hash)) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }
    qemu_mutex_unlock(&r->shader_cache_lock);

    FILE *shader_file = qemu_fopen(shader_path, "rb");
    if (!shader_file) {
        goto error;
    }

    size_t nread;
    #define READ_OR_ERR(data, data_len) \
        do { \
            nread = fread(data, data_len, 1, shader_file); \
            if (nread != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    READ_OR_ERR(&cached_xemu_version_len, sizeof(cached_xemu_version_len));

    cached_xemu_version = g_malloc(cached_xemu_version_len +1);
    READ_OR_ERR(cached_xemu_version, cached_xemu_version_len);
    if (strcmp(cached_xemu_version, xemu_version) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));

    cached_gl_vendor = g_malloc(gl_vendor_len);
    READ_OR_ERR(cached_gl_vendor, gl_vendor_len);
    if (strcmp(cached_gl_vendor, shader_gl_vendor) != 0) {
        fclose(shader_file);
        goto error;
    }

    READ_OR_ERR(&program_binary_format, sizeof(program_binary_format));
    READ_OR_ERR(&state, sizeof(state));
    READ_OR_ERR(&shader_size, sizeof(shader_size));

    program_buffer = g_malloc(shader_size);
    READ_OR_ERR(program_buffer, shader_size);

    #undef READ_OR_ERR

    fclose(shader_file);
    g_free(shader_path);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);

    qemu_mutex_lock(&r->shader_cache_lock);
    LruNode *node = lru_lookup(&r->shader_cache, hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    /* If we happened to regenerate this shader already, then we may as well use the new one */
    if (binding->initialized) {
        qemu_mutex_unlock(&r->shader_cache_lock);
        return;
    }

    binding->program_format = program_binary_format;
    binding->program_size = shader_size;
    binding->program = program_buffer;
    binding->cached = true;
    qemu_mutex_unlock(&r->shader_cache_lock);
    return;

error:
    /* Delete the shader so it won't be loaded again */
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(program_buffer);
    g_free(cached_xemu_version);
    g_free(cached_gl_vendor);
}

static void *shader_reload_lru_from_disk(void *arg)
{
    if (!g_config.perf.cache_shaders) {
        return NULL;
    }

    PGRAPHState *pg = (PGRAPHState*) arg;
    char *shader_lru_path = shader_get_lru_cache_path();

    FILE *lru_shaders_list = qemu_fopen(shader_lru_path, "rb");
    g_free(shader_lru_path);
    if (!lru_shaders_list) {
        return NULL;
    }

    uint64_t hash;
    while (fread(&hash, sizeof(uint64_t), 1, lru_shaders_list) == 1) {
        shader_load_from_disk(pg, hash);
    }

    return NULL;
}

static void shader_cache_entry_init(Lru *lru, LruNode *node, const void *state)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    memcpy(&binding->state, state, sizeof(ShaderState));
    binding->initialized = false;
    binding->cached = false;
    binding->program = NULL;
    binding->save_thread = NULL;
    binding->has_geometry_shader = pgraph_glsl_need_geom(&binding->state.geom);
}

static void shader_cache_entry_post_evict(Lru *lru, LruNode *node)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    if (binding->pending_compile) {
        /* Eviction collided with an in-flight async compile. The cache is
         * 50K entries and at most a handful of compiles are in flight at
         * any time, so this should never trigger. If it does, the slice
         * needs additional coordination (wait-for-compile or refuse-evict
         * with a retry) — abort so the bug surfaces immediately rather
         * than racing the worker's generate_shaders. */
        fprintf(stderr,
                "nv2a: shader_cache eviction collided with pending async "
                "compile (binding=%p hash=%llx)\n",
                binding, (unsigned long long)binding->node.hash);
        abort();
    }

    if (binding->save_thread) {
        qemu_thread_join(binding->save_thread);
        g_free(binding->save_thread);
    }

    glDeleteProgram(binding->gl_program);
    if (binding->program) {
        g_free(binding->program);
    }

    binding->cached = false;
    binding->save_thread = NULL;
    binding->program = NULL;
    binding->has_geometry_shader = false;
    memset(&binding->state, 0, sizeof(ShaderState));
}

static bool shader_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    ShaderBinding *binding = container_of(node, ShaderBinding, node);
    return memcmp(&binding->state, key, sizeof(ShaderState));
}

static void *pgraph_gl_shader_compile_worker(void *arg);

void pgraph_gl_init_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    qemu_mutex_init(&r->shader_cache_lock);
    qemu_mutex_init(&r->shader_module_cache_lock);
    qemu_event_init(&r->shader_cache_writeback_complete, false);

    if (!shader_gl_vendor) {
        shader_gl_vendor = (const char *) glGetString(GL_VENDOR);
    }

    shader_create_cache_folder();

    /* FIXME: Make this configurable */
    const size_t shader_cache_size = 50*1024;
    lru_init(&r->shader_cache);
    r->shader_cache_entries = malloc(shader_cache_size * sizeof(ShaderBinding));
    assert(r->shader_cache_entries != NULL);
    for (int i = 0; i < shader_cache_size; i++) {
        lru_add_free(&r->shader_cache, &r->shader_cache_entries[i].node);
    }

    r->shader_cache.init_node = shader_cache_entry_init;
    r->shader_cache.compare_nodes = shader_cache_entry_compare;
    r->shader_cache.post_node_evict = shader_cache_entry_post_evict;

    qemu_thread_create(&r->shader_disk_thread, "pgraph.renderer_state->shader_cache",
                       shader_reload_lru_from_disk, pg, QEMU_THREAD_JOINABLE);

    /* FIXME: Make this configurable */
    const size_t shader_module_cache_size = 50*1024;
    lru_init(&r->shader_module_cache);
    r->shader_module_cache_entries =
        g_malloc_n(shader_module_cache_size, sizeof(ShaderModuleCacheEntry));
    assert(r->shader_module_cache_entries != NULL);
    for (int i = 0; i < shader_module_cache_size; i++) {
        lru_add_free(&r->shader_module_cache, &r->shader_module_cache_entries[i].node);
    }

    r->shader_module_cache.init_node = shader_module_cache_entry_init;
    r->shader_module_cache.compare_nodes = shader_module_cache_entry_compare;
    r->shader_module_cache.post_node_evict = shader_module_cache_entry_post_evict;

    /* Async shader compile worker (XEMU_PGRAPH_ASYNC_SHADER_COMPILE=1).
     * The third shared GloContext is created in early_context_init only
     * when the env var was set, so its presence is the canonical signal
     * that async is enabled here. */
    r->async_shader_compile_enabled = (g_nv2a_context_shader_compile != NULL);
    if (r->async_shader_compile_enabled) {
        qemu_mutex_init(&r->compile_queue_lock);
        qemu_cond_init(&r->compile_queue_cond);
        QSIMPLEQ_INIT(&r->compile_queue);
        r->compile_queue_depth = 0;
        r->compile_thread_stop = false;
        r->compile_thread_pg = pg;
        qemu_thread_create(&r->compile_thread, "pgraph.gl_async_compile",
                           pgraph_gl_shader_compile_worker, pg,
                           QEMU_THREAD_JOINABLE);
        r->compile_thread_started = true;
    }
}

void pgraph_gl_finalize_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Stop the async compile worker first so it cannot touch the cache or
     * GL state during teardown. */
    if (r->compile_thread_started) {
        qemu_mutex_lock(&r->compile_queue_lock);
        r->compile_thread_stop = true;
        qemu_cond_signal(&r->compile_queue_cond);
        qemu_mutex_unlock(&r->compile_queue_lock);
        qemu_thread_join(&r->compile_thread);
        qemu_cond_destroy(&r->compile_queue_cond);
        qemu_mutex_destroy(&r->compile_queue_lock);
        r->compile_thread_started = false;
    }

    // Clear out shader cache
    pgraph_gl_shader_write_cache_reload_list(pg); // FIXME: also flushes, rename for clarity
    free(r->shader_cache_entries);
    r->shader_cache_entries = NULL;

    lru_flush(&r->shader_module_cache);
    g_free(r->shader_module_cache_entries);
    r->shader_module_cache_entries = NULL;

    qemu_mutex_destroy(&r->shader_module_cache_lock);
    qemu_mutex_destroy(&r->shader_cache_lock);
}

/* Async shader compile worker thread.
 *
 * Pulls ShaderBinding* entries off PGRAPHGLState::compile_queue and runs
 * generate_shaders() against the dedicated g_nv2a_context_shader_compile GL
 * context. Programs/shaders are in the SDL shared name space, so the
 * renderer's later glUseProgram on g_nv2a_context_render picks up the work
 * the worker did here. The renderer's bind path checks
 * binding->pending_compile under shader_cache_lock and skips the draw while
 * a compile is outstanding, so this thread holds no shader_cache_lock during
 * the slow generate_shaders call.
 *
 * The worker is the only thread that ever sets binding->initialized=true on
 * the async path; the renderer's synchronous path also sets it but only
 * when pending_compile is false, so the two writers do not race on a
 * single binding.
 */
static void *pgraph_gl_shader_compile_worker(void *arg)
{
    PGRAPHState *pg = (PGRAPHState *)arg;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glo_set_current(g_nv2a_context_shader_compile);

    while (true) {
        ShaderBinding *binding = NULL;

        qemu_mutex_lock(&r->compile_queue_lock);
        while (QSIMPLEQ_EMPTY(&r->compile_queue) && !r->compile_thread_stop) {
            qemu_cond_wait(&r->compile_queue_cond, &r->compile_queue_lock);
        }
        if (r->compile_thread_stop && QSIMPLEQ_EMPTY(&r->compile_queue)) {
            qemu_mutex_unlock(&r->compile_queue_lock);
            break;
        }
        binding = QSIMPLEQ_FIRST(&r->compile_queue);
        QSIMPLEQ_REMOVE_HEAD(&r->compile_queue, compile_queue_link);
        r->compile_queue_depth--;
        qemu_mutex_unlock(&r->compile_queue_lock);

        /* Compile + link without holding shader_cache_lock. The binding
         * pointer is stable because the LRU eviction path aborts on
         * pending_compile (see shader_cache_entry_post_evict). The
         * shader_module_cache is guarded by its own lock inside
         * generate_shaders. validate_program=false because the worker
         * context has no VAO bound. */
        generate_shaders(r, binding, false);

        if (g_config.perf.cache_shaders) {
            pgraph_gl_shader_cache_to_disk(binding);
        }

        /* Publish completion. Order matters: clear pending_compile only
         * AFTER initialized is set inside generate_shaders, so any
         * renderer thread that observes pending_compile==false on a
         * binding will also observe initialized==true. */
        smp_wmb();
        qemu_mutex_lock(&r->shader_cache_lock);
        binding->pending_compile = false;
        qemu_mutex_unlock(&r->shader_cache_lock);

        nv2a_profile_inc_counter_atomic(
            NV2A_PROF_SHADER_COMPILE_ASYNC_COMPLETED);
    }

    glo_set_current(NULL);
    return NULL;
}

static void *shader_write_to_disk(void *arg)
{
    ShaderBinding *binding = (ShaderBinding*) arg;

    char *shader_bin = shader_get_bin_directory(binding->node.hash);
    char *shader_path = shader_get_binary_path(shader_bin, binding->node.hash);

    static uint64_t gl_vendor_len;
    if (gl_vendor_len == 0) {
        gl_vendor_len = (uint64_t) (strlen(shader_gl_vendor) + 1);
    }

    static uint64_t xemu_version_len = 0;
    if (xemu_version_len == 0) {
        xemu_version_len = (uint64_t) (strlen(xemu_version) + 1);
    }

    qemu_mkdir(shader_bin);
    g_free(shader_bin);

    FILE *shader_file = qemu_fopen(shader_path, "wb");
    if (!shader_file) {
        goto error;
    }

    size_t written;
    #define WRITE_OR_ERR(data, data_size) \
        do { \
            written = fwrite(data, data_size, 1, shader_file); \
            if (written != 1) { \
                fclose(shader_file); \
                goto error; \
            } \
        } while (0)

    WRITE_OR_ERR(&xemu_version_len, sizeof(xemu_version_len));
    WRITE_OR_ERR(xemu_version, xemu_version_len);

    WRITE_OR_ERR(&gl_vendor_len, sizeof(gl_vendor_len));
    WRITE_OR_ERR(shader_gl_vendor, gl_vendor_len);

    WRITE_OR_ERR(&binding->program_format, sizeof(binding->program_format));
    WRITE_OR_ERR(&binding->state, sizeof(binding->state));

    WRITE_OR_ERR(&binding->program_size, sizeof(binding->program_size));
    WRITE_OR_ERR(binding->program, binding->program_size);

    #undef WRITE_OR_ERR

    fclose(shader_file);

    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;

    return NULL;

error:
    fprintf(stderr, "nv2a: Failed to write shader binary file to %s\n", shader_path);
    qemu_unlink(shader_path);
    g_free(shader_path);
    g_free(binding->program);
    binding->program = NULL;
    return NULL;
}

void pgraph_gl_shader_cache_to_disk(ShaderBinding *binding)
{
    if (binding->cached) {
        return;
    }

    GLint program_size;
    glGetProgramiv(binding->gl_program, GL_PROGRAM_BINARY_LENGTH, &program_size);

    if (binding->program) {
        g_free(binding->program);
        binding->program = NULL;
    }

    /* program_size might be zero on some systems, if no binary formats are supported */
    if (program_size == 0) {
        return;
    }

    binding->program = g_malloc(program_size);
    GLsizei program_size_copied;
    glGetProgramBinary(binding->gl_program, program_size, &program_size_copied,
                       &binding->program_format, binding->program);
    assert(glGetError() == GL_NO_ERROR);

    binding->program_size = program_size_copied;
    binding->cached = true;

    char name[24];
    snprintf(name, sizeof(name), "scache-%llx", (unsigned long long) binding->node.hash);
    binding->save_thread = g_malloc0(sizeof(QemuThread));
    qemu_thread_create(binding->save_thread, name, shader_write_to_disk, binding, QEMU_THREAD_JOINABLE);
}

static void apply_uniform_updates(const UniformInfo *info, int *locs,
                                  void *values, size_t count)
{
    for (int i = 0; i < count; i++) {
        if (locs[i] == -1) {
            continue;
        }

        void *value = (char*)values + info[i].val_offs;

        switch (info[i].type) {
        case UniformElementType_uint:
            glUniform1uiv(locs[i], info[i].count, value);
            break;
        case UniformElementType_int:
            glUniform1iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec2:
            glUniform2iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_ivec4:
            glUniform4iv(locs[i], info[i].count, value);
            break;
        case UniformElementType_float:
            glUniform1fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec2:
            glUniform2fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec3:
            glUniform3fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_vec4:
            glUniform4fv(locs[i], info[i].count, value);
            break;
        case UniformElementType_mat2:
            glUniformMatrix2fv(locs[i], info[i].count, GL_FALSE, value);
            break;
        default:
            g_assert_not_reached();
        }
    }

    assert(glGetError() == GL_NO_ERROR);
}

// FIXME: Dirty tracking
// FIXME: Consider UBO to align with VK renderer
static void update_shader_uniforms(PGRAPHState *pg, ShaderBinding *binding)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    VshUniformValues vsh_values;
    pgraph_glsl_set_vsh_uniform_values(pg, &binding->state.vsh,
                                  binding->uniform_locs.vsh, &vsh_values);
    apply_uniform_updates(VshUniformInfo, binding->uniform_locs.vsh,
                          &vsh_values, VshUniform__COUNT);

    PshUniformValues psh_values;
    pgraph_glsl_set_psh_uniform_values(pg, binding->uniform_locs.psh, &psh_values);

    for (int i = 0; i < 4; i++) {
        if (r->texture_binding[i] != NULL) {
            float scale = r->texture_binding[i]->scale;
            psh_values.texScale[i] = scale;
        }
    }
    apply_uniform_updates(PshUniformInfo, binding->uniform_locs.psh,
                          &psh_values, PshUniform__COUNT);
}

void pgraph_gl_bind_shaders(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Reset the skip flag at the top of every bind. Async path may set it
     * back to true if the requested shader is not yet compiled. */
    r->shader_skip_draw = false;

    bool binding_changed = false;
    if (r->shader_binding &&
        !pgraph_glsl_check_shader_state_dirty(pg, &r->shader_binding->state)) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND_NOTDIRTY);
        if (r->shader_binding->has_geometry_shader) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_SHADER_BIND_NOTDIRTY);
        }
        goto update_uniforms;
    }

    ShaderBinding *old_binding = r->shader_binding;
    ShaderState state = pgraph_glsl_get_shader_state(pg);

    NV2A_GL_DGROUP_BEGIN("%s (%s)", __func__,
                         state.vsh.is_fixed_function ? "FF" : "PROG");

    qemu_mutex_lock(&r->shader_cache_lock);

    uint64_t shader_state_hash =
        fast_hash((uint8_t *)&state, sizeof(ShaderState));

    LruNode *node = lru_lookup(&r->shader_cache, shader_state_hash, &state);
    ShaderBinding *binding = container_of(node, ShaderBinding, node);

    bool initialized = qatomic_read(&binding->initialized);

    /* Disk-cache fast path: try only when no async compile is already in
     * flight for this binding. With async enabled, the worker may already
     * own this binding and be mid-compile via generate_shaders. */
    if (!initialized && !binding->pending_compile) {
        if (pgraph_gl_shader_load_from_memory(binding)) {
            initialized = true;
        }
    }

    if (!initialized && r->async_shader_compile_enabled) {
        if (!binding->pending_compile) {
            binding->pending_compile = true;
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
            nv2a_profile_inc_counter(NV2A_PROF_SHADER_COMPILE_ASYNC_QUEUED);

            qemu_mutex_lock(&r->compile_queue_lock);
            QSIMPLEQ_INSERT_TAIL(&r->compile_queue, binding,
                                 compile_queue_link);
            r->compile_queue_depth++;
            qemu_cond_signal(&r->compile_queue_cond);
            qemu_mutex_unlock(&r->compile_queue_lock);
        }

        /* Skip the draw while compile is in flight. r->shader_binding is
         * left at its previous value (or NULL) so downstream draw paths
         * never act on a half-initialized binding. The renderer's flow
         * still terminates the GL debug group and releases the cache
         * lock. */
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_DRAWS_SKIPPED_PENDING);
        r->shader_skip_draw = true;
        qemu_mutex_unlock(&r->shader_cache_lock);
        NV2A_GL_DGROUP_END();
        return;
    }

    if (!initialized) {
        /* Synchronous fallback (default path when async is off, or async
         * is on but the binding still needs an initial compile that
         * cannot be deferred for some future reason). */
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_GEN);
        generate_shaders(r, binding, true);
        if (g_config.perf.cache_shaders) {
            pgraph_gl_shader_cache_to_disk(binding);
        }
    }

    assert(qatomic_read(&binding->initialized));
    r->shader_binding = binding;
    pg->program_data_dirty = false;
    pgraph_gl_trace_native_tri_depth_state(pg, binding, "bind", "shader");

    qemu_mutex_unlock(&r->shader_cache_lock);

    binding_changed = (r->shader_binding != old_binding);
    if (binding_changed) {
        nv2a_profile_inc_counter(NV2A_PROF_SHADER_BIND);
        if (r->shader_binding->has_geometry_shader) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_SHADER_BIND);
        }
        glUseProgram(r->shader_binding->gl_program);
    }

    NV2A_GL_DGROUP_END();

update_uniforms:
    assert(r->shader_binding);
    assert(qatomic_read(&r->shader_binding->initialized));
    update_shader_uniforms(pg, r->shader_binding);
}

GLuint pgraph_gl_compile_shader(const char *vs_src, const char *fs_src)
{
    GLint status;
    char err_buf[512];

    // Compile vertex shader
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(vs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Vertex shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Compile fragment shader
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        glGetShaderInfoLog(fs, sizeof(err_buf), NULL, err_buf);
        err_buf[sizeof(err_buf)-1] = '\0';
        fprintf(stderr, "Fragment shader compilation failed: %s\n", err_buf);
        exit(1);
    }

    // Link vertex and fragment shaders
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);

    // Flag shaders for deletion (will still be retained for lifetime of prog)
    glDeleteShader(vs);
    glDeleteShader(fs);

    return prog;
}
