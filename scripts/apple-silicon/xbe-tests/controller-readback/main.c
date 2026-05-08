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
#include <stdbool.h>
#include <stdio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

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
