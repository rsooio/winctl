#include "uia.h"
#include "win.h"

#include <uiautomationclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/* ---------- UTF-8 / UTF-16 ---------- */

static wchar_t *utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *w_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static BSTR utf8_to_bstr(const char *s) {
    wchar_t *w = utf8_to_w(s);
    if (!w) return NULL;
    BSTR b = SysAllocString(w);
    free(w);
    return b;
}

/* ---------- ControlType 映射 ---------- */

static const struct { int id; const char *name; } TYPE_NAMES[] = {
    {50000, "Button"},      {50001, "Calendar"},    {50002, "CheckBox"},
    {50003, "ComboBox"},    {50004, "Edit"},        {50005, "Hyperlink"},
    {50006, "Image"},       {50007, "ListItem"},    {50008, "List"},
    {50009, "Menu"},        {50010, "MenuBar"},     {50011, "MenuItem"},
    {50012, "ProgressBar"}, {50013, "RadioButton"}, {50014, "ScrollBar"},
    {50015, "Slider"},      {50016, "Spinner"},     {50017, "StatusBar"},
    {50018, "Tab"},         {50019, "TabItem"},     {50020, "Text"},
    {50021, "ToolBar"},     {50022, "ToolTip"},     {50023, "Tree"},
    {50024, "TreeItem"},    {50025, "Custom"},      {50026, "Group"},
    {50027, "Thumb"},       {50028, "DataGrid"},    {50029, "DataItem"},
    {50030, "Document"},    {50031, "SplitButton"}, {50032, "Window"},
    {50033, "Pane"},        {50034, "Header"},      {50035, "HeaderItem"},
    {50036, "Table"},       {50037, "TitleBar"},    {50038, "Separator"},
    {50039, "AppBar"},
};

static const char *type_name(int id) {
    for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; i++)
        if (TYPE_NAMES[i].id == id)
            return TYPE_NAMES[i].name;
    return NULL;
}

static int name_type(const char *name) {
    for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; i++)
        if (strcmp(TYPE_NAMES[i].name, name) == 0)
            return TYPE_NAMES[i].id;
    return -1;
}

/* ---------- XPath 解析（1.0 子集） ----------
 * 支持：/A/B/C、A/B/C、//A、/A//B、*、[n] 位置谓词、
 *       [@Name='x'] [@Name="x"] [@Type='x'] [@Id='x'] [@Class='x']、
 *       = != *= 包含 ^= 前缀 $= 后缀、and */

enum { XP_ATTR_NAME, XP_ATTR_TYPE, XP_ATTR_ID, XP_ATTR_CLASS, XP_ATTR_PID };
enum { XP_OP_EQ, XP_OP_NE, XP_OP_CONTAINS, XP_OP_PREFIX, XP_OP_SUFFIX };

typedef struct XPath XPath;

typedef struct {
    int attr;
    int op;
    char val[256];
    int is_path;        /* 1 = 子路径谓词（存在性，sub 有效） */
    XPath *sub;         /* 子路径谓词 */
} XPred;

typedef struct {
    char name[32];      /* 类型名或 "*" */
    int pos;            /* [n]，0 表示无 */
    XPred preds[4];
    int npreds;
    int descendant;     /* 本段前为 // */
} XStep;

struct XPath { XStep steps[32]; int n; };

static int el_matches(IUIAutomationTreeWalker *walker, IUIAutomationElement *el,
                     const XStep *st);

static int has_subpath(IUIAutomationTreeWalker *walker, IUIAutomationElement *root,
                      const XPath *sub);

static int attr_id(const char *s) {
    if (_stricmp(s, "name") == 0) return XP_ATTR_NAME;
    if (_stricmp(s, "type") == 0) return XP_ATTR_TYPE;
    if (_stricmp(s, "id") == 0) return XP_ATTR_ID;
    if (_stricmp(s, "class") == 0) return XP_ATTR_CLASS;
    if (_stricmp(s, "pid") == 0) return XP_ATTR_PID;
    return -1;
}

static int parse_xpath(const char *expr, XPath *xp, char *err, size_t errlen);

/* 解析子路径谓词内容（[ 内、前缀已剥除）：扫描配对的 ]，递归 parse_xpath。
 * desc 覆盖子路径首段的 descendant（./=0、无前缀=1 等）。成功后 *pp 指向 ] 之后 */
static int parse_subpath(const char **pp, XPred *pr, int desc, char *err, size_t errlen) {
    const char *s = *pp;
    int depth = 0;
    while (*s) {
        if (*s == '[')
            depth++;
        else if (*s == ']') {
            if (depth == 0)
                break;
            depth--;
        }
        s++;
    }
    if (*s != ']') {
        snprintf(err, errlen, "子路径谓词缺少 ]");
        return 0;
    }
    size_t len = (size_t)(s - *pp);
    if (len >= 512) {
        snprintf(err, errlen, "子路径过长");
        return 0;
    }
    char buf[512];
    memcpy(buf, *pp, len);
    buf[len] = '\0';
    XPath *sub = malloc(sizeof(XPath));
    if (!sub) {
        snprintf(err, errlen, "内存不足");
        return 0;
    }
    if (!parse_xpath(buf, sub, err, errlen)) {
        free(sub);
        return 0;
    }
    if (sub->n > 0)
        sub->steps[0].descendant = desc;
    pr->sub = sub;
    pr->is_path = 1;
    *pp = s; /* 停在配对的 ] 处，由调用方统一消费 */
    return 1;
}

static int parse_xpath(const char *expr, XPath *xp, char *err, size_t errlen) {
    memset(xp, 0, sizeof *xp);
    const char *p = expr;
    int first = 1;

    for (;;) {
        while (*p == ' ')
            p++;
        int descendant = 0;
        if (p[0] == '/' && p[1] == '/') {
            descendant = 1;
            p += 2;
        } else if (*p == '/') {
            p++;
        } else if (!first) {
            break; /* 结束 */
        }
        if (first && !descendant && !*p)
            break;
        if (xp->n >= 32) { snprintf(err, errlen, "路径过长"); return 0; }

        XStep *st = &xp->steps[xp->n];
        st->descendant = descendant;

        /* 节点测试：类型名或 * */
        size_t i = 0;
        while (p[i] && p[i] != '/' && p[i] != '[' && p[i] != ' ' && i < sizeof st->name - 1) {
            st->name[i] = p[i];
            i++;
        }
        if (i == 0) { snprintf(err, errlen, "非法路径段"); return 0; }
        st->name[i] = '\0';
        p += i;

        /* 谓词 */
        while (*p == '[') {
            p++;
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                /* 位置谓词 [n]，不产生属性条件 */
                int v = 0;
                while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
                st->pos = v;
            } else if (*p == '/' || (*p == '.' && p[1] == '/')) {
                /* 子路径谓词：[/X] 严格子级，[//X] 任意深度，[./X] 严格子级，[.//X] 任意深度 */
                if (st->npreds >= 4) { snprintf(err, errlen, "谓词过多"); return 0; }
                XPred *pr = &st->preds[st->npreds];
                memset(pr, 0, sizeof *pr);
                int desc;
                const char *sp = p;
                if (p[0] == '/' && p[1] == '/') { desc = 1; sp = p + 2; }
                else if (p[0] == '/') { desc = 0; sp = p + 1; }
                else if (p[1] == '/' && p[2] == '/') { desc = 1; sp = p + 3; } /* .// */
                else { desc = 0; sp = p + 2; } /* ./ */
                if (!parse_subpath(&sp, pr, desc, err, errlen))
                    return 0;
                st->npreds++;
                p = sp;
            } else if (*p == '@' || isalpha((unsigned char)*p)) {
                int is_subpath = 0;
                /* 无前缀子路径：名字后跟 [ 或 /（如 [Text[@Name='x']]）→ 子路径，任意深度 */
                if (*p != '@') {
                    const char *q = p;
                    while (isalnum((unsigned char)*q) || *q == '_' || *q == '-')
                        q++;
                    while (*q == ' ')
                        q++;
                    if (*q == '[' || *q == '/') {
                        if (st->npreds >= 4) { snprintf(err, errlen, "谓词过多"); return 0; }
                        XPred *pr = &st->preds[st->npreds];
                        memset(pr, 0, sizeof *pr);
                        if (!parse_subpath(&p, pr, 1, err, errlen))
                            return 0;
                        st->npreds++;
                        is_subpath = 1;
                    }
                }
                /* 属性谓词；@ 可选（XPath 兼容，子集内无歧义）；支持 and 连接多个（同一 [ ] 内） */
                if (is_subpath)
                    ; /* 子路径已消费，[] 由公共代码统一收尾 */
                else
                for (;;) {
                    if (st->npreds >= 4) { snprintf(err, errlen, "谓词过多"); return 0; }
                    XPred *pr = &st->preds[st->npreds];
                    memset(pr, 0, sizeof *pr);
                    if (*p == '@')
                        p++;
                    char an[16];
                    size_t ai = 0;
                    while (*p && *p != '=' && *p != '!' && *p != '*' && *p != '^' &&
                           *p != '$' && *p != ']' && *p != ' ' && ai < sizeof an - 1)
                        an[ai++] = *p++;
                    an[ai] = '\0';
                    pr->attr = attr_id(an);
                    if (pr->attr < 0) { snprintf(err, errlen, "未知属性 @%s", an); return 0; }
                    while (*p == ' ') p++;
                    if (p[0] == '!' && p[1] == '=') { pr->op = XP_OP_NE; p += 2; }
                    else if (p[0] == '*' && p[1] == '=') { pr->op = XP_OP_CONTAINS; p += 2; }
                    else if (p[0] == '^' && p[1] == '=') { pr->op = XP_OP_PREFIX; p += 2; }
                    else if (p[0] == '$' && p[1] == '=') { pr->op = XP_OP_SUFFIX; p += 2; }
                    else if (*p == '=') { pr->op = XP_OP_EQ; p++; }
                    else { snprintf(err, errlen, "谓词缺少比较符"); return 0; }
                    while (*p == ' ') p++;
                    char quote = *p;
                    if (quote == '\'' || quote == '"') {
                        /* 引号值 */
                        p++;
                        size_t vi = 0;
                        while (*p && *p != quote && vi < sizeof pr->val - 1) {
                            if (*p == '\\' && p[1]) p++;
                            pr->val[vi++] = *p++;
                        }
                        pr->val[vi] = '\0';
                        if (*p != quote) { snprintf(err, errlen, "谓词值引号未闭合"); return 0; }
                        p++;
                    } else {
                        /* 无引号值：读到空格或 ] */
                        size_t vi = 0;
                        while (*p && *p != ' ' && *p != ']' && vi < sizeof pr->val - 1)
                            pr->val[vi++] = *p++;
                        pr->val[vi] = '\0';
                        if (vi == 0) { snprintf(err, errlen, "谓词值缺失"); return 0; }
                    }
                    st->npreds++;
                    /* and 连接下一个谓词 */
                    while (*p == ' ') p++;
                    if (strncmp(p, "and", 3) == 0 &&
                        (p[3] == ' ' || p[3] == '\t' || p[3] == '\0')) {
                        p += 3;
                        while (*p == ' ') p++;
                        if (*p != '@' && !isalpha((unsigned char)*p)) {
                            snprintf(err, errlen, "and 后需属性谓词");
                            return 0;
                        }
                        continue;
                    }
                    break;
                }
            } else {
                snprintf(err, errlen, "不支持的谓词"); return 0;
            }
            while (*p == ' ') p++;
            if (*p != ']') { snprintf(err, errlen, "谓词缺少 ]"); return 0; }
            p++;
        }

        xp->n++;
        first = 0;
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (*p != '/') { snprintf(err, errlen, "路径段之间需 /"); return 0; }
    }
    return 1;
}

