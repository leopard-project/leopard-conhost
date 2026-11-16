# Input encoding contract (X3TP CHAR/KEYDOWN -> PTY bytes)

`/dev/console` delivers 24-byte `x3tp_event_t` records to conhost, which turns
them into the byte stream written to the PTY master.  The PTY (leopard-xsk
`driver/fs/vfs/pty.cpp`) applies the line discipline; the VT layer only parses
bytes that the slave writes back.  Erase therefore belongs to the **input side**
of the PTY, not to the VT output stream.

## Root cause: Backspace was inserted instead of erasing

`pty_termios_default()` sets canonical mode with `c_cc[VERASE] = 0x7f`.  A PS/2
Backspace arrives as `X3TP_EVENT_CHAR` with `value = 0x08` (packed U+0008), and
`x3tp_event_encode()` used to copy that byte verbatim.  conhost then wrote
`0x08` to the master, which is **not** `VERASE`:

- the byte was inserted into the canonical line buffer, and
- with `ECHOCTL` it was echoed as the two characters `^H`.

The KEYDOWN/keycode path already mapped Backspace to `0x7f`; only the CHAR path
was wrong.  The fix is entirely in userspace (`x3tp_event.c`); no kernel
primitive was added or changed.

## Encoding rules (current)

| Input record | Encoded PTY byte(s) | Notes |
|---|---|---|
| `CHAR` U+0008, no `CTRL` flag | `0x7f` | Backspace key; future IME "erase last character" |
| `CHAR` U+0008, `CTRL` flag | `0x08` | real Ctrl+H is preserved |
| `CHAR` U+007F, any flags | `0x7f` | DEL stays DEL |
| `CHAR` `'?'` + `CTRL` flag | `0x7f` | Ctrl+? (existing behaviour) |
| `KEYDOWN` value `0x08` | `0x7f` | keycode compatibility path |
| `KEYDOWN` `X3TP_KEYCODE_BACKSPACE` | `0x7f` | via `input_encode_key(INPUT_KEY_BACKSPACE)` |
| `KEYUP` | no bytes | releases are never queued |

Printable text still arrives as `CHAR` and is written as UTF-8; Ctrl+letter
chords are still converted to their C0 control byte.

## Contract for future IME producers

- To erase one character, send `CHAR` U+0008 **or** U+007F **without** the
  `CTRL` flag.  conhost normalizes either to the terminal erase byte `0x7f`.
- Do not insert a literal control byte such as `0x08` into committed text to
  "simulate" backspace: it would be stored as data by the line discipline.
- Do not synthesize `KEYDOWN` for printable text; use `CHAR` commit records.

## Applications that change VERASE

conhost's default is `0x7f`.  An application that sets a different `VERASE`
(through `TCSETS`, e.g. plain BSD-style `0x08`) must either:

1. switch to raw mode and implement its own line editor, or
2. make its input method emit the erase byte it configured.

conhost does not inspect the slave termios for `VERASE`, so it cannot adapt
automatically; the kernel does not translate erase bytes for userspace.

## VT output semantics

`vt.c` treats `0x08` (BS) as "move cursor one column left" and `0x7f` as DEL,
i.e. a no-op on output.  This is intentional and correct: erase is signalled by
the PTY echo sequence (`"\b \b"`, or the 6-byte `"\b \b\b \b"` form after a
two-cell control-character echo), not by sending `0x7f` as an output command.
`tests/test_x3tp_event.c` covers both ECHOE forms and asserts the resulting
cells ("ab", cursor column 2) plus the DEL no-op.

## Serial mirror

For headless/debug tests, `console_host.c` mirrors every successful PTY master
read to the serial port through `xapi_OutputSerial()` (small NUL-terminated
chunks, embedded NULs skipped).  conhost's own diagnostics are not mirrored (the
kernel already sends owner-context `xapi_Printf` output to serial).  Password
input never appears because the PTY suppresses echo while `ECHO` is off.
