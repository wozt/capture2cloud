#include "proc.h"

#include <coreinit/foreground.h>
#include <proc_ui/procui.h>
#include <whb/log.h>

/*
 * This follows the lifecycle proven on real hardware by
 * ax88179_aroma_driver/tests/safe_exit_test.
 *
 * Do not force SYSLaunchMenu(), SYSRelaunchTitle(), or disable HOME.
 * Let the normal HOME menu own the exit:
 *
 *     HOME -> Quitter
 *          -> PROCUI_STATUS_EXITING
 *          -> cleanup
 *          -> return from main()
 */

static int g_running;
static int g_procui_up;
static int g_last_status = -1;

static uint32_t on_save(void *context)
{
    (void)context;
    OSSavesDone_ReadyToRelease();
    return 0;
}

void proc_init(void)
{
    WHBLogPrintf("proc: normal HOME overlay mode");

    g_running = 1;
    g_procui_up = 1;
    g_last_status = -1;

    ProcUIInitEx(&on_save, NULL);
}

int proc_running(void)
{
    if (!g_running) {
        return 0;
    }

    const ProcUIStatus status = ProcUIProcessMessages(TRUE);

    if ((int)status != g_last_status) {
        WHBLogPrintf("proc: ProcUI status=%d", (int)status);
        g_last_status = (int)status;
    }

    if (status == PROCUI_STATUS_EXITING) {
        WHBLogPrintf("proc: system requested exit");
        g_running = 0;
    } else if (status == PROCUI_STATUS_RELEASE_FOREGROUND) {
        WHBLogPrintf("proc: release foreground");
        ProcUIDrawDoneRelease();
    }

    if (!g_running && g_procui_up) {
        WHBLogPrintf("proc: ProcUI shutdown begin");
        ProcUIShutdown();
        g_procui_up = 0;
        WHBLogPrintf("proc: ProcUI shutdown end");
    }

    return g_running;
}

void proc_stop(void)
{
    /*
     * Deliberately do NOT force a system transition here.
     *
     * The AX safe-exit probe established that the reliable route is the
     * normal HOME overlay followed by "Quitter".
     */
    WHBLogPrintf("proc: local exit requested; use HOME menu -> Quitter");
}

void proc_shutdown(void)
{
    /*
     * Normally proc_running() already did this after EXITING.
     * Keep this fallback for an early startup failure.
     */
    if (g_procui_up) {
        WHBLogPrintf("proc: final ProcUI shutdown");
        ProcUIShutdown();
        g_procui_up = 0;
    }

    g_running = 0;
    WHBLogPrintf("proc: shutdown complete, returning from main");
}
