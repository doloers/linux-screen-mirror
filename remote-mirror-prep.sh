#!/bin/bash
# remote-mirror-prep.sh — 在「被投屏的远端」上准备屏幕外发所需的一切
#
# 用途：让远端具备「屏幕 → 本机」的能力。本脚本【在远端执行】，本机只需能 ssh 过去。
# 用法：
#   bash remote-mirror-prep.sh                      # 只体检（不需要 root）
#   sudo bash remote-mirror-prep.sh --install       # 装最小集（wf-recorder + wlr-randr + grim）
#   sudo bash remote-mirror-prep.sh --install-all   # 再加 waypipe + wl-mirror（"把远端应用拿到本机跑"用）
#   bash remote-mirror-prep.sh --help
#
# 背景（2026-09-30 实测：OnePlus 6 / postmarketOS / Phosh）：
#   · 正确的投屏链路是「远端采集编码 → 管道 → 本机播放」：
#         ssh 远端 'wf-recorder -o <输出> -c libx264 -m mpegts -f /dev/stdout' | mpv -
#   · waypipe **不能**镜像远端屏幕：它让远端应用看到的是**本机** compositor
#     （实测：waypipe 里跑 wayland-info，wl_output 报的是本机 eDP-1）。
#     所以 waypipe + wl-mirror 只适合"把远端应用拿到本机跑"。
#   · 远端**息屏/DPMS off** 时 screencopy 必然失败，现象像协议问题：
#       grim: "no supported format found" / wf-recorder: "Failed to copy frame"
#     唤醒：wlr-randr --output <输出> --on
# 回滚：见脚本末尾（按发行版列出卸载命令；本脚本不改任何配置文件）
set -uo pipefail

SELF="remote-mirror-prep.sh"
MODE="check"
for a in "$@"; do
  case "$a" in
    --check) MODE="check" ;;
    --install) MODE="install" ;;
    --install-all) MODE="install-all" ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "未知参数：$a（用 --help）" >&2; exit 2 ;;
  esac
done

say()  { printf '%s\n' "$*"; }
ok()   { printf '  [OK]   %s\n' "$*"; }
miss() { printf '  [缺]   %s\n' "$*"; }
warn() { printf '  [注意] %s\n' "$*"; }
hdr()  { printf '\n== %s ==\n' "$*"; }

# ---------- 身份/环境 ----------
# 关键：用 sudo 跑时，显示相关的探测必须切回**会话用户**身份；否则 XDG_RUNTIME_DIR 是 root 的，
# wlr-randr / grim 全都读不到 Wayland socket（这是本脚本第一版踩过的坑）。
RUNAS=""
if [ "$(id -u)" -eq 0 ] && [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
  RUNAS="$SUDO_USER"
  export XDG_RUNTIME_DIR="/run/user/$(id -u "$RUNAS")"
else
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
fi
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
  WAYLAND_DISPLAY=$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v lock | head -1)" 2>/dev/null)
  export WAYLAND_DISPLAY
