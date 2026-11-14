/*
 * leopard-conhost — console host (Windows conhost.exe analogue).
 *
 * It owns the XTTTP console (input sink + output buffer), starts the shell as
 * a child process, and renders the shell's output until it exits.  Login is
 * handled by leopard-getty, which execs this program after authentication.
 */
#include "xtuiapi.h"
#include "libsys.h"
#include "console_host.h"

#define DEFAULT_SHELL "/apps/system/lpsh.elf"

int main(int argc, char *argv[], char *envp[])
{
        const char *shell = DEFAULT_SHELL;
        WSTR env[1] = { NULL };

        (void)envp;

        if (argc > 1 && argv[1] != NULL && argv[1][0] != '\0')
                shell = argv[1];

        return conhost_run_session(shell, env);
}
