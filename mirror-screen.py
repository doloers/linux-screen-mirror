#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mirror-screen — 「把远端 Linux 屏幕投到本机」TUI 配置器 / 启动器

⚠️ 设计更正（2026-09-30，实测得出）：
   waypipe **不能**镜像远端屏幕。waypipe 的模式是"应用在远端、显示在本机"，
   远端应用看到的是**本机 compositor**（实测：在 waypipe 里 wayland-info 报的是本机
   的 wl_output eDP-1 与本机协议），所以 `waypipe ssh 远端 wl-mirror 远端输出名`
   必然报 "output not found"。
   ✅ 正确做法是"远端采集编码 → 管道 → 本机播放"：
        ssh 远端 'wf-recorder -o <输出> -c libx264 -m mpegts -f /dev/stdout' | mpv -
      或者远端跑 wayvnc、本机用 VNC 客户端（可反向操作）。

两种模式：
  1) 远端屏幕 → 本机窗口（wf-recorder 流）   ← 默认，已实测可用
  2) 远端应用 → 本机运行（waypipe）          ← 另一种用途（app 在远端、显示在本机）

用法：
  mirror-screen              打开 TUI
  mirror-screen --run        用保存的配置直接投屏（可绑快捷键）
  mirror-screen --wake       只唤醒远端屏幕（很多"抓不到帧"的根因就是它息屏了）
  mirror-screen --check      体检远端（桌面/工具/抓屏实测/Vulkan）
  mirror-screen --dry-run    只打印将要执行的命令
  mirror-screen --list-hosts 列出候选 SSH 目标

⚠️ 血泪注意：远端屏幕若处于息屏/DPMS off，screencopy 会失败，现象是
   grim 报 "no supported format found"、wf-recorder 报 "Failed to copy frame" —— 
   看起来像协议/权限问题，其实只需要把屏幕点亮（TUI 里有"唤醒远端屏幕"）。

