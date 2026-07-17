/*
 * xr_shell — OpenXR shell spike for xemu on Quest 3 (Spike A).
 *
 * Goal of this spike (docs/openxr-shell-design.md):
 *   1. Stand up an immersive OpenXR session from a NativeActivity inside
 *      the existing app.
 *   2. Passthrough behind (XR_FB_passthrough) + one quad composition layer
 *      populated from the emulator's exported AHardwareBuffer.
 *   3. Log whether Android gamepad KeyEvents reach the activity while
 *      immersive (the decisive input question for the full shell).
 *   4. Exercise XR_FB_display_refresh_rate + XR_EXT_performance_settings.
 *
 * Deliberately single-file and dependency-light. Until the emulator produces
 * its first frame, the quad is transparent so passthrough remains visible.
 */

#include <android/log.h>
#include <android/native_activity.h>
#include <android/performance_hint.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/bitmap.h>
#include <android/keycodes.h>
#include <dlfcn.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "xemu-xrshell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define OXR(fn)                                                        \
    do {                                                               \
        XrResult _r = (fn);                                            \
        if (XR_FAILED(_r)) {                                           \
            LOGE("OpenXR error %d at %s:%d (%s)", (int)_r, __FILE__,   \
                 __LINE__, #fn);                                       \
        }                                                              \
    } while (0)

typedef APerformanceHintManager *(*PFN_adpf_get_manager)(void);
typedef APerformanceHintSession *(*PFN_adpf_create_session)(
    APerformanceHintManager *, const int32_t *, size_t, int64_t);
typedef int64_t (*PFN_adpf_get_update_rate)(APerformanceHintManager *);
typedef int (*PFN_adpf_report)(APerformanceHintSession *, int64_t);
typedef void (*PFN_adpf_close)(APerformanceHintSession *);

static PFN_adpf_get_manager p_adpf_get_manager;
static PFN_adpf_create_session p_adpf_create_session;
static PFN_adpf_get_update_rate p_adpf_get_update_rate;
static PFN_adpf_report p_adpf_report;
static PFN_adpf_close p_adpf_close;

typedef struct {
    struct android_app *app;
    bool resumed;
    bool session_running;

    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local_space;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_pbuffer;

    /* Quad layer swapchain */
    XrSwapchain swapchain;
    uint32_t swapchain_len;
    XrSwapchainImageOpenGLESKHR *images;
    GLuint *fbos;
    int quad_w, quad_h;

    /* Passthrough */
    XrPassthroughFB passthrough;
    XrPassthroughLayerFB passthrough_layer;
    bool have_passthrough;

    /* Refresh rate */
    PFN_xrRequestDisplayRefreshRateFB request_refresh;
    PFN_xrGetDisplayRefreshRateFB get_refresh;

    uint64_t frame_no;

    /* Emulator frame feed (Spike B). Resolved from libxemu.so at runtime;
     * NULL until the emulator process side is up. */
    struct AHardwareBuffer *(*acquire_ahb)(uint64_t *seq);
    float (*get_display_aspect)(void); /* guest GPIO decision: 4:3 or 16:9 */

    /* Gamepad forwarding: SDL can't see a paired pad while the XR NativeActivity
     * has focus, so we translate Android gamepad events and push them to the
     * emulator via this resolved setter. pad_* accumulate current state. */
    void (*set_gamepad)(uint16_t buttons, const int16_t *axis, int naxis);
    uint16_t pad_buttons;
    int16_t pad_axis[6];  /* LTRIG,RTRIG,LSTICK_X,LSTICK_Y,RSTICK_X,RSTICK_Y */
    uint64_t last_seq;
    /* Per-AHB EGLImage/texture cache (AHBs are a small stable ring). */
    struct {
        struct AHardwareBuffer *ahb;
        EGLImageKHR image;
        GLuint tex;
    } ahb_cache[8];
    GLuint blit_prog;
    GLint blit_tex_uniform;
    GLuint blit_vao;
    bool bridge_fallback_logged;
    bool bridge_ready_logged;

    /* 6DOF window state (task #9). Quad pose in local_space + world size. */
    XrVector3f quad_pos;
    XrQuaternionf quad_orient;
    float quad_size_m;      /* width in meters; height derives from aspect */
    float quad_aspect;      /* w/h */

    /* Controller input */
    XrActionSet action_set;
    XrAction grab_action;   /* squeeze/grip: move the window */
    XrAction resize_action; /* trigger: resize while held */
    XrAction stick_action;  /* thumbstick: push/pull + scale */
    XrAction aim_pose_action;
    XrSpace aim_space[2];   /* 0=left, 1=right */
    XrPath hand_path[2];
    bool input_ready;
    bool action_sets_attached;

    /* Grab drag state */
    int grab_hand;          /* -1 none, else 0/1 */
    XrVector3f grab_offset;  /* window pos relative to controller at grab */
    bool resizing;

    /* Emulator control bridges (resolved from libxemu.so alongside the feed);
     * NULL until the emulator side is up. */
    void (*request_load_disc)(const char *path);
    void (*request_quit)(void);   /* eject + reset -> Xbox dashboard */
    bool (*get_fp_jit)(void);     /* running process's active FP JIT mode */
    int (*get_vcpu_tid)(void);
    PFN_xrSetAndroidApplicationThreadKHR set_android_thread;
    bool xr_renderer_thread_set;
    int xr_vcpu_tid_set;

    /* Android Dynamic Performance Framework: separate sessions because the XR
     * renderer and guest vCPU are independent periodic workloads. All calls
     * are made from this thread (APerformanceHintSession is not thread-safe). */
    APerformanceHintManager *adpf_manager;
    APerformanceHintSession *adpf_render;
    APerformanceHintSession *adpf_vcpu;
    int adpf_vcpu_tid;
    int affinity_vcpu_tid;
    int64_t adpf_update_rate_ns;
    int64_t adpf_render_report_ns;
    int64_t adpf_vcpu_report_ns;
    int64_t adpf_last_frame_seq_ns;
    bool adpf_tried;

    /* In-VR shell UI (JNI-backed, rendered to its own quad layer). */
    bool menu_open;
    bool menu_disabled;     /* set after a JNI failure; picker off for session */
    XrSwapchain menu_swapchain;
    uint32_t menu_swapchain_len;
    XrSwapchainImageOpenGLESKHR *menu_images;
    GLuint *menu_fbos;
    int menu_w, menu_h;
    GLuint menu_tex;        /* holds the latest uploaded menu bitmap */
    int menu_tex_w, menu_tex_h; /* allocated storage; enables SubImage path */
    bool menu_tex_ready;    /* menu_tex has valid contents to blit */
    GLuint menu_blit_prog;  /* alpha-preserving blit (vs opaque blit_prog) */
    XrVector3f menu_pos;
    XrQuaternionf menu_orient;
    float menu_size_m;

    /* View-anchored placement + laser pointer for the shell UI. */
    XrSpace view_space;
    XrTime last_predicted_time;
    XrAction menu_click_action; /* left controller menu button */
    bool menu_btn_prev;
    int pointer_hand;           /* hand whose ray hit the panel, -1 = none */
    bool pointer_inside;
    float pointer_x, pointer_y; /* panel pixels */
    int pointer_sent_x, pointer_sent_y;
    bool pointer_sent_press;
    bool pointer_pressed;       /* trigger click with hysteresis */
    GLuint cursor_prog;
    GLint cursor_center_loc, cursor_radius_loc;
    int64_t guest_ms_push_ns;   /* last setGuestFrameMs push */
    float (*get_game_frame_ms)(void); /* libxemu pace readout, may be NULL */

    /* JNI bridge to XrMenuBridge (Kotlin owns the model + Canvas rendering). */
    JavaVM *jvm;
    JNIEnv *jni_env;        /* android_main thread, attached once */
    jobject menu_bridge;    /* global ref, NULL if JNI init failed */
    jmethodID m_refresh, m_count, m_isDirty, m_render, m_move, m_toggleFp,
        m_activate, m_setActiveFp, m_selectByName, m_startEmulator,
        m_pointer, m_pointerExit, m_scroll, m_navPage, m_command, m_button,
        m_setGuestMs;
    bool jni_tried;
    bool emulator_bootstrap_started;

    /* Menu input edge state */
    int nav_latch;          /* -1/0/1: debounces vertical stick/hat/dpad nav */
    int nav_latch_x;        /* -1/0/1: debounces horizontal nav */
    uint16_t pad_deferred;  /* START/BACK held but not yet forwarded (combo) */
    uint16_t synth_buttons; /* synthetic START/BACK asserted to the guest */
    int64_t synth_retract_ns; /* monotonic deadline to drop synth_buttons */
} XrShell;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool adpf_enabled(void)
{
    const char *env = getenv("XEMU_ADPF");
    return !env || !env[0] || strcmp(env, "0") != 0;
}

static void adpf_init_render(XrShell *s)
{
    if (s->adpf_tried) return;
    s->adpf_tried = true;
    if (!adpf_enabled()) {
        LOGI("ADPF: disabled by XEMU_ADPF=0");
        return;
    }
    void *libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (libandroid) {
        p_adpf_get_manager = (PFN_adpf_get_manager)dlsym(
            libandroid, "APerformanceHint_getManager");
        p_adpf_create_session = (PFN_adpf_create_session)dlsym(
            libandroid, "APerformanceHint_createSession");
        p_adpf_get_update_rate = (PFN_adpf_get_update_rate)dlsym(
            libandroid, "APerformanceHint_getPreferredUpdateRateNanos");
        p_adpf_report = (PFN_adpf_report)dlsym(
            libandroid, "APerformanceHint_reportActualWorkDuration");
        p_adpf_close = (PFN_adpf_close)dlsym(
            libandroid, "APerformanceHint_closeSession");
    }
    if (!p_adpf_get_manager || !p_adpf_create_session ||
        !p_adpf_get_update_rate || !p_adpf_report || !p_adpf_close) {
        LOGI("ADPF: API unavailable on this Android version");
        return;
    }
    s->adpf_manager = p_adpf_get_manager();
    if (!s->adpf_manager) {
        LOGI("ADPF: manager unavailable");
        return;
    }
    int32_t tid = (int32_t)syscall(SYS_gettid);
    s->adpf_render = p_adpf_create_session(
        s->adpf_manager, &tid, 1, 8333333LL);
    s->adpf_update_rate_ns =
        p_adpf_get_update_rate(s->adpf_manager);
    if (s->adpf_update_rate_ns <= 0) s->adpf_update_rate_ns = 8333333LL;
    LOGI("ADPF: render tid=%d session=%s target=8.33ms update=%.2fms",
         tid, s->adpf_render ? "ok" : "failed",
         (double)s->adpf_update_rate_ns / 1000000.0);
}

static void adpf_init_vcpu(XrShell *s, int tid)
{
    if (!s->adpf_manager || s->adpf_vcpu || tid <= 0 ||
        tid == s->adpf_vcpu_tid) return;
    int32_t thread = (int32_t)tid;
    s->adpf_vcpu = p_adpf_create_session(
        s->adpf_manager, &thread, 1, 16666667LL);
    s->adpf_vcpu_tid = tid;
    LOGI("ADPF: vCPU tid=%d session=%s target=16.67ms", tid,
         s->adpf_vcpu ? "ok" : "failed");
}

static void adpf_report(APerformanceHintSession *session, int64_t actual_ns,
                        int64_t now, int64_t update_rate, int64_t *last_report)
{
    if (!session || actual_ns <= 0 || now - *last_report < update_rate) return;
    int rc = p_adpf_report(session, actual_ns);
    if (rc == 0) *last_report = now;
}

/* Forward the current pad to the emulator: physical buttons minus any deferred
 * (undecided combo) bits, OR'd with synthetic bits held long enough to be
 * sampled by the guest. */
static void xr_forward_pad(XrShell *s)
{
    if (s->set_gamepad) {
        uint16_t b = (uint16_t)((s->pad_buttons & ~s->pad_deferred) |
                                s->synth_buttons);
        s->set_gamepad(b, s->pad_axis, 6);
    }
}

/* --- minimal vec/quat math --- */
static XrVector3f v3_sub(XrVector3f a, XrVector3f b)
{
    return (XrVector3f){ a.x - b.x, a.y - b.y, a.z - b.z };
}
static XrVector3f v3_add(XrVector3f a, XrVector3f b)
{
    return (XrVector3f){ a.x + b.x, a.y + b.y, a.z + b.z };
}
static XrVector3f q_rotate(XrQuaternionf q, XrVector3f v)
{
    /* v' = v + 2*cross(q.xyz, cross(q.xyz,v) + q.w*v) */
    XrVector3f u = { q.x, q.y, q.z };
    XrVector3f t = { u.y * v.z - u.z * v.y + q.w * v.x,
                     u.z * v.x - u.x * v.z + q.w * v.y,
                     u.x * v.y - u.y * v.x + q.w * v.z };
    XrVector3f c = { u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z,
                     u.x * t.y - u.y * t.x };
    return (XrVector3f){ v.x + 2.0f * c.x, v.y + 2.0f * c.y, v.z + 2.0f * c.z };
}

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;

static const char *BLIT_VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "  uv = vec2(p.x, 1.0 - p.y);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char *BLIT_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "void main() { frag = vec4(texture(tex, uv).rgb, 1.0); }\n";
/* Menu blit keeps the source alpha so transparent panel edges reveal the
 * passthrough / emulator layers behind the quad. */
static const char *MENU_BLIT_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "void main() { frag = texture(tex, uv); }\n";
/* Laser-pointer cursor: white core with a dark rim, anti-aliased, drawn over
 * the menu texture (scissored to the cursor's bounding box). */
static const char *CURSOR_FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform vec2 center;\n"
    "uniform float radius;\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "void main() {\n"
    "  float d = distance(gl_FragCoord.xy, center);\n"
    "  float aa = 1.5;\n"
    "  float disc = 1.0 - smoothstep(radius - aa, radius + aa, d);\n"
    "  float core = 1.0 - smoothstep(radius * 0.62 - aa, radius * 0.62 + aa, d);\n"
    "  vec3 rgb = mix(vec3(0.04, 0.05, 0.07), vec3(1.0), core);\n"
    "  frag = vec4(rgb * disc, disc);\n"
    "}\n";

static GLuint compile_prog(const char *vs_src, const char *fs_src)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        LOGE("blit vertex shader compile failed: %s", log);
    }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        LOGE("blit fragment shader compile failed: %s", log);
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        LOGE("blit prog link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static GLuint ahb_to_texture(XrShell *s, struct AHardwareBuffer *ahb)
{
    for (int i = 0; i < 8; i++) {
        if (s->ahb_cache[i].ahb == ahb) {
            return s->ahb_cache[i].tex;
        }
    }
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (!s->ahb_cache[i].ahb) {
            slot = i;
            break;
        }
    }
    if (slot < 0) { /* ring rolled (resize); reset cache */
        for (int i = 0; i < 8; i++) {
            glDeleteTextures(1, &s->ahb_cache[i].tex);
            if (s->ahb_cache[i].image != EGL_NO_IMAGE_KHR) {
                p_eglDestroyImageKHR(s->egl_display, s->ahb_cache[i].image);
            }
            if (s->ahb_cache[i].ahb) {
                AHardwareBuffer_release(s->ahb_cache[i].ahb);
            }
            memset(&s->ahb_cache[i], 0, sizeof(s->ahb_cache[i]));
        }
        slot = 0;
    }
    if (!p_eglGetNativeClientBufferANDROID || !p_eglCreateImageKHR ||
        !p_eglDestroyImageKHR || !p_glEGLImageTargetTexture2DOES) {
        LOGE("frame bridge: required EGLImage entry point unavailable");
        return 0;
    }
    EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(ahb);
    if (!cb) {
        LOGE("frame bridge: eglGetNativeClientBufferANDROID returned NULL for %p",
             ahb);
        return 0;
    }
    static const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                                    EGL_NONE };
    EGLImageKHR img = p_eglCreateImageKHR(s->egl_display, EGL_NO_CONTEXT,
                                          EGL_NATIVE_BUFFER_ANDROID, cb,
                                          attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR failed for AHB %p (0x%x)", ahb,
             eglGetError());
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    if (!tex) {
        LOGE("frame bridge: glGenTextures returned 0 (err=0x%x)",
             glGetError());
        p_eglDestroyImageKHR(s->egl_display, img);
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        LOGE("frame bridge: glEGLImageTargetTexture2DOES failed for %p (0x%x)",
             ahb, gl_error);
        glDeleteTextures(1, &tex);
        p_eglDestroyImageKHR(s->egl_display, img);
        glBindTexture(GL_TEXTURE_2D, 0);
        return 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    /* An EGLImage does not substitute for an explicit native-buffer owner.
     * Keep one cache reference until this imported texture is evicted. */
    AHardwareBuffer_acquire(ahb);
    s->ahb_cache[slot].ahb = ahb;
    s->ahb_cache[slot].image = img;
    s->ahb_cache[slot].tex = tex;
    AHardwareBuffer_Desc desc;
    AHardwareBuffer_describe(ahb, &desc);
    LOGI("imported emulator AHB %p as tex %u (%ux%u)", ahb, tex,
         desc.width, desc.height);
    return tex;
}

/* Try to resolve the emulator frame feed; libxemu.so may not be loaded yet. */
static void resolve_emulator_feed(XrShell *s)
{
    if (s->acquire_ahb && s->get_display_aspect && s->set_gamepad &&
        s->request_load_disc && s->get_vcpu_tid) {
        return;
    }
    void *h = dlopen("libxemu.so", RTLD_NOLOAD | RTLD_LAZY);
    if (!h) {
        return;
    }
    if (!s->acquire_ahb) {
        s->acquire_ahb = (struct AHardwareBuffer *(*)(uint64_t *))
            dlsym(h, "xemu_xr_acquire_display_ahb");
        if (s->acquire_ahb) {
            LOGI("emulator frame feed resolved");
        }
    }
    if (!s->get_display_aspect) {
        s->get_display_aspect = (float (*)(void))
            dlsym(h, "xemu_xr_get_display_aspect");
        if (s->get_display_aspect) {
            LOGI("emulator native-aspect bridge resolved");
        }
    }
    if (!s->set_gamepad) {
        s->set_gamepad = (void (*)(uint16_t, const int16_t *, int))
            dlsym(h, "xemu_xr_set_gamepad_state");
        if (s->set_gamepad) {
            LOGI("emulator gamepad forwarding resolved");
        }
    }
    if (!s->request_load_disc) {
        s->request_load_disc =
            (void (*)(const char *))dlsym(h, "xemu_xr_request_load_disc");
        if (s->request_load_disc) {
            LOGI("emulator disc-swap bridge resolved");
        }
    }
    if (!s->request_quit) {
        s->request_quit =
            (void (*)(void))dlsym(h, "xemu_xr_request_quit_to_dashboard");
    }
    if (!s->get_fp_jit) {
        s->get_fp_jit = (bool (*)(void))dlsym(h, "xemu_get_fp_jit");
    }
    if (!s->get_vcpu_tid) {
        s->get_vcpu_tid = (int (*)(void))
            dlsym(h, "xemu_get_vcpu_thread_id");
    }
    if (!s->get_game_frame_ms) {
        s->get_game_frame_ms = (float (*)(void))
            dlsym(h, "xemu_xr_get_game_frame_ms");
    }
}

/* Keep the physical OpenXR quad in the same aspect that xui's desktop
 * presenter derives from the Xbox PM GPIO.  The texture itself remains the
 * native framebuffer; only the composition-layer geometry changes. */
static void xr_update_emulator_aspect(XrShell *s)
{
    if (!s->get_display_aspect) {
        return;
    }

    float aspect = s->get_display_aspect();
    if (aspect < 1.2f || aspect > 2.0f || aspect == s->quad_aspect) {
        return;
    }

    s->quad_aspect = aspect;
    LOGI("emulator display aspect %.3f", (double)aspect);
}

static void xr_apply_thread_settings(XrShell *s)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("XEMU_XR_THREAD_SETTINGS");
        enabled = (!env || !env[0] || strcmp(env, "0") != 0) ? 1 : 0;
        LOGI("thread settings: %s", enabled ? "enabled" : "disabled");
    }
    if (s->get_vcpu_tid) {
        int tid = s->get_vcpu_tid();
        adpf_init_vcpu(s, tid);
        const char *pin_env = getenv("XEMU_VCPU_FAST_CORES");
        if (tid > 0 && tid != s->affinity_vcpu_tid && pin_env && pin_env[0] &&
            strcmp(pin_env, "0") != 0) {
            unsigned long mask = (1UL << 4) | (1UL << 5);
            int rc = (int)syscall(SYS_sched_setaffinity, tid, sizeof(mask), &mask);
            LOGI("vCPU affinity: tid=%d cpus=4-5 rc=%d", tid, rc);
            if (rc == 0) s->affinity_vcpu_tid = tid;
        }
    }
    if (!enabled) {
        return;
    }

    if (!s->set_android_thread) {
        if (XR_FAILED(xrGetInstanceProcAddr(
                s->instance, "xrSetAndroidApplicationThreadKHR",
                (PFN_xrVoidFunction *)&s->set_android_thread)) ||
            !s->set_android_thread) {
            return;
        }
    }

    if (!s->xr_renderer_thread_set) {
        uint32_t tid = (uint32_t)syscall(SYS_gettid);
        XrResult r = s->set_android_thread(
            s->session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, tid);
        LOGI("thread settings: XR renderer tid=%u rc=%d", tid, (int)r);
        s->xr_renderer_thread_set = XR_SUCCEEDED(r);
    }

    if (s->get_vcpu_tid) {
        int tid = s->get_vcpu_tid();
        if (tid > 0 && tid != s->xr_vcpu_tid_set) {
            XrResult r = s->set_android_thread(
                s->session, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,
                (uint32_t)tid);
            LOGI("thread settings: vCPU application-main tid=%d rc=%d",
                 tid, (int)r);
            if (XR_SUCCEEDED(r)) {
                s->xr_vcpu_tid_set = tid;
            }
        }
    }
}

