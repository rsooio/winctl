#include "win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 辅助 ---------- */

static HWND parse_hwnd(const char *s) {
    return (HWND)(ULONG_PTR)strtoull(s, NULL, 16);
}

static char *utf8_to_w_str(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return (char *)w;
}

/* ---------- 窗口枚举 ---------- */

struct EnumCtx {
    unsigned long long *hwnds;
    int n;
    int max;
    int all;
};

static BOOL CALLBACK enum_top_proc(HWND hwnd, LPARAM lp) {
    struct EnumCtx *ctx = (struct EnumCtx *)lp;
    if (!ctx->all && !IsWindowVisible(hwnd))
        return TRUE; /* 默认过滤不可见窗口 */
    if (ctx->n < ctx->max)
        ctx->hwnds[ctx->n++] = (unsigned long long)(ULONG_PTR)hwnd;
    return TRUE;
}

int win_enum_top(int all, unsigned long long *hwnds, int max) {
    struct EnumCtx ctx = {hwnds, 0, max, all};
    EnumWindows(enum_top_proc, (LPARAM)&ctx);
    return ctx.n;
}

/* ---------- 窗口状态 ---------- */

int win_set_window_state(const char *hwnd_str, const char *state) {
    HWND hwnd = parse_hwnd(hwnd_str);
    int cmd;
    if (strcmp(state, "maximized") == 0)
        cmd = SW_MAXIMIZE;
    else if (strcmp(state, "minimized") == 0)
        cmd = SW_MINIMIZE;
    else
        cmd = SW_RESTORE;
    ShowWindow(hwnd, cmd);
    return 1;
}

int win_get_window_state(const char *hwnd_str, char *out, size_t cap) {
    HWND hwnd = parse_hwnd(hwnd_str);
    if (IsZoomed(hwnd))
        snprintf(out, cap, "maximized");
    else if (IsIconic(hwnd))
        snprintf(out, cap, "minimized");
    else
        snprintf(out, cap, "normal");
    return 1;
}

int win_get_pid(const char *hwnd_str, unsigned long *pid_out) {
    HWND hwnd = parse_hwnd(hwnd_str);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    *pid_out = (unsigned long)pid;
    return 1;
}

/* ---------- 剪贴板（内部使用） ---------- */

int win_set_clipboard(const char *utf8) {
    wchar_t *w = (wchar_t *)utf8_to_w_str(utf8);
    if (!w)
        return 0;
    size_t cch = wcslen(w) + 1;
    int ok = 0;
    for (int i = 0; i < 5 && !ok; i++) {
        if (!OpenClipboard(NULL)) {
            Sleep(20);
            continue;
        }
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, cch * sizeof(wchar_t));
        if (h) {
            wchar_t *dst = GlobalLock(h);
            if (dst) {
                memcpy(dst, w, cch * sizeof(wchar_t));
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h))
                    ok = 1;
            }
            if (!ok)
                GlobalFree(h);
        }
        CloseClipboard();
        if (!ok)
            Sleep(20);
    }
    free(w);
    return ok;
}

/* ---------- 键盘（内部使用） ---------- */

static void key_event(int vk, int up) {
    INPUT in = {0};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    if (up)
        in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof in);
}

void win_tap_combo(int vk1, int vk2) {
    key_event(vk1, 0);
    key_event(vk2, 0);
    key_event(vk2, 1);
    key_event(vk1, 1);
}

/* ---------- 输出辅助 ---------- */

int win_console_width(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(h, &csbi))
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return 0;
}

/* 控制字符（tab/换行等）替换为空格 */
static void clean_ctl(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20)
            *s = ' ';
}

void win_emit_name(SB *out, const char *name, int avail) {
    if (!name || avail <= 0)
        return;
    char *nm = strdup(name);
    clean_ctl(nm);
    size_t len = strlen(nm);
    if ((int)len <= avail) {
        sb_adds(out, nm);
        free(nm);
        return;
    }
    int keep = avail - 1; /* 留一个字符给省略号 */
    if (keep < 1) {
        free(nm);
        sb_adds(out, "\xE2\x80\xA6");
        return;
    }
    /* 回退到 UTF-8 字符边界，避免截断多字节字符 */
    while (keep > 0 && ((unsigned char)nm[keep] & 0xC0) == 0x80)
        keep--;
    nm[keep] = '\0';
    sb_adds(out, nm);
    free(nm);
    sb_adds(out, "\xE2\x80\xA6");
}
