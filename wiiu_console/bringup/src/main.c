/*
 * The smallest thing that can be said to work on this console.
 *
 * It brings up the two screens and paints them black. Nothing else: no
 * network, no decoder, no touch, no keyboard, no settings. It exists
 * because the full client has now failed twice in ways that could have
 * been anywhere in that stack, and there was no baseline to measure
 * against.
 *
 * What each outcome tells us:
 *
 *   both screens go black, and MINUS returns to the Wii U menu
 *       -- launching, OSScreen, ProcUI and the exit path are all fine,
 *          and every remaining problem is above them.
 *
 *   the screens go black and MINUS does nothing
 *       -- VPADRead is not giving this program input at all, which
 *          would explain the touch as well as the buttons.
 *
 *   nothing happens, or it returns to the menu by itself
 *       -- the problem is launching or ProcUI, below everything the
 *          client does.
 *
 * Deliberately not a black PICTURE from the network: black is what an
 * empty framebuffer already is, so this proves the framebuffer is ours
 * and being shown.
 */
#include <malloc.h>
#include <stdlib.h>

#include <coreinit/screen.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/proc.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBProcInit();
    VPADInit();
    OSScreenInit();

    void *tv = memalign(0x100, OSScreenGetBufferSizeEx(SCREEN_TV));
    void *drc = memalign(0x100, OSScreenGetBufferSizeEx(SCREEN_DRC));
    if (!tv || !drc) {
        WHBProcShutdown();
        return 1;
    }
    OSScreenSetBufferEx(SCREEN_TV, tv);
    OSScreenSetBufferEx(SCREEN_DRC, drc);
    OSScreenEnableEx(SCREEN_TV, 1);
    OSScreenEnableEx(SCREEN_DRC, 1);

    while (WHBProcIsRunning()) {
        VPADStatus vpad;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        if (err == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_MINUS)) {
            break;
        }

        OSScreenClearBufferEx(SCREEN_TV, 0x00000000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);
        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    OSScreenShutdown();
    free(tv);
    free(drc);

    /* Returning from main() does not send this console anywhere; it has
     * to be told. Same lesson as the client. */
    SYSLaunchMenu();
    while (WHBProcIsRunning()) {
    }
    WHBProcShutdown();
    return 0;
}
