/*
 * xr_shell — OpenXR shell spike for xemu on Quest 3 (Spike A).
 *
 * Goal of this spike (docs/openxr-shell-design.md):
 *   1. Stand up an immersive OpenXR session from a NativeActivity inside
 *      the existing app.
 *   2. Passthrough behind (XR_FB_passthrough) + one quad composition
 *      layer showing a generated test pattern.
 *   3. Log whether Android gamepad KeyEvents reach the activity while
 *      immersive (the decisive input question for the full shell).
 *   4. Exercise XR_FB_display_refresh_rate + XR_EXT_performance_settings.
 *
 * Deliberately single-file and dependency-light; the emulator is NOT
 * wired in yet. Frame content: animated color bars so motion is obvious.
 */

#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/bitmap.h>
#include <android/keycodes.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    GLuint blit_vao;

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

    /* Grab drag state */
    int grab_hand;          /* -1 none, else 0/1 */
    XrVector3f grab_offset;  /* window pos relative to controller at grab */
    bool resizing;

    /* Emulator control bridges (resolved from libxemu.so alongside the feed);
     * NULL until the emulator side is up. */
    void (*request_load_disc)(const char *path);
    void (*request_quit)(void);   /* eject + reset -> Xbox dashboard */
    bool (*get_fp_jit)(void);     /* running process's active FP JIT mode */

    /* In-VR game picker menu (JNI-backed, rendered to its own quad layer). */
    bool menu_open;
    bool menu_disabled;     /* set after a JNI failure; picker off for session */
    XrSwapchain menu_swapchain;
    uint32_t menu_swapchain_len;
    XrSwapchainImageOpenGLESKHR *menu_images;
    GLuint *menu_fbos;
    int menu_w, menu_h;
    GLuint menu_tex;        /* holds the latest uploaded menu bitmap */
    bool menu_tex_ready;    /* menu_tex has valid contents to blit */
    GLuint menu_blit_prog;  /* alpha-preserving blit (vs opaque blit_prog) */
    XrVector3f menu_pos;
    XrQuaternionf menu_orient;
    float menu_size_m;

    /* JNI bridge to XrMenuBridge (Kotlin owns the list + Canvas rendering). */
    JavaVM *jvm;
    JNIEnv *jni_env;        /* android_main thread, attached once */
    jobject menu_bridge;    /* global ref, NULL if JNI init failed */
    jmethodID m_refresh, m_count, m_isDirty, m_render, m_move, m_toggleFp,
        m_activate, m_setActiveFp;
    bool jni_tried;

    /* Menu input edge state */
    int nav_latch;          /* -1/0/1: debounces stick/hat/dpad navigation */
    uint16_t pad_deferred;  /* START/BACK held but not yet forwarded (combo) */
} XrShell;

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