/* Lazily attach the android_main thread to the JVM and construct the Kotlin
 * XrMenuBridge (which owns the game list + Canvas rendering). App classes are
 * not visible via FindClass on this native thread, so we load the class through
 * the NativeActivity's own ClassLoader. Runs once; the menu is simply disabled
 * if any step fails. */
static void menu_jni_init(XrShell *s)
{
    if (s->jni_tried) {
        return;
    }
    s->jni_tried = true;

    s->jvm = s->app->activity->vm;
    JNIEnv *env = NULL;
    if ((*s->jvm)->AttachCurrentThread(s->jvm, &env, NULL) != JNI_OK || !env) {
        LOGE("menu: AttachCurrentThread failed");
        s->menu_disabled = true;
        return;
    }
    s->jni_env = env;

    /* All the locals below (classes, loader, ctor result) are freed in one shot
     * by PopLocalFrame; the bridge survives as a global ref. */
    if ((*env)->PushLocalFrame(env, 16) != 0) {
        (*env)->ExceptionClear(env);
        LOGE("menu: PushLocalFrame failed");
        s->menu_disabled = true;
        return;
    }

    jobject activity = s->app->activity->clazz;
    jclass acls = (*env)->GetObjectClass(env, activity);
    jmethodID get_loader = (*env)->GetMethodID(
        env, acls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = get_loader ?
        (*env)->CallObjectMethod(env, activity, get_loader) : NULL;
    jclass lcls = (*env)->FindClass(env, "java/lang/ClassLoader");
    jmethodID load_class = lcls ? (*env)->GetMethodID(
        env, lcls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : NULL;
    jstring cname = (*env)->NewStringUTF(env, "com.izzy2lost.x1box.XrMenuBridge");
    jclass bcls = (loader && load_class && cname) ?
        (jclass)(*env)->CallObjectMethod(env, loader, load_class, cname) : NULL;
    if ((*env)->ExceptionCheck(env) || !bcls) {
        (*env)->ExceptionClear(env);
        LOGE("menu: XrMenuBridge class not found; picker disabled");
        goto fail;
    }
    jmethodID ctor = (*env)->GetMethodID(env, bcls, "<init>",
                                         "(Landroid/content/Context;)V");
    jobject bridge = ctor ? (*env)->NewObject(env, bcls, ctor, activity) : NULL;
    if ((*env)->ExceptionCheck(env) || !bridge) {
        (*env)->ExceptionClear(env);
        LOGE("menu: XrMenuBridge construction failed; picker disabled");
        goto fail;
    }
    s->m_refresh = (*env)->GetMethodID(env, bcls, "refresh", "()V");
    s->m_count = (*env)->GetMethodID(env, bcls, "count", "()I");
    s->m_isDirty = (*env)->GetMethodID(env, bcls, "isDirty", "()Z");
    s->m_render = (*env)->GetMethodID(env, bcls, "render",
                                      "(II)Landroid/graphics/Bitmap;");
    s->m_move = (*env)->GetMethodID(env, bcls, "moveSelection", "(II)V");
    s->m_toggleFp = (*env)->GetMethodID(env, bcls, "toggleFpJit", "()V");
    s->m_activate = (*env)->GetMethodID(env, bcls, "activate",
                                        "()Ljava/lang/String;");
    s->m_setActiveFp =
        (*env)->GetMethodID(env, bcls, "setActiveFpJit", "(ZZ)V");
    s->m_selectByName = (*env)->GetMethodID(env, bcls, "selectByName",
                                            "(Ljava/lang/String;)Z");
    s->m_startEmulator = (*env)->GetMethodID(env, bcls, "startEmulator", "()V");
    s->m_pointer = (*env)->GetMethodID(env, bcls, "pointer", "(FFZ)Z");
    s->m_pointerExit = (*env)->GetMethodID(env, bcls, "pointerExit", "()V");
    s->m_scroll = (*env)->GetMethodID(env, bcls, "scroll", "(F)V");
    s->m_navPage = (*env)->GetMethodID(env, bcls, "navPage", "(I)V");
    s->m_command = (*env)->GetMethodID(env, bcls, "consumeCommand", "()I");
    s->m_button = (*env)->GetMethodID(env, bcls, "gamepadButton", "(I)V");
    s->m_setGuestMs =
        (*env)->GetMethodID(env, bcls, "setGuestFrameMs", "(F)V");
    /* A missing method ID leaves a pending exception AND would abort ART on the
     * next Call*; validate all before publishing the bridge. */
    if ((*env)->ExceptionCheck(env) || !s->m_refresh || !s->m_count ||
        !s->m_isDirty || !s->m_render || !s->m_move || !s->m_toggleFp ||
        !s->m_activate || !s->m_setActiveFp || !s->m_selectByName ||
        !s->m_startEmulator || !s->m_pointer || !s->m_pointerExit ||
        !s->m_scroll || !s->m_navPage || !s->m_command || !s->m_button ||
        !s->m_setGuestMs) {
        (*env)->ExceptionClear(env);
        LOGE("menu: method resolution failed; picker disabled");
        goto fail;
    }
    s->menu_bridge = (*env)->NewGlobalRef(env, bridge);
    (*env)->PopLocalFrame(env, NULL);
    LOGI("menu: JNI bridge ready");
    return;

fail:
    s->menu_disabled = true;
    (*env)->PopLocalFrame(env, NULL);
}

static bool menu_jni_check(XrShell *s, const char *where);

static void start_emulator_once(XrShell *s)
{
    if (s->emulator_bootstrap_started) {
        return;
    }
    s->emulator_bootstrap_started = true;
    menu_jni_init(s);
    if (!s->menu_bridge) {
        LOGE("bootstrap: JNI bridge unavailable; emulator not started");
        return;
    }
    LOGI("bootstrap: immersive NativeActivity owns process; starting SDL/xemu");
    (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge,
                                  s->m_startEmulator);
    menu_jni_check(s, "startEmulator");
}

/* After any JNI call that can throw: if an exception is pending, log it, clear
 * it, and permanently disable the picker for this session (never let it reach
 * the process and abort). Returns true if the menu was just disabled. */
static bool menu_jni_check(XrShell *s, const char *where)
{
    JNIEnv *env = s->jni_env;
    if (!env || !(*env)->ExceptionCheck(env)) {
        return false;
    }
    (*env)->ExceptionDescribe(env); /* dumps stack to logcat, also clears it */
    (*env)->ExceptionClear(env);
    LOGE("menu: JNI exception at %s; disabling picker for session", where);
    if (s->menu_bridge) {
        (*env)->DeleteGlobalRef(env, s->menu_bridge);
        s->menu_bridge = NULL;
    }
    s->menu_open = false;
    s->menu_disabled = true;
    return true;
}

/* Upload an ARGB_8888 menu Bitmap into menu_tex (RGBA). */
static void menu_upload_texture(XrShell *s, jobject bitmap)
{
    JNIEnv *env = s->jni_env;
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) !=
        ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("menu: bitmap getInfo failed");
        return;
    }
    void *pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) !=
            ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        LOGE("menu: bitmap lockPixels failed");
        return;
    }
    if (!s->menu_tex) {
        glGenTextures(1, &s->menu_tex);
        glBindTexture(GL_TEXTURE_2D, s->menu_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, s->menu_tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(info.stride / 4));
    /* sRGB internal format: Canvas writes sRGB-encoded bytes, so sampling must
     * decode to linear (the SRGB8_ALPHA8 swapchain re-encodes on store).
     * Uploading as plain RGBA would double-encode and wash the menu out.
     * Re-uploads (hover/scroll) reuse the storage via TexSubImage. */
    if (s->menu_tex_w == (int)info.width && s->menu_tex_h == (int)info.height) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info.width, info.height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, info.width,
                     info.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        s->menu_tex_w = (int)info.width;
        s->menu_tex_h = (int)info.height;
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    AndroidBitmap_unlockPixels(env, bitmap);
    s->menu_tex_ready = true;
}

