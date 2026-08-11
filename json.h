#ifndef WJB_JSON_H
#define WJB_JSON_H

#include <stddef.h>

/* 动态字符串缓冲，用于序列化 JSON 响应 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} SB;

void sb_init(SB *sb);
void sb_free(SB *sb);
void sb_add(SB *sb, const char *s, size_t n);
void sb_adds(SB *sb, const char *s);
/* 写入 JSON 字符串字面量（带引号和转义），s 为 UTF-8 */
void sb_json_str(SB *sb, const char *s);
/* 写入 JSON 字符串字面量或 null */
void sb_json_str_or_null(SB *sb, const char *s);
void sb_json_bool(SB *sb, int b);
void sb_json_int(SB *sb, long long v);

/*
 * 轻量 JSON 值提取，面向本驱动扁平请求体（{"op":..., "hwnd":...}）。
 * 不引第三方库，仅支持字符串/整数/布尔/null 值。
 * json 输入缓冲在整个处理期间必须保持有效。
 */

/* 返回 key 对应字符串值的原始内容指针（引号内、未反转义），非字符串或不存在返回 NULL */
const char *jstr(const char *json, const char *key);
/* 反转义拷贝到 out（UTF-8），返回 1 成功 0 失败/不存在 */
int jget_str(const char *json, const char *key, char *out, size_t cap);
int jget_int(const char *json, const char *key, long long *out);
int jget_bool(const char *json, const char *key, int *out);

#endif
