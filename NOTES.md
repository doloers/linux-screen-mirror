# 实现细节与设计取舍（NOTES）

## 为什么默认是「portal 零编码」而不是「wf-recorder 流」

两者的本质差别不是编码效率，而是**有没有队列**：

- `wf-recorder -r <fps>` 的 fps 滤镜是**只排队、不丢帧**的。远端出帧速率一旦超过 `-r`，队列就单调增长。
  实测同一台手机：延迟从 0 ms 一路漂到 **57 s**（即"卡一下然后快进"）。压低帧率只能减慢漂移，不能根治。
- portal + PipeWire 是**实时图**：`pipewiresrc` 挂上去之后，GStreamer 按自己的消费速度拉帧，
  积压就丢（远端还额外加了 `queue leaky=upstream max-size-buffers=2 min-buffers=2`）。
  代码路径上根本不存在"攒一段再放出来"的可能。

所以 `portal` 模式在**静止画面**时远端 CPU 是 **1%**（PipeWire 只在画面变化时出帧），
`wf-recorder` 模式则不管画面动不动都在编码（26~35%）。

代价是带宽：裸 I420 是 `W×H×1.5` 字节/帧，360×760@10fps 静止时约 80 KB/s、剧烈变化时约 19 Mbit/s。
局域网内无压力；如果你要走公网，请用 `stream` 模式（H.264）。

## portal 模式的实现要点

1. **D-Bus 会话的生命周期**：portal 的 ScreenCast 会话与发起它的 D-Bus 连接绑定。
   所以拿到 PipeWire fd 之后**绝对不能用 `os.execvp` 启动 gst**（exec 会替换进程、断掉 D-Bus 连接
   ⇒ portal 销毁会话 ⇒ 节点消失 ⇒ gst 报 `target not found`）。
   正确做法：`subprocess.Popen(..., pass_fds=(fd,))`，让 D-Bus 连接留在父进程里活到投屏结束。
2. **节点引用**：`pipewiresrc` 用 `target-object=<节点名>`（用 `pw-dump` 按 id 反查名字，常见是
   `xdg-desktop-portal-wlr`）。旧的 `path=` 是已废弃的 String 属性，会直接失败。
3. **授权持久化**：首次授权后把 `restore_token` 写远端 `~/.local/state/portal_cast.token`，
   之后 `SelectSources` 带上它就不再弹窗。
4. **尺寸**：portal 给的是**逻辑分辨率**（手机 360×760）。再往上放缩没有信息增益，只会浪费带宽。

## 低延迟参数为什么是这几个

```bash
# 本机 mpv
--cache=no --demuxer-readahead-secs=0 --demuxer-max-bytes=8MiB
--untimed --video-sync=desync
```

- 管道输入时 mpv 默认 `cache=auto` 会**先攒一段再一次性放出来** —— 这就是用户报的"卡一下然后快进"。
- `--demuxer-max-bytes` **不能调太小**：720p 单帧可能 >64 KB，太小会让 demuxer 拼不出完整帧 ⇒ 完全没画面。
- 远端 `--drop-old`（即 `queue leaky=upstream`）：积压时丢旧帧而不是丢新帧。

## 本机窗口尺寸：为什么交给 niri 规则，而不是 mpv 参数

按时间顺序试过并被否定的：

| 做法 | 结果 |
|---|---|
| `--geometry=50%x100%` | Wayland 下被 mpv 自己忽略（5 种写法，niri 给的窗口一模一样） |
| `--autofit=WxH` | 同样被忽略（窗口变成 811×457） |
| `--vf=pad` + `--window-scale` | 能定尺寸 ✓，但**mpv 之后会按视频尺寸重新吸附**，比邻窗矮 16px，且焦点一变就跳 |
| 启动后用 IPC 改 `window-scale` | 立刻生效，但过一会儿又被 mpv 吸附回去（像 niri 在回弹，其实不是） |
| niri `window-rule` + `default-window-height { fixed N }` | niri 只把它当参考（1012 与 1028 只差 1px） |
| niri `window-rule` + `open-floating true` | 尺寸确实恒定 ✓，但窗口**脱离平铺布局**（用户明确要求平铺） |
| **niri `window-rule` + `tiled-state true`** | ✓✓ 恒定 + 与邻窗齐平 + 仍是平铺 |

`tiled-state true` 的作用是**告诉客户端"你在平铺布局里"**：mpv（和 foot 这类会做尺寸吸附的客户端）
收到后就不再自己调窗口尺寸，把尺寸完全交给合成器。
`mirror-screen.py` 因此**不再传任何尺寸参数**给 mpv，只传 `--wayland-app-id=portal-cast` 让规则能匹配上；
其它显示方式（贴合视频 / 全屏）用 `portal-cast-fitted` 这个 app-id，避免被那条"半幅"规则套住。

