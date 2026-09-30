/* portal_cast.c — 通过 xdg-desktop-portal 的 ScreenCast 拿到 PipeWire 抓屏流，
 * 用 GStreamer 做成**裸视频**（零编码）写到 stdout，配合 ssh 管道给本机 mpv 播放。
 *
 * 这是 portal_cast.py 的 C 重写版：同样走 portal/PipeWire/GStreamer，但用 libdbus 直连，
 * 不再需要 python3 / python3-dbus / PyGObject。
 *   实测（OnePlus 6 / postmarketOS，2026-09-30）：启动 0.24 s → 0.01 s，RSS 22.7 MB → 1.2 MB。
 *
 * 用法（在手机上跑）：
 *     portal_cast                    # 默认 360x760，I420，写 stdout
 *     W=540 H=1140 portal_cast       # 改输出尺寸
 *     portal_cast --drop-old         # fdsink 前加 leaky 队列（积压丢旧帧）
 *     portal_cast --probe            # 只走 portal 拿到 node/fd 后退出
 *     portal_cast --hold 30          # 拿到 fd 后保持会话 30 秒（供探查）
 *
 * 首次运行会在手机上弹授权框，点「允许/共享」；之后靠 restore_token 免弹窗。
 * 依赖：libdbus-1（运行）/ gst-launch-1.0 + gst-plugin-pipewire（运行）/ dbus 头文件（编译）
 * 编译：gcc -O2 -s -o portal_cast portal_cast.c $(pkg-config --cflags --libs dbus-1)
 */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void logmsg(const char *fmt, ...);

/* 取“消息里的 unix fd”：各发行版导出情况不一致 ——
 *   · 正常发行版：公开符号 dbus_message_get_unix_fd
 *   · Alpine/postmarketOS：只有私有 _dbus_message_get_unix_fd（带私有版本标签，
 *     直接链接要写死版本号 ⇒ 用 dlopen 在运行时解析，编译期零依赖、可回退）。
 * 两者都没有时返回 -1，由调用方报错。 */
static int msg_get_unix_fd(DBusMessage *m, unsigned int index) {
    typedef int (*fn_t)(DBusMessage *, unsigned int);
    static fn_t fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void *h = dlopen("libdbus-1.so.3", RTLD_LAZY);
        if (h) {
            fn = (fn_t)dlsym(h, "dbus_message_get_unix_fd");
            if (!fn) fn = (fn_t)dlsym(h, "_dbus_message_get_unix_fd");
        }
    }
    return fn ? fn(m, index) : -1;
}

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define IFACE_SC "org.freedesktop.portal.ScreenCast"
#define IFACE_REQ "org.freedesktop.portal.Request"

#define RESPONSE_TIMEOUT_SEC 300

static const char *token_file(void) {
    static char path[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sizeof path, "%s/.local/state/portal_cast.token", home);
    return path;
}

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ---------- 主循环：等某个 Request 路径上的 Response 信号 ---------- */

typedef struct {
    const char *want_path;
    int got;
    dbus_int32_t response;
    DBusMessage *reply;      /* 取到后 add_ref，交由调用方 unref */
} WaitCtx;

static DBusHandlerResult response_filter(DBusConnection *conn, DBusMessage *msg, void *user) {
    WaitCtx *ctx = user;
    DBusError err;

    if (!dbus_message_is_signal(msg, IFACE_REQ, "Response")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!dbus_message_has_path(msg, ctx->want_path)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    dbus_error_init(&err);
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
        logmsg("!! Response 信号格式不符合预期");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    dbus_message_iter_get_basic(&it, &ctx->response);
    ctx->reply = dbus_message_ref(msg);
    ctx->got = 1;
    (void)conn;
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* a{sv} 里按键取值（字符串） */
static int dict_get_string(DBusMessageIter *dict, const char *key, char *out, size_t outsz) {
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, var;
        const char *k = NULL;
        dbus_message_iter_recurse(&it, &ent);
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_STRING) break;
        dbus_message_iter_get_basic(&ent, &k);
        dbus_message_iter_next(&ent);
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_VARIANT) { dbus_message_iter_next(&it); continue; }
        dbus_message_iter_recurse(&ent, &var);
        int t = dbus_message_iter_get_arg_type(&var);
        if (k && strcmp(k, key) == 0) {
            const char *s = NULL;
            if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH || t == DBUS_TYPE_SIGNATURE) {
                dbus_message_iter_get_basic(&var, &s);
                if (s) { snprintf(out, outsz, "%s", s); return 1; }
            }
            return 0;
        }
        dbus_message_iter_next(&it);
    }
    return 0;
}

