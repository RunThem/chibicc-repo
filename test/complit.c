#include "test.h"

typedef struct Tree {
  int val;
  struct Tree *lhs;
  struct Tree *rhs;
} Tree;

Tree *tree = &(Tree){
  1,
  &(Tree){
    2,
    &(Tree){ 3, 0, 0 },
    &(Tree){ 4, 0, 0 }
  },
  0
};

// File-scope compound literals have static storage duration, so each
// becomes an anonymous global - including the one that follows a
// function definition, which the resolve pass reaches at file scope
// again after descending into a body.
int *before = (int[]){ 1, 2 };
int mid(void) { return ((int[]){ 7 })[0]; }
int *after = (int[]){ 3, 4 };

int main() {
  ASSERT(1, (int){1});
  ASSERT(2, ((int[]){0,1,2})[2]);
  ASSERT('a', ((struct {char a; int b;}){'a', 3}).a);
  ASSERT(3, ({ int x=3; (int){x}; }));
  (int){3} = 5;

  ASSERT(1, tree->val);
  ASSERT(2, tree->lhs->val);
  ASSERT(3, tree->lhs->lhs->val);
  ASSERT(4, tree->lhs->rhs->val);

  ASSERT(1, before[0]);
  ASSERT(2, before[1]);
  ASSERT(7, mid());
  ASSERT(3, after[0]);
  ASSERT(4, after[1]);

  printf("OK\n");
  return 0;
}
