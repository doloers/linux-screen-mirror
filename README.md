# linux-screen-mirror

> **把另一台 Linux（手机 / SBC / 备用机）的整块屏幕投到本机** —— 零编码、低延迟、TUI 一键开播。
> 在 **Arch Linux + niri 26.04 + PipeWire**（本机）← **OnePlus 6 / postmarketOS / Phosh**（被投屏端）上开发并日常使用。

```
远端（手机）                                        本机（笔记本）
┌──────────────────────────────────────┐          ┌──────────────────────────────┐
│ xdg-desktop-portal ScreenCast        │          │                              │
│      ↓ (PipeWire 节点, 逻辑 360x760) │          │                              │
│ gst-launch-1.0                       │   ssh    │                              │
│   videoconvert / videoscale          │ ───────► │ mpv                          │
│   ★ 不编码 ★  → 裸 I420              │  管道    │   --demuxer-lavf-format=     │
│      ↓                               │          │        rawvideo              │
│ stdout ──────────────────────────────┼─────────►│   --cache=no --untimed       │
└──────────────────────────────────────┘          └──────────────────────────────┘
        远端 CPU ≈ 1~15%                                  本机 CPU ≈ 3.4%
```

## 三种模式

| 模式 | 链路 | 远端 CPU（实测） | 延迟（实测） | 适用 |
|---|---|---|---|---|
| **`portal`（默认）** | portal → PipeWire → `gst-launch`（只转换/缩放，**不编码**）→ 裸 I420 → ssh → mpv | **1%（静止）/ 15%（连续变化）** | ~115 ms | 现代 Wayland 桌面（Phosh/wlroots/GNOME/KDE） |
| `stream` | 远端 `wf-recorder` 编 H.264 → ssh → mpv | 26–35%（可飙到 66%） | 75–120 ms | 老系统 / 没有 portal / 只想跑编码器 |
| `waypipe` | `waypipe ssh <host> <应用>`：把**远端某个应用**拿到本机跑 | — | — | ⚠️ **不能镜像远端屏幕**，只是转发单个应用 |

> `portal` 模式为什么这么省：挂上 PipeWire 图之后**不做任何编码**，直接把 I420 裸帧走 ssh。
> 远端只出 ~1%（静止时甚至是 0：PipeWire 只在画面变化时出帧）。代价是带宽（静止 ~80 KB/s，剧烈变化时 ~19 Mbit/s）——
> 局域网内完全不是问题。

## 特性

- 🪶 **零编码投屏**：远端不跑 x264，手机不发热、不掉帧，静止画面时几乎零占用
- 🎛️ **TUI 配置界面**：模式 / 主机 / 帧率 / 缩放 / 编码档位 / 硬解 / 显示方式，全部菜单里点选，配置存 `~/.config/mirror-screen/config.json`
- 🖥️ **本机窗口形态可控**：半幅窗口（默认，niri 规则钉死尺寸）/ 贴合视频 / 全屏；配套 `niri-cast-rule.sh` 一键装规则
- ⚡ **极低延迟**：`--cache=no --demuxer-readahead-secs=0 --untimed --video-sync=desync` + 远端 `leaky` 队列（积压就丢旧帧，绝不"卡一下然后快进"）
- 🔌 **ssh 连接复用**：`ControlMaster/ControlPersist` —— 这台手机每条新 ssh 要 285~307 ms，复用后 21~45 ms，开播 2.0 s → **0.50 s**
- 🌙 **自动唤醒远端屏幕**：远端息屏时抓屏必然失败（`no supported format found`），开播前先 `wlr-randr --on`
- 🩺 **自检**：`--check` 一次体检远端（依赖、输出名、抓屏实测、portal 可用性）
- 🧹 **关窗即收尾**：关掉投屏窗口自动清理远端进程；运行时报文全部进日志文件，不刷屏

## 依赖

**本机**：`mpv`、`ssh`、`niri`（查工作区尺寸；其它合成器可改 `workspace_geometry()`）

**远端（被投屏端）**：

| 包（Alpine/postmarketOS） | 用途 | 备注 |
|---|---|---|
| `gstreamer` `gstreamer-tools` `gst-plugins-base` `gst-plugins-good` | portal 模式的取帧与格式转换 | `gst-launch-1.0` 来自 `gstreamer-tools` |
| `gst-plugin-pipewire` `pipewire-tools` | 让 GStreamer 能读 PipeWire 节点 | 缺了会报 `pipewiresrc` 不存在 |
| `python3-dbus`（可选 `pygobject`） | 远端 portal 客户端 | dbus 1.4.0 / gi 通常已装 |
| `wf-recorder`（仅 `stream` 模式） | 抓屏 + 编码 | |
| `wlr-randr` | 唤醒屏幕 / 探测输出名 | |

远端一键体检/安装：`./remote-mirror-prep.sh <host>`（支持 apk/pacman/apt/dnf）。

## 安装与使用

