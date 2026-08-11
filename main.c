#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include "json.h"
#include "uia.h"
#include "win.h"

/* ---------- CLI 参数辅助 ---------- */

/* CRT main 的 argv 是 ANSI(GBK) 编码，中文参数会乱码；
 * 用 CommandLineToArgvW 取 UTF-16 参数并转 UTF-8 */
static char *w_to_utf8_arg(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static char **argv_utf8(int *argc_out) {
    int n;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!wargv) {
        *argc_out = 0;
        return NULL;
    }
    char **argv = malloc((n + 1) * sizeof(char *));
    for (int i = 0; i < n; i++)
        argv[i] = w_to_utf8_arg(wargv[i]);
    argv[n] = NULL;
    LocalFree(wargv);
    *argc_out = n;
    return argv;
}

static const char *arg(int i, int argc, char **argv) {
    return i < argc ? argv[i] : NULL;
}

/* 从 start 开始扫描布尔 flag */
static int has_flag(int argc, char **argv, int start, const char *name) {
    for (int i = start; i < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return 1;
    return 0;
}

/* 取带值 flag（--button right 形式），未找到返回 NULL */
static const char *flag_val(int argc, char **argv, int start, const char *name) {
    for (int i = start; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}

/* 取下一个非 flag 位置参数（跳过 --xxx，带值 flag 连带跳过其值），i 为游标 */
static const char *next_arg(int argc, char **argv, int *i) {
    while (*i < argc) {
        const char *s = argv[*i];
        (*i)++;
        if (s[0] == '-' && s[1] != '\0') {
            if (strcmp(s, "--button") == 0 || strcmp(s, "--action") == 0 ||
                strcmp(s, "--class") == 0 || strcmp(s, "--name") == 0)
                (*i)++; /* 带值 flag，连带跳过其值 */
            continue;
        }
        return s;
    }
    return NULL;
}

/* 错误：stderr 输出 + 非零退出码 */
static int fail(int code, const char *msg) {
    fprintf(stderr, "%s\n", msg);
    return code;
}

/* 输出为 UTF-8。交互式终端（WSL interop 的 conhost / PowerShell）默认按
 * 系统代码页（中文系统 936/GBK）解码字节，需显式切到 65001；
 * stdout 是管道/重定向时 GetConsoleMode 失败，自动跳过，零影响。
 * 同时将 stdout/stderr 设为二进制模式，避免 CRT 把 \n 转成 \r\n 污染管道 */
static void init_console_utf8(void) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode))
        SetConsoleOutputCP(CP_UTF8);
}

/* ---------- locator ---------- */

/* locator = 16 进制 hwnd 前缀（0x 可选）+ 可选 /xpath；xpath 为 NULL 表示根元素 */
static int parse_locator(const char *loc, char *hwnd_buf, size_t cap,
                         const char **xpath) {
    const char *p = loc;
    if (!*p)
        return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    const char *start = p;
    while (*p && *p != '/') {
        if (!isxdigit((unsigned char)*p))
            return 0;
        p++;
    }
    if (p == start)
        return 0;
    size_t n = (size_t)(p - start);
    if (n >= cap)
        return 0;
    memcpy(hwnd_buf, start, n);
    hwnd_buf[n] = '\0';
    *xpath = (*p == '/') ? p : NULL;
    return 1;
}

/* UIA 初始化 + 定位（取第一个匹配），成功返回 0 */
static int uia_locate(const char *loc, Uia *u, void **el_out, const char **xp_out) {
    char hwnd_buf[32];
    const char *xp;
    if (!parse_locator(loc, hwnd_buf, sizeof hwnd_buf, &xp))
        return fail(1, "locator 无效");
    if (!uia_init(u, hwnd_buf))
        return fail(2, "窗口句柄无效");
    void *el = uia_find(u, xp);
    if (!el) {
        uia_free(u);
        return fail(3, "元素未找到");
    }
    *el_out = el;
    *xp_out = xp;
    return 0;
}

/* ---------- 命令 ---------- */

static int cmd_list(int argc, char **argv, int a, int json) {
    int i = a;
    const char *loc = next_arg(argc, argv, &i);
    int all = has_flag(argc, argv, a, "--all");
    SB out;
    sb_init(&out);
    int rc = 0;

    if (!loc || loc[0] == '/') {
        /* 全局模式：虚拟根 = 所有顶层窗口；默认 xpath /Window（顶层窗口列表），
         * 全量输出用 // 加 * */
        char err[128];
        if (!uia_list_all(all, loc ? loc : "/Window", json, err, sizeof err, &out)) {
            rc = fail(1, err);
            goto out_free;
        }
        if (json)
            printf("[%s]\n", out.buf);
        else if (out.buf[0])
            printf("%s\n", out.buf);
    } else {
        char hwnd_buf[32];
        const char *xp;
        char err[128];
        if (!parse_locator(loc, hwnd_buf, sizeof hwnd_buf, &xp)) {
            rc = fail(1, "locator 无效");
            goto out_free;
        }
        Uia u;
        if (!uia_init(&u, hwnd_buf)) {
            rc = fail(2, "窗口句柄无效");
            goto out_free;
        }
        if (!uia_list(&u, xp, json, err, sizeof err, &out)) {
            uia_free(&u);
            rc = fail(1, err);
            goto out_free;
        }
        uia_free(&u);
        if (json)
            printf("[%s]\n", out.buf);
        else if (out.buf[0])
            printf("%s\n", out.buf);
    }

out_free:
    sb_free(&out);
    return rc;
}

