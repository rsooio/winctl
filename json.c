#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 序列化 ---------- */

void sb_init(SB *sb) {
    sb->buf = malloc(64);
    sb->len = 0;
    sb->cap = 64;
    sb->buf[0] = '\0';
}

void sb_free(SB *sb) {
    free(sb->buf);
    sb->buf = NULL;
}

static void sb_reserve(SB *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap)
        return;
    size_t cap = sb->cap * 2;
    while (sb->len + extra + 1 > cap)
        cap *= 2;
    sb->buf = realloc(sb->buf, cap);
    sb->cap = cap;
}

void sb_add(SB *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_adds(SB *sb, const char *s) {
    sb_add(sb, s, strlen(s));
}

void sb_json_str(SB *sb, const char *s) {
    sb_adds(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  sb_adds(sb, "\\\""); break;
        case '\\': sb_adds(sb, "\\\\"); break;
        case '\n': sb_adds(sb, "\\n"); break;
        case '\r': sb_adds(sb, "\\r"); break;
        case '\t': sb_adds(sb, "\\t"); break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof esc, "\\u%04x", c);
                sb_adds(sb, esc);
            } else {
                sb_add(sb, (const char *)&c, 1);
            }
        }
    }
    sb_adds(sb, "\"");
}

void sb_json_str_or_null(SB *sb, const char *s) {
    if (s)
        sb_json_str(sb, s);
    else
        sb_adds(sb, "null");
}

void sb_json_bool(SB *sb, int b) {
    sb_adds(sb, b ? "true" : "false");
}

void sb_json_int(SB *sb, long long v) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%lld", v);
    sb_adds(sb, tmp);
}

/* ---------- 解析 ---------- */

/* 跳过空白 */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* 在 p 处解析字符串字面量，返回内容起点（跳过开引号），*end 指向收尾引号 */
static const char *json_str_raw(const char *p, const char **end) {
    p = skip_ws(p);
    if (*p != '"')
        return NULL;
    p++;
    const char *q = p;
    while (*q && *q != '"') {
        if (*q == '\\')
            q++;
        q++;
    }
    if (*q != '"')
        return NULL;
    *end = q;
    return p;
}

const char *jstr(const char *json, const char *key) {
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *k = p + 1;
        if (strncmp(k, key, klen) == 0 && k[klen] == '"') {
            const char *q = k + klen + 1;
            q = skip_ws(q);
            if (*q == ':') {
                const char *v, *vend;
                if ((v = json_str_raw(q + 1, &vend)) != NULL) {
                    (void)v;
                    return vend; /* 调用方自行截取，见下 */
                }
                return NULL;
            }
        }
        p = k;
    }
    return NULL;
}

/* 反转义一段 JSON 字符串内容（[start, end)）到 out */
static int unescape(const char *start, const char *end, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = start; p < end; p++) {
        if (*p != '\\') {
            if (n + 1 >= cap) return 0;
            out[n++] = *p;
            continue;
        }
        p++;
        if (p >= end) return 0;
        char c = 0;
        switch (*p) {
        case '"':  c = '"'; break;
        case '\\': c = '\\'; break;
        case '/':  c = '/'; break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'n':  c = '\n'; break;
        case 'r':  c = '\r'; break;
        case 't':  c = '\t'; break;
        case 'u': {
            if (p + 4 >= end) return 0;
            unsigned cp = 0;
            for (int i = 1; i <= 4; i++) {
                char h = p[i];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= h - '0';
                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                else return 0;
            }
            p += 4;
            /* 仅支持 BMP；代理对输出替换符 */
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                if (n + 3 >= cap) return 0;
                out[n++] = 0xEF; out[n++] = 0xBF; out[n++] = 0xBD;
                continue;
            }
            /* UTF-8 编码 */
            int len;
            if (cp < 0x80) len = 1;
            else if (cp < 0x800) len = 2;
            else len = 3;
            if (n + len >= cap) return 0;
            if (len == 3) {
                out[n++] = 0xE0 | (cp >> 12);
                out[n++] = 0x80 | ((cp >> 6) & 0x3F);
            } else if (len == 2) {
                out[n++] = 0xC0 | (cp >> 6);
            } else {
                out[n++] = (char)cp;
                continue;
            }
            out[n++] = 0x80 | (cp & 0x3F);
            continue;
        }
        default:
            return 0;
        }
        if (n + 1 >= cap) return 0;
        out[n++] = c;
    }
    out[n] = '\0';
    return 1;
}

int jget_str(const char *json, const char *key, char *out, size_t cap) {
    const char *p = json;
    size_t klen = strlen(key);
    while ((p = strchr(p, '"')) != NULL) {
        const char *k = p + 1;
        if (strncmp(k, key, klen) == 0 && k[klen] == '"') {
            const char *q = skip_ws(k + klen + 1);
            if (*q == ':') {
                const char *v = skip_ws(q + 1);
                if (*v != '"') return 0;
                const char *start = v + 1;
                const char *end = start;
                while (*end && *end != '"') {
                    if (*end == '\\') end++;
                    end++;
                }
                if (*end != '"') return 0;
                return unescape(start, end, out, cap);
            }
        }
        p = k;
    }
    return 0;
}

int jget_int(const char *json, const char *key, long long *out) {
    char tmp[64];
    if (!jget_str(json, key, tmp, sizeof tmp))
        return 0;
    char *end;
    long long v = strtoll(tmp, &end, 10);
    if (*end)
        return 0;
    *out = v;
    return 1;
}

int jget_bool(const char *json, const char *key, int *out) {
    char tmp[8];
    if (!jget_str(json, key, tmp, sizeof tmp))
        return 0;
    if (strcmp(tmp, "true") == 0) { *out = 1; return 1; }
    if (strcmp(tmp, "false") == 0) { *out = 0; return 1; }
    return 0;
}