static void menu_move(XrShell *s, int dx, int dy)
{
    if (s->menu_bridge) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_move,
                                      dx, dy);
        menu_jni_check(s, "moveSelection");
    }
}

static void menu_toggle_fp(XrShell *s)
{
    if (s->menu_bridge) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_toggleFp);
        menu_jni_check(s, "toggleFpJit");
    }
}

/* Place a panel dist meters ahead of the current gaze (yaw only, facing the
 * user, at eye height). Falls back to the previous pose if the view can't be
 * located this frame. */
static void place_in_front(XrShell *s, float dist, XrVector3f *pos,
                           XrQuaternionf *orient)
{
    if (!s->view_space || !s->last_predicted_time) {
        return;
    }
    XrSpaceLocation loc = { .type = XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(s->view_space, s->local_space,
                                s->last_predicted_time, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        return;
    }
    XrVector3f fwd = q_rotate(loc.pose.orientation, (XrVector3f){ 0, 0, -1 });
    float len = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len < 0.1f) {
        return; /* looking straight up/down: keep the previous pose */
    }
    float fx = fwd.x / len, fz = fwd.z / len;
    pos->x = loc.pose.position.x + fx * dist;
    pos->y = loc.pose.position.y;
    pos->z = loc.pose.position.z + fz * dist;
    /* Quad +Z normal must point back at the head: R_y(theta)*(0,0,1) = -f. */
    float theta = atan2f(-fx, -fz);
    orient->x = 0.0f;
    orient->y = sinf(theta * 0.5f);
    orient->z = 0.0f;
    orient->w = cosf(theta * 0.5f);
}

static void menu_open(XrShell *s)
{
    if (s->menu_disabled) {
        return;
    }
    menu_jni_init(s);
    if (!s->menu_bridge) {
        return;
    }
    (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_refresh);
    if (menu_jni_check(s, "refresh")) {
        return;
    }
    /* Tell the menu the running process's active FP JIT mode so it can flag
     * per-game toggles that only take effect on the next cold launch. */
    if (s->m_setActiveFp) {
        bool known = s->get_fp_jit != NULL;
        bool value = known && s->get_fp_jit();
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge,
                                      s->m_setActiveFp, (jboolean)known,
                                      (jboolean)value);
        if (menu_jni_check(s, "setActiveFpJit")) {
            return;
        }
    }
    s->menu_open = true;
    s->nav_latch = 0;
    /* Spawn the panel in front of the user's current gaze. */
    place_in_front(s, 1.15f, &s->menu_pos, &s->menu_orient);
    s->pointer_hand = -1;
    s->pointer_inside = false;
    s->pointer_pressed = false;
    s->pointer_sent_press = false;
    s->pointer_sent_x = s->pointer_sent_y = -1;
    /* Release any held inputs so nothing sticks in the game while navigating. */
    s->pad_buttons = 0;
    s->pad_deferred = 0;
    s->synth_buttons = 0;
    memset(s->pad_axis, 0, sizeof(s->pad_axis));
    if (s->set_gamepad) {
        s->set_gamepad(0, s->pad_axis, 6);
    }
    LOGI("menu opened");
}