配置：~/.config/mirror-screen/config.json
"""
from __future__ import annotations

import argparse
import curses
import hashlib
import json
import locale
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

APP = "mirror-screen"
CFG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / APP
CFG_FILE = CFG_DIR / "config.json"
CACHE_DIR = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / APP

MODE_PORTAL = "远端屏幕 → 本机（portal 零编码）"   # 推荐：延迟相当、远端 CPU 低一个数量级、无编码队列
MODE_STREAM = "远端屏幕 → 本机窗口（wf-recorder 流）"
MODE_WAYPIPE = "远端应用 → 本机运行（waypipe）"
MODES = [MODE_PORTAL, MODE_STREAM, MODE_WAYPIPE]

CODEC_CHOICES = ["libx264", "libx264rgb", "mpeg4", "h264_v4l2m2m"]
# 编码档位（实测：压得越狠≠越快；越狠 CPU 越高、延迟无改善）
#   极速=ultrafast 手机 CPU 最低（实测 26% 单核 / 每帧 ~13ms）
#   均衡=superfast 当前默认（35% / ~18ms）
#   省带宽=medium  码率最低但 CPU 翻倍（66% / ~33ms）
ENCODE_PROFILES = {
    "极速（手机 CPU 最低）": "preset=ultrafast crf=20",
    "均衡（默认）": "preset=superfast crf=20",
    "省带宽（手机最吃力）": "preset=medium crf=24",
}
# h264 = 裸 Annex-B 流，无容器 ⇒ mpv 无 demuxer 缓冲（实测 Cache 0.00s vs mpegts 0.50s）
MUXER_CHOICES = ["h264", "mpegts", "matroska", "nut"]
COMPRESS_CHOICES = ["lz4", "lz4=4", "zstd", "zstd=9", "none"]
VIDEO_CHOICES = ["none", "h264", "h264,bpf=8000000", "vp9"]
DISPLAY_CHOICES = ["半幅窗口", "贴合视频", "全屏"]
SCALE_CHOICES = ["", "360", "540", "720", "1080"]
FPS_CHOICES = ["10", "15", "20", "24", "30"]
# 必须默认 vaapi：装上 vulkan-intel 后，auto-safe 会挑 h264-vulkan，
# 而本机 Mesa 没有 VK_KHR_video_decode_queue → 逐帧报 "Device does not support ..." 然后 no frame!
HWDEC_CHOICES = ["vaapi", "auto-safe", "no"]

DEFAULTS = {
    "mode": MODE_PORTAL,
    "host": "", "user": "", "port": "22",
    "output": "",                 # 远端输出名，如 DSI-1 / eDP-1
    "codec": "libx264", "muxer": "h264", "fps": "15", "scale": "",
    "encode": "极速（手机 CPU 最低）",   # 见 ENCODE_PROFILES
    "lowlat": True,              # mpv 极低延迟：--untimed --video-sync=desync --demuxer-readahead-secs=0
    "fullscreen": False,          # 旧开关，仅 waypipe 模式残留；流模式请用下面的 display
    "remote_client": "auto",     # 远端 portal 客户端：auto(优先 C)/c/python；C 版启动 0.07s、RSS 1.2MB（Python 0.4s/25MB）
    "window_height": "100%",
    "keep_size": True,             # 后台看护窗口尺寸：被 niri 压小后自动拉回（关掉就不会自动恢复）       # 半幅窗口模式的窗口高度：相对“当下普通窗口能占的高度”(usable_height())，100% = 与其他窗口齐平
    "hwdec": "vaapi",             # vaapi / auto-safe / no；见 HWDEC_CHOICES 注释
    "quiet": True,                # 运行时静默：远端 ffmpeg 报告 2>/dev/null，mpv --really-quiet --no-terminal
    "exit_on_close": True,         # mpv 窗口一关就退出程序（并清理远端编码器）
    "close_tui": True,            # 开投屏时就把 TUI/终端窗口关掉（投屏进程脱离终端后台跑）
    "mpv_extra": "",
    "wake": True,                 # 投屏前先点亮远端屏幕
    # waypipe 模式的选项
    "compress": "lz4", "video": "none", "nogpu": False, "display": "半幅窗口",
    "wp_command": "foot",
}

RUNTIME_EXPORTS = (
    'export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"; '
    'if [ -z "${WAYLAND_DISPLAY:-}" ]; then '
    'WAYLAND_DISPLAY=$(basename "$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v lock | head -1)" 2>/dev/null); '
    "export WAYLAND_DISPLAY; fi; "
    'export NIRI_SOCKET="$(ls $XDG_RUNTIME_DIR/niri.*.sock 2>/dev/null | head -1)"; '
    'export HYPRLAND_INSTANCE_SIGNATURE="$(ls $XDG_RUNTIME_DIR/hypr/ 2>/dev/null | head -1)"; '
)

PROBE_SCRIPT = RUNTIME_EXPORTS + (
    "niri msg outputs 2>/dev/null | sed -n 's/^Output \"[^\"]*\" (\\([^)]*\\)).*/\\1/p' | head -1; "
    "swaymsg -t get_outputs 2>/dev/null | sed -n 's/.*\"name\": \"\\([^\"]*\\)\".*/\\1/p' | head -1; "
    "hyprctl monitors 2>/dev/null | sed -n 's/^Monitor \\([^ ]*\\).*/\\1/p' | head -1; "
    "wlr-randr 2>/dev/null | awk 'NF && $1 ~ /^[A-Za-z]/ {print $1; exit}'"
)

CHECK_SCRIPT = RUNTIME_EXPORTS + "\n".join([
    "printf 'compositor=%s\\n' \"$( (niri msg outputs >/dev/null 2>&1 && echo niri) || (swaymsg -t get_outputs >/dev/null 2>&1 && echo sway) || (hyprctl monitors >/dev/null 2>&1 && echo hyprland) || (pgrep -x phoc >/dev/null 2>&1 && echo phoc/Phosh) || (pgrep -x labwc >/dev/null 2>&1 && echo labwc) || echo unknown)\"",
    "printf 'display=%s\\n' \"$WAYLAND_DISPLAY\"",
    "printf 'wfrecorder=%s\\n' \"$(command -v wf-recorder >/dev/null 2>&1 && echo yes || echo no)\"",
    "printf 'wlrmirror=%s\\n' \"$(command -v wl-mirror >/dev/null 2>&1 && echo yes || echo no)\"",
    "printf 'waypipe=%s\\n' \"$(command -v waypipe >/dev/null 2>&1 && echo yes || echo no)\"",
    "printf 'wlrrandr=%s\\n' \"$(command -v wlr-randr >/dev/null 2>&1 && echo yes || echo no)\"",
    "printf 'grim=%s\\n' \"$(command -v grim >/dev/null 2>&1 && echo yes || echo no)\"",
    "printf 'vulkan=%s\\n' \"$(ls /usr/share/vulkan/icd.d/*.json 2>/dev/null | wc -l)\"",
    "printf 'powersave=%s\\n' \"$(iw dev wlan0 get power_save 2>/dev/null | awk '{print $NF}')\"",
    "OUT=$(wlr-randr 2>/dev/null | awk 'NF && $1 ~ /^[A-Za-z]/ {print $1; exit}'); "
    "printf 'drmdpms=%s\\n' \"$(cat /sys/class/drm/card*-$OUT/dpms 2>/dev/null | head -1)\"; "
    "rm -f /tmp/.mirror-screen-test.png; "
    "printf 'capture=%s\\n' \"$(grim -o $OUT /tmp/.mirror-screen-test.png >/dev/null 2>&1 && stat -c%s /tmp/.mirror-screen-test.png 2>/dev/null || echo FAIL)\"; "
    "rm -f /tmp/.mirror-screen-test.png",
])


# ---------------------------------------------------------------- helpers

def load_cfg() -> dict:
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(CFG_FILE.read_text(encoding="utf-8")))
    except Exception:
        pass
    return cfg


def save_cfg(cfg: dict) -> None:
    CFG_DIR.mkdir(parents=True, exist_ok=True)
    CFG_FILE.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def cycle(values: list, current, step: int = 1):
    try:
        i = values.index(str(current))
    except ValueError:
        return values[0]
    return values[(i + step) % len(values)]


def ssh_target(cfg: dict) -> str:
    host = (cfg.get("host") or "").strip()
    user = (cfg.get("user") or "").strip()
    return f"{user}@{host}" if user else host


def ssh_target_display(cfg: dict) -> str:
    return ssh_target(cfg) if (cfg.get("host") or "").strip() else "<目标主机>"


def ssh_argv(cfg: dict) -> list:
    # 连接复用：这台手机新建一条 ssh（KEX+密钥）要 400~800ms！
    # 复用后，预检/清理/探测这些附属命令几乎免费（~10ms），投屏启动快很多。
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    ctrl = CACHE_DIR / "cm-%r@%h-%p"
    a = ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15", "-o", "Compression=no",
         "-o", "ControlMaster=auto", "-o", f"ControlPath={ctrl}", "-o", "ControlPersist=300"]
    port = str(cfg.get("port", "22")).strip()
    if port and port != "22":
        a += ["-p", port]
    return a + [ssh_target(cfg)]


def ssh_wrapper(cfg: dict) -> Path:
    port = str(cfg.get("port", "22")).strip()
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    p = CACHE_DIR / f"ssh-port{port}.sh"
    p.write_text(f'#!/bin/sh\nexec /usr/bin/ssh -p {port} "$@"\n', encoding="utf-8")
    p.chmod(0o755)
    return p


def state_log_path() -> Path:
    d = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state")) / APP
    d.mkdir(parents=True, exist_ok=True)
    return d / "last-run.log"


# 降编码延迟的 x264 调优：分片并行（多核手机把一帧切开并行编）+ 关掉所有前瞻
X264_LATENCY_PARAMS = "sliced-threads=1:rc-lookahead=0:sync-lookahead=0:bframes=0"


def build_remote_command(cfg: dict) -> str:
    """远端命令（字符串形式，交给 ssh 执行）"""
    out = (cfg.get("output") or "").strip()
    cmd = f"wf-recorder -y -o {out} -c {cfg['codec']} -x yuv420p -r {cfg['fps']}"
    prof = ENCODE_PROFILES.get(cfg.get("encode") or "", "preset=superfast crf=20")
    for kv in prof.split():
        cmd += f" -p {kv}"
    cmd += " -p tune=zerolatency"
    if "x264" in (cfg.get("codec") or ""):
        cmd += f" -p x264-params={X264_LATENCY_PARAMS}"   # 多核并行分片 ⇒ 每帧编码延迟更低
    if (cfg.get("scale") or "").strip():
        cmd += f' -F "scale={cfg["scale"]}:-1"'
    cmd += f" -m {cfg['muxer']} -f /dev/stdout"
    # 不再加 2>/dev/null：远端 stderr 会被 _run_pipeline_inner 重定向进日志文件，
    # 终端本来就看不到，但报错能留存，便于排查
    return cmd


def build_remote_argv(cfg: dict) -> list:
    return ssh_argv(cfg) + [build_remote_command(cfg)]


def build_player_argv(cfg: dict) -> list:
    mpv = ["mpv", "--profile=low-latency", "--no-config", f"--hwdec={cfg.get('hwdec') or 'vaapi'}",
           f"--demuxer-lavf-format={cfg['muxer']}", "--force-window=immediate",
           f"--wayland-app-id={cast_app_id(cfg)}",   # 半幅窗口交给 niri 规则给尺寸
           f"--title=MIRROR:{ssh_target_display(cfg)}"]
    # 窗口尺寸统一交给 display_args（全屏=--fs / 半幅窗口=pad+window-scale / 贴合视频），
    # 不再看旧开关 fullscreen，避免同时加 --fs 和 --geometry 打架
    if cfg.get("lowlat"):
        # 注意：max-bytes 不能太小！太小会让 demuxer 拼不出大帧（720p 单帧可 >64KB）→ 完全没画面。
        mpv += ["--demuxer-readahead-secs=0", "--demuxer-max-bytes=8MiB",
                "--untimed", "--video-sync=desync"]
    sc = int((cfg.get("scale") or "360") or 360)
    mpv += display_args(cfg, sc, round(sc * 760 / 360))
    mpv += ["--keep-open=yes"]      # 流意外中断时保留窗口（而不是闪一下就消失）
    if cfg.get("quiet", True):
        mpv += ["--really-quiet", "--no-terminal"]     # 只管终端安静
    mpv += [x for x in (cfg.get("mpv_extra") or "").split()] + ["-"]
    return mpv


def niri_gaps() -> int:
    """niri 配置里的 gaps（逻辑像素，四周各一份）。读不到就用 8。"""
    try:
        txt = (Path.home() / ".config/niri/config.kdl").read_text()
        m = re.search(r"^\s*gaps\s+(\d+)", txt, re.M)
        if m:
            return int(m.group(1))
    except Exception:
        pass
    return 8


def _tallest_normal_window() -> float:
    """当前 niri 里最高的非浮动、非投屏窗口高度（逻辑像素）；量不到返回 0。"""
    best = 0.0
    try:
        r = subprocess.run(["niri", "msg", "--json", "windows"], capture_output=True, text=True, timeout=5)
        for w in json.loads(r.stdout or "[]"):
            if w.get("is_floating") or (w.get("app_id") or "") in ("mpv", "portal-cast"):
                continue                       # 排除浮动窗口和投屏窗口（避免自反馈）
            ts = (w.get("layout") or {}).get("tile_size") or [0, 0]
            best = max(best, float(ts[1]))
    except Exception:
        pass
    return best


def usable_height_target() -> int:
    """看护用的目标高度：**与旁边的普通窗口齐平**（取当前最高的非投屏、非浮动窗口）。

    为什么不直接要"工作区 − 2×gaps"：niri 会把平铺窗口钳制在**当时的可用区**内
    （状态栏占位 + gaps 都算），乐观值只会让看护反复重下发、白折腾。
    实测：窗口尺寸本身已由 niri 规则 + tiled-state 钉死，这个看护只是兜底。"""
    h_ws, _ = workspace_geometry()
    best = _tallest_normal_window()
    return round(best) if best >= h_ws * 0.5 else round(h_ws - 2 * niri_gaps())


def usable_height() -> int:
    """**乐观**可用高度（窗口目标）= 工作区高度 − 上下 gaps。

    实测（2026-09-30，工作区 1028、gaps 8）：
      · 投屏**前**量「最高窗口」只有 982（状态栏还占着约 15px 保留区）；
      · 投屏**期间**状态栏不再占位，窗口能到 1028 − 2×8 = 1012（三个终端实测 1013）。
    另：niri 在每次布局重算时会把窗口**拨回 mpv 启动时请求的尺寸**，所以用 IPC 事后改尺寸
    会被回弹（实测改成 825x1015 → 过一会变回 810x997）；尺寸只能一开始就给对。
    因此策略是：窗口按乐观值请求（能满高就满高），画面按保守值渲染（留余量，见 display_args）。
    """
    h_ws, _ = workspace_geometry()
    return round(max(h_ws - 2 * niri_gaps(), _tallest_normal_window()))


def _height_pct(cfg: dict) -> float:
    pct = str(cfg.get("window_height") or "100%").rstrip("%")
    try:
        return float(pct) / 100.0
    except ValueError:
        return 1.0


def target_window_logical(cfg: dict) -> tuple:
    """窗口的目标尺寸（逻辑像素）：宽 = 工作区宽 × 0.5，高 = 乐观可用高度 × 百分比。"""
    h_ws, scale = workspace_geometry()
    w_out = 1645
    try:
        r = subprocess.run(["niri", "msg", "--json", "outputs"], capture_output=True, text=True, timeout=5)
        d = json.loads(r.stdout or "{}")
        ws = [int(v.get("logical", {}).get("width", 0)) for v in d.values()]
        ws = [w for w in ws if w > 0]
        if ws:
            w_out = min(ws)
    except Exception:
        pass
    return round(w_out * 0.5), round(usable_height_target() * _height_pct(cfg)), scale


def keep_cast_window_size(cfg: dict, player_argv: list, key: str, stop, logpath: Path) -> None:
    """后台“尺寸看护”：投屏窗口被压小后自动拉回（用户要求：失焦/状态栏变化后不许损失高度）。

    实测（2026-09-30）：
      · niri 把平铺窗口压在**当时的可用区**内，可用区变大后**不会自己长回来**
        （旁边窗口已 1013，投屏窗口仍停在 997 ⇒ 比邻窗矮 16px）；
      · 配上 app-id=portal-cast 的 niri 规则后，mpv 通过 IPC 改 window-scale 是生效的
        （实测设 903x1111 后 30s 不被打回）。
    ⇒ 每 2s 对账一次；发现低于目标就**重新下发启动时那个目标尺寸（幂等，不做比例叠加）**，
      连续 5 次都留不住就放弃（说明可用区确实不够），不和 niri 打架。
    """
    arg = next((a for a in player_argv if a.startswith("--window-scale=")), None)
    ipc = next((a.split("=", 1)[1] for a in player_argv if a.startswith("--input-ipc-server=")), None)
    tgt = window_target(cfg, *frame_size_of(cfg))
    if not arg or not ipc or not tgt:
        return
    ws_target = float(arg.split("=", 1)[1])      # 启动时算好的目标尺寸（幂等使用）
    tgt_h = tgt[1]
    fails, wait, cooldown = 0, 0.0, 4.0
    while not stop.wait(2.0):
        cw, ch = cast_window_size(key)
        if not ch:
            continue
        if abs(ch - tgt_h) <= 8:                 # 已在目标尺寸（±3px 是 mpv 取整抖动，别去动它）
            fails, wait = 0, 0.0
            continue
        if fails >= 5:                           # 试够了：可用区不够大，认了
            continue
        wait += 2.0
        if wait < cooldown:
            continue
        wait = 0.0
        mpv_ipc(ipc, ["set_property", "window-scale", ws_target])
        time.sleep(2.5)                          # 等 niri 的钳制/回弹表现完
        _, after = cast_window_size(key)
        if abs(after - tgt_h) <= 8:
            fails, cooldown = 0, 4.0
        else:
            fails += 1
            cooldown = min(cooldown * 2, 60.0)
        try:
            with open(logpath, "ab") as lg:       # 自己开句柄，避免和远端 stderr 抢文件指针
                lg.write(f"[keep] 实测 {cw}x{ch}（目标高 {tgt_h}）→ 重下发 window-scale={ws_target}"
                         f" → 实测高 {after}{' ✓' if abs(after - tgt_h) <= 8 else f'（第{fails}次没留住）'}\n".encode())
        except Exception:
            pass


def cast_app_id(cfg: dict) -> str:
    """投屏窗口的 Wayland app-id。半幅窗口用 portal-cast（niri 规则按它给尺寸）；
    其他显示方式用 portal-cast-fitted，避免被那条「半幅」规则套住。"""
    mode = cfg.get("display") or "半幅窗口"
    return "portal-cast" if mode == "半幅窗口" else "portal-cast-fitted"


def frame_size_of(cfg: dict) -> tuple:
    """投屏画面的像素尺寸（与 build_player_argv / build_pipeline 里的推导一致）"""
    if cfg.get("mode") == MODE_PORTAL:
        return portal_size(cfg)
    sc = int((cfg.get("scale") or "360") or 360)
    return sc, round(sc * 760 / 360)


def window_target(cfg: dict, frame_w: int, frame_h: int):
    """投屏窗口的目标尺寸（逻辑像素）；全屏返回 None。"""
    mode = cfg.get("display") or "半幅窗口"
    if mode == "全屏":
        return None
    w_log, h_log, _ = target_window_logical(cfg)
    if mode == "贴合视频":                     # 窗口紧贴画面（9:19 竖条）
        return round(h_log * frame_w / max(1, frame_h)), h_log
    return w_log, h_log


def cast_window_size(key: str) -> tuple:
    """从 niri 读投屏窗口的实际尺寸（逻辑像素）：(0,0) = 没找到"""
    try:
        r = subprocess.run(["niri", "msg", "--json", "windows"], capture_output=True, text=True, timeout=4)
        for w in json.loads(r.stdout or "[]"):
            # 投屏窗口的 app-id：带 --wayland-app-id=portal-cast 的是新写法，旧写法是 "mpv"
            if (w.get("app_id") or "") in ("mpv", "portal-cast") and key in str(w.get("title") or ""):
                ts = (w.get("layout") or {}).get("tile_size") or [0, 0]
                return round(float(ts[0])), round(float(ts[1]))
    except Exception:
        pass
    return 0, 0


def mpv_ipc(path, command: list, timeout: float = 2.0) -> dict:
    """向 mpv 的 IPC 发一条 JSON 命令（失败静默返回 {}）"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(str(path))
        s.sendall((json.dumps({"command": command}) + "\n").encode())
        time.sleep(0.12)
        return json.loads((s.recv(65536).decode().splitlines() or ["{}"])[0] or "{}")
    except Exception:
        return {}
    finally:
        try:
            s.close()
        except Exception:
            pass