```bash
git clone https://github.com/doloers/linux-screen-mirror
cd linux-screen-mirror

./install-mirror-screen.sh           # 编译 C 版主程序 + 装到 ~/.local/bin + fuzzel 入口
mirror-screen                        # 打开 TUI 配置界面（C/ncurses）
mirror-screen-py                     # Python 版界面（回退/对照）
mirror-screen --run                  # 用已保存配置直接开播（可绑快捷键）
mirror-screen --check                # 远端体检
mirror-screen --dry-run              # 只打印将要执行的命令
./install-mirror-screen.sh --uninstall   # 干净卸载
```

首次开播时远端会弹一次 portal 授权对话框，之后 token 存在远端 `~/.local/state/portal_cast.token`，不再打扰。

**让投屏窗口尺寸固定**（niri 用户，强烈建议）：

```bash
./niri-cast-rule.sh install   # 幂等 + 自动备份 + niri validate
./niri-cast-rule.sh check     # 看是否已装
./niri-cast-rule.sh remove    # 卸载
```

## 实测数据

同一台手机（OnePlus 6 / postmarketOS / Phosh，DSI-1 逻辑 360×760）→ 本机（2880×1800 @1.75）：

| 指标 | `portal`（零编码） | `wf-recorder` 流 |
|---|---|---|
| 远端 CPU（静止 / 连续变化） | **1% / 15%** | 26–35%（压得狠时 66%） |
| 链路码率（静止 / 变化中） | 80 KB/s / ~19 Mbit/s | ~180 kbit/s（H.264） |
| 端到端延迟 | ~115 ms（10 Hz 色块钟：均值间隔 101 ms，最大 245 ms） | 75–120 ms |
| 本机 mpv CPU（VA-API） | ~3.4% | ~2.6% |
| 帧率稳定性 | PipeWire 实时图，**无队列** | ⚠️ `-r` 的 fps 滤镜**只排队不丢帧**，实测漂移 0 ms → **57 s** |

其它实测结论：

- **"压缩换速度"是伪命题**：H.264 只占链路 0.07%，把 preset 压狠只会让远端 CPU 翻倍（35% → 66%）、延迟毫无改善。
- **远端 WiFi 省电**是"反应很慢"的真凶：关掉后每包 RTT 94.7 ms → **10.7 ms**。
- **ssh 建连**在这台手机要 285–307 ms，所以预检合并成一条连接、并全程复用 ControlMaster。
- `wayvnc + VNC 客户端`测过：能出画面但延迟 379–1343 ms，且要 ssh 隧道（远端 nftables input=drop），已放弃。

## 投屏窗口的显示

`显示方式` 三档：**半幅窗口（默认）** / 贴合视频 / 全屏。

半幅窗口**不向 mpv 传任何尺寸参数**，尺寸交给 niri 规则（`configs/niri-window-rule.kdl`）：

```kdl
window-rule {
    match app-id=r#"^portal-cast$"#     // mpv 用 --wayland-app-id=portal-cast 启动，不影响日常 mpv
    tiled-state true                     // ★ 关键
    default-column-width { proportion 0.5; }
}
```

`tiled-state true` 是关键：不加它，**mpv 会按视频尺寸自己"吸附"窗口**（它以为自己在浮动），
现象是窗口永远比邻窗矮一截（**997 vs 1013**），而且聚焦/失焦、切工作区时还会跳。
同样的坑 foot 也有（按字符格吸附 ⇒ 底部白隔 10~20px）。加上之后：

- 窗口 **810×1013**（平铺 ✓），与旁边满高窗口**完全齐平**
- 焦点切 4 次 + 来回切工作区，尺寸**全程不变**
- 画面区域 837×1751 物理、宽高比 **0.478**（手机 9:19 = 0.474，**无变形**），两侧黑边各 165 逻辑像素

被排除掉的方案（都实测过）：`--geometry`（Wayland 下被 mpv 忽略）、`default-window-height { fixed }`（niri 只当参考，差 1px）、
`border{off}`/`focus-ring{off}`（无效）、启动后用 IPC 改 `window-scale`（被 mpv 吸附回去）、
`open-floating true`（尺寸确实稳，但脱离平铺布局 ✗）。

## 性能：热路径的 C 重写

配置界面（TUI）留在 Python，但**真正跑在关键路径上的两个进程**是 C：

| 进程 | 位置 | Python 版 | C 版 | 收益 |
|---|---|---|---|---|
| `portal_cast` | **远端（被投屏机）** | 25.3 MB / 启动 0.37–0.41 s | **1.2 MB / 0.07–0.10 s** | 省 24 MB，每次开播省 ~0.3 s |
| `mirror-screen` | 本机（配置界面 + 投屏期间常驻的监管进程） | 27.9 MB / 启动 77 ms | **~2 MB / 1 ms** | 省 26 MB |

配置界面（TUI）也已经是 C（ncurses）：`mirror-screen` 不带参数就是它，按 Ctrl-R 开播后自动脱离终端。
主程序所有选项（`--run/--dry-run/--print-config/--wake/--check/--list-hosts`）都在 C 版里；
Python 版完整保留为 `mirror-screen-py`（回退/对照用），两边共用同一份 `config.json`，可互相读写。

