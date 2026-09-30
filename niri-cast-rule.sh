#!/usr/bin/env bash
# 投屏窗口尺寸规则（niri）——让投屏窗口恒为「半幅屏宽 × 满高」，且焦点/工作区切换都不改变尺寸。
#
# 为什么需要（2026-09-30 实测，走了不少弯路）：
#   · mpv 的 --geometry 在 Wayland 下被 mpv 自己忽略；
#   · 光加 window-rule 也不够：mpv 会按视频尺寸自己“吸附”窗口（它以为自己在浮动），
#     现象就是窗口永远比旁边的窗口矮一截（997 vs 1013），聚焦/失焦还会跳；
#   · 解法是 rule 里加 `tiled-state true`（＝告诉 mpv“你在平铺布局里”）——
#     这与 foot 按字符格吸附尺寸是同一类问题，所以很多人给 foot 也加这条。
#   · 高度不用写：niri 会把平铺窗口铺满「可用高度 − 上下 gaps」。
#   规则只匹配 app-id=portal-cast（mpv 用 --wayland-app-id=portal-cast 启动，mirror-screen.py 已默认带上），
#   所以不影响你日常看视频的 mpv（它们的 app-id 仍是 "mpv"）。
#
# 用法：
#   ./niri-cast-rule.sh check     # 看是否已装
#   ./niri-cast-rule.sh install   # 幂等安装（会先备份 config.kdl，并跑 niri validate）
#   ./niri-cast-rule.sh remove    # 卸载（只删本脚本写的那一段，并备份）
#   ./niri-cast-rule.sh install   # 重复执行 = 检查是否最新，不是则更新

set -euo pipefail

CFG="${HOME}/.config/niri/config.kdl"
MARK_BEGIN="// >>> mirror-screen:portal-cast-rule >>>"
MARK_END="// <<< mirror-screen:portal-cast-rule <<<"

block() {
    cat <<'KDL'
// >>> mirror-screen:portal-cast-rule >>>
// 投屏窗口（mpv 用 --wayland-app-id=portal-cast 启动，不影响日常 mpv）
// 目的：窗口恒为「半幅屏宽 × 满高」——和普通窗口齐平，且焦点/工作区切换都不变。
// 关键：tiled-state true —— 告诉 mpv 它被平铺了。否则 mpv 会按视频尺寸自己“吸附”窗口
//       （现象：永远比邻窗矮一截 997 vs 1013，聚焦/失焦还会跳；这也是你那条 foot 规则在做的事）。
// 高度不用写：niri 会把平铺窗口铺满「可用高度 − 上下 gaps」（本机 1028 − 16 = 1012）。
KDL
    cat <<'KDL'
window-rule {
    match app-id=r#"^portal-cast$"#
    tiled-state true
    default-column-width { proportion 0.5; }
}
// <<< mirror-screen:portal-cast-rule <<<
KDL
}

need_cfg() {
    [ -f "$CFG" ] || { echo "找不到 $CFG" >&2; exit 2; }
}

backup() {
    local dir="${HOME}/.config/niri/backups"; mkdir -p "$dir"
    local b="$dir/config.kdl.bak-$(date +%Y%m%d-%H%M%S)-niri-cast-rule"
    cp -a "$CFG" "$b"; echo "  已备份: $b"
}

case "${1:-check}" in
install)
    need_cfg
    want="$(block)"
    if grep -qF "$MARK_BEGIN" "$CFG"; then
        cur=$(python3 - "$CFG" "$MARK_BEGIN" "$MARK_END" <<'PYX'
import sys, pathlib
cfg, b, e = sys.argv[1], sys.argv[2], sys.argv[3]
t = pathlib.Path(cfg).read_text()
i, j = t.index(b), t.index(e) + len(e)
sys.stdout.write(t[i:j])
PYX
)
        if [ "$cur" = "$want" ]; then echo "  已是最新 ✓"; exit 0; fi
        backup
        python3 - "$CFG" "$MARK_BEGIN" "$MARK_END" "$want" <<'PYX'
import sys, pathlib
cfg, b, e, new = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
t = pathlib.Path(cfg).read_text()
i, j = t.index(b), t.index(e) + len(e)
pathlib.Path(cfg).write_text(t[:i] + new + t[j:])
PYX
        echo "  已更新为最新规则 ✓"
    else
        backup
        printf '\n%s\n' "$want" >> "$CFG"
        echo "  已追加规则 ✓"
    fi
    niri validate -c "$CFG" >/dev/null 2>&1 && echo "  校验通过 ✓" || { echo "  ✗ 校验失败，请回滚备份" >&2; exit 1; }
    ;;
remove)
    need_cfg
    grep -qF "$MARK_BEGIN" "$CFG" || { echo "  未安装，无需卸载"; exit 0; }
    backup
    python3 - "$CFG" "$MARK_BEGIN" "$MARK_END" <<'PY'
import sys, pathlib
cfg, b, e = sys.argv[1], sys.argv[2], sys.argv[3]
t = pathlib.Path(cfg).read_text()
i, j = t.index(b), t.index(e) + len(e)
while i > 0 and t[i-1] == "\n": i -= 1
pathlib.Path(cfg).write_text(t[:i] + t[j:].lstrip("\n"))
PY
    niri validate -c "$CFG" >/dev/null 2>&1 && echo "  已卸载并校验通过 ✓" || echo "  ✗ 校验失败，请回滚备份" >&2
    ;;
check)
    if grep -qF "$MARK_BEGIN" "$CFG"; then
        echo "  已安装："
        sed -n "/$(printf '%s' "$MARK_BEGIN" | sed 's/[][\.*^$/]/\\&/g')/,/^}/p" "$CFG" | sed 's/^/    /'
    else
        echo "  未安装（投屏窗口尺寸会随焦点变化）"
    fi
    ;;
*)
    echo "用法: $0 {check|install|remove}" >&2; exit 1;;
esac
