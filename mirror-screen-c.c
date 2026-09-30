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
    char buf[64];
    snprintf(buf, sizeof buf, "--window-scale=%.3f", (height * scale - 8.0) / (fh > 0 ? fh : 760));
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
    char prof[128] = "preset=superfast crf=20";
    if (strstr(c->encode, "极速")) snprintf(prof, sizeof prof, "preset=superfast crf=20");
    else if (strstr(c->encode, "极低") || strstr(c->encode, "ultrafast")) snprintf(prof, sizeof prof, "preset=ultrafast crf=22");
    else if (strstr(c->encode, "均衡")) snprintf(prof, sizeof prof, "preset=veryfast crf=23");
    else if (strstr(c->encode, "低带宽")) snprintf(prof, sizeof prof, "preset=medium crf=26");
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
        append_argv(argv, n, "--wayland-app-id=portal-cast-fitted");
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
    snprintf(title, sizeof title, "PORTAL-CAST:%s", tgt);
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

static int run_cast(const Cfg *c) {
    char title[500], tgt[400];
    ssh_target(c, tgt, sizeof tgt);
    snprintf(title, sizeof title, "PORTAL-CAST:%s", tgt);

    log_fd = open(g_log, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) { fprintf(stderr, "无法写日志 %s\n", g_log); return 2; }

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
    fprintf(stderr, "用法: mirror-screen-c {--run|--dry-run|--print-config|--wake}\n");
    return 2;
}
