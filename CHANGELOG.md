# 更新记录

## 1.1.0 — 2026-09-30

热路径 C 重写（功能不变）：

- 新增 `portal_cast.c`（远端 portal 客户端，libdbus）：25.3 MB → **1.2 MB**，启动 0.41 s → **0.10 s**；
  首次开播在远端自动编译，失败自动回退 Python 版（TUI 可选 auto/c/python）。
- 新增 `mirror-screen-c.c`（本机监管引擎）：27.9 MB → **1.9 MB**，启动 77 ms → **1 ms**；
  TUI 的 `--run` 会 exec 它，失败回退 Python 实现。`--dry-run` 输出与 Python 版逐字节一致。
- `install-mirror-screen.sh` 会编译并安装 C 引擎，同时安装远端客户端源码（供同步/远端编译）。


## 1.0.0 — 2026-09-30

首个版本。功能与实测结论：

- `portal` 零编码投屏（默认）：xdg-desktop-portal → PipeWire → GStreamer（只转换/缩放）→ ssh → mpv。
  远端 CPU 1%（静止）/ 15%（连续变化），延迟 ~115 ms，静止时链路仅 80 KB/s。
- `stream` 模式：远端 `wf-recorder` 编 H.264 → ssh → mpv（26–35% CPU，延迟 75–120 ms）。
  已知局限：`-r` 的 fps 滤镜只排队不丢帧，长时间会漂移（实测 0 ms → 57 s）。
- `waypipe` 模式：转发远端单个应用（**不能**镜像远端屏幕，已在文档中标注）。
- TUI 配置界面 + `--run / --check / --dry-run / --wake / --print-config` 命令行。
- ssh 连接复用（ControlMaster）：开播 2.0 s → 0.50 s。
- 投屏窗口显示三档，半幅窗口尺寸由 niri 规则 + `tiled-state true` 钉死（810×1013，与邻窗齐平，焦点/工作区切换不变）。
- `remote-mirror-prep.sh` 远端体检/安装；`niri-cast-rule.sh` niri 规则装/卸/查。
- 文档：`docs/research-airplay-and-linux-casting.md`（生态调研与方案对比）、`docs/evidence-measurements.md`（逐条实测）、`NOTES.md`（实现取舍）。