static void menu_close(XrShell *s)
{
    s->menu_open = false;
    s->nav_latch = 0;
    if (s->menu_bridge) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge,
                                      s->m_pointerExit);
        menu_jni_check(s, "pointerExit");
    }
    s->pointer_inside = false;
    s->pointer_pressed = false;
    /* Resume the game with a clean pad; the user re-presses. Prevents the
     * activation press (e.g. A) from leaking into the resumed title. */
    s->pad_buttons = 0;
    s->pad_deferred = 0;
    s->synth_buttons = 0;
    memset(s->pad_axis, 0, sizeof(s->pad_axis));
    if (s->set_gamepad) {
        s->set_gamepad(0, s->pad_axis, 6);
    }
    LOGI("menu closed");
}

/* Quit the running game back to the Xbox dashboard (eject + guest reset). */
static void menu_quit(XrShell *s)
{
    if (s->request_quit) {
        LOGI("menu: quit to dashboard");
        s->request_quit();
    } else {
        LOGE("menu: quit bridge unavailable (emulator not up?)");
    }
    menu_close(s);
}

static void menu_activate(XrShell *s)
{
    if (!s->menu_bridge) {
        menu_close(s);
        return;
    }
    JNIEnv *env = s->jni_env;
    jstring jpath =
        (jstring)(*env)->CallObjectMethod(env, s->menu_bridge, s->m_activate);
    if (menu_jni_check(s, "activate")) {
        menu_close(s);
        return;
    }
    if (jpath) {
        const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
        if (path) {
            LOGI("menu: launching %s", path);
            if (s->request_load_disc) {
                s->request_load_disc(path);
            } else {
                LOGE("menu: disc-swap bridge unavailable (emulator not up?)");
            }
            (*env)->ReleaseStringUTFChars(env, jpath, path);
        }
        (*env)->DeleteLocalRef(env, jpath);
    }
    menu_close(s);
}

/* Drain UI-raised commands (pointer clicks / contextual buttons). Values
 * mirror XrMenuBridge.CMD_*. */
static void menu_drain_commands(XrShell *s)
{
    if (!s->menu_open || !s->menu_bridge) {
        return;
    }
    for (int guard = 0; guard < 4; guard++) {
        jint cmd = (*s->jni_env)->CallIntMethod(s->jni_env, s->menu_bridge,
                                                s->m_command);
        if (menu_jni_check(s, "consumeCommand")) {
            return;
        }
        switch (cmd) {
        case 0:
            return;
        case 1: /* close */
            menu_close(s);
            return;
        case 2: /* quit to dashboard */
            menu_quit(s);
            return;
        case 3: /* recenter both panels in front of the current gaze */
            place_in_front(s, 1.5f, &s->quad_pos, &s->quad_orient);
            place_in_front(s, 1.15f, &s->menu_pos, &s->menu_orient);
            break;
        case 4: /* launch the selected game */
            menu_activate(s);
            return;
        default:
            return;
        }
    }
}

/* Contextual face button (0=A activate, 1=X, 2=Y): the Kotlin model decides
 * what it means on the current page, then raised commands are drained. */
static void menu_button(XrShell *s, int code)
{
    if (!s->menu_bridge) {
        return;
    }
    (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_button,
                                  code);
    if (menu_jni_check(s, "gamepadButton")) {
        return;
    }
    menu_drain_commands(s);
}

static void menu_nav_page(XrShell *s, int dir)
{
    if (s->menu_bridge) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_navPage,
                                      dir);
        menu_jni_check(s, "navPage");
    }
}

/*
 * Debug-only autonomous menu exercise. XEMU_MENU_AUTOTEST=1 drives
 * open -> navigate -> FP-toggle+revert -> close through the production
 * menu paths; XEMU_MENU_AUTOTEST=switch:<file.iso> additionally reopens,
 * selects that entry by name and activates it (live switch + guest
 * reset). Exists because adb key injection cannot reach an unfocused
 * immersive activity, so headset-free validation must enter below the
 * Android-input layer. Off (zero work) unless the env var is set.
 */
static void menu_autotest_step(XrShell *s)
{
    static int enabled = -1;
    static const char *switch_target;
    static uint32_t frame;
    static int phase;

    if (enabled < 0) {
        const char *env = getenv("XEMU_MENU_AUTOTEST");
        /* The NativeActivity session reaches its first XR frame before the
         * SDL/xemu worker has read prefs and exported env_vars.  Do not latch
         * a missing debug flag in that short interval: after a published
         * emulator frame (last_seq != 0), its environment is fully applied
         * and an absent flag can safely become the zero-work production path.
         */
        if ((!env || !env[0]) && !s->last_seq) {
            return;
        }
        enabled = (env && env[0]) ? 1 : 0;
        if (enabled && strncmp(env, "switch:", 7) == 0) {
            switch_target = env + 7;
        }
        if (enabled) {
            LOGI("menu autotest: armed%s%s", switch_target ? ", switch to " : "",
                 switch_target ? switch_target : "");
        }
    }
    if (!enabled || phase > 6 || s->menu_disabled) {
        return;
    }
    frame++;
    switch (phase) {
    case 0: /* ~5s after session start: open */
        if (frame >= 600) { menu_open(s); phase = 1; frame = 0; }
        break;
    case 1: /* navigate down, down, up */
        if (frame == 120 || frame == 240) { menu_move(s, 0, 1); }
        else if (frame == 360) { menu_move(s, 0, -1); }
        else if (frame >= 480) { menu_toggle_fp(s); phase = 2; frame = 0; }
        break;
    case 2: /* revert the toggle so no persistent state is left behind */
        if (frame >= 120) { menu_toggle_fp(s); phase = 3; frame = 0; }
        break;
    case 3: /* page tour: settings -> system -> library, then close */
        if (frame == 120 || frame == 300 || frame == 480) {
            menu_nav_page(s, 1);
        } else if (frame == 200 || frame == 380) {
            menu_move(s, 0, 1); /* exercise focus on the toured page */
        } else if (frame >= 600) {
            menu_close(s);
            LOGI("menu autotest: nav/toggle/page phase PASS");
            phase = switch_target ? 4 : 7;
            frame = 0;
        }
        break;
    case 4: /* reopen for the switch test */
        if (frame >= 600) { menu_open(s); phase = 5; frame = 0; }
        break;
    case 5:
        if (frame >= 120 && s->menu_bridge) {
            JNIEnv *env = s->jni_env;
            jstring jn = (*env)->NewStringUTF(env, switch_target);
            jboolean ok = (*env)->CallBooleanMethod(
                env, s->menu_bridge, s->m_selectByName, jn);
            (*env)->DeleteLocalRef(env, jn);
            if (!menu_jni_check(s, "selectByName") && ok) {
                phase = 6; frame = 0;
            } else {
                LOGE("menu autotest: selectByName(%s) FAILED", switch_target);
                menu_close(s);
                phase = 7;
            }
        }
        break;
    case 6:
        if (frame >= 120) {
            LOGI("menu autotest: activating switch target");
            menu_activate(s);
            phase = 7;
        }
        break;
    }
}

static void egl_init(XrShell *s)
{
    s->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(s->egl_display, NULL, NULL);
    static const EGLint cfg_attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                       EGL_SURFACE_TYPE,
                                       EGL_PBUFFER_BIT,
                                       EGL_NONE };
    EGLConfig cfg;
    EGLint n;
    eglChooseConfig(s->egl_display, cfg_attr, &cfg, 1, &n);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3,
                                       EGL_NONE };
    s->egl_context =
        eglCreateContext(s->egl_display, cfg, EGL_NO_CONTEXT, ctx_attr);
    static const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16,
                                      EGL_NONE };
    s->egl_pbuffer = eglCreatePbufferSurface(s->egl_display, cfg, pb_attr);
    eglMakeCurrent(s->egl_display, s->egl_pbuffer, s->egl_pbuffer,
                   s->egl_context);
    LOGI("EGL ready: %s", glGetString(GL_VERSION));

    p_eglGetNativeClientBufferANDROID =
        (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress(
            "eglGetNativeClientBufferANDROID");
    p_eglCreateImageKHR =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    s->blit_prog = compile_prog(BLIT_VS, BLIT_FS);
    s->menu_blit_prog = compile_prog(BLIT_VS, MENU_BLIT_FS);
    s->cursor_prog = compile_prog(BLIT_VS, CURSOR_FS);
    s->cursor_center_loc = glGetUniformLocation(s->cursor_prog, "center");
    s->cursor_radius_loc = glGetUniformLocation(s->cursor_prog, "radius");
    s->blit_tex_uniform = glGetUniformLocation(s->blit_prog, "tex");
    if (s->blit_tex_uniform < 0) {
        LOGE("frame bridge: blit sampler uniform unavailable");
    }
    glGenVertexArrays(1, &s->blit_vao);
}

