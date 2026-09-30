/* mirror-screen-c — 投屏运行时（监管）引擎
 *
 * 与 mirror-screen.py 共用同一份配置 ~/.config/mirror-screen/config.json，
 * 负责真正跑管线：远端取流 → ssh 管道 → 本机 mpv，并管日志、预检、收尾。
 *
 * 为什么单独做 C 版：投屏期间这个监管进程要一直活着，实测 Python 版常驻
 *   27.9 MB RSS / 启动 77 ms；C 版 1.2 MB / 1 ms。配置界面（TUI）仍用 Python，
 *   只有这条热路径换 C —— 功能与行为保持一致（--dry-run 输出逐字对齐）。
 *
 * 用法：
 *   mirror-screen-c --run            # 用保存的配置投屏（TUI/快捷键会 exec 它）
 *   mirror-screen-c --dry-run        # 只打印将要执行的命令
 *   mirror-screen-c --print-config   # 打印解析出的配置
 *   mirror-screen-c --wake           # 只唤醒远端屏幕
 * 编译：gcc -O2 -s -o mirror-screen-c mirror-screen-c.c
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP "mirror-screen"
#define MAXARGS 64

static char g_home[512], g_cfg[600], g_log[600], g_cache[600], g_state[600];

/* ---------------- 配置：够用的扁平 JSON 读取 ---------------- */

static char *g_json;          /* 整个配置文件 */

static char *read_whole(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    if (len) *len = (long)got;
    return buf;
}

/* 把 "..." 里的转义解出来，写进 out（含 \uXXXX → UTF-8） */
static void unescape(const char **p, char *out, size_t outsz) {
    size_t o = 0;
    const char *s = *p;
    while (*s && *s != '"' && o + 1 < outsz) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) {
                case 'n': out[o++] = '\n'; break;
                case 't': out[o++] = '\t'; break;
                case 'r': out[o++] = '\r'; break;
                case 'b': out[o++] = '\b'; break;
                case 'f': out[o++] = '\f'; break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)s[1 + i]); i++) {
                        char c = s[1 + i];
                        cp = cp * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower(c) - 'a' + 10));
                    }
                    s += 4;
                    if (cp < 0x80) out[o++] = (char)cp;
                    else if (cp < 0x800) {
                        if (o + 2 > outsz) break;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 3 > outsz) break;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out[o++] = *s; break;
            }
            s++;
        } else {
            out[o++] = *s++;
        }
    }
    *p = s;
    out[o] = 0;
}

/* 取 "key": <值>；字符串走 unescape，其它（true/false/数字）原样拷 */
static int cfg_raw(const char *key, char *out, size_t outsz, int *is_string) {
    if (!g_json) return 0;
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = g_json;
    while ((p = strstr(p, pat))) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') { p += strlen(pat); continue; }        /* 不是键（可能是值里的同名字符串） */
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '"') {
            q++;
            unescape(&q, out, outsz);
            if (is_string) *is_string = 1;
            return 1;
        }
        size_t o = 0;
        while (*q && *q != ',' && *q != '}' && *q != '\n' && o + 1 < outsz) out[o++] = *q++;
        while (o && (out[o - 1] == ' ' || out[o - 1] == '\r')) o--;
        out[o] = 0;
        if (is_string) *is_string = 0;
        return 1;
    }
    return 0;
}

static void cfg_str(const char *key, const char *dflt, char *out, size_t outsz) {
    if (!cfg_raw(key, out, outsz, NULL)) snprintf(out, outsz, "%s", dflt);
}

static int cfg_bool(const char *key, int dflt) {
    char v[32];
    if (!cfg_raw(key, v, sizeof v, NULL)) return dflt;
    return strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

static int cfg_int(const char *key, int dflt) {
    char v[32];
    if (!cfg_raw(key, v, sizeof v, NULL) || !v[0]) return dflt;
    int n = atoi(v);
    return n > 0 ? n : dflt;
}

/* ---------------- 小工具 ---------------- */

static char *xstrdup(const char *s) { char *p = strdup(s ? s : ""); return p ? p : (char *)""; }

/* Python shlex.quote 的等价实现（预览输出要对齐） */
static char *shq(const char *s) {
    static char buf[8192];
    size_t need = 2;
    for (const char *p = s; *p; p++) need += (*p == '\'') ? 5 : 1;
    if (need >= sizeof buf) return xstrdup(s);
    int plain = 1;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || strchr("_@%+=:,./-", *p))) { plain = 0; break; }
    if (plain && *s) { snprintf(buf, sizeof buf, "%s", s); return buf; }
    char *o = buf;
    *o++ = '\'';
    for (const char *p = s; *p; p++) {
        if (*p == '\'') { memcpy(o, "'\"'\"'", 5); o += 5; } else *o++ = *p;
    }
    *o++ = '\'';
    *o = 0;
    return buf;
}

static void append_argv(char **argv, int *n, const char *s) {
    if (*n < MAXARGS - 1) argv[(*n)++] = xstrdup(s);
    argv[*n] = NULL;
}

static void append_argvf(char **argv, int *n, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    append_argv(argv, n, buf);
}