def workspace_geometry() -> tuple:
    """当前 niri 工作区的（逻辑高度, 输出缩放）；取不到就退回 (1028, 1.0)。
    注意 mpv 的 --autofit 用的是**物理像素**，所以要乘缩放因子。"""
    try:
        r = subprocess.run(["niri", "msg", "--json", "outputs"], capture_output=True, text=True, timeout=5)
        d = json.loads(r.stdout or "{}")
        hs, scales = [], []
        for v in d.values():
            lg = v.get("logical", {})
            if int(lg.get("height", 0)) > 0:
                hs.append(int(lg["height"]))
                scales.append(float(lg.get("scale", 1.0)) or 1.0)
        if hs:
            return min(hs), min(scales)
    except Exception:
        pass
    return 1028, 1.0


def display_args(cfg: dict, frame_w: int, frame_h: int) -> list:
    """按「显示方式」生成 mpv 的窗口尺寸参数。

    半幅窗口**不传任何尺寸**：尺寸由 niri 的 window-rule 决定（app-id=portal-cast，
    见 ~/.config/niri/config.kdl，用 niri-cast-rule.sh 管理）。规则里 `tiled-state true`
    是关键——否则 mpv 会按视频尺寸自己“吸附”窗口（比邻窗矮 997 vs 1013，且焦点一变就跳），
    这与用户给 foot 加的那条规则同理。mpv 只负责把竖屏视频在窗口里等比居中（两侧黑边）。
    """
    mode = cfg.get("display") or "半幅窗口"
    if mode == "全屏":
        return ["--fs"]
    if mode == "贴合视频":
        return [autofit_arg(cfg, frame_w, frame_h)]
    return []


