/*
 * controller-readback — Tier-2 retail-input preflight.
 *
 * Unlike controller-roundtrip, this does not read the oracle agent's
 * persistent synthetic buffer. It asks SDL_GameController what the normal
 * Xbox controller stack sees after chainload, writes a key=value report to
 * D:, drops a done marker, and reboots. That makes it a safe proof point for
 * future retail-game input injection work: first prove the read side, then
 * hook the producer.
 */
#include <SDL.h>
#include <hal/debug.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "../lib/xbed_tier2_hook.h"

#define REPORT_TXT "D:\\controller-readback.txt"
#define DONE_TXT   "D:\\controller-readback-done.txt"

static SDL_GameController *open_first_controller(void)
{
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController *pad = SDL_GameControllerOpen(i);
            if (pad) {
                return pad;
            }
        }
    }
    return NULL;
}

static int axis(SDL_GameController *pad, SDL_GameControllerAxis a)
{
    return pad ? (int)SDL_GameControllerGetAxis(pad, a) : 0;
}

static int button(SDL_GameController *pad, SDL_GameControllerButton b)
{
    return pad ? (int)SDL_GameControllerGetButton(pad, b) : 0;
}

static int ensure_e_drive_mounted(void)
{
    if (nxIsDriveMounted('E')) return 1;
    if (nxMountDrive('E', "\\Device\\Harddisk0\\Partition1")) return 1;
    return 0;
}

static int parse_hex(const char *s, uintptr_t *out)
{
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return -1;
    s += 2;
    uintptr_t acc = 0;
    int digits = 0;
    while (*s && *s != '\r' && *s != '\n') {
        int v = -1;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = 10 + (*s - 'a');
        else if (*s >= 'A' && *s <= 'F') v = 10 + (*s - 'A');
        else return -1;
        if (digits >= (int)(sizeof(uintptr_t) * 2)) return -1;
        acc = (acc << 4) | (uintptr_t)v;
        digits++;
        s++;
    }
    if (digits == 0) return -1;
    *out = acc;
    return 0;
}

static struct xbed_tier2_hook_page *tier2_attach(uintptr_t *out_phys)
{
    if (!ensure_e_drive_mounted()) return NULL;
    FILE *fp = fopen(XBED_TIER2_HOOK_ANCHOR_PATH, "rb");
    if (!fp) return NULL;
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return NULL;

    char *line[4] = {0};
    int li = 0;
    char *p = buf;
    line[li++] = p;
    while (*p && li < 4) {
        if (*p == '\n') {
            *p = 0;
            if (p[1] != 0) line[li++] = p + 1;
        }
        p++;
    }
    if (li < 4 || strncmp(line[0], "XT2H", 4) != 0) return NULL;

    uintptr_t phys = 0, recorded_virt = 0, recorded_size = 0;
    if (parse_hex(line[1], &phys) != 0 ||
        parse_hex(line[2], &recorded_virt) != 0 ||
        parse_hex(line[3], &recorded_size) != 0) {
        return NULL;
    }
    if (phys == 0 || phys >= 0x04000000u ||
        recorded_size < sizeof(struct xbed_tier2_hook_page) ||
        recorded_size > XBED_TIER2_HOOK_PAGE_SIZE) {
        return NULL;
    }

    uintptr_t virt = phys | 0x80000000u;
    struct xbed_tier2_hook_page *page =
        (struct xbed_tier2_hook_page *)virt;
    if ((uintptr_t)MmGetPhysicalAddress((PVOID)page) != phys) return NULL;
    if (page->magic != XBED_TIER2_HOOK_MAGIC ||
        page->version != XBED_TIER2_HOOK_VERSION ||
        page->size < sizeof(*page)) {
        return NULL;
    }
    *out_phys = phys;
    (void)recorded_virt;
    return page;
}

