#ifndef WJB_UIA_H
#define WJB_UIA_H

#include <windows.h>
#include "json.h"

typedef struct Uia Uia;

struct Uia {
    void *uia;                 /* IUIAutomation * */
    void *root;                /* IUIAutomationElement *，hwnd 对应元素（引用持有） */
    void *walker;              /* IUIAutomationTreeWalker *，ControlView */
    void *cache;               /* IUIAutomationCacheRequest * */
    HWND hwnd;
    int left, top;             /* 根窗口屏幕偏移，rect 换算用 */
};

/* 初始化 UIA 并绑定窗口句柄（任意元素句柄），失败返回 0 */
int uia_init(Uia *u, const char *hwnd_str);
void uia_free(Uia *u);

/* list 命令：XPath 集合求值（从根查找，输出全部匹配节点）。
 * xpath 为 NULL/空串/"/" = 整树；json=1 输出 JSON 数组内容，否则 xpath/name/hwnd 对齐列。
 * 失败返回 0，err 写入错误描述 */
int uia_list(Uia *u, const char *xpath, int json, char *err, size_t errlen, SB *out);

/* list 全局模式：虚拟根 = 所有顶层窗口（all=1 包含不可见）。
 * xpath 为 NULL/空串 = 全量；否则从虚拟根求值，第一段匹配顶层窗口 */
int uia_list_all(int all, const char *xpath, int json, char *err, size_t errlen,
                 SB *out);

/* XPath 定位第一个匹配（引用 +1，调用方 Release），xpath 为 NULL/空 = 根元素 */
void *uia_find(Uia *u, const char *xpath);
/* 释放 uia_find 返回的元素 */
void uia_release_element(void *el);

/* 输出元素属性到 out：json=1 输出 JSON 对象，否则 kv 行式（key: value）。
 * xpath 为 NULL 时输出根元素的 "/Type" 路径；根元素 name 用 GetWindowTextW 读取 */
int uia_element_prop(Uia *u, void *el, const char *xpath, int json, SB *out);

/* 元素操作，返回 1 成功 0 失败 */
int uia_click(Uia *u, void *el, const char *action, const char *button, int foreground);
int uia_set_value(Uia *u, void *el, const char *value);
int uia_focus(Uia *u, void *el);
/* 返回 malloc 的 UTF-8 字符串，调用方 free；无值返回 NULL */
char *uia_get_value(Uia *u, void *el);
/* 元素窗口句柄：有 NativeWindowHandle 用之，否则根 hwnd（窗口操作用） */
unsigned long long uia_element_hwnd(Uia *u, void *elv);
/* 元素自身 NativeWindowHandle（实时属性），无独立句柄返回 0 */
unsigned long long uia_element_own_hwnd(void *elv);

#endif