static void xr_create_instance(XrShell *s)
{
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction *)&init_loader);
    if (init_loader) {
        XrLoaderInitInfoAndroidKHR li = {
            .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
            .applicationVM = s->app->activity->vm,
            .applicationContext = s->app->activity->clazz,
        };
        init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&li);
    }

    /* Request only extensions the runtime actually offers; log the rest. */
    uint32_t avail_n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &avail_n, NULL);
    XrExtensionProperties *avail =
        calloc(avail_n, sizeof(XrExtensionProperties));
    for (uint32_t i = 0; i < avail_n; i++) {
        avail[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(NULL, avail_n, &avail_n, avail);

    const char *wanted[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_FB_PASSTHROUGH_EXTENSION_NAME,
        XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
        XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME,
        XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME,
    };
    const char *exts[16];
    uint32_t ext_n = 0;
    for (unsigned w = 0; w < sizeof(wanted) / sizeof(wanted[0]); w++) {
        bool found = false;
        for (uint32_t i = 0; i < avail_n; i++) {
            if (strcmp(avail[i].extensionName, wanted[w]) == 0) {
                found = true;
                break;
            }
        }
        LOGI("ext %s: %s", wanted[w], found ? "ok" : "MISSING");
        if (found) {
            exts[ext_n++] = wanted[w];
        }
    }
    free(avail);

    XrInstanceCreateInfoAndroidKHR android_info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
        .applicationVM = s->app->activity->vm,
        .applicationActivity = s->app->activity->clazz,
    };
    XrInstanceCreateInfo ici = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .next = &android_info,
        .enabledExtensionCount = ext_n,
        .enabledExtensionNames = exts,
    };
    strcpy(ici.applicationInfo.applicationName, "xemu-xr-spike");
    strcpy(ici.applicationInfo.engineName, "xemu");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    OXR(xrCreateInstance(&ici, &s->instance));

    XrInstanceProperties ip = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    OXR(xrGetInstanceProperties(s->instance, &ip));
    LOGI("runtime: %s %u.%u.%u", ip.runtimeName,
         XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi = { .type = XR_TYPE_SYSTEM_GET_INFO,
                            .formFactor =
                                XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
    OXR(xrGetSystem(s->instance, &sgi, &s->system));

    xrGetInstanceProcAddr(s->instance, "xrRequestDisplayRefreshRateFB",
                          (PFN_xrVoidFunction *)&s->request_refresh);
    xrGetInstanceProcAddr(s->instance, "xrGetDisplayRefreshRateFB",
                          (PFN_xrVoidFunction *)&s->get_refresh);
}

/* Task #9: controller action set for 6DOF window move/resize. Touch
 * controller profile. grip = grab-to-move, trigger = resize-mode,
 * thumbstick = push/pull (Y) + scale (X while resizing). */
static void xr_input_init(XrShell *s)
{
    XrActionSetCreateInfo asci = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asci.actionSetName, "window_controls");
    strcpy(asci.localizedActionSetName, "Window Controls");
    if (XR_FAILED(xrCreateActionSet(s->instance, &asci, &s->action_set))) {
        LOGE("action set create failed; 6DOF controls disabled");
        return;
    }
    xrStringToPath(s->instance, "/user/hand/left", &s->hand_path[0]);
    xrStringToPath(s->instance, "/user/hand/right", &s->hand_path[1]);

    XrActionCreateInfo aci = { .type = XR_TYPE_ACTION_CREATE_INFO };
    aci.countSubactionPaths = 2;
    aci.subactionPaths = s->hand_path;

    aci.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    strcpy(aci.actionName, "grab"); strcpy(aci.localizedActionName, "Grab");
    xrCreateAction(s->action_set, &aci, &s->grab_action);
    strcpy(aci.actionName, "resize"); strcpy(aci.localizedActionName, "Resize");
    xrCreateAction(s->action_set, &aci, &s->resize_action);

    aci.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    strcpy(aci.actionName, "stick"); strcpy(aci.localizedActionName, "Stick");
    xrCreateAction(s->action_set, &aci, &s->stick_action);

    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy(aci.actionName, "aim"); strcpy(aci.localizedActionName, "Aim");
    xrCreateAction(s->action_set, &aci, &s->aim_pose_action);

    /* Left-controller menu button toggles the shell UI (right menu button is
     * reserved by Horizon). Single subaction: bound to the left hand only. */
    XrActionCreateInfo mci = { .type = XR_TYPE_ACTION_CREATE_INFO };
    mci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strcpy(mci.actionName, "shell_menu");
    strcpy(mci.localizedActionName, "Shell Menu");
    xrCreateAction(s->action_set, &mci, &s->menu_click_action);

    XrPath p_grip_l, p_grip_r, p_trig_l, p_trig_r, p_stk_l, p_stk_r,
        p_aim_l, p_aim_r, p_menu_l;
    xrStringToPath(s->instance, "/user/hand/left/input/squeeze/value", &p_grip_l);
    xrStringToPath(s->instance, "/user/hand/right/input/squeeze/value", &p_grip_r);
    xrStringToPath(s->instance, "/user/hand/left/input/trigger/value", &p_trig_l);
    xrStringToPath(s->instance, "/user/hand/right/input/trigger/value", &p_trig_r);
    xrStringToPath(s->instance, "/user/hand/left/input/thumbstick", &p_stk_l);
    xrStringToPath(s->instance, "/user/hand/right/input/thumbstick", &p_stk_r);
    xrStringToPath(s->instance, "/user/hand/left/input/aim/pose", &p_aim_l);
    xrStringToPath(s->instance, "/user/hand/right/input/aim/pose", &p_aim_r);
    xrStringToPath(s->instance, "/user/hand/left/input/menu/click", &p_menu_l);

    XrActionSuggestedBinding b[] = {
        { s->grab_action, p_grip_l }, { s->grab_action, p_grip_r },
        { s->resize_action, p_trig_l }, { s->resize_action, p_trig_r },
        { s->stick_action, p_stk_l }, { s->stick_action, p_stk_r },
        { s->aim_pose_action, p_aim_l }, { s->aim_pose_action, p_aim_r },
        { s->menu_click_action, p_menu_l },
    };
    XrPath profile;
    xrStringToPath(s->instance,
                   "/interaction_profiles/oculus/touch_controller", &profile);
    XrInteractionProfileSuggestedBinding sug = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile = profile,
        .countSuggestedBindings = sizeof(b) / sizeof(b[0]),
        .suggestedBindings = b,
    };
    if (XR_FAILED(xrSuggestInteractionProfileBindings(s->instance, &sug))) {
        LOGE("suggest bindings failed; 6DOF controls disabled");
        return;
    }

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo asp = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = s->aim_pose_action,
            .subactionPath = s->hand_path[h],
            .poseInActionSpace = { .orientation = { 0, 0, 0, 1 } },
        };
        xrCreateActionSpace(s->session, &asp, &s->aim_space[h]);
    }
    s->grab_hand = -1;
    s->input_ready = true;
    LOGI("6DOF window controls ready (grip=move, trigger=resize, stick=push/scale)");
}

/* Quest boots the CPU perf domain at SUSTAINED_LOW by default. Since the
 * emulator is a sustained heavy CPU workload (TCG + Vulkan command recording on
 * the guest thread), lift both domains to SUSTAINED_HIGH — the max *sustained*
 * level (not the burst BOOST tier), which maximizes clocks without the thermal/
 * battery risk of BOOST, matching the "performance while controlling power"
 * goal. XR_EXT_performance_settings is already enabled at instance creation.
 * Non-fatal if the runtime lacks the entry point. */
static void xr_set_perf_levels(XrShell *s)
{
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;
    if (XR_FAILED(xrGetInstanceProcAddr(
            s->instance, "xrPerfSettingsSetPerformanceLevelEXT",
            (PFN_xrVoidFunction *)&set_level)) ||
        set_level == NULL) {
        LOGI("perf settings: entry point unavailable");
        return;
    }
    const char *boost_env = getenv("XEMU_XR_PERF_BOOST");
    bool boost = boost_env && boost_env[0] && strcmp(boost_env, "0") != 0;
    XrPerfSettingsLevelEXT level = boost
        ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT
        : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    XrResult rc = set_level(s->session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
    XrResult rg = set_level(s->session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
    LOGI("perf settings: CPU/GPU -> %s (cpu rc=%d gpu rc=%d)",
         boost ? "BOOST" : "SUSTAINED_HIGH", (int)rc, (int)rg);
}

static void xr_attach_action_set(XrShell *s)
{
    if (!s->input_ready || s->action_sets_attached) {
        return;
    }
    XrSessionActionSetsAttachInfo ai = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &s->action_set,
    };
    XrResult r = xrAttachSessionActionSets(s->session, &ai);
    if (XR_SUCCEEDED(r)) {
        s->action_sets_attached = true;
    } else {
        LOGE("xrAttachSessionActionSets failed: %d", (int)r);
    }
}

static float action_float(XrShell *s, XrAction a, int hand)
{
    XrActionStateGetInfo gi = { .type = XR_TYPE_ACTION_STATE_GET_INFO,
                                .action = a,
                                .subactionPath = s->hand_path[hand] };
    XrActionStateFloat st = { .type = XR_TYPE_ACTION_STATE_FLOAT };
    xrGetActionStateFloat(s->session, &gi, &st);
    return st.isActive ? st.currentState : 0.0f;
}

static XrVector2f action_vec2(XrShell *s, XrAction a, int hand)
{
    XrActionStateGetInfo gi = { .type = XR_TYPE_ACTION_STATE_GET_INFO,
                                .action = a,
                                .subactionPath = s->hand_path[hand] };
    XrActionStateVector2f st = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
    xrGetActionStateVector2f(s->session, &gi, &st);
    return st.isActive ? st.currentState : (XrVector2f){ 0, 0 };
}

static bool action_bool(XrShell *s, XrAction a)
{
    XrActionStateGetInfo gi = { .type = XR_TYPE_ACTION_STATE_GET_INFO,
                                .action = a,
                                .subactionPath = XR_NULL_PATH };
    XrActionStateBoolean st = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
    xrGetActionStateBoolean(s->session, &gi, &st);
    return st.isActive && st.currentState;
}

/* Intersect a controller aim pose with the menu quad. Returns panel pixel
 * coordinates when the ray hits the panel front within its bounds. */
static bool menu_ray_hit(XrShell *s, const XrPosef *aim, float *out_x,
                         float *out_y)
{
    XrVector3f n = q_rotate(s->menu_orient, (XrVector3f){ 0, 0, 1 });
    XrVector3f d = q_rotate(aim->orientation, (XrVector3f){ 0, 0, -1 });
    float denom = d.x * n.x + d.y * n.y + d.z * n.z;
    if (fabsf(denom) < 1e-4f) {
        return false;
    }
    XrVector3f rel = v3_sub(s->menu_pos, aim->position);
    float t = (rel.x * n.x + rel.y * n.y + rel.z * n.z) / denom;
    if (t < 0.05f || t > 10.0f) {
        return false;
    }
    XrVector3f hit = v3_add(aim->position,
                            (XrVector3f){ d.x * t, d.y * t, d.z * t });
    XrQuaternionf inv = { -s->menu_orient.x, -s->menu_orient.y,
                          -s->menu_orient.z, s->menu_orient.w };
    XrVector3f local = q_rotate(inv, v3_sub(hit, s->menu_pos));
    float w = s->menu_size_m;
    float h = w * (float)s->menu_h / (float)s->menu_w;
    float u = local.x / w + 0.5f;
    float v = 0.5f - local.y / h;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return false;
    }
    *out_x = u * (float)s->menu_w;
    *out_y = v * (float)s->menu_h;
    return true;
}

/* Laser-pointer processing while the shell UI is open: hover, trigger click
 * (with hysteresis), and stick scrolling all route into the Kotlin model. */