/* a{sv} 里取 streams = a(ua{sv}) 的第一个 node id；返回 1 表示取到 */
static int dict_get_first_stream_node(DBusMessageIter *dict, dbus_int32_t *node_out) {
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter ent, var, arr;
        const char *k = NULL;
        dbus_message_iter_recurse(&it, &ent);
        dbus_message_iter_get_basic(&ent, &k);
        dbus_message_iter_next(&ent);
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_VARIANT) { dbus_message_iter_next(&it); continue; }
        dbus_message_iter_recurse(&ent, &var);
        if (k && strcmp(k, "streams") == 0 && dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&var, &arr);
            if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
                DBusMessageIter st;
                dbus_message_iter_recurse(&arr, &st);
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_UINT32 || dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT32) {
                    dbus_int32_t v = 0;
                    uint32_t u = 0;
                    if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_UINT32) {
                        dbus_message_iter_get_basic(&st, &u);
                        v = (dbus_int32_t)u;
                    } else {
                        dbus_message_iter_get_basic(&st, &v);
                    }
                    *node_out = v;
                    return 1;
                }
            }
        }
        dbus_message_iter_next(&it);
    }
    return 0;
}

/* 等 Response；成功时把 results（a{sv}）迭代器初始化到 *out_results */
static int wait_response(DBusConnection *conn, const char *req_path, DBusMessage **out_msg) {
    WaitCtx ctx = { .want_path = req_path, .got = 0, .reply = NULL };
    char match[512];

    snprintf(match, sizeof match,
             "type='signal',interface='%s',member='Response',path='%s'", IFACE_REQ, req_path);
    dbus_bus_add_match(conn, match, NULL);
    dbus_connection_add_filter(conn, response_filter, &ctx, NULL);

    for (int waited = 0; !ctx.got && waited < RESPONSE_TIMEOUT_SEC * 10; waited++) {
        dbus_connection_read_write_dispatch(conn, 100);
    }
    dbus_connection_remove_filter(conn, response_filter, &ctx);
    dbus_bus_remove_match(conn, match, NULL);

    if (!ctx.got) {
        logmsg("!! 等待 %s 的 Response 超时（%d 秒内没在手机上点『允许』）", req_path, RESPONSE_TIMEOUT_SEC);
        return 0;
    }
    if (ctx.response != 0) {
        logmsg("!! portal 返回 response=%d（1=用户取消, 2=其它错误）", ctx.response);
        return 0;
    }
    *out_msg = ctx.reply;      /* 调用方负责 unref */
    return 1;
}

/* 往 parent 里塞一个 a{sv}（libdbus 的父子迭代器必须是不同变量，容易写错，故封装） */
typedef struct {
    const char *key;
    const char *sig;
    int type;
    const void *val;
} Opt;

static void append_opts(DBusMessageIter *parent, const Opt *opts, int n) {
    DBusMessageIter arr, ent, var;
    dbus_message_iter_open_container(parent, DBUS_TYPE_ARRAY, "{sv}", &arr);
    for (int i = 0; i < n; i++) {
        const char *k = opts[i].key;
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &ent);
        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &k);
        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, opts[i].sig, &var);
        dbus_message_iter_append_basic(&var, opts[i].type, opts[i].val);
        dbus_message_iter_close_container(&ent, &var);
        dbus_message_iter_close_container(&arr, &ent);
    }
    dbus_message_iter_close_container(parent, &arr);
}