fi
export NIRI_SOCKET="${NIRI_SOCKET:-$(ls "$XDG_RUNTIME_DIR"/niri.*.sock 2>/dev/null | head -1)}"
asuser() { if [ -n "$RUNAS" ]; then sudo -u "$RUNAS" env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" NIRI_SOCKET="${NIRI_SOCKET:-}" "$@"; else "$@"; fi; }
# command -v 是 shell 内建，不能直接当外部程序跑，所以另开一层 sh
asuser_sh() { if [ -n "$RUNAS" ]; then sudo -u "$RUNAS" env XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" sh -c "$1"; else sh -c "$1"; fi; }
has() { asuser_sh "command -v $1 >/dev/null 2>&1"; }

hdr "环境"
DISTRO="unknown"; PKG=""
if   command -v apk    >/dev/null 2>&1; then DISTRO="apk";    PKG="apk add --no-cache"
elif command -v pacman >/dev/null 2>&1; then DISTRO="arch";   PKG="pacman -S --needed --noconfirm"
elif command -v apt    >/dev/null 2>&1; then DISTRO="debian"; PKG="apt-get install -y"
elif command -v dnf    >/dev/null 2>&1; then DISTRO="fedora"; PKG="dnf install -y"
elif command -v zypper >/dev/null 2>&1; then DISTRO="suse";   PKG="zypper install -y"
fi
say "  发行版      : $DISTRO   $(grep -E '^PRETTY_NAME' /etc/os-release 2>/dev/null | cut -d= -f2- | tr -d '"')"
say "  架构/内核   : $(uname -m) / $(uname -r)"
PCIIDS=$( (lspci -nn 2>/dev/null || true) | grep -iE 'vga|3d|display' )
if [ -n "$PCIIDS" ]; then
  say "  显卡        : $(echo "$PCIIDS" | head -1 | sed 's/^[0-9a-f:.]* *//')"
  VENDORS=""
  echo "$PCIIDS" | grep -qE '\[8086:' && VENDORS="$VENDORS intel"
  echo "$PCIIDS" | grep -qE '\[1002:' && VENDORS="$VENDORS amd"
  echo "$PCIIDS" | grep -qE '\[10de:' && VENDORS="$VENDORS nvidia"
  VENDORS=${VENDORS# }
else
  say "  显卡        : 无 PCI 显卡（SoC，如手机/SBC —— 通常没有 VA-API 硬件编码）"
fi

# ---------- 桌面会话 ----------
hdr "桌面会话（screencopy 类方案要求 wlroots 系：niri/sway/hyprland/phoc/labwc/river/weston）"
COMP="unknown"
if   asuser niri msg outputs       >/dev/null 2>&1; then COMP="niri"
elif asuser swaymsg -t get_outputs >/dev/null 2>&1; then COMP="sway"
elif asuser hyprctl monitors       >/dev/null 2>&1; then COMP="hyprland"
elif pgrep -x phoc                 >/dev/null 2>&1; then COMP="phoc (Phosh)"
elif pgrep -x labwc                >/dev/null 2>&1; then COMP="labwc"
elif pgrep -x river                >/dev/null 2>&1; then COMP="river"
fi
say "  WAYLAND_DISPLAY = ${WAYLAND_DISPLAY:-（空）}    探测身份 = ${RUNAS:-$(id -un)}"
if [ "$COMP" = "unknown" ]; then
  miss "桌面类型没认出来（GNOME/KDE 不支持 screencopy 类方案）"
else
  ok "桌面 = $COMP"
fi

OUT=""
if asuser_sh "command -v wlr-randr >/dev/null 2>&1"; then
  OUT=$(asuser wlr-randr 2>/dev/null | awk 'NF && $1 ~ /^[A-Za-z]/ {print $1; exit}')
fi
# 兑底：按合成器自带的 CLI 取（没装 wlr-randr 也能拿到，如 niri 用 niri msg outputs）
if [ -z "$OUT" ]; then
  case "$COMP" in
    niri)     OUT=$(asuser sh -c 'niri msg outputs 2>/dev/null | sed -n "s/^Output \"[^\"]*\" (\([^)]*\)).*/\1/p" | head -1') ;;
    sway)     OUT=$(asuser sh -c 'swaymsg -t get_outputs 2>/dev/null | sed -n "s/.*\"name\": \"\([^\"]*\)\".*/\1/p" | head -1') ;;
    hyprland) OUT=$(asuser sh -c 'hyprctl monitors 2>/dev/null | sed -n "s/^Monitor \([^ ]*\).*/\1/p" | head -1') ;;
  esac
fi
if [ -n "$OUT" ]; then
  ok "输出名 = $OUT"
else
  miss "输出名未知（需要会话用户身份下的 wlr-randr；也可看 /sys/class/drm/card*-*/status）"
fi

# ---------- 息屏/DPMS ----------
hdr "屏幕电源状态（息屏 = 抓屏必然失败，这是第一大坑）"
DPMS=""
if [ -n "$OUT" ]; then
  DPMS=$(cat /sys/class/drm/card*-"$OUT"/dpms 2>/dev/null | head -1)
fi
case "$DPMS" in
  Off|off) miss "DPMS = Off（息屏）→ 先唤醒：wlr-randr --output $OUT --on" ;;
  "")      warn "读不到 DPMS（非 DRM 平台或权限不足）；若抓屏失败先怀疑息屏" ;;
  *)       ok "DPMS = $DPMS" ;;
esac

# ---------- 抓屏实测 ----------
hdr "抓屏实测（最权威的一步）"
if asuser grim -o "${OUT:-__none__}" /tmp/.prep-test.png >/dev/null 2>&1; then
  SZ=$(stat -c%s /tmp/.prep-test.png 2>/dev/null)
  ok "grim 抓屏成功（${SZ:-?} 字节）"
