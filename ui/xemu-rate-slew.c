/*
 * Apple Silicon performance fork: emulation-rate slewing implementation.
 *
 * See include/qemu/xemu-rate-slew.h for the API contract. This file
 * holds the lightweight state and the env-var parse. The actual
 * vblank_interval_ns is mutated through an extern declaration matching
 * ui/xemu.c.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"

#include <SDL3/SDL.h>

#include "qemu/xemu-rate-slew.h"

/* ui/xemu.c — global vblank interval used by both timer paths. */
extern uint64_t vblank_interval_ns;

#define XEMU_VBLANK_DEFAULT_NS 16666666ULL

static bool s_initialized;
static bool s_env_enabled;
static double s_host_hz;
static double s_ratio;
static bool s_active;

static bool parse_env_flag(const char *name, bool *out)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return false;
    }
    *out = (v[0] != '0');
    return true;
}

static void apply_locked(double host_hz)
{
    s_host_hz = host_hz;

    if (host_hz <= 0.0) {
        s_ratio = 0.0;
        s_active = false;
        qatomic_set_u64(&vblank_interval_ns, XEMU_VBLANK_DEFAULT_NS);
        return;
    }

    s_ratio = host_hz / 60.0;

    if (s_env_enabled && s_ratio >= 0.95 && s_ratio <= 1.05) {
        /* New interval = baseline 16,666,666 ns × (60 / host_hz). At
         * host 59.94 Hz that's ~16,683,317 ns; at host 60.05 Hz that's
         * ~16,652,789 ns. Both stay within ±5 % of the baseline so
         * downstream timers (RDTSC scaling, audio sample math) see
         * sub-perceptual drift only. */
        double new_ns = 16666666.0 * (60.0 / host_hz);
        uint64_t v = (uint64_t)(new_ns + 0.5);
        qatomic_set_u64(&vblank_interval_ns, v);
        s_active = true;
    } else {
        qatomic_set_u64(&vblank_interval_ns, XEMU_VBLANK_DEFAULT_NS);
        s_active = false;
    }
}

static double query_host_hz(XemuRateSlewWindow *window)
{
    if (window == NULL) {
        return 0.0;
    }
    SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) {
        return 0.0;
    }
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    if (mode == NULL) {
        return 0.0;
    }
    /* SDL_DisplayMode::refresh_rate is a float in SDL3. Returns 0 when
     * SDL cannot determine the rate. */
    return (double)mode->refresh_rate;
}

bool xemu_rate_slew_init(XemuRateSlewWindow *window)
{
    if (!s_initialized) {
        bool env = false;
        /* Accept either spelling. XEMU_RATE_SLEW wins over the GL-named
         * alias when both are set. */
        if (!parse_env_flag("XEMU_RATE_SLEW", &env)) {
            parse_env_flag("XEMU_GL_RATE_SLEW", &env);
        }
        s_env_enabled = env;
        s_initialized = true;
    }

    double hz = query_host_hz(window);
    apply_locked(hz);

    fprintf(stderr,
            "xemu-rate-slew: enabled=%d host_hz=%.4f ratio=%.6f "
            "active=%d vblank_interval_ns=%llu\n",
            (int)s_env_enabled,
            s_host_hz,
            s_ratio,
            (int)s_active,
            (unsigned long long)qatomic_read_u64(&vblank_interval_ns));

    return s_active;
}

void xemu_rate_slew_update(XemuRateSlewWindow *window)
{
    if (!s_initialized) {
        return;
    }
    double hz = query_host_hz(window);
    if (hz == s_host_hz) {
        return;
    }
    apply_locked(hz);
    fprintf(stderr,
            "xemu-rate-slew: host_hz changed -> %.4f ratio=%.6f "
            "active=%d vblank_interval_ns=%llu\n",
            s_host_hz,
            s_ratio,
            (int)s_active,
            (unsigned long long)qatomic_read_u64(&vblank_interval_ns));
}

double xemu_rate_slew_current_ratio(void)
{
    return s_ratio;
}

bool xemu_rate_slew_is_active(void)
{
    return s_active;
}

double xemu_rate_slew_host_hz(void)
{
    return s_host_hz;
}

void xemu_rate_slew_emit(FILE *out)
{
    if (out == NULL || !s_initialized) {
        return;
    }
    /* Emit unconditionally once initialized: the user wants to see
     * "slew is configured but disabled" too. */
    uint64_t ratio_e6 = (uint64_t)(s_ratio * 1000000.0 + 0.5);
    fprintf(out,
            " RATE_SLEW_RATIO_E6=%llu RATE_SLEW_ACTIVE=%d",
            (unsigned long long)ratio_e6,
            s_active ? 1 : 0);
}
