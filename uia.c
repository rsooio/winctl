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

static int type_name_ct(IUIAutomationElement *el, const char *name);
static int el_matches(IUIAutomationTreeWalker *walker, IUIAutomationElement *el,
                     const XStep *st);
static IUIAutomationElement *step_walk(Uia *u, IUIAutomationElement *from,
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

/* ---------- 定位求值（取第一个） ---------- */

static IUIAutomationElement *step_find(Uia *u, IUIAutomationElement *from,
                                       const XStep *st, int subtree) {
    IUIAutomation *uia = u->uia;
    if (!subtree)
        return step_walk(u, from, st);

    /* 降级路径（//）：FindAll 只做宽筛选（ControlView + 类型）。
     * 属性谓词（Name 等）与位置在客户端用 el_matches 过滤——
     * PropertyCondition 的属性匹配依赖 provider 端实现，部分应用（XAML）不支持 Name 条件 */
    IUIAutomationCondition *cond = NULL;
    uia->lpVtbl->get_ControlViewCondition(uia, &cond);
    if (strcmp(st->name, "*") != 0) {
        VARIANT v;
        VariantInit(&v);
        V_VT(&v) = VT_I4;
        V_I4(&v) = name_type(st->name);
        if (V_I4(&v) < 0) {
            VariantClear(&v);
            if (cond) cond->lpVtbl->Release(cond);
            return NULL;
        }
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
    from->lpVtbl->FindAll(from, 7, cond, &arr);
    if (cond)
        cond->lpVtbl->Release(cond);
    if (!arr)
        return NULL;

    int n = 0;
    arr->lpVtbl->get_Length(arr, &n);
    IUIAutomationElement *found = NULL;
    int count = 0;
    for (int i = 0; i < n && !found; i++) {
        IUIAutomationElement *el = NULL;
        arr->lpVtbl->GetElement(arr, i, &el);
        if (!el)
            continue;
        if (el_matches(NULL, el, st)) {
            count++;
            if (st->pos == 0 || count == st->pos)
                found = el; /* 引用来自 GetElement，保留 */
            else
                el->lpVtbl->Release(el);
        } else {
            el->lpVtbl->Release(el);
        }
    }
    arr->lpVtbl->Release(arr);
    return found;
}

/* 手动遍历 ControlView 子级（与集合求值同视图同顺序），返回第 pos 个匹配（pos==0 取第一个） */
static IUIAutomationElement *step_walk(Uia *u, IUIAutomationElement *from,
                                       const XStep *st) {
    IUIAutomationTreeWalker *walker = u->walker;
    int n = 0;
    IUIAutomationElement *child = NULL;
    walker->lpVtbl->GetFirstChildElement(walker, from, &child);
    while (child) {
        if (el_matches(NULL, child, st)) {
            n++;
            if (st->pos == 0 || n == st->pos)
                return child; /* 引用来自 GetFirstChildElement，调用方负责 Release */
        }
        IUIAutomationElement *next = NULL;
        walker->lpVtbl->GetNextSiblingElement(walker, child, &next);
        child->lpVtbl->Release(child);
        child = next;
    }
    return NULL;
}

void uia_release_element(void *el) {
    if (el)
        ((IUIAutomationElement *)el)->lpVtbl->Release(el);
}

void *uia_find(Uia *u, const char *xpath) {
    IUIAutomationElement *root = u->root;
    if (!xpath || !xpath[0]) {
        /* 根元素 */
        root->lpVtbl->AddRef(root);
        return root;
    }
    char err[128];
    XPath xp;
    if (!parse_xpath(xpath, &xp, err, sizeof err))
        return NULL;

    /* 起点：根元素 */
    IUIAutomationElement *cur = root;
    cur->lpVtbl->AddRef(cur);

    for (int i = 0; i < xp.n; i++) {
        XStep *st = &xp.steps[i];
        int first = (i == 0);
        IUIAutomationElement *next = NULL;

        if (first) {
            /* 第一段：根自身 → 子级 → 整棵子树，依次尝试 */
            if (!st->pos && !st->npreds &&
                (strcmp(st->name, "*") == 0 ||
                 (st->name[0] && type_name_ct(cur, st->name)))) {
                next = cur; /* 根自身命中，继续下一段 */
            } else {
                next = step_find(u, cur, st, 0);
                if (!next)
                    next = step_find(u, cur, st, 1);
            }
        } else {
            next = step_find(u, cur, st, st->descendant);
        }

        if (next != cur) {
            cur->lpVtbl->Release(cur);
            cur = next;
        }
        if (!cur)
            return NULL;
    }
    return cur;
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

/* 路径模式匹配：segs 为该节点从根起的完整祖先链，steps 为 XPath 各步。
 * 末段（steps 最后一段）必须匹配节点自身（链最后一段）；
 * 非 descendant 步严格逐级；descendant 步（//）跳过任意深度找匹配段 */
static int path_match(IUIAutomationTreeWalker *walker, const Seg *segs, int depth,
                      const XPath *xp) {
    int i = 0, j = 0;
    while (j < xp->n) {
        const XStep *st = &xp->steps[j];
        int last = (j == xp->n - 1);
        if (st->descendant) {
            if (last) {
                /* 末段（//B）：节点自身（链最后一段）必须匹配 */
                return depth > 0 && seg_matches(walker, &segs[depth - 1], st);
            }
            int k = i;
            while (k < depth && !seg_matches(walker, &segs[k], st))
                k++;
            if (k >= depth)
                return 0;
            i = k + 1;
        } else {
            if (i >= depth)
                return 0;
            if (!seg_matches(walker, &segs[i], st))
                return 0;
            i++;
        }
        j++;
    }
    return i == depth;
}

typedef struct {
    char *xpath;
    char *name;
    unsigned long long hwnd;
} ListRow;

typedef struct {
    SB *out;
    int json;
    int first;          /* JSON 数组元素分隔 */
    ListRow *rows;      /* 列模式收集 */
    int n, cap;
    const XPath *xp;    /* NULL = 全部 */
    int max_depth;      /* 无 descendant 段时的匹配深度，超深子树不访问；0 = 不限 */
    Seg segs[64];
    int depth;
} ListCtx;

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
static void walk_nodes(ListCtx *ctx, IUIAutomationTreeWalker *walker,
                       IUIAutomationCacheRequest *cache, Uia *u,
                       IUIAutomationElement *parent, const char *px,
                       CONTROLTYPEID *last_ct, int *count) {
    IUIAutomationElement *child = NULL;
    walker->lpVtbl->GetFirstChildElementBuildCache(walker, parent, cache, &child);
    while (child) {
        CONTROLTYPEID ct = 0;
        child->lpVtbl->get_CachedControlType(child, &ct);
        const char *tn = type_name(ct);
        if (!tn) tn = "Custom";

        int cnt = (ct == *last_ct) ? (*count + 1) : 1;
        *last_ct = ct;
        *count = cnt;

        char *own = malloc(strlen(px) + strlen(tn) + 16);
        if (cnt > 1)
            sprintf(own, "%s/%s[%d]", px, tn, cnt);
        else
            sprintf(own, "%s/%s", px, tn);

        ctx->segs[ctx->depth].el = child;
        ctx->segs[ctx->depth].cnt = cnt;
        ctx->depth++;
        int matched = !ctx->xp || path_match(walker, ctx->segs, ctx->depth, ctx->xp);
        ctx->depth--;

        if (matched) {
            BSTR bname = NULL;
            char *name = NULL;
            if (SUCCEEDED(child->lpVtbl->get_CachedName(child, &bname)) && bname) {
                name = w_to_utf8(bname);
                SysFreeString(bname);
            }
            BOOL enabled = FALSE;
            child->lpVtbl->get_CachedIsEnabled(child, &enabled);

            VARIANT v;
            int isinvoke = 0, isscroll = 0;
            VariantInit(&v);
            if (SUCCEEDED(child->lpVtbl->GetCachedPropertyValue(child, UIA_IsInvokePatternAvailablePropertyId, &v)))
                isinvoke = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
            VariantClear(&v);
            VariantInit(&v);
            if (SUCCEEDED(child->lpVtbl->GetCachedPropertyValue(child, UIA_IsScrollPatternAvailablePropertyId, &v)))
                isscroll = (V_VT(&v) == VT_BOOL && V_BOOL(&v) != 0);
            VariantClear(&v);

            unsigned long long own_hwnd = cached_own_hwnd(child);

            RECT rect = {0};
            child->lpVtbl->get_CachedBoundingRectangle(child, &rect);

            emit_node(ctx, own, name, enabled ? 1 : 0, isinvoke, isscroll,
                      own_hwnd,
                      rect.left - u->left, rect.top - u->top,
                      rect.right - u->left, rect.bottom - u->top,
                      tn);
            free(name);
        }

        /* 子级：同类型计数重置；深度限制（无 descendant 段时匹配深度 = 段数，
         * 更深的子树不可能包含匹配节点，跳过访问） */
        if (!ctx->max_depth || ctx->depth < ctx->max_depth) {
            CONTROLTYPEID sub_last = 0;
            int sub_count = 0;
            ctx->depth++;
            walk_nodes(ctx, walker, cache, u, child, own, &sub_last, &sub_count);
            ctx->depth--;
        }

        free(own);

        IUIAutomationElement *next = NULL;
        walker->lpVtbl->GetNextSiblingElementBuildCache(walker, child, cache, &next);
        child->lpVtbl->Release(child);
        child = next;
    }
}

/* 遍历单根：root 为窗口元素（段 0），输出匹配子树。top=1 时该段支持 @Pid 谓词 */
static void walk_root(ListCtx *ctx, IUIAutomationTreeWalker *walker,
                      IUIAutomationCacheRequest *cache, Uia *u,
                      IUIAutomationElement *root, const char *root_type,
                      int root_cnt, int top, unsigned long long root_hwnd,
                      const char *root_xpath) {
    ctx->segs[0].el = root;
    ctx->segs[0].cnt = root_cnt;
    ctx->segs[0].top = top;
    ctx->segs[0].hwnd = root_hwnd;
    ctx->depth = 1;
    if (!ctx->xp || path_match(walker, ctx->segs, 1, ctx->xp)) {
        BSTR bname = NULL;
        char *name = NULL;
        if (SUCCEEDED(root->lpVtbl->get_CachedName(root, &bname)) && bname) {
            name = w_to_utf8(bname);
            SysFreeString(bname);
        }
        BOOL enabled = FALSE;
        root->lpVtbl->get_CachedIsEnabled(root, &enabled);
        RECT rect = {0};
        root->lpVtbl->get_CachedBoundingRectangle(root, &rect);
        emit_node(ctx, root_xpath, name, enabled ? 1 : 0, 0, 0,
                  root_hwnd,
                  rect.left - u->left, rect.top - u->top,
                  rect.right - u->left, rect.bottom - u->top,
                  root_type);
        free(name);
    }
    CONTROLTYPEID sub_last = 0;
    int sub_count = 0;
    walk_nodes(ctx, walker, cache, u, root, root_xpath, &sub_last, &sub_count);
}

/* 计算深度限制：xpath 无 descendant 段时，匹配节点深度 = 段数；否则 0（不限） */
static int xpath_max_depth(const XPath *xp) {
    if (!xp)
        return 0;
    for (int i = 0; i < xp->n; i++)
        if (xp->steps[i].descendant)
            return 0;
    return xp->n;
}

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

int uia_list(Uia *u, const char *xpath, int json, char *err, size_t errlen, SB *out) {
    XPath xp;
    const XPath *xp_ptr = NULL;
    if (xpath && xpath[0] && strcmp(xpath, "/") != 0) {
        if (!parse_xpath(xpath, &xp, err, errlen))
            return 0;
        if (!check_subpath_placement(&xp, 0, err, errlen))
            return 0;
        xp_ptr = &xp;
    }

    ListCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.out = out;
    ctx.json = json;
    ctx.first = 1;
    ctx.xp = xp_ptr;
    ctx.max_depth = xpath_max_depth(xp_ptr);

    /* 根元素 */
    IUIAutomationElement *root = u->root;
    CONTROLTYPEID rct = 0;
    root->lpVtbl->get_CachedControlType(root, &rct);
    const char *rtn = type_name(rct);
    if (!rtn) rtn = "Custom";
    char root_xpath[64];
    snprintf(root_xpath, sizeof root_xpath, "/%s", rtn);

    walk_root(&ctx, u->walker, u->cache, u, root, rtn, 1, 1,
              (unsigned long long)(ULONG_PTR)u->hwnd, root_xpath);

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
static void process_root(ListCtx *ctx, IUIAutomationTreeWalker *walker,
                         IUIAutomationCacheRequest *cache, Uia *u,
                         IUIAutomationElement *root, unsigned long long rhwnd,
                         const XStep *first_st, int first_direct,
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

    /* 根段匹配检查：非 descendant 首段不匹配则跳过整树遍历 */
    if (first_direct) {
        Seg root_seg = {root, cnt, 1, rhwnd};
        if (!seg_matches(walker, &root_seg, first_st))
            return;
    }

    /* 顶层窗口段 xpath 不带 [n]（Z 序编号无意义，稳定标识用 hwnd 列） */
    char root_xpath[64];
    snprintf(root_xpath, sizeof root_xpath, "/%s", rtn);

    walk_root(ctx, walker, cache, u, root, rtn, cnt, 1, rhwnd, root_xpath);
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
    ctx.xp = xp_ptr;
    ctx.max_depth = xpath_max_depth(xp_ptr);

    Uia u;
    memset(&u, 0, sizeof u);

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
                    process_root(&ctx, walker, cache, &u, root, rhwnd,
                                 first_st, first_direct, &last_ct, &count);
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
            process_root(&ctx, walker, cache, &u, root, hwnds[i],
                         first_st, first_direct, &last_ct, &count);
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
    char root_path[64];
    if (!xpath) {
        snprintf(root_path, sizeof root_path, "/%s", tn);
        xpath = root_path;
    }

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
    /* 回退：聚焦 + 剪贴板 + Ctrl+A / Ctrl+V */
    el->lpVtbl->SetFocus(el);
    SetForegroundWindow(u->hwnd);
    Sleep(20);
    win_set_clipboard(value);
    win_tap_combo(VK_CONTROL, 'A');
    win_tap_combo(VK_CONTROL, 'V');
    return 1;
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

static int type_name_ct(IUIAutomationElement *el, const char *name) {
    CONTROLTYPEID ct = 0;
    el->lpVtbl->get_CurrentControlType(el, &ct);
    const char *tn = type_name(ct);
    return tn && strcmp(tn, name) == 0;
}
