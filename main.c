/*
 * leopard-conhost — PTY terminal host (Windows conhost.exe analogue).
 *
 * It owns the console framebuffer, creates a PTY pair, starts leopard-getty on
 * the slave, and bridges pty master <-> VT <-> framebuffer.  getty authenticates
 * on the PTY and execs the user's shell in the same session.
 */
#include "xtuiapi.h"
#include "libsys.h"
#include "console_host.h"

#define DEFAULT_CHILD "/apps/leopard/leopard-getty.elf"

int main(int argc, char *argv[], char *envp[])
{
        const char *child = DEFAULT_CHILD;
        WSTR env[1] = { NULL };

        (void)envp;

        if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                child = argv[1];

        /* Keep the XTTTP sink registered for the legacy getch fallback.  The
         * PTY path does not depend on this; it is only an input compatibility
         * bridge until a kernel input endpoint exists. */
        conhost_mark_terminal();

        return conhost_run_session(child, env);
}