static int cmd_prop(int argc, char **argv, int a, int json) {
    int i = a;
    const char *loc = next_arg(argc, argv, &i);
    if (!loc)
        return fail(1, "缺少 locator");
    const char *key = next_arg(argc, argv, &i);
    const char *val = next_arg(argc, argv, &i);

    Uia u;
    void *el;
    const char *xp;
    int rc = uia_locate(loc, &u, &el, &xp);
    if (rc)
        return rc;

    SB out;
    sb_init(&out);

    if (!key) {
        /* 读全部属性 */
        uia_element_prop(&u, el, xp, json, &out);
        printf("%s\n", out.buf);
    } else if (strcmp(key, "value") == 0) {
        if (!val) {
            /* 读值 */
            char *v = uia_get_value(&u, el);
            if (json) {
                sb_json_str_or_null(&out, v);
                printf("%s\n", out.buf);
            } else {
                printf("%s\n", v ? v : "");
            }
            free(v);
        } else {
            /* 写值 */
            if (!uia_set_value(&u, el, val))
                rc = fail(4, "写入失败");
        }
    } else if (strcmp(key, "state") == 0) {
        char hb[32];
        snprintf(hb, sizeof hb, "0x%llx", uia_element_hwnd(&u, el));
        if (!val) {
            /* 读窗口状态 */
            char st[16];
            win_get_window_state(hb, st, sizeof st);
            printf("%s\n", st);
        } else {
            /* 写窗口状态 */
            if (!win_set_window_state(hb, val))
                rc = fail(4, "设置窗口状态失败");
        }
    } else if (strcmp(key, "pid") == 0) {
        /* 只读：进程 ID */
        if (val) {
            rc = fail(1, "pid 只读");
        } else {
            char hb[32];
            snprintf(hb, sizeof hb, "0x%llx", uia_element_hwnd(&u, el));
            unsigned long pid = 0;
            win_get_pid(hb, &pid);
            if (json) {
                sb_json_int(&out, (long long)pid);
                printf("%s\n", out.buf);
            } else {
                printf("%lu\n", pid);
            }
        }
    } else {
        rc = fail(1, "未知属性");
    }

    sb_free(&out);
    uia_release_element(el);
    uia_free(&u);
    return rc;
}

static int cmd_click(int argc, char **argv, int a, int json) {
    (void)json;
    int i = a;
    const char *loc = next_arg(argc, argv, &i);
    if (!loc)
        return fail(1, "缺少 locator");
    const char *action = flag_val(argc, argv, a, "--action");
    if (!action) action = "click";
    const char *button = flag_val(argc, argv, a, "--button");
    if (!button) button = "left";
    int foreground = has_flag(argc, argv, a, "--mouse");

    Uia u;
    void *el;
    const char *xp;
    int rc = uia_locate(loc, &u, &el, &xp);
    if (rc)
        return rc;
    int ok = uia_click(&u, el, action, button, foreground);
    uia_release_element(el);
    uia_free(&u);
    return ok ? 0 : fail(4, "点击失败");
}

static int cmd_focus(int argc, char **argv, int a, int json) {
    (void)json;
    int i = a;
    const char *loc = next_arg(argc, argv, &i);
    if (!loc)
        return fail(1, "缺少 locator");

    Uia u;
    void *el;
    const char *xp;
    int rc = uia_locate(loc, &u, &el, &xp);
    if (rc)
        return rc;
    int ok = uia_focus(&u, el);
    uia_release_element(el);
    uia_free(&u);
    return ok ? 0 : fail(4, "设置焦点失败");
}

/* ---------- 帮助 ---------- */

