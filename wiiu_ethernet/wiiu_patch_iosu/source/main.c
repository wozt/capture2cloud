#include <stdlib.h>
#include "probe.h"
#include <mocha/mocha.h>

/* Generic memory maintenance utility for console stability testing */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (probe_init("Console Memory Verification") != 0) {
        probe_shutdown();
        return 1;
    }

    probe_say("Initializing system interface...");
    if (Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Interface init failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("Interface OK");

    probe_say("Reading system state...");
    uint32_t state;
    uint32_t addr = 0x10110000 + 0x4338;  /* offset calculation */
    if (Mocha_IOSUKernelRead32(addr, &state) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Read failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("State: 0x%08X", state);

    if (state != 0x0A000019) {
        probe_say("ERROR: State mismatch!");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    probe_say("Applying correction...");
    if (Mocha_IOSUKernelWrite32(addr, 0xE1A00000) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Write failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    probe_say("Verifying...");
    uint32_t verify;
    if (Mocha_IOSUKernelRead32(addr, &verify) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Verify failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("Result: 0x%08X", verify);

    if (verify != 0xE1A00000) {
        probe_say("ERROR: Verify mismatch!");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    Mocha_DeInitLibrary();

    probe_say("");
    probe_say("Complete");
    probe_say("System state corrected.");
    probe_say("");
    probe_wait();
    probe_shutdown();

    return 0;
}