/* ---------- 谓词匹配 ---------- */

static int str_match(const char *s, const char *val, int op) {
    switch (op) {
    case XP_OP_EQ: return strcmp(s, val) == 0;
    case XP_OP_NE: return strcmp(s, val) != 0;
    case XP_OP_CONTAINS: return strstr(s, val) != NULL;
    case XP_OP_PREFIX: return strncmp(s, val, strlen(val)) == 0;
    case XP_OP_SUFFIX: {
        size_t ls = strlen(s), lv = strlen(val);
        return ls >= lv && strcmp(s + ls - lv, val) == 0;
    }
    }
    return 0;
}

/* 属性谓词求值；s 为 UTF-8 属性值，可为 NULL（元素无该属性） */
static int pred_value_ok(const char *s, const XPred *pr) {
    if (pr->attr == XP_ATTR_TYPE) {
        /* 类型谓词只按 = / != 处理 */
        int eq = s && strcmp(s, pr->val) == 0;
        return pr->op == XP_OP_NE ? !eq : eq;
    }
    if (!s)
        return pr->op == XP_OP_NE; /* 无值 ≠ x 成立 */
    return str_match(s, pr->val, pr->op);
}

/* 单步匹配（缓存属性，集合求值用：遍历元素均带缓存，零 UIA 跨进程调用） */
static int el_matches_cached(IUIAutomationElement *el, const XStep *st) {
    if (st->name[0] && strcmp(st->name, "*") != 0) {
        CONTROLTYPEID ct = 0;
        el->lpVtbl->get_CachedControlType(el, &ct);
        if (name_type(st->name) != ct)
            return 0;
    }
    for (int i = 0; i < st->npreds; i++) {
        const XPred *pr = &st->preds[i];
        if (pr->is_path)
            continue; /* 子路径已由 seg_matches（顶层窗口段）验证 */
        if (pr->attr == XP_ATTR_PID)
            continue; /* @Pid 已由 seg_matches（顶层窗口段）验证，这里跳过避免二次求值失败 */
        if (pr->attr == XP_ATTR_TYPE) {
            CONTROLTYPEID ct = 0;
            el->lpVtbl->get_CachedControlType(el, &ct);
            if (!pred_value_ok(type_name(ct), pr))
                return 0;
        } else {
            BSTR b = NULL;
            if (pr->attr == XP_ATTR_NAME)
                el->lpVtbl->get_CachedName(el, &b);
            else if (pr->attr == XP_ATTR_ID)
                el->lpVtbl->get_CachedAutomationId(el, &b);
            else
                el->lpVtbl->get_CachedClassName(el, &b);
            if (b) {
                char *s = w_to_utf8(b);
                int ok = pred_value_ok(s, pr);
                free(s);
                SysFreeString(b);
                if (!ok)
                    return 0;
            } else if (!pred_value_ok(NULL, pr)) {
                return 0;
            }
        }
    }
    return 1;
}

/* 单步匹配（实时属性，定位族用；不含位置谓词，位置由调用方按兄弟序处理） */
static int el_matches(IUIAutomationTreeWalker *walker, IUIAutomationElement *el,
                     const XStep *st) {
    if (st->name[0] && strcmp(st->name, "*") != 0) {
        CONTROLTYPEID ct = 0;
        el->lpVtbl->get_CurrentControlType(el, &ct);
        if (name_type(st->name) != ct)
            return 0;
    }
    for (int i = 0; i < st->npreds; i++) {
        const XPred *pr = &st->preds[i];
        if (pr->is_path) {
            /* 子路径谓词：walker 可用（子路径求值链）时递归检查，否则不支持 */
            if (!walker)
                return 0;
            if (!has_subpath(walker, el, pr->sub))
                return 0;
            continue;
        }
        if (pr->attr == XP_ATTR_PID)
            return 0; /* @Pid 仅 list 全局模式（顶层窗口段）支持 */
        if (pr->attr == XP_ATTR_TYPE) {
            CONTROLTYPEID ct = 0;
            el->lpVtbl->get_CurrentControlType(el, &ct);
            if (!pred_value_ok(type_name(ct), pr))
                return 0;
        } else {
            /* 属性谓词用专用 get_CurrentXxx 读取，GetCurrentPropertyValue 在部分
             * provider（XAML）上对 Name 等返回空 */
            BSTR b = NULL;
            if (pr->attr == XP_ATTR_NAME)
                el->lpVtbl->get_CurrentName(el, &b);
            else if (pr->attr == XP_ATTR_ID)
                el->lpVtbl->get_CurrentAutomationId(el, &b);
            else
                el->lpVtbl->get_CurrentClassName(el, &b);
            if (b) {
                char *s = w_to_utf8(b);
                int ok = pred_value_ok(s, pr);
                free(s);
                SysFreeString(b);
                if (!ok)
                    return 0;
            } else if (!pred_value_ok(NULL, pr)) {
                return 0;
            }
        }
    }
    return 1;
}

void uia_release_element(void *el) {
    if (el)
        ((IUIAutomationElement *)el)->lpVtbl->Release(el);
}

/* ---------- JSON 输出 ---------- */

