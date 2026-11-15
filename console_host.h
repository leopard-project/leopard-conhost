#pragma once

#include "xtuiapi.h"

/* XTTTP compatibility syscalls.  They are only used by the bring-up fallback
 * for apps that still call XAPI_PRINTF/XAPI_INPUT; the PTY path does not
 * depend on them. */
#define SXAH_MARK_IS_TERMINAL     128956723895689204ULL
#define SXAH_WRITE_INPUT_BUFFER   128956723895689205ULL
#define SXAH_READ_OUTPUT_BUFFER   128956723895689206ULL
#define SXAH_CHECK_INPUT_BUFFER   128956723895689207ULL
#define SXAH_UNLOCK_OUTPUT_LOCK   128956723895689208ULL

#define XTTTP_INPUT_STATUS_WAITING 0x1
#define XTTTP_INPUT_STATUS_NO_ECHO 0x2

/* Register the XTTTP sink used by the legacy getch fallback. */
void conhost_mark_terminal(void);

/* Create the PTY, start the shell on the slave, and run the terminal loop. */
int conhost_run_session(const char *shell, WSTR envp[]);
