#ifndef NSYSNET_SHIM_H
#define NSYSNET_SHIM_H

/* Registers the nsysnet.rpl socket/DNS hooks with the FunctionPatcher
 * module. Idempotent within a process. Returns 0 on success, -1 if the
 * patcher module is unavailable (titles then keep using the console's
 * own network). */
int nsysnet_shim_install(void);

/* Removes this process's patches; call before the hosting title is
 * finalized. */
void nsysnet_shim_uninstall(void);

#endif
