# 实测：Linux→Linux（手机 OnePlus 6 → 本机）屏幕投送 — 2026-09-30 19:35 CST

## 0) 设备真相
  远端 192.168.1.x = OnePlus 6 (oneplus-enchilada)，postmarketOS edge (Alpine 系, apk)，aarch64，内核 7.1.0-rc1-sdm845
  桌面 = Phosh on phoc（XDG_CURRENT_DESKTOP=Phosh:GNOME, WAYLAND_DISPLAY=wayland-0，合成器 = /usr/bin/phoc）
  DRM 输出名 = DSI-1（1080x2280，wlr-randr/wayland-info 均确认）；无 PCI 显卡、无 VA-API
  本机 = niri 26.04 / eDP-1；本机侧只用到 mpv（已装）

## 1) 决定性证据：waypipe 不能镜像远端屏幕（我先前方案的错误）
  $ waypipe --no-gpu -c lz4 ssh user@192.168.1.x wayland-info | grep -E "wl_output|screencopy|name:"
    interface: 'zwlr_screencopy_manager_v1', version: 3
    interface: 'wl_output', name: eDP-1
    	name: eDP-1
    	description: Samsung Display Corp. - ATNA40HQ09-0 - eDP-1     ← 本机面板，不是远端 DSI-1
  ⇒ waypipe 的 app 看到的是【本机】compositor；因此 wl-mirror <远端输出名> 必然失败：
    error: options::find_output(): output DSI-1 not found

## 2) 第一大坑：远端息屏 → screencopy 全灭（现象极像协议/权限问题）
  $ cat /sys/class/drm/card0-DSI-1/dpms      → Off
  $ cat /sys/class/drm/card0-DSI-1/enabled   → disabled
  $ cat /sys/class/backlight/ae94000.dsi.0/bl_power → 4 (FB_BLANK_POWERDOWN)
  $ grim -o DSI-1 /tmp/x.png                 → "no supported format found"
  $ wf-recorder -o DSI-1 ...                 → "Failed to copy frame, retrying... too many times, exiting!"
  $ wl-mirror -b screencopy DSI-1 (在手机上)  → mirror-screencopy::backend_cancel(): cancelling capture due to error
  唤醒：$ wlr-randr --output DSI-1 --on  → dpms=On enabled=enabled bl_power=0；grim 立刻恢复（221540 字节 PNG）

## 3) 打通的链路与实测数据
  $ ssh -o BatchMode=yes user@192.168.1.x \
      "wf-recorder -o DSI-1 -c libx264 -x yuv420p -r 20 -m mpegts -f /dev/stdout" | \
    mpv --profile=low-latency --no-config --hwdec=auto-safe --demuxer-lavf-format=mpegts --force-window=yes -
  · niri 窗口出现：Title "PHONE-MIRROR"/"MIRROR:user@192.168.1.x"，App ID mpv
  · 本机 mpv CPU 2.6%（VA-API 硬解），RSS 230 MB
  · 手机端 wf-recorder 27% 单核，load avg 1.39；手机 wlan0 tx ≈ 22.8 KB/s（≈183 kbit/s，静态画面）
  · 画面核实：让 mpv 自己截帧（IPC: screenshot-to-file）得到 1080x2280 PNG，内容是手机 Phosh 桌面（pi 终端界面），
    见 evidence/phone-mirror-frame.png（已归档）
  · 手机 WiFi: SSID doloers, -51 dBm, 400 Mbit/s VHT；但同网段 ping RTT 113~603 ms（手机侧省电/调度所致，
    不影响功能，但影响交互延迟）

## 4) sshd 转发开关（waypipe 模式才需要，但排错时也撞上了）
  远端 sshd 原配置：AllowTcpForwarding no（Alpine 默认）→ waypipe 报
    Warning: remote port forwarding failed for listen path /tmp/waypipe-server-*.sock
  验证：手工 ssh -R 也复现 'Remote: port forwarding refused'
  修复：/etc/ssh/sshd_config 改 AllowTcpForwarding yes + 追加 StreamLocalBindUnlink yes，
        sshd -t 通过后重启（postmarketOS 用 systemd：systemctl restart sshd；无 OpenRC rc-service）
  备份：远端 /etc/ssh/sshd_config.bak-20260930-192626
  修复后：ssh -R 测试通过（远端 socket 建立成功）

