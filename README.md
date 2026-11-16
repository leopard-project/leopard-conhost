# leopard-conhost

LeopardOS console host（Windows `conhost.exe` 的对应物）。

```text
shell/app <-> /dev/pts/N <-> pty master <-> conhost <-> framebuffer
```

- `leopard-getty` 只负责认证，认证后 exec 本程序并传入 shell 路径；
- conhost 创建 PTY master/slave，把 shell 放进新 session，slave 作为
  controlling terminal，并 dup2 到 0/1/2；
- 父进程保留 master，把输出送入 VT 解析器，再把 dirty 行渲染到 `/dev/fb0`；
  同时把 PTY 输出镜像到串口，供无头测试和现场诊断使用；
- 键盘输入从 `/dev/console` 读取 24 字节 `x3tp_event_t` 记录并写回 master；
- shell 退出、master EOF/EIO 后清理并退出，由 `leopard-rc` respawn getty。

## 后端边界

| 文件 | 职责 |
|------|------|
| `pty.c` | `posix_openpt` / `grantpt` / `unlockpt` / `ptsname`、fork、`setsid`、`TIOCSCTTY`、`TIOCSWINSZ`、dup2/exec、`TCGETS` no-echo 查询 |
| `vt.c` | VT/ANSI 单元格缓冲、UTF-8、光标/擦除/SGR/滚动区/备用屏/DECCKM |
| `screen.c` | `/dev/fb0` + `FBIOGET_*` + mmap；不可用时退回 `xapi_Printf` 文本输出；IME 状态条 overlay |
| `input.c` | `/dev/console` 探测、record 重组/部分缓冲、seat 字节源 |
| `x3tp_event.c` | `x3tp_event_t` 解码、packed UTF-8、KEY_* → ANSI（DECCKM/application keypad） |
| `ime_client.c` | fail-open IME 客户端：fork manager、pipe 传输、队列/deadline/回放 |
| `console_host.c` | poll 循环、PTY/VT/screen/input/IME 编排 |

## `/dev/console` 输入 ABI

`/dev/console` 是只读 seat0 endpoint，`poll/POLLIN`，空读返回 `-EAGAIN`，
`root:Admin 0660`。每条记录 24 字节：

```c
typedef struct x3tp_event {
    uint32_t type;      /* 1 CHAR, 2 KEYDOWN, 3 KEYUP */
    uint32_t value;     /* CHAR: packed UTF-8; KEY: KEY_* code */
    uint32_t flags;     /* INPUT_FLAG_* */
    uint32_t reserved;  /* 0 */
    uint64_t sequence;  /* monotonic */
} x3tp_event_t;
```

- 读取总是使用 24 的整数倍长度（内核拒绝 <24 的 buffer 并返回 `-EINVAL`）；
  conhost 本地保留 staging，能处理跨 read 的短尾。
- CHAR 直接输出 UTF-8 字节；Ctrl 修饰位转成 C0 控制字符。
- 退格/擦除是 PTY 输入语义：无 CTRL 的 `CHAR U+0008`（Backspace，未来
  IME 的“删一个字”）归一化为 `0x7f`，匹配内核 PTY 默认的 `VERASE=0x7f`；
  真正的 Ctrl+H 仍发送 `0x08`。详见 `docs/INPUT_ENCODING.md`。
- KEYDOWN/KEYUP 使用 `include/ps2/keyboard.h` 的 `KEY_*` 码（>=128）：
  方向键/Home/End/Delete/PgUp/PgDn/F1-F12 按 DECCKM 编码；KEYUP 不产生字节。
- 内核 v1 没有 keypad `KEY_*` 码，数字小键盘以 CHAR 到达，因此 application
  keypad 编码在 `/dev/console` 路径上不可达（`input_encode_key` 保留能力）。
- `/dev/console` 读失败时关闭该输入源；若 open 失败则尝试 seat 字节源。
  PTY 输出/VT/framebuffer 路径不受影响。

## IME 集成

- fork `/apps/system/leopard-ime.elf` 并通过 stdin/stdout pipe 通信；启动失败则整条 IME 路径关闭，不影响输入。
- 每个按键带 seq + 原始字节排队；manager 回复 PASS/COMMIT/CONSUMED/COMMIT_PASS。
- 100 ms 内没有回复就按原字节回放；socket 断开时整个队列按序回放。
- `TCGETS` 查询 slave 的 ECHO 位得到 password/no-echo context；该 context 下
  manager 直接 PASS，不进入 compose。
- preedit/候选通过 UI 帧回到 conhost，绘制在终端底行（字体无 CJK 时显示 `?`）。

## 兼容回退

- `/dev/fb0` 不存在或 mmap 失败时，dirty 行通过 `xapi_Printf` 输出，PTY/VT
  路径仍可测试；framebuffer 是首选路径。
- `/dev/console` 不可用时（或非 event 模式下显式配置了 seat 字节源），键盘
  字节直接写回 PTY master。

## 构建 / 测试

```sh
python3 gen_ninja.py && ninja build   # → out/leopard-conhost.elf
ninja pack                            # → out/leopard-conhost-*.xbps

cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -I. \
   tests/test_x3tp_event.c x3tp_event.c input.c vt.c -o /tmp/test_x3tp_event
/tmp/test_x3tp_event
```

安装位置：`/apps/leopard/leopard-conhost.elf`