def autofit_arg(cfg: dict, frame_w: int, frame_h: int) -> str:
    """返回 mpv 的窗口尺寸参数（普通窗口、非全屏，但高度占满工作区）。

    mpv 的 --autofit 在 niri 下会被忽略（实测窗口变成 811x457），而**自然的视频尺寸会被采纳**
    （视频 360x760 → 窗口 206x434 逻辑）。所以改成 --window-scale：窗口 = 视频尺寸 × 倍数，
    倍数 = 期望物理高度 / 视频高度。"""
    h_log, scale = workspace_geometry()
    h_log = max(300, h_log - 8)                       # 留一点余量
    want_phys = h_log * scale                         # 期望的窗口物理高度
    ws = max(1.0, round(want_phys / max(1, frame_h), 2))
    return f"--window-scale={ws}"


def portal_size(cfg: dict) -> tuple:
    """portal 模式输出尺寸：宽度取 scale，高度按本机记录的 9:19 比例推（与远端逻辑分辨率一致）"""
    w = int((cfg.get("scale") or "360") or 360)
    if w <= 0:
        w = 360
    return w, round(w * 760 / 360)


def build_pipeline(cfg: dict) -> tuple:
    """返回 ('pipeline'|'portal', 远端argv, 播放argv) 或 ('argv', 命令argv)"""
    if cfg.get("mode") == MODE_PORTAL:
        W, H = portal_size(cfg)
        remote = f"W={W} H={H} " + " ".join(portal_client_argv(cfg))
    if cfg.get("mode") == MODE_PORTAL:
        if cfg.get("lowlat", True):
            remote += " --drop-old"   # 让远端管线用 leaky 队列：积压时丢旧帧
        player = ["mpv", "--no-config", "--hwdec=no",
                  "--demuxer-lavf-format=rawvideo",
                  f"--demuxer-lavf-o=video_size={W}x{H},pixel_format=yuv420p",
                  # 关键：管道流默认会开缓存 ⇒ 攒一段再一次性放出（观感=卡一下然后快进）
                  "--cache=no", "--demuxer-readahead-secs=0", "--demuxer-max-bytes=8MiB",
                  "--untimed", "--video-sync=desync", "--force-window=immediate",
                  # 专属 app-id：配合 ~/.config/niri/config.kdl 里 app-id=portal-cast 的 window-rule，
                  # 让 niri 固定「半幅宽 × 满高」且在焦点切换时不再重算尺寸（不影响日常 mpv）
                  f"--wayland-app-id={cast_app_id(cfg)}",
                  *display_args(cfg, W, H),
                  f"--input-ipc-server={CACHE_DIR / 'portal-cast.sock'}",
                  f"--title=PORTAL-CAST:{ssh_target_display(cfg)}"]
        if cfg.get("quiet", True):
            player += ["--really-quiet", "--no-terminal"]
        player += [x for x in (cfg.get("mpv_extra") or "").split()] + ["-"]
        return ("portal", ssh_argv(cfg) + [remote], player)
    if cfg.get("mode") == MODE_STREAM:
        return ("pipeline", build_remote_argv(cfg), build_player_argv(cfg))
    # waypipe 模式：把远端应用拿到本机跑（注意：不是镜像远端屏幕）
    wp = ["waypipe"]
    if cfg.get("nogpu"):
        wp.append("--no-gpu")
    if cfg.get("compress") and cfg["compress"] != "none":
        wp += ["-c", cfg["compress"]]
    if cfg.get("video") and cfg["video"] != "none":
        wp += ["--video", cfg["video"]]
    port = str(cfg.get("port", "22")).strip()
    if port and port != "22":
        wp += ["--ssh-bin", str(ssh_wrapper(cfg))]
    inner = (cfg.get("wp_command") or "foot").split()
    if (cfg.get("display") or "") == "全屏" and inner and inner[0] == "wayvnc":
        pass
    return ("argv", wp + ["ssh", ssh_target(cfg)] + inner)


def shlex_quote(s: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_@%+=:,./-]+", s or ""):
        return s
    return "'" + (s or "").replace("'", "'\"'\"'") + "'"


def preview(cfg: dict) -> str:
    kind, *rest = build_pipeline(cfg)
    if kind in ("pipeline", "portal"):
        remote, player = rest
        return " ".join(shlex_quote(x) for x in remote) + " | " + " ".join(shlex_quote(x) for x in player)
    parts = list(rest[0])
    try:
        i = parts.index("ssh")
        parts[i + 1] = ssh_target_display(cfg)
    except (ValueError, IndexError):
        pass
    return " ".join(parts)


def is_normal_exit(rc) -> bool:
    """正常结束：自己关窗口(0)，或被 signal 终止(-15/143/-2/130)"""
    return rc in (0, -2, -15, 130, 143)


