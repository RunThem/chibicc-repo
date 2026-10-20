#include "test.h"
#include "stddef.h"

int attr_unused __attribute__((unused));
int attr_aligned __attribute__((aligned(16)));

static void attr_cleanup_fn(int *p) { (void)p; }

static void attr_cleanup_use(void) {
  int x __attribute__((cleanup(attr_cleanup_fn)));
  (void)x;
}

void attr_format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void attr_format(const char *fmt, ...) { (void)fmt; }
void attr_visibility(void) __attribute__((visibility("hidden")));
void attr_visibility(void) {}

__attribute__((noreturn)) void attr_noreturn(void);
void attr_noreturn(void) { for (;;) ; }

struct AttrMember {
  int a __attribute__((unused));
  int b __attribute__((aligned(8)));
};

int main() {
  attr_unused = 1;
  ASSERT(1, attr_unused);
  ASSERT(4, sizeof(attr_aligned));
  ASSERT(0, ({ struct AttrMember x; x.a = 0; x.b = 1; x.a; }));
  attr_visibility();
  attr_format("%d", 1);

  ASSERT(5, ({ struct { char a; int b; } __attribute__((packed)) x; sizeof(x); }));
  ASSERT(0, offsetof(struct __attribute__((packed)) { char a; int b; }, a));
  ASSERT(1, offsetof(struct __attribute__((packed)) { char a; int b; }, b));

  ASSERT(5, ({ struct __attribute__((packed)) { char a; int b; } x; sizeof(x); }));
  ASSERT(0, offsetof(struct { char a; int b; } __attribute__((packed)), a));
  ASSERT(1, offsetof(struct { char a; int b; } __attribute__((packed)), b));

  ASSERT(9, ({ typedef struct { char a; int b[2]; } __attribute__((packed)) T; sizeof(T); }));
  ASSERT(9, ({ typedef struct __attribute__((packed)) { char a; int b[2]; } T; sizeof(T); }));

  ASSERT(1, offsetof(struct __attribute__((packed)) T { char a; int b[2]; }, b));
  ASSERT(1, _Alignof(struct __attribute__((packed)) { char a; int b[2]; }));

  ASSERT(8, ({ struct __attribute__((aligned(8))) { int a; } x; _Alignof(x); }));
  ASSERT(8, ({ struct { int a; } __attribute__((aligned(8))) x; _Alignof(x); }));

  ASSERT(8, ({ struct __attribute__((aligned(8), packed)) { char a; int b; } x; _Alignof(x); }));
  ASSERT(8, ({ struct { char a; int b; } __attribute__((aligned(8), packed)) x; _Alignof(x); }));
  ASSERT(1, offsetof(struct __attribute__((aligned(8), packed)) { char a; int b; }, b));
  ASSERT(1, offsetof(struct { char a; int b; } __attribute__((aligned(8), packed)), b));

  ASSERT(8, ({ struct __attribute__((aligned(8))) __attribute__((packed)) { char a; int b; } x; _Alignof(x); }));
  ASSERT(8, ({ struct { char a; int b; } __attribute__((aligned(8))) __attribute__((packed)) x; _Alignof(x); }));
  ASSERT(1, offsetof(struct __attribute__((aligned(8))) __attribute__((packed)) { char a; int b; }, b));
  ASSERT(1, offsetof(struct { char a; int b; } __attribute__((aligned(8))) __attribute__((packed)), b));

  ASSERT(8, ({ struct __attribute__((aligned(8))) { char a; int b; } __attribute__((packed)) x; _Alignof(x); }));
  ASSERT(1, offsetof(struct __attribute__((aligned(8))) { char a; int b; } __attribute__((packed)), b));

  ASSERT(16, ({ struct __attribute__((aligned(8+8))) { char a; int b; } x; _Alignof(x); }));

  printf("OK\n");
  return 0;
}
