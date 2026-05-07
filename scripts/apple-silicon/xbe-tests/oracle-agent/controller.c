/*
 * xbox-oracle-agent — synthetic controller state buffer + RPC commands.
 * See controller.h for the architecture overview.
 */
#include "controller.h"
#include "protocol.h"

#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The canonical synthetic-input buffer. Defined here in the agent's
 * BSS so its address is stable for the life of the agent process.
 * After a runxbe chainload the agent's address space is recycled by
 * the kernel; see controller.h for the (future) cross-XBE plan. */
struct oracle_ctrl_buffer g_oracle_ctrl;

/* Wall-clock helper. nxdk's xboxkrnl exports
 * `KeQueryPerformanceCounter` / `KeQueryPerformanceFrequency` as
 * ULONGLONG-returning functions (NOT the LARGE_INTEGER* API the
 * Win32 ABI uses). Convert ticks → microseconds; the result is
 * stamped on the buffer to give external consumers (a future shim or
 * kernel hook) a freshness signal. */
static uint64_t now_us(void)
{
    uint64_t freq = (uint64_t)KeQueryPerformanceFrequency();
    if (freq == 0) return 0;
    uint64_t ctr = (uint64_t)KeQueryPerformanceCounter();
    /* Reduce overflow risk: convert to microseconds via
     * (ctr / freq) * 1e6 + ((ctr % freq) * 1e6) / freq.
     * Xbox's perf-counter frequency is 3.375 MHz so 64-bit math
     * has plenty of headroom even at multi-decade uptime. */
    return ctr * 1000000ull / freq;
}

void oracle_ctrl_init(void)
{
    memset(&g_oracle_ctrl, 0, sizeof(g_oracle_ctrl));
    g_oracle_ctrl.magic = ORACLE_CTRL_MAGIC;
    g_oracle_ctrl.version = ORACLE_CTRL_VERSION;
}

/* ---- name → bit / axis index lookup ----
 *
 * The names match xemu's xemu_input_button_names / xemu_input_axis_names
 * tables (ui/xemu-input.c) so a recorded XEMU_RECORD_INPUT CSV from
 * xemu replays cleanly through this protocol without translation. */
struct kv_button { const char *name; uint16_t bit; };
struct kv_axis   { const char *name; int       index; };

static const struct kv_button s_buttons[] = {
    { "a",            ORACLE_BTN_A          },
    { "b",            ORACLE_BTN_B          },
    { "x",            ORACLE_BTN_X          },
    { "y",            ORACLE_BTN_Y          },
    { "dpad_up",      ORACLE_BTN_DPAD_UP    },
    { "dpad_down",    ORACLE_BTN_DPAD_DOWN  },
    { "dpad_left",    ORACLE_BTN_DPAD_LEFT  },
    { "dpad_right",   ORACLE_BTN_DPAD_RIGHT },
    { "start",        ORACLE_BTN_START      },
    { "back",         ORACLE_BTN_BACK       },
    { "white",        ORACLE_BTN_WHITE      },
    { "black",        ORACLE_BTN_BLACK      },
    { "lstick_btn",   ORACLE_BTN_LSTICK     },
    { "rstick_btn",   ORACLE_BTN_RSTICK     },
    { "guide",        ORACLE_BTN_GUIDE      },
    { NULL,           0                     },
};

enum {
    AXIS_LT = 0, AXIS_RT, AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY,
};
static const struct kv_axis s_axes[] = {
    { "ltrigger", AXIS_LT },
    { "rtrigger", AXIS_RT },
    { "lstick_x", AXIS_LX },
    { "lstick_y", AXIS_LY },
    { "rstick_x", AXIS_RX },
    { "rstick_y", AXIS_RY },
    { NULL,       -1     },
};

/* Strict signed parser for axis values. Accepts optional leading sign
 * and base-10 digits; rejects overflow against int32 range. Returns 0
 * on success, -1 otherwise. */
static int parse_int(const char *s, int32_t *out)
{
    if (!s || !*s) return -1;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (!*s) return -1;
    uint32_t acc = 0;
    while (*s) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t d = (uint32_t)(*s - '0');
        if (acc > (UINT32_MAX - d) / 10) return -1;
        acc = acc * 10 + d;
        s++;
    }
    if (neg) {
        if (acc > 0x80000000u) return -1;
        *out = (int32_t)(-(int64_t)acc);
    } else {
        if (acc > 0x7FFFFFFFu) return -1;
        *out = (int32_t)acc;
    }
    return 0;
}

/* Some args use op_parse_kv_u32 (which only accepts unsigned). For
 * signed axis values we need a separate helper that takes the raw
 * key= token. Returns 0 on success. */
static int parse_kv_int(const char *args, const char *key, int32_t *out)
{
    /* Locate "<key>=" and then parse the value substring (terminate
     * at whitespace). Allocate an on-stack copy just long enough. */
    if (!args || !key) return -1;
    size_t klen = strlen(key);
    const char *p = args;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            char tmp[24];
            size_t i = 0;
            while (*v && *v != ' ' && *v != '\t' && i + 1 < sizeof(tmp)) {
                tmp[i++] = *v++;
            }
            tmp[i] = 0;
            return parse_int(tmp, out);
        }
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return -1;
}