static void out_element_json(SB *out, const char *name, const char *value,
                             int isenabled, int isinvoke, int isscroll,
                             unsigned long long ehwnd, int l, int t, int r, int b,
                             const char *type, const char *xpath) {
    sb_adds(out, "{\"hwnd\":");
    if (ehwnd) {
        char hbuf[32];
        snprintf(hbuf, sizeof hbuf, "\"0x%llx\"", ehwnd);
        sb_adds(out, hbuf);
    } else {
        sb_adds(out, "null");
    }
    sb_adds(out, ",\"enabled\":");
    sb_json_bool(out, isenabled);
    sb_adds(out, ",\"invokable\":");
    sb_json_bool(out, isinvoke);
    sb_adds(out, ",\"scrollable\":");
    sb_json_bool(out, isscroll);
    sb_adds(out, ",\"name\":");
    sb_json_str_or_null(out, name);
    sb_adds(out, ",\"value\":");
    sb_json_str_or_null(out, value);
    sb_adds(out, ",\"rect\":{\"left\":");
    sb_json_int(out, l);
    sb_adds(out, ",\"top\":");
    sb_json_int(out, t);
    sb_adds(out, ",\"right\":");
    sb_json_int(out, r);
    sb_adds(out, ",\"bottom\":");
    sb_json_int(out, b);
    sb_adds(out, "},\"type\":");
    sb_json_str(out, type);
    sb_adds(out, ",\"xpath\":");
    sb_json_str(out, xpath);
    sb_adds(out, "}");
}

/* 元素自身 NativeWindowHandle（缓存属性）；无独立句柄返回 0 */
static unsigned long long cached_own_hwnd(IUIAutomationElement *el) {
    VARIANT v;
    unsigned long long h = 0;
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCachedPropertyValue(el, UIA_NativeWindowHandlePropertyId, &v))) {
        if (V_VT(&v) == VT_I4 && V_I4(&v) != 0)
            h = (unsigned long long)(ULONG_PTR)(INT_PTR)V_I4(&v);
        else if (V_VT(&v) == VT_UI4 && V_UI4(&v) != 0)
            h = (unsigned long long)(ULONG_PTR)V_UI4(&v);
    }
    VariantClear(&v);
    return h;
}

/* ---------- list：集合求值 ---------- */

/* 祖先段：元素 + 父下同类型兄弟序；top 标记顶层窗口段（全局模式），hwnd 为其窗口句柄 */
typedef struct {
    IUIAutomationElement *el;
    int cnt;
    int top;
    unsigned long long hwnd;
} Seg;

/* 子路径谓词求值：from 子树中查找匹配 st 的元素（手动 ControlView 遍历，
 * 与树生成同视图同顺序）。subtree=1 时任意深度（文档序），pos 按每父计数 */
static IUIAutomationElement *sub_has_match(IUIAutomationTreeWalker *walker,
                                           IUIAutomationElement *from,
                                           const XStep *st, int subtree) {
    int n = 0;
    IUIAutomationElement *child = NULL;
    walker->lpVtbl->GetFirstChildElement(walker, from, &child);
    while (child) {
        if (el_matches(walker, child, st)) {
            n++;
            if (st->pos == 0 || n == st->pos)
                return child; /* 引用来自 GetFirstChildElement，调用方 Release */
        }
        if (subtree) {
            IUIAutomationElement *hit = sub_has_match(walker, child, st, 1);
            if (hit) {
                child->lpVtbl->Release(child);
                return hit;
            }
        }
        IUIAutomationElement *next = NULL;
        walker->lpVtbl->GetNextSiblingElement(walker, child, &next);
        child->lpVtbl->Release(child);
        child = next;
    }
    return NULL;
}

/* 存在性检查：root 子树中存在匹配子路径 sub 的元素 */
static int has_subpath(IUIAutomationTreeWalker *walker, IUIAutomationElement *root,
                       const XPath *sub) {
    if (!sub || sub->n == 0)
        return 0;
    IUIAutomationElement *cur = root;
    cur->lpVtbl->AddRef(cur);
    for (int i = 0; i < sub->n; i++) {
        const XStep *st = &sub->steps[i];
        IUIAutomationElement *next = sub_has_match(walker, cur, st, st->descendant);
        if (!next) {
            cur->lpVtbl->Release(cur);
            return 0;
        }
        if (i > 0 || cur != root)
            cur->lpVtbl->Release(cur);
        cur = next;
    }
    cur->lpVtbl->Release(cur);
    return 1;
}