## 5) 新工具链自检输出（本机侧）
  $ mirror-screen --check   （体检远端，含抓屏实测）
    compositor=phoc/Phosh  display=wayland-0  wfrecorder=yes  wlrrandr=yes  grim=yes
    vulkan=1  drmdpms=On  capture=221164        ← capture 是真实抓屏字节数
  $ mirror-screen --wake    → On
  $ mirror-screen --dry-run → ssh ... wf-recorder -o DSI-1 ... | mpv --demuxer-lavf-format=mpegts -
  $ ssh user@手机 "sudo bash -s -- --check" < remote-mirror-prep.sh   （新增 apk/Phosh/息屏检测）
    [OK] 桌面 = phoc (Phosh)   [OK] 输出名 = DSI-1   [OK] DPMS = On   [OK] grim 抓屏成功
  $ bash remote-mirror-prep.sh --check   （本机自测，验证 niri/无 wlr-randr 分支）
    [OK] 桌面 = niri   [OK] 输出名 = eDP-1   [OK] DPMS = On   [OK] grim 抓屏成功（1128879 字节）

## 6) 追加：wf-recorder 的覆盖确认坑（用户实际踩到）
现象：`Output file "/dev/stdout" exists. Overwrite? Y/n:` 卡住；用户按 Y 时 niri 回
      `No key binding found for key 'Y'`（按键没送进等待输入的进程）。
源码（ammen99/wf-recorder src/main.cpp）：
    if (stat(filename) == 0 && !S_ISCHR(buffer.st_mode)) {   // 字符设备豁免，管道会触发提问
        std::cerr << "Output file ... exists. Overwrite? Y/n: ";
        std::getline(std::cin, input);
    }
  ⇒ `/dev/stdout` 在管道场景是 FIFO → 触发提问；只有 stdout 是 tty（字符设备）才不问。
为什么早先"能跑"：当时 stdin 是 /dev/null，getline 立刻 EOF、空串被判为"是"，纯属侥幸。
修复：加 `-y` / `--overwrite`（源码里就是"Force overwriting the output file without prompting"）。
验证：把 stdin 接成一个永不结束的管道（sleep 60 | ...），日志中 Overwrite 出现 0 次，画面立即开始播。

## 7) 追加：本机 mpv 的 hwdec 陷阱（装 vulkan-intel 的副作用）+ damage 行为
现象（用户遇到）：mpv 逐帧刷
  [ffmpeg/video] h264: Device does not support the VK_KHR_video_decode_queue extension!
  [ffmpeg/video] h264: no frame!
原因：mpv 的 --hwdec=auto-safe 在 Vulkan 可用时先挑 h264-vulkan；本机 vulkaninfo 里
      VK_KHR_video_decode_queue 计数 = 0（Mesa 26.2.3 / Panther Lake 未提供 Vulkan Video）。
对照实测（同一条 540x1140@20fps 流，靠在手机上循环发通知制造 damage）：
  --hwdec=vaapi     → "[vd] Using hardware decoding (vaapi)." ×1，0 报错；IPC 截图 376162 B（内容是手机屏）
  --hwdec=auto-safe → 8× "Device does not support the VK_KHR_video_decode_queue extension!" + 1× "no frame!"；
                      回退后才出图（截图 372308 B）
ffmpeg 側对照：-hwaccel vulkan 初始化失败后会回退到软解并正常解码（frame=320）；
               ffmpeg 软解同一流 frame=175~320 正常 ⇒ 流本身没问题。
修复：mpv 固定 --hwdec=vaapi（TUI 新增「本机硬解」选项，默认 vaapi）。
另一个行为：wf-recorder 是 damage 驱动的 —— 远端屏幕完全静止时几乎不发帧
（这一轮测试初期"0 帧"就是这个原因，不是解码问题）；制造屏幕变化后帧数正常。

## 8) 追加：延迟与"看不到全部屏幕"的完整实测（用户反馈后）
### 8.1 "看不到全部"= 窗口比屏幕还高
  niri: 输出 eDP-1 逻辑尺寸 1645x1028（物理 2880x1800, scale 1.75）
  mpv 窗口（无 --fs）: tile_size = 810.29 x 1708.57, window_size = 808x1706
  ⇒ 1708 > 1028，视频底部约 680 逻辑像素在屏幕外
  加 --fs 后: tile_size = 1646.29 x 1029.14 → 整屏可见
  证据图: evidence/phone-mirror-fullscreen.png（顶部状态栏到底部屏幕键盘全在画面里）
