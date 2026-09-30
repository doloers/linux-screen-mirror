#!/usr/bin/env python3
"""从 mirror-screen.py 导出 C 侧需要的常量（模式/档位/远端脚本/默认值）→ stdout。
pip 无关：只看同目录或 ~/AIworks/02-本机运维 下的 mirror-screen.py。"""
import importlib.util, pathlib, sys

def find():
    here = pathlib.Path(__file__).resolve().parent
    for p in (here / "mirror-screen.py", pathlib.Path.home() / "AIworks/02-本机运维/mirror-screen.py"):
        if p.exists():
            return p
    sys.exit("找不到 mirror-screen.py")

spec = importlib.util.spec_from_file_location("ms", find())
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

def cstr(s):
    out = []
    for ch in s:
        if ch == "\\": out.append("\\\\")
        elif ch == '"': out.append('\\"')
        elif ch == "\n": out.append('\\n"\n    "')
        elif ch == "\t": out.append("\\t")
        else:
            b = ch.encode()
            if len(b) == 1 and 32 <= b[0] < 127: out.append(ch)
            else: out.append("".join(f"\\x{x:02x}" for x in b) + '""')
    return '"' + "".join(out) + '"'

print("/* 自动生成（gen-consts.py）：从 mirror-screen.py 导出的常量与远端脚本，勿手改 */")
print("#ifndef MS_CONSTS_H\n#define MS_CONSTS_H\n")
def arr(name, items):
    print(f"static const char *{name}[] = {{")
    for it in items: print(f"    {cstr(it)},")
    print("    0\n};\n")
arr("MS_MODES", m.MODES); arr("MS_FPS", m.FPS_CHOICES); arr("MS_SCALE", m.SCALE_CHOICES)
arr("MS_CODEC", m.CODEC_CHOICES); arr("MS_MUXER", m.MUXER_CHOICES); arr("MS_HWDEC", m.HWDEC_CHOICES)
arr("MS_DISPLAY", m.DISPLAY_CHOICES); arr("MS_COMPRESS", m.COMPRESS_CHOICES); arr("MS_VIDEO", m.VIDEO_CHOICES)
arr("MS_ENCODE_KEYS", list(m.ENCODE_PROFILES))
arr("MS_PROFILE_KEYS", ["auto（优先 C）", "c（libdbus）", "python"])
arr("MS_DISPLAY_ARG_KEYS", ["半幅窗口", "贴合视频", "全屏"])
print("static const char *MS_CHECK_SCRIPT =\n    " + cstr(m.CHECK_SCRIPT) + ";\n")
print("static const char *MS_PROBE_SCRIPT =\n    " + cstr(m.PROBE_SCRIPT) + ";\n")
print("static const char *MS_DEFAULT_KEYS[] = {")
print("    " + ", ".join(cstr(k) for k in m.DEFAULTS) + ", 0};")
print("static const char *MS_DEFAULT_STR[] = {")
print("    " + ", ".join(cstr("true" if v is True else ("false" if v is False else str(v))) for v in m.DEFAULTS.values()) + ", 0};")
print("static const int MS_DEFAULT_STRS[] = {")
print("    " + ", ".join("1" if isinstance(v, bool) else "0" for v in m.DEFAULTS.values()) + ", 0};")
print("#endif")