static int run_capture(char *const argv[], char *out, size_t outsz, int timeout_s) {
    int pfd[2];
    if (pipe(pfd) < 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t o = 0;
    ssize_t r;
    while ((r = read(pfd[0], out + o, outsz - 1 - o)) > 0 && o + (size_t)r < outsz - 1) o += (size_t)r;
    if (r > 0) o += (size_t)r;
    out[o] = 0;
    close(pfd[0]);
    int st = 0;
    for (int i = 0; i < timeout_s * 20; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return -1;
}

static int run_silent(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

static void notify(const char *title, const char *body) {
    char *argv[5] = { (char *)"notify-send", (char *)title, (char *)body, NULL, NULL };
    if (body && *body) run_silent(argv);
    else { argv[2] = NULL; run_silent(argv); }
}

static void ts_prefix(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &tm);
}

/* ---------------- 管线构建（与 mirror-screen.py 对齐） ---------------- */

typedef struct {
    char mode[128], host[256], user[128], port[16], output[64], scale[32], fps[16], codec[32],
         muxer[32], encode[128], hwdec[32], display[64], mpv_extra[1024];
    int lowlat, quiet, wake, fullscreen;
} Cfg;

static void cfg_load(Cfg *c) {
    cfg_str("mode", "", c->mode, sizeof c->mode);
    cfg_str("host", "", c->host, sizeof c->host);
    cfg_str("user", "", c->user, sizeof c->user);
    cfg_str("port", "22", c->port, sizeof c->port);
    cfg_str("output", "", c->output, sizeof c->output);
    cfg_str("scale", "360", c->scale, sizeof c->scale);
    cfg_str("fps", "15", c->fps, sizeof c->fps);
    cfg_str("codec", "libx264", c->codec, sizeof c->codec);
    cfg_str("muxer", "h264", c->muxer, sizeof c->muxer);
    cfg_str("encode", "极速（手机 CPU 最低）", c->encode, sizeof c->encode);
    cfg_str("hwdec", "vaapi", c->hwdec, sizeof c->hwdec);
    cfg_str("display", "半幅窗口", c->display, sizeof c->display);
    cfg_str("mpv_extra", "", c->mpv_extra, sizeof c->mpv_extra);
    c->lowlat = cfg_bool("lowlat", 1);
    c->quiet = cfg_bool("quiet", 1);
    c->wake = cfg_bool("wake", 1);
    c->fullscreen = cfg_bool("fullscreen", 0);
}

static int is_portal(const Cfg *c) { return strstr(c->mode, "portal") != NULL; }
static int is_stream(const Cfg *c) { return strstr(c->mode, "wf-recorder") != NULL; }

static void ssh_target(const Cfg *c, char *out, size_t n) {
    if (c->user[0]) snprintf(out, n, "%s@%s", c->user, c->host);
    else snprintf(out, n, "%s", c->host);
}

static void ssh_argv(const Cfg *c, char **argv, int *n) {
    char ctrl[700];
    snprintf(ctrl, sizeof ctrl, "%s/cm-%%r@%%h-%%p", g_cache);
    append_argv(argv, n, "ssh");
    append_argv(argv, n, "-o"); append_argv(argv, n, "BatchMode=yes");
    append_argv(argv, n, "-o"); append_argv(argv, n, "ServerAliveInterval=15");
    append_argv(argv, n, "-o"); append_argv(argv, n, "Compression=no");
    append_argv(argv, n, "-o"); append_argv(argv, n, "ControlMaster=auto");
    append_argv(argv, n, "-o"); append_argvf(argv, n, "ControlPath=%s", ctrl);
    append_argv(argv, n, "-o"); append_argv(argv, n, "ControlPersist=300");
    if (c->port[0] && strcmp(c->port, "22") != 0) { append_argv(argv, n, "-p"); append_argv(argv, n, c->port); }
    char tgt[400];
    ssh_target(c, tgt, sizeof tgt);
    append_argv(argv, n, tgt);
}

/* 远端 portal 客户端：优先 C（启动 0.07s/RSS 1.2MB），否则 python 版 */
static const char *portal_client(void) {
    char p[700];
    snprintf(p, sizeof p, "%s/remote-client", g_cache);
    FILE *f = fopen(p, "r");
    if (f) {
        char v[64] = "";
        if (fgets(v, sizeof v, f)) {
            char *nl = strchr(v, '\n');
            if (nl) *nl = 0;
            fclose(f);
            if (strcmp(v, "c") == 0) return "/tmp/portal_cast";
        } else fclose(f);
    }
    return "python3 /tmp/portal_cast.py";
}

/* 需要的输出尺寸（portal_styled） */
static void out_size(const Cfg *c, int *w, int *h) {
    int s = atoi(c->scale);
    if (s <= 0) s = 360;
    *w = s;
    *h = (int)((double)s * 760.0 / 360.0 + 0.5);
}

/* 显示方式 → mpv 尺寸参数（半幅窗口不传；全屏 --fs；贴合视频用 niri 工作区算） */
static void display_args(const Cfg *c, int *dargc, char **dargv) {
    if (strcmp(c->display, "全屏") == 0) { dargv[(*dargc)++] = xstrdup("--fs"); return; }
    if (strcmp(c->display, "贴合视频") != 0) return;      /* 半幅窗口：尺寸交给 niri 规则，不传参数 */
    char *qv[5] = { (char *)"niri", (char *)"msg", (char *)"--json", (char *)"outputs", NULL };
    char out[16384] = "";
    run_capture(qv, out, sizeof out, 4);
    double height = 1028.0, scale = 1.0;
    const char *hh = strstr(out, "\"logical\"");
    if (hh) {
        const char *hp = strstr(hh, "\"height\"");
        const char *sp = strstr(out, "\"scale\"");
        if (hp) height = atof(strchr(hp, ':') + 1);
        if (sp) scale = atof(strchr(sp, ':') + 1);
        if (scale <= 0) scale = 1.0;
    }
    int fw, fh;
    out_size(c, &fw, &fh);                                 /* 视频帧高（portal/stream 都用这个） */
    /* 与 Python 的 autofit_arg 对齐：先按逻辑像素留 8px 余量，再乘缩放换成物理高度，最后取 2 位小数 */
    double h_log = height - 8.0;
    if (h_log < 300.0) h_log = 300.0;
    double ws = (h_log * scale) / (double)(fh > 0 ? fh : 760);
    ws = ((double)((long)(ws * 100.0 + 0.5))) / 100.0;      /* round(x, 2) */
    if (ws < 1.0) ws = 1.0;
    char buf[64];
    snprintf(buf, sizeof buf, "--window-scale=%.2f", ws);
    dargv[(*dargc)++] = xstrdup(buf);
}

/* 远端命令（字符串，交给 ssh 执行） */
static void remote_command(const Cfg *c, char *out, size_t n) {
    if (is_portal(c)) {
        int w, h;
        out_size(c, &w, &h);
        snprintf(out, n, "W=%d H=%d %s", w, h, portal_client());
        if (c->lowlat) snprintf(out + strlen(out), n - strlen(out), " --drop-old");
        return;
    }
    /* wf-recorder 流 */
    /* 与 Python 的 ENCODE_PROFILES 一字不差 */
    char prof[128] = "preset=superfast crf=20";
    if (strstr(c->encode, "极速")) snprintf(prof, sizeof prof, "preset=ultrafast crf=20");
    else if (strstr(c->encode, "均衡")) snprintf(prof, sizeof prof, "preset=superfast crf=20");
    else if (strstr(c->encode, "省带宽")) snprintf(prof, sizeof prof, "preset=medium crf=24");
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "wf-recorder -y -o %s -c %s -x yuv420p -r %s", c->output, c->codec, c->fps);
    char *save = NULL, *tok = strtok_r(prof, " ", &save);
    while (tok) { snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " -p %s", tok); tok = strtok_r(NULL, " ", &save); }
    snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " -p tune=zerolatency");
    if (strstr(c->codec, "x264"))
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd),
                 " -p x264-params=sliced-threads=1:rc-lookahead=0:sync-lookahead=0:bframes=0");
    if (c->scale[0])
        snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " -F \"scale=%s:-1\"", c->scale);
    snprintf(cmd + strlen(cmd), sizeof cmd - strlen(cmd), " -m %s -f /dev/stdout", c->muxer);
    snprintf(out, n, "%s", cmd);
}

/* mpv 命令行 */
static void player_argv(const Cfg *c, char **argv, int *n, const char *title) {
    append_argv(argv, n, "mpv");
    if (is_portal(c)) {
        int w, h;
        out_size(c, &w, &h);
        append_argv(argv, n, "--no-config");
        append_argv(argv, n, "--hwdec=no");
        append_argv(argv, n, "--demuxer-lavf-format=rawvideo");
        append_argvf(argv, n, "--demuxer-lavf-o=video_size=%dx%d,pixel_format=yuv420p", w, h);
        if (c->lowlat) {
            append_argv(argv, n, "--cache=no");
            append_argv(argv, n, "--demuxer-readahead-secs=0");
            append_argv(argv, n, "--demuxer-max-bytes=8MiB");
            append_argv(argv, n, "--untimed");
            append_argv(argv, n, "--video-sync=desync");
        }
        append_argv(argv, n, "--force-window=immediate");
        char appid[128];
        snprintf(appid, sizeof appid, "--wayland-app-id=%s",
                 strcmp(c->display, "半幅窗口") == 0 ? "portal-cast" : "portal-cast-fitted");
        append_argv(argv, n, appid);
        char *dargv[8];
        int dargc = 0;
        display_args(c, &dargc, dargv);
        for (int i = 0; i < dargc; i++) append_argv(argv, n, dargv[i]);
        char ipc[700];
        snprintf(ipc, sizeof ipc, "--input-ipc-server=%s/portal-cast.sock", g_cache);
        append_argv(argv, n, ipc);
        append_argvf(argv, n, "--title=%s", title);
    } else {
        append_argv(argv, n, "--profile=low-latency");
        append_argv(argv, n, "--no-config");
        append_argvf(argv, n, "--hwdec=%s", c->hwdec[0] ? c->hwdec : "vaapi");
        append_argvf(argv, n, "--demuxer-lavf-format=%s", c->muxer);
        append_argv(argv, n, "--force-window=immediate");
        char appid2[128];
        snprintf(appid2, sizeof appid2, "--wayland-app-id=%s",
                 strcmp(c->display, "半幅窗口") == 0 ? "portal-cast" : "portal-cast-fitted");
        append_argv(argv, n, appid2);
        append_argvf(argv, n, "--title=%s", title);
        if (c->lowlat) {
            append_argv(argv, n, "--demuxer-readahead-secs=0");
            append_argv(argv, n, "--demuxer-max-bytes=8MiB");
            append_argv(argv, n, "--untimed");
            append_argv(argv, n, "--video-sync=desync");
        }
        char *dargv[8];
        int dargc = 0;
        display_args(c, &dargc, dargv);
        for (int i = 0; i < dargc; i++) append_argv(argv, n, dargv[i]);
        append_argv(argv, n, "--keep-open=yes");       /* stream 分支 Python 有这个参数 */
    }
    if (c->quiet) { append_argv(argv, n, "--really-quiet"); append_argv(argv, n, "--no-terminal"); }
    if (c->mpv_extra[0]) {
        char extra[1024];
        snprintf(extra, sizeof extra, "%s", c->mpv_extra);
        char *save = NULL, *tok = strtok_r(extra, " ", &save);
        while (tok) { append_argv(argv, n, tok); tok = strtok_r(NULL, " ", &save); }
    }
    append_argv(argv, n, "-");
}