### 8.2 延迟来源逐个量化
  mpv 的 "Cache: Xs" = 已缓冲秒数（延迟下限），三组对照（手机端持续制造画面变化）:
      -m mpegts（原配置）                       中位 Cache 0.50s
      -m mpegts + mpv 降缓冲/不等待时序          中位 Cache 0.40s
      -m h264（裸流）+ mpv 降缓冲/不等待时序     中位 Cache 0.00s   ← 采用
  WiFi 省电（手机 wlan0, 30 个包 ping 192.168.1.x）:
      省电 on : rtt min/avg/max/mdev = 2.5/94.7/256.2/70.8 ms
      省电 off: rtt min/avg/max/mdev = 1.8/10.7/ 53.1/ 8.7 ms   ← 采用（持久化）
  持久化做法（手机, NetworkManager）:
      /etc/NetworkManager/conf.d/wifi-powersave.conf → [connection] wifi.powersave = 2
      nmcli general reload; nmcli device reapply wlan0   （不断 SSH）
      备份: /etc/NetworkManager/conf.d.bak-20260930-201951
### 8.3 采用的最终命令（本机侧）
  ssh -o BatchMode=yes user@192.168.1.x \
    'wf-recorder -y -o DSI-1 -c libx264 -x yuv420p -r 20 -F "scale=540:-1" -m h264 -f /dev/stdout' \
    | mpv --profile=low-latency --no-config --hwdec=vaapi --demuxer-lavf-format=h264 \
          --force-window=yes --fs \
          --demuxer-readahead-secs=0 --demuxer-max-bytes=64KiB --untimed --video-sync=desync -
  复测：Cache 中位 0.00s，错误 0，窗口全屏 1646x1029

## 9) 追加：运行时报文与退出行为（用户要求）
需求：① 跑投屏时不刷程序报文；② mpv 窗口一关，程序自动退出。
实现：
  · 远端命令追加 `2>/dev/null` → ffmpeg 报告在远端就被丢弃，不再经 ssh 回到本机
  · mpv 追加 `--really-quiet --no-terminal`（实测：零输出且照常播放）
  · run_pipeline() 用 Popen：ssh stdout=PIPE → mpv stdin；`mpv.wait()` 在窗口关闭时返回
    之后 terminate 本地 ssh，再 `ssh … pkill -x wf-recorder` 兜底清理远端编码器
  · 全部运行时报文改为写入 ~/.local/state/mirror-screen/last-run.log（含命令行与 [cleanup] 结果）
实测（niri msg action close-window 关窗口，等同用户点关闭）：
  程序退出码 = 0        终端输出 = 0 字节（完全静默）
  mpv 残留 = 0          手机 wf-recorder = 已清理
  日志尾部： [cleanup] 远端 pkill rc=1
另一个发现：远端 wf-recorder 在管道断开后通常会自己退出（下次写帧时 EPIPE），
但**屏幕静止时它可能一直挂着**（不写就不报错），所以那条 pkill 兜底是必要的；
手工验证该 pkill 有效：对真实存活的 wf-recorder 执行 → rc=0 且进程消失。

## 10) 追加：开投屏即关掉配置界面（用户要求）
实现：detach_cast() —— 用 start_new_session=True 把 `mirror-screen --run` 丢到新会话后台跑，
     TUI 自己立即 SystemExit → fuzzel 拉起的 foot 窗口随之关闭；
     父进程会等 2 秒确认子进程没立刻挂掉（挂了就把日志尾部显示在状态栏，不退界面）。
实测：
  · 子进程： ps -o pid,ppid,sid,stat → 336434 336434 Ss（会话 leader，已脱离终端）✓
  · TUI 退出 code=0 ✓   · mpv 全屏窗口出现 ✓   · 手机 wf-recorder 起来 ✓
  · 关掉 mpv 窗口后：日志出现 [cleanup] 远端 pkill rc=0，mpv 无残留、cast.pid 已删、手机端已清理 ✓
  · 模拟 fuzzel：foot -e <自动投屏> → niri 里 foot 窗口数 4 →（短暂 5）→ 4，即临时终端确实关掉了 ✓
诊断改进：mpv 增加 --log-file=~/.local/state/mirror-screen/mpv.log（终端静默但日志完整），
         排查"窗口没出来"这类问题就靠它（本次就靠它确认 mpv 初始化正常）。

