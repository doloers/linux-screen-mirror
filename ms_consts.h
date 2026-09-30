/* 自动生成：从 mirror-screen.py 导出的常量与远端脚本（勿手改） */
#ifndef MS_CONSTS_H
#define MS_CONSTS_H

static const char *MS_MODES[] = {
    "\xe8\xbf\x9c""\xe7\xab\xaf""\xe5\xb1\x8f""\xe5\xb9\x95"" \xe2\x86\x92"" \xe6\x9c\xac""\xe6\x9c\xba""\xef\xbc\x88""portal \xe9\x9b\xb6""\xe7\xbc\x96""\xe7\xa0\x81""\xef\xbc\x89""",
    "\xe8\xbf\x9c""\xe7\xab\xaf""\xe5\xb1\x8f""\xe5\xb9\x95"" \xe2\x86\x92"" \xe6\x9c\xac""\xe6\x9c\xba""\xe7\xaa\x97""\xe5\x8f\xa3""\xef\xbc\x88""wf-recorder \xe6\xb5\x81""\xef\xbc\x89""",
    "\xe8\xbf\x9c""\xe7\xab\xaf""\xe5\xba\x94""\xe7\x94\xa8"" \xe2\x86\x92"" \xe6\x9c\xac""\xe6\x9c\xba""\xe8\xbf\x90""\xe8\xa1\x8c""\xef\xbc\x88""waypipe\xef\xbc\x89""",
    0
};

static const char *MS_FPS[] = {
    "10",
    "15",
    "20",
    "24",
    "30",
    0
};

static const char *MS_SCALE[] = {
    "",
    "360",
    "540",
    "720",
    "1080",
    0
};

static const char *MS_CODEC[] = {
    "libx264",
    "libx264rgb",
    "mpeg4",
    "h264_v4l2m2m",
    0
};

static const char *MS_MUXER[] = {
    "h264",
    "mpegts",
    "matroska",
    "nut",
    0
};

static const char *MS_HWDEC[] = {
    "vaapi",
    "auto-safe",
    "no",
    0
};

static const char *MS_DISPLAY[] = {
    "\xe5\x8d\x8a""\xe5\xb9\x85""\xe7\xaa\x97""\xe5\x8f\xa3""",
    "\xe8\xb4\xb4""\xe5\x90\x88""\xe8\xa7\x86""\xe9\xa2\x91""",
    "\xe5\x85\xa8""\xe5\xb1\x8f""",
    0
};

static const char *MS_COMPRESS[] = {
    "lz4",
    "lz4=4",
    "zstd",
    "zstd=9",
    "none",
    0
};

static const char *MS_VIDEO[] = {
    "none",
    "h264",
    "h264,bpf=8000000",
    "vp9",
    0
};

static const char *MS_ENCODE_KEYS[] = {
    "\xe6\x9e\x81""\xe9\x80\x9f""\xef\xbc\x88""\xe6\x89\x8b""\xe6\x9c\xba"" CPU \xe6\x9c\x80""\xe4\xbd\x8e""\xef\xbc\x89""",
    "\xe5\x9d\x87""\xe8\xa1\xa1""\xef\xbc\x88""\xe9\xbb\x98""\xe8\xae\xa4""\xef\xbc\x89""",
    "\xe7\x9c\x81""\xe5\xb8\xa6""\xe5\xae\xbd""\xef\xbc\x88""\xe6\x89\x8b""\xe6\x9c\xba""\xe6\x9c\x80""\xe5\x90\x83""\xe5\x8a\x9b""\xef\xbc\x89""",
    0
};

static const char *MS_PROFILE_KEYS[] = {
    "auto\xef\xbc\x88""\xe4\xbc\x98""\xe5\x85\x88"" C\xef\xbc\x89""",
    "c\xef\xbc\x88""libdbus\xef\xbc\x89""",
    "python",
    0
};

static const char *MS_DISPLAY_ARG_KEYS[] = {
    "\xe5\x8d\x8a""\xe5\xb9\x85""\xe7\xaa\x97""\xe5\x8f\xa3""",
    "\xe8\xb4\xb4""\xe5\x90\x88""\xe8\xa7\x86""\xe9\xa2\x91""",
    "\xe5\x85\xa8""\xe5\xb1\x8f""",
    0
};