static void print_dry_run(const Cfg *c) {
    char rcmd[4096], tgt[400], title[500];
    remote_command(c, rcmd, sizeof rcmd);
    ssh_target(c, tgt, sizeof tgt);
    snprintf(title, sizeof title, "%s%s", is_portal(c) ? "PORTAL-CAST:" : "MIRROR:", tgt);
    char *sargv[MAXARGS];
    int sn = 0;
    ssh_argv(c, sargv, &sn);
    append_argv(sargv, &sn, rcmd);
    char *pargv[MAXARGS];
    int pn = 0;
    player_argv(c, pargv, &pn, title);
    for (int i = 0; i < sn; i++) printf("%s%s", i ? " " : "", shq(sargv[i]));
    printf(" | ");
    for (int i = 0; i < pn; i++) printf("%s%s", i ? " " : "", shq(pargv[i]));
    printf("\n");
}

/* ---------------- 运行 ---------------- */

static int log_fd = -1;

static void logline(const char *fmt, ...) {
    if (log_fd < 0) return;
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t w = write(log_fd, buf, (size_t)n);
    (void)w;
}

static int stop_previous_cast(void) {
    char pf[700];
    snprintf(pf, sizeof pf, "%s/cast.pid", g_state);
    FILE *f = fopen(pf, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid > 1 && kill(pid, 0) == 0) {
        kill(pid, SIGTERM);
        for (int i = 0; i < 30; i++) { if (kill(pid, 0) != 0) break; usleep(100000); }
        kill(pid, SIGKILL);
        return 1;
    }
    return 0;
}

static void sync_remote_client(const Cfg *c);      /* 定义在后面（TUI 段之前） */

static int run_cast(const Cfg *c) {
    char title[500], tgt[400];
    ssh_target(c, tgt, sizeof tgt);
    snprintf(title, sizeof title, "PORTAL-CAST:%s", tgt);

    log_fd = open(g_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) { fprintf(stderr, "无法写日志 %s\n", g_log); return 2; }
    if (is_portal(c)) sync_remote_client(c);        /* 远端 portal 客户端（含 C 版编译/回退） */

    /* 记一笔头（与 Python 版同格式） */
    char ts[64];
    ts_prefix(ts, sizeof ts);
    dprintf(log_fd, "\n===== %s ", ts);
    dprintf(log_fd, "%s", tgt);        /* 简化：目标主机 */
    dprintf(log_fd, "\n");

    char *sargv[MAXARGS];
    int sn = 0;
    ssh_argv(c, sargv, &sn);
    int base = sn;
    char rcmd[4096];
    remote_command(c, rcmd, sizeof rcmd);

    /* 预检：亮屏 + 清残留（一条 ssh，15s 超时） */
    if (c->wake || 1) {
        char pre[1024] = "";
        if (c->wake && c->output[0]) snprintf(pre, sizeof pre, "wlr-randr --output %s --on ; ", c->output);
        strncat(pre, "pkill -9 -x wf-recorder ; true", sizeof pre - strlen(pre) - 1);
        char *pargv[MAXARGS];
        int pn = 0;
        for (int i = 0; i < base; i++) pargv[pn++] = sargv[i];
        pargv[pn] = NULL;
        append_argv(pargv, &pn, pre);
        int prc = run_silent(pargv);
        logline("[preflight] 亮屏+清残留（单次连接）rc=%d", prc);
    }

    /* ssh | mpv */
    char *ssh_argv_full[MAXARGS];
    int an = 0;
    for (int i = 0; i < base; i++) ssh_argv_full[an++] = sargv[i];
    append_argv(ssh_argv_full, &an, rcmd);

    char *pargv[MAXARGS];
    int pn = 0;
    player_argv(c, pargv, &pn, title);

    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return 2; }
    pid_t pssh = fork();
    if (pssh == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(ssh_argv_full[0], ssh_argv_full);
        _exit(127);
    }
    pid_t pmpv = fork();
    if (pmpv == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(pargv[0], pargv);
        _exit(127);
    }
    close(pipefd[0]); close(pipefd[1]);

    /* 自己记 pid，供下次 stop_previous_cast */
    char pf[700];
    snprintf(pf, sizeof pf, "%s/cast.pid", g_state);
    FILE *f = fopen(pf, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }

    time_t t0 = time(NULL);
    int st = 0;
    if (pmpv > 0) waitpid(pmpv, &st, 0);
    int ran = (int)(time(NULL) - t0);
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    kill(pssh, SIGTERM);
    waitpid(pssh, &st, 0);

    if (ran < 5) {
        logline("[warn] 投屏只维持了 %ds 就结束（rc=%d）", ran, rc);
        notify("投屏结束得太快，可能远端出错了", "详见 ~/.local/state/mirror-screen/last-run.log");
    }
    /* 兑底清理远端 */
    char *cargv[MAXARGS];
    int cn = 0;
    for (int i = 0; i < base; i++) cargv[cn++] = sargv[i];
    append_argv(cargv, &cn, "pkill -9 -x wf-recorder ; pgrep -x wf-recorder >/dev/null && echo STILL-ALIVE || true");
    int crc = run_silent(cargv);
    logline("[cleanup] 远端 pkill rc=%d", crc);

    unlink(pf);
    close(log_fd);
    return rc;
}

static void print_config(const Cfg *c) {
    printf("mode=%s\nhost=%s\nuser=%s\nport=%s\noutput=%s\nscale=%s\nfps=%s\ncodec=%s\nmuxer=%s\n"
           "display=%s\nlowlat=%d\nquiet=%d\nwake=%d\nportal_client=%s\n",
           c->mode, c->host, c->user, c->port, c->output, c->scale, c->fps, c->codec, c->muxer,
           c->display, c->lowlat, c->quiet, c->wake, portal_client());
}

/* ---------------- 远端客户端同步（C 版自己做，不依赖 Python） ---------------- */

static void sha256_of(const char *path, char *out, size_t outsz) {
    out[0] = 0;
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "sha256sum %s 2>/dev/null | cut -c1-16", path);
    FILE *f = popen(cmd, "r");
    if (!f) return;
    if (fgets(out, (int)outsz, f)) {
        char *nl = strchr(out, '\n');
        if (nl) *nl = 0;
    }
    pclose(f);
}

/* 本机脚本位置：先看二进制旁边，再看工作区 */
static int find_local(const char *name, char *out, size_t outsz) {
    char exe[512] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            snprintf(out, outsz, "%s/%s", exe, name);
            if (access(out, R_OK) == 0) return 1;
        }
    }
    snprintf(out, outsz, "%s/AIworks/02-本机运维/%s", g_home, name);
    return access(out, R_OK) == 0;
}

