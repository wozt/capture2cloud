#ifndef CAPTURE2CLOUD_VERSION_H
#define CAPTURE2CLOUD_VERSION_H

/* The release this build is.
 *
 * Kept here so the host, the console client and the page all say the
 * same thing, and pinned by a test against the VERSION file and the
 * Android build so a bump that misses one of them fails the suite
 * rather than shipping three answers to "which version am I running".
 */
#define C2C_VERSION "1.2.3"

#endif
