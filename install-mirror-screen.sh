#!/bin/bash
# install-mirror-screen.sh — 安装/卸载「投屏到另一台 Linux」TUI 配置器（无需 root）
#
#   ./install-mirror-screen.sh              安装
#   ./install-mirror-screen.sh --uninstall  卸载
#
# 安装内容（全部在用户目录，可干净回滚）：
#   ~/.local/bin/mirror-screen                       TUI 主程序（来自同目录 mirror-screen.py）
#   ~/.local/share/applications/mirror-screen.desktop        fuzzel 入口：打开配置界面
#   ~/.local/share/applications/mirror-screen-quick.desktop  fuzzel 入口：用上次配置直接投屏
#   ~/.config/mirror-screen/config.json              首次运行时自动生成（卸载不删，除非 --purge）
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
BIN="$BIN_DIR/mirror-screen"
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/mirror-screen"

write_desktop() {
  local file="$1" name="$2" comment="$3" exec_args="$4"
  cat > "$file" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=$name
GenericName=Waypipe 投屏
Comment=$comment
Exec=$BIN $exec_args
Terminal=true
Categories=Utility;
Keywords=投屏;镜像;投屏配置;cast;mirror;waypipe;screen;airplay;
Icon=video-display
StartupNotify=false
EOF
}

case "${1:-install}" in
  --uninstall|uninstall)
    rm -f "$BIN" "$BIN_DIR/mirror-screen-c" "$BIN_DIR/mirror-screen-py" "$BIN_DIR/ms_consts.h" "$BIN_DIR/portal_cast.py" "$BIN_DIR/portal_cast.c" "$APP_DIR/mirror-screen.desktop" "$APP_DIR/mirror-screen-quick.desktop"
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APP_DIR" 2>/dev/null || true
    echo "已卸载：$BIN 与两个 .desktop"
    echo "配置保留在 $CFG_DIR（要一并删除：rm -rf '$CFG_DIR'）"
    exit 0
    ;;
  install|"")
    ;;
  *)
    echo "用法：$0 [--uninstall]" >&2
    exit 2
    ;;
esac

[ -f "$SRC/mirror-screen.py" ] || { echo "找不到 $SRC/mirror-screen.py" >&2; exit 1; }

mkdir -p "$BIN_DIR" "$APP_DIR"
install -m 755 "$SRC/mirror-screen.py" "$BIN"
# 备份 Python 版入口（保留为 mirror-screen-py，可随时回退）
install -m 755 "$SRC/mirror-screen.py" "$BIN_DIR/mirror-screen-py"
# C 版主程序（TUI + 运行时引擎）：常量从 Python 侧自动导出，避免两边手抄不一致
TMPC=$(mktemp -d)
if command -v gcc >/dev/null 2>&1 && [ -f "$SRC/mirror-screen-c.c" ]; then
    if command -v python3 >/dev/null 2>&1; then
        python3 "$SRC/gen-consts.py" > "$TMPC/ms_consts.h" 2>/dev/null || cp -f "$SRC/ms_consts.h" "$TMPC/" 2>/dev/null
    else
        cp -f "$SRC/ms_consts.h" "$TMPC/" 2>/dev/null
    fi
    if gcc -O2 -s -I"$TMPC" -o "$TMPC/mirror-screen" "$SRC/mirror-screen-c.c" -lncursesw -ldl 2>"$TMPC/err"; then
        install -m 755 "$TMPC/mirror-screen" "$BIN_DIR/mirror-screen"
        ln -sf "$BIN_DIR/mirror-screen" "$BIN_DIR/mirror-screen-c"
        echo "  已装 C 版主程序: $BIN_DIR/mirror-screen（TUI + 引擎；投屏监管进程 ~1.9MB，Python 版 27.9MB）"
    else
        echo "  （C 版编译失败，主入口仍用 Python 版；原因：$(head -2 "$TMPC/err" | tr '\n' ' ')）"
    fi
    # ms_consts.h 一并装好，便于离线重编
    [ -f "$TMPC/ms_consts.h" ] && install -m 644 "$TMPC/ms_consts.h" "$BIN_DIR/ms_consts.h"
else
    echo "  （无 gcc，主入口用 Python 版 mirror-screen-py）"
fi
rm -rf "$TMPC"
# 远端 portal 客户端的源码：python 版是回退，c 版会在手机上编译（更快更省）
for f in portal_cast.py portal_cast.c; do
    [ -f "$SRC/$f" ] && install -m 644 "$SRC/$f" "$BIN_DIR/$f"
done
write_desktop "$APP_DIR/mirror-screen.desktop" \
  "投屏到另一台 Linux" \
  "用 waypipe + wl-mirror 把另一台 Linux 的画面镜像到本机（TUI 配置器）" ""
write_desktop "$APP_DIR/mirror-screen-quick.desktop" \
  "投屏到另一台 Linux（直接连接）" \
  "跳过配置界面，用上次保存的目标直接开始投屏" "--run"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APP_DIR" 2>/dev/null || true

echo "== 已安装 =="
ls -l "$BIN" "$APP_DIR/mirror-screen.desktop" "$APP_DIR/mirror-screen-quick.desktop"
echo
echo "== .desktop 校验 =="
for f in "$APP_DIR/mirror-screen.desktop" "$APP_DIR/mirror-screen-quick.desktop"; do
  desktop-file-validate "$f" && echo "  ✔ $(basename "$f")"
done
echo
echo "== 自检：--dry-run（不投屏，只打印命令） =="
"$BIN" --dry-run || true
echo
echo "== 依赖检查 =="
for c in waypipe wl-mirror ssh; do
  printf '  %-10s %s\n' "$c" "$(command -v "$c" || echo '缺失！')"
done
echo
echo "fuzzel 里搜「投屏」即可看到两个入口："
echo "  投屏到另一台 Linux            → 打开 TUI 配置界面"
echo "  投屏到另一台 Linux（直接连接）→ 用上次配置直接投屏"
echo
echo "== 回滚 =="
echo "  $0 --uninstall"
echo "  配置文件（若也要删）：rm -rf '$CFG_DIR'"