## 11) 追加：mpv "没窗口/闪退" 的真正原因（用户反馈）
现象：点开始投屏后，配置界面关掉了，但**没有 mpv 窗口**（用户描述"窗口闪退，没有拉起 mpv"）。
实测对照（手机屏幕静止、无 damage 时）：
  --force-window=yes        → niri 窗口数 = 0   ← 就是用户看到的现象
  --force-window=immediate  → niri 窗口数 = 1   ← 修复
根因：wf-recorder 是 **damage 驱动**的；屏幕静止时一帧都不发。裸流（-m h264）里没有容器头，
      mpv 拿不到首帧就**不创建窗口**（--force-window=yes 要等有视频参数/首帧）。
     immediate 模式则在解析文件之前就把窗口建好。
其他加固（同一次改）：
  · 去掉远端命令里的 `2>/dev/null` —— 因为 ssh 的 stderr 本来就重定向进 last-run.log，
    终端仍然静默，但远端 ffmpeg 的报错能留存（这次排查立刻受益）
  · 开播前预检：① `wlr-randr --output X --on` 亮屏 ② `pkill -x wf-recorder` 清残留
    （手机上残留的旧编码器会让新实例启动即挂 → 也是"闪退"的来源之一）
  · mpv 加 `--keep-open=yes`：流意外中断时保留窗口而不是直接消失
  · mpv 加 `--log-file=…/mpv.log`；启动 <5s 就结束会写 `[warn]` 并 `notify-send` 弹通知（dunst 在跑）
  · 日志改为写完命令行立即 flush，便于边跑边 tail

## 12) 追加：压缩 vs 延迟（回答"压缩能否提速"）
方法：手机端持续制造 damage（通知横幅循环），分别测 码率 / 手机端 wf-recorder CPU / 端到端延迟。
  · 码率与 CPU：跑 12 秒，取 /proc/<pid>/stat 的 utime+stime 差值 / 时间（单核百分比）
  · 端到端延迟：mpv IPC 每 ~15ms 抓一帧 JPG（--screenshot-format=jpg -q30），触发用
    `wlr-randr --output DSI-1 --scale 2.5`（立即生效、无动画），量"触发→抓到的帧内容变化"
结果（360x760@20fps，链路 400 Mbit/s / 信号 -52 dBm）：
  ultrafast crf=20  ： 码率 ~280 kbit/s   手机 26% 单核   端到端中位 381 ms（样本 340/360/381/401）
  superfast crf=20  ： 码率 267 kbit/s    手机 35% 单核   端到端中位 361 ms（样本 345/349/361/362）
  medium    crf=24  ： 码率 195 kbit/s    手机 66% 单核   端到端未测（CPU 已翻倍）
  rawvideo（不压缩） ： 码率 94.8 Mbit/s（占链路 24%）  手机 13.6% 单核（每帧 ~7ms）  未测出更优
结论：带宽占用仅 0.07%，压缩档位对延迟无实质影响；压得更狠只增加远端 CPU。
     rawvideo 能播（窗口正常创建），但代价 24% 链路容量而无延迟收益 → 未纳入 TUI。
     TUI 新增「编码档位」：极速（ultrafast，默认）/ 均衡（superfast）/ 省带宽（medium）。
注：端到端数值已包含手机自身处理（改缩放引发的 UI 重排），属上限；横向比较有效。

## 13) 追加：延迟攻坚（用户反馈「超过 1 秒」）
### 13.1 确凿发现并修复：远端 ssh 建连极慢
  无复用：307 / 293 / 285 / 287 ms（每次都是完整 KEX）
  有复用(ControlMaster)：317ms(建 master) 之后 23 / 21 / 33 / 45 ms
  ⇒ 投屏原本要开 3 条连接（预检亮屏、清残留、流水线） = 约 0.9-1.9s 启动开销
  修复：ssh_argv 统一加 ControlMaster/ControlPath/ControlPersist；预检合并为一次 ssh
  实测：启动→投屏窗口出现 = 0.50 s（两次一致；原约 1.9-2.0s）
