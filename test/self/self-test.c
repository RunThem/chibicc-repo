// The library's unit self-tests, kept out of libchibicc. They link the
// archive directly and use its internal symbols (the .a symbol policy:
// consumers must not call them, but a test may). The compiler driver no
// longer carries -hashmap-test / -arena-test / -sema-state-test.
//
// test/self-test.sh compiles this file with the host cc against
// CHIBICC_LIB and runs it, so the same tests exercise the default,
// stage2, Pratt and stage2-Pratt archives.

#include "chibicc.h"

// Verifies the realloc contract with an active context: the tail is
// zeroed and the old bytes are preserved.
static void arena_test(ChibiccContext *ctx) {
  char *p = chibicc_arena_calloc(ctx, 4, 1);
  p[0] = 1;
  p[3] = 4;

  char *q = chibicc_arena_realloc(ctx, p, 4, 16);
  assert(q[0] == 1 && q[3] == 4);
  for (int i = 4; i < 16; i++)
    assert(q[i] == 0);

  char *r = chibicc_arena_realloc(ctx, NULL, 0, 8);
  for (int i = 0; i < 8; i++)
    assert(r[i] == 0);

  printf("arena: ok\n");
}

static void hashmap_test(ChibiccContext *ctx) {
  HashMap *map = chibicc_arena_calloc(ctx, 1, sizeof(HashMap));

  for (int i = 0; i < 5000; i++)
    hashmap_put(ctx, map, format(ctx, "key %d", i), (void *)(size_t)i);
  for (int i = 1000; i < 2000; i++)
    hashmap_delete(ctx, map, format(ctx, "key %d", i));
  for (int i = 1500; i < 1600; i++)
    hashmap_put(ctx, map, format(ctx, "key %d", i), (void *)(size_t)i);
  for (int i = 6000; i < 7000; i++)
    hashmap_put(ctx, map, format(ctx, "key %d", i), (void *)(size_t)i);

  for (int i = 0; i < 1000; i++)
    assert((size_t)hashmap_get(map, format(ctx, "key %d", i)) == i);
  for (int i = 1000; i < 1500; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 1500; i < 1600; i++)
    assert((size_t)hashmap_get(map, format(ctx, "key %d", i)) == i);
  for (int i = 1600; i < 2000; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 2000; i < 5000; i++)
    assert((size_t)hashmap_get(map, format(ctx, "key %d", i)) == i);
  for (int i = 5000; i < 6000; i++)
    assert(hashmap_get(map, "no such key") == NULL);
  for (int i = 6000; i < 7000; i++)
    hashmap_put(ctx, map, format(ctx, "key %d", i), (void *)(size_t)i);

  assert(hashmap_get(map, "no such key") == NULL);

  // Delete-heavy traffic must not leave tombstones behind: once they
  // outnumber the live entries the table rehashes (C8).
  HashMap *tomb = chibicc_arena_calloc(ctx, 1, sizeof(HashMap));
  for (int i = 0; i < 1000; i++)
    hashmap_put(ctx, tomb, format(ctx, "t %d", i), (void *)(size_t)i);
  for (int i = 0; i < 900; i++)
    hashmap_delete(ctx, tomb, format(ctx, "t %d", i));
  assert(tomb->ndeleted <= tomb->used - tomb->ndeleted);
  assert(hashmap_get(tomb, "t 0") == NULL);
  assert((size_t)hashmap_get(tomb, "t 999") == 999);
  hashmap_put(ctx, tomb, "t 0", (void *)123);
  assert((size_t)hashmap_get(tomb, "t 0") == 123);
  assert((size_t)hashmap_get(tomb, "t 999") == 999);

  printf("hashmap: ok\n");
}

// Runs two small translation units through the public facades without a
// context reset and checks that the second run starts from clean sema
// state: label_gotos holds one entry per function of the current run,
// the unique-name counter restarts at zero, and the returned globals
// list is the current TU's.
static bool sema_state_test(void) {
  char *first =
    "int f(void) { char *s = \"x\"; goto a; a: return s[0]; }\n"
    "int g(void) { goto b; b: return 2; }\n";
  char *second = "int h(void) { goto c; c: return 3; }\n";

  ChibiccContext *ctx = chibicc_context_new();
  bool ok = true;

  for (int round = 0; round < 2 && ok; round++) {
    char *src = round ? second : first;
    ChibiccToken *tok = chibicc_tokenize_buffer(ctx, "<sema-state>", src);
    if (tok)
      tok = chibicc_preprocess(ctx, tok);
    ChibiccNode *ast = tok ? chibicc_parse(ctx, tok) : NULL;
    ChibiccObj *prog = ast ? chibicc_sema(ctx, ast) : NULL;
    if (!prog) {
      ok = false;
      break;
    }

    int n = 0;
    for (LabelGotoList *l = ctx->label_gotos; l; l = l->next)
      n++;

    if (round == 0) {
      if (n != 2) {
        fprintf(stderr, "sema-state: first TU has %d goto lists, want 2\n", n);
        ok = false;
      }
    } else {
      if (n != 1) {
        fprintf(stderr, "sema-state: second TU has %d goto lists, want 1\n", n);
        ok = false;
      }

      // Only the current TU's objects may be in the list: its function
      // and the anonymous strings begin_function creates for __func__
      // and __FUNCTION__.
      int anon = 0;
      int fn_named = 0;
      for (ChibiccObj *o = prog; o; o = o->next) {
        if (!strcmp(o->name, "h"))
          fn_named++;
        else if (!strncmp(o->name, ".L..", 4))
          anon++;
        else {
          fprintf(stderr, "sema-state: stale global %s\n", o->name);
          ok = false;
          break;
        }
      }

      if (ok && fn_named != 1) {
        fprintf(stderr, "sema-state: second globals list has no h\n");
        ok = false;
      }

      // The counter restarted at zero and counted exactly this TU's
      // anonymous objects.
      if (ok && ctx->unique_name_id != anon) {
        fprintf(stderr, "sema-state: unique_name_id is %d, anon is %d\n",
                ctx->unique_name_id, anon);
        ok = false;
      }
    }
  }

  chibicc_context_free(ctx);
  if (ok)
    printf("sema-state: ok\n");
  return ok;
}

int main(void) {
  ChibiccContext *ctx = chibicc_context_new();
  arena_test(ctx);
  hashmap_test(ctx);
  chibicc_context_free(ctx);

  return sema_state_test() ? 0 : 1;
}