static void menu_pointer_update(XrShell *s, XrSpaceLocation loc[2], float dt)
{
    if (!s->menu_bridge) {
        return;
    }
    /* Pick the pointing hand: keep the current one while it still hits the
     * panel; otherwise prefer the right hand. */
    int hit_hand = -1;
    float px = 0, py = 0;
    int order[3];
    int order_n = 0;
    if (s->pointer_hand >= 0) order[order_n++] = s->pointer_hand;
    order[order_n++] = 1;
    order[order_n++] = 0;
    for (int i = 0; i < order_n; i++) {
        int h = order[i];
        if (h < 0 || h > 1) continue;
        if (!(loc[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(loc[h].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            continue;
        }
        if (menu_ray_hit(s, &loc[h].pose, &px, &py)) {
            hit_hand = h;
            break;
        }
    }

    if (hit_hand < 0) {
        s->pointer_hand = -1;
        if (s->pointer_inside) {
            s->pointer_inside = false;
            s->pointer_pressed = false;
            s->pointer_sent_press = false;
            (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge,
                                          s->m_pointerExit);
            menu_jni_check(s, "pointerExit");
        }
        return;
    }

    s->pointer_hand = hit_hand;
    s->pointer_inside = true;
    s->pointer_x = px;
    s->pointer_y = py;

    /* Trigger click with hysteresis so jitter can't double-fire. */
    float trig = action_float(s, s->resize_action, hit_hand);
    if (!s->pointer_pressed && trig > 0.6f) {
        s->pointer_pressed = true;
    } else if (s->pointer_pressed && trig < 0.4f) {
        s->pointer_pressed = false;
    }

    int ix = (int)px, iy = (int)py;
    if (ix != s->pointer_sent_x || iy != s->pointer_sent_y ||
        s->pointer_pressed != s->pointer_sent_press) {
        s->pointer_sent_x = ix;
        s->pointer_sent_y = iy;
        s->pointer_sent_press = s->pointer_pressed;
        (*s->jni_env)->CallBooleanMethod(s->jni_env, s->menu_bridge,
                                         s->m_pointer, px, py,
                                         (jboolean)s->pointer_pressed);
        if (menu_jni_check(s, "pointer")) {
            return; /* bridge disabled: no further calls this frame */
        }
    }

    /* Stick on the pointing hand scrolls the page smoothly. */
    XrVector2f stick = action_vec2(s, s->stick_action, hit_hand);
    if (stick.y > 0.15f || stick.y < -0.15f) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_scroll,
                                      -stick.y * 1500.0f * dt);
        menu_jni_check(s, "scroll");
    }
}

/* Per-frame controller update. Menu closed: grip = grab-to-move the game
 * window, trigger = resize (stick X scales, Y pushes/pulls). Menu open: the
 * ray drives the UI (trigger = click, stick = scroll) and grip moves the UI
 * panel instead. The left-controller menu button toggles the shell UI in
 * both states. */
static void xr_update_window(XrShell *s, XrTime predicted, float dt)
{
    if (!s->input_ready) {
        return;
    }
    XrActiveActionSet aas = { .actionSet = s->action_set,
                              .subactionPath = XR_NULL_PATH };
    XrActionsSyncInfo si = { .type = XR_TYPE_ACTIONS_SYNC_INFO,
                             .countActiveActionSets = 1,
                             .activeActionSets = &aas };
    if (XR_FAILED(xrSyncActions(s->session, &si))) {
        return;
    }

    /* Menu button edge: toggle the shell UI. */
    if (s->menu_click_action) {
        bool down = action_bool(s, s->menu_click_action);
        if (down && !s->menu_btn_prev) {
            if (s->menu_open) {
                menu_close(s);
            } else {
                menu_open(s);
            }
        }
        s->menu_btn_prev = down;
    }

    /* Locate both controllers. */
    XrSpaceLocation loc[2] = { { .type = XR_TYPE_SPACE_LOCATION },
                               { .type = XR_TYPE_SPACE_LOCATION } };
    for (int h = 0; h < 2; h++) {
        if (s->aim_space[h]) {
            xrLocateSpace(s->aim_space[h], s->local_space, predicted, &loc[h]);
        }
    }

    /* Grab: prefer a hand already grabbing; else whichever grip is pressed.
     * Targets the shell panel while the menu is open, else the game window. */
    XrVector3f *tgt_pos = s->menu_open ? &s->menu_pos : &s->quad_pos;
    XrQuaternionf *tgt_orient = s->menu_open ? &s->menu_orient
                                             : &s->quad_orient;
    int want = -1;
    for (int h = 0; h < 2; h++) {
        if (action_float(s, s->grab_action, h) > 0.6f &&
            (loc[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            want = h;
            if (h == s->grab_hand) {
                break;
            }
        }
    }
    if (want >= 0) {
        XrPosef cp = loc[want].pose;
        if (s->grab_hand != want) {
            /* Begin grab: record window offset in controller-local frame. */
            XrQuaternionf inv = { -cp.orientation.x, -cp.orientation.y,
                                  -cp.orientation.z, cp.orientation.w };
            s->grab_offset = q_rotate(inv, v3_sub(*tgt_pos, cp.position));
            s->grab_hand = want;
        }
        /* Move: window rides the controller. Orientation follows controller
         * so the panel faces where you point. */
        *tgt_pos = v3_add(cp.position, q_rotate(cp.orientation,
                                                s->grab_offset));
        *tgt_orient = cp.orientation;
    } else {
        s->grab_hand = -1;
    }

    if (s->menu_open) {
        menu_pointer_update(s, loc, dt);
        menu_drain_commands(s);
        return;
    }

    /* Resize mode: either trigger held (menu closed only). */
    bool resize = action_float(s, s->resize_action, 0) > 0.6f ||
                  action_float(s, s->resize_action, 1) > 0.6f;

    /* Thumbstick: use the right stick (fall back to left). */
    XrVector2f stick = action_vec2(s, s->stick_action, 1);
    if (stick.x == 0 && stick.y == 0) {
        stick = action_vec2(s, s->stick_action, 0);
    }
    const float DEAD = 0.15f;
    if (resize) {
        if (stick.x > DEAD || stick.x < -DEAD) {
            s->quad_size_m *= (1.0f + stick.x * 0.6f * dt);
            if (s->quad_size_m < 0.3f) s->quad_size_m = 0.3f;
            if (s->quad_size_m > 4.0f) s->quad_size_m = 4.0f;
        }
    }
    if (stick.y > DEAD || stick.y < -DEAD) {
        /* Push/pull along the window's forward (-Z) axis. */
        XrVector3f fwd = q_rotate(s->quad_orient, (XrVector3f){ 0, 0, -1 });
        float d = -stick.y * 0.8f * dt;
        s->quad_pos = v3_add(s->quad_pos,
                             (XrVector3f){ fwd.x * d, fwd.y * d, fwd.z * d });
    }
}

static bool xr_create_session(XrShell *s)
{
    /* GLES requirements query is mandatory before session creation. */
    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = NULL;
    xrGetInstanceProcAddr(s->instance,
                          "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction *)&get_reqs);
    XrGraphicsRequirementsOpenGLESKHR reqs = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR
    };
    OXR(get_reqs(s->instance, s->system, &reqs));

    XrGraphicsBindingOpenGLESAndroidKHR gfx = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
        .display = s->egl_display,
        .config = EGL_NO_CONFIG_KHR,
        .context = s->egl_context,
    };
    XrSessionCreateInfo sci = { .type = XR_TYPE_SESSION_CREATE_INFO,
                                .next = &gfx,
                                .systemId = s->system };
    OXR(xrCreateSession(s->instance, &sci, &s->session));

    XrReferenceSpaceCreateInfo rsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = { .orientation = { 0, 0, 0, 1 },
                                  .position = { 0, 0, 0 } },
    };
    OXR(xrCreateReferenceSpace(s->session, &rsci, &s->local_space));

    /* Head pose for gaze-anchored panel placement (menu open / recenter). */
    XrReferenceSpaceCreateInfo vsci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW,
        .poseInReferenceSpace = { .orientation = { 0, 0, 0, 1 },
                                  .position = { 0, 0, 0 } },
    };
    OXR(xrCreateReferenceSpace(s->session, &vsci, &s->view_space));

    xr_input_init(s);

    /* Passthrough */
    PFN_xrCreatePassthroughFB create_pt = NULL;
    PFN_xrCreatePassthroughLayerFB create_ptl = NULL;
    PFN_xrPassthroughStartFB start_pt = NULL;
    xrGetInstanceProcAddr(s->instance, "xrCreatePassthroughFB",
                          (PFN_xrVoidFunction *)&create_pt);
    xrGetInstanceProcAddr(s->instance, "xrCreatePassthroughLayerFB",
                          (PFN_xrVoidFunction *)&create_ptl);
    xrGetInstanceProcAddr(s->instance, "xrPassthroughStartFB",
                          (PFN_xrVoidFunction *)&start_pt);
    if (create_pt && create_ptl && start_pt) {
        XrPassthroughCreateInfoFB pci = {
            .type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB
        };
        if (XR_SUCCEEDED(create_pt(s->session, &pci, &s->passthrough))) {
            XrPassthroughLayerCreateInfoFB plci = {
                .type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB,
                .passthrough = s->passthrough,
                .purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB,
            };
            if (XR_SUCCEEDED(create_ptl(s->session, &plci,
                                        &s->passthrough_layer)) &&
                XR_SUCCEEDED(start_pt(s->passthrough))) {
                s->have_passthrough = true;
                LOGI("passthrough ready");
            }
        }
    }

    /* The package declares BOUNDARYLESS_APP, which Meta permits only for a
     * passthrough MR experience. Never present the old opaque fallback: it
     * would both violate that contract and leave the user without the real
     * world behind the resizable emulator window. */
    if (!s->have_passthrough) {
        LOGE("passthrough required for boundaryless MR; refusing opaque XR session");
        return false;
    }

    /* Quad swapchain (matches Xbox 2x-scaled output for later reuse) */
    s->quad_w = 1280;
    s->quad_h = 960;
    XrSwapchainCreateInfo sw = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = GL_SRGB8_ALPHA8,
        .sampleCount = 1,
        .width = s->quad_w,
        .height = s->quad_h,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    OXR(xrCreateSwapchain(s->session, &sw, &s->swapchain));
    OXR(xrEnumerateSwapchainImages(s->swapchain, 0, &s->swapchain_len, NULL));
    s->images = calloc(s->swapchain_len, sizeof(*s->images));
    for (uint32_t i = 0; i < s->swapchain_len; i++) {
        s->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    OXR(xrEnumerateSwapchainImages(s->swapchain, s->swapchain_len,
                                   &s->swapchain_len,
                                   (XrSwapchainImageBaseHeader *)s->images));
    s->fbos = calloc(s->swapchain_len, sizeof(GLuint));
    glGenFramebuffers(s->swapchain_len, s->fbos);
    for (uint32_t i = 0; i < s->swapchain_len; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s->images[i].image, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("frame bridge: quad FBO %u incomplete (0x%x)", i, status);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("swapchain ready: %ux%u x%u images", s->quad_w, s->quad_h,
         s->swapchain_len);

    if (s->request_refresh) {
        OXR(s->request_refresh(s->session, 120.0f));
        float rate = 0;
        if (s->get_refresh) {
            s->get_refresh(s->session, &rate);
        }
        LOGI("display refresh requested 120, now %.0f", rate);
    }

    /* Shell-UI quad swapchain (landscape panel). Allocated up front but only
     * presented while the menu is open, so there is no per-frame cost when
     * closed. */
    s->menu_w = 1600;
    s->menu_h = 1000;
    XrSwapchainCreateInfo msw = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = GL_SRGB8_ALPHA8,
        .sampleCount = 1,
        .width = s->menu_w,
        .height = s->menu_h,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    OXR(xrCreateSwapchain(s->session, &msw, &s->menu_swapchain));
    OXR(xrEnumerateSwapchainImages(s->menu_swapchain, 0, &s->menu_swapchain_len,
                                   NULL));
    s->menu_images = calloc(s->menu_swapchain_len, sizeof(*s->menu_images));
    for (uint32_t i = 0; i < s->menu_swapchain_len; i++) {
        s->menu_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    }
    OXR(xrEnumerateSwapchainImages(
        s->menu_swapchain, s->menu_swapchain_len, &s->menu_swapchain_len,
        (XrSwapchainImageBaseHeader *)s->menu_images));
    s->menu_fbos = calloc(s->menu_swapchain_len, sizeof(GLuint));
    glGenFramebuffers(s->menu_swapchain_len, s->menu_fbos);
    for (uint32_t i = 0; i < s->menu_swapchain_len; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s->menu_fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, s->menu_images[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    LOGI("menu swapchain ready: %ux%u x%u images", s->menu_w, s->menu_h,
         s->menu_swapchain_len);
    return true;
}

static void draw_transparent_placeholder(XrShell *s, GLuint fbo)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, s->quad_w, s->quad_h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void xr_frame(XrShell *s)
{
    XrFrameWaitInfo fwi = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
    OXR(xrWaitFrame(s->session, &fwi, &fs));
    s->last_predicted_time = fs.predictedDisplayTime;
    int64_t work_start_ns = now_ns();
    adpf_init_render(s);
    XrFrameBeginInfo fbi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    OXR(xrBeginFrame(s->session, &fbi));

    /* Retract an expired synthetic START/BACK tap (see on_input). */
    if (s->synth_buttons && now_ns() >= s->synth_retract_ns) {
        s->synth_buttons = 0;
        xr_forward_pad(s);
    }

    menu_autotest_step(s);

    /* Resolve before constructing the composition layer: aspect changes are
     * visible in the same XR frame and never perturb frame production. */
    resolve_emulator_feed(s);
    xr_update_emulator_aspect(s);

    /* Drive 6DOF window move/resize from the controllers. */
    xr_update_window(s, fs.predictedDisplayTime, 1.0f / 72.0f);

    float qh = s->quad_size_m / (s->quad_aspect > 0 ? s->quad_aspect : 1.333f);
    XrCompositionLayerQuad quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = s->local_space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = { .swapchain = s->swapchain,
                      .imageRect = { .offset = { 0, 0 },
                                     .extent = { s->quad_w, s->quad_h } } },
        .pose = { .orientation = s->quad_orient, .position = s->quad_pos },
        .size = { s->quad_size_m, qh },
    };

    if (fs.shouldRender) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO
        };
        OXR(xrAcquireSwapchainImage(s->swapchain, &ai, &idx));
        XrSwapchainImageWaitInfo wi = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = XR_INFINITE_DURATION
        };
        OXR(xrWaitSwapchainImage(s->swapchain, &wi));

        xr_apply_thread_settings(s);
        GLuint emu_tex = 0;
        struct AHardwareBuffer *emu_ahb = NULL;
        uint64_t emu_seq = 0;
        if (s->acquire_ahb) {
            emu_ahb = s->acquire_ahb(&emu_seq);
            if (emu_ahb) {
                emu_tex = ahb_to_texture(s, emu_ahb);
                if (emu_seq != s->last_seq) {
                    int64_t frame_now = now_ns();
                    if (s->adpf_last_frame_seq_ns > 0) {
                        adpf_report(s->adpf_vcpu,
                                    frame_now - s->adpf_last_frame_seq_ns,
                                    frame_now, s->adpf_update_rate_ns,
                                    &s->adpf_vcpu_report_ns);
                    }
                    s->adpf_last_frame_seq_ns = frame_now;
                }
                if (emu_seq != s->last_seq && (emu_seq % 600) == 0) {
                    LOGI("emulator frame seq %llu",
                         (unsigned long long)emu_seq);
                }
                s->last_seq = emu_seq;
                AHardwareBuffer_release(emu_ahb); /* cache holds its own ref via EGLImage */
            }
        }
        if (emu_tex) {
            glBindFramebuffer(GL_FRAMEBUFFER, s->fbos[idx]);
            glViewport(0, 0, s->quad_w, s->quad_h);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(s->blit_prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, emu_tex);
            if (s->blit_tex_uniform >= 0) {
                glUniform1i(s->blit_tex_uniform, 0);
            }
            glBindVertexArray(s->blit_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            GLenum gl_error = glGetError();
            if (gl_error != GL_NO_ERROR) {
                LOGE("frame bridge: blit failed (tex=%u seq=%llu err=0x%x)",
                     emu_tex, (unsigned long long)emu_seq, gl_error);
            }
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            draw_transparent_placeholder(s, s->fbos[idx]);
        }
        if (!emu_tex && !s->bridge_fallback_logged) {
            LOGI("frame bridge: feed=%s ahb=%p seq=%llu tex=%u mode=%s",
                 s->acquire_ahb ? "ready" : "unresolved", emu_ahb,
                 (unsigned long long)emu_seq, emu_tex,
                 emu_tex ? "emulator" : "fallback");
            s->bridge_fallback_logged = true;
        } else if (emu_tex && !s->bridge_ready_logged) {
            LOGI("frame bridge: feed=%s ahb=%p seq=%llu tex=%u mode=%s",
                 s->acquire_ahb ? "ready" : "unresolved", emu_ahb,
                 (unsigned long long)emu_seq, emu_tex,
                 emu_tex ? "emulator" : "fallback");
            s->bridge_ready_logged = true;
        }
        glFinish();
        XrSwapchainImageReleaseInfo ri = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
        };
        OXR(xrReleaseSwapchainImage(s->swapchain, &ri));

        /* Menu overlay: re-upload the bitmap only when the model reports a
         * change, then blit it (alpha preserved) into the menu swapchain. */
        if (s->menu_open && s->menu_bridge) {
            JNIEnv *env = s->jni_env;
            /* Push the guest pace readout about once a second; 0 when no
             * fresh emulator frame arrived in the last 2 seconds. */
            int64_t push_now = now_ns();
            if (push_now - s->guest_ms_push_ns > 1000000000LL) {
                s->guest_ms_push_ns = push_now;
                float ms = 0.0f;
                if (s->get_game_frame_ms && s->adpf_last_frame_seq_ns > 0 &&
                    push_now - s->adpf_last_frame_seq_ns < 2000000000LL) {
                    ms = s->get_game_frame_ms();
                }
                (*env)->CallVoidMethod(env, s->menu_bridge, s->m_setGuestMs,
                                       ms);
                menu_jni_check(s, "setGuestFrameMs");
            }
            jboolean dirty = s->menu_bridge ?
                (*env)->CallBooleanMethod(env, s->menu_bridge, s->m_isDirty) :
                JNI_FALSE;
            if (menu_jni_check(s, "isDirty")) {
                dirty = JNI_FALSE;
            }
            if (s->menu_bridge && (dirty || !s->menu_tex_ready)) {
                jobject bmp = (*env)->CallObjectMethod(
                    env, s->menu_bridge, s->m_render, s->menu_w, s->menu_h);
                if (menu_jni_check(s, "render")) {
                    /* render threw (e.g. bitmap OOM): picker disabled. */
                } else if (bmp) {
                    menu_upload_texture(s, bmp);
                    (*env)->DeleteLocalRef(env, bmp);
                }
            }
            if (s->menu_open && s->menu_tex_ready) {
                uint32_t midx = 0;
                XrSwapchainImageAcquireInfo mai = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                OXR(xrAcquireSwapchainImage(s->menu_swapchain, &mai, &midx));
                XrSwapchainImageWaitInfo mwi = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                    .timeout = XR_INFINITE_DURATION };
                OXR(xrWaitSwapchainImage(s->menu_swapchain, &mwi));
                glBindFramebuffer(GL_FRAMEBUFFER, s->menu_fbos[midx]);
                glViewport(0, 0, s->menu_w, s->menu_h);
                glDisable(GL_SCISSOR_TEST);
                glDisable(GL_BLEND);
                glClearColor(0, 0, 0, 0);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(s->menu_blit_prog);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, s->menu_tex);
                glBindVertexArray(s->blit_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                /* Laser-pointer cursor on top of the panel (FBO origin is
                 * bottom-left; panel pixels are top-left). */
                if (s->pointer_inside && s->cursor_prog) {
                    const float radius = 11.0f;
                    float cx = s->pointer_x;
                    float cy = (float)s->menu_h - s->pointer_y;
                    glUseProgram(s->cursor_prog);
                    glUniform2f(s->cursor_center_loc, cx, cy);
                    glUniform1f(s->cursor_radius_loc,
                                s->pointer_pressed ? radius * 0.8f : radius);
                    glEnable(GL_BLEND);
                    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    glEnable(GL_SCISSOR_TEST);
                    int sx = (int)(cx - radius - 3.0f);
                    int sy = (int)(cy - radius - 3.0f);
                    int sw = (int)(radius * 2.0f + 6.0f);
                    glScissor(sx < 0 ? 0 : sx, sy < 0 ? 0 : sy, sw, sw);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    glDisable(GL_SCISSOR_TEST);
                    glDisable(GL_BLEND);
                }
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glFinish();
                XrSwapchainImageReleaseInfo mri = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                OXR(xrReleaseSwapchainImage(s->menu_swapchain, &mri));
            }
        }
    }

    XrCompositionLayerPassthroughFB pt_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
        .layerHandle = s->passthrough_layer,
    };

    float menu_qh = s->menu_size_m * (float)s->menu_h / (float)s->menu_w;
    XrCompositionLayerQuad menu_quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = s->local_space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = { .swapchain = s->menu_swapchain,
                      .imageRect = { .offset = { 0, 0 },
                                     .extent = { s->menu_w, s->menu_h } } },
        .pose = { .orientation = s->menu_orient, .position = s->menu_pos },
        .size = { s->menu_size_m, menu_qh },
    };

    const XrCompositionLayerBaseHeader *layers[3];
    uint32_t nlayers = 0;
    if (s->have_passthrough) {
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&pt_layer;
    }
    layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quad;
    /* Picker draws on top of the emulator quad. */
    if (s->menu_open && s->menu_tex_ready) {
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&menu_quad;
    }

    XrFrameEndInfo fei = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = fs.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = nlayers,
        .layers = layers,
    };
    OXR(xrEndFrame(s->session, &fei));
    int64_t work_end_ns = now_ns();
    adpf_report(s->adpf_render, work_end_ns - work_start_ns, work_end_ns,
                s->adpf_update_rate_ns, &s->adpf_render_report_ns);

    s->frame_no++;
    if (s->frame_no % 300 == 0) {
        float rate = 0;
        if (s->get_refresh) {
            s->get_refresh(s->session, &rate);
        }
        LOGI("frame %llu, refresh %.0f",
             (unsigned long long)s->frame_no, rate);
    }
}