### 13.2 端到端延迟（触发器：手机改缩放，可重复）
  默认            448ms (338/354/448/488)
  --no-dmabuf     492ms
  -B 60           330ms(第一轮) / 461ms(复测)   ← 两轮差 130ms ⇒ 噪声，未采纳
  -r 30           351ms
  -B 120 / 270p   373 / 367ms
  结论：350-500ms 量级、抖动 ±150ms；上述「改进」多在噪声内，只有连接复用是数量级差异。
### 13.3 编码侧已到最优点
  tune=zerolatency 隐含 sliced-threads；额外 x264-params 实测 CPU 28.4%→28.2%
### 13.4 手机抓屏本身很贵（关键线索）
  grim（同一条 screencopy 路径）：整屏 0.776/0.759/0.759/0.746/0.753 s；缩略图 0.631/0.645/0.652/0.624/0.594 s
  ⇒ Adreno+freedreno 的 GPU→CPU 回读慢；wf-recorder 走 dmabuf 能撑 20fps(14ms/帧 CPU)，
     但 compositor 帧产出/抓屏调度是剩余 300-400ms 的主要来源，本机侧无法再优化。
  注意：重复发通知会被 phosh 限流（后期不再渲染横幅），做测量别依赖它。

## 14) 追加：wf-recorder 替代方案实测
筛选标准：跟不上时是否丢帧（wf-recorder 的 fps 滤镜无丢帧策略 ⇒ 堆积）。
· wf-recorder：必需 -r（去掉后 0 字节输出，实测）；最坏条件下延迟 0ms→57s（堆积）
· wl-screenrec 0.3.2：❌ 实测不可用 —— `--no-hw` 与 `--ffmpeg-encoder libx264` 都仍初始化 VAAPI：
  "[VAAPI] Failed to initialise VAAPI connection ... failed to create encoder(s)"（Adreno 无 VA-API）
· wayvnc 0.10：✅ 手机可跑（监听 5900；systemd-run 常驻；**空闲 3 秒 0 CPU ticks**，证明按需产帧）
  VNC 协议本身按需发送 + damage 合并 ⇒ 有界；本机需 VNC 客户端（tigervnc/gtk-vnc）
  注：手机 nftables input=dorp，仅 22 可入 ⇒ 走 ssh -L 隧道（已验证）
· portal+PipeWire：组件齐备（ScreenCast 接口 ✓、pipewire ✓、gst-plugin-pipewire ✓、
  pw-cat --media-type Video ✓）⇒ 可做"零编码 + 实时图（不排队）"路径，需 portal 客户端+手机端授权
· OBS Studio 32 / gnome-remote-desktop：Alpine 有包，前者天生丢帧语义+可 UDP 输出，但手机上偏重
· 排除：grim 轮询（抓一帧 0.6-0.8s）、wl-mirror（只本机显示）、wcap（未打包）

## 15) 追加：方案2(wayvnc) 与 方案3(portal) 的实测
方案2 wayvnc：
  手机端可跑（systemd-run 常驻 systemd 单元 wayvnc-test；空闲 3s = 0 CPU ticks ⇒ 按需产帧）
  phoc：日志 ext-image-copy-capture "No supported buffer formats were found"（与社区 issue 同源）；
       设 WAYVNC_NO_EXT_IMAGE_COPY=1 后仍能出图
  本机：装 gtk-vnc 1.5.0（唯一新增包，0.9MB），gvncviewer 127.0.0.1 窗口正常；gvnccapture 抓帧 54KB
  网络：手机 nftables input=drop，仅 22 可入 ⇒ 必须 ssh -L 隧道（已验证）
  延迟测量（三次尝试均被采样污染）：
    RFB 客户端(Raw 编码) 中位 379ms（=1080x2280 原始帧传输时间，非真实流延迟）
    gvnccapture 采样 1036/1049/1181/1223/1296/1312/1343ms（含冷连接+全屏更新开销）
    窗口取色 未完成（脚本被手机负载压到 ssh 超时）
  ⇒ 结论：读数均不比 wf-recorder 好（后者同方法下 0~50ms）；wayvnc 的价值在键鼠操作 + 协议有界
方案3 portal+PipeWire：
  CreateSession 正常响应（/org/freedesktop/portal/desktop/request/1_1142/probe1）⇒ portal 可用
  手机已有 python3-dbus 1.4.0；gstreamer / gst-plugin-pipewire 在 Alpine 仓库
  未实施：需要 apk add gstreamer gst-plugin-pipewire + portal 客户端脚本 + 手机端点一次"允许"

