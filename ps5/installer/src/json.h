/* PS5SX2 Installer: a small, strict JSON reader (enough for GitHub's release API). */
#pragma once

#include <stddef.h>

typedef enum { JS_NULL, JS_FALSE, JS_TRUE, JS_NUM, JS_STR, JS_ARR, JS_OBJ } js_type;

typedef struct js_node js_node;
struct js_node {
  js_type type;
  char *key;  /* member name inside an object */
  char *str;  /* JS_STR: decoded UTF-8; JS_NUM: the number as written */
  size_t len; /* bytes in str */
  js_node *child;
  js_node *next;
};

js_node *js_parse(const char *text, size_t len); /* NULL on error (err_get()) */
void js_free(js_node *root);

const js_node *js_get(const js_node *obj, const char *key);
const char *js_str(const js_node *n); /* NULL unless a string without NUL bytes */
int js_int64(const js_node *n, long long *out); /* 0 if an integer */
int js_is_true(const js_node *n);