def notify(title: str, body: str = "") -> None:
    """桌面通知（dunst 在跑）：后台投屏出问题时能把话说清楚，而不是默默退掉。"""
    exe = shutil.which("notify-send")
    if not exe:
        return
    try:
        subprocess.run([exe, "-a", "mirror-screen", "-u", "normal", title, body[:400]],
                       timeout=5, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def terminate_proc(p) -> None:
    try:
        p.terminate()
    except Exception:
        return
    try:
        p.wait(timeout=3)
    except Exception:
        try:
            p.kill()
        except Exception:
            pass


def cast_pidfile() -> Path:
    return state_log_path().parent / "cast.pid"


def stop_previous_cast() -> None:
    """若上一次投屏（我们自己启的）还活着，先优雅停掉，避免两个 mpv 窗口/两个远端编码器。"""
    pf = cast_pidfile()
    try:
        pid = int(pf.read_text().strip())
    except Exception:
        return
    if pid > 0:
        try:
            os.kill(pid, 0)
        except OSError:
            pid = 0                      # 已经不在了
        if pid:
            try:
                os.kill(pid, signal.SIGTERM)
            except Exception:
                pass
            for _ in range(10):
                time.sleep(0.2)
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
    try:
        pf.unlink()
    except Exception:
        pass


REMOTE_CLIENT_CACHE = CACHE_DIR / "remote-client"     # 内容: "c" / "python"，由同步阶段写


def script_path(name: str) -> Path:
    """远端脚本/源码的本机位置（安装后在 ~/.local/bin，仓库里在 02-本机运维）"""
    for cand in (Path(__file__).resolve().parent / name,
                 Path.home() / "AIworks/02-本机运维" / name):
        if cand.exists():
            return cand
    return Path("/nonexistent") / name


def c_engine_path() -> Path:
    """C 版运行时引擎（mirror-screen-c）。找不到返回一个不存在的路径，调用方跳过。"""
    for cand in (Path(__file__).resolve().parent / "mirror-screen-c",
                 Path.home() / ".local/bin/mirror-screen-c"):
        if cand.exists() and os.access(cand, os.X_OK):
            return cand
    return Path("/nonexistent/mirror-screen-c")


def portal_client_argv(cfg: dict) -> list:
    """远端 portal 客户端的命令前缀（W=/H= 由调用方加）。
    优先用 C 版（启动 0.07s / RSS 1.2MB），编译不出来就回落 Python 版（0.4s / 25MB）。"""
    want = (cfg.get("remote_client") or "auto").strip()
    if want == "python":
        return ["python3", "/tmp/portal_cast.py"]
    if want == "c":
        return ["/tmp/portal_cast"]
    try:                                     # auto：看同步阶段写下的缓存
        if REMOTE_CLIENT_CACHE.read_text().strip() == "c":
            return ["/tmp/portal_cast"]
    except OSError:
        pass
    return ["python3", "/tmp/portal_cast.py"]


def _remote_sha(cfg: dict, path: str) -> str:
    r = subprocess.run(ssh_argv(cfg) + [f"sha256sum {path} 2>/dev/null | cut -c1-16"],
                       capture_output=True, text=True, timeout=20)
    return r.stdout.strip()


def _push_file(cfg: dict, src: Path, dst: str, log=None) -> bool:
    try:
        local = hashlib.sha256(src.read_bytes()).hexdigest()[:16]
        if _remote_sha(cfg, f"/tmp/{dst}") == local:
            return True
        r = subprocess.run(["scp", "-q"] + ssh_argv(cfg)[1:-1] + [str(src), f"{ssh_target(cfg)}:/tmp/{dst}"],
                           capture_output=True, timeout=90)
        return r.returncode == 0
    except Exception as e:
        if log:
            log.write(f"[portal] 传 {dst} 失败：{e}\n".encode())
        return False


def sync_portal_script(cfg: dict, log=None) -> None:
    """同步远端 portal 客户端，并在需要时在**手机上**编译 C 版（失败自动回退 Python）。

    C 版收益（实测 OnePlus 6）：启动 0.37s → 0.07s，峰值 RSS 25MB → 1.2MB；
    而且它在每次开播的关键路径上，所以直接体现在开播速度上。
    """
    want = (cfg.get("remote_client") or "auto").strip()
    if not _push_file(cfg, script_path("portal_cast.py"), "portal_cast.py", log):
        return
    if want == "python":
        REMOTE_CLIENT_CACHE.write_text("python")
        return
    if not _push_file(cfg, script_path("portal_cast.c"), "portal_cast.c", log):
        REMOTE_CLIENT_CACHE.write_text("python")
        return
    # 只在 .c 更新过或二进制不存在时编译（手机上 gcc + dbus 头文件）
    build = ("cd /tmp && if [ ! -x portal_cast ] || [ portal_cast.c -nt portal_cast ]; then "
             "gcc -O2 -s -o portal_cast portal_cast.c $(pkg-config --cflags --libs dbus-1 2>/dev/null) -ldl 2>&1; fi; "
             "[ -x portal_cast ] && echo BUILD-OK || echo BUILD-FAIL")
    try:
        r = subprocess.run(ssh_argv(cfg) + [build], capture_output=True, text=True, timeout=120)
        ok = "BUILD-OK" in r.stdout
    except Exception as e:
        ok = False
        if log:
            log.write(f"[portal] 远端编译 C 客户端失败：{e}\n".encode())
    REMOTE_CLIENT_CACHE.parent.mkdir(parents=True, exist_ok=True)
    REMOTE_CLIENT_CACHE.write_text("c" if ok else "python")
    if log:
        if ok:
            log.write("[portal] 远端 C 客户端就绪（启动 ~0.07s / RSS ~1.2MB）\n".encode())
        else:
            detail = (r.stdout or r.stderr or "").strip().splitlines()[-3:] if 'r' in dir() else []
            log.write(f"[portal] 远端编译不成功，回退 Python 客户端；gcc 输出：{' | '.join(detail)}\n".encode())


def run_pipeline(cfg: dict) -> int:
    """跑投屏：mpv 窗口一关就返回。运行时报文全部进日志文件，不刷屏。"""
    logpath = state_log_path()
    pf = cast_pidfile()
    try:
        pf.write_text(str(os.getpid()))
    except Exception:
        pass
    try:
        return _run_pipeline_inner(cfg, logpath)
    finally:
        try:
            if pf.read_text().strip() == str(os.getpid()):
                pf.unlink()
        except Exception:
            pass


def _run_pipeline_inner(cfg: dict, logpath: Path) -> int:
    kind, *rest = build_pipeline(cfg)
    with open(logpath, "ab") as log:
        log.write((time.strftime("\n===== %Y-%m-%d %H:%M:%S ") + preview(cfg) + "\n").encode())
        log.flush()                        # 立即落盘，方便实时 tail
        if kind in ("pipeline", "portal"):
            remote_argv, player_argv = rest
            if kind == "portal":
                sync_portal_script(cfg, log)
            # 预检合并成**一次** ssh（每条新连接在这台手机上要 400~800ms，别开多条）
            out_name = (cfg.get("output") or "").strip()
            pre = []
            if cfg.get("wake") and out_name:
                pre.append(f"wlr-randr --output {out_name} --on")
            pre.append("pkill -9 -x wf-recorder")
            try:
                cr = subprocess.run(ssh_argv(cfg) + [" ; ".join(pre) + " ; true"],
                                    stdout=log, stderr=log, timeout=15)
                log.write(f"[preflight] 亮屏+清残留（单次连接）rc={cr.returncode}\n".encode())
            except Exception as e:
                log.write(f"[preflight] 失败：{e}\n".encode())
            try:
                r = subprocess.Popen(remote_argv, stdout=subprocess.PIPE, stderr=log)
            except FileNotFoundError as e:
                log.write(f"启动失败：{e}\n".encode())
                return 127
            try:
                p = subprocess.Popen(player_argv, stdin=r.stdout, stdout=log, stderr=log)
            except FileNotFoundError as e:
                log.write(f"启动失败：{e}\n".encode())
                terminate_proc(r)
                return 127
            # 窗口尺寸：记下目标，并起一个后台“看护”——niri 会把窗口压在当时的可用区内、
            # 且可用区变大后不会自己长回来（旁边窗口 1013 时投屏窗口还停在 997）。
            stop = threading.Event()
            if kind in ("portal", "pipeline"):
                try:
                    tgt = window_target(cfg, *frame_size_of(cfg))
                    if tgt:
                        log.write(f"[window] 目标窗口 {tgt[0]}x{tgt[1]}（逻辑像素；尺寸实际由 niri 规则决定）\n".encode())
                        log.flush()
                    if cfg.get("keep_size", True):
                        threading.Thread(
                            target=keep_cast_window_size,
                            args=(cfg, player_argv, "PORTAL-CAST" if kind == "portal" else "MIRROR",
                                  stop, logpath),
                            daemon=True).start()
                except Exception as e:
                    log.write(f"[window] 看护未启动：{e}\n".encode())
            t0 = time.time()
            try:
                p.wait()                       # ← 关掉 mpv 窗口时在这里返回
            except KeyboardInterrupt:
                terminate_proc(p)
            stop.set()
            ran = time.time() - t0
            if ran < 5:
                try:
                    tail = "；".join(logpath.read_text(errors="replace").splitlines()[-4:])
                except Exception:
                    tail = ""
                log.write(f"[warn] 投屏只维持了 {ran:.1f}s 就结束（rc={p.returncode}）\n".encode())
                notify("投屏结束得太快，可能远端出错了", tail)
            if r.stdout:
                try:
                    r.stdout.close()
                except Exception:
                    pass
            terminate_proc(r)
            # 远端编码器兑底清理（ssh 断开后一般会自行退出，这里确保一下）
            try:
                cr = subprocess.run(ssh_argv(cfg) + ["pkill -9 -x wf-recorder ; pgrep -x wf-recorder >/dev/null && echo STILL-ALIVE || true"],
                                    stdout=log, stderr=log, timeout=10)
                log.write(f"[cleanup] 远端 pkill rc={cr.returncode}\n".encode())
            except Exception as e:
                log.write(f"[cleanup] 失败：{e}\n".encode())
            return p.returncode if p.returncode is not None else 0
        argv = rest[0]
        try:
            p = subprocess.Popen(argv, stdout=log, stderr=log)
        except FileNotFoundError as e:
            log.write(f"启动失败：{e}\n".encode())
            return 127
        try:
            p.wait()
        except KeyboardInterrupt:
            terminate_proc(p)
        return p.returncode or 0


def host_candidates(cfg: dict) -> list:
    out = []
    if cfg.get("host"):
        out.append(f"{cfg['host']}|最近使用")
    sc = Path.home() / ".ssh" / "config"
    if sc.exists():
        for line in sc.read_text(errors="replace").splitlines():
            m = re.match(r"\s*Host\s+(.+)", line, re.I)
            if m:
                for h in m.group(1).split():
                    if "*" not in h and "?" not in h and h not in [x.split("|")[0] for x in out]:
                        out.append(f"{h}|ssh config")
    kh = Path.home() / ".ssh" / "known_hosts"
    if kh.exists():
        for line in kh.read_text(errors="replace").splitlines():
            if not line.strip() or line.startswith("|"):
                continue
            for h in line.split()[0].split(","):
                h = h.strip()
                if h.startswith("[") and "]" in h:
                    h = h[1:h.index("]")]
                if h and h not in [x.split("|")[0] for x in out]:
                    out.append(f"{h}|known_hosts")
    return out


def run_capture(argv: list, timeout: int = 20):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).strip()
    except FileNotFoundError:
        return 127, f"找不到命令：{argv[0]}"
    except subprocess.TimeoutExpired:
        return 124, "超时"


