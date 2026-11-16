#pragma once

#include "xtuiapi.h"

/* Create the PTY, start the shell on the slave, and run the terminal loop.
 * conhost is a pure PTY host: /dev/console events go to the PTY, and PTY
 * output is rendered through the VT layer to the framebuffer. */
int conhost_run_session(const char *shell, WSTR envp[]);