elif command -v grim >/dev/null 2>&1 || has grim; then
  miss "grim 抓屏失败（典型报错 'no supported format found'）→ 99% 是屏幕息屏，唤醒后再试"
else
  warn "没装 grim，无法做抓屏实测（apk add grim / pacman -S grim）"
fi
rm -f /tmp/.prep-test.png

# ---------- 组件 ----------
hdr "必需组件（屏幕 → 本机）"
NEED=()
for c in wf-recorder wlr-randr; do
  if has "$c"; then ok "$c 已装"; else miss "$c 未装"; NEED+=("$c"); fi
done
hdr "可选组件（把远端应用拿到本机跑：waypipe）"
for c in waypipe wl-mirror; do
  if has "$c"; then ok "$c 已装"; else warn "$c 未装（非必需）"; fi
done

# ---------- sshd 转发（仅 waypipe 需要） ----------
hdr "sshd 转发开关（只有 waypipe 模式依赖）"
if [ "$(id -u)" -eq 0 ] && command -v sshd >/dev/null 2>&1; then
  FWD=$(sshd -T 2>/dev/null | awk '/^allowtcpforwarding/{print $2}')
  case "$FWD" in
    no) miss "AllowTcpForwarding=no → waypipe 会报 'remote port forwarding failed'（改 /etc/ssh/sshd_config 后重启 sshd）" ;;
    *)  ok "AllowTcpForwarding=${FWD:-默认}" ;;
  esac
else
  say "  （需要 root 才能读；远端可跑：sudo sshd -T | grep -i allowtcpforwarding）"
fi

# ---------- 硬件编码 ----------
hdr "硬件编码能力（可选；SoC/手机通常没有 VA-API，用软件 libx264 即可）"
if has vainfo; then
  VA=$(asuser vainfo 2>/dev/null | sed -n 's/.*Driver version: //p' | head -1)
  [ -n "$VA" ] && ok "VA-API: $VA" || warn "vainfo 无驱动 → 退回软件编码"
else
  warn "无 vainfo（非必需）"
fi
say "  提示：wf-recorder 也可试 -c h264_v4l2m2m 走 V4L2 硬编（如骁龙 venus），不行就退回 libx264"

# ---------- 安装 ----------
if [ "$MODE" != "check" ]; then
  hdr "安装（$MODE）"
  [ "$(id -u)" -eq 0 ] || { warn "安装需要 root：sudo bash $SELF $MODE"; exit 1; }
  [ "$DISTRO" = "debian" ] && apt-get update -qq
  PKGS=(wf-recorder wlr-randr grim)
  [ "$MODE" = "install-all" ] && PKGS+=(waypipe wl-mirror)
  set -x; $PKG "${PKGS[@]}"; set +x
  hdr "安装后复查"; exec bash "$0" --check
fi

# ---------- 结论 ----------
hdr "结论 / 下一步"
if [ "${#NEED[@]}" -eq 0 ] && [ -n "$OUT" ]; then
  ok "远端可以外发屏幕了。本机侧执行（或直接用 TUI「开始投屏」）："
  say  "     ssh 用户@远端 'wf-recorder -y -o $OUT -c libx264 -x yuv420p -r 20 -m mpegts -f /dev/stdout' | mpv --profile=low-latency --hwdec=vaapi --demuxer-lavf-format=mpegts -"
  say  "     （-y 必不可少：不加时 wf-recorder 会因 '/dev/stdout' 已存在而问 'Overwrite? Y/n:'，"
  say  "       而那个提问的按键未必能送进去；stdin 为 /dev/null 时凑巧能过，终端下会卡住）"
  case "$DPMS" in Off|off) say "     先唤醒： ssh 用户@远端 'wlr-randr --output $OUT --on'";; esac
  say "  手机/SBC 建议降负载：-r 15 加 -F \"scale=540:-1\""
else
  miss "还缺：${NEED[*]:-（输出名未知）} → sudo bash $SELF --install"
fi

cat <<'EOF'

== 回滚 ==
postmarketOS/Alpine : sudo apk del wf-recorder wlr-randr grim [waypipe wl-mirror]
Arch                : sudo pacman -Rns wf-recorder wlr-randr grim [waypipe wl-mirror]
Debian/Ubuntu       : sudo apt purge wf-recorder wlr-randr grim [waypipe wl-mirror]
Fedora              : sudo dnf remove wf-recorder wlr-randr grim [waypipe wl-mirror]
（本脚本只装包，不改任何配置文件；这些工具都按需运行，不留常驻服务）
EOF