/* 远端脚本 */
static const char *MS_CHECK_SCRIPT =
    "export XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\"; if [ -z \"${WAYLAND_DISPLAY:-}\" ]; then WAYLAND_DISPLAY=$(basename \"$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v lock | head -1)\" 2>/dev/null); export WAYLAND_DISPLAY; fi; export NIRI_SOCKET=\"$(ls $XDG_RUNTIME_DIR/niri.*.sock 2>/dev/null | head -1)\"; export HYPRLAND_INSTANCE_SIGNATURE=\"$(ls $XDG_RUNTIME_DIR/hypr/ 2>/dev/null | head -1)\"; printf 'compositor=%s\\n' \"$( (niri msg outputs >/dev/null 2>&1 && echo niri) || (swaymsg -t get_outputs >/dev/null 2>&1 && echo sway) || (hyprctl monitors >/dev/null 2>&1 && echo hyprland) || (pgrep -x phoc >/dev/null 2>&1 && echo phoc/Phosh) || (pgrep -x labwc >/dev/null 2>&1 && echo labwc) || echo unknown)\"\n"
    "printf 'display=%s\\n' \"$WAYLAND_DISPLAY\"\n"
    "printf 'wfrecorder=%s\\n' \"$(command -v wf-recorder >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "printf 'wlrmirror=%s\\n' \"$(command -v wl-mirror >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "printf 'waypipe=%s\\n' \"$(command -v waypipe >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "printf 'wlrrandr=%s\\n' \"$(command -v wlr-randr >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "printf 'grim=%s\\n' \"$(command -v grim >/dev/null 2>&1 && echo yes || echo no)\"\n"
    "printf 'vulkan=%s\\n' \"$(ls /usr/share/vulkan/icd.d/*.json 2>/dev/null | wc -l)\"\n"
    "printf 'powersave=%s\\n' \"$(iw dev wlan0 get power_save 2>/dev/null | awk '{print $NF}')\"\n"
    "OUT=$(wlr-randr 2>/dev/null | awk 'NF && $1 ~ /^[A-Za-z]/ {print $1; exit}'); printf 'drmdpms=%s\\n' \"$(cat /sys/class/drm/card*-$OUT/dpms 2>/dev/null | head -1)\"; rm -f /tmp/.mirror-screen-test.png; printf 'capture=%s\\n' \"$(grim -o $OUT /tmp/.mirror-screen-test.png >/dev/null 2>&1 && stat -c%s /tmp/.mirror-screen-test.png 2>/dev/null || echo FAIL)\"; rm -f /tmp/.mirror-screen-test.png";

static const char *MS_PROBE_SCRIPT =
    "export XDG_RUNTIME_DIR=\"${XDG_RUNTIME_DIR:-/run/user/$(id -u)}\"; if [ -z \"${WAYLAND_DISPLAY:-}\" ]; then WAYLAND_DISPLAY=$(basename \"$(ls $XDG_RUNTIME_DIR/wayland-* 2>/dev/null | grep -v lock | head -1)\" 2>/dev/null); export WAYLAND_DISPLAY; fi; export NIRI_SOCKET=\"$(ls $XDG_RUNTIME_DIR/niri.*.sock 2>/dev/null | head -1)\"; export HYPRLAND_INSTANCE_SIGNATURE=\"$(ls $XDG_RUNTIME_DIR/hypr/ 2>/dev/null | head -1)\"; niri msg outputs 2>/dev/null | sed -n 's/^Output \"[^\"]*\" (\\([^)]*\\)).*/\\1/p' | head -1; swaymsg -t get_outputs 2>/dev/null | sed -n 's/.*\"name\": \"\\([^\"]*\\)\".*/\\1/p' | head -1; hyprctl monitors 2>/dev/null | sed -n 's/^Monitor \\([^ ]*\\).*/\\1/p' | head -1; wlr-randr 2>/dev/null | awk 'NF && $1 ~ /^[A-Za-z]/ {print $1; exit}'";

/* 默认配置（键名, 值, 是否字符串） */
static const char *MS_DEFAULT_KEYS[] = {
    "mode", "host", "user", "port", "output", "codec", "muxer", "fps", "scale", "encode", "lowlat", "fullscreen", "remote_client", "window_height", "keep_size", "hwdec", "quiet", "exit_on_close", "close_tui", "mpv_extra", "wake", "compress", "video", "nogpu", "display", "wp_command", 0};
static const char *MS_DEFAULT_STR[] = {
    "\xe8\xbf\x9c""\xe7\xab\xaf""\xe5\xb1\x8f""\xe5\xb9\x95"" \xe2\x86\x92"" \xe6\x9c\xac""\xe6\x9c\xba""\xef\xbc\x88""portal \xe9\x9b\xb6""\xe7\xbc\x96""\xe7\xa0\x81""\xef\xbc\x89""", "", "", "22", "", "libx264", "h264", "15", "", "\xe6\x9e\x81""\xe9\x80\x9f""\xef\xbc\x88""\xe6\x89\x8b""\xe6\x9c\xba"" CPU \xe6\x9c\x80""\xe4\xbd\x8e""\xef\xbc\x89""", "true", "false", "auto", "100%", "true", "vaapi", "true", "true", "true", "", "true", "lz4", "none", "false", "\xe5\x8d\x8a""\xe5\xb9\x85""\xe7\xaa\x97""\xe5\x8f\xa3""", "foot", 0};
static const int MS_DEFAULT_STRS[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0};
#endif