## 16) 方案3（portal 零编码）落地实测 —— 2026-09-30
链路：xdg-desktop-portal ScreenCast → PipeWire 节点(116/117/127...) → GStreamer(仅 videoconvert/videoscale) → 裸 I420 → ssh → 本机 mpv
portal 探测（多次）：CreateSession → SelectSources → Start → node=NNN, 原始分辨率 (360,760)（= 手机逻辑分辨率）
                    OpenPipeWireRemote → fd=7 (类型 socket:[...])；节点在全局图中可见：
                    id=NNN media.class=Video/Source name=xdg-desktop-portal-wlr state=running
授权：首次弹框，成功后保存 restore_token → 之后日志显示“使用已保存的 restore_token（应不再弹窗）”，用户确认不再弹窗
性能（同机同网，pgrep -x 精确取进程）：
  静止画面 5s  ：CPU 1% 单核，带宽 80 KB/s
  持续变化 8s  ：CPU 15% 单核，带宽 2354 KB/s = 19 Mbit/s（360x760 I420）
  本机 mpv     ：3.4% CPU
延迟（触发式：远端改缩放 → 镜像画面变化，与 wf-recorder 同方法）：115 / 113 ms
清理：关 mpv 窗口 → 远端 gst-launch-1.0 = 0、本机 mpv = 0
关键坑（已修）：
  1) pipewiresrc 必须用 target-object=<名字>（path= 已废弃且为字符串）；数字 id 匹配不上，
     需 pw-dump 由 id 反查 node.name
  2) 必须用 subprocess.Popen 跑 gst-launch；用 os.execvp 会断开 D-Bus ⇒ portal 销毁会话 ⇒ target not found
  3) portal 的 streams[].size 就是远端逻辑分辨率，放大无收益
  4) 远端残留进程需 pkill -9；统计进程用 pgrep -x（pgrep -f 会自匹配 ssh 命令行）

## 17) 追加：修「卡一下然后快进到最后一帧」（用户反馈）
根因：portal 模式的 mpv 缺低延迟参数 —— mpv 对管道输入默认 cache=auto，会攒一段再一次性播完（观感=卡+快进）。
       wf-recorder 模式因 --profile=low-latency（内含 cache=no）无此问题。
修法：① mpv 加 --cache=no --demuxer-readahead-secs=0 --demuxer-max-bytes=8MiB
      ② 远端管线加 queue leaky=upstream max-size-buffers=2（--drop-old，丢旧帧保最新；裸帧自包含，丢帧安全）
      ③ ssh -o Compression=no
量化（远端 10Hz 颜色时钟为基准，本机 mpv IPC 每 ~20ms 取帧，统计画面变化间隔）：
  修后： 139 帧/14s，平均 101ms（期望 100），最大 245ms，>2 倍期望仅 2 次
  再压 PipeWire 缓冲 min/max-buffers=2：136 帧/14s，平均 103ms，最大 221ms，>2 倍期望 2 次
  ⇒ 元凶就是 mpv 缓存；PipeWire 缓冲池非瓶颈（仍保留 2 帧上限）
另一个好性质：屏幕静止时 portal 路径 CPU 0% / 带宽 0 KB/s（damage 驱动不空转）

## 18) 追加：投屏窗口改成“普通窗口、非全屏、高度占满”（用户要求）
做法：mpv --window-scale=<期望物理高度/视频高度>，期望高度 = niri 工作区逻辑高度(1028) × 输出缩放(1.75) - 8px
实测：窗口 485 x 1023（工作区 1645x1028），宽高比 0.474 = 精确 9/19 ⇒ 手机整屏可见、尽量大、且非全屏
否决的两条路（实测）：
  · --autofit=845x1785 → niri 忽略该尺寸，窗口变 811x457（横着）✗
  · niri window-rule 按 app-id=mpv（483x1018 ✓ 但会误伤日常看视频的 mpv）；改按 title 匹配又因
    规则在窗口创建时求值、mpv 标题后设 ⇒ 匹配不上 ✗
备注：临时加过的 niri 规则已撤销，config.kdl 与备份 bak-20260930-224507 逐字节一致（无残留）

