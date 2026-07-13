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
    uint64_t last_seq;
    /* Per-AHB EGLImage/texture cache (AHBs are a small stable ring). */
    struct {
        struct AHardwareBuffer *ahb;
        EGLImageKHR image;
        GLuint tex;
    } ahb_cache[8];
    GLuint blit_prog;
    GLuint blit_vao;
} XrShell;

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
    if (s->acquire_ahb) {
        return;
    }
    void *h = dlopen("libxemu.so", RTLD_NOLOAD | RTLD_LAZY);
    if (!h) {
        return;
    }
    s->acquire_ahb = (struct AHardwareBuffer *(*)(uint64_t *))
        dlsym(h, "xemu_xr_acquire_display_ahb");
    if (s->acquire_ahb) {
        LOGI("emulator frame feed resolved");
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
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    s->blit_prog = compile_prog(BLIT_VS, BLIT_FS);
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

    XrCompositionLayerQuad quad = {
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = s->local_space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = { .swapchain = s->swapchain,
                      .imageRect = { .offset = { 0, 0 },
                                     .extent = { s->quad_w, s->quad_h } } },
        .pose = { .orientation = { 0, 0, 0, 1 },
                  .position = { 0.0f, 0.0f, -1.5f } },
        .size = { 1.6f, 1.2f },
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
    }

    XrCompositionLayerPassthroughFB pt_layer = {
        .type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
        .layerHandle = s->passthrough_layer,
    };

    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nlayers = 0;
    if (s->have_passthrough) {
        layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&pt_layer;
    }
    layers[nlayers++] = (const XrCompositionLayerBaseHeader *)&quad;

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

/* Decisive input experiment: do gamepad events reach the NativeActivity
 * while immersive? Every key/motion event is logged with its source. */
static int32_t on_input(struct android_app *app, AInputEvent *event)
{
    int32_t type = AInputEvent_getType(event);
    int32_t src = AInputEvent_getSource(event);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        LOGI("INPUT key code=%d source=0x%x action=%d",
             AKeyEvent_getKeyCode(event), src, AKeyEvent_getAction(event));
        return 0; /* don't consume; observe only */
    }
    if (type == AINPUT_EVENT_TYPE_MOTION &&
        (src & AINPUT_SOURCE_JOYSTICK)) {
        LOGI("INPUT joystick motion source=0x%x lx=%.2f",
             src, AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0));
        return 0;
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
    LOGI("xr_shell spike exiting");
}