# ---------------------------------------------------------------- TUI

class TUI:
    def __init__(self, scr, cfg: dict):
        self.scr = scr
        self.cfg = cfg
        self.rows = []
        self.sel = 0
        self.status = ""
        self.err = False
        self.build_rows()

    def build_rows(self):
        c = self.cfg
        is_stream = c["mode"] == MODE_STREAM
        rows = [
            dict(kind="cycle", key="mode", label="模式", choices=MODES, value=c["mode"],
                 hint="wf-recorder 流 = 把远端屏幕投过来（推荐，已实测）；waypipe = 把远端应用拿到本机跑，【不能】镜像远端屏幕"),
            dict(kind="text", key="host", label="目标主机", value=c["host"], hint="远端 IP/主机名；Ctrl-L 选候选"),
            dict(kind="text", key="user", label="登录用户", value=c["user"], hint="留空 = ssh 默认用户"),
            dict(kind="text", key="port", label="SSH 端口", value=c["port"], hint="非 22 时 waypipe 模式会自动包一层 ssh"),
        ]
        if is_stream:
            rows += [
                dict(kind="text", key="output", label="远端输出名", value=c["output"], hint="如 DSI-1 / eDP-1；可用「探测远端输出名」"),
                dict(kind="cycle", key="fps", label="帧率", choices=FPS_CHOICES, value=c["fps"], hint="远端软编吃 CPU：手机/SBC 建议 15~20"),
                dict(kind="cycle", key="scale", label="缩放宽度", choices=SCALE_CHOICES, value=c["scale"], hint="portal 模式：门户已给逻辑分辨率（手机约 360），再放大没有收益；wf-recorder 模式建议 360~540"),
                dict(kind="cycle", key="encode", label="编码档位", choices=list(ENCODE_PROFILES), value=c.get("encode", "极速（手机 CPU 最低）"),
                     hint="实测：压得越狠≠越快（medium 比 superfast 手机 CPU 翻倍且延迟无改善）；要更快请降帧率/分辨率"),
                dict(kind="cycle", key="codec", label="编码器", choices=CODEC_CHOICES, value=c["codec"], hint="libx264 通用；h264_v4l2m2m 试硬件编码"),
                dict(kind="cycle", key="muxer", label="封装", choices=MUXER_CHOICES, value=c["muxer"],
                     hint="h264 = 裸流无容器（最低延迟，实测 Cache 0.00s）；mpegts 会多 ~0.5s 缓冲"),
                dict(kind="cycle", key="display", label="窗口显示", choices=DISPLAY_CHOICES,
                     value=(c.get("display") if c.get("display") in DISPLAY_CHOICES else "半幅窗口"),
                     hint="半幅窗口 = 普通窗口（宽 = 工作区一半），mpv 把远端画面缩放居中；贴合视频 = 窗口紧贴画面（9:19 竖条）；全屏 = --fs"),
                *([dict(kind="cycle", key="remote_client", label="远端客户端",
                        choices=["auto（优先 C）", "c（libdbus）", "python"], value=(c.get("remote_client") or "auto"),
                        hint="C 版启动 0.07s / 峰值 RSS 1.2MB；Python 版 0.4s / 25MB。auto=优先 C，编译不出来自动回退")]
                  if c.get("mode") == MODE_PORTAL else []),
                dict(kind="toggle", key="keep_size", label="锁定窗口尺寸", value=c.get("keep_size", True),
                     hint="niri 会把窗口压在当时的可用区内且不会自己长回来（实测邻窗 1013 时投屏停在 997）；开着就自动拉回。注意：手动用 Mod+U 调高度也会被拉回"),
                dict(kind="toggle", key="lowlat", label="极低延迟 mpv", value=c.get("lowlat", True),
                     hint="--untimed --video-sync=desync --demuxer-readahead-secs=0"),
                dict(kind="cycle", key="hwdec", label="本机硬解", choices=HWDEC_CHOICES, value=c.get("hwdec", "vaapi"),
                     hint="保持 vaapi：auto-safe 会挑 Vulkan 解码，而本机 Mesa 没有 VK_KHR_video_decode_queue → 逐帧报错 no frame"),
                dict(kind="text", key="mpv_extra", label="mpv 附加参数", value=c["mpv_extra"], hint="如 --no-audio --speed=1"),
            ]
        else:
            rows += [
                dict(kind="cycle", key="compress", label="压缩", choices=COMPRESS_CHOICES, value=c["compress"], hint="waypipe 传输压缩"),
                dict(kind="cycle", key="video", label="视频编码", choices=VIDEO_CHOICES, value=c["video"], hint="需要两端都有 Vulkan"),
                dict(kind="toggle", key="nogpu", label="强制 --no-gpu", value=c["nogpu"], hint="无 Vulkan 时打开"),
                dict(kind="cycle", key="display", label="窗口显示", choices=DISPLAY_CHOICES, value=c["display"], hint="（waypipe 模式保留项）"),
                dict(kind="text", key="wp_command", label="远端命令", value=c["wp_command"], hint="要在本机跑起来的远端程序，如 foot / weston-terminal"),
            ]
        rows += [
            dict(kind="toggle", key="wake", label="投屏前先唤醒远端屏幕", value=c["wake"],
                 hint="必开！远端息屏时 screencopy 会失败（grim: no supported format found / wf-recorder: Failed to copy frame）"),
            dict(kind="toggle", key="quiet", label="运行时不刷报文", value=c.get("quiet", True),
                 hint="远端 ffmpeg 报告 2>/dev/null + mpv --really-quiet --no-terminal；日志写入 ~/.local/state/mirror-screen/last-run.log"),
            dict(kind="toggle", key="exit_on_close", label="关掉窗口即退出", value=c.get("exit_on_close", True),
                 hint="mpv 窗口一关就结束整个程序，并顺手清理远端 wf-recorder（不留下残留进程）"),
            dict(kind="toggle", key="close_tui", label="投屏时关闭本界面", value=c.get("close_tui", True),
                 hint="开投屏后把 TUI 连同它所在的终端窗口一起关掉，只留投屏窗口（后台盘居进程负责收尾）"),
            dict(kind="action", label="▶ 开始投屏", action="run", hint="Ctrl-R；关掉 mpv 窗口即自动结束"),
            dict(kind="action", label="唤醒远端屏幕", action="wake", hint="wlr-randr --output <输出名> --on"),
            dict(kind="action", label="体检远端环境", action="check", hint="桌面/工具/抓屏实测(含息屏检测)/Vulkan，缺什么直接告诉你"),
            dict(kind="action", label="探测远端输出名", action="probe", hint="niri→sway→hyprctl→wlr-randr 依次尝试"),
            dict(kind="action", label="测试 SSH 连通", action="sshtest", hint="BatchMode，不会卡在密码提示"),
            dict(kind="action", label="保存配置", action="save", hint="Ctrl-S"),
            dict(kind="action", label="退出", action="quit", hint="q"),
        ]
        self.rows = rows
        self.sel = min(self.sel, len(rows) - 1)

    # ---- 绘制
    def draw(self):
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        self._add(0, 0, " 投屏到另一台 Linux · wf-recorder/waypipe 配置器 ".ljust(w - 1), curses.A_REVERSE)
        y = 2
        for i, row in enumerate(self.rows):
            sel = (i == self.sel)
            attr = curses.A_REVERSE if sel else curses.A_NORMAL
            if row["kind"] == "action":
                self._add(y, 2, ("▸ " if sel else "  ") + row["label"], curses.A_REVERSE if sel else curses.A_BOLD)
            else:
                self._add(y, 2, ("▸ " if sel else "  ") + f"{row['label']:<14}", curses.A_BOLD)
                val = "[✓]" if row["kind"] == "toggle" and row.get("value") else ("[ ]" if row["kind"] == "toggle" else (str(row.get("value", "")) or "（空）"))
                self._add(y, 20, val[: max(0, w - 22)], attr)
            y += 1
        y += 1
        self._add(y, 2, "将执行：", curses.A_BOLD)
        y += 1
        for line in self._wrap(preview(self.cfg), w - 4):
            self._add(y, 4, line, curses.A_DIM)
            y += 1
        y += 1
        if self.cfg["mode"] == MODE_WAYPIPE:
            self._add(y, 2, "[注意] waypipe 模式显示的是远端程序，不是远端屏幕", curses.A_BOLD)
            y += 1
        hint = self.rows[self.sel].get("hint", "")
        if hint:
            self._add(y, 2, hint[: w - 4], curses.A_DIM)
            y += 1
        if self.status:
            self._add(y, 2, self.status[: w - 4], curses.A_BOLD)
        self._add(h - 1, 0, " ↑↓ 选择  Enter 编辑/执行  ←→ 切换  Ctrl-L 候选主机  Ctrl-R 投屏  Ctrl-W 唤醒远端  q 退出 ".ljust(w - 1), curses.A_REVERSE)
        self.scr.refresh()

    def _add(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h:
            try:
                self.scr.addstr(y, x, text[: max(0, w - x - 1)], attr)
            except curses.error:
                pass

    @staticmethod
    def _wrap(text, width):
        out, cur = [], ""
        for tok in str(text).split(" "):
            if len(cur) + len(tok) + 1 > width:
                out.append(cur)
                cur = tok
            else:
                cur = f"{cur} {tok}".strip()
        out.append(cur)
        return out

    # ---- 编辑/交互
    def edit_line(self, prompt, initial):
        h, w = self.scr.getmaxyx()
        curses.curs_set(1)
        buf = list(str(initial))
        while True:
            self._add(h - 2, 0, (prompt + "".join(buf)).ljust(w - 1))
            self.scr.move(h - 2, min(len(prompt) + len(buf), w - 2))
            self.scr.refresh()
            ch = self.scr.get_wch()
            if ch in ("\n", "\r", curses.KEY_ENTER):
                break
            if ch == "\x1b":
                curses.curs_set(0)
                return initial
            if ch in ("\x7f", "\b", curses.KEY_BACKSPACE):
                if buf:
                    buf.pop()
                continue
            if ch == "\x15":
                buf.clear()
                continue
            if isinstance(ch, str) and ch.isprintable():
                buf.append(ch)
        curses.curs_set(0)
        return "".join(buf)

    def pick_host(self):
        cands = host_candidates(self.cfg)
        if not cands:
            self.status = "没有候选主机，请直接编辑「目标主机」"
            return
        h, w = self.scr.getmaxyx()
        idx = 0
        while True:
            self.scr.erase()
            self._add(0, 0, " 选择目标主机（Enter 确认，Esc 取消） ".ljust(w - 1), curses.A_REVERSE)
            for i, item in enumerate(cands[: h - 3]):
                host, src = item.split("|", 1)
                self._add(1 + i, 2, f"{host:<30} {src}", curses.A_REVERSE if i == idx else curses.A_NORMAL)
            self.scr.refresh()
            ch = self.scr.get_wch()
            if ch == "\x1b":
                return
            if ch in ("\n", "\r", curses.KEY_ENTER):
                self.cfg["host"] = cands[idx].split("|")[0]
                self.build_rows()
                self.status = f"目标已设为 {self.cfg['host']}"
                return
            if ch in (curses.KEY_UP, "k"):
                idx = max(0, idx - 1)
            if ch in (curses.KEY_DOWN, "j"):
                idx = min(len(cands) - 1, idx + 1)

    # ---- 动作
    def _ssh_run(self, script, timeout=25):
        return run_capture(ssh_argv(self.cfg) + [script], timeout)

    def do_wake(self):
        out = (self.cfg.get("output") or "").strip()
        if not out:
            self.status = "先填/探测「远端输出名」再唤醒"
            self.err = True
            return
        self.status = f"正在唤醒远端屏幕（{out}）…"
        self.draw()
        code, txt = self._ssh_run(f"wlr-randr --output {out} --on 2>&1; sleep 1; cat /sys/class/drm/card*-{out}/dpms 2>/dev/null | head -1")
        self.status = f"唤醒结果：{txt.splitlines()[-1] if txt else '(无输出)'}（rc={code}）"
        self.err = code != 0

    def do_check(self):
        self.status = "正在体检远端…"
        self.draw()
        code, out = self._ssh_run(CHECK_SCRIPT, 40)
        info = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                info[k.strip()] = v.strip()
        if code != 0 or not info:
            self.status = f"体检失败（rc={code}）：{(out.splitlines() or [''])[-1][:110]}"
            self.err = True
            return
        notes = []
        comp = info.get("compositor", "unknown")
        if info.get("wfrecorder") != "yes":
            notes.append("[!] 远端缺 wf-recorder → apk add wf-recorder / pacman -S wf-recorder")
        if info.get("wlrrandr") != "yes" and info.get("output"):
            notes.append("[!] 远端缺 wlr-randr（无法远程唤醒屏幕）")
        ps = (info.get("powersave") or "").strip()
        if ps == "on":
            notes.append("[!] 远端 WiFi 省电开着 → 延迟 ~95ms、抖动 70ms（关掉后 ~11ms）：sudo iw dev wlan0 set power_save off")
        cap = (info.get("capture") or "").strip()
        if cap in ("FAIL", ""):
            dpms = (info.get("drmdpms") or "").strip()
            notes.append(f"[!] 远端抓屏失败（dpms={dpms or '?'}）→ 多半是息屏，点「唤醒远端屏幕」")
        else:
            notes.append(f"远端 OK：{comp} · {info.get('display')} · 抓屏 {cap} 字节 · Vulkan {info.get('vulkan')} ICD")
        if comp == "unknown":
            notes.append("[!] 桌面类型未识别（GNOME/KDE 不支持 screencopy 类方案）")
        self.build_rows()
        self.status = "；".join(notes)
        self.err = any(n.startswith("[!]") for n in notes)

    def run_cast(self):
        if not (self.cfg.get("host") or "").strip():
            self.status = "请先填「目标主机」"
            self.err = True
            return
        if self.cfg.get("wake"):
            self.do_wake()
        save_cfg(self.cfg)
        if self.cfg.get("close_tui", True):
            self.detach_cast()
            return
        curses.def_prog_mode()
        curses.endwin()
        rc = run_pipeline(self.cfg)
        curses.reset_prog_mode()
        self.scr.refresh()
        self.status = f"投屏结束（rc={rc}）· 日志：{state_log_path()}"
        self.err = not is_normal_exit(rc)
        if self.cfg.get("exit_on_close", True):
            if not is_normal_exit(rc):
                print(f"投屏异常结束 rc={rc}；日志：{state_log_path()}")
            raise SystemExit(0 if is_normal_exit(rc) else rc)

    def detach_cast(self):
        """把投屏放到一个脱离终端的新会话里跑，然后自己退出 —— 终端窗口随之关闭。"""
        stop_previous_cast()
        cmd = [sys.executable, str(Path(__file__).resolve()), "--run"]
        try:
            child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL, start_new_session=True,
                                     cwd=str(Path.home()))
        except Exception as e:
            self.status = f"启动失败：{e}"
            self.err = True
            return
        # 给 2 秒看它是不是立刻挂了（那样直接把日志尾部显示出来，不退界面）
        try:
            rc = child.wait(timeout=2)
        except subprocess.TimeoutExpired:
            rc = None
        if rc is not None:
            try:
                tail = "；".join(state_log_path().read_text(errors="replace").splitlines()[-3:])
            except Exception:
                tail = ""
            self.status = f"投屏启动失败（rc={rc}）：{tail[:220]}"
            self.err = True
            return
        raise SystemExit(0)          # 界面就此关闭（fuzzel 拉起的终端也会跟着关）

    def do_action(self, action):
        if action == "run":
            self.run_cast()
        elif action == "wake":
            self.do_wake()
        elif action == "check":
            self.do_check()
        elif action == "probe":
            self.status = "正在探测远端输出名…"
            self.draw()
            code, out = self._ssh_run(PROBE_SCRIPT, 25)
            first = next((l.strip() for l in out.splitlines() if l.strip() and " " not in l.strip()), "")
            if code == 0 and first:
                self.cfg["output"] = first
                self.build_rows()
                self.status = f"远端输出 = {first}"
                self.err = False
            else:
                self.status = f"探测失败（rc={code}）：{out.splitlines()[-1][:110] if out else '无输出'}"
                self.err = True
        elif action == "sshtest":
            self.status = "正在测试 SSH…"
            self.draw()
            code, out = self._ssh_run("true", 15)
            self.status = "SSH 可用" if code == 0 else f"SSH 不通（rc={code}）：{out.splitlines()[-1][:110] if out else ''}"
            self.err = code != 0
        elif action == "save":
            save_cfg(self.cfg)
            self.status = f"已保存到 {CFG_FILE}"
            self.err = False
        elif action == "quit":
            save_cfg(self.cfg)
            raise SystemExit

    def loop(self):
        curses.curs_set(0)
        while True:
            self.draw()
            row = self.rows[self.sel]
            ch = self.scr.get_wch()
            if ch in ("q", "\x03"):
                save_cfg(self.cfg)
                return
            if ch in (curses.KEY_UP, "k"):
                self.sel = (self.sel - 1) % len(self.rows)
            elif ch in (curses.KEY_DOWN, "j"):
                self.sel = (self.sel + 1) % len(self.rows)
            elif ch == "\x0c":
                self.pick_host()
            elif ch == "\x12":
                self.run_cast()
            elif ch == "\x17":
                self.do_wake()
            elif ch == "\x13":
                save_cfg(self.cfg)
                self.status = f"已保存到 {CFG_FILE}"
            elif ch in ("\n", "\r", curses.KEY_ENTER, " "):
                if row["kind"] == "action":
                    self.do_action(row["action"])
                elif row["kind"] == "text":
                    self.cfg[row["key"]] = self.edit_line(f"{row['label']}：", row["value"])
                    self.build_rows()
                elif row["kind"] == "cycle":
                    self.cfg[row["key"]] = cycle(row["choices"], self.cfg.get(row["key"]))
                    self.build_rows()
                elif row["kind"] == "toggle":
                    self.cfg[row["key"]] = not self.cfg.get(row["key"], False)
                    self.build_rows()
            elif ch in (curses.KEY_RIGHT, "l", "+"):
                if row["kind"] == "cycle":
                    self.cfg[row["key"]] = cycle(row["choices"], self.cfg.get(row["key"]))
                elif row["kind"] == "toggle":
                    self.cfg[row["key"]] = not self.cfg.get(row["key"], False)
                self.build_rows()
            elif ch in (curses.KEY_LEFT, "h", "-"):
                if row["kind"] == "cycle":
                    self.cfg[row["key"]] = cycle(row["choices"], self.cfg.get(row["key"]), -1)
                elif row["kind"] == "toggle":
                    self.cfg[row["key"]] = not self.cfg.get(row["key"], False)
                self.build_rows()