static int seg_matches(IUIAutomationTreeWalker *walker, const Seg *seg, const XStep *st) {
    if (st->pos && seg->cnt != st->pos)
        return 0;
    for (int i = 0; i < st->npreds; i++) {
        const XPred *pr = &st->preds[i];
        if (pr->is_path) {
            /* 子路径谓词：子树存在匹配（仅顶层窗口段） */
            if (!seg->top)
                return 0;
            if (!has_subpath(walker, seg->el, pr->sub))
                return 0;
            continue;
        }
        if (pr->attr == XP_ATTR_PID) {
            /* @Pid：顶层窗口段的进程 ID（十进制字符串比较） */
            if (!seg->top) {
                if (pr->op != XP_OP_NE)
                    return 0;
                continue;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId((HWND)(ULONG_PTR)seg->hwnd, &pid);
            char buf[32];
            snprintf(buf, sizeof buf, "%lu", (unsigned long)pid);
            if (!str_match(buf, pr->val, pr->op))
                return 0;
        }
    }
    return el_matches_cached(seg->el, st);
}

typedef struct ListRow_ {
    char *xpath;
    char *name;
    unsigned long long hwnd;
} ListRow;

typedef struct ListCtx_ {
    SB *out;
    int json;
    int first;          /* JSON 数组元素分隔 */
    ListRow *rows;      /* 列模式收集 */
    int n, cap;
} ListCtx;

/* ---------- 步进求值（list 核心） ----------
 * 与定位（uia_find 的 step_walk/step_find）同语义同口径：自顶向下逐段展开，
 * 非 // 段从上一段候选的子级找（FindAllBuildCache(Children) 一次取全子级），
 * // 段从候选子树任意深度找（FindAllBuildCache(7) 宽筛），谓词客户端过滤，
 * [n] 为完整匹配计数。候选 xpath 由步进携带或回溯父链生成 */

/* 候选：匹配元素 + 相对上一段起点的路径（"" = 起点自身） */
typedef struct {
    IUIAutomationElement *el;
    char *path;
} Cand;

typedef struct { Cand *v; int n, cap; } CandList;

static void cand_push(CandList *cl, IUIAutomationElement *el, char *path) {
    if (cl->n >= cl->cap) {
        cl->cap = cl->cap ? cl->cap * 2 : 16;
        cl->v = realloc(cl->v, cl->cap * sizeof *cl->v);
    }
    cl->v[cl->n].el = el;
    cl->v[cl->n].path = path;
    cl->n++;
}

static void cand_free(CandList *cl) {
    for (int i = 0; i < cl->n; i++) {
        cl->v[i].el->lpVtbl->Release(cl->v[i].el);
        free(cl->v[i].path);
    }
    free(cl->v);
    memset(cl, 0, sizeof *cl);
}

static char *path_append(const char *px, const char *tn, int cnt) {
    size_t n = strlen(px) + strlen(tn) + 24;
    char *r = malloc(n);
    if (cnt > 1)
        snprintf(r, n, "%s/%s[%d]", px, tn, cnt);
    else
        snprintf(r, n, "%s/%s", px, tn);
    return r;
}

/* el 在父下的同类型兄弟序（从 1 起）；未找到返回 0（树已变化） */
static int sibling_index(IUIAutomation *uia, IUIAutomationTreeWalker *walker,
                         IUIAutomationCacheRequest *cache,
                         IUIAutomationElement *parent, IUIAutomationElement *el,
                         CONTROLTYPEID ct) {
    int idx = 0;
    IUIAutomationElement *child = NULL;
    walker->lpVtbl->GetFirstChildElementBuildCache(walker, parent, cache, &child);
    while (child) {
        CONTROLTYPEID cct = 0;
        child->lpVtbl->get_CachedControlType(child, &cct);
        if (cct == ct)
            idx++;
        BOOL same = FALSE;
        uia->lpVtbl->CompareElements(uia, el, child, &same);
        IUIAutomationElement *next = NULL;
        if (!same)
            walker->lpVtbl->GetNextSiblingElementBuildCache(walker, child, cache, &next);
        child->lpVtbl->Release(child);
        if (same)
            return idx;
        child = next;
    }
    return 0;
}

/* 生成 el 相对 from（不含 from 自身）的路径：回溯父链，每段类型 + 同类型兄弟序。
 * 失败（provider 不支持 GetParentElement / 树变化）返回 NULL */
static char *backtrack_path(IUIAutomation *uia, IUIAutomationTreeWalker *walker,
                            IUIAutomationCacheRequest *cache,
                            IUIAutomationElement *from, IUIAutomationElement *el) {
    IUIAutomationElement *ups[64]; /* ups[0]=el 的父 … ups[k-1]=from 的直接子级 */
    int k = 0;
    IUIAutomationElement *cur = el;
    for (;;) {
        IUIAutomationElement *parent = NULL;
        walker->lpVtbl->GetParentElementBuildCache(walker, cur, cache, &parent);
        if (!parent)
            goto fail;
        BOOL same = FALSE;
        uia->lpVtbl->CompareElements(uia, from, parent, &same);
        if (same) {
            parent->lpVtbl->Release(parent);
            break;
        }
        if (k >= (int)(sizeof ups / sizeof ups[0])) {
            parent->lpVtbl->Release(parent);
            goto fail;
        }
        ups[k++] = parent;
        cur = parent;
    }
    SB sb;
    sb_init(&sb);
    for (int i = k - 1; i >= 0; i--) {
        IUIAutomationElement *par = (i == k - 1) ? from : ups[i + 1];
        CONTROLTYPEID ct = 0;
        ups[i]->lpVtbl->get_CachedControlType(ups[i], &ct);
        const char *tn = type_name(ct);
        if (!tn) tn = "Custom";
        int idx = sibling_index(uia, walker, cache, par, ups[i], ct);
        char *seg = path_append("", tn, idx > 0 ? idx : 1);
        sb_adds(&sb, seg);
        free(seg);
    }
    {
        IUIAutomationElement *par = k > 0 ? ups[0] : from;
        CONTROLTYPEID ct = 0;
        el->lpVtbl->get_CachedControlType(el, &ct);
        const char *tn = type_name(ct);
        if (!tn) tn = "Custom";
        int idx = sibling_index(uia, walker, cache, par, el, ct);
        char *seg = path_append("", tn, idx > 0 ? idx : 1);
        sb_adds(&sb, seg);
        free(seg);
    }
    for (int i = 0; i < k; i++)
        ups[i]->lpVtbl->Release(ups[i]);
    return sb.buf;
fail:
    for (int i = 0; i < k; i++)
        ups[i]->lpVtbl->Release(ups[i]);
    return NULL;
}

typedef struct {
    ListCtx *ctx;
    Uia *u;
    int top_mode; /* 全局模式：首段 // 时起点自身参与匹配（顶层窗口段） */
} StepCtx;

static void emit_el(ListCtx *ctx, Uia *u, IUIAutomationElement *el, const char *xpath);

/* 单段候选收集：from 下按 subtree 范围收集匹配 st 的候选（文档序）。
 * pos==0 全收，pos>0 只收第 pos 个匹配；include_self=1 时 from 自身先参与
 * 匹配（用 top_seg 语义，@Pid/子路径谓词仅此处生效），其后计数并入 */
static void collect_cands(StepCtx *s, IUIAutomationElement *from, const XStep *st,
                          int subtree, int include_self, const Seg *top_seg,
                          CandList *cl) {
    Uia *u = s->u;
    int seen = 0;
    if (include_self) {
        int ok = top_seg ? seg_matches(u->walker, top_seg, st)
                         : el_matches_cached(from, st);
        if (ok) {
            seen++;
            if (st->pos == 0 || st->pos == 1) {
                from->lpVtbl->AddRef(from); /* 候选持有引用 */
                cand_push(cl, from, strdup(""));
            }
        }
    }

    int type_id = name_type(st->name);
    if (strcmp(st->name, "*") != 0 && type_id < 0)
        return; /* 未知类型名：无候选 */

    IUIAutomation *uia = u->uia;
    IUIAutomationCondition *cond = NULL;
    uia->lpVtbl->get_ControlViewCondition(uia, &cond);
    if (strcmp(st->name, "*") != 0) {
        VARIANT v;
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = type_id;
        IUIAutomationCondition *pc = NULL;
        uia->lpVtbl->CreatePropertyCondition(uia, UIA_ControlTypePropertyId, v, &pc);
        VariantClear(&v);
        if (cond && pc) {
            IUIAutomationCondition *ac = NULL;
            uia->lpVtbl->CreateAndCondition(uia, cond, pc, &ac);
            cond->lpVtbl->Release(cond);
            pc->lpVtbl->Release(pc);
            cond = ac;
        } else if (pc) {
            cond = pc;
        }
    }

    IUIAutomationElementArray *arr = NULL;
    from->lpVtbl->FindAllBuildCache(from, subtree ? 7 : 2, cond, u->cache, &arr);
    if (cond)
        cond->lpVtbl->Release(cond);
    if (!arr)
        return;

    int n = 0;
    arr->lpVtbl->get_Length(arr, &n);
    CONTROLTYPEID last_ct = 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        IUIAutomationElement *el = NULL;
        arr->lpVtbl->GetElement(arr, i, &el);
        if (!el)
            continue;
        CONTROLTYPEID ct = 0;
        el->lpVtbl->get_CachedControlType(el, &ct);
        const char *tn = type_name(ct);
        if (!tn) tn = "Custom";
        if (!subtree) {
            /* 生成路径用：同类型兄弟序（对所有子级计数，与匹配无关） */
            cnt = (ct == last_ct) ? cnt + 1 : 1;
            last_ct = ct;
        }
        int ok = el_matches_cached(el, st);
        if (ok)
            seen++;
        if (ok && (st->pos == 0 || seen == st->pos)) {
            char *path = subtree ? backtrack_path(uia, u->walker, u->cache, from, el)
                                 : path_append("", tn, cnt);
            if (path)
                cand_push(cl, el, path);
            else
                el->lpVtbl->Release(el);
            if (st->pos > 0) {
                arr->lpVtbl->Release(arr);
                return; /* 第 pos 个已命中，后续不再需要 */
            }
        } else {
            el->lpVtbl->Release(el);
        }
    }
    arr->lpVtbl->Release(arr);
}

/* 步进递归：from 已匹配 steps[0..si-1]，px 为其路径；逐段展开收集全部匹配 */
static void steps_walk(StepCtx *s, IUIAutomationElement *from, const XPath *xp,
                       int si, const char *px, const Seg *top_seg) {
    const XStep *st = &xp->steps[si];
    int last = (si == xp->n - 1);
    CandList cl;
    memset(&cl, 0, sizeof cl);
    collect_cands(s, from, st, st->descendant, s->top_mode && si == 0, top_seg, &cl);
    for (int i = 0; i < cl.n; i++) {
        size_t n = strlen(px) + strlen(cl.v[i].path) + 1;
        char *full = malloc(n);
        snprintf(full, n, "%s%s", px, cl.v[i].path);
        if (last)
            emit_el(s->ctx, s->u, cl.v[i].el, full);
        else
            steps_walk(s, cl.v[i].el, xp, si + 1, full, NULL);
        free(full);
    }
    cand_free(&cl);
}

/* 全量输出特判：双斜杠星号（无谓词）等价整树 dump，走整树递归避免逐候选回溯 */
static int is_full_dump(const XPath *xp) {
    return xp->n == 1 && xp->steps[0].descendant &&
           strcmp(xp->steps[0].name, "*") == 0 &&
           xp->steps[0].pos == 0 && xp->steps[0].npreds == 0;
}

static void row_push(ListCtx *ctx, const char *xpath, const char *name,
                     unsigned long long hwnd) {
    if (ctx->n >= ctx->cap) {
        ctx->cap = ctx->cap ? ctx->cap * 2 : 128;
        ctx->rows = realloc(ctx->rows, ctx->cap * sizeof *ctx->rows);
    }
    ListRow *r = &ctx->rows[ctx->n++];
    r->xpath = strdup(xpath);
    r->name = name && name[0] ? strdup(name) : NULL;
    r->hwnd = hwnd;
}

static void emit_node(ListCtx *ctx, const char *xpath,
                      const char *name, int enabled, int isinvoke, int isscroll,
                      unsigned long long hwnd, int l, int t, int r, int b,
                      const char *type) {
    if (ctx->json) {
        if (!ctx->first) sb_adds(ctx->out, ",");
        ctx->first = 0;
        out_element_json(ctx->out, name, NULL, enabled, isinvoke, isscroll,
                         hwnd, l, t, r, b, type, xpath);
    } else {
        row_push(ctx, xpath, name, hwnd);
    }
}

static void clean_text(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20)
            *s = ' ';
}