static int push_file(const Cfg *c, const char *name, int base, char **sargv, int timeout_s) {
    char local[700], rsha[64], lsha[64];
    if (!find_local(name, local, sizeof local)) return 0;
    sha256_of(local, lsha, sizeof lsha);
    char *qargv[MAXARGS];
    int qn = 0;
    for (int i = 0; i < base; i++) qargv[qn++] = sargv[i];
    char q[512];
    snprintf(q, sizeof q, "sha256sum /tmp/%s 2>/dev/null | cut -c1-16", name);
    append_argv(qargv, &qn, q);
    char out[256] = "";
    run_capture(qargv, out, sizeof out, timeout_s);
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    snprintf(rsha, sizeof rsha, "%s", out);
    if (lsha[0] && strcmp(lsha, rsha) == 0) return 1;            /* 远端已是最新 */
    char tgt[400];
    ssh_target(c, tgt, sizeof tgt);
    char *scpargv[8] = { (char *)"scp", (char *)"-q", NULL, NULL, NULL, NULL, NULL, NULL };
    int si = 2;
    /* 复用 ssh 选项（去掉开头的 ssh 与结尾的 target） */
    for (int i = 1; i < base - 1 && si < 6; i++) scpargv[si++] = sargv[i];
    char dst[600];
    snprintf(dst, sizeof dst, "%s:/tmp/%s", tgt, name);
    scpargv[si++] = local;
    scpargv[si++] = dst;
    return run_silent(scpargv) == 0;
}

/* 同步远端 portal 客户端：python 版总同步；auto/c 时再传 .c 并在远端编译，失败即回退 */
static void sync_remote_client(const Cfg *c) {
    char mode[64] = "auto";
    cfg_str("remote_client", "auto", mode, sizeof mode);
    char *sargv[MAXARGS];
    int sn = 0;
    ssh_argv(c, sargv, &sn);
    int base = sn;
    int ok_py = push_file(c, "portal_cast.py", base, sargv, 20);
    char cache[700];
    snprintf(cache, sizeof cache, "%s/remote-client", g_cache);
    if (!ok_py || !strcmp(mode, "python")) {
        FILE *f = fopen(cache, "w");
        if (f) { fputs("python", f); fclose(f); }
        return;
    }
    if (!strcmp(mode, "c")) {                                       /* 强制 C：不同步 .c 也会用它 */
        FILE *f = fopen(cache, "w");
        if (f) { fputs("c", f); fclose(f); }
        return;
    }
    if (!push_file(c, "portal_cast.c", base, sargv, 30)) {
        FILE *f = fopen(cache, "w");
        if (f) { fputs("python", f); fclose(f); }
        return;
    }
    char *bargv[MAXARGS];
    int bn = 0;
    for (int i = 0; i < base; i++) bargv[bn++] = sargv[i];
    append_argv(bargv, &bn,
        "cd /tmp && if [ ! -x portal_cast ] || [ portal_cast.c -nt portal_cast ]; then "
        "gcc -O2 -s -o portal_cast portal_cast.c $(pkg-config --cflags --libs dbus-1 2>/dev/null) -ldl 2>&1; fi; "
        "[ -x portal_cast ] && echo BUILD-OK || echo BUILD-FAIL");
    char out[4096] = "";
    run_capture(bargv, out, sizeof out, 120);
    int ok = strstr(out, "BUILD-OK") != NULL;
    FILE *f = fopen(cache, "w");
    if (f) { fputs(ok ? "c" : "python", f); fclose(f); }
    if (log_fd >= 0)
        logline("[portal] 远端 %s 客户端就绪", ok ? "C（启动 ~0.07s / RSS ~1.2MB）" : "Python（C 编译不成功，已回退）");
}

/* ==================================================================
 * TUI（ncurses）—— 与 mirror-screen.py 的配置界面对齐
 * 常量（模式/档位/远端脚本/默认值）从 Python 自动导出到 ms_consts.h，
 * 生成命令见 install-mirror-screen.sh，避免两边手抄不一致。
 * ================================================================== */
#include <locale.h>
#include <ncurses.h>
#include "ms_consts.h"

#define MS_MAXKV 64

static char g_k[MS_MAXKV][64];
static char g_v[MS_MAXKV][1024];
static int  g_isstr[MS_MAXKV];
static int  g_nkv;

static int kv_idx(const char *k) {
    for (int i = 0; i < g_nkv; i++) if (strcmp(g_k[i], k) == 0) return i;
    return -1;
}
static const char *kv_get(const char *k) {
    int i = kv_idx(k);
    return i >= 0 ? g_v[i] : "";
}
static void kv_set(const char *k, const char *v) {
    int i = kv_idx(k);
    if (i < 0) {
        if (g_nkv >= MS_MAXKV) return;
        i = g_nkv++;
        snprintf(g_k[i], sizeof g_k[i], "%s", k);
        g_isstr[i] = 1;
    }
    snprintf(g_v[i], sizeof g_v[i], "%s", v);
}
static int kv_bool(const char *k, int dflt) {
    int i = kv_idx(k);
    if (i < 0 || !g_v[i][0]) return dflt;
    return strcmp(g_v[i], "true") == 0 || strcmp(g_v[i], "1") == 0;
}
static void kv_toggle(const char *k) { kv_set(k, kv_bool(k, 0) ? "false" : "true"); }

/* 从 ms_consts.h 的默认值表 + 配置文件填充键值表 */
static void kv_load(void) {
    g_nkv = 0;
    for (int i = 0; MS_DEFAULT_KEYS[i]; i++) {
        snprintf(g_k[i], sizeof g_k[i], "%s", MS_DEFAULT_KEYS[i]);
        snprintf(g_v[i], sizeof g_v[i], "%s", MS_DEFAULT_STR[i]);
        g_isstr[i] = MS_DEFAULT_STRS[i];
        g_nkv = i + 1;
    }
    for (int i = 0; i < g_nkv; i++) {                      /* 文件里有就用文件里的 */
        char tmp[1024];
        int isstr = 0;
        if (cfg_raw(g_k[i], tmp, sizeof tmp, &isstr)) {
            snprintf(g_v[i], sizeof g_v[i], "%s", tmp);
            g_isstr[i] = isstr;
        }
    }
}

/* JSON 转义（ensure_ascii=False：UTF-8 原样写） */
static void json_escape(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && o + 8 < n; p++) {
        switch (*p) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            default:
                if (*p < 0x20) { o += (size_t)snprintf(out + o, n - o, "\\u%04x", *p); }
                else out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

static const char *json_bool_str(const char *k, int dflt) { return kv_bool(k, dflt) ? "true" : "false"; }

static void kv_save(void) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_cfg);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\n");
    for (int i = 0; i < g_nkv; i++) {
        char esc[2048];
        json_escape(g_v[i], esc, sizeof esc);
        if (strcmp(g_k[i], "lowlat") == 0 || strcmp(g_k[i], "quiet") == 0 ||
            strcmp(g_k[i], "exit_on_close") == 0 || strcmp(g_k[i], "close_tui") == 0 ||
            strcmp(g_k[i], "wake") == 0 || strcmp(g_k[i], "nogpu") == 0 ||
            strcmp(g_k[i], "fullscreen") == 0 || strcmp(g_k[i], "keep_size") == 0)
            fprintf(f, "  \"%s\": %s%s\n", g_k[i], json_bool_str(g_k[i], 0), i + 1 < g_nkv ? "," : "");
        else
            fprintf(f, "  \"%s\": \"%s\"%s\n", g_k[i], esc, i + 1 < g_nkv ? "," : "");
    }
    fprintf(f, "}\n");
    fclose(f);
    rename(tmp, g_cfg);
}

/* 从键值表装出引擎用的 Cfg */
static void cfg_from_map(Cfg *c) {
    Cfg t;
    memset(&t, 0, sizeof t);
    cfg_str("mode", "", t.mode, sizeof t.mode);
    cfg_str("host", "", t.host, sizeof t.host);
    cfg_str("user", "", t.user, sizeof t.user);
    cfg_str("port", "22", t.port, sizeof t.port);
    cfg_str("output", "", t.output, sizeof t.output);
    cfg_str("scale", "360", t.scale, sizeof t.scale);
    cfg_str("fps", "15", t.fps, sizeof t.fps);
    cfg_str("codec", "libx264", t.codec, sizeof t.codec);
    cfg_str("muxer", "h264", t.muxer, sizeof t.muxer);
    cfg_str("encode", "极速（手机 CPU 最低）", t.encode, sizeof t.encode);
    cfg_str("hwdec", "vaapi", t.hwdec, sizeof t.hwdec);
    cfg_str("display", "半幅窗口", t.display, sizeof t.display);
    cfg_str("mpv_extra", "", t.mpv_extra, sizeof t.mpv_extra);
    t.lowlat = kv_bool("lowlat", 1);
    t.quiet = kv_bool("quiet", 1);
    t.wake = kv_bool("wake", 1);
    t.fullscreen = kv_bool("fullscreen", 0);
    *c = t;
}

