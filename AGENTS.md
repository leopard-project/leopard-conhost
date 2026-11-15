# PROJECT KNOWLEDGE BASE — leopard-conhost

PTY terminal host. Login is **not** handled here; `leopard-getty` authenticates
and execs this program with the shell path as argv[1].

- `pty.c` creates `/dev/ptmx` -> `/dev/pts/N`, calls `setsid`/`TIOCSCTTY` in the
  child, sets `TIOCSWINSZ`, dup2s the slave to 0/1/2 and execs the shell.  It also
  exposes `TCGETS`-based `pty_no_echo()` for IME password context.
- `console_host.c` keeps the master and runs the poll loop.
- `vt.c` owns the cell grid and dirty rows; `screen.c` is the only framebuffer
  code (including the IME status overlay); `input.c` + `x3tp_event.c` are the
  `/dev/console` event boundary; `ime_client.c` is the fail-open manager client.
- `/dev/fb0` (FBIOGET_FSCREENINFO/FBIOGET_VSCREENINFO + mmap) is the primary
  output backend.  If it is unavailable, dirty rows go through `xapi_Printf`.
- `/dev/console` (`x3tp_event_t`, 24-byte records) is the primary keyboard
  source.  Read must use a 24-byte multiple: the kernel returns `-EINVAL` for
  smaller buffers and `-EAGAIN` when empty.
- On open, conhost flushes queued records until `-EAGAIN`.  Login
  (`leopard-getty`) reads the legacy keyboard queue in parallel, so the input
  core ring still holds the typed username/password; without the flush the new
  shell would receive them as commands.
- XTTTP output/input hooks remain only as a bring-up compatibility fallback for
  apps that still use XAPI_PRINTF/XAPI_INPUT, and for `/dev/console` open/read
  failure.  Do not make XAPI_INPUT the primary keyboard path or add a
  graphics.sys dependency.
- No global focus policy here; Win32K (future) decides which window is active.
- IME transport is a stdin/stdout **pipe pair**, not a socket.  conhost creates
  two `pipe()`s, forks `leopard-ime.elf` with the child ends on fd 0/1, and
  keeps the parent ends.  pipefs is the kernel's real blocking IPC primitive
  (wait queue, poll locks only the pipe), unlike AF_UNIX reads/polls which
  yield-spin and can deadlock when both peers poll.  No bind/connect/name.
- conhost caches two pieces of manager state locally: `ime_client.active`
  (Ctrl+Space on/off) and `ime_client.composing` (a compose run is in
  progress).  The manager sends `STATE` frames after hello, toggles and every
  engine result.
- Fast path: IME off → direct PTY; IME on but not composing → only ASCII
  letters go to the manager (they can start a compose run), while
  space/enter/punctuation/digits/special keys/Ctrl-Alt chords go straight to
  the PTY.  This mirrors OJ's kernel filter returning false for direct input.
  When composing, every key goes to the manager.
- Password/no-echo keys bypass the IME on the client side.
- The manager is a per-session child of conhost (GUI sessions will start it
  from the compositor the same way).  It logs to stderr, which conhost
  redirects to `/dev/null` so lifecycle noise never reaches the terminal.