static GLuint compile_prog(const char *vs_src, const char *fs_src)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
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
            /* EGLImages leak here in the spike; bounded by resizes. */
            memset(&s->ahb_cache[i], 0, sizeof(s->ahb_cache[i]));
        }
        slot = 0;
    }
    EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(ahb);
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
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
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
    if (s->acquire_ahb && s->set_gamepad && s->request_load_disc) {
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
    s->m_move = (*env)->GetMethodID(env, bcls, "moveSelection", "(I)V");
    s->m_toggleFp = (*env)->GetMethodID(env, bcls, "toggleFpJit", "()V");
    s->m_activate = (*env)->GetMethodID(env, bcls, "activate",
                                        "()Ljava/lang/String;");
    s->m_setActiveFp =
        (*env)->GetMethodID(env, bcls, "setActiveFpJit", "(ZZ)V");
    /* A missing method ID leaves a pending exception AND would abort ART on the
     * next Call*; validate all before publishing the bridge. */
    if ((*env)->ExceptionCheck(env) || !s->m_refresh || !s->m_count ||
        !s->m_isDirty || !s->m_render || !s->m_move || !s->m_toggleFp ||
        !s->m_activate || !s->m_setActiveFp) {
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
     * Uploading as plain RGBA would double-encode and wash the menu out. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, info.width, info.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    AndroidBitmap_unlockPixels(env, bitmap);
    s->menu_tex_ready = true;
}

static void menu_move(XrShell *s, int delta)
{
    if (s->menu_bridge) {
        (*s->jni_env)->CallVoidMethod(s->jni_env, s->menu_bridge, s->m_move,
                                      delta);
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
    /* Release any held inputs so nothing sticks in the game while navigating. */
    s->pad_buttons = 0;
    s->pad_deferred = 0;
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
    /* Resume the game with a clean pad; the user re-presses. Prevents the
     * activation press (e.g. A) from leaking into the resumed title. */
    s->pad_buttons = 0;
    s->pad_deferred = 0;
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
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    s->blit_prog = compile_prog(BLIT_VS, BLIT_FS);
    s->menu_blit_prog = compile_prog(BLIT_VS, MENU_BLIT_FS);
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

    XrPath p_grip_l, p_grip_r, p_trig_l, p_trig_r, p_stk_l, p_stk_r,
        p_aim_l, p_aim_r;
    xrStringToPath(s->instance, "/user/hand/left/input/squeeze/value", &p_grip_l);
    xrStringToPath(s->instance, "/user/hand/right/input/squeeze/value", &p_grip_r);
    xrStringToPath(s->instance, "/user/hand/left/input/trigger/value", &p_trig_l);
    xrStringToPath(s->instance, "/user/hand/right/input/trigger/value", &p_trig_r);
    xrStringToPath(s->instance, "/user/hand/left/input/thumbstick", &p_stk_l);
    xrStringToPath(s->instance, "/user/hand/right/input/thumbstick", &p_stk_r);
    xrStringToPath(s->instance, "/user/hand/left/input/aim/pose", &p_aim_l);
    xrStringToPath(s->instance, "/user/hand/right/input/aim/pose", &p_aim_r);

    XrActionSuggestedBinding b[] = {
        { s->grab_action, p_grip_l }, { s->grab_action, p_grip_r },
        { s->resize_action, p_trig_l }, { s->resize_action, p_trig_r },
        { s->stick_action, p_stk_l }, { s->stick_action, p_stk_r },
        { s->aim_pose_action, p_aim_l }, { s->aim_pose_action, p_aim_r },
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
    XrResult rc = set_level(s->session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                            XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    XrResult rg = set_level(s->session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                            XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    LOGI("perf settings: CPU/GPU -> SUSTAINED_HIGH (cpu rc=%d gpu rc=%d)",
         (int)rc, (int)rg);
}

static void xr_attach_action_set(XrShell *s)
{
    if (!s->input_ready) {
        return;
    }
    XrSessionActionSetsAttachInfo ai = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &s->action_set,
    };
    OXR(xrAttachSessionActionSets(s->session, &ai));
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

/* Per-frame 6DOF window update. dt in seconds; predicted display time for
 * the aim pose. Grip on either hand grabs the window and moves it rigidly
 * with the controller (position + a follow of controller yaw). Trigger
 * held = resize: thumbstick X scales, Y pushes/pulls along view. Stick Y
 * without resize also nudges depth. */
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

    /* Locate both controllers. */
    XrSpaceLocation loc[2] = { { .type = XR_TYPE_SPACE_LOCATION },
                               { .type = XR_TYPE_SPACE_LOCATION } };
    for (int h = 0; h < 2; h++) {
        if (s->aim_space[h]) {
            xrLocateSpace(s->aim_space[h], s->local_space, predicted, &loc[h]);
        }
    }

    /* Resize mode: either trigger held. */
    bool resize = action_float(s, s->resize_action, 0) > 0.6f ||
                  action_float(s, s->resize_action, 1) > 0.6f;

    /* Grab: prefer a hand already grabbing; else whichever grip is pressed. */
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
            s->grab_offset = q_rotate(inv, v3_sub(s->quad_pos, cp.position));
            s->grab_hand = want;
        }
        /* Move: window rides the controller. Orientation follows controller
         * so the panel faces where you point. */
        s->quad_pos = v3_add(cp.position, q_rotate(cp.orientation,
                                                    s->grab_offset));
        s->quad_orient = cp.orientation;
    } else {
        s->grab_hand = -1;
    }

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

static void xr_create_session(XrShell *s)
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
                                        &s->passthrough_layer))) {
                start_pt(s->passthrough);
                s->have_passthrough = true;
                LOGI("passthrough ready");
            }
        }
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

    /* Menu quad swapchain (portrait). Allocated up front but only presented
     * while the picker is open, so there is no per-frame cost when closed. */
    s->menu_w = 1024;
    s->menu_h = 1280;
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
}