/* 预览一行（"将执行：" 那一块） */
static void preview_line(const Cfg *c, char *out, size_t n) {
    char rcmd[4096], tgt[400], title[500];
    remote_command(c, rcmd, sizeof rcmd);
    ssh_target(c, tgt, sizeof tgt);
    snprintf(title, sizeof title, "%s%s", is_portal(c) ? "PORTAL-CAST:" : "MIRROR:", tgt);
    char *sargv[MAXARGS];
    int sn = 0;
    ssh_argv(c, sargv, &sn);
    append_argv(sargv, &sn, rcmd);
    char *pargv[MAXARGS];
    int pn = 0;
    player_argv(c, pargv, &pn, title);
    size_t o = 0;
    o += (size_t)snprintf(out + o, n - o, "%s", "");
    for (int i = 0; i < sn && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", shq(sargv[i]));
    o += (size_t)snprintf(out + o, n - o, " | ");
    for (int i = 0; i < pn && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", shq(pargv[i]));
}

/* ---------------- TUI 数据与绘制 ---------------- */

typedef struct {
    char kind;                 /* 'c' cycle, 't' text, 'b' toggle, 'a' action */
    const char *key, *label, *hint, *action;
    const char **choices;
} Row;

static Row g_rows[64];
static int g_nrows, g_sel;
static char g_status[2048];
static int g_err;
static int g_running;          /* TUI 循环是否继续 */

static int count_cp(const char *s) {          /* UTF-8 码点数（跟 Python len() 对齐） */
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}

static void row_add(char kind, const char *key, const char *label, const char **choices,
                    const char *hint, const char *action) {
    if (g_nrows >= 63) return;
    Row *r = &g_rows[g_nrows++];
    memset(r, 0, sizeof *r);
    r->kind = kind; r->key = key; r->label = label; r->choices = choices;
    r->hint = hint; r->action = action;
}

static int in_choices(const char **ch, const char *v) {
    for (int i = 0; ch[i]; i++) if (strcmp(ch[i], v) == 0) return 1;
    return 0;
}

static void build_rows(void) {
    g_nrows = 0;
    /* 最上面就是「开始投屏」：打开界面 → Enter 即可开播 */
    row_add('a', NULL, "▶ 开始投屏", NULL, "Enter 立即投屏（Ctrl-R 同效）；关掉 mpv 窗口即自动结束", "run");
    int is_portal = strcmp(kv_get("mode"), MS_MODES[0]) == 0;
    int is_stream = strcmp(kv_get("mode"), MS_MODES[2]) != 0;   /* portal 与 wf-recorder 共用这批行（waypipe 才是另一套） */
    row_add('c', "mode", "模式", MS_MODES,
            "portal 零编码 = 远端不编码、CPU 最低（推荐）；wf-recorder 流 = 旧方案；waypipe = 把远端应用拿到本机跑，【不能】镜像远端屏幕", NULL);
    row_add('t', "host", "目标主机", NULL, "远端 IP/主机名；Ctrl-L 选候选", NULL);
    row_add('t', "user", "登录用户", NULL, "留空 = ssh 默认用户", NULL);
    row_add('t', "port", "SSH 端口", NULL, "非 22 时 waypipe 模式会自动包一层 ssh", NULL);
    if (is_stream) {
        row_add('t', "output", "远端输出名", NULL, "如 DSI-1 / eDP-1；可用「探测远端输出名」", NULL);
        row_add('c', "fps", "帧率", MS_FPS, "远端软编吃 CPU：手机/SBC 建议 15~20", NULL);
        row_add('c', "scale", "缩放宽度", MS_SCALE, "portal 模式：门户已给逻辑分辨率（手机约 360），再放大没有收益", NULL);
        row_add('c', "encode", "编码档位", MS_ENCODE_KEYS, "实测：压得越狠≠越快（medium 比 superfast 手机 CPU 翻倍且延迟无改善）", NULL);
        row_add('c', "codec", "编码器", MS_CODEC, "libx264 通用；h264_v4l2m2m 试硬件编码", NULL);
        row_add('c', "muxer", "封装", MS_MUXER, "h264 = 裸流无容器（最低延迟，实测 Cache 0.00s）", NULL);
        row_add('c', "display", "窗口显示", MS_DISPLAY, "半幅窗口 = 普通窗口（niri 规则定尺寸），mpv 把远端画面缩放居中", NULL);
        if (is_portal)
            row_add('c', "remote_client", "远端客户端", MS_PROFILE_KEYS,
                    "C 版启动 0.07s / 峰值 RSS 1.2MB；Python 版 0.4s / 25MB。auto=优先 C，编译不出来自动回退", NULL);
        row_add('b', "keep_size", "锁定窗口尺寸", NULL, "niri 会把窗口压在当时的可用区内且不会自己长回来；开着就自动拉回", NULL);
        row_add('b', "lowlat", "极低延迟 mpv", NULL, "--untimed --video-sync=desync --demuxer-readahead-secs=0", NULL);
        row_add('c', "hwdec", "本机硬解", MS_HWDEC, "保持 vaapi：auto-safe 会挑 Vulkan 解码，而本机 Mesa 没有 VK_KHR_video_decode_queue", NULL);
        row_add('t', "mpv_extra", "mpv 附加参数", NULL, "如 --no-audio --speed=1", NULL);
    } else {
        row_add('c', "compress", "压缩", MS_COMPRESS, "waypipe 传输压缩", NULL);
        row_add('c', "video", "视频编码", MS_VIDEO, "需要两端都有 Vulkan", NULL);
        row_add('b', "nogpu", "强制 --no-gpu", NULL, "无 Vulkan 时打开", NULL);
        row_add('c', "display", "窗口显示", MS_DISPLAY, "（waypipe 模式保留项）", NULL);
        row_add('t', "wp_command", "远端命令", NULL, "要在本机跑起来的远端程序，如 foot", NULL);
    }
    row_add('b', "wake", "投屏前先唤醒远端屏幕", NULL, "必开！远端息屏时 screencopy 会失败", NULL);
    row_add('b', "quiet", "运行时不刷报文", NULL, "远端报告 + mpv 静默；日志写 ~/.local/state/mirror-screen/last-run.log", NULL);
    row_add('b', "exit_on_close", "关掉窗口即退出", NULL, "mpv 窗口一关就结束整个程序，并顺手清理远端残留", NULL);
    row_add('b', "close_tui", "投屏时关闭本界面", NULL, "开投屏后把 TUI 连同终端窗口一起关掉，只留投屏窗口", NULL);
    row_add('a', NULL, "唤醒远端屏幕", NULL, "wlr-randr --output <输出名> --on", "wake");
    row_add('a', NULL, "体检远端环境", NULL, "桌面/工具/抓屏实测(含息屏检测)/Vulkan", "check");
    row_add('a', NULL, "探测远端输出名", NULL, "niri→sway→hyprctl→wlr-randr 依次尝试", "probe");
    row_add('a', NULL, "测试 SSH 连通", NULL, "BatchMode，不会卡在密码提示", "sshtest");
    row_add('a', NULL, "保存配置", NULL, "Ctrl-S", "save");
    row_add('a', NULL, "退出", NULL, "q", "quit");
    if (g_sel >= g_nrows) g_sel = g_nrows - 1;
    if (g_sel < 0) g_sel = 0;
}

static void put(int y, int x, const char *text, int attr) {
    int h, w;
    getmaxyx(stdscr, h, w);
    if (y < 0 || y >= h) return;
    move(y, x);
    attron(attr);
    int max = w - x - 1;
    if (max > 0) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", text);
        if ((int)strlen(buf) > max) buf[max] = 0;
        addstr(buf);
    }
    attroff(attr);
}

static void wrap_text(const char *text, int width, char out[][512], int *nout, int maxlines) {
    *nout = 0;
    char cur[512] = "";
    const char *p = text;
    while (*p && *nout < maxlines) {
        const char *sp = strchr(p, ' ');
        size_t len = sp ? (size_t)(sp - p) : strlen(p);
        char tok[512];
        snprintf(tok, sizeof tok, "%.*s", (int)len, p);
        int ccl = count_cp(cur), tcl = count_cp(tok);
        if (ccl > 0 && ccl + tcl + 1 > width) {
            snprintf(out[(*nout)++], 512, "%s", cur);
            snprintf(cur, sizeof cur, "%s", tok);
        } else if (ccl == 0) {
            snprintf(cur, sizeof cur, "%s", tok);
        } else {
            char tmp[512];
            snprintf(tmp, sizeof tmp, "%s %s", cur, tok);
            snprintf(cur, sizeof cur, "%s", tmp);
        }
        p = sp ? sp + 1 : p + strlen(p);
    }
    if (*nout < maxlines) snprintf(out[(*nout)++], 512, "%s", cur);
}

static void tui_draw(void) {
    int h, w;
    getmaxyx(stdscr, h, w);
    erase();
    char hdr[512];
    snprintf(hdr, sizeof hdr, " 投屏到另一台 Linux · portal/wf-recorder 配置器 ");
    put(0, 0, hdr, A_REVERSE);
    int y = 2;
    for (int i = 0; i < g_nrows; i++) {
        Row *r = &g_rows[i];
        int sel = (i == g_sel);
        char line[1024];
        if (r->kind == 'a') {
            snprintf(line, sizeof line, "%s%s", sel ? "▸ " : "  ", r->label);
            put(y, 2, line, sel ? A_REVERSE : A_BOLD);
        } else {
            snprintf(line, sizeof line, "%s%-14s", sel ? "▸ " : "  ", r->label);
            put(y, 2, line, A_BOLD);
            const char *v = kv_get(r->key);
            char val[2048];
            if (r->kind == 'b') snprintf(val, sizeof val, "%s", kv_bool(r->key, 0) ? "[✓]" : "[ ]");
            else snprintf(val, sizeof val, "%s", (v && *v) ? v : "（空）");
            put(y, 20, val, sel ? A_REVERSE : A_NORMAL);
        }
        y++;
    }
    y++;
    put(y, 2, "将执行：", A_BOLD);
    y++;
    Cfg c;
    cfg_from_map(&c);
    char prev[8192];
    preview_line(&c, prev, sizeof prev);
    char lines[24][512];
    int nl = 0;
    wrap_text(prev, w - 4 > 0 ? w - 4 : 40, lines, &nl, 20);
    for (int i = 0; i < nl; i++) { put(y, 4, lines[i], A_DIM); y++; }
    y++;
    if (strcmp(kv_get("mode"), MS_MODES[2]) == 0) {
        put(y, 2, "[注意] waypipe 模式显示的是远端程序，不是远端屏幕", A_BOLD);
        y++;
    }
    if (g_rows[g_sel].hint) { put(y, 2, g_rows[g_sel].hint, A_DIM); y++; }
    if (g_status[0]) put(y, 2, g_status, A_BOLD);
    put(h - 1, 0, " ↑↓ 选择  Enter 编辑/执行  ←→ 切换  Ctrl-L 候选主机  Ctrl-R 投屏  Ctrl-W 唤醒远端  q 退出 ", A_REVERSE);
    refresh();
}

/* 单行编辑：返回新值；Esc 取消（返回原值指针） */
static void edit_line(const char *prompt, const char *initial, char *out, size_t outsz) {
    int h, w;
    getmaxyx(stdscr, h, w);
    curs_set(1);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", initial ? initial : "");
    while (1) {
        char line[2048];
        snprintf(line, sizeof line, "%s%s", prompt, buf);
        put(h - 2, 0, line, A_NORMAL);
        move(h - 2, (int)(count_cp(prompt) + count_cp(buf)) < w - 1 ? (int)(count_cp(prompt) + count_cp(buf)) : w - 2);
        refresh();
        wint_t ch;
        int rc = wget_wch(stdscr, &ch);
        if (rc == ERR) continue;
        if (rc == KEY_CODE_YES) {
            if (ch == KEY_BACKSPACE) { size_t l = strlen(buf); while (l && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) l--; if (l) buf[--l] = 0; }
            continue;
        }
        if (ch == L'\n' || ch == L'\r') break;
        if (ch == 27) { snprintf(out, outsz, "%s", initial ? initial : ""); curs_set(0); return; }
        if (ch == 21) { buf[0] = 0; continue; }                 /* Ctrl-U 清空 */
        if (ch == 127 || ch == 8) { size_t l = strlen(buf); while (l && ((unsigned char)buf[l - 1] & 0xC0) == 0x80) l--; if (l) buf[--l] = 0; continue; }
        if (ch >= 32) {
            char mb[8];
            int n = wctomb(mb, (wchar_t)ch);
            if (n > 0 && strlen(buf) + (size_t)n < sizeof buf - 1) { memcpy(buf + strlen(buf), mb, (size_t)n); buf[strlen(buf) + n] = 0; }
        }
    }
    curs_set(0);
    snprintf(out, outsz, "%s", buf);
}

/* 候选主机：~/.ssh/config 的 Host + known_hosts + 当前值 */
static int host_candidates(char out[][300], int max) {
    int n = 0;
    const char *cur = kv_get("host");
    if (cur && *cur) snprintf(out[n++], 300, "%s|当前配置", cur);
    char path[600];
    snprintf(path, sizeof path, "%s/.ssh/config", g_home);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f) && n < max) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (strncasecmp(p, "host ", 5) == 0 || strncasecmp(p, "host\t", 5) == 0) {
                char *save = NULL;
                for (char *t = strtok_r(p + 5, " \t\r\n", &save); t; t = strtok_r(NULL, " \t\r\n", &save)) {
                    if (strpbrk(t, "*?") || n >= max) continue;
                    snprintf(out[n++], 300, "%s|~/.ssh/config", t);
                }
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/.ssh/known_hosts", g_home);
    f = fopen(path, "r");
    if (f) {
        char line[4096];
        while (fgets(line, sizeof line, f) && n < max) {
            if (line[0] == '|') continue;                       /* 哈希过的跳过 */
            char *comma = strchr(line, ',');
            char *sp = strchr(line, ' ');
            if (!sp) continue;
            *sp = 0;
            if (comma) *comma = 0;
            char *h = line;
            if (h[0] == '[') { char *close = strchr(h, ']'); if (close) *close = 0, h++; }
            int dup = 0;
            for (int i = 0; i < n; i++) if (strncmp(out[i], h, strlen(h)) == 0) { dup = 1; break; }
            if (!dup && *h && !strpbrk(h, "*?")) snprintf(out[n++], 300, "%s|known_hosts", h);
        }
        fclose(f);
    }
    return n;
}