/* 列模式：hwnd/xpath/name 对齐列；终端下 name 超宽截断省略号，xpath 永远完整 */
static void list_emit_cols(ListCtx *ctx, SB *out) {
    int maxh = 0, maxx = 0;
    char hbuf[32];
    for (int i = 0; i < ctx->n; i++) {
        int lh = 0;
        if (ctx->rows[i].hwnd) {
            snprintf(hbuf, sizeof hbuf, "0x%llx", ctx->rows[i].hwnd);
            lh = (int)strlen(hbuf);
        }
        int lx = (int)strlen(ctx->rows[i].xpath);
        if (lh > maxh) maxh = lh;
        if (lx > maxx) maxx = lx;
    }
    int width = win_console_width();
    /* 剩余给 name 的宽度：终端宽度 - hwnd 列 - xpath 列 - 两个分隔符(4) */
    int avail = width ? width - maxh - maxx - 4 : 0;
    for (int i = 0; i < ctx->n; i++) {
        if (i) sb_adds(out, "\n");
        char buf[512];
        if (ctx->rows[i].hwnd)
            snprintf(hbuf, sizeof hbuf, "0x%llx", ctx->rows[i].hwnd);
        else
            hbuf[0] = '\0';
        snprintf(buf, sizeof buf, "%-*s  ", maxh, hbuf);
        sb_adds(out, buf);
        snprintf(buf, sizeof buf, "%-*s  ", maxx, ctx->rows[i].xpath);
        sb_adds(out, buf);
        if (ctx->rows[i].name)
            win_emit_name(out, ctx->rows[i].name, width ? avail : INT_MAX);
    }
    for (int i = 0; i < ctx->n; i++) {
        free(ctx->rows[i].xpath);
        free(ctx->rows[i].name);
    }
    free(ctx->rows);
    ctx->rows = NULL;
    ctx->n = ctx->cap = 0;
}

/* 递归遍历：parent 为已带缓存的元素，px 为其 xpath 前缀 */
/* 输出单个匹配元素：读缓存属性 + emit_node */
static void emit_el(ListCtx *ctx, Uia *u, IUIAutomationElement *el, const char *xpath) {
    BSTR bname = NULL;
    char *name = NULL;
    if (SUCCEEDED(el->lpVtbl->get_CachedName(el, &bname)) && bname) {
        name = w_to_utf8(bname);
        SysFreeString(bname);
    }
    BOOL enabled = FALSE;
    el->lpVtbl->get_CachedIsEnabled(el, &enabled);

    VARIANT v;
    int isinvoke = 0, isscroll = 0;
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCachedPropertyValue(el, UIA_IsInvokePatternAvailablePropertyId, &v)))
        isinvoke = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
    VariantClear(&v);
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCachedPropertyValue(el, UIA_IsScrollPatternAvailablePropertyId, &v)))
        isscroll = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
    VariantClear(&v);

    unsigned long long own_hwnd = cached_own_hwnd(el);

    CONTROLTYPEID ct = 0;
    el->lpVtbl->get_CachedControlType(el, &ct);
    const char *tn = type_name(ct);
    if (!tn) tn = "Custom";
    RECT rect = {0};
    el->lpVtbl->get_CachedBoundingRectangle(el, &rect);

    emit_node(ctx, xpath, name, enabled ? 1 : 0, isinvoke, isscroll,
              own_hwnd,
              rect.left - u->left, rect.top - u->top,
              rect.right - u->left, rect.bottom - u->top,
              tn);
    free(name);
}

/* 整树输出（无 xpath 查询或双斜杠星号）：每父一次 FindAllBuildCache(Children) 取全子级，
 * 路径由递归携带（px 为 parent 的路径），无逐候选回溯 */
static void dump_tree(StepCtx *s, IUIAutomationElement *parent, const char *px) {
    Uia *u = s->u;
    IUIAutomation *uia = u->uia;
    IUIAutomationCondition *cond = NULL;
    uia->lpVtbl->get_ControlViewCondition(uia, &cond);
    IUIAutomationElementArray *arr = NULL;
    parent->lpVtbl->FindAllBuildCache(parent, 2 /* Children */, cond, u->cache, &arr);
    if (cond)
        cond->lpVtbl->Release(cond);
    if (!arr)
        return;

    int n = 0;
    arr->lpVtbl->get_Length(arr, &n);
    CONTROLTYPEID last_ct = 0;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        IUIAutomationElement *el = NULL;
        arr->lpVtbl->GetElement(arr, i, &el);
        if (!el)
            continue;
        CONTROLTYPEID ct = 0;
        el->lpVtbl->get_CachedControlType(el, &ct);
        const char *tn = type_name(ct);
        if (!tn) tn = "Custom";
        cnt = (ct == last_ct) ? cnt + 1 : 1;
        last_ct = ct;
        char *own = path_append(px, tn, cnt);
        emit_el(s->ctx, u, el, own);
        dump_tree(s, el, own);
        free(own);
        el->lpVtbl->Release(el);
    }
    arr->lpVtbl->Release(arr);
}

/* 计算深度限制：xpath 无 descendant 段时，匹配节点深度 = 段数；否则 0（不限） */

/* 子路径谓词位置检查：allow_first=1（全局模式）时仅第一段（顶层窗口段）允许 */
static int check_subpath_placement(const XPath *xp, int allow_first,
                                   char *err, size_t errlen) {
    for (int i = 0; i < xp->n; i++) {
        for (int j = 0; j < xp->steps[i].npreds; j++) {
            if (xp->steps[i].preds[j].is_path && (!allow_first || i > 0)) {
                snprintf(err, errlen, "子路径谓词仅支持顶层窗口段（list 全局模式第一段）");
                return 0;
            }
        }
    }
    return 1;
}

/* @Pid 位置检查：仅全局模式第一段（顶层窗口段）允许，其余显式报错 */
static int check_pid_placement(const XPath *xp, int allow_first,
                               char *err, size_t errlen) {
    for (int i = 0; i < xp->n; i++) {
        for (int j = 0; j < xp->steps[i].npreds; j++) {
            if (xp->steps[i].preds[j].attr == XP_ATTR_PID && (!allow_first || i > 0)) {
                snprintf(err, errlen, "@Pid 谓词仅支持顶层窗口段（list 全局模式第一段）");
                return 0;
            }
        }
    }
    return 1;
}

/* ---------- 定位（严格唯一） ----------
 * 与 list 同一求值核心（collect_cands）：穷举末段匹配（最多收 4 个用于诊断），
 * 恰好 1 个 → 返回该元素；0 个 → 未找到；多个 → 歧义报错并列出候选路径 */

typedef struct {
    IUIAutomationElement *el; /* 引用持有 */
    char *path;
} Found;

typedef struct { Found *v; int n, cap; } FoundList;

static void found_push(FoundList *fl, IUIAutomationElement *el, char *path) {
    if (fl->n >= fl->cap) {
        fl->cap = fl->cap ? fl->cap * 2 : 4;
        fl->v = realloc(fl->v, fl->cap * sizeof *fl->v);
    }
    el->lpVtbl->AddRef(el); /* 转移引用 */
    fl->v[fl->n].el = el;
    fl->v[fl->n].path = path;
    fl->n++;
}

static void found_free(FoundList *fl) {
    for (int i = 0; i < fl->n; i++) {
        fl->v[i].el->lpVtbl->Release(fl->v[i].el);
        free(fl->v[i].path);
    }
    free(fl->v);
    memset(fl, 0, sizeof *fl);
}

/* 收集末段匹配（与 steps_walk 同求值），达 4 个候选即停（足够诊断） */
static void find_walk(Uia *u, IUIAutomationElement *from, const XPath *xp,
                      int si, const char *px, FoundList *out) {
    const XStep *st = &xp->steps[si];
    int last = (si == xp->n - 1);
    CandList cl;
    memset(&cl, 0, sizeof cl);
    StepCtx s;
    s.ctx = NULL;
    s.u = u;
    s.top_mode = 0;
    collect_cands(&s, from, st, st->descendant, 0, NULL, &cl);
    for (int i = 0; i < cl.n; i++) {
        size_t n = strlen(px) + strlen(cl.v[i].path) + 1;
        char *full = malloc(n);
        snprintf(full, n, "%s%s", px, cl.v[i].path);
        if (last) {
            found_push(out, cl.v[i].el, full); /* full 所有权转移 */
        } else {
            find_walk(u, cl.v[i].el, xp, si + 1, full, out);
            free(full);
        }
        if (out->n >= 4)
            break;
    }
    cand_free(&cl);
}

