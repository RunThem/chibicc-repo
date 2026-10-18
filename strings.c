#include "chibicc.h"

void strarray_push(StringArray *arr, char *s) {
  if (!arr->data) {
    arr->data = chibicc_arena_calloc(8, sizeof(char *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    int oldcap = arr->capacity;
    arr->data = chibicc_arena_realloc(arr->data, sizeof(char *) * oldcap,
                                      sizeof(char *) * oldcap * 2);
    arr->capacity = oldcap * 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = NULL;
  }

  arr->data[arr->len++] = s;
}

void nodearray_push(NodeArray *arr, Node *node) {
  if (!arr->data) {
    arr->data = chibicc_arena_calloc(8, sizeof(Node *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    int oldcap = arr->capacity;
    arr->data = chibicc_arena_realloc(arr->data, sizeof(Node *) * oldcap,
                                      sizeof(Node *) * oldcap * 2);
    arr->capacity = oldcap * 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = NULL;
  }

  arr->data[arr->len++] = node;
}

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

// \xHH for everything invisible (including the space, so that a dump
// line never ends in whitespace that tools could strip).
void print_escaped(FILE *out, char *p, int len) {
  for (int i = 0; i < len; i++) {
    unsigned char c = p[i];

    switch (c) {
    case '\n': fprintf(out, "\\n"); break;
    case '\t': fprintf(out, "\\t"); break;
    case '\r': fprintf(out, "\\r"); break;
    case '\\': fprintf(out, "\\\\"); break;
    default:
      if (c < 0x20 || c >= 0x7f || c == ' ')
        fprintf(out, "\\x%02x", c);
      else
        fputc(c, out);
    }
  }
}

// Returns true if a given file exists. Used by the preprocessor and the
// driver; it lives in the library so a consumer does not need main.c.
bool file_exists(char *path) {
  struct stat st;
  return !stat(path, &st);
}

// Takes a printf-style format string and returns a formatted string in
// the active context's arena (or malloc when there is no context).
char *format(char *fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char *buf = chibicc_arena_alloc(len + 1, 1);
  vsnprintf(buf, len + 1, fmt, ap);
  va_end(ap);
  return buf;
}