static void xr_poll_events(XrShell *s)
{
    XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(s->instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *sc =
                (XrEventDataSessionStateChanged *)&ev;
            LOGI("session state -> %d", (int)sc->state);
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {
                    .type = XR_TYPE_SESSION_BEGIN_INFO,
                    .primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                };
                OXR(xrBeginSession(s->session, &bi));
                xr_attach_action_set(s);
                xr_set_perf_levels(s);
                s->session_running = true;
            } else if (sc->state == XR_SESSION_STATE_FOCUSED) {
                start_emulator_once(s);
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                OXR(xrEndSession(s->session));
                s->session_running = false;
            }
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        ev.next = NULL;
    }
}

/* Emulator controller button masks (must match ui/xemu-input.h). LB/RB map to
 * the Original Xbox Duke's White/Black buttons (standard xemu mapping). */
#define GP_A (1u<<0)
#define GP_B (1u<<1)
#define GP_X (1u<<2)
#define GP_Y (1u<<3)
#define GP_DL (1u<<4)
#define GP_DU (1u<<5)
#define GP_DR (1u<<6)
#define GP_DD (1u<<7)
#define GP_BACK (1u<<8)
#define GP_START (1u<<9)
#define GP_WHITE (1u<<10)
#define GP_BLACK (1u<<11)
#define GP_LS (1u<<12)
#define GP_RS (1u<<13)
#define GP_GUIDE (1u<<14)
/* axis indices (CONTROLLER_AXIS_*): LTRIG,RTRIG,LX,LY,RX,RY */
#define AX_LTRIG 0
#define AX_RTRIG 1
#define AX_LX 2
#define AX_LY 3
#define AX_RX 4
#define AX_RY 5