void *uia_find(Uia *u, const char *xpath, char *err, size_t errlen) {
    IUIAutomationElement *root = u->root;
    if (!xpath || !xpath[0]) {
        /* 根元素 */
        root->lpVtbl->AddRef(root);
        return root;
    }
    XPath xp;
    if (!parse_xpath(xpath, &xp, err, errlen))
        return NULL;
    if (!check_subpath_placement(&xp, 0, err, errlen))
        return NULL;
    if (!check_pid_placement(&xp, 0, err, errlen))
        return NULL;

    FoundList fl;
    memset(&fl, 0, sizeof fl);
    find_walk(u, root, &xp, 0, "", &fl);
    if (fl.n == 0) {
        snprintf(err, errlen, "元素未找到: %s", xpath);
        return NULL;
    }
    if (fl.n > 1) {
        SB sb;
        sb_init(&sb);
        sb_adds(&sb, "匹配多个元素: ");
        for (int i = 0; i < fl.n; i++) {
            if (i)
                sb_adds(&sb, "  ");
            sb_adds(&sb, fl.v[i].path);
        }
        sb_adds(&sb, "（请用 [n] 或属性谓词收窄）");
        snprintf(err, errlen, "%s", sb.buf);
        sb_free(&sb);
        found_free(&fl);
        return NULL;
    }
    IUIAutomationElement *el = fl.v[0].el;
    free(fl.v[0].path);
    free(fl.v);
    return el; /* 引用转移给调用方 */
}


int uia_list(Uia *u, const char *xpath, int json, char *err, size_t errlen, SB *out) {
    XPath xp;
    const XPath *xp_ptr = NULL;
    if (xpath && xpath[0] && strcmp(xpath, "/") != 0) {
        if (!parse_xpath(xpath, &xp, err, errlen))
            return 0;
        if (!check_subpath_placement(&xp, 0, err, errlen))
            return 0;
        if (!check_pid_placement(&xp, 0, err, errlen))
            return 0;
        xp_ptr = &xp;
    }

    ListCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;
    ctx.json = json;
    ctx.first = 1;
    StepCtx s;
    s.ctx = &ctx;
    s.u = u;
    s.top_mode = 0;

    /* 根元素：xpath 相对根向下，根自身不参与匹配（根行 xpath 为 /） */
    IUIAutomationElement *root = u->root;
    if (!xp_ptr) {
        /* 整树输出：根行 + 子树 */
        emit_el(&ctx, u, root, "/");
        dump_tree(&s, root, "");
    } else if (is_full_dump(xp_ptr)) {
        /* 全量通配（双斜杠星号）：根下全部（根自身不参与） */
        dump_tree(&s, root, "");
    } else {
        steps_walk(&s, root, xp_ptr, 0, "", NULL);
    }

    if (!json)
        list_emit_cols(&ctx, out);
    else {
        free(ctx.rows);
        ctx.rows = NULL;
    }
    return 1;
}

/* 顶层窗口 Win32 预筛：第一段（非 descendant）的 @Name/@Class/@Pid 谓词与窗口属性
 * 明确不匹配时排除。保守规则：无标题窗口不排除（UIA Name 可能由 provider 提供，
 * 与 GetWindowText 不同，宁可多遍历不可漏结果）；返回 1 = 排除 */
static int win_prefilter(HWND hwnd, const XStep *st) {
    wchar_t title[512];
    int has_title = GetWindowTextW(hwnd, title, 512) > 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char pid_buf[32];
    snprintf(pid_buf, sizeof pid_buf, "%lu", (unsigned long)pid);
    wchar_t cls_w[256];
    GetClassNameW(hwnd, cls_w, 256);
    char *cls = w_to_utf8(cls_w);

    for (int i = 0; i < st->npreds; i++) {
        const XPred *pr = &st->preds[i];
        if (pr->is_path)
            continue; /* 子路径谓词无法 Win32 预筛，由 seg_matches 求值 */
        int ok = 1;
        if (pr->attr == XP_ATTR_PID) {
            ok = str_match(pid_buf, pr->val, pr->op);
        } else if (pr->attr == XP_ATTR_CLASS) {
            ok = cls && str_match(cls, pr->val, pr->op);
        } else if (pr->attr == XP_ATTR_NAME) {
            if (has_title) {
                char *t = w_to_utf8(title);
                ok = t && str_match(t, pr->val, pr->op);
                free(t);
            }
            /* 无标题：不排除 */
        } else {
            continue; /* @Type/@Id 无法用 Win32 判断 */
        }
        if (!ok) {
            free(cls);
            return 1;
        }
    }
    free(cls);
    return 0;
}

/* 默认排除的进程名：大树 provider 拖慢全量查询（遍历与初始化），--all 时包含 */
static const wchar_t *EXCLUDED_PROCS[] = { L"firefox.exe" };

/* 窗口所属进程是否在默认排除名单 */
static int is_excluded_proc(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return 0;
    wchar_t name[MAX_PATH];
    DWORD sz = MAX_PATH;
    int excluded = 0;
    if (QueryFullProcessImageNameW(h, 0, name, &sz)) {
        wchar_t *base = wcsrchr(name, L'\\');
        base = base ? base + 1 : name;
        for (size_t i = 0; i < sizeof EXCLUDED_PROCS / sizeof EXCLUDED_PROCS[0]; i++) {
            if (_wcsicmp(base, EXCLUDED_PROCS[i]) == 0) {
                excluded = 1;
                break;
            }
        }
    }
    CloseHandle(h);
    return excluded;
}

/* 处理一个顶层窗口元素：可见性过滤（桌面路径）、默认排除进程、Win32 预筛、
 * 同类型计数、根段匹配检查，匹配则遍历子树 */
static void process_root(ListCtx *ctx, Uia *u,
                         IUIAutomationElement *root, unsigned long long rhwnd,
                         const XPath *xp, const XStep *first_st, int first_direct,
                         CONTROLTYPEID *last_ct, int *count) {
    /* Win32 预筛：明确不匹配的窗口跳过 */
    if (first_direct && first_st->npreds > 0 &&
        win_prefilter((HWND)(ULONG_PTR)rhwnd, first_st))
        return;
    RECT rc;
    GetWindowRect((HWND)(ULONG_PTR)rhwnd, &rc);
    u->left = rc.left;
    u->top = rc.top;

    CONTROLTYPEID rct = 0;
    root->lpVtbl->get_CachedControlType(root, &rct);
    const char *rtn = type_name(rct);
    if (!rtn) rtn = "Custom";

    int cnt = (rct == *last_ct) ? (*count + 1) : 1;
    *last_ct = rct;
    *count = cnt;

    /* 顶层窗口段 xpath 不带 [n]（Z 序编号无意义，稳定标识用 hwnd 列） */
    char root_xpath[64];
    snprintf(root_xpath, sizeof root_xpath, "/%s", rtn);

    StepCtx s;
    s.ctx = ctx;
    s.u = u;
    s.top_mode = 1;

    if (!xp) {
        /* 全量：窗口行 + 整树 */
        emit_el(ctx, u, root, root_xpath);
        dump_tree(&s, root, root_xpath);
        return;
    }
    if (first_direct) {
        /* 非 // 首段：根段（顶层窗口）严格匹配，其后从窗口子级起步进 */
        Seg root_seg = {root, cnt, 1, rhwnd};
        if (!seg_matches(u->walker, &root_seg, first_st))
            return;
        if (xp->n == 1)
            emit_el(ctx, u, root, root_xpath);
        else
            steps_walk(&s, root, xp, 1, root_xpath, NULL);
    } else {
        /* // 首段：顶层窗口自身参与（descendant-or-self 的 self），全量通配特判走整树 */
        if (is_full_dump(xp)) {
            emit_el(ctx, u, root, root_xpath);
            dump_tree(&s, root, root_xpath);
        } else {
            Seg root_seg = {root, cnt, 1, rhwnd};
            steps_walk(&s, root, xp, 0, root_xpath, &root_seg);
        }
    }
}

/* list 全局模式：虚拟根 = 所有顶层窗口（默认仅可见）。
 * xpath 为 NULL/空串 = 全量；否则 xpath 从虚拟根求值，第一段匹配顶层窗口。
 * 第一段非 descendant 时：Win32 预筛 + 根段匹配检查，不匹配窗口跳过整树遍历 */