# ---------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser(description="把远端 Linux 屏幕投到本机（wf-recorder→mpv）或把远端应用拿到本机跑（waypipe）")
    ap.add_argument("--run", action="store_true", help="用保存的配置直接投屏")
    ap.add_argument("--wake", action="store_true", help="只唤醒远端屏幕")
    ap.add_argument("--check", action="store_true", help="体检远端环境")
    ap.add_argument("--dry-run", action="store_true", help="只打印将要执行的命令")
    ap.add_argument("--list-hosts", action="store_true", help="列出候选 SSH 目标")
    ap.add_argument("--print-config", action="store_true", help="打印当前配置 JSON")
    args = ap.parse_args()

    cfg = load_cfg()

    if args.list_hosts:
        for item in host_candidates(cfg):
            print(item.replace("|", "\t"))
        return 0
    if args.print_config:
        print(json.dumps(cfg, ensure_ascii=False, indent=2))
        return 0
    if args.dry_run:
        print(preview(cfg))
        return 0
    if args.wake:
        code, out = run_capture(ssh_argv(cfg) + [f"wlr-randr --output {cfg['output']} --on 2>&1; sleep 1; cat /sys/class/drm/card*-{cfg['output']}/dpms 2>/dev/null | head -1"], 25)
        print(out or "(无输出)")
        return code
    if args.check:
        code, out = run_capture(ssh_argv(cfg) + [CHECK_SCRIPT], 40)
        print(out)
        return code
    if args.run:
        if not (cfg.get("host") or "").strip():
            print("尚未配置目标主机，请先运行 mirror-screen 打开配置界面。", file=sys.stderr)
            return 2
        stop_previous_cast()
        # 优先用 C 引擎跑投屏（监管进程常驻 ~1.2MB，Python 版 27.9MB；启动 1ms vs 77ms）
        eng = c_engine_path()
        if eng and (cfg.get("engine") or "auto") != "python":
            try:
                sync_portal_script(cfg)          # 远端客户端准备（C 编译/回退），日志在投屏日志里
                os.execv(str(eng), [str(eng), "--run"])   # exec：不留 Python 进程
            except Exception as e:
                print(f"（C 引擎启动失败，回退 Python 实现：{e}）", file=sys.stderr)
        rc = run_pipeline(cfg)
        if not is_normal_exit(rc):
            print(f"投屏异常结束 rc={rc}；日志：{state_log_path()}", file=sys.stderr)
        return rc
    if not sys.stdout.isatty():
        print("需要在终端里运行（这是 TUI）。可用 --run / --wake / --check / --dry-run / --list-hosts。", file=sys.stderr)
        return 2
    try:
        locale.setlocale(locale.LC_ALL, "")
    except Exception:
        pass
    return curses.wrapper(lambda scr: TUI(scr, cfg).loop()) or 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
