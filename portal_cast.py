#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
portal_cast.py — 通过 xdg-desktop-portal 的 ScreenCast 拿到 PipeWire 抓屏流，
用 GStreamer 做成**裸视频**（零编码）写到 stdout，配合 ssh 管道给本机 mpv 播放。

用法（在手机上跑）：
    portal_cast.py                 # 默认 360x760，I420
    W=540 H=1140 portal_cast.py    # 改输出尺寸
    portal_cast.py --probe         # 只走 portal 拿到 node/fd 后退出（不启动 GStreamer）

依赖：python3-dbus、PyGObject、gstreamer + gst-plugin-pipewire（gst-launch-1.0）
首次运行会在手机屏幕上弹出授权对话框，需要点“允许/共享”。
"""
import json
import os
import subprocess
import sys

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

PORTAL = "org.freedesktop.portal.Desktop"
OPATH = "/org/freedesktop/portal/desktop"
IFACE_SC = "org.freedesktop.portal.ScreenCast"
IFACE_REQ = "org.freedesktop.portal.Request"


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class Portal:
    def __init__(self, bus, loop):
        self.bus = bus
        self.loop = loop
        self.sc = dbus.Interface(bus.get_object(PORTAL, OPATH), IFACE_SC)
        self.n = 0

    def _wait(self, req_path):
        """等 Request 对象的 Response 信号"""
        got = {}
        handler = None

        def cb(response, results):
            got["response"] = int(response)
            got["results"] = results
            self.loop.quit()

        handler = self.bus.add_signal_receiver(
            cb, signal_name="Response", dbus_interface=IFACE_REQ, path=req_path)
        # 防止信号在 add_signal_receiver 之前就到达：先查一次（portal 不提供该属性，所以加超时）
        GLib.timeout_add_seconds(300, self.loop.quit)
        self.loop.run()
        try:
            self.bus.remove_signal_receiver(
                handler, signal_name="Response", dbus_interface=IFACE_REQ, path=req_path)
        except Exception:
            pass
        if "response" not in got:
            raise RuntimeError(f"等待 {req_path} 的 Response 超时（300 秒内没在手机上点『允许』）")
        if got["response"] != 0:
            raise RuntimeError(f"portal 返回 response={got['response']}（1=用户取消, 2=其它错误）")
        return got["results"]

    def create_session(self):
        self.n += 1
        tok = f"tok{self.n}"
        req = self.sc.CreateSession({
            "handle_token": dbus.String(tok),
            "session_handle_token": dbus.String(f"sess{tok}"),
        })
        return str(self._wait(req)["session_handle"])

    TOKEN_FILE = os.path.expanduser("~/.local/state/portal_cast.token")

    def select_sources(self, sess):
        self.n += 1
        opts = {
            "handle_token": dbus.String(f"tok{self.n}"),
            "types": dbus.UInt32(1),          # 1 = MONITOR（整屏）
            "multiple": dbus.Boolean(False),
            "cursor_mode": dbus.UInt32(2),    # 2 = 嵌入光标
            "persist_mode": dbus.UInt32(2),   # 2 = 持久授权（配合 restore_token 免二次弹窗）
        }
        try:
            tok = open(self.TOKEN_FILE).read().strip()
            if tok:
                opts["restore_token"] = dbus.String(tok)
                log("   使用已保存的 restore_token（应不再弹窗）")
        except OSError:
            pass
        req = self.sc.SelectSources(sess, opts)
        self._wait(req)

    def start(self, sess):
        self.n += 1
        req = self.sc.Start(sess, "", {"handle_token": dbus.String(f"tok{self.n}")})
        res = self._wait(req)
        streams = res.get("streams") or []
        if not streams:
            raise RuntimeError("portal 没返回任何 stream")
        tok = res.get("restore_token")
        if tok:
            try:
                os.makedirs(os.path.dirname(self.TOKEN_FILE), exist_ok=True)
                open(self.TOKEN_FILE, "w").write(str(tok))
                log("   已保存 restore_token")
            except OSError:
                pass
        node = int(streams[0][0])
        pos = streams[0][1].get("position")
        size = streams[0][1].get("size")
        return node, (tuple(int(v) for v in size) if size else None)

    def open_pipewire(self, sess):
        fd = self.sc.OpenPipeWireRemote(sess, {})
        # dbus-python 返回 dbus.types.UnixFd
        if hasattr(fd, "take"):
            fd = fd.take()
        elif hasattr(fd, "get"):
            fd = fd.get()          # 有些版本是 Gio.UnixFDList
        return int(fd)


def main():
    probe = "--probe" in sys.argv
    drop_old = "--drop-old" in sys.argv
    W = int(os.environ.get("W", "360"))
    H = int(os.environ.get("H", "760"))
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    loop = GLib.MainLoop()
    bus = dbus.SessionBus()
    p = Portal(bus, loop)
    log("== CreateSession ==")
    sess = p.create_session()
    log("   session =", sess)
    log("== SelectSources（手机上会弹授权框，请点『允许』）==")
    p.select_sources(sess)
    log("== Start ==")
    node, size = p.start(sess)
    log(f"   node={node} 原始分辨率={size}")
    fd = p.open_pipewire(sess)
    log(f"   pipewire fd={fd}  (类型: {os.readlink(f"/proc/self/fd/{fd}")[:40]})")
    hold = 0
    if "--hold" in sys.argv:
        i = sys.argv.index("--hold")
        hold = int(sys.argv[i+1]) if i+1 < len(sys.argv) else 30
    if hold:
        log(f"   保持会话 {hold} 秒（供探查）")
        import time
        time.sleep(hold)
        return
    if probe:
        return
    os.set_inheritable(fd, True)
    # pipewiresrc 的 target-object 按名字匹配更可靠：用 pw-dump 由 id 反查 node.name
    target = str(node)
    try:
        dump = json.loads(subprocess.run(["pw-dump"], capture_output=True, text=True).stdout or "[]")
        for o in dump:
            if o.get("type") == "PipeWire:Interface:Node" and str(o.get("id")) == str(node):
                nm = ((o.get("info") or {}).get("props") or {}).get("node.name")
                if nm:
                    target = str(nm)
                break
        log(f"   节点名 = {target}")
    except Exception as e:
        log("   pw-dump 反查失败，退回用 id:", e)
    caps = f"video/x-raw,format=I420,width={W},height={H}"
    # --drop-old：在 fdsink 前放一个 leaky 队列，积压时丢【旧】帧（保留最新），
    # 避免网络抖动时先卡住、再一次性快进。裸 I420 每帧自包含，丢帧安全。
    mid = "queue leaky=upstream max-size-buffers=2 max-size-bytes=0 max-size-time=0 ! " if drop_old else ""
    pipeline = (f"pipewiresrc fd={fd} target-object={target} min-buffers=2 max-buffers=2 ! videoconvert ! videoscale ! {caps} ! "
                f"{mid}fdsink fd=1")
    cmd = ["gst-launch-1.0", "-q"] + pipeline.split()
    log("== 启动 gst-launch-1.0 子进程（保持 portal 会话存活）==")
    proc = subprocess.Popen(cmd, pass_fds=(fd,), stdout=sys.stdout.buffer, stderr=None)
    try:
        rc = proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        rc = 130
    log("gst-launch 退出 rc=", rc)
    sys.exit(rc)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log("!! 失败：", e)
        sys.exit(1)