int uia_list_all(int all, const char *xpath, int json, char *err, size_t errlen,
                 SB *out) {
    XPath xp;
    const XPath *xp_ptr = NULL;
    if (xpath && xpath[0] && strcmp(xpath, "/") != 0) {
        if (!parse_xpath(xpath, &xp, err, errlen))
            return 0;
        if (!check_subpath_placement(&xp, 1, err, errlen))
            return 0;
        if (!check_pid_placement(&xp, 1, err, errlen))
            return 0;
        xp_ptr = &xp;
    }
    if (xp_ptr && xp_ptr->n > 0 && xp_ptr->steps[0].pos > 0) {
        /* 顶层窗口序号依赖 Z 序，不稳定无意义；仅支持属性谓词定位 */
        snprintf(err, errlen, "顶层窗口不支持位置谓词 [n]，请用属性谓词（@Name/@Pid/@Class）");
        return 0;
    }
    const XStep *first_st = (xp_ptr && xp_ptr->n > 0) ? &xp_ptr->steps[0] : NULL;
    int first_direct = first_st && !first_st->descendant;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IUIAutomation *uia = NULL;
    if (FAILED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IUIAutomation, (void **)&uia))) {
        CoUninitialize();
        snprintf(err, errlen, "UIA 初始化失败");
        return 0;
    }
    IUIAutomationCacheRequest *cache = NULL;
    if (FAILED(uia->lpVtbl->CreateCacheRequest(uia, &cache))) {
        uia->lpVtbl->Release(uia);
        CoUninitialize();
        snprintf(err, errlen, "UIA 初始化失败");
        return 0;
    }
    cache->lpVtbl->AddProperty(cache, UIA_NamePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_ControlTypePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_BoundingRectanglePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsEnabledPropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_NativeWindowHandlePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsInvokePatternAvailablePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsScrollPatternAvailablePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_AutomationIdPropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_ClassNamePropertyId);
    IUIAutomationTreeWalker *walker = NULL;
    uia->lpVtbl->get_ControlViewWalker(uia, &walker);

    unsigned long long hwnds[512];
    int n = win_enum_top(all, hwnds, 512);

    ListCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;
    ctx.json = json;
    ctx.first = 1;

    Uia u;
    memset(&u, 0, sizeof u);
    u.uia = uia;
    u.cache = cache;
    u.walker = walker;

    /* 顶层窗口同类型计数（与子树内 [n] 语义一致） */
    CONTROLTYPEID last_ct = 0;
    int count = 0;

    if (!all) {
        /* 默认（仅可见）：桌面根 FindAllBuildCache(Children) 一次调用取全部顶层窗口
         * 元素（带缓存），跳过逐窗口 ElementFromHandleBuildCache */
        IUIAutomationCondition *cond = NULL;
        uia->lpVtbl->get_ControlViewCondition(uia, &cond);
        IUIAutomationElement *desktop = NULL;
        if (SUCCEEDED(uia->lpVtbl->GetRootElement(uia, &desktop)) && desktop) {
            IUIAutomationElementArray *arr = NULL;
            if (SUCCEEDED(desktop->lpVtbl->FindAllBuildCache(desktop, 0x2 /* Children */,
                                                              cond, cache, &arr)) && arr) {
                int an = 0;
                arr->lpVtbl->get_Length(arr, &an);
                for (int i = 0; i < an; i++) {
                    IUIAutomationElement *root = NULL;
                    arr->lpVtbl->GetElement(arr, i, &root);
                    if (!root)
                        continue;
                    unsigned long long rhwnd = cached_own_hwnd(root);
                    /* 桌面 FindAll 可能含隐藏窗口，过滤 */
                    if (rhwnd && !IsWindowVisible((HWND)(ULONG_PTR)rhwnd)) {
                        root->lpVtbl->Release(root);
                        continue;
                    }
                    /* 默认排除进程（如 firefox）：--all 才包含 */
                    if (rhwnd && is_excluded_proc((HWND)(ULONG_PTR)rhwnd)) {
                        root->lpVtbl->Release(root);
                        continue;
                    }
                    process_root(&ctx, &u, root, rhwnd,
                                 xp_ptr, first_st, first_direct, &last_ct, &count);
                    root->lpVtbl->Release(root);
                }
                arr->lpVtbl->Release(arr);
            }
            desktop->lpVtbl->Release(desktop);
        }
        if (cond)
            cond->lpVtbl->Release(cond);
    } else {
        /* --all：EnumWindows + ElementFromHandleBuildCache（含不可见窗口） */
        for (int i = 0; i < n; i++) {
            HWND hwnd = (HWND)(ULONG_PTR)hwnds[i];
            if (first_direct && first_st->npreds > 0 && win_prefilter(hwnd, first_st))
                continue;
            IUIAutomationElement *root = NULL;
            if (FAILED(uia->lpVtbl->ElementFromHandleBuildCache(uia, hwnd, cache, &root)) || !root)
                continue;
            process_root(&ctx, &u, root, hwnds[i],
                         xp_ptr, first_st, first_direct, &last_ct, &count);
            root->lpVtbl->Release(root);
        }
    }

    if (walker)
        walker->lpVtbl->Release(walker);
    if (cache)
        cache->lpVtbl->Release(cache);
    uia->lpVtbl->Release(uia);
    CoUninitialize();

    if (!json)
        list_emit_cols(&ctx, out);
    else {
        free(ctx.rows);
        ctx.rows = NULL;
    }
    return 1;
}

/* ---------- 元素属性 ---------- */

unsigned long long uia_element_hwnd(Uia *u, void *elv) {
    /* 元素自身 NativeWindowHandle；无则回退根 hwnd（窗口操作用） */
    unsigned long long h = uia_element_own_hwnd(elv);
    return h ? h : (unsigned long long)(ULONG_PTR)u->hwnd;
}

/* 元素自身 NativeWindowHandle（实时属性），无独立句柄返回 0 */
unsigned long long uia_element_own_hwnd(void *elv) {
    IUIAutomationElement *el = elv;
    VARIANT v;
    unsigned long long h = 0;
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCurrentPropertyValue(el, UIA_NativeWindowHandlePropertyId, &v))) {
        if (V_VT(&v) == VT_I4 && V_I4(&v) != 0)
            h = (unsigned long long)(ULONG_PTR)(INT_PTR)V_I4(&v);
        else if (V_VT(&v) == VT_UI4 && V_UI4(&v) != 0)
            h = (unsigned long long)(ULONG_PTR)V_UI4(&v);
    }
    VariantClear(&v);
    return h;
}

int uia_element_prop(Uia *u, void *elv, const char *xpath, int json, SB *out) {
    IUIAutomationElement *el = elv;

    /* name：根元素用 GetWindowTextW（比 UIA CurrentName 可靠），其余 UIA 实时读取 */
    char *name = NULL;
    if (el == u->root) {
        wchar_t buf[512];
        if (GetWindowTextW(u->hwnd, buf, 512) > 0)
            name = w_to_utf8(buf);
    } else {
        BSTR b = NULL;
        if (SUCCEEDED(el->lpVtbl->get_CurrentName(el, &b)) && b) {
            name = w_to_utf8(b);
            SysFreeString(b);
        }
    }

    CONTROLTYPEID ct = 0;
    el->lpVtbl->get_CurrentControlType(el, &ct);
    const char *tn = type_name(ct);
    if (!tn) tn = "Custom";

    BOOL enabled = FALSE;
    el->lpVtbl->get_CurrentIsEnabled(el, &enabled);

    VARIANT v;
    int isinvoke = 0, isscroll = 0;
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCurrentPropertyValue(el, UIA_IsInvokePatternAvailablePropertyId, &v)))
        isinvoke = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
    VariantClear(&v);
    VariantInit(&v);
    if (SUCCEEDED(el->lpVtbl->GetCurrentPropertyValue(el, UIA_IsScrollPatternAvailablePropertyId, &v)))
        isscroll = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
    VariantClear(&v);

    char *value = NULL;
    IUIAutomationValuePattern *vp = NULL;
    if (SUCCEEDED(el->lpVtbl->GetCurrentPatternAs(el, UIA_ValuePatternId,
                    &IID_IUIAutomationValuePattern, (void **)&vp)) && vp) {
        BSTR bv = NULL;
        if (SUCCEEDED(vp->lpVtbl->get_CurrentValue(vp, &bv)) && bv) {
            value = w_to_utf8(bv);
            SysFreeString(bv);
        }
        vp->lpVtbl->Release(vp);
    }

    RECT rect = {0};
    el->lpVtbl->get_CurrentBoundingRectangle(el, &rect);
    unsigned long long ehwnd = (el == u->root)
        ? (unsigned long long)(ULONG_PTR)u->hwnd
        : uia_element_own_hwnd(el);

    if (xpath && !xpath[0])
        xpath = NULL;
    if (!xpath)
        xpath = "/"; /* 根自身：xpath 相对根，根行路径为 / */

    if (json) {
        out_element_json(out, name, value, enabled ? 1 : 0, isinvoke, isscroll,
                         ehwnd,
                         rect.left - u->left, rect.top - u->top,
                         rect.right - u->left, rect.bottom - u->top,
                         tn, xpath);
    } else {
        char buf[64];
        if (ehwnd) {
            sb_adds(out, "hwnd: ");
            snprintf(buf, sizeof buf, "0x%llx\n", ehwnd);
            sb_adds(out, buf);
        }
        sb_adds(out, "enabled: ");
        sb_adds(out, enabled ? "true\n" : "false\n");
        sb_adds(out, "invokable: ");
        sb_adds(out, isinvoke ? "true\n" : "false\n");
        sb_adds(out, "scrollable: ");
        sb_adds(out, isscroll ? "true\n" : "false\n");
        sb_adds(out, "name: ");
        if (name) {
            char *n = strdup(name);
            clean_text(n);
            sb_adds(out, n);
            free(n);
        }
        sb_adds(out, "\nvalue: ");
        if (value) {
            char *v2 = strdup(value);
            clean_text(v2);
            sb_adds(out, v2);
            free(v2);
        }
        sb_adds(out, "\nrect: ");
        snprintf(buf, sizeof buf, "%d,%d,%d,%d\n",
                 (int)(rect.left - u->left), (int)(rect.top - u->top),
                 (int)(rect.right - u->left), (int)(rect.bottom - u->top));
        sb_adds(out, buf);
        sb_adds(out, "type: ");
        sb_adds(out, tn);
        sb_adds(out, "\nxpath: ");
        sb_adds(out, xpath);
        sb_adds(out, "\n");
    }

    free(name);
    free(value);
    return 1;
}

