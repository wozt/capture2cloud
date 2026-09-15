/*
 * SDL2 on this console, proven the same way GX2 was.
 *
 * Established by the two probes before this one: OSScreen works, GX2
 * works, and GX2 shows nothing at all if OSScreen was ever up. So the
 * client has to be a GX2 program from its first line -- which is where
 * the keyboard, the HOME overlay and the video decoder all want it.
 *
 * SDL2's Wii U renderer IS GX2, and it brings a font, touch and drawing
 * without a shader assembler, which this toolchain does not have. This
 * checks that before anything is built on it: red for five seconds,
 * then green, then a white square that follows a finger.
 *
 * Moonlight's own font.c would have done the text, and is not used: it
 * is GPLv3, and copying it would put capture2cloud under GPLv3 too.
 * That is not a decision to make on somebody's behalf.
 */
#include <SDL2/SDL.h>

#include <whb/log.h>
#include <whb/log_udp.h>

#include "proc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBLogUdpInit();
    WHBLogPrintf("sdl2only: starting");
    proc_init();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        WHBLogPrintf("sdl2only: SDL_Init failed: %s", SDL_GetError());
        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }
    WHBLogPrintf("sdl2only: SDL_Init ok, %d joysticks, %d touch devices",
                 SDL_NumJoysticks(), SDL_GetNumTouchDevices());
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        WHBLogPrintf("  joystick %d: %s", i, j ? SDL_JoystickName(j) : "?");
    }

    SDL_Window *window = SDL_CreateWindow("capture2cloud", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, 1280, 720,
                                          SDL_WINDOW_SHOWN);
    if (!window) {
        WHBLogPrintf("sdl2only: no window: %s", SDL_GetError());
        SDL_Quit();
        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        WHBLogPrintf("sdl2only: no renderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        proc_shutdown();
        WHBLogUdpDeinit();
        return 1;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0) {
        WHBLogPrintf("sdl2only: renderer '%s', accelerated=%d", info.name ? info.name : "?",
                     (info.flags & SDL_RENDERER_ACCELERATED) ? 1 : 0);
    }

    int frame = 0;
    int touch_x = -1, touch_y = -1;
    int running = 1;
    while (running && proc_running()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            /*
             * Every event type, logged.
             *
             * SDL_FINGERDOWN produced nothing at all on the first run,
             * and ports differ about whether a touch panel arrives as a
             * finger or as a mouse. Guessing which cost two rounds with
             * the raw API already; this says what actually comes.
             */
            switch (e.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
                touch_x = (int)(e.tfinger.x * 1280.0f);
                touch_y = (int)(e.tfinger.y * 720.0f);
                WHBLogPrintf("event: FINGER %d at %d,%d (norm %.3f,%.3f)", (int)e.type,
                             touch_x, touch_y, e.tfinger.x, e.tfinger.y);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEMOTION:
                touch_x = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                touch_y = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                WHBLogPrintf("event: MOUSE %d at %d,%d", (int)e.type, touch_x, touch_y);
                break;
            case SDL_JOYBUTTONDOWN:
                WHBLogPrintf("event: JOYBUTTON %d", (int)e.jbutton.button);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                WHBLogPrintf("event: PADBUTTON %d", (int)e.cbutton.button);
                break;
            default:
                if (e.type != SDL_JOYAXISMOTION && e.type != SDL_CONTROLLERAXISMOTION) {
                    WHBLogPrintf("event: type %u", (unsigned)e.type);
                }
                break;
            }
        }

        const int red = frame < 300;
        SDL_SetRenderDrawColor(renderer, red ? 255 : 0, red ? 0 : 255, 0, 255);
        SDL_RenderClear(renderer);

        if (touch_x >= 0) {
            SDL_Rect r = { touch_x - 20, touch_y - 20, 40, 40 };
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &r);
        }
        SDL_RenderPresent(renderer);

        if (++frame % 60 == 0) {
            WHBLogPrintf("sdl2only: %d frames (%s)", frame, red ? "red" : "green");
        }
    }

    WHBLogPrintf("sdl2only: done after %d frames", frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    proc_shutdown();
    WHBLogUdpDeinit();
    return 0;
}
