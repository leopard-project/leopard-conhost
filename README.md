# leopard-conhost

LeopardOS console host（Windows `conhost.exe` 的对应物）。

- 拥有一个 XTTTP console：input sink + output buffer；
- 接收 `leopard-getty` 传入的 shell 路径；
- fork/exec shell，循环渲染输出、转发输入；
- shell 退出后 conhost 退出，由 `leopard-rc` respawn getty。

```sh
python3 gen_ninja.py && ninja build   # → out/leopard-conhost.elf
ninja pack                            # → out/leopard-conhost-*.xbps
```

安装位置：`/apps/leopard/leopard-conhost.elf`