/* ---------- 元素操作 ---------- */

int uia_init(Uia *u, const char *hwnd_str) {
    memset(u, 0, sizeof *u);
    u->hwnd = (HWND)(ULONG_PTR)strtoull(hwnd_str, NULL, 16);
    if (!u->hwnd)
        return 0;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    HRESULT hr;
    IUIAutomation *uia = NULL;
    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void **)&uia);
    if (FAILED(hr))
        return 0;
    u->uia = uia;

    IUIAutomationCacheRequest *cache = NULL;
    if (FAILED(uia->lpVtbl->CreateCacheRequest(uia, &cache))) {
        uia->lpVtbl->Release(uia);
        u->uia = NULL;
        return 0;
    }
    cache->lpVtbl->AddProperty(cache, UIA_NamePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_ControlTypePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_BoundingRectanglePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsEnabledPropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_NativeWindowHandlePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsInvokePatternAvailablePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_IsScrollPatternAvailablePropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_AutomationIdPropertyId);
    cache->lpVtbl->AddProperty(cache, UIA_ClassNamePropertyId);
    u->cache = cache;

    IUIAutomationElement *root = NULL;
    hr = uia->lpVtbl->ElementFromHandleBuildCache(uia, u->hwnd, cache, &root);
    if (FAILED(hr) || !root) {
        cache->lpVtbl->Release(cache);
        uia->lpVtbl->Release(uia);
        u->cache = NULL;
        u->uia = NULL;
        return 0;
    }
    u->root = root;

    IUIAutomationTreeWalker *walker = NULL;
    uia->lpVtbl->get_ControlViewWalker(uia, &walker);
    u->walker = walker;

    RECT rc;
    GetWindowRect(u->hwnd, &rc);
    u->left = rc.left;
    u->top = rc.top;
    return 1;
}

void uia_free(Uia *u) {
    if (u->cache)
        ((IUIAutomationCacheRequest *)u->cache)->lpVtbl->Release(u->cache);
    if (u->walker)
        ((IUIAutomationTreeWalker *)u->walker)->lpVtbl->Release(u->walker);
    if (u->root)
        ((IUIAutomationElement *)u->root)->lpVtbl->Release(u->root);
    if (u->uia)
        ((IUIAutomation *)u->uia)->lpVtbl->Release(u->uia);
    CoUninitialize();
    memset(u, 0, sizeof *u);
}

int uia_click(Uia *u, void *elv, const char *action, const char *button, int foreground) {
    IUIAutomationElement *el = elv;
    int right = button && _stricmp(button, "right") == 0;
    int press = action && _stricmp(action, "press") == 0;
    int dbl = action && _stricmp(action, "dbclick") == 0;

    RECT r = {0};
    if (FAILED(el->lpVtbl->get_CurrentBoundingRectangle(el, &r)))
        return 0;
    int sx = (int)(((long long)r.left + r.right) / 2);
    int sy = (int)(((long long)r.top + r.bottom) / 2);

    if (foreground) {
        /* 前台：激活根窗口，真实鼠标点击屏幕坐标 */
        SetForegroundWindow(u->hwnd);
        Sleep(20);
        SetCursorPos(sx, sy);
        Sleep(10);
        DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
        DWORD up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
        INPUT in[4] = {0};
        int n = 0;
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = down;
        n++;
        if (!press) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = up;
            n++;
        }
        if (dbl) {
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = down;
            n++;
            in[n].type = INPUT_MOUSE;
            in[n].mi.dwFlags = up;
            n++;
        }
        SendInput(n, in, sizeof(INPUT));
        return 1;
    }

    /* 后台：投递到元素自身的窗口句柄（NativeWindowHandle），无独立句柄则沿
     * UIA 树向上找最近的有句柄祖先，最后退到根窗口。
     * 不用 WindowFromPoint：窗口被其他窗口遮挡时它会取到错误的顶层窗口 */
    HWND ehwnd = u->hwnd;
    IUIAutomationElement *e = el;
    for (;;) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(e->lpVtbl->GetCurrentPropertyValue(e, UIA_NativeWindowHandlePropertyId, &v))) {
            if (V_VT(&v) == VT_I4 && V_I4(&v) != 0) {
                ehwnd = (HWND)(INT_PTR)V_I4(&v);
                VariantClear(&v);
                break;
            } else if (V_VT(&v) == VT_UI4 && V_UI4(&v) != 0) {
                ehwnd = (HWND)(ULONG_PTR)V_UI4(&v);
                VariantClear(&v);
                break;
            }
        }
        VariantClear(&v);
        if (e == u->root)
            break;
        IUIAutomationElement *parent = NULL;
        if (FAILED(((IUIAutomationTreeWalker *)u->walker)->lpVtbl->GetParentElement(u->walker, e, &parent)) || !parent)
            break;
        if (e != el)
            e->lpVtbl->Release(e);
        e = parent;
    }
    if (e != el)
        e->lpVtbl->Release(e);
    POINT origin = {0, 0};
    ClientToScreen(ehwnd, &origin);
    LPARAM lp = MAKELPARAM(sx - origin.x, sy - origin.y);
    UINT down = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    UINT up = right ? WM_RBUTTONUP : WM_LBUTTONUP;
    WPARAM wp = right ? MK_RBUTTON : MK_LBUTTON;
    PostMessageW(ehwnd, WM_MOUSEMOVE, 0, lp);
    PostMessageW(ehwnd, down, wp, lp);
    if (!press)
        PostMessageW(ehwnd, up, 0, lp);
    if (dbl) {
        PostMessageW(ehwnd, down, wp, lp);
        PostMessageW(ehwnd, up, 0, lp);
    }
    return 1;
}

int uia_set_value(Uia *u, void *elv, const char *value) {
    (void)u;
    IUIAutomationElement *el = elv;
    IUIAutomationValuePattern *vp = NULL;
    if (SUCCEEDED(el->lpVtbl->GetCurrentPatternAs(el, UIA_ValuePatternId,
                    &IID_IUIAutomationValuePattern, (void **)&vp)) && vp) {
        BSTR b = utf8_to_bstr(value);
        HRESULT hr = b ? vp->lpVtbl->SetValue(vp, b) : E_FAIL;
        if (b)
            SysFreeString(b);
        vp->lpVtbl->Release(vp);
        if (SUCCEEDED(hr))
            return 1;
    }
    /* 无 ValuePattern 或 SetValue 失败：直接报错，不做剪贴板回退（回退无反馈，易伪装成功） */
    return 0;
}

int uia_focus(Uia *u, void *elv) {
    (void)u;
    IUIAutomationElement *el = elv;
    return SUCCEEDED(el->lpVtbl->SetFocus(el)) ? 1 : 0;
}

char *uia_get_value(Uia *u, void *elv) {
    (void)u;
    IUIAutomationElement *el = elv;
    IUIAutomationValuePattern *vp = NULL;
    if (FAILED(el->lpVtbl->GetCurrentPatternAs(el, UIA_ValuePatternId,
                    &IID_IUIAutomationValuePattern, (void **)&vp)) || !vp)
        return NULL;
    BSTR b = NULL;
    char *s = NULL;
    if (SUCCEEDED(vp->lpVtbl->get_CurrentValue(vp, &b)) && b) {
        s = w_to_utf8(b);
        SysFreeString(b);
    }
    vp->lpVtbl->Release(vp);
    return s;
}

/* ---------- 辅助 ---------- */
