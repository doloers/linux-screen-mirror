# Linux 上 AirPlay 开发情况调研 — 归档

**时间**：2026-09-30
**归档位置**：`~/AIworks/01-技术研究/2026-09-30_AirPlay-Linux生态调研/`
**起因**：承接 [`2026-09-28_投屏与网络栈迁移`](../2026-09-28_投屏与网络栈迁移/)——那条线卡在 doubletake → 小米电视的最后一跳，需要一个"整体生态是什么状态、还有没有别的路"的判断
**结果**：定性清楚 —— **Linux 当 AirPlay 接收端已经工业级成熟；当发送端（Linux → Apple TV/电视）是整个生态最薄的一环，全球实际上只有一个活跃项目**。本调研为**只读**（未改动本机任何配置、未装任何包）

---

## 一、一句话结论

| 方向 | 能力 | 代表项目 | 状态 |
|---|---|---|---|
| **收** | AirPlay 2 音频（多房间 / 48k 无损 / 5.1+7.1） | `shairport-sync` 5.5.x + `nqptp` | ✅ **生产可用**，Arch 官方仓库有包 |
| **收** | 屏幕镜像 + 音频（手机/电脑 → Linux 桌面） | `UxPlay` 1.73.7 | ✅ 可用（走 AirPlay 2 的 **legacy** 协议） |
| **收** | HLS 视频（YouTube 类） | `UxPlay -hls` | ⚠️ 实验性、仅 YouTube |
| **收** | AirPlay 2 参考实现（多房间） | `openairplay/airplay2-receiver`（Python） | ⚠️ 实验，但协议覆盖最全（唯一 FairPlay v3 Python 实现） |
| **发** | 桌面音频 → 音箱/电视（RAOP = AirPlay 1） | PipeWire `module-raop-sink` / `OwnTone` | ⚠️ PipeWire 在 Arch **没编译进 RAOP**；OwnTone 自带一套可用 |
| **发** | 桌面**镜像** → Apple TV / 电视 | `doubletake` | ⚠️ **唯一活跃实现、单人维护、对非 Apple 接收端兼容性差** |
| **发** | 音源多房间分发到 AirPlay 1+2 | `OwnTone` | ⚠️ 默认优先 RAOP，不支持 speaker group |
| **控制/自动化** | 配对、播放 URL、遥控 | `pyatv` | ✅ 成熟（但**不做桌面镜像**） |

**核心不对称**：接收端（别人投给 Linux）成熟，发送端（Linux 投给别人）单薄。原因不是没人做，而是**协议与法律门槛在发送侧**（见 §五）。

---

## 二、为什么偏偏是"收强发弱"（机制层面）

| 环节 | 接收端 | 发送端 |
|---|---|---|
| 配对 | 对方（iOS/macOS）来配对，完成 HAP/SRP-6a 后**把流密钥交给你**，你只管解密播放 | 你要主动向接收端**证明自己是合法发送者**：FairPlay（SAP）认证 |
| FairPlay | 只需解密 AES 密钥（v3 已逆向） | 历史上只能**执行 Apple 抽取出来的 ARM64 二进制**；doubletake 现在的 README 自称已是"clean Go implementation"，但这块依然是全生态最脆、最有法律灰度的部分 |
| MFi | 官方认证接收端需要 MFi 硬件模块 —— `airplay2-receiver` 明确写 "**may never implement** MFi Authentication (requires MFi hardware module)" | 同左，第三方永远拿不到 |
| 规范 | 逆向资料够用（AirPlay 1 有非官方规范） | AirPlay 2 **官方规范从未公开**，全靠逆向 |
| 结论 | 逆向者只需"照着收" | 逆向者要过 Apple 的加密握手 + 面对各家接收端自己的私有实现差异 |

> 独立佐证：GNOME 投屏面板项目 `kast` 的原话 —— "AirPlay live screen mirroring *out* … needs FairPlay/MFi authentication, and **the only Linux sender runs extracted Apple code**."

---

## 三、接收端（Linux 当接收者）——成熟区

