// 树外消费者验收 (issue #27, 决议 #14): 本文件只包含公共头
// <libcp/libcp.h>, 链接 libcp.a, 不包含内部伞头, 不链接 codegen.c,
// 也不认识仓库布局. 它演示两件事:
//
//   1. 真实降级与发射: 对 int f(int *p) { return *(p + 1); } 走完
//      tokenize / preprocess / parse / sema, 用公共类型信息把指针加法
//      缩放到字节偏移, 按 sema 树发射 x86-64 汇编并写到 stdout.
//   2. 诊断回调: 语法错误与语义错误各一次, 回调必须收到文件名, 消息
//      与锚点 token, 公共入口返回 NULL, 且 libcp_context_failed 为 true.
//
// 本消费者只覆盖验收输入所需的形状: 第一个形参是 `T *`, 函数体返回
// `*(p + <整数常量>)`, 加载 int. 其他形状显式失败, 不猜.
//
// 用法: consumer <good.c> <bad-syntax.c> <bad-semantic.c>
// 退出码为 0 表示两条路径都通过.

#include <libcp/libcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const LibcpNode* find_return(const LibcpNode* stmt) {
  for (; stmt; stmt = stmt->next)
    if (stmt->kind == ND_RETURN)
      return stmt;
  return NULL;
}

// 发射 `return *(p + i);`: p 必须是函数的第一个形参 (System V AMD64
// 的 %rdi), 索引必须是整数常量. 字节偏移来自公共类型记录 - 指针的
// base 大小乘以索引常量 - 不是写死的数字; 对 int *p, 偏移是 1 * 4.
static bool emit_return(FILE* out, const LibcpObj* fn, const LibcpNode* ret) {
  // sema 把 return 表达式包在到返回类型的隐式 cast 里; 跳过它.
  const LibcpNode* expr = ret->lhs;
  while (expr && expr->kind == ND_CAST && expr->value.is_implicit)
    expr = expr->lhs;
  if (!expr || expr->kind != ND_DEREF || !expr->lhs)
    return false;

  const LibcpNode* addr = expr->lhs;
  if (addr->kind != ND_ADD || !addr->lhs || !addr->rhs)
    return false;

  const LibcpNode* base = addr->lhs;
  const LibcpNode* index = addr->rhs;
  if (base->kind != ND_VAR || index->kind != ND_NUM)
    return false;

  // 第一个形参由调用约定放在 %rdi.
  if (!fn->name || !fn->params || base->decl.var != fn->params)
    return false;

  if (!base->ty || base->ty->kind != TY_PTR || !base->ty->base ||
      !expr->ty || expr->ty->kind != TY_INT)
    return false;

  long offset = (long)index->value.val * (long)base->ty->base->size;
  if (offset < 0)
    return false;

  fprintf(out, "  .globl %s\n", fn->name);
  fprintf(out, "%s:\n", fn->name);
  fprintf(out, "  mov %ld(%%rdi), %%eax\n", offset);
  fprintf(out, "  ret\n");
  return true;
}

static bool check_lowering(char* path) {
  LibcpContext* ctx = libcp_context_new();
  if (!ctx)
    return false;

  bool ok = false;
  LibcpToken* tok = libcp_tokenize_file(ctx, path);
  if (!tok)
    goto out;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    goto out;
  LibcpNode* ast = libcp_parse(ctx, tok);
  if (!ast || libcp_context_failed(ctx))
    goto out;
  LibcpObj* prog = libcp_sema(ctx, ast);
  if (!prog || libcp_context_failed(ctx))
    goto out;

  LibcpObj* fn = NULL;
  for (LibcpObj* obj = prog; obj; obj = obj->next) {
    if (obj->is_function && obj->body) {
      fn = obj;
      break;
    }
  }
  if (!fn || !fn->body || fn->body->kind != ND_BLOCK)
    goto out;

  const LibcpNode* ret = find_return(fn->body->decl.body);
  if (!ret)
    goto out;

  ok = emit_return(stdout, fn, ret);
out:
  libcp_context_free(ctx);
  return ok;
}

struct diag_capture {
  const char* path;
  int hits;
  bool fields_ok;
  const LibcpToken* tok;
};

static void capture_diag(void* user, const LibcpDiagnostic* d) {
  struct diag_capture* cap = user;
  cap->hits++;
  if (!d->filename || strcmp(d->filename, cap->path) != 0)
    cap->fields_ok = false;
  if (!d->message || d->message[0] == '\0')
    cap->fields_ok = false;
  if (d->severity != LIBCP_DIAG_ERROR)
    cap->fields_ok = false;
  if (d->tok)
    cap->tok = d->tok;
  else
    cap->fields_ok = false;
}

// 一个错误输入. `parse_fails` 区分两种路径: 语法错误让 libcp_parse
// 返回 NULL, 语义错误让 libcp_sema 返回 NULL. 两条路径都必须标记
// context, 并交给回调恰好一条带文件名的错误, 其中锚点 token 非 NULL.
static bool check_diagnostics(char* path, bool parse_fails) {
  LibcpContext* ctx = libcp_context_new();
  if (!ctx)
    return false;

  struct diag_capture cap = {path, 0, true, NULL};
  libcp_context_set_diagnostic(ctx, capture_diag, &cap);

  bool ok = false;
  LibcpToken* tok = libcp_tokenize_file(ctx, path);
  if (!tok)
    goto out;
  tok = libcp_preprocess(ctx, tok);
  if (!tok)
    goto out;
  LibcpNode* ast = libcp_parse(ctx, tok);
  if (parse_fails) {
    if (ast)
      goto out;
  } else {
    if (!ast || libcp_context_failed(ctx))
      goto out;
    if (libcp_sema(ctx, ast) != NULL)
      goto out;
  }
  if (!libcp_context_failed(ctx) || cap.hits != 1 || !cap.fields_ok ||
      !cap.tok)
    goto out;
  ok = true;
out:
  libcp_context_free(ctx);
  return ok;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr,
            "usage: %s <good.c> <bad-syntax.c> <bad-semantic.c>\n",
            argv[0]);
    return 2;
  }
  if (!check_lowering(argv[1])) {
    fprintf(stderr, "outside: lowering/emission failed\n");
    return 1;
  }
  if (!check_diagnostics(argv[2], true)) {
    fprintf(stderr, "outside: syntax diagnostic failed\n");
    return 1;
  }
  if (!check_diagnostics(argv[3], false)) {
    fprintf(stderr, "outside: semantic diagnostic failed\n");
    return 1;
  }
  return 0;
}