## 19) 追加：显示方式三档，默认「半幅窗口」（用户要求：窗口半幅 + mpv 缩放视频）
TUI 显示方式：半幅窗口(--geometry=50%x100%，默认) / 贴合视频(--window-scale) / 全屏(--fs)
实测（工作区 1645x1028，scale 1.75）：
  半幅窗口 → 窗口 825 x 1031（宽 50% / 高 100%），视频由 mpv 缩放居中、两侧黑边 ✓（截图确认）
  贴合视频 → 485 x 1023（比例 9/19，几乎无黑边）
结论：niri 采纳窗口请求尺寸 ⇒ --geometry=50%x100% 最简单且自适应；--autofit 被 niri 忽略；
      niri window-rule 按 app-id 会误伤日常 mpv、按 title 又因规则求值早于 mpv 设标题而匹配不上。

## 20) 追加：显示方式深度实测（2026-09-30 深夜）——"窗口高度不对"→"留余量"定案
目标：窗口 = 半幅宽 × 满高，手机画面等比缩放居中。
【否定结论（都实测过）】
  · mpv --geometry 在 Wayland 下被 mpv 忽略：50%x100% / 1440x1799 / 1462x1827 / 822x1028 / 1440x1800
    五种写法，niri 给的窗口完全一致（810x1013 = 当时 niri 默认列宽 × 可用高度）
  · --autofit 被 niri 忽略（窗口变 811x457）
  · niri window-rule 不划算：按 app-id="mpv" 会误伤日常看视频的 mpv；按 title 匹配又因规则
    求值早于 mpv 设标题而匹配不上
  · 事后用 IPC 改 window-scale 立刻生效（825x1015 ✓）但会被回弹（810x997）：niri 每次布局重算
    都把窗口拨回 mpv **启动时**请求的尺寸
【可行结论】
  · --window-scale 是唯一可靠的尺寸旋钮（窗口 = 视频尺寸 × 倍数，实测分毫不差）
  · 要"宽窗口 + 画面居中"，先用 --vf=pad 把画面补成**目标窗口宽高比**，再 --window-scale 定尺寸
  · 尺寸还跟**焦点状态**有关（用户定位）：聚焦 825x1015 / 失焦 810x997，差值 = focus ring + border + gaps
  · 投屏**前**量到的普通窗口高度偏小（982，状态栏占 ~15px 保留区）；投屏**期间** = 1028 − 2×gaps = 1012
【定案（用户提议：画面留余量，别跟 niri 抢尺寸）】
  窗口请求 = 乐观可用高度 1028 − 2×8 = 1012
  画面渲染 = 1012 × 96% = 972 < 窗口最小可能高度 997
  ⇒ 窗口无论被压到多少、焦点怎么变，mpv 只把画面等比缩小，**绝不切内容**
  实测：窗口 810x997（失焦）/ 825x1015（聚焦），mpv 画面矩形 1414x1740 物理 = 808x994 逻辑，
        两侧黑边 + 上下留余量，截图确认手机整屏（状态栏→键盘）完整 ✓
  TUI 新增「窗口高度」档：100% / 98% / 95% / 90% / 85%（在 96% 余量之上再乘）

## 21) 追加：让投屏窗口尺寸**恒定**（2026-09-30 收尾；用户："窗口高度还是会变小，能否不变"）
【问题】上一版只保证"不切画面"，但窗口本身随焦点变：聚焦 825x1015 / 失焦 810x997（跳 18px）。
【关键发现】mpv 支持 `--wayland-app-id=`（默认硬编码 "mpv"，改符号链接也不会变；但该选项可改）。
  给它一个专属 app-id（portal-cast）后，niri 用 window-rule 匹配这个 app-id，尺寸就被钉住了。
  实测（焦点来回切 6 次）：tile 始终 810x997 ✓（普通窗口不受影响，它们仍是 app-id=mpv）
【规则取值】
  default-column-width { proportion 0.5 }     → 半幅宽 ✓
  default-window-height { fixed 1012 }        → 1028 − 2×gaps(8) ✓
  ✗ proportion 1.0 不行：niri 把状态栏保留区也算进比例，只得 997（比普通窗口矮 16px）
  niri 不允许平铺窗口超过可用区（状态栏占位 + gaps 都算）：状态栏可见时实际 997；不占位时才到 1012
【实现】mirror-screen.py 两种窗口模式都加 `--wayland-app-id=portal-cast`；
  规则用 ~/AIworks/02-本机运维/niri-cast-rule.sh {check|install|remove} 管理（幂等 + 备份 + niri validate）
