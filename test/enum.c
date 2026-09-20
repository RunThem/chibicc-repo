#include "test.h"

// Bare file-scope definitions: the enum-constant records enter the
// top-level chain followed by non-enum declaration records.
enum { FILE_A = 3, FILE_B };
int file_enum_fn(void) { return FILE_B; }
enum { C1 };
enum { C2 = C1 + 1 };
typedef enum { TD_A = 1 } TD_E;

int main() {
  ASSERT(0, ({ enum { zero, one, two }; zero; }));
  ASSERT(1, ({ enum { zero, one, two }; one; }));
  ASSERT(2, ({ enum { zero, one, two }; two; }));
  ASSERT(5, ({ enum { five=5, six, seven }; five; }));
  ASSERT(6, ({ enum { five=5, six, seven }; six; }));
  ASSERT(0, ({ enum { zero, five=5, three=3, four }; zero; }));
  ASSERT(5, ({ enum { zero, five=5, three=3, four }; five; }));
  ASSERT(3, ({ enum { zero, five=5, three=3, four }; three; }));
  ASSERT(4, ({ enum { zero, five=5, three=3, four }; four; }));
  ASSERT(4, ({ enum { zero, one, two } x; sizeof(x); }));
  ASSERT(4, ({ enum t { zero, one, two }; enum t y; sizeof(y); }));
  ASSERT(4, file_enum_fn());
  ASSERT(1, C2);
  ASSERT(4, ({ TD_E x = TD_A; sizeof(x); }));

  {
    // An enum run followed by an extern-declaration record in a block
    // chain.
    enum { BLK_X = 9 };
    extern int unused_extern;
    ASSERT(9, BLK_X);
  }

  printf("OK\n");
  return 0;
}
