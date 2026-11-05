// Read-only macro-expansion provenance channels (plans/PLAN.md, 判据 5).
//
// stream:
//
// first violation. It never calls libcp_error() (判据 4), so the channel can
//
// 只读的宏展开 provenance 通道 (plans/PLAN.md, 判据 5): 打印与校验
// preprocess.c 在宏展开时建立的来源链. 本文件不属于编译路径.
//
// -dump-expansions 为预处理后的 token 流中每个非 EOF token 打印一行.
// <chain> 对从源码读入的 token 为 "-"; 否则按最内层在前的顺序列出该
// token 的 LibcpMacroProv 各层, 用 ">" 连接, 每层给出宏名, 调用位置,
// 宏体或从 1 开始计数的实参序号, 以及定义位置. 宏没有 #define token 时
// (内建宏, -D 定义) 定义位置打印 "?:?". 由拼接产生的宏名使调用位置
// 成为合成位置, 与其它合成 token 一样打印为 @synth.
//
// -verify-expansions 检查 plans/RESULT.md 中的链不变式 (I1-I3), 并在
// stderr 上最多打印一条消息: 成功时退出码 0, 第一处违规时退出码 1.
// 它从不调用 libcp_error() (判据 4), 因此该通道可以在损坏的流上运行,
// 而不会拖垮进程. 除 dump 之外, 它是这些访问器的唯一消费者. 两个通道
// 都不触碰编译路径.

#include "libcp.h"

// 合成缓冲区 (拼接, 字符串化, 内建值) 的 raw 就是它自身的文本;
// 真实文件的 raw 则是另一块分配.
static bool is_synthetic(LibcpToken* tok) {
  return tok->file->raw == tok->file->contents;
}

// 把 token 位置打印为 <file>:<line>; 位置不是真实文件的行时打印
// <file>:@synth. phys-line = line_no - line_delta, 因为
// libcp_preprocess() 最后会把 #line 偏移加到每个 token 的行号上.
static void print_pos(FILE* out, LibcpToken* tok) {
  if (is_synthetic(tok))
    fprintf(out, "%s:@synth", tok->file->name);
  else
    fprintf(out, "%s:%d", tok->file->name, tok->line_no - tok->line_delta);
}

// 按行打印展开流: 位置, 种类, 拼写与展开链 (无链时打印 "-").
void libcp_expansion_dump(LibcpContext* ctx, LibcpToken* tok, FILE* out) {
  ENTER_VOID(ctx);
  for (; tok->kind != TK_EOF; tok = tok->next) {
    fputs("T ", out);
    print_pos(out, tok);
    fprintf(out, " %s ", libcp_token_kind_name(ctx, tok->kind));
    libcp_print_escaped(out, tok->loc, tok->len);

    LibcpMacroProv* prov = libcp_token_provenance(tok);
    if (!prov) {
      fputs(" -\n", out);
      continue;
    }

    fputc(' ', out);
    for (LibcpMacroProv* p = prov; p; p = libcp_macro_prov_next(p)) {
      if (p != prov)
        fputc('>', out);

      LibcpMacroExpansion* e = libcp_macro_prov_expansion(p);
      fprintf(out, "%s@", libcp_macro_expansion_macro(e));
      print_pos(out, libcp_macro_expansion_name(e));

      int arg_index = libcp_macro_prov_arg_index(p);
      if (arg_index < 0)
        fputs(":body:", out);
      else
        fprintf(out, ":arg%d:", arg_index + 1);

      LibcpToken* def = libcp_macro_expansion_def(e);
      if (def)
        print_pos(out, def);
      else
        fputs("?:?", out);
    }
    fputc('\n', out);
  }
  LEAVE(ctx);
}

// 比这个深度更长的链按损坏处理, 而不是当作真实展开: 递归展开受
// hideset 约束, 真实链都很短, 这个上界防止损坏的链陷入死循环.
#define MAX_PROV_DEPTH 1000

// 实参列表从它的第一个 token 一直到 EOF 标记 (空实参就只有该标记).
// 这里加了上界, 使畸形的列表不会在此死循环.
static bool verify_arg_list(LibcpToken* tok) {
  int n = 0;
  for (; tok; tok = tok->next) {
    if (tok->kind == TK_EOF)
      return true;
    if (++n > 1 << 20)
      return false;
  }
  return false;
}

// 向 stderr 报告一处 provenance 违规, 前缀与驱动器诊断的 "chibicc: " 一致.
static void report(LibcpToken* tok, char* what) {
  fputs("chibicc: ", stderr);
  print_pos(stderr, tok);
  fprintf(stderr, ": invalid macro provenance: %s\n", what);
}

// I1: 链头是产生该 token 的展开, 它的层要么是宏体, 要么是合法的实参
// 下标. I2: 实参层必须指向一个类函数且非内建展开的已有实参, 并且每个
// 实参列表都良构. I3: 同一节点不得出现两次 (链只在前端插入, 并共享
// 不可变的尾部).
static bool verify_chain(LibcpToken* tok) {
  LibcpMacroProv* head = libcp_token_provenance(tok);
  int depth            = 0;

  for (LibcpMacroProv* p = head; p; p = libcp_macro_prov_next(p)) {
    if (++depth > MAX_PROV_DEPTH) {
      report(tok, "chain is too deep");
      return false;
    }

    int seen  = 0;
    int steps = 0;
    for (LibcpMacroProv* q = head; q; q = libcp_macro_prov_next(q)) {
      if (q == p)
        seen++;
      if (++steps > MAX_PROV_DEPTH) {
        report(tok, "chain is too deep");
        return false;
      }
    }
    if (seen > 1) {
      report(tok, "chain node appears twice");
      return false;
    }

    LibcpMacroExpansion* e = libcp_macro_prov_expansion(p);
    if (!e) {
      report(tok, "layer without an expansion");
      return false;
    }

    LibcpToken* name = libcp_macro_expansion_name(e);
    char* macro      = libcp_macro_expansion_macro(e);
    if (!name || !macro) {
      report(tok, "expansion without a name");
      return false;
    }

    if (strlen(macro) != (size_t)name->len || strncmp(macro, name->loc, name->len)) {
      report(tok, "macro name does not match the call token");
      return false;
    }

    int arg_index = libcp_macro_prov_arg_index(p);
    int arg_count = libcp_macro_expansion_arg_count(e);
    if (arg_index < -1 || arg_index >= arg_count) {
      report(tok, "argument index out of range");
      return false;
    }

    for (int i = 0; i < arg_count; i++)
      if (!verify_arg_list(libcp_macro_expansion_arg(e, i))) {
        report(tok, "malformed argument list");
        return false;
      }

    if (arg_index >= 0 &&
        (libcp_macro_expansion_end(e) == name || libcp_macro_expansion_is_builtin(e))) {
      report(tok, "argument layer on a non-function-like expansion");
      return false;
    }
  }
  return true;
}

// 校验流中每个携带 provenance 的 token 的展开链; 遇到第一处违规即返回 false.
bool libcp_expansion_verify(LibcpContext* ctx, LibcpToken* tok) {
  ENTER_RET(ctx, false);
  for (; tok->kind != TK_EOF; tok = tok->next)
    if (libcp_token_provenance(tok) && !verify_chain(tok)) {
      LEAVE(ctx);
      return false;
    }
  LEAVE(ctx);
  return true;
}
