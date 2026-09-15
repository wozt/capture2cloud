#include <stdlib.h>
#include "probe.h"
#include <mocha/mocha.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (probe_init("IOSU Patch: AX88179 Unlock") != 0) {
        probe_shutdown();
        return 1;
    }

    probe_say("Initializing Mocha...");
    if (Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Mocha init failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("Mocha OK");

    probe_say("Reading 0x10114338...");
    uint32_t original;
    if (Mocha_IOSUKernelRead32(0x10114338, &original) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Read failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("Got: 0x%08X (expect 0x0A000019)", original);

    if (original != 0x0A000019) {
        probe_say("ERROR: Value mismatch!");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    probe_say("Writing NOP (0xE1A00000)...");
    if (Mocha_IOSUKernelWrite32(0x10114338, 0xE1A00000) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Write failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    probe_say("Verifying patch...");
    uint32_t patched;
    if (Mocha_IOSUKernelRead32(0x10114338, &patched) != MOCHA_RESULT_SUCCESS) {
        probe_say("ERROR: Readback failed");
        probe_wait();
        probe_shutdown();
        return 1;
    }
    probe_say("Read: 0x%08X", patched);

    if (patched != 0xE1A00000) {
        probe_say("ERROR: Patch mismatch!");
        probe_wait();
        probe_shutdown();
        return 1;
    }

    Mocha_DeInitLibrary();

    probe_say("");
    probe_say("SUCCESS: IOSU patched!");
    probe_say("You can now test AX88179.");
    probe_say("");
    probe_wait();
    probe_shutdown();

    return 0;
}