static uint16_t gp_keycode_mask(int32_t code)
{
    switch (code) {
    case AKEYCODE_BUTTON_A:      return GP_A;
    case AKEYCODE_BUTTON_B:      return GP_B;
    case AKEYCODE_BUTTON_X:      return GP_X;
    case AKEYCODE_BUTTON_Y:      return GP_Y;
    case AKEYCODE_BUTTON_L1:     return GP_WHITE;
    case AKEYCODE_BUTTON_R1:     return GP_BLACK;
    case AKEYCODE_BUTTON_THUMBL: return GP_LS;
    case AKEYCODE_BUTTON_THUMBR: return GP_RS;
    case AKEYCODE_BUTTON_START:  return GP_START;
    case AKEYCODE_BUTTON_SELECT: return GP_BACK;
    case AKEYCODE_BUTTON_MODE:   return GP_GUIDE;
    case AKEYCODE_DPAD_UP:       return GP_DU;
    case AKEYCODE_DPAD_DOWN:     return GP_DD;
    case AKEYCODE_DPAD_LEFT:     return GP_DL;
    case AKEYCODE_DPAD_RIGHT:    return GP_DR;
    default:                     return 0;
    }
}

static int16_t gp_axf(float v)  /* stick: -1..1 -> int16 (SDL/Xbox convention) */
{
    if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
    return (int16_t)(v * 32767.f);
}
static int16_t gp_axt(float v)  /* trigger: 0..1 -> 0..32767 */
{
    if (v > 1.f) v = 1.f; else if (v < 0.f) v = 0.f;
    return (int16_t)(v * 32767.f);
}

/* Evaluate menu navigation from the unified pad state (dpad keys, hat, or the
 * left stick), edge-triggered per axis so one push moves focus one step. */
static void menu_nav_eval(XrShell *s)
{
    int dy = 0, dx = 0;
    if (s->pad_buttons & GP_DU) dy = -1;
    else if (s->pad_buttons & GP_DD) dy = 1;
    if (s->pad_buttons & GP_DL) dx = -1;
    else if (s->pad_buttons & GP_DR) dx = 1;
    if (dy == 0) {
        int16_t ly = s->pad_axis[AX_LY]; /* up = positive (see on_input) */
        if (ly > 16000) dy = -1;
        else if (ly < -16000) dy = 1;
    }
    if (dx == 0) {
        int16_t lx = s->pad_axis[AX_LX];
        if (lx < -16000) dx = -1;
        else if (lx > 16000) dx = 1;
    }
    if (dy != 0) {
        if (s->nav_latch == 0) {
            menu_move(s, 0, dy);
            s->nav_latch = dy;
        }
    } else {
        s->nav_latch = 0;
    }
    if (dx != 0) {
        if (s->nav_latch_x == 0) {
            menu_move(s, dx, 0);
            s->nav_latch_x = dx;
        }
    } else {
        s->nav_latch_x = 0;
    }
}

/* Translate Android gamepad input and forward it to the emulator (SDL can't see
 * the pad while the XR NativeActivity holds focus). While the picker is open,
 * gamepad input drives the menu instead and is not forwarded to the game. */
static int32_t on_input(struct android_app *app, AInputEvent *event)
{
    XrShell *s = (XrShell *)app->userData;
    int32_t type = AInputEvent_getType(event);
    int32_t src = AInputEvent_getSource(event);

    if (type == AINPUT_EVENT_TYPE_KEY &&
        (src & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK |
                AINPUT_SOURCE_DPAD))) {
        uint16_t mask = gp_keycode_mask(AKeyEvent_getKeyCode(event));
        if (!mask) return 0;
        int32_t action = AKeyEvent_getAction(event);
        if (action == AKEY_EVENT_ACTION_DOWN) s->pad_buttons |= mask;
        else if (action == AKEY_EVENT_ACTION_UP) s->pad_buttons &= ~mask;

        if (s->menu_open) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                if (mask == GP_A) menu_button(s, 0);
                else if (mask == GP_B) menu_close(s);
                else if (mask == GP_X) menu_button(s, 1);
                else if (mask == GP_Y) menu_button(s, 2);
                else if (mask == GP_WHITE) menu_nav_page(s, -1);
                else if (mask == GP_BLACK) menu_nav_page(s, 1);
                else if ((s->pad_buttons & (GP_START | GP_BACK)) ==
                         (GP_START | GP_BACK)) menu_close(s);
            }
            menu_nav_eval(s);
            return 1; /* consume; never forward to the game while in the menu */
        }

        /* Reserved combo opens the picker (mirrors the 2D Start+Back menu). */
        if (action == AKEY_EVENT_ACTION_DOWN &&
            (s->pad_buttons & (GP_START | GP_BACK)) ==
                (GP_START | GP_BACK)) {
            menu_open(s);
            return 1;
        }

        /* Hold-and-decide for START/BACK so a combo-in-progress never leaks a
         * spurious lone press into the game. Defer the bit while only one of the
         * pair is held; on release without a completed combo, emit a momentary
         * tap so the button still works on its own. */
        if (mask == GP_START || mask == GP_BACK) {
            if (action == AKEY_EVENT_ACTION_DOWN) {
                s->pad_deferred |= mask;
            } else if (action == AKEY_EVENT_ACTION_UP &&
                       (s->pad_deferred & mask)) {
                /* Lone tap (combo never completed): assert a synthetic press
                 * long enough to survive a guest poll (the XR pad path has no
                 * MIN_BUTTON_HOLD auto-extend), retracted from the frame loop.
                 * A momentary back-to-back pulse here is never sampled. */
                s->pad_deferred &= ~mask;
                s->synth_buttons |= mask;
                s->synth_retract_ns = now_ns() + 60000000LL; /* 60 ms */
                xr_forward_pad(s);
                return 1;
            }
        }

        xr_forward_pad(s);
        static int gp_key_log = 0;
        if (gp_key_log++ < 12) {
            LOGI("XR gamepad key: code=%d action=%d -> buttons=0x%04x fwd=%d",
                 AKeyEvent_getKeyCode(event), action, s->pad_buttons,
                 s->set_gamepad != NULL);
        }
        return 1; /* consume gamepad keys */
    }

    if (type == AINPUT_EVENT_TYPE_MOTION && (src & AINPUT_SOURCE_JOYSTICK)) {
        /* Y axes are negated: Android reports stick-up as negative, but the
         * emulator's convention (see keyboard map + SDL path's default
         * invert_axis_*_y) is stick-up = POSITIVE. X and triggers match as-is. */
        s->pad_axis[AX_LX] = gp_axf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0));
        s->pad_axis[AX_LY] = gp_axf(-AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
        s->pad_axis[AX_RX] = gp_axf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0));
        s->pad_axis[AX_RY] = gp_axf(-AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0));
        float lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        if (lt == 0.f) lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        if (rt == 0.f) rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        s->pad_axis[AX_LTRIG] = gp_axt(lt);
        s->pad_axis[AX_RTRIG] = gp_axt(rt);
        float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        s->pad_buttons &= ~(GP_DL | GP_DR | GP_DU | GP_DD);
        if (hx < -0.5f) s->pad_buttons |= GP_DL;
        else if (hx > 0.5f) s->pad_buttons |= GP_DR;
        if (hy < -0.5f) s->pad_buttons |= GP_DU;
        else if (hy > 0.5f) s->pad_buttons |= GP_DD;
        if (s->menu_open) {
            menu_nav_eval(s);
            return 1; /* drive the menu; don't forward to the game */
        }
        xr_forward_pad(s);
        return 1;
    }
    return 0;
}

static void on_app_cmd(struct android_app *app, int32_t cmd)
{
    XrShell *s = (XrShell *)app->userData;
    switch (cmd) {
    case APP_CMD_RESUME: s->resumed = true; break;
    case APP_CMD_PAUSE: s->resumed = false; break;
    default: break;
    }
}

void android_main(struct android_app *app)
{
    XrShell shell;
    memset(&shell, 0, sizeof(shell));
    shell.app = app;
    app->userData = &shell;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;

    /* Default window placement: 1.5 m ahead, 1.2 m wide, facing the user. */
    shell.quad_pos = (XrVector3f){ 0.0f, 0.0f, -1.5f };
    shell.quad_orient = (XrQuaternionf){ 0, 0, 0, 1 };
    shell.quad_size_m = 1.2f;
    shell.quad_aspect = 4.0f / 3.0f;
    shell.grab_hand = -1;

    /* Shell UI panel: landscape, a little closer than the game window; it is
     * re-anchored in front of the user's gaze every time it opens. */
    shell.menu_pos = (XrVector3f){ 0.0f, 0.0f, -1.15f };
    shell.menu_orient = (XrQuaternionf){ 0, 0, 0, 1 };
    shell.menu_size_m = 1.15f;
    shell.pointer_hand = -1;
    shell.pointer_sent_x = -1;
    shell.pointer_sent_y = -1;

    LOGI("xr_shell spike starting");
    egl_init(&shell);
    xr_create_instance(&shell);
    if (!xr_create_session(&shell)) {
        LOGE("xr_shell exiting: required passthrough setup failed");
        ANativeActivity_finish(app->activity);
        return;
    }

    while (!app->destroyRequested) {
        int events;
        struct android_poll_source *source;
        int timeout = shell.session_running ? 0 : 250;
        while (ALooper_pollOnce(timeout, NULL, &events,
                                (void **)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
            timeout = 0;
        }
        xr_poll_events(&shell);
        if (shell.session_running) {
            xr_frame(&shell);
        }
    }

    /* Release the JNI bridge and detach this thread if we ever attached it. */
    if (shell.jvm && shell.jni_env) {
        if (shell.menu_bridge) {
            (*shell.jni_env)->DeleteGlobalRef(shell.jni_env, shell.menu_bridge);
            shell.menu_bridge = NULL;
        }
        (*shell.jvm)->DetachCurrentThread(shell.jvm);
        shell.jni_env = NULL;
    }
    if (shell.adpf_vcpu) {
        p_adpf_close(shell.adpf_vcpu);
    }
    if (shell.adpf_render) {
        p_adpf_close(shell.adpf_render);
    }
    LOGI("xr_shell spike exiting");
}
