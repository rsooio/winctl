#ifndef WJB_WIN_H
#define WJB_WIN_H

#include <windows.h>
#include "json.h"

/* 枚举顶层窗口句柄（默认仅可见；all=1 包含不可见），写入 hwnds，返回数量（上限 max） */
int win_enum_top(int all, unsigned long long *hwnds, int max);
/* state: normal|maximized|minimized，成功返回 1 */
int win_set_window_state(const char *hwnd_str, const char *state);
/* 关闭窗口：PostMessage(WM_CLOSE)，异步投递不等待应用处理（避免确认框阻塞） */
int win_close_window(const char *hwnd_str);
/* 读窗口状态到 out（normal|maximized|minimized），返回 1 */
int win_get_window_state(const char *hwnd_str, char *out, size_t cap);
/* 读窗口所属进程 ID，返回 1 */
int win_get_pid(const char *hwnd_str, unsigned long *pid_out);
/* 剪贴板（内部使用：文本输入回退路径） */
int win_set_clipboard(const char *utf8);
void win_tap_combo(int vk1, int vk2);

/* 终端宽度：stdout 非控制台（管道/重定向）返回 0 */
int win_console_width(void);
/* 输出 name 列：控制字符清洗 + 可用宽度不足时截断追加省略号（…）；avail<=0 不输出 */
void win_emit_name(SB *out, const char *name, int avail);

#endif
