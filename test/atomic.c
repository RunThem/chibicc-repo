#include "test.h"
#include <stdatomic.h>
#include <pthread.h>

static int incr(_Atomic int *p) {
  int oldval = *p;
  int newval;
  do {
    newval = oldval + 1;
  } while (!atomic_compare_exchange_weak(p, &oldval, newval));
  return newval;
}

static int add1(void *arg) {
  _Atomic int *x = arg;
  for (int i = 0; i < 1000*1000; i++)
    incr(x);
  return 0;
}

static int add2(void *arg) {
  _Atomic int *x = arg;
  for (int i = 0; i < 1000*1000; i++)
    (*x)++;
  return 0;
}

static int add3(void *arg) {
  _Atomic int *x = arg;
  for (int i = 0; i < 1000*1000; i++)
    *x += 5;
  return 0;
}

static int add_millions(void) {
  _Atomic int x = 0;

  pthread_t thr1;
  pthread_t thr2;
  pthread_t thr3;

  pthread_create(&thr1, NULL, add1, &x);
  pthread_create(&thr2, NULL, add2, &x);
  pthread_create(&thr3, NULL, add3, &x);

  for (int i = 0; i < 1000*1000; i++)
    x--;

  pthread_join(thr1, NULL);
  pthread_join(thr2, NULL);
  pthread_join(thr3, NULL);
  return x;
}

int main() {
  ASSERT(6*1000*1000, add_millions());

  ASSERT(3, ({ int x=3; atomic_exchange(&x, 5); }));
  ASSERT(5, ({ int x=3; atomic_exchange(&x, 5); x; }));

  // PLAN A10.2: the retry-loop forms, including a shift.
  ASSERT(8, ({ _Atomic int x=3; x += 5; x; }));
  ASSERT(4, ({ _Atomic int x=10; x -= 6; x; }));
  ASSERT(6, ({ _Atomic int x=3; x *= 2; x; }));
  ASSERT(0, ({ _Atomic int x=3; x >>= 2; x; }));

  // PLAN B2: an atomic pointer. The retry loop's addition combines the
  // old value with the operand already carrying the element scaling,
  // so one unit advances by one element and the scaling must not run a
  // second time - that would leave `p` at `a + 8` / `a - 6` here.
  ASSERT(1, ({ int a[5]; _Atomic(int *) p = a; p += 2; p == a + 2; }));
  ASSERT(1, ({ int a[5]; int *q = a + 4; _Atomic(int *) p = q; p -= 2; p == a + 2; }));

  printf("OK\n");
  return 0;
}