【实测结果】窗口 810x997 恒定 ✓；mpv 画面矩形 808x994 逻辑，手机画面约 954px 高，上下各留 ~21px 余量 ✓
【坑】标记注释必须用 KDL 的 `//`；写成 `#` 会让 niri validate 报 "unexpected token" 而整个配置加载失败。

## 22) 追加：窗口尺寸恒定的**最终解** = 浮动窗口（2026-09-30 深夜；用户："失焦再聚焦还是会损失窗口高度"）
【现象】失焦再聚焦后，投屏窗口比旁边窗口矮（997 vs 邻窗 1013）——用户截图确认。
【逐条排除】
  · app-id 规则 + default-window-height { fixed 1012 }：焦点切换仍改尺寸 ✗（niri 每次焦点变化按当时可用高度重算 ⇒ 997/998）
  · 规则改 fixed 1028：只差 1px（998）✗ ⇒ 规则值几乎不起作用，niri 用自己的可用高度钳制
  · 规则里 border{off} / focus-ring{off}：无效 ✗
  · 启动后 IPC 改 window-scale（+看护重下发）：能到 1015，但每次焦点变化又被拨回 997 ⇒ 拉锯 ✗
    （日志实证：[keep] 被压到 810x997 → 重下发 → 实测 1015 ✓ → 又一次 738x907 → 1015 ✓）
【根因】niri 给平铺窗口的高度 = 当时的可用高度 − 上下 gaps（1028 − 状态栏~15 − 16 ≈ 997）；
  而旁边窗口停在很久以前的旧值（1013，niri 不会缩它们）⇒ 视觉上"投屏窗口比邻窗矮"，且追不上。
【最终解】window-rule 里加 `open-floating true`：
  浮动窗口尺寸完全由应用决定，niri 不接管 ⇒ 焦点切换/工作区切换尺寸恒定。
实测：窗口 825x1015（±3px 是 mpv 取整），与满高邻窗 1013 齐平 ✓；画面矩形 823x1013 逻辑，手机整屏完整 ✓（截图）
配套：mpv `--wayland-app-id=portal-cast`（不影响日常 mpv）；`niri-cast-rule.sh check|install|remove` 管理规则；
  mirror-screen.py 内置「尺寸看护」（keep_size，幂等重下发 + 5 次失败退避）；画面 VIDEO_MARGIN 回到 1.0（顶满）。
【坑】脚本标记注释必须用 KDL 的 `//`，写成 `#` 会让 niri validate 报 unexpected token、配置整体加载失败。

## 23) 追加：窗口高度恒定的**最终解** = tiled-state true（平铺保持；更正 §22 的误判）
【更正】§22 把原因归给"niri 每次布局重算把窗口拨回启动请求"——**不对**。
  真正原因是 **mpv 自己按视频尺寸"吸附"窗口**（它以为自己在浮动）：窗口永远比邻窗矮 16px
  （997 vs 1013），聚焦/失焦还会跳。niri 只是把 mpv 给的尺寸照单全收。
  证据链：加 tiled-state true 后，同样的 niri 配置、同样的 mpv，窗口立刻变成 810x1013 且恒定 ✓。
【最终配置】window-rule { match app-id=r#"^portal-cast$"#; tiled-state true;
            default-column-width { proportion 0.5 } }
  与你给 foot 加的那条规则同源（foot 同样会按字符格吸附尺寸 ⇒ 底部白隔 10~20px）。
  高度不用写：niri 把平铺窗口铺满「可用高度 − 上下 gaps」（1028 − 16 = 1012）。
【配套代码】mirror-screen.py：半幅窗口不再给 mpv 传任何尺寸参数（--vf=pad / --window-scale 删掉），
  只传 --wayland-app-id=portal-cast；其他显示方式用 portal-cast-fitted 以免被这条规则套住。
【实测】焦点切 4 次 + 工作区来回切：窗口恒 810x1013 浮动=False（平铺 ✓），与邻窗 1013 齐平 ✓，
  看护 0 次触发；画面区域 837x1751 物理、宽高比 0.478（手机 9:19=0.474，无变形 ✓），两侧黑边各 165 逻辑。
【排除表】--geometry ✗(Wayland 无效) / fixed 高度 ✗(只差 1px) / border+focus-ring off ✗ /
  事后 IPC 改尺寸 ✗(被 mpv 吸附回去) / open-floating ✗(脱离平铺) ⇒ tiled-state true ✓