static void pick_host(void) {
    char cands[64][300];
    int n = host_candidates(cands, 64);
    if (n <= 0) { snprintf(g_status, sizeof g_status, "没有候选主机，请直接编辑「目标主机」"); return; }
    int idx = 0;
    while (1) {
        int h, w;
        getmaxyx(stdscr, h, w);
        erase();
        put(0, 0, " 选择目标主机（Enter 确认，Esc 取消） ", A_REVERSE);
        for (int i = 0; i < n && i < h - 3; i++) {
            char host[300], src[200];
            snprintf(host, sizeof host, "%s", cands[i]);
            char *bar = strchr(host, '|');
            if (bar) { *bar = 0; snprintf(src, sizeof src, "%s", bar + 1); } else src[0] = 0;
            char line[600];
            snprintf(line, sizeof line, "%-30s %s", host, src);
            put(1 + i, 2, line, i == idx ? A_REVERSE : A_NORMAL);
        }
        refresh();
        wint_t ch;
        int rc = wget_wch(stdscr, &ch);
        if (rc == ERR) continue;
        if (rc == KEY_CODE_YES) {
            if (ch == KEY_UP) idx = idx > 0 ? idx - 1 : 0;
            if (ch == KEY_DOWN) idx = idx < n - 1 ? idx + 1 : n - 1;
            continue;
        }
        if (ch == 27) return;
        if (ch == L'\n' || ch == L'\r') {
            char host[300];
            snprintf(host, sizeof host, "%s", cands[idx]);
            char *bar = strchr(host, '|');
            if (bar) *bar = 0;
            kv_set("host", host);
            build_rows();
            snprintf(g_status, sizeof g_status, "目标已设为 %s", host);
            return;
        }
        if (ch == L'k') idx = idx > 0 ? idx - 1 : 0;
        if (ch == L'j') idx = idx < n - 1 ? idx + 1 : n - 1;
    }
}