static void write_report(SDL_GameController *pad, int frames, const char *status)
{
    FILE *f = fopen(REPORT_TXT, "wb");
    if (!f) {
        debugPrint("controller-readback: failed to open report\n");
        return;
    }

    fprintf(f, "status=%s\n", status);
    fprintf(f, "frames=%d\n", frames);
    fprintf(f, "has_controller=%d\n", pad ? 1 : 0);
    uintptr_t tier2_phys = 0;
    struct xbed_tier2_hook_page *tier2 = tier2_attach(&tier2_phys);
    uint32_t slot_rva =
        *(volatile uint32_t *)(uintptr_t)XBED_TIER2_KE_RAISE_SLOT_VA;
    fprintf(f, "tier2.status=%s\n", tier2 ? "ok" : "absent");
    fprintf(f, "tier2.anchor=%s\n", XBED_TIER2_HOOK_ANCHOR_PATH);
    fprintf(f, "tier2.slot_rva=0x%08lx\n", (unsigned long)slot_rva);
    fprintf(f, "tier2.expected_original_rva=0x%08lx\n",
            (unsigned long)XBED_TIER2_KE_RAISE_ORIGINAL_RVA);
    if (tier2) {
        fprintf(f, "tier2.page_phys=0x%08lx\n",
                (unsigned long)tier2_phys);
        fprintf(f, "tier2.page_virt=0x%08lx\n",
                (unsigned long)(uintptr_t)tier2);
        fprintf(f, "tier2.hook_rva=0x%08lx\n",
                (unsigned long)tier2->hook_rva);
        fprintf(f, "tier2.code_size=%lu\n",
                (unsigned long)tier2->code_size);
        fprintf(f, "tier2.flags=0x%08lx\n",
                (unsigned long)tier2->flags);
        fprintf(f, "tier2.installed=%d\n",
                slot_rva == tier2->hook_rva ? 1 : 0);
        fprintf(f, "tier2.calls=%lu\n", (unsigned long)tier2->calls);
        fprintf(f, "tier2.installs=%lu\n",
                (unsigned long)tier2->installs);
        fprintf(f, "tier2.uninstalls=%lu\n",
                (unsigned long)tier2->uninstalls);
    }
    if (pad) {
        fprintf(f, "player_index=%d\n", SDL_GameControllerGetPlayerIndex(pad));
        fprintf(f, "vendor=0x%04x\n", SDL_GameControllerGetVendor(pad));
        fprintf(f, "product=0x%04x\n", SDL_GameControllerGetProduct(pad));
        fprintf(f, "axis.leftx=%d\n", axis(pad, SDL_CONTROLLER_AXIS_LEFTX));
        fprintf(f, "axis.lefty=%d\n", axis(pad, SDL_CONTROLLER_AXIS_LEFTY));
        fprintf(f, "axis.rightx=%d\n", axis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
        fprintf(f, "axis.righty=%d\n", axis(pad, SDL_CONTROLLER_AXIS_RIGHTY));
        fprintf(f, "axis.lefttrigger=%d\n", axis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        fprintf(f, "axis.righttrigger=%d\n", axis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        fprintf(f, "button.a=%d\n", button(pad, SDL_CONTROLLER_BUTTON_A));
        fprintf(f, "button.b=%d\n", button(pad, SDL_CONTROLLER_BUTTON_B));
        fprintf(f, "button.x=%d\n", button(pad, SDL_CONTROLLER_BUTTON_X));
        fprintf(f, "button.y=%d\n", button(pad, SDL_CONTROLLER_BUTTON_Y));
        fprintf(f, "button.back=%d\n", button(pad, SDL_CONTROLLER_BUTTON_BACK));
        fprintf(f, "button.start=%d\n", button(pad, SDL_CONTROLLER_BUTTON_START));
        fprintf(f, "button.white=%d\n", button(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
        fprintf(f, "button.black=%d\n", button(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
        fprintf(f, "button.dpad_up=%d\n", button(pad, SDL_CONTROLLER_BUTTON_DPAD_UP));
        fprintf(f, "button.dpad_down=%d\n", button(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
        fprintf(f, "button.dpad_left=%d\n", button(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
        fprintf(f, "button.dpad_right=%d\n", button(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
        fprintf(f, "button.leftstick=%d\n", button(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK));
        fprintf(f, "button.rightstick=%d\n", button(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK));
    }
    fflush(f);
    fclose(f);

    FILE *done = fopen(DONE_TXT, "wb");
    if (done) {
        fprintf(done, "done\n");
        fclose(done);
    }
}

int main(void)
{
    debugPrint("controller-readback v0.1\n");

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        FILE *f = fopen(REPORT_TXT, "wb");
        if (f) {
            fprintf(f, "status=sdl_init_failed\n");
            fprintf(f, "error=%s\n", SDL_GetError());
            fclose(f);
        }
        Sleep(1000);
        HalReturnToFirmware(HalRebootRoutine);
        return 1;
    }

    SDL_GameController *pad = NULL;
    SDL_Event e;
    int frames = 0;

    for (frames = 0; frames < 300; frames++) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                pad = SDL_GameControllerOpen(e.cdevice.which);
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
                SDL_GameController *removed =
                    SDL_GameControllerFromInstanceID(e.cdevice.which);
                if (removed == pad) {
                    SDL_GameControllerClose(pad);
                    pad = NULL;
                }
            }
        }
        if (!pad) {
            pad = open_first_controller();
        }
        SDL_GameControllerUpdate();
        Sleep(16);
    }

    write_report(pad, frames, pad ? "ok" : "no_controller");
    if (pad) {
        SDL_GameControllerClose(pad);
    }
    SDL_Quit();
    Sleep(1000);
    HalReturnToFirmware(HalRebootRoutine);
    return 0;
}
