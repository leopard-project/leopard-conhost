# PROJECT KNOWLEDGE BASE — leopard-conhost

Console host. Login is **not** handled here; `leopard-getty` authenticates and
execs this program with the shell path as argv[1].

- `SXAH_MARK_IS_TERMINAL` makes this process group the XTTTP console owner.
- `conhost_run_session()` forks the shell and runs the output/input loop.
- No global focus policy here; Win32K (future) decides which window is active.