/* ---------- portal 调用 ---------- */

static int call_portal(DBusConnection *conn, const char *method, DBusMessage *req, char *req_path_out, size_t pathsz) {
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, req, 60000, NULL);
    dbus_message_unref(req);
    if (!reply) return 0;
    const char *path = NULL;
    if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) {
        snprintf(req_path_out, pathsz, "%s", path);
    } else {
        logmsg("!! %s 没有返回 Request 路径", method);
    }
    dbus_message_unref(reply);
    return path != NULL;
}

int main(int argc, char **argv) {
    int probe = 0, drop_old = 0, hold = 0;
    int W = 360, H = 760;
    const char *e;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--drop-old")) drop_old = 1;
        else if (!strcmp(argv[i], "--hold")) hold = (i + 1 < argc) ? atoi(argv[++i]) : 30;
    }
    if ((e = getenv("W")) && atoi(e) > 0) W = atoi(e);
    if ((e = getenv("H")) && atoi(e) > 0) H = atoi(e);

    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        logmsg("!! 连接 session bus 失败：%s", err.message ? err.message : "?");
        return 1;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    char req_path[512];
    /* 1) CreateSession */
    logmsg("== CreateSession ==");
    DBusMessage *m = dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, IFACE_SC, "CreateSession");
    DBusMessageIter top;
    const char *tok = "tok1";
    {
        const char *tok_sess = "sess_tok1";
        Opt o[] = { { "handle_token", "s", DBUS_TYPE_STRING, &tok },
                    { "session_handle_token", "s", DBUS_TYPE_STRING, &tok_sess } };
        dbus_message_iter_init_append(m, &top);
        append_opts(&top, o, 2);
    }
    if (!call_portal(conn, "CreateSession", m, req_path, sizeof req_path)) return 1;

    char session[512] = "";
    static char saved[1024];      /* SelectSources 用的 restore_token，后面要和新的比较 */
    const char *sess_p = session;
    DBusMessage *resp = NULL;
    if (!wait_response(conn, req_path, &resp)) return 1;
    {
        DBusMessageIter r, res_it;
        if (dbus_message_iter_init(resp, &r) && dbus_message_iter_next(&r) &&
            dbus_message_iter_get_arg_type(&r) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&r, &res_it);
            dict_get_string(&res_it, "session_handle", session, sizeof session);
        }
        dbus_message_unref(resp);
    }
    if (!session[0]) { logmsg("!! 没拿到 session_handle"); return 1; }
    logmsg("   session = %s", session);

    /* 2) SelectSources（会弹授权框） */
    logmsg("== SelectSources（手机上会弹授权框，请点『允许』）==");
    m = dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, IFACE_SC, "SelectSources");
    {
        const char *tok2 = "tok2";
        uint32_t types = 1;        /* 1 = MONITOR（整屏） */
        dbus_bool_t multiple = FALSE;
        uint32_t cursor = 2;       /* 2 = 嵌入光标 */
        uint32_t persist = 2;      /* 2 = 持久授权 */
        Opt o[6] = {
            { "handle_token", "s", DBUS_TYPE_STRING, &tok2 },
            { "types", "u", DBUS_TYPE_UINT32, &types },
            { "multiple", "b", DBUS_TYPE_BOOLEAN, &multiple },
            { "cursor_mode", "u", DBUS_TYPE_UINT32, &cursor },
            { "persist_mode", "u", DBUS_TYPE_UINT32, &persist },
        };
        int n = 5;
        /* 有 restore_token 就带上（免二次弹窗） */
        FILE *tf = fopen(token_file(), "r");
        if (tf) {
            if (fgets(saved, sizeof saved, tf)) {
                char *nl = strchr(saved, '\n');
                if (nl) *nl = 0;
            }
            fclose(tf);
        }
        if (saved[0]) {
            char *sp = saved;      /* 注意：DBUS_TYPE_STRING 要的是「指向指针的指针」，不能直接传数组 */
            o[n++] = (Opt){ "restore_token", "s", DBUS_TYPE_STRING, &sp };
            logmsg("   使用已保存的 restore_token（应不再弹窗）");
        }
        dbus_message_iter_init_append(m, &top);
        dbus_message_iter_append_basic(&top, DBUS_TYPE_OBJECT_PATH, &sess_p);
        append_opts(&top, o, n);
    }
    if (!call_portal(conn, "SelectSources", m, req_path, sizeof req_path)) return 1;
    resp = NULL;
    if (!wait_response(conn, req_path, &resp)) return 1;
    dbus_message_unref(resp);

    /* 3) Start */
    logmsg("== Start ==");
    m = dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, IFACE_SC, "Start");
    {
        const char *tok3 = "tok3";
        const char *parent = "";
        Opt o[] = { { "handle_token", "s", DBUS_TYPE_STRING, &tok3 } };
        dbus_message_iter_init_append(m, &top);
        dbus_message_iter_append_basic(&top, DBUS_TYPE_OBJECT_PATH, &sess_p);
        dbus_message_iter_append_basic(&top, DBUS_TYPE_STRING, &parent);
        append_opts(&top, o, 1);
    }
    if (!call_portal(conn, "Start", m, req_path, sizeof req_path)) return 1;

    dbus_int32_t node = -1;
    char new_token[1024] = "";
    resp = NULL;
    if (!wait_response(conn, req_path, &resp)) return 1;
    {
        DBusMessageIter r, res_it;
        if (dbus_message_iter_init(resp, &r) && dbus_message_iter_next(&r) &&
            dbus_message_iter_get_arg_type(&r) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&r, &res_it);
            dict_get_first_stream_node(&res_it, &node);
            if (dbus_message_iter_init(resp, &r) && dbus_message_iter_next(&r) &&
                dbus_message_iter_get_arg_type(&r) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&r, &res_it);
                dict_get_string(&res_it, "restore_token", new_token, sizeof new_token);
            }
        }
        dbus_message_unref(resp);
    }
    if (node < 0) { logmsg("!! portal 没返回任何 stream"); return 1; }
    logmsg("   node=%d", (int)node);
    if (new_token[0] && strcmp(new_token, saved) != 0) {   /* saved 在 SelectSources 里填过 */
        char dir[512];
        snprintf(dir, sizeof dir, "%s/.local/state", getenv("HOME") ? getenv("HOME") : "/tmp");
        mkdir(dir, 0700);
        FILE *w = fopen(token_file(), "w");
        if (w) { fputs(new_token, w); fclose(w); logmsg("   已保存 restore_token"); }
    }

    /* 4) OpenPipeWireRemote → 拿 PipeWire fd */
    m = dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, IFACE_SC, "OpenPipeWireRemote");
    dbus_message_iter_init_append(m, &top);
    dbus_message_iter_append_basic(&top, DBUS_TYPE_OBJECT_PATH, &sess_p);
    append_opts(&top, NULL, 0);
    DBusMessage *fd_reply = dbus_connection_send_with_reply_and_block(conn, m, 60000, &err);
    dbus_message_unref(m);
    if (!fd_reply) {
        logmsg("!! OpenPipeWireRemote 失败：%s", err.message ? err.message : "?");
        return 1;
    }
    int pfd = -1;
    {
        DBusMessageIter r;
        if (dbus_message_iter_init(fd_reply, &r) && dbus_message_iter_get_arg_type(&r) == DBUS_TYPE_UNIX_FD) {
            int val = -1;
            dbus_message_iter_get_basic(&r, &val);
            /* 两种语义都兼容：
             *   · 老 libdbus（<1.16）：值是“索引”，要用 dbus_message_get_unix_fd 换成 fd
             *   · 新 libdbus（≥1.16，已删掉该公开 API）：值**本身就是 fd**
             * 用 fcntl 试一下就知道 —— 索引一般不会恰好是个有效的 fd。 */
            pfd = msg_get_unix_fd(fd_reply, (unsigned)(val < 0 ? 0 : val));
            if (pfd < 0 && val >= 0 && fcntl(val, F_GETFD) != -1) {
                pfd = val;
                logmsg("   （libdbus 未提供索引→fd API，直接采用消息中的 fd 值 %d）", val);
            }
        }
        dbus_message_unref(fd_reply);
    }
    if (pfd < 0) { logmsg("!! 没拿到 PipeWire fd"); return 1; }
    logmsg("   pipewire fd=%d", pfd);
    fcntl(pfd, F_SETFD, 0);          /* 必须可继承：gst-launch 要用它 */

    if (hold) {
        logmsg("   保持会话 %d 秒（供探查）", hold);
        sleep(hold);
        return 0;
    }
    if (probe) return 0;

    /* 5) 用 pw-dump 按 id 反查 node.name（pipewiresrc 用名字匹配更可靠） */
    char target[256];
    snprintf(target, sizeof target, "%d", (int)node);
    {
        char cmd[128];
        snprintf(cmd, sizeof cmd, "pw-dump %d 2>/dev/null", (int)node);
        FILE *p = popen(cmd, "r");
        if (p) {
            char buf[65536];
            size_t n = fread(buf, 1, sizeof buf - 1, p);
            buf[n] = 0;
            pclose(p);
            char *k = strstr(buf, "\"node.name\"");
            if (k) {
                char *q = strchr(k, ':');
                char *s1 = q ? strchr(q, '"') : NULL;
                char *s2 = s1 ? strchr(s1 + 1, '"') : NULL;
                if (s2 && s2 - s1 - 1 < (long)sizeof target) {
                    memcpy(target, s1 + 1, (size_t)(s2 - s1 - 1));
                    target[s2 - s1 - 1] = 0;
                }
            }
            logmsg("   节点名 = %s", target);
        }
    }

    /* 6) 启动 gst-launch-1.0（保持 portal 会话存活：本进程必须活着） */
    char fds[32], tgt[300], caps[128], queue[256] = "";
    char minb[32] = "min-buffers=2", maxb[32] = "max-buffers=2";
    snprintf(fds, sizeof fds, "fd=%d", pfd);            /* GStreamer 要 fd=<n> 这种单 token */
    snprintf(tgt, sizeof tgt, "target-object=%s", target);
    snprintf(caps, sizeof caps, "video/x-raw,format=I420,width=%d,height=%d", W, H);
    if (drop_old)
        snprintf(queue, sizeof queue,
                 "queue leaky=upstream max-size-buffers=2 max-size-bytes=0 max-size-time=0");
    logmsg("== 启动 gst-launch-1.0 子进程（保持 portal 会话存活）==");

    pid_t pid = fork();
    if (pid < 0) { logmsg("!! fork 失败：%s", strerror(errno)); return 1; }
    if (pid == 0) {
        char *gargv[32];
        int n = 0;
        gargv[n++] = (char *)"gst-launch-1.0";
        gargv[n++] = (char *)"-q";
        gargv[n++] = (char *)"pipewiresrc";
        gargv[n++] = fds;                    /* fd=<n> */
        gargv[n++] = tgt;                    /* target-object=<name> */
        gargv[n++] = minb;                   /* min-buffers=2 */
        gargv[n++] = maxb;                   /* max-buffers=2 */
        gargv[n++] = (char *)"!";
        gargv[n++] = (char *)"videoconvert";
        gargv[n++] = (char *)"!";
        gargv[n++] = (char *)"videoscale";
        gargv[n++] = (char *)"!";
        gargv[n++] = (char *)caps;
        gargv[n++] = (char *)"!";
        if (queue[0]) {
            char *tok, *save = NULL;
            for (tok = strtok_r(queue, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
                gargv[n++] = tok;
            gargv[n++] = (char *)"!";
        }
        gargv[n++] = (char *)"fdsink";
        gargv[n++] = (char *)"fd=1";
        gargv[n] = NULL;
        execvp(gargv[0], gargv);
        fprintf(stderr, "!! exec gst-launch-1.0 失败：%s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    logmsg("gst-launch 退出 rc=%d", rc);
    return rc;
}