/* ---------------- TUI 动作 ---------------- */

static int ssh_run(const char *script, char *out, size_t outsz, int timeout_s) {
    Cfg c;
    cfg_from_map(&c);
    char *argv[MAXARGS];
    int n = 0;
    ssh_argv(&c, argv, &n);
    append_argv(argv, &n, script);
    return run_capture(argv, out, outsz, timeout_s);
}

static void do_wake(void) {
    const char *outname = kv_get("output");
    if (!outname || !*outname) {
        snprintf(g_status, sizeof g_status, "先填/探测「远端输出名」再唤醒");
        g_err = 1;
        return;
    }
    snprintf(g_status, sizeof g_status, "正在唤醒远端屏幕（%s）…", outname);
    g_err = 0;
    tui_draw();
    char script[1024], out[8192];
    snprintf(script, sizeof script,
             "wlr-randr --output %s --on 2>&1; sleep 1; cat /sys/class/drm/card*-%s/dpms 2>/dev/null | head -1",
             outname, outname);
    int rc = ssh_run(script, out, sizeof out, 25);
    char *last = strrchr(out, '\n');
    const char *tail = (last && last[1]) ? last + 1 : (out[0] ? out : "(无输出)");
    snprintf(g_status, sizeof g_status, "唤醒结果：%s（rc=%d）", tail, rc);
    g_err = rc != 0;
}

static void do_check(void) {
    snprintf(g_status, sizeof g_status, "正在体检远端…");
    g_err = 0;
    tui_draw();
    static char out[65536];
    int rc = ssh_run(MS_CHECK_SCRIPT, out, sizeof out, 40);
    char comp[128] = "unknown", info_display[256] = "", cap[128] = "", ps[64] = "", dpms[64] = "", vulkan[64] = "";
    int has_wfr = 0, has_randr = 0, got = 0;
    char *line = out;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *k = line, *v = eq + 1;
            while (*k == ' ' || *k == '\t') k++;
            if (!strcmp(k, "compositor")) snprintf(comp, sizeof comp, "%s", v);
            else if (!strcmp(k, "display")) snprintf(info_display, sizeof info_display, "%s", v);
            else if (!strcmp(k, "capture")) snprintf(cap, sizeof cap, "%s", v);
            else if (!strcmp(k, "powersave")) snprintf(ps, sizeof ps, "%s", v);
            else if (!strcmp(k, "drmdpms")) snprintf(dpms, sizeof dpms, "%s", v);
            else if (!strcmp(k, "vulkan")) snprintf(vulkan, sizeof vulkan, "%s", v);
            else if (!strcmp(k, "wfrecorder")) has_wfr = !strcmp(v, "yes");
            else if (!strcmp(k, "wlrrandr")) has_randr = !strcmp(v, "yes");
            got = 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    if (rc != 0 || !got) {
        snprintf(g_status, sizeof g_status, "体检失败（rc=%d）", rc);
        g_err = 1;
        return;
    }
    char notes[2048] = "";
    if (!has_wfr) strncat(notes, "[!] 远端缺 wf-recorder → apk add wf-recorder / pacman -S wf-recorder；", sizeof notes - strlen(notes) - 1);
    if (!has_randr && info_display[0]) strncat(notes, "[!] 远端缺 wlr-randr（无法远程唤醒屏幕）；", sizeof notes - strlen(notes) - 1);
    if (strcmp(ps, "on") == 0) strncat(notes, "[!] 远端 WiFi 省电开着 → 延迟 ~95ms；关掉后 ~11ms；", sizeof notes - strlen(notes) - 1);
    char part[600];
    if (cap[0] == 0 || strcmp(cap, "FAIL") == 0) {
        snprintf(part, sizeof part, "[!] 远端抓屏失败（dpms=%s）→ 多半是息屏，点「唤醒远端屏幕」；", dpms[0] ? dpms : "?");
        strncat(notes, part, sizeof notes - strlen(notes) - 1);
    } else {
        snprintf(part, sizeof part, "远端 OK：%s · %s · 抓屏 %s 字节 · Vulkan %s ICD；", comp, info_display, cap, vulkan[0] ? vulkan : "?");
        strncat(notes, part, sizeof notes - strlen(notes) - 1);
    }
    if (strcmp(comp, "unknown") == 0) strncat(notes, "[!] 桌面类型未识别（GNOME/KDE 不支持 screencopy 类方案）；", sizeof notes - strlen(notes) - 1);
    build_rows();
    snprintf(g_status, sizeof g_status, "%s", notes);
    g_err = strstr(notes, "[!]") != NULL;
}

static void detach_cast(void) {
    stop_previous_cast();
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); }
        char exe[512];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n <= 0) snprintf(exe, sizeof exe, "mirror-screen-c");
        else exe[n] = 0;
        execl(exe, exe, "--run", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { snprintf(g_status, sizeof g_status, "启动失败"); g_err = 1; return; }
    /* 2 秒内若立即退出，说明起不来，把日志尾巴显示出来 */
    for (int i = 0; i < 20; i++) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            char tail[1024] = "";
            FILE *f = fopen(g_log, "r");
            if (f) {
                char line[512], last[3][512];
                int k = 0;
                while (fgets(line, sizeof line, f)) { snprintf(last[k % 3], sizeof last[0], "%s", line); k++; }
                fclose(f);
                snprintf(tail, sizeof tail, "%s", last[k ? (k - 1) % 3 : 0]);
            }
            snprintf(g_status, sizeof g_status, "投屏启动失败：%s", tail);
            g_err = 1;
            return;
        }
        usleep(100000);
    }
    g_running = 0;                 /* 界面就此关闭（终端窗口跟着关） */
}

static void do_action(const char *action) {
    if (!strcmp(action, "run")) {
        if (!kv_get("host")[0]) { snprintf(g_status, sizeof g_status, "请先填「目标主机」"); g_err = 1; return; }
        if (kv_bool("wake", 1)) do_wake();
        kv_save();
        if (kv_bool("close_tui", 1)) { detach_cast(); return; }
        def_prog_mode();
        endwin();
        Cfg c;
        cfg_from_map(&c);
        int rc = run_cast(&c);
        reset_prog_mode();
        refresh();
        snprintf(g_status, sizeof g_status, "投屏结束（rc=%d）· 日志：%s", rc, g_log);
        g_err = rc != 0;
        if (kv_bool("exit_on_close", 1)) g_running = 0;
    } else if (!strcmp(action, "wake")) {
        do_wake();
    } else if (!strcmp(action, "check")) {
        do_check();
    } else if (!strcmp(action, "probe")) {
        snprintf(g_status, sizeof g_status, "正在探测远端输出名…");
        tui_draw();
        char out[16384];
        int rc = ssh_run(MS_PROBE_SCRIPT, out, sizeof out, 25);
        char first[128] = "";
        char *l = out;
        while (l && *l) {
            char *nl = strchr(l, '\n');
            if (nl) *nl = 0;
            if (*l && !strchr(l, ' ')) { snprintf(first, sizeof first, "%s", l); break; }
            l = nl ? nl + 1 : NULL;
        }
        if (rc == 0 && first[0]) {
            kv_set("output", first);
            build_rows();
            snprintf(g_status, sizeof g_status, "远端输出 = %s", first);
            g_err = 0;
        } else {
            snprintf(g_status, sizeof g_status, "探测失败（rc=%d）", rc);
            g_err = 1;
        }
    } else if (!strcmp(action, "sshtest")) {
        snprintf(g_status, sizeof g_status, "正在测试 SSH…");
        tui_draw();
        char out[4096];
        int rc = ssh_run("true", out, sizeof out, 15);
        snprintf(g_status, sizeof g_status, "%s", rc == 0 ? "SSH 可用" : "SSH 不通");
        g_err = rc != 0;
    } else if (!strcmp(action, "save")) {
        kv_save();
        snprintf(g_status, sizeof g_status, "已保存到 %s", g_cfg);
        g_err = 0;
    } else if (!strcmp(action, "quit")) {
        kv_save();
        g_running = 0;
    }
}

