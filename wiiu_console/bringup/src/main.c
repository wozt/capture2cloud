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
#include <stdio.h>
#include <stdlib.h>

#include <coreinit/screen.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include "proc.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    proc_init();
    WHBLogUdpInit();
    WHBLogPrintf("bringup: starting");
    VPADInit();
    OSScreenInit();

    void *tv = memalign(0x100, OSScreenGetBufferSizeEx(SCREEN_TV));
    void *drc = memalign(0x100, OSScreenGetBufferSizeEx(SCREEN_DRC));
    if (!tv || !drc) {
        proc_shutdown();
        return 1;
    }
    OSScreenSetBufferEx(SCREEN_TV, tv);
    OSScreenSetBufferEx(SCREEN_DRC, drc);
    OSScreenEnableEx(SCREEN_TV, 1);
    OSScreenEnableEx(SCREEN_DRC, 1);

    /*
     * Black, plus the one thing that cannot be established by being
     * described over a chat: what VPADRead is actually handing this
     * program.
     *
     * Every number on screen is raw. Nothing here interprets, scales or
     * decides -- the point is to see what arrives, not what I think
     * arrives, having now been wrong about that twice.
     */
    while (proc_running()) {
        VPADStatus vpad;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        if (err == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_MINUS)) {
    /*
     * How a program on this console asks to leave.
     *
     * The first version broke out of the loop, called SYSLaunchMenu()
     * and then spun on `while (proc_running()) {}` waiting to be
     * taken down. Nothing had told ProcUI anything, so that condition
     * stayed true and the spin never ended -- two black screens and a
     * forced power-off, which is exactly what came back from the sofa.
     *
     * WHBProcStopRunning() is the switch. SYSLaunchMenu() says where to
     * go next, that says to stop going, and the ordinary loop condition
     * does the rest.
     */
            proc_stop();
        }

        OSScreenClearBufferEx(SCREEN_TV, 0x00000000);
        OSScreenClearBufferEx(SCREEN_DRC, 0x00000000);

        char line[128];
        snprintf(line, sizeof(line), "VPADRead err %d", (int)err);
        OSScreenPutFontEx(SCREEN_TV, 2, 2, line);
        OSScreenPutFontEx(SCREEN_DRC, 1, 2, line);

        if (err == VPAD_READ_SUCCESS) {
            snprintf(line, sizeof(line), "hold %08X  trig %08X", (unsigned)vpad.hold,
                     (unsigned)vpad.trigger);
            OSScreenPutFontEx(SCREEN_TV, 2, 3, line);
            OSScreenPutFontEx(SCREEN_DRC, 1, 3, line);

            snprintf(line, sizeof(line), "tpNormal   %4u,%4u t=%u v=%u", vpad.tpNormal.x,
                     vpad.tpNormal.y, vpad.tpNormal.touched, vpad.tpNormal.validity);
            OSScreenPutFontEx(SCREEN_TV, 2, 5, line);
            OSScreenPutFontEx(SCREEN_DRC, 1, 5, line);

            snprintf(line, sizeof(line), "tpFiltered1 %4u,%4u t=%u", vpad.tpFiltered1.x,
                     vpad.tpFiltered1.y, vpad.tpFiltered1.touched);
            OSScreenPutFontEx(SCREEN_TV, 2, 6, line);
            OSScreenPutFontEx(SCREEN_DRC, 1, 6, line);

            /* Both calibrations, side by side, from the filtered point:
             * one of these is the coordinate space the panel really
             * speaks, and this is how we find out which. */
            VPADTouchData plain, ex854;
            VPADGetTPCalibratedPoint(VPAD_CHAN_0, &plain, &vpad.tpFiltered1);
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &ex854, &vpad.tpFiltered1);
            snprintf(line, sizeof(line), "calib plain %4u,%4u   ex854 %4u,%4u", plain.x, plain.y,
                     ex854.x, ex854.y);
            OSScreenPutFontEx(SCREEN_TV, 2, 7, line);
            OSScreenPutFontEx(SCREEN_DRC, 1, 7, line);

            /* And a mark where the plain calibration says the finger
             * is, scaled as 1280x720 -> this panel. If the mark lands
             * under the finger, that assumption is right. */
            if (vpad.tpNormal.touched) {
                const int mx = (int)plain.x * 854 / 1280;
                const int my = (int)plain.y * 480 / 720;
                for (int dy = -10; dy <= 10; dy++) {
                    for (int dx = -10; dx <= 10; dx++) {
                        const int px = mx + dx, py = my + dy;
                        if (px >= 0 && px < 854 && py >= 0 && py < 480) {
                            OSScreenPutPixelEx(SCREEN_DRC, (uint32_t)px, (uint32_t)py,
                                               0xFFFFFFFF);
                        }
                    }
                }
            }
        }

        OSScreenPutFontEx(SCREEN_TV, 2, 10, "A = GX2 red test, MINUS quits");
        OSScreenPutFontEx(SCREEN_DRC, 1, 10, "A = GX2 red test, MINUS quits");

        /*
         * The one experiment that separates the two remaining stories
         * about the keyboard.
         *
         * Its logs say it is created and running its loop, so setup is
         * fine and only the picture is missing. Either handing the
         * display from OSScreen to GX2 does not work in this process at
         * all, or it works and swkbd in particular draws nothing.
         *
         * So: do the handover with nothing in it but a flat red. If the
         * screens go red, the handover is sound and swkbd is the
         * problem. If they stay black, the handover is the problem and
         * no amount of work on swkbd would ever have helped.
         */
        if (err == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_A)) {
            WHBLogPrintf("gx2 test: OSScreenShutdown");
            OSScreenShutdown();
            WHBLogPrintf("gx2 test: WHBGfxInit -> %d", (int)WHBGfxInit());
            for (int frame = 0; frame < 180 && proc_running(); frame++) {
                WHBGfxBeginRender();
                WHBGfxBeginRenderTV();
                WHBGfxClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                WHBGfxFinishRenderTV();
                WHBGfxBeginRenderDRC();
                WHBGfxClearColor(1.0f, 0.0f, 0.0f, 1.0f);
                WHBGfxFinishRenderDRC();
                WHBGfxFinishRender();
            }
            WHBLogPrintf("gx2 test: WHBGfxShutdown");
            WHBGfxShutdown();
            OSScreenInit();
            OSScreenSetBufferEx(SCREEN_TV, tv);
            OSScreenSetBufferEx(SCREEN_DRC, drc);
            OSScreenEnableEx(SCREEN_TV, 1);
            OSScreenEnableEx(SCREEN_DRC, 1);
            WHBLogPrintf("gx2 test: back on OSScreen");
        }

        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);
    }

    WHBLogPrintf("bringup: done");
    WHBLogUdpDeinit();
    OSScreenShutdown();
    free(tv);
    free(drc);

    proc_shutdown();
    return 0;
}