| 项目 | 语言 | ★ | 最近活跃 | 最新版本 | 能做什么 / 关键限制 |
|---|---|---|---|---|---|
| [`mikebrady/shairport-sync`](https://github.com/mikebrady/shairport-sync) | C | 8874 | 2026-09-27 | **5.5.2**（2026-09-14） | AirPlay 1 + AirPlay 2 **音频**：48k 无损立体声、5.1/7.1（AP2 buffered）、FFmpeg 转码、MQTT/D-Bus、AirPlay 2 可加进 Home App。限制：**不支持 96/192k、Dolby Atmos、Windows iTunes**；无远程控制（development 分支实验性）；**同一 IP 不能跑多个 AP2 实例**；AP2 计时依赖 `nqptp` 独占 UDP 319/320；虚拟机/蓝牙下计时差 |
| [`FDH2/UxPlay`](https://github.com/FDH2/UxPlay) | C | 3116 | 2026-09-29 | **1.73.7**（2026-09-04） | AirPlay **镜像**接收 + ALAC 音频 + HLS(YouTube)；GStreamer 渲染，可硬解（v4l2/vaapi）；1.73.7 修了 `lib/raop_handlers.h` 的安全公告 **GHSA-479c-ww7g-wgp8**，并适配 **iOS 27 改了 TEARDOWN 行为**；Apple 视频 DRM 无法解密 |
| [`openairplay/airplay2-receiver`](https://github.com/openairplay/airplay2-receiver) | Python | 2423 | 2026-06-29 | — | 实验性但**协议覆盖最全**：HomeKit transient/持久配对、FairPlay v3、REALTIME + BUFFERED、ALAC/AAC/OPUS/PCM、RTCP、RFC2198 冗余；**无精确 PTP/NTP 同步** |
| [`mikebrady/nqptp`](https://github.com/mikebrady/nqptp) | C | 158 | 2026-09-27 | — | "Not Quite PTP"：AirPlay 2 同步的必需守护进程 |
| [`FD-/RPiPlay`](https://github.com/FD-/RPiPlay) | C++ | 5224 | **2023-04-14** | — | 历史项目（OpenMAX/树莓派），**已停更**，功能被 UxPlay 取代（V4L2+GStreamer） |
| [`philippe44/AirConnect`](https://github.com/philippe44/AirConnect) | C | 4186 | 2026-09-28 | — | **反方向桥接**：把 AirPlay 音源转发给 UPnP/Sonos/Chromecast 设备 |
| Rust 新一波 | Rust | 均 <50 | 2026-07~09 | — | [`metaneutrons/shairplay-rust`](https://github.com/metaneutrons/shairplay-rust)（LGPL，`#![forbid(unsafe_code)]`、AP1+AP2 buffered、多声道）、[`r4v3n6101/rairplay`](https://github.com/r4v3n6101/rairplay)（GPL）、[`st3fan/openairplay2`](https://github.com/st3fan/openairplay2)（MIT）——**很小很新，但说明 AP2 接收端正被重写成 Rust** |
| [`sijow/gst-airplay`](https://github.com/sijow/gst-airplay) | C | 96 | 2024-03-03 | — | GStreamer `airplaysrc`（只收视频、不收音频），已停滞 |

**本机相关**：`shairport-sync` 与 `nqptp` 都在 **Arch 官方 extra 仓库**（分别为 `5.0.4-3`、`1.2.8-1`）——接收端的安装成本基本等于零。注意官方仓库的 5.0.4 落后上游 5.5.2 若干个小版本。

---

## 四、发送端（Linux 当发送者）——薄弱区，也是你那条线的所在

| 项目 | 语言 | ★ | 最近活跃 | 形态与限制 |
|---|---|---|---|---|
| [`omarroth/doubletake`](https://github.com/omarroth/doubletake) | Go | 105 | 2026-09-29 | **Linux 唯一活跃的 AirPlay 镜像外发实现**。LGPL-3.0。最新发布 **v0.4.0（2026-07-12）**，main 已加 **HEVC、音频重传、延迟自适应**（2026-08-24）。Wayland(PipeWire/portal)+X11 抓屏，NVENC/VA-API/x264/OpenH264 编码，ChaCha20-Poly1305，FairPlay SAP，SRP-6a 配对 + 凭据持久化，daemon 模式 + `doubletake-ctl`，KDE plasmoid。**需要接收端反向连回 3 个 UDP 端口 + 1 个 TCP 事件通道**，否则接收端静默挂住。已测设备含 AppleTV3,2 / 4K 2nd/3rd、Roku、三星、海信；**已知不可用：Xiaomi 4K HDR TV（issue #4）** |
| [`mrCode/castr`](https://github.com/mrCode/castr) | Go | 0 | 2026-09-27 | Hyprland 外壳（mirror/extend、waybar 指示）。**实测经验很值钱**：v0.4.0 在 Hyprland 上抓屏不可用（要 `doubletake-git`）；**目标延迟 <80ms 时接收端会中途挂断**（Apple TV 在 50ms 约 30 秒断流，100ms 全程稳定） |
| [`asuramaya/kast`](https://github.com/asuramaya/kast) | Shell | 8 | 2026-09-05 | GNOME 的"Win+K 投屏面板"（AirPlay+Miracast+Chromecast 一个入口）。**仍是胶水**：外发音频用 PipeWire RAOP、放文件用 pyatv、收屏用 uxplay、收音用 shairport-sync；**明确承认 AirPlay 镜像外发做不到** |
| [`owntone/owntone-server`](https://github.com/owntone/owntone-server) | C | 2562 | 2026-09-24 | 媒体服务器（原 forked-daapd）。可推向 **AirPlay 1+2** 接收端、支持多房间"一次推多台"，但**默认优先 RAOP（AirPlay 1）**；维护者明说：AP2 输出只有 NTP 计时，而 shairport-sync 的 AP2 只支持 PTP，**两者不兼容**；**不支持 speaker group**；可用 `shairport-sync → 命名管道 → OwnTone` 搭多房间"路由器" |
| PipeWire `module-raop-sink` / `module-raop-discover` | C | — | 上游活跃 | 桌面音频层的 RAOP 外发（**= AirPlay 1**）。可手工指定 `raop.ip`/`raop.port`（不依赖 mDNS）；文档列 `raop.audio.codec = PCM/ALAC/AAC/AAC-ELD`、`raop.encryption.type = none/RSA/auth_setup/fp_sap25`；1.4.6 起可用 context.property 关闭 RAOP。**但本机 Arch 打包没有编译 RAOP（实测见 §六）** |
| [`TurkFork/tuxplay`](https://github.com/turkfork/tuxplay) | Go+Rust | 0 | 2026-03-16 | 在 PipeWire RAOP 之上的"控制面"（发现、路由、音量、分组；GTK4 GUI）。很新，尚不成熟 |
| [`postlund/pyatv`](https://github.com/postlund/pyatv) | Python | 1177 | 2026-08-14 | 配对（含 HAP）/播放 URL/遥控，**控制层不是镜像层** |
| 历史/停滞 | — | — | — | `hfujita/pulseaudio-raop2`（143★，2018 停更，已并入 PA 11 → Arch 现在拆成 `extra/pulseaudio-rtp`）；`shairplay`（AUR 包是 2018 快照）；`airplay-rs`（Rust 发送端库，0.0.1） |

---

## 五、协议与知识源（做二次开发该看什么）

| 资料 | 覆盖 | 备注 |
|---|---|---|
| [`openairplay/airplay-spec`](https://github.com/openairplay/airplay-spec)（Unofficial AirPlay Specification） | **AirPlay 1 / RAOP** | 85★，2022-01 之后未更新；AirPlay 1 已可视为"公开知识" |
| [pyatv 协议文档](https://pyatv.dev/documentation/protocols/) | HAP 配对（PS/PV 消息表、TLV8/OPACK）、各设备的配对需求判定 | 做配对逻辑最实用的参考 |
| [Emanuele Cozzi · AirPlay 2 Internals](https://emanuelecozzi.net/docs/airplay2) | AP2 音频、RTSP/SETUP、编解码、多房间 | 作者自述"边逆向边写"，聚焦音频 |
| shairport-sync `AIRPLAY2.md` | **AP2 实际能/不能做什么** | 想了解"官方之外的 AP2 能力边界"看这份最省事 |
| 关键事实 | ① AirPlay 2 规范从未公开；② FairPlay **v2** 社区未实现；③ **MFi 需要 Apple 授权硬件模块，永远不可能开源**；④ AP2 同步靠 PTP（UDP 319/320），RAOP 靠 NTP | 这四条决定了"能做到什么程度"的天花板 |

---

## 六、本机实测（Arch / 2026-09-30，只读核对）

原始输出见 [`evidence/local-check.txt`](evidence/local-check.txt)。

| 检查 | 结果 |
|---|---|
| `pacman -Ss 'airplay\|nqptp'` | 只有 `extra/shairport-sync 5.0.4-3`、`extra/nqptp 1.2.8-1`（上游已到 5.5.2） |
| AUR | `uxplay 1:1.73.7-1`（2026-09-04）、`doubletake 0.4.0-3`（2026-09-29 刚更新）、`doubletake-git 0.4.0.r35.gae06722-1`、`shairplay`（2018 快照） |
| PipeWire 1.6.9 的 RAOP | **不存在**：`pacman -Ql libpipewire \| grep -i raop` 为空、`find /usr/lib -name '*raop*'` 为空、`ldd libpipewire` 里**没有 openssl/avahi**（⇒ Arch 打包时就没编 RAOP），也没有对应 man 页；只有 `pipewire` 包里的 `/usr/share/pipewire/pipewire.conf.avail/50-raop.conf` 这份"可用但无模块"的配置片段 |
| 结论 | "桌面音频投到电视"这条路**在本机开箱不通**：要么自己重编 `libpipewire`（开 RAOP），要么绕开 PipeWire 用 `OwnTone`（自带发送栈）/`pyatv` |
| 网络前提 | 本网络 `paru` 不可用（TLS 中间设备挑 ALPN），AUR 只能手工 `makepkg`（详见 2026-09-28 那份研究） |

---

## 七、对那台小米电视的意义（承接 issue #56）

你现在的位置，正好落在这个生态**最薄的一格**：唯一活跃的发送端（doubletake）× 最不可控的接收端（伪装成 `AppleTV3,2` 的国产电视）。

- `doubletake` 的 issue **#56 至今 open、0 回复**（2026-09-30 复核），且 main 分支 2026-08-24 之后无新提交 —— 短期内不要指望上游。
- 社区里所有"成功案例"都是 **真 Apple TV 或已知品牌电视**；`doubletake` 的已知失败清单里本来就有一台小米（issue #4）。
- 按性价比排序的三个下一步：

| # | 动作 | 能得到什么 | 成本 |
|---|---|---|---|
| 1 | **借一台真 Apple TV 跑一遍 doubletake** | 把"doubletake 有问题"和"电视有问题"彻底分离。这是唯一能给出定论的动作 | 借机器 |
| 2 | 用 **`doubletake-git`（main）** 再试，并**显式给 `-target-latency-ms 100`** | main 已有 HEVC/音频重传/延迟自适应；`castr` 作者实测 **<80ms 会让接收端中途挂断**，你现在没显式指定 | 低 |
| 3 | 用现成的 `tools/probe-host.py` 查这台电视**是否还广播 `_raop._tcp`** | 若广播：`OwnTone`（自带 RAOP/AP2 发送栈，与 PipeWire 无关）是唯一"不重编系统、不用 doubletake"的**音频**路径，值得一试；若只广播 `_airplay._tcp`：说明这台电视压根没打算接受第三方音频流，音频这条线可直接判死（省掉后续所有折腾） | 低 |

- **预期管理**：即便镜像某天通了，AirPlay 镜像外发始终是"逆向 + 未认证"路径 —— 客户端一变就可能回归（例：**iOS 27 改了 TEARDOWN 行为**，UxPlay 要到 1.73.7 才跟上；UxPlay 自己也长期提醒"legacy 协议随时可能被 Apple 移除"）。如果目的只是"把画面放到电视上"，**Miracast / DLNA / Chromecast / HDMI 的长期稳定性都优于 AirPlay 外发**。

---

## 八、附：另一台 Linux 投屏到本机（Linux→Linux 实操方案）

**一句话**：Linux 之间没有原生的统一"投屏"协议，可选的都是"一对工具对撞"。本机（AURA-ARCH / niri 26.04）实测能力与五种方案见下（原始探测输出：`evidence/linux-to-linux-probe.txt`）。

**本机相关实测**（2026-09-30）：

| 项目 | 结果 |
|---|---|
| niri 截屏协议 | ✅ `zwlr_screencopy_manager_v1`（v1 级：只有 `capture_output`）、✅ `zwp_virtual_keyboard_manager_v1`；❌ `ext-image-copy-capture`、❌ `wlr-export-dmabuf`、❌ `wlr-output-management` |
| 本机已装 | `mpv`、`grim`/`slurp`、`openssh`（sshd enabled+active）、**UxPlay 1.73.7**（AirPlay 镜像接收端） |
| 官方仓库可装 | `wayvnc`(210K)、`waypipe`(2.0M)、`wl-mirror`(128K)、`wf-recorder`(122K)、`tigervnc`(7.0M)、`moonlight-qt` |
| 仅 AUR | `sunshine`(84 votes)、`gnome-network-displays`(47 votes，**只是发送端**) |

| 方案 | 方向 | 对方（源）要装 | 本机（收）要装 | 能操作对方 | 适合 |
|---|---|---|---|---|---|
| **A. AirPlay**：doubletake → 本机 UxPlay | **推** | doubletake（AUR；或 `-bin`） | 已有 ✅ | ❌ 只看 | 要"投屏"的原味，与手机投屏同一套 |
| ~~**B. waypipe + wl-mirror**~~ ❌ **原理不成立** | — | — | — | — | 见下方更正：waypipe 只能“把远端**应用**拿到本机跑”，**不能**镜像远端屏幕（2026-09-30 实测）|
| **C. wayvnc + VNC 客户端** | 拉 | `wayvnc` | `tigervnc` | ✅ 鼠标键盘 | 要远程操作那台机 |
| **D. Sunshine + Moonlight** | 拉 | `sunshine`（AUR） | `moonlight-qt` | ✅ | 游戏级流畅度 + 音频 |
| **E. wf-recorder 推流 + mpv 收** ⭐**已实测可用** | 推 | `wf-recorder` + `wlr-randr`(+`grim` 自检) | `mpv` ✅ | ❌ 只看 | **推荐**：无需 GPU/Vulkan，手机/SBC 也能用 |

### A. AirPlay（与本机现状一致，唯一"推"式投屏）

```bash
# —— 对方（源设备）：
#   AUR 装 doubletake（或 doubletake-bin；注意 v0.4.0 有已知解析问题，优选 -git/main）
doubletake -target 172.16.60.184 -port 7100 -pin 1234 -hwaccel vaapi

# —— 本机（接收）：UxPlay 的端口必须固定下来
pkill -x uxplay
setsid nohup stdbuf -oL -eL uxplay -n "AURA-ARCH" -nh -vs waylandsink -p 7100 -pin 1234 \
  >~/aur-builds/uxplay-run.log 2>&1 &   # -p 7100 ⇒ TCP/UDP 7100/7101/7102
```

三个要点：① **UxPlay 默认随机端口**，走 AirPlay 必须 `-p` 固定；② 用 `-pin 1234`（Apple 式 PIN 配对 = SRP-6a，正好是 doubletake `-pin` 期待的那条通道）——而这个 `-pin` 对 **iOS 端**会诱发"输入隔空播放密码"对话框（§七 踩过），对 doubletake 无此问题；③ doubletake 需要**接收端反向连回**它的 3 个 UDP + 1 个 TCP 端口，对方防火墙要放行（`-port-range 60000-60010`）。
**未验证项**：doubletake ↔ UxPlay 属"社区实现 × 社区实现"，两边 README 的已测设备表都没列对方 ⇒ 需实测（自测办法见本节末）。

### B. ~~waypipe + wl-mirror~~ —— ⚠️ **更正：这条路不能镜像远端屏幕**

> **2026-09-30 实测更正**：我最初把这条写成“把对方的屏投过来”，**那是错的**，抱歉。
> waypipe 的工作方式是“**应用在远端、显示在本机**”；远端程序看到的其实是**本机** compositor。
> 决定性证据（把 `wayland-info` 直接作为 waypipe 的命令跑，不套 `sh -c`）：
>
> ```
> interface: 'zwlr_screencopy_manager_v1', version: 3     ← 有 screencopy，但是本机的
> interface: 'wl_output', name: eDP-1
> 	name: eDP-1
> 	description: Samsung Display Corp. - ATNA40HQ09-0 - eDP-1   ← 本机面板，不是远端屏
> ```
>
> 所以 `wl-mirror <远端输出名>` 必失败：`error: options::find_output(): output DSI-1 not found`。
> **waypipe 仍有价值，但用途是**“把远端的**应用**拿到本机跑”（如 `waypipe --no-gpu -c lz4 ssh 对方 foot`），不是屏幕镜像。

保留的两条实测结论仍有效：① 本机没装 Vulkan 驱动时 waypipe 会因 DMABUF 通道直接崩（需 `--no-gpu` 或装 `vulkan-intel`）；② `--no-gpu` 只需加在本地 client 侧（证据：`evidence/waypipe-transport-selftest.txt`）。

### C. wayvnc + VNC 客户端（要操作对方）

```bash
# 对方：sudo pacman -S wayvnc && wayvnc            （默认监听 5900）
# 对方若报采集错误，强制回退到 wlr-screencopy（niri 没有 ext-image-copy-capture）：
WAYVNC_NO_EXT_IMAGE_COPY=1 wayvnc
# 本机：sudo pacman -S tigervnc && vncviewer 对方IP::5900
```

niri 有 screencopy + `zwp_virtual_keyboard` ⇒ 机制上可行（社区反馈回退到 wlr-screencopy 后正常）。安全：VNC 明文，只在内网用或加 `ssh -L 5900:localhost:5900` 隧道。

### D. Sunshine + Moonlight（最流畅、唯一带音频的开箱方案）

```bash
# 对方：yay -S sunshine（或 sunshine-bin，AUR）  本机：sudo pacman -S moonlight-qt
```

优点：H.264/HEVC 硬编 + 硬解、高帧率、**自带音频流**、延迟最低。代价：配置最多；Sunshine 的 KMS 抓屏需 `setcap cap_sys_admin+p`，或改用 XDG portal 抓屏（niri 官方主推 portal，本机 portal 已配好）。

### E. wf-recorder 推流 + mpv 收 ⭐（推荐，2026-09-30 实测打通）

思路：远端采集+编码 → 管道 → 本机 mpv 解码显示。

```bash
# 远端需：wf-recorder、wlr-randr（唤醒屏幕）、grim（自检）    本机需：mpv（已装）
# 在【本机】执行：
ssh -o BatchMode=yes 用户@远端 \
  'wf-recorder -y -o DSI-1 -c libx264 -x yuv420p -r 20 -F "scale=540:-1" -m h264 -f /dev/stdout' \
  | mpv --profile=low-latency --no-config --hwdec=vaapi --demuxer-lavf-format=h264 \
        --force-window=yes --fs \
        --demuxer-readahead-secs=0 --demuxer-max-bytes=64KiB --untimed --video-sync=desync -
# 远端息屏时抓屏必然失败（grim: no supported format found / wf-recorder: Failed to copy frame）
#   先唤醒：ssh 用户@远端 'wlr-randr --output DSI-1 --on'
# 延迟三件套（实测）：① 远端关 WiFi 省电（RTT 94.7→10.7ms）
#   ② 用裸流 -m h264（mpv Cache 0.50→0.00s）③ mpv --untimed --video-sync=desync
```

**实测结果**（OnePlus 6 / postmarketOS / Phosh → 本机 niri）：1080x2280@20fps 正常显示；
本机 mpv 走 VA-API 硬解仅 **2.6% CPU**；手机侧 libx264 约 **27% 单核**，静态画面码率约 **180 kbit/s**。
画面用 mpv 自身截图核实（帧里是手机 Phosh 桌面，非本机屏幕）。
坑：`wf-recorder -f -` **不支持 stdout 写法**（会被当成文件名、后面还问 Overwrite），要用 `-f /dev/stdout` 并**加 `-y`**。
坑2：本机 mpv **必须 `--hwdec=vaapi`**：装了 `vulkan-intel` 后 `auto-safe` 会挑 Vulkan 解码，而本机 Mesa 没有
`VK_KHR_video_decode_queue` → 逐帧报 `Device does not support the VK_KHR_video_decode_queue extension!` 然后 `no frame!`。
坑3：`wf-recorder` 是 **damage 驱动**的 —— 远端屏幕完全静止时几乎不发帧，mpv 看着像卡住其实正常（动一下屏幕即有画面）。
补充（实测）：**“压缩换速度”是伪命题** —— 链路上码率只占 0.07%，把压缩调狠只会让远端 CPU 翻倍（35%→66%）、延迟无改善；每帧工作量（分辨率/帧率/preset）才是延迟杠杆。
体验：本 TUI 开投屏时会**把配置界面（终端窗口）一起关掉**，屏上只留投屏窗口；已实现「**运行时完全静默**」（远端 `2>/dev/null` + mpv `--really-quiet --no-terminal`，日志写文件）
与「**关掉 mpv 窗口即退出**」（并兑底 `pkill` 清理远端编码器）；两份 README 有详细说明。

**补充（2026-09-30 深夜，当前 TUI 默认已是 portal 零编码模式）**：方案 3「`xdg-desktop-portal` ScreenCast → PipeWire →
`gst-launch-1.0`（只 videoconvert/videoscale，**不编码**）→ ssh → mpv」已落地：手机 CPU 从 27–35% 降到
**1%（静止）/ 15%（连续变化）**，延迟 ~115 ms，且没有 wf-recorder `-r` 那种队列漂移（实测延迟 0 ms → 57 s）。
窗口显示默认改为「**半幅窗口（浮动）+ 画面顶满**」：实测 mpv 的 `--geometry` 在 Wayland 下无效；平铺窗口在 niri 里
会被每次焦点变化按「当时的可用高度」重算（997），而旁边窗口停在旧值（1013）⇒ 视觉上永远矮一截。
最终查明是 **mpv 自己按视频尺寸吸附窗口**，故在 niri 规则里加 `tiled-state true`（与 foot 那条规则同源），
配合 mpv `--wayland-app-id=portal-cast`；**仍是平铺**，实测窗口恒 810×1013、与邻窗齐平、焦点/工作区切换不变。细节与脚本见 `~/AIworks/02-本机运维/README.md` §六。

### 排除掉的路

- **Miracast**：Linux 侧能做接收端的只有 MiracleCast（不在官方仓库、不稳），`gnome-network-displays`（AUR）只是发送端 ⇒ Linux→Linux 走 Miracast 基本可判死（与 §七 小米电视那条同因）。
- **Chromecast / DLNA**：只支持播"文件/URL"，不支持桌面镜像。

### 怎么选（一句话版）

只想看对方画面 → **E**（wf-recorder → mpv，已实测）；还要鼠标键盘 → **C**（wayvnc）；要流畅视频/游戏/声音 → **D**；要“投屏”原味（同一套投屏菜单体验） → **A**；要把远端**应用**拿到本机跑 → **B**（waypipe，**不是**屏幕镜像）。

### A 方案的自测办法（不需要第二台机器）

在本机装 `go` 并编译 doubletake（源码构建约几分钟，AUR `doubletake-bin` 亦可），然后 `doubletake -target 127.0.0.1 -port 7100 -pin 1234` 投给本机的 UxPlay：**只要能出画面，就说明 Linux→Linux 的 AirPlay 链路成立**（环路自测，画面是"自己投自己"）。

---

## 九、来源

**仓库/版本数据**：GitHub REST API 实时查询（`stars` / `pushed_at` / `releases`），AUR RPC v5，本机 `pacman`，核对时间 **2026-09-30**（原始输出见 `evidence/local-check.txt`）。
**文档**：shairport-sync `AIRPLAY2.md` / `RELEASENOTES.md`、UxPlay `README.txt` 与 release notes、pyatv 协议文档、PipeWire `module-raop-sink` / `module-raop-discover` 文档、openairplay `airplay-spec` 与 `airplay2-receiver` README、OwnTone 文档与 issue #1882/#1413、doubletake README 与 PR #9/#13、castr / kast README、ArchWiki（Shairport Sync / OwnTone）。

**本调研的边界**：只做资料与公开数据核对，**未在本机安装/启用任何 AirPlay 组件**，未改动既有投屏成果目录；结论以 2026-09-30 的生态状态为准（该领域 6~12 个月就会显著变化，尤其是发送端）。
