#pragma once

#include "xtuiapi.h"

/* Private XJ380 extension syscalls used by the console host. */
#define SXAH_MARK_IS_TERMINAL     128956723895689204ULL
#define SXAH_WRITE_INPUT_BUFFER   128956723895689205ULL
#define SXAH_READ_OUTPUT_BUFFER   128956723895689206ULL
#define SXAH_CHECK_INPUT_BUFFER   128956723895689207ULL
#define SXAH_UNLOCK_OUTPUT_LOCK   128956723895689208ULL

#define XTTTP_INPUT_STATUS_WAITING 0x1
#define XTTTP_INPUT_STATUS_NO_ECHO 0x2

/* Mark this process group as the XTTTP console and own its sink. */
void conhost_mark_terminal(void);

/* Run the shell as a console client and stay alive until it exits. */
int conhost_run_session(const char *shell, WSTR envp[]);