代码里还留了一个**尺寸看护**线程（TUI 开关「锁定窗口尺寸」）：每 2 s 对一次账，低于目标就幂等重下发一次
启动时的目标尺寸，连续 5 次留不住就指数退避放弃——正常情况下它一次都不会触发（实测 0 次）。

## ssh 与预检

- 这台手机每条新 ssh 连接 **285~307 ms**（KEX 慢），所以：`ControlMaster=auto` + `ControlPersist=300`
  复用连接后降到 **21~45 ms**，开播总耗时 2.0 s → **0.50 s**。
- 开播前的预检（唤醒屏幕 + 清理远端残留编码器）**合并成一条 ssh**，不要开多条。
- 远端进程若用 `setsid nohup … &` 起了会被回收（postmarketOS 上是 systemd），需要时用
  `systemd-run --uid=<uid> --unit=<name> --setenv=WAYLAND_DISPLAY=… --setenv=XDG_RUNTIME_DIR=… <cmd>`。

## 远端环境相关的坑

- 远端 **nftables `input` 策略是 drop**（postmarketOS 默认），只有 22 端口可达 ⇒ 任何"直连远端端口"的方案
  （如 wayvnc 的 5900）都要走 ssh 隧道。
- 远端 `grim` 一帧要 0.6~0.8 s（SHM 回读慢），别拿它做连续抓屏。
- 远端没有 VA-API（Adreno），`wl-screenrec` 即使指定 `--no-hw` 也会初始化 VAAPI ⇒ 直接不可用。

## 日志与清理

- 运行时报文全部写 `~/.local/state/mirror-screen/last-run.log`（含远端 stderr），终端不刷屏。
- `mpv.log`、`cast.pid` 也在同目录。
- 关掉投屏窗口 ⇒ 进程退出 ⇒ 自动清理远端编码器/portal 会话；异常早退会有桌面通知。

## 关于 C 重写（2026-09-30）

**改了哪些、没改哪些**：只把热路径的两个进程换成 C —— 远端 portal 客户端、本机投屏监管进程。
TUI 配置界面、远端体检（`--check`）、waypipe 模式仍在 Python：它们不在关键路径上，搬过去只增加维护面。
TUI 的 `--run` 用 `execv` 换成 C 引擎（不留 Python 进程），失败自动回退 Python 实现，行为一致。

**为什么收益明显**：这两个进程分别"每次开播启动一次"和"整个投屏期间常驻"。
Python 解释器 + `gi`/`dbus` 的导入开销让远端客户端要 0.37 s / 25 MB（在开播的关键路径上），
本机监管进程则常驻 27.9 MB。C 版分别是 0.07 s / 1.2 MB 与 1.9 MB。

**正确性怎么保证**：C 版与 Python 版共用同一份 `config.json`；`--dry-run` 输出逐字节一致
（ssh 参数、mpv 参数、三种显示方式全对齐）；端到端实测画面正常、比例正确（0.482 ≈ 手机 9:19）。

**踩到的 C 坑**（都在 README 的坑列表里）：D-Bus 父子迭代器混用、`DBUS_TYPE_STRING` 传值语义、
libdbus 1.16 删掉 unix fd 公开 API（改用 `fcntl` 试探值本身是不是 fd）、GStreamer 参数单 token、
以及"用 gcc 编译时别把输出管道给 `head`"（SIGPIPE 会让编译静默失败）。

## TUI 也搬进 C 之后（2026-10-01）

- 一个二进制搞定：`mirror-screen` 不带参数 = ncurses 配置界面；带参数 = `--run/--dry-run/--print-config/--wake/--check/--list-hosts`。
  TUI 里 Ctrl-R 开播时会 `fork + setsid` 出自己（`--run`）再退出，于是终端窗口关闭、投屏在后台跑 —— 与 Python 版行为一致。
- **常量单一来源**：模式名、档位列表、远端体检/探测脚本、默认配置都从 `mirror-screen.py` 自动导出成 `ms_consts.h`
  （`gen-consts.py`，安装时生成），C 侧不手抄，避免两边漂移。
- **配置互操作**：两版读写同一份 `config.json`，实测 C 存 → Python 读、Python 存 → C 读都正常，
  字段逐字段一致（含布尔/字符串类型）。
- 顺手修了一个 Python 版就有的 bug：`is_stream` 原本写成 `== MODE_STREAM`，导致 **portal 模式下看不到
  fps/缩放/硬解/远端客户端/锁定窗口尺寸等专用选项**（它们只在 wf-recorder 模式出现）。改成 `!= MODE_WAYPIPE`，两版一致。
