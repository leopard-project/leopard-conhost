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

#ifndef CONHOST_DEFAULT_CHILD
#define CONHOST_DEFAULT_CHILD "/apps/leopard/leopard-getty.elf"
#endif

#define DEFAULT_CHILD CONHOST_DEFAULT_CHILD

int main(int argc, char *argv[], char *envp[])
{
        const char *child = DEFAULT_CHILD;
        WSTR env[1] = { NULL };

        (void)envp;

        if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                child = argv[1];

        return conhost_run_session(child, env);
}