static int parse_port_arg(const char *args, uint32_t *out_port,
                          int allow_missing)
{
    uint32_t p = 0;
    if (op_parse_kv_u32(args, "port", &p) != 0) {
        if (allow_missing) {
            *out_port = (uint32_t)-1;
            return 0;
        }
        return -1;
    }
    if (p >= ORACLE_CTRL_NUM_PORTS) return -2;
    *out_port = p;
    return 0;
}

/* Saturating clamp helpers. Values are pre-validated to int32 range
 * by parse_int above. */
static int16_t clamp_i16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767)  return 32767;
    return (int16_t)v;
}

/* Trigger range matches xemu's `axis[CONTROLLER_AXIS_LTRIG]`: int16
 * 0..32767 (only the positive half is meaningful — triggers don't
 * report negative values). Negative input is clamped to 0. The Xbox
 * XID HID-report u8 0..255 is produced from this by `>> 7` at
 * `hw/xbox/xid.c:108-109` (a future kernel-hook shim does the same). */
static int16_t clamp_trigger(int32_t v)
{
    if (v < 0)     return 0;
    if (v > 32767) return 32767;
    return (int16_t)v;
}

/* Lookup helpers */
static const struct kv_button *find_button(const char *name)
{
    for (const struct kv_button *e = s_buttons; e->name; e++) {
        if (strcmp(name, e->name) == 0) return e;
    }
    return NULL;
}
static const struct kv_axis *find_axis(const char *name)
{
    for (const struct kv_axis *e = s_axes; e->name; e++) {
        if (strcmp(name, e->name) == 0) return e;
    }
    return NULL;
}

/* Mutate one axis on a port-state struct without a switch in the
 * three call sites. */
static void apply_axis(struct oracle_ctrl_port_state *p, int axis, int32_t val)
{
    switch (axis) {
        case AXIS_LT: p->ltrigger = clamp_trigger(val); break;
        case AXIS_RT: p->rtrigger = clamp_trigger(val); break;
        case AXIS_LX: p->lstick_x = clamp_i16(val); break;
        case AXIS_LY: p->lstick_y = clamp_i16(val); break;
        case AXIS_RX: p->rstick_x = clamp_i16(val); break;
        case AXIS_RY: p->rstick_y = clamp_i16(val); break;
        default: break;
    }
}

static void mark_changed(struct oracle_ctrl_port_state *p)
{
    p->seq++;
    p->timestamp_us = now_us();
}

/* ---- RPC handlers ---- */

int cmd_controller_set(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/0);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.set port=N [buttons=0xHHHH] "
                        "[lt=N] [rt=N] [lx=N] [ly=N] [rx=N] [ry=N]");
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];

    int touched = 0;
    uint32_t btns = 0;
    if (op_parse_kv_u32(args, "buttons", &btns) == 0) {
        st->buttons = (uint16_t)(btns & 0xFFFFu);
        touched = 1;
    }
    int32_t v = 0;
    if (parse_kv_int(args, "lt", &v) == 0) { st->ltrigger = clamp_trigger(v); touched = 1; }
    if (parse_kv_int(args, "rt", &v) == 0) { st->rtrigger = clamp_trigger(v); touched = 1; }
    if (parse_kv_int(args, "lx", &v) == 0) { st->lstick_x = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "ly", &v) == 0) { st->lstick_y = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "rx", &v) == 0) { st->rstick_x = clamp_i16(v); touched = 1; }
    if (parse_kv_int(args, "ry", &v) == 0) { st->rstick_y = clamp_i16(v); touched = 1; }

    if (!touched) {
        /* Bare `controller.set port=N` is treated as a `seq+timestamp`
         * heartbeat — useful for the replay tool to bump the sequence
         * counter without changing state. */
    }
    mark_changed(st);
    op_send_okf(c, "port=%u seq=%u buttons=0x%04x lt=%d rt=%d "
                   "lx=%d ly=%d rx=%d ry=%d",
                (unsigned)port, (unsigned)st->seq,
                (unsigned)st->buttons,
                (int)st->ltrigger, (int)st->rtrigger,
                (int)st->lstick_x, (int)st->lstick_y,
                (int)st->rstick_x, (int)st->rstick_y);
    return 0;
}

