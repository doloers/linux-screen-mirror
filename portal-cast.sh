#!/bin/bash
# portal-cast.sh — 方案3：把远端（手机/其它 Linux）屏幕以「零编码裸视频」投到本机 mpv
#
# 链路：xdg-desktop-portal ScreenCast → PipeWire 节点 → GStreamer(只做格式转换/缩放，不编码)
#       → 裸 I420 视频 → ssh 管道 → 本机 mpv
# 与之相比：wf-recorder 路径要在远端跑 x264（占 26~35% 单核，且有固定帧率队列会堆积）；
#          本路径远端只占 ~0.6~4% 单核，且无编码队列。
#
# 用法：
#   portal-cast.sh             # 默认 360x760
#   portal-cast.sh 540 1140    # 指定输出尺寸
#   PHONE=user@host portal-cast.sh
#   portal-cast.sh --check     # 只做预检（脚本同步/屏幕唤醒/清残留），不投屏
#
# 依赖（远端）：python3-dbus、PyGObject、gstreamer + gst-plugin-pipewire + gstreamer-tools、
#               xdg-desktop-portal(含 ScreenCast 后端)
# 依赖（本机）：mpv
# 首次运行或授权被撤销时，**远端屏幕上会弹一次授权框**（点“允许”即可；之后用 restore_token 免弹窗）
set -uo pipefail

PHONE="${PHONE:-user@<phone-ip>}"
W="${1:-360}"; H="${2:-760}"
[ "${1:-}" = "--check" ] && { W=360; H=760; CHECK=1; }

HERE="$(cd "$(dirname "$(realpath "$0")")" && pwd)"
SRC="$HERE/portal_cast.py"
LOG="${XDG_STATE_HOME:-$HOME/.local/state}/portal-cast.log"
mkdir -p "$(dirname "$LOG")"
SSH_OPTS=(-o BatchMode=yes -o ServerAliveInterval=15
          -o ControlMaster=auto -o "ControlPath=$HOME/.cache/mirror-screen/cm-%r@%h-%p"
          -o ControlPersist=300)

say() { printf '%s\n' "$*" >&2; }
[ -f "$SRC" ] || { say "缺少 $SRC"; exit 1; }

# 0) 远端脚本同步（内容变了才传）
LOCAL_SUM=$(sha256sum "$SRC" | cut -c1-16)
REMOTE_SUM=$(ssh "${SSH_OPTS[@]}" "$PHONE" 'sha256sum /tmp/portal_cast.py 2>/dev/null | cut -c1-16' || true)
if [ "$LOCAL_SUM" != "$REMOTE_SUM" ]; then
  say "== 同步 portal 客户端到远端 =="
  scp -q "${SSH_OPTS[@]}" "$SRC" "$PHONE:/tmp/portal_cast.py" || { say "!! scp 失败"; exit 1; }
fi

# 1) 预检：唤醒屏幕 + 清掉残留的 gst/客户端（残留会占着 portal 节点，导致新会话异常）
say "== 预检（唤醒远端屏幕 / 清理残留）=="
ssh "${SSH_OPTS[@]}" "$PHONE" 'wlr-randr --output DSI-1 --on 2>/dev/null; sudo pkill -9 -f gst-launch 2>/dev/null; pkill -9 -f portal_cast 2>/dev/null; true'

if [ -n "${CHECK:-}" ]; then
  say "== 预检 OK（未投屏）=="
  ssh "${SSH_OPTS[@]}" "$PHONE" 'python3 /tmp/portal_cast.py --probe' 2>&1 | sed 's/^/   /'
  exit 0
fi

# 2) 投屏（mpv 关窗即结束；结束时清理远端）
say "== 投屏中（关掉 mpv 窗口即结束）尺寸 ${W}x${H}，日志：$LOG =="
ssh "${SSH_OPTS[@]}" "$PHONE" "W=$W H=$H python3 /tmp/portal_cast.py" 2>>"$LOG" | \
mpv --no-config --hwdec=no \
    --demuxer-lavf-format=rawvideo \
    --demuxer-lavf-o="video_size=${W}x${H},pixel_format=yuv420p" \
    --untimed --video-sync=desync --force-window=immediate --fs \
    --really-quiet --no-terminal --input-ipc-server="${XDG_RUNTIME_DIR:-/tmp}/portal-cast.sock" \
    --title="PORTAL-CAST:${PHONE}" - >>"$LOG" 2>&1
rc=$?

say "== 收尾（清理远端进程）=="
ssh "${SSH_OPTS[@]}" "$PHONE" 'pkill -9 -f portal_cast 2>/dev/null; sudo pkill -9 -f gst-launch 2>/dev/null; true'
say "投屏结束 rc=$rc；日志：$LOG"
exit "$rc"
