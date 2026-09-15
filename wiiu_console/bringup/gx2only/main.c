/*
 * GX2 on its own, having never touched OSScreen.
 *
 * The previous probe established that after OSScreen has been up,
 * handing the display to GX2 leaves a blank screen: WHBGfxInit returns
 * 1, the render loop runs its frames without blocking, shutdown
 * completes, and nothing is ever seen. That kills the keyboard, which
 * draws with GX2, and it kills the plan the client was built on.
 *
 * What it does not say is whether GX2 works here AT ALL. If this paints
 * red, the rule is simply "do not mix" and the client should be GX2
 * from its first line -- which is where the keyboard, the HOME overlay
 * and the video decoder all want it anyway. If this is black too, GX2
 * is unavailable in this environment and the whole approach has to
 * change.
 *
 * Red, then green after five seconds, so a still image cannot be
 * confused with a frozen one.
 */
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include "proc.h"
#include <sysapp/launch.h>
#include <vpad/input.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    proc_init();
    WHBLogUdpInit();
    WHBLogPrintf("gx2only: starting, no OSScreen at any point");
    VPADInit();

    const int ok = (int)WHBGfxInit();
    WHBLogPrintf("gx2only: WHBGfxInit -> %d", ok);
    if (!ok) {
        WHBLogUdpDeinit();
        proc_shutdown();
        return 1;
    }

    int frame = 0;
    while (proc_running()) {
        VPADStatus vpad;
        VPADReadError err;
        VPADRead(VPAD_CHAN_0, &vpad, 1, &err);
        if (err == VPAD_READ_SUCCESS && (vpad.trigger & VPAD_BUTTON_MINUS)) {
            proc_stop();
        }

        /* Red for the first five seconds, then green: a picture that
         * changes cannot be mistaken for a frozen one. */
        const float r = (frame < 300) ? 1.0f : 0.0f;
        const float g = (frame < 300) ? 0.0f : 1.0f;

        WHBGfxBeginRender();
        WHBGfxBeginRenderTV();
        WHBGfxClearColor(r, g, 0.0f, 1.0f);
        WHBGfxFinishRenderTV();
        WHBGfxBeginRenderDRC();
        WHBGfxClearColor(r, g, 0.0f, 1.0f);
        WHBGfxFinishRenderDRC();
        WHBGfxFinishRender();

        if (++frame % 60 == 0) {
            WHBLogPrintf("gx2only: %d frames drawn (%s)", frame,
                         frame < 300 ? "red" : "green");
        }
    }

    WHBLogPrintf("gx2only: done after %d frames", frame);
    WHBGfxShutdown();
    WHBLogUdpDeinit();
    proc_shutdown();
    return 0;
}
