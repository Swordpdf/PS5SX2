/* PS5SX2 Installer: a small, strict JSON reader (RFC 8259), depth-limited. */
#include "json.h"

#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct {
  const char *p, *end;
  int depth;
} jp;

static void ws(jp *j) {
  while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
    j->p++;
}

static js_node *node_new(js_type t) {
  js_node *n = calloc(1, sizeof(*n));
  if (!n)
    err_set("out of memory");
  else
    n->type = t;
  return n;
}

void js_free(js_node *n) {
  while (n) {
    js_node *next = n->next;
    js_free(n->child);
    free(n->key);
    free(n->str);
    free(n);
    n = next;
  }
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (unsigned)(c - 'A' + 10);
    else
      return -1;
  }
  *out = v;
  return 0;
}

static void put_utf8(sbuf *b, unsigned cp) {
  char u[4];
  size_t n;
  if (cp < 0x80) {
    u[0] = (char)cp;
    n = 1;
  } else if (cp < 0x800) {
    u[0] = (char)(0xc0 | (cp >> 6));
    u[1] = (char)(0x80 | (cp & 0x3f));
    n = 2;
  } else if (cp < 0x10000) {
    u[0] = (char)(0xe0 | (cp >> 12));
    u[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
    u[2] = (char)(0x80 | (cp & 0x3f));
    n = 3;
  } else {
    u[0] = (char)(0xf0 | (cp >> 18));
    u[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    u[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    u[3] = (char)(0x80 | (cp & 0x3f));
    n = 4;
  }
  sb_append(b, u, n);
}

/* At the opening quote; returns a malloc'ed string. */
static char *parse_string(jp *j, size_t *len_out) {
  j->p++; /* " */
  sbuf b;
  sb_init(&b);
  if (sb_reserve(&b, 16) != 0)
    return NULL;
  b.data[0] = '\0';
  while (j->p < j->end) {
    const unsigned char c = (unsigned char)*j->p;
    if (c == '"') {
      j->p++;
      if (!b.data && sb_reserve(&b, 1) != 0)
        return NULL;
      *len_out = b.len;
      return b.data;
    }
    if (c < 0x20) {
      err_set("JSON: control character in a string");
      sb_free(&b);
      return NULL;
    }
    if (c != '\\') {
      sb_append(&b, j->p, 1);
      j->p++;
      continue;
    }
    if (j->end - j->p < 2)
      break;
    const char e = j->p[1];
    j->p += 2;
    switch (e) {
    case '"':
      sb_append(&b, "\"", 1);
      break;
    case '\\':
      sb_append(&b, "\\", 1);
      break;
    case '/':
      sb_append(&b, "/", 1);
      break;
    case 'b':
      sb_append(&b, "\b", 1);
      break;
    case 'f':
      sb_append(&b, "\f", 1);
      break;
    case 'n':
      sb_append(&b, "\n", 1);
      break;
    case 'r':
      sb_append(&b, "\r", 1);
      break;
    case 't':
      sb_append(&b, "\t", 1);
      break;
    case 'u': {
      unsigned cp;
      if (j->end - j->p < 4 || hex4(j->p, &cp) != 0) {
        err_set("JSON: bad \\u escape");
        sb_free(&b);
        return NULL;
      }
      j->p += 4;
      if (cp >= 0xd800 && cp <= 0xdbff) {
        unsigned lo;
        if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u' && hex4(j->p + 2, &lo) == 0 && lo >= 0xdc00 &&
            lo <= 0xdfff) {
          j->p += 6;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        } else
          cp = 0xfffd;
      } else if (cp >= 0xdc00 && cp <= 0xdfff)
        cp = 0xfffd;
      put_utf8(&b, cp);
      break;
    }
    default:
      err_set("JSON: bad escape");
      sb_free(&b);
      return NULL;
    }
  }
  err_set("JSON: unfinished string");
  sb_free(&b);
  return NULL;
}

static js_node *parse_value(jp *j);

static js_node *parse_number(jp *j) {
  const char *s = j->p;
  if (j->p < j->end && *j->p == '-')
    j->p++;
  if (j->p >= j->end)
    goto bad;
  if (*j->p == '0')
    j->p++;
  else if (*j->p >= '1' && *j->p <= '9')
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
      j->p++;
  else
    goto bad;
  if (j->p < j->end && *j->p == '.') {
    j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
      goto bad;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
      j->p++;
  }
  if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
    j->p++;
    if (j->p < j->end && (*j->p == '+' || *j->p == '-'))
      j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
      goto bad;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9')
      j->p++;
  }
  {
    js_node *n = node_new(JS_NUM);
    if (!n)
      return NULL;
    n->str = str_ndup(s, (size_t)(j->p - s));
    n->len = (size_t)(j->p - s);
    if (!n->str) {
      free(n);
      return NULL;
    }
    return n;
  }
bad:
  err_set("JSON: bad number");
  return NULL;
}

static int lit(jp *j, const char *w) {
  const size_t n = strlen(w);
  if ((size_t)(j->end - j->p) >= n && memcmp(j->p, w, n) == 0) {
    j->p += n;
    return 1;
  }
  return 0;
}

static js_node *parse_container(jp *j, int obj) {
  if (++j->depth > MAX_DEPTH) {
    err_set("JSON: nested too deep");
    return NULL;
  }
  js_node *n = node_new(obj ? JS_OBJ : JS_ARR);
  if (!n)
    return NULL;
  j->p++; /* { or [ */
  js_node **tail = &n->child;
  ws(j);
  if (j->p < j->end && *j->p == (obj ? '}' : ']')) {
    j->p++;
    j->depth--;
    return n;
  }
  for (;;) {
    ws(j);
    char *key = NULL;
    if (obj) {
      size_t kl;
      if (j->p >= j->end || *j->p != '"') {
        err_set("JSON: expected a member name");
        goto fail;
      }
      key = parse_string(j, &kl);
      if (!key)
        goto fail;
      ws(j);
      if (j->p >= j->end || *j->p != ':') {
        free(key);
        err_set("JSON: expected ':'");
        goto fail;
      }
      j->p++;
    }
    js_node *v = parse_value(j);
    if (!v) {
      free(key);
      goto fail;
    }
    v->key = key;
    *tail = v;
    tail = &v->next;
    ws(j);
    if (j->p >= j->end) {
      err_set("JSON: unfinished %s", obj ? "object" : "array");
      goto fail;
    }
    if (*j->p == ',') {
      j->p++;
      continue;
    }
    if (*j->p == (obj ? '}' : ']')) {
      j->p++;
      j->depth--;
      return n;
    }
    err_set("JSON: expected ',' or '%c'", obj ? '}' : ']');
    goto fail;
  }
fail:
  js_free(n);
  return NULL;
}

static js_node *parse_value(jp *j) {
  ws(j);
  if (j->p >= j->end) {
    err_set("JSON: unexpected end");
    return NULL;
  }
  const char c = *j->p;
  if (c == '{' || c == '[')
    return parse_container(j, c == '{');
  if (c == '"') {
    js_node *n = node_new(JS_STR);
    if (!n)
      return NULL;
    n->str = parse_string(j, &n->len);
    if (!n->str) {
      free(n);
      return NULL;
    }
    return n;
  }
  if (c == '-' || (c >= '0' && c <= '9'))
    return parse_number(j);
  if (lit(j, "true"))
    return node_new(JS_TRUE);
  if (lit(j, "false"))
    return node_new(JS_FALSE);
  if (lit(j, "null"))
    return node_new(JS_NULL);
  err_set("JSON: unexpected character");
  return NULL;
}

js_node *js_parse(const char *text, size_t len) {
  jp j;
  j.p = text;
  j.end = text + len;
  j.depth = 0;
  js_node *root = parse_value(&j);
  if (!root)
    return NULL;
  ws(&j);
  if (j.p != j.end) {
    err_set("JSON: extra data after the value");
    js_free(root);
    return NULL;
  }
  return root;
}

const js_node *js_get(const js_node *obj, const char *key) {
  if (!obj || obj->type != JS_OBJ)
    return NULL;
  for (const js_node *c = obj->child; c; c = c->next)
    if (c->key && !strcmp(c->key, key))
      return c;
  return NULL;
}

const char *js_str(const js_node *n) {
  if (!n || n->type != JS_STR || strlen(n->str) != n->len)
    return NULL;
  return n->str;
}

int js_int64(const js_node *n, long long *out) {
  if (!n || n->type != JS_NUM)
    return -1;
  char *end = NULL;
  errno = 0;
  const long long v = strtoll(n->str, &end, 10);
  if (errno || !end || *end)
    return -1;
  *out = v;
  return 0;
}

int js_is_true(const js_node *n) { return n && n->type == JS_TRUE; }