- **远端客户端** `portal_cast.c`：只依赖 `libdbus`（不再需要 python3-dbus / PyGObject）。
  首次开播时**在远端自动编译**（`gcc -O2 … $(pkg-config --cflags --libs dbus-1)`），编译不出来或运行失败
  自动回退 Python 版（TUI 设置项「远端客户端」= auto / c / python）。
- **本机监管引擎** `mirror-screen-c.c`：共用同一份 `config.json`，负责预检、`ssh | mpv` 管线、日志、收尾、通知。
  TUI 的 `--run` 会 `execv` 它（不留 Python 进程）；找不到或 `engine=python` 时行为与以前完全一致。
- 正确性验证：C 版与 Python 版的 `--dry-run` 输出**逐字节一致**（ssh 参数、mpv 参数、三种显示方式全对齐）。

## 踩过的坑（血泪）

1. **远端息屏**时抓屏必然失败，现象极像协议/权限问题 —— 开播前必须 `wlr-randr --output <名> --on`
2. **裸流 + 静止画面时 mpv 不建窗口** ⇒ 必须 `--force-window=immediate`
3. **`--geometry` 在 Wayland 下被 mpv 自己忽略**（5 种写法结果一模一样）
4. **mpv 会吸附窗口尺寸**（见上）⇒ niri 规则必须带 `tiled-state true`
5. **`pipewiresrc` 要用 `target-object=<节点名>`**（`path=` 已废弃）；且远端 portal 客户端**必须用子进程方式跑 gst**，
   不能用 `os.execvp` —— exec 会杀掉 D-Bus 连接 ⇒ portal 销毁会话 ⇒ 节点消失 ⇒ `target not found`
6. **本机 mpv 必须 `--hwdec=vaapi`**：装过 `vulkan-intel` 后 `auto-safe` 会挑 Vulkan 解码，而本机 Mesa 没有
   `VK_KHR_video_decode_queue` ⇒ 逐帧报错然后 `no frame!`
7. **`wf-recorder -f -` 不支持 stdout**，要写 `-f /dev/stdout` 并加 `-y`
8. 清理进程时别用 `pkill -f <模式>` —— 模式会匹配到**自己那条命令行**，把自己杀掉（我踩了两次），
   改用 `pkill -x <进程名>` 或按 PID
9. 脚本里给 niri 配置文件加标记时，注释必须用 KDL 的 `//`；写成 `#` 会让 `niri validate` 报错、**整个配置加载失败**
10. 写 C 版 D-Bus 客户端时：父子迭代器必须是**不同变量**（否则消息签名错乱）；`DBUS_TYPE_STRING` 要传**指向指针的指针**；
    Alpine 的 libdbus 不导出 `dbus_message_get_unix_fd`（1.16 起该公开 API 已删除），而消息里的 fd 值**本身就是 fd** ——
    用 `fcntl(F_GETFD)` 试探即可两种语义通吃；GStreamer 参数必须是 `min-buffers=2` 这种**单 token**

## 文件

| 文件 | 说明 |
|---|---|
| `mirror-screen-c.c` | **主程序**：TUI 配置界面（ncurses）+ 运行时引擎，编译成一个二进制 |
| `mirror-screen.py` | Python 版（保留为 `mirror-screen-py`，回退/对照） |
| `install-mirror-screen.sh` | 安装/卸载到 `~/.local/bin`（含编译 C 引擎），并生成 fuzzel 入口 |
| `portal_cast.py` / `portal_cast.c` | **远端侧** portal 客户端（Python 版 / C 版）：ScreenCast → PipeWire → 裸视频写 stdout |
| `portal-cast.sh` | 命令行一键启动器（不开 TUI） |
| `niri-cast-rule.sh` | niri 窗口规则装/卸/查（半幅窗口 + `tiled-state`） |
| `remote-mirror-prep.sh` | **在远端跑**：体检/安装依赖、探测输出名、抓屏实测 |
| `gen-consts.py` / `ms_consts.h` | 从 Python 侧自动导出常量（模式/档位/远端脚本/默认值）→ C 头文件，避免两边手抄不一致 |
| `configs/` | niri 窗口规则片段、TUI 配置示例 |
| `docs/` | AirPlay/Linux 投屏生态调研、全部实测证据 |

## 文档

- [`docs/research-airplay-and-linux-casting.md`](docs/research-airplay-and-linux-casting.md) —— AirPlay 在 Linux 的生态调研 + Linux→Linux 各家方案对比（waypipe / wayvnc / Sunshine+Moonlight / wf-recorder）
- [`docs/evidence-measurements.md`](docs/evidence-measurements.md) —— 逐条实测记录（延迟、CPU、带宽、窗口尺寸、坑的复现与结论）
- [`NOTES.md`](NOTES.md) —— 实现细节与设计取舍
- [`CHANGELOG.md`](CHANGELOG.md)

## 许可

MIT © 2026 doloers