static void usage(void) {
    puts("NAME");
    puts("    winctl - Windows 自动化驱动（一次进程一个操作）");
    puts("");
    puts("SYNOPSIS");
    puts("    winctl <命令> [参数...]");
    puts("    winctl --help");
    puts("");
    puts("COMMANDS");
    puts("    list [--all] [--json]");
    puts("        元素查询（集合语义）：无 locator = /Window（顶层窗口列表）；全量输出用 //*；");
    puts("        locator 以 / 开头 = 全局查询（虚拟根）；hwnd 前缀 = 单窗口，无 xpath = 整树");
    puts("");
    puts("    prop <locator> [key] [value] [--json]");
    puts("        读写属性：无 value 参读，有 value 参写；key 缺省读全部（kv 行式）");
    puts("");
    puts("    click <locator> [--mouse] [--button B] [--action A]");
    puts("        点击元素（取第一个匹配）");
    puts("");
    puts("    focus <locator>");
    puts("        聚焦元素（取第一个匹配）");
    puts("");
    puts("    prop/click/focus 的 locator 取第一个匹配；list 是唯一集合语义命令");
    puts("");
    puts("LOCATOR");
    puts("    <hwnd>            根元素（16 进制，0x 前缀可选）；可为任意窗口/元素句柄，");
    puts("                      list 输出列的 hwnd 可直接作为根");
    puts("    <hwnd>/<xpath>    定位其下元素，xpath 相对该根；首段必须匹配根元素类型，");
    puts("                      深层元素用完整路径（含根类型段）或以元素 hwnd 为根");
    puts("    /<xpath>          list 全局查询：xpath 从虚拟根求值（所有顶层窗口），");
    puts("                      首段 /Window 匹配顶层窗口（仅支持属性谓词，不支持 [n]），");
    puts("                      //Window 匹配任意深度");
    puts("    xpath 语法: / // * [n] 位置谓词（树内）、[@Name=''] [@Type=''] [@Id=''] [@Class=''] [@Pid='']");
    puts("                属性谓词（@Pid 仅顶层窗口段）、*= 包含 ^= 前缀 $= 后缀、and、!=");
    puts("");
    puts("PROPERTIES");
    puts("    value   可读可写。ValuePattern 读写，失败回退聚焦+剪贴板输入");
    puts("    state   可读可写。窗口状态，取值 normal|maximized|minimized");
    puts("    pid     只读。元素窗口句柄所属进程 ID（十进制）");
    puts("");
    puts("OPTIONS");
    puts("    --all        列出不可见窗口与默认排除进程的窗口（默认排除 firefox.exe 等大树进程）");
    puts("    --json       输出完整 JSON");
    puts("    --mouse      点击使用前台真实鼠标（默认后台 PostMessage，不动鼠标）");
    puts("    --button B   鼠标按键 left|right");
    puts("    --action A   点击动作 click|press|dbclick");
    puts("");
    puts("EXAMPLES");
    puts("    winctl list                                 顶层窗口列表（默认 /Window，快）");
    puts("    winctl list //*                            全量（所有可见窗口的元素树）");
    puts("    winctl list /Window[@Name*='记事本']        按标题查顶层窗口（预筛，快）");
    puts("    winctl list /Window[@Pid='1234']           按进程查顶层窗口（弹窗场景，快）");
    puts("    winctl list /Window[@Name*='记事本']/Pane//Button   顶层窗口内查询");
    puts("    winctl list 0x1a2b                          单窗口整树");
    puts("    winctl list \"0x1a2b//*[@Name^='打开']\"       单窗口内按名称前缀过滤");
    puts("    winctl prop 0x1a2b value                    读值");
    puts("    winctl prop 0x1a2b value 文本               写值");
    puts("    winctl prop 0x1a2b state maximized          最大化窗口");
    puts("    winctl prop 0x1a2b pid                      读进程 ID");
    puts("    winctl click 0x1a2b/Window/Pane/Button      点击");
    puts("");
    puts("    顶层窗口定位仅支持属性谓词（[@Name]/[@Pid]/[@Class]），不支持位置谓词 [n]");
    puts("    （顶层窗口序号依 Z 序变化，不稳定）；树内 [n] 为兄弟序，结构稳定可用");
    puts("    全局查询 @Name/@Class/@Pid 谓词走 Win32 预筛（毫秒级）；");
    puts("    [n] 位置谓词与 // 后代查询需全量遍历（秒级）");
    puts("");
    puts("OUTPUT");
    puts("    默认人类可读（对齐列 / kv 行式）；--json 输出完整 JSON");
    puts("    元素列表列: hwnd xpath name；终端下 name 超宽省略号，xpath 永远完整");
    puts("");
    puts("EXIT STATUS");
    puts("    0  成功（含查询空集）");
    puts("    1  参数/locator 无效");
    puts("    2  窗口句柄无效");
    puts("    3  元素未找到");
    puts("    4  操作失败");
}/* ---------- 入口 ---------- */

int main(int argc, char **argv) {
    init_console_utf8();
    /* 用 UTF-16 命令行重新解析参数（CRT argv 为 ANSI 编码，中文会乱码） */
    char **uargv = argv_utf8(&argc);
    if (uargv)
        argv = uargv;

    int rc = 0;
    const char *cmd = arg(1, argc, argv);
    if (!cmd) {
        usage();
        goto free_out;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        goto free_out;
    }

    int a = 2;
    int json = has_flag(argc, argv, a, "--json");

    if (strcmp(cmd, "list") == 0)
        rc = cmd_list(argc, argv, a, json);
    else if (strcmp(cmd, "prop") == 0)
        rc = cmd_prop(argc, argv, a, json);
    else if (strcmp(cmd, "click") == 0)
        rc = cmd_click(argc, argv, a, json);
    else if (strcmp(cmd, "focus") == 0)
        rc = cmd_focus(argc, argv, a, json);
    else {
        usage();
        rc = fail(1, "未知命令");
    }

    fflush(stdout);
free_out:
    if (uargv) {
        for (int i = 0; i < argc; i++)
            free(uargv[i]);
        free(uargv);
    }
    return rc;
}