static int tui_main(void) {
    setlocale(LC_ALL, "");
    kv_load();
    build_rows();
    g_sel = 0;
    g_running = 1;
    g_status[0] = 0;
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    while (g_running) {
        tui_draw();
        wint_t ch;
        int rc = wget_wch(stdscr, &ch);
        if (rc == ERR) continue;
        Row *row = &g_rows[g_sel];
        if (rc == KEY_CODE_YES) {
            if (ch == KEY_UP) g_sel = (g_sel - 1 + g_nrows) % g_nrows;
            else if (ch == KEY_DOWN) g_sel = (g_sel + 1) % g_nrows;
            else if (ch == KEY_ENTER) {
                if (row->kind == 'a') do_action(row->action);
                else if (row->kind == 't') { char v[1024], p[128]; snprintf(p, sizeof p, "%s：", row->label); edit_line(p, kv_get(row->key), v, sizeof v); kv_set(row->key, v); build_rows(); }
                else if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i + 1) % n]);
                    build_rows();
                } else if (row->kind == 'b') { kv_toggle(row->key); build_rows(); }
            } else if (ch == KEY_LEFT) {
                if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i - 1 + n) % n]);
                } else if (row->kind == 'b') kv_toggle(row->key);
                build_rows();
            } else if (ch == KEY_RIGHT) {
                if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i + 1) % n]);
                } else if (row->kind == 'b') kv_toggle(row->key);
                build_rows();
            } else if (ch == KEY_BACKSPACE) {
                /* ignore */
            }
            continue;
        }
        switch (ch) {
            case L'q': case 3: kv_save(); g_running = 0; break;
            case L'k': g_sel = (g_sel - 1 + g_nrows) % g_nrows; break;
            case L'j': g_sel = (g_sel + 1) % g_nrows; break;
            case 12: pick_host(); break;                     /* Ctrl-L */
            case 18: do_action("run"); break;                /* Ctrl-R */
            case 23: do_wake(); break;                       /* Ctrl-W */
            case 19: kv_save(); snprintf(g_status, sizeof g_status, "已保存到 %s", g_cfg); g_err = 0; break;  /* Ctrl-S */
            case L' ': case L'\n': case L'\r': {
                if (row->kind == 'a') do_action(row->action);
                else if (row->kind == 't') { char v[1024], p[128]; snprintf(p, sizeof p, "%s：", row->label); edit_line(p, kv_get(row->key), v, sizeof v); kv_set(row->key, v); build_rows(); }
                else if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i + 1) % n]);
                    build_rows();
                } else if (row->kind == 'b') { kv_toggle(row->key); build_rows(); }
                break;
            }
            case L'l': case L'+': {
                if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i + 1) % n]);
                } else if (row->kind == 'b') kv_toggle(row->key);
                build_rows();
                break;
            }
            case L'h': case L'-': {
                if (row->kind == 'c') {
                    const char **cs = row->choices;
                    int i = 0, n = 0;
                    for (; cs[n]; n++) if (!strcmp(cs[n], kv_get(row->key))) i = n;
                    kv_set(row->key, cs[(i - 1 + n) % n]);
                } else if (row->kind == 'b') kv_toggle(row->key);
                build_rows();
                break;
            }
            default: break;
        }
    }
    endwin();
    return 0;
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    snprintf(g_home, sizeof g_home, "%s", home ? home : "/tmp");
    const char *xdgc = getenv("XDG_CONFIG_HOME");
    const char *xdgs = getenv("XDG_STATE_HOME");
    const char *xdgca = getenv("XDG_CACHE_HOME");
    char base[600];
    if (xdgc && *xdgc) snprintf(base, sizeof base, "%s", xdgc);
    else snprintf(base, sizeof base, "%s/.config", g_home);
    snprintf(g_cfg, sizeof g_cfg, "%s/%s/config.json", base, APP);
    if (xdgs && *xdgs) snprintf(base, sizeof base, "%s", xdgs);
    else snprintf(base, sizeof base, "%s/.local/state", g_home);
    snprintf(g_state, sizeof g_state, "%s/%s", base, APP);
    snprintf(g_log, sizeof g_log, "%s/last-run.log", g_state);
    if (xdgca && *xdgca) snprintf(base, sizeof base, "%s", xdgca);
    else snprintf(base, sizeof base, "%s/.cache", g_home);
    snprintf(g_cache, sizeof g_cache, "%s/%s", base, APP);
    mkdir(g_cache, 0700);
    mkdir(g_state, 0700);

    long n = 0;
    g_json = read_whole(g_cfg, &n);
    Cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg_load(&cfg);

    if (argc == 1 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) return tui_main();   /* 无参数 = 配置界面 */
    if (argc > 1 && strcmp(argv[1], "--print-config") == 0) { print_config(&cfg); return 0; }
    if (argc > 1 && strcmp(argv[1], "--dry-run") == 0) { print_dry_run(&cfg); return 0; }
    if (argc > 1 && strcmp(argv[1], "--wake") == 0) {
        if (!cfg.host[0]) { fprintf(stderr, "尚未配置目标主机\n"); return 2; }
        char *a[MAXARGS];
        int an = 0;
        ssh_argv(&cfg, a, &an);
        append_argvf(a, &an, "wlr-randr --output %s --on 2>&1; sleep 1", cfg.output);
        return run_silent(a);
    }
    if (argc > 1 && strcmp(argv[1], "--run") == 0) {
        if (!cfg.host[0]) { fprintf(stderr, "尚未配置目标主机\n"); return 2; }
        if (!is_portal(&cfg) && !is_stream(&cfg)) {
            fprintf(stderr, "C 引擎只实现 portal / wf-recorder 两种模式；waypipe 模式请用 mirror-screen.py\n");
            return 2;
        }
        stop_previous_cast();
        return run_cast(&cfg);
    }
    if (argc > 1 && strcmp(argv[1], "--list-hosts") == 0) {
        char cands[64][300];
        int n = host_candidates(cands, 64);
        for (int i = 0; i < n; i++) {
            char host[300], src[128] = "";
            snprintf(host, sizeof host, "%s", cands[i]);
            char *bar = strchr(host, '|');
            if (bar) { *bar = 0; snprintf(src, sizeof src, "%s", bar + 1); }
            if (src[0]) printf("%s\t%s\n", host, src);
            else printf("%s\n", host);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--check") == 0) {
        char *a[MAXARGS];
        int an = 0;
        ssh_argv(&cfg, a, &an);
        append_argv(a, &an, MS_CHECK_SCRIPT);
        static char out[65536];
        int rc = run_capture(a, out, sizeof out, 45);
        fputs(out, stdout);
        return rc;
    }
    fprintf(stderr, "用法: mirror-screen-c {--run|--dry-run|--print-config|--wake|--check|--list-hosts}\n");
    return 2;
}