static void draw_test_pattern(XrShell *s, GLuint fbo)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, s->quad_w, s->quad_h);
    glEnable(GL_SCISSOR_TEST);
    int bands = 8;
    for (int i = 0; i < bands; i++) {
        float phase = (float)((s->frame_no / 2 + i) % bands) / bands;
        glScissor(i * s->quad_w / bands, 0, s->quad_w / bands, s->quad_h);
        glClearColor(phase, 1.0f - phase, (i & 1) ? 1.0f : 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void xr_frame(XrShell *s)
{
    XrFrameWaitInfo fwi = { .type = XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
    OXR(xrWaitFrame(s->session, &fwi, &fs));
    XrFrameBeginInfo fbi = { .type = XR_TYPE_FRAME_BEGIN_INFO };
    OXR(xrBeginFrame(s->session, &fbi));

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

        resolve_emulator_feed(s);
        GLuint emu_tex = 0;
        if (s->acquire_ahb) {
            uint64_t seq = 0;
            struct AHardwareBuffer *ahb = s->acquire_ahb(&seq);
            if (ahb) {
                emu_tex = ahb_to_texture(s, ahb);
                if (seq != s->last_seq && (seq % 600) == 0) {
                    LOGI("emulator frame seq %llu",
                         (unsigned long long)seq);
                }
                s->last_seq = seq;
                AHardwareBuffer_release(ahb); /* cache holds its own ref via EGLImage */
            }
        }
        if (emu_tex) {
            glBindFramebuffer(GL_FRAMEBUFFER, s->fbos[idx]);
            glViewport(0, 0, s->quad_w, s->quad_h);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(s->blit_prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, emu_tex);
            glBindVertexArray(s->blit_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            draw_test_pattern(s, s->fbos[idx]);
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
            jboolean dirty =
                (*env)->CallBooleanMethod(env, s->menu_bridge, s->m_isDirty);
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

/* Evaluate menu navigation from the unified pad state (dpad keys, hat, or left
 * stick Y), edge-triggered via nav_latch so one push advances one row. */
static void menu_nav_eval(XrShell *s)
{
    int dir = 0;
    if (s->pad_buttons & GP_DU) dir = -1;
    else if (s->pad_buttons & GP_DD) dir = 1;
    if (dir == 0) {
        int16_t ly = s->pad_axis[AX_LY]; /* up = positive (see on_input) */
        if (ly > 16000) dir = -1;
        else if (ly < -16000) dir = 1;
    }
    if (dir != 0) {
        if (s->nav_latch == 0) {
            menu_move(s, dir);
            s->nav_latch = dir;
        }
    } else {
        s->nav_latch = 0;
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
                if (mask == GP_A) menu_activate(s);
                else if (mask == GP_B) menu_close(s);
                else if (mask == GP_X) menu_toggle_fp(s);
                else if (mask == GP_Y) menu_quit(s);
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
                s->pad_deferred &= ~mask;
                if (s->set_gamepad) {
                    uint16_t fwd = s->pad_buttons & ~s->pad_deferred;
                    s->set_gamepad((uint16_t)(fwd | mask), s->pad_axis, 6);
                    s->set_gamepad(fwd, s->pad_axis, 6);
                }
                return 1;
            }
        }

        if (s->set_gamepad)
            s->set_gamepad((uint16_t)(s->pad_buttons & ~s->pad_deferred),
                           s->pad_axis, 6);
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
        if (s->set_gamepad)
            s->set_gamepad((uint16_t)(s->pad_buttons & ~s->pad_deferred),
                           s->pad_axis, 6);
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

    /* Picker sits a little closer than the game window and faces the user. */
    shell.menu_pos = (XrVector3f){ 0.0f, 0.0f, -1.2f };
    shell.menu_orient = (XrQuaternionf){ 0, 0, 0, 1 };
    shell.menu_size_m = 0.9f;

    LOGI("xr_shell spike starting");
    egl_init(&shell);
    xr_create_instance(&shell);
    xr_create_session(&shell);

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
    LOGI("xr_shell spike exiting");
}