int cmd_controller_button(struct netconn *c, const char *args)
{
    uint32_t port = 0, val = 0;
    char name[24];
    if (parse_port_arg(args, &port, /*allow_missing=*/0) < 0 ||
        op_parse_kv_str(args, "name", name, sizeof(name)) != 0 ||
        op_parse_kv_u32(args, "value", &val) != 0) {
        op_send_errf(c, "usage: controller.button port=N name=<id> value=<0|1>");
        return 0;
    }
    const struct kv_button *b = find_button(name);
    if (!b) {
        op_send_errf(c, "unknown button '%s'", name);
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    if (val) st->buttons |= b->bit;
    else     st->buttons &= (uint16_t)~b->bit;
    mark_changed(st);
    op_send_okf(c, "port=%u %s=%u buttons=0x%04x seq=%u",
                (unsigned)port, name, (unsigned)(val ? 1 : 0),
                (unsigned)st->buttons, (unsigned)st->seq);
    return 0;
}

int cmd_controller_axis(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int32_t  val = 0;
    char name[24];
    if (parse_port_arg(args, &port, /*allow_missing=*/0) < 0 ||
        op_parse_kv_str(args, "name", name, sizeof(name)) != 0 ||
        parse_kv_int(args, "value", &val) != 0) {
        op_send_errf(c, "usage: controller.axis port=N name=<id> value=<int>");
        return 0;
    }
    const struct kv_axis *a = find_axis(name);
    if (!a) {
        op_send_errf(c, "unknown axis '%s'", name);
        return 0;
    }
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    apply_axis(st, a->index, val);
    mark_changed(st);
    op_send_okf(c, "port=%u %s=%d seq=%u",
                (unsigned)port, name, (int)val, (unsigned)st->seq);
    return 0;
}

static void emit_port_lines(struct netconn *c, uint32_t port)
{
    struct oracle_ctrl_port_state *st = &g_oracle_ctrl.port[port];
    char buf[160];
    snprintf(buf, sizeof(buf),
             "port.%u.buttons=0x%04x", (unsigned)port,
             (unsigned)st->buttons);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.triggers lt=%d rt=%d",
             (unsigned)port, (int)st->ltrigger,
             (int)st->rtrigger);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.lstick x=%d y=%d",
             (unsigned)port, (int)st->lstick_x, (int)st->lstick_y);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.rstick x=%d y=%d",
             (unsigned)port, (int)st->rstick_x, (int)st->rstick_y);
    op_send_line(c, buf);
    snprintf(buf, sizeof(buf),
             "port.%u.seq=%u", (unsigned)port, (unsigned)st->seq);
    op_send_line(c, buf);
    /* timestamp_us is KeQueryPerformanceCounter-derived microseconds
     * since the agent booted (not wall-clock); useful as a freshness
     * indicator for downstream consumers that read the buffer
     * directly. Print as %llu since `unsigned long` is 32-bit on the
     * Xbox i386 ABI but timestamp_us is uint64_t. */
    snprintf(buf, sizeof(buf),
             "port.%u.timestamp_us=%llu",
             (unsigned)port,
             (unsigned long long)st->timestamp_us);
    op_send_line(c, buf);
}

int cmd_controller_get(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/1);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.get [port=N]");
        return 0;
    }
    op_send_text_begin(c, 0);
    if (port == (uint32_t)-1) {
        for (uint32_t p = 0; p < ORACLE_CTRL_NUM_PORTS; p++) {
            emit_port_lines(c, p);
        }
    } else {
        emit_port_lines(c, port);
    }
    op_send_text_end(c);
    return 0;
}

int cmd_controller_clear(struct netconn *c, const char *args)
{
    uint32_t port = 0;
    int rc = parse_port_arg(args, &port, /*allow_missing=*/1);
    if (rc < 0) {
        op_send_errf(c, "usage: controller.clear [port=N]");
        return 0;
    }
    if (port == (uint32_t)-1) {
        for (uint32_t p = 0; p < ORACLE_CTRL_NUM_PORTS; p++) {
            uint32_t seq = g_oracle_ctrl.port[p].seq;
            memset(&g_oracle_ctrl.port[p], 0,
                   sizeof(g_oracle_ctrl.port[p]));
            g_oracle_ctrl.port[p].seq = seq;
            mark_changed(&g_oracle_ctrl.port[p]);
        }
        op_send_okf(c, "all ports cleared");
    } else {
        uint32_t seq = g_oracle_ctrl.port[port].seq;
        memset(&g_oracle_ctrl.port[port], 0,
               sizeof(g_oracle_ctrl.port[port]));
        g_oracle_ctrl.port[port].seq = seq;
        mark_changed(&g_oracle_ctrl.port[port]);
        op_send_okf(c, "port %u cleared", (unsigned)port);
    }
    return 0;
}

int cmd_controller_buffer_info(struct netconn *c, const char *args)
{
    (void)args;
    op_send_okf(c,
                "addr=0x%08lx size=%u magic=0x%08lx version=%u "
                "ports=%u port_state_size=%u",
                (unsigned long)(uintptr_t)&g_oracle_ctrl,
                (unsigned)sizeof(g_oracle_ctrl),
                (unsigned long)g_oracle_ctrl.magic,
                (unsigned)g_oracle_ctrl.version,
                (unsigned)ORACLE_CTRL_NUM_PORTS,
                (unsigned)sizeof(struct oracle_ctrl_port_state));
    return 0;
}
