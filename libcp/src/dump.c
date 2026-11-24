// dump.c - 只读的 dump 通道: 第 1 层 token 流与第 3 层 AST 的文本形式.
//
// 两个公共入口 (libcp_token_dump / libcp_ast_dump) 只写调用者给的
// FILE *: 不返回分配对象, 不选择处理阶段, 不 exit. 输出格式是公共契约,
// 分别由 test/trivia.sh 与 test/ast.sh 逐字节锁定. token 种类名, 转义
// 打印, 换行判定与缩进等 helper 继续是内部符号.

#include "libcp.h"

// 打印 token dump 的一行: 标签, 行号, 文件内字节偏移, 种类以及文本
// (对没有拼写的 token, 即 EOF, 省略文本). 文本按 C 字面量风格转义:
// \n, \t, \r 与 \\, 其余特殊字节为 \xHH.
static void print_dump_line(FILE* out,
                            const char* tag,
                            int line,
                            int offset,
                            const char* kind,
                            const char* text,
                            int len) {
  fprintf(out, "%s %d %d %s", tag, line, offset, kind);
  if (len) {
    fputc(' ', out);
    libcp_print_escaped(out, text, len);
  }
  fputc('\n', out);
}

// 返回从 p 开始跳过所有反斜杠续行之后的原始位置 (没有续行时返回 p).
static const char* skip_conts(const char* p, const char* end) {
  while (p + 1 < end && p[0] == '\\' && libcp_is_line_break(p + 1))
    p += 1 + libcp_line_break_len(p + 1);
  return p;
}

// 打印原始 token 流, 以及 token 之间被跳过的字节 (普通空格 dump 为
// \x20, 使每段文本各占一列). "T" 行是 token, "R" 行是 token 前面的一段
// 字节, 按词法分析器跳过的片段拆分: 行注释 (lc), 块注释 (bc), 换行 (nl),
// 一段空白 (ws), 反斜杠续行 (cont) 或文件开头的 BOM (bom). 一个 token 的
// 所有片段都在它的 "T" 行之前. 两者都用原始坐标, 因此 dump 出的字节正好
// 平铺整个文件.
static void print_token_dump(LibcpContext* ctx, const LibcpToken* tok, FILE* out) {
  for (; tok; tok = tok->next) {
    const char* raw = tok->file->raw;
    const char* p   = tok->trivia;
    const char* end = p + tok->trivia_len;

    while (p < end) {
      const char* q;
      const char* kind;

      if (p[0] == '\\' && p + 1 < end && libcp_is_line_break(p + 1)) {
        q    = p + 1 + libcp_line_break_len(p + 1);
        kind = "cont";
      } else if (libcp_is_line_break(p)) {
        q    = p + libcp_line_break_len(p);
        kind = "nl";
      } else if (p[0] == '/' && p + 1 < end && p[1] == '/') {
        // 行注释一直延伸到下一个不被续行连接的换行.
        for (q = p + 2; q < end;) {
          q = skip_conts(q, end);
          if (q >= end)
            break;
          if (libcp_is_line_break(q))
            break;
          q++;
        }
        kind = "lc";
      } else if (p[0] == '/' && p + 1 < end && p[1] == '*') {
        // 块注释是闭合的 (未闭合的块注释会被 libcp_tokenize() 拒绝).
        q = p + 2;
        for (;;) {
          q = skip_conts(q, end);
          if (q >= end)
            break;
          if (q[0] == '*') {
            const char* s = skip_conts(q + 1, end);
            if (s < end && s[0] == '/') {
              q = s + 1;
              break;
            }
          }
          q++;
        }
        kind = "bc";
      } else if (p == raw && !strncmp(p, "\xef\xbb\xbf", 3)) {
        q    = p + 3;
        kind = "bom";
      } else {
        assert(isspace((unsigned char)*p));
        for (q = p;
             q < end && !libcp_is_line_break(q) && q[0] != '\\' && isspace((unsigned char)q[0]);
             q++)
          ;
        kind = "ws";
      }

      print_dump_line(out, "R", tok->file->lines[p - raw], p - raw, kind, p, q - p);
      p = q;
    }

    print_dump_line(out,
                    "T",
                    tok->line_no,
                    tok->raw_loc - raw,
                    libcp_token_kind_name(ctx, tok->kind),
                    tok->raw_loc,
                    tok->raw_len);
  }
}

//
// -dump-ast: 忠实反映源码的语法树, 在 sema 看到它之前输出.
//
// 只读通道. 文本是确定性的: 不打印地址, 不遍历 map, 字段顺序固定,
// 因此只要两次构建把同一份源码解析成同一形状, 打印的 dump 就逐字节
// 相同. 宏展开产生的 token 打印为 @synth: 它的位置是宏体或合成缓冲区,
// 而不是表达式的源码位置.
//

static char* ast_node_kind_name(LibcpNodeKind kind) {
  switch (kind) {
    case ND_NULL_EXPR: return "ND_NULL_EXPR";
    case ND_ADD: return "ND_ADD";
    case ND_SUB: return "ND_SUB";
    case ND_MUL: return "ND_MUL";
    case ND_DIV: return "ND_DIV";
    case ND_NEG: return "ND_NEG";
    case ND_MOD: return "ND_MOD";
    case ND_BITAND: return "ND_BITAND";
    case ND_BITOR: return "ND_BITOR";
    case ND_BITXOR: return "ND_BITXOR";
    case ND_SHL: return "ND_SHL";
    case ND_SHR: return "ND_SHR";
    case ND_EQ: return "ND_EQ";
    case ND_NE: return "ND_NE";
    case ND_LT: return "ND_LT";
    case ND_LE: return "ND_LE";
    case ND_GT: return "ND_GT";
    case ND_GE: return "ND_GE";
    case ND_ASSIGN: return "ND_ASSIGN";
    case ND_INCDEC: return "ND_INCDEC";
    case ND_SUBSCRIPT: return "ND_SUBSCRIPT";
    case ND_COND: return "ND_COND";
    case ND_COMMA: return "ND_COMMA";
    case ND_MEMBER: return "ND_MEMBER";
    case ND_ADDR: return "ND_ADDR";
    case ND_DEREF: return "ND_DEREF";
    case ND_NOT: return "ND_NOT";
    case ND_BITNOT: return "ND_BITNOT";
    case ND_LOGAND: return "ND_LOGAND";
    case ND_LOGOR: return "ND_LOGOR";
    case ND_RETURN: return "ND_RETURN";
    case ND_IF: return "ND_IF";
    case ND_FOR: return "ND_FOR";
    case ND_WHILE: return "ND_WHILE";
    case ND_DO: return "ND_DO";
    case ND_SWITCH: return "ND_SWITCH";
    case ND_CASE: return "ND_CASE";
    case ND_BLOCK: return "ND_BLOCK";
    case ND_GOTO: return "ND_GOTO";
    case ND_BREAK: return "ND_BREAK";
    case ND_CONTINUE: return "ND_CONTINUE";
    case ND_GOTO_EXPR: return "ND_GOTO_EXPR";
    case ND_LABEL: return "ND_LABEL";
    case ND_LABEL_VAL: return "ND_LABEL_VAL";
    case ND_FUNCALL: return "ND_FUNCALL";
    case ND_EXPR_STMT: return "ND_EXPR_STMT";
    case ND_STMT_EXPR: return "ND_STMT_EXPR";
    case ND_IDENT: return "ND_IDENT";
    case ND_VAR: return "ND_VAR";
    case ND_VLA_PTR: return "ND_VLA_PTR";
    case ND_NUM: return "ND_NUM";
    case ND_STRING: return "ND_STRING";
    case ND_SIZEOF: return "ND_SIZEOF";
    case ND_ALIGNOF: return "ND_ALIGNOF";
    case ND_GENERIC: return "ND_GENERIC";
    case ND_GENERIC_ASSOC: return "ND_GENERIC_ASSOC";
    case ND_TYPES_COMPATIBLE: return "ND_TYPES_COMPATIBLE";
    case ND_REG_CLASS: return "ND_REG_CLASS";
    case ND_CAST: return "ND_CAST";
    case ND_MEMZERO: return "ND_MEMZERO";
    case ND_DECL: return "ND_DECL";
    case ND_GVAR_DECL: return "ND_GVAR_DECL";
    case ND_FUNCDEF: return "ND_FUNCDEF";
    case ND_TYPEDEF: return "ND_TYPEDEF";
    case ND_ENUM_CONST: return "ND_ENUM_CONST";
    case ND_COMPOUND_LITERAL: return "ND_COMPOUND_LITERAL";
    case ND_ASM: return "ND_ASM";
    case ND_CAS: return "ND_CAS";
    case ND_EXCH: return "ND_EXCH";
  }
  return "ND_UNKNOWN";
}

static char* ast_type_kind_name(LibcpTypeKind kind) {
  switch (kind) {
    case TY_VOID: return "void";
    case TY_BOOL: return "bool";
    case TY_CHAR: return "char";
    case TY_SHORT: return "short";
    case TY_INT: return "int";
    case TY_LONG: return "long";
    case TY_FLOAT: return "float";
    case TY_DOUBLE: return "double";
    case TY_LDOUBLE: return "ldouble";
    case TY_ENUM: return "enum";
    case TY_PTR: return "ptr";
    case TY_FUNC: return "func";
    case TY_ARRAY: return "array";
    case TY_VLA: return "vla";
    case TY_TYPEOF: return "typeof";
    case TY_STRUCT: return "struct";
    case TY_UNION: return "union";
  }
  return "type?";
}

// 类型中源码写出的部分: 种类, 基类型/返回类型链, 数组还包含声明的长度.
// 大小, 偏移和地址是 sema/codegen 的结论, 故意不打印.
static void ast_type_sig(FILE* out, const LibcpType* ty) {
  if (!ty) {
    fputc('-', out);
    return;
  }

  if (ty->is_atomic)
    fputs("atomic(", out);

  if (ty->is_unsigned &&
      (ty->kind == TY_CHAR || ty->kind == TY_SHORT || ty->kind == TY_INT || ty->kind == TY_LONG))
    fputc('u', out);

  switch (ty->kind) {
    case TY_PTR:
      fputs("ptr(", out);
      ast_type_sig(out, ty->base);
      fputc(')', out);
      break;
    case TY_ARRAY:
      fprintf(out, "array(%d,", ty->array_len);
      ast_type_sig(out, ty->base);
      fputc(')', out);
      break;
    case TY_VLA:
      fputs("vla(", out);
      ast_type_sig(out, ty->base);
      fputc(')', out);
      break;
    case TY_FUNC:
      fputs("func(", out);
      ast_type_sig(out, ty->return_ty);
      fputc(')', out);
      break;
    default: fputs(ast_type_kind_name(ty->kind), out); break;
  }

  if (ty->is_atomic)
    fputc(')', out);
}

static void ast_quoted(FILE* out, const char* p, int len) {
  fputc('"', out);
  for (int i = 0; i < len; i++) {
    switch (p[i]) {
      case '\n': fputs("\\n", out); break;
      case '\t': fputs("\\t", out); break;
      case '\r': fputs("\\r", out); break;
      case '\\': fputs("\\\\", out); break;
      case '"': fputs("\\\"", out); break;
      default: fputc(p[i], out); break;
    }
  }
  fputc('"', out);
}

static void ast_span(FILE* out, const LibcpToken* tok) {
  if (!tok) {
    fputs("@-", out);
    return;
  }

  if (tok->prov) {
    fputs("@synth", out);
    return;
  }

  const char* line = tok->loc;
  while (line > tok->file->contents && line[-1] != '\n')
    line--;
  fprintf(out, "@%s:%d:%d", tok->file->name, tok->line_no, (int)(tok->loc - line) + 1);
}

static void ast_token(FILE* out, const LibcpToken* tok) {
  if (!tok) {
    fputs("@- \"\"", out);
    return;
  }
  ast_span(out, tok);
  fputc(' ', out);
  ast_quoted(out, tok->loc, tok->len);
}

static void ast_indent(FILE* out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);
}

static void ast_fields(FILE* out, const LibcpNode* node) {
  LibcpNodeFamily fam = libcp_node_family(node->kind);

  switch (node->kind) {
    case ND_ASSIGN:
      if (node->assign.op)
        fprintf(out, " op=%s", ast_node_kind_name(node->assign.op));
      break;
    case ND_INCDEC:
      fprintf(out, " is_post=%d addend=%d", node->incdec.is_post, node->incdec.addend);
      break;
    case ND_COND:
      if (node->ctrl.is_elvis)
        fputs(" is_elvis=1", out);
      break;
    case ND_MEMBER:
      if (node->access.arrow_tok)
        fputs(" arrow=1", out);
      break;
    case ND_GOTO:
    case ND_LABEL:
    case ND_LABEL_VAL:
      if (node->jump.label) {
        fputs(" label=", out);
        ast_quoted(out, node->jump.label, strlen(node->jump.label));
      }
      break;
    case ND_BLOCK:
      if (node->decl.is_scope_block)
        fputs(" is_scope_block=1", out);
      break;
    case ND_CAST:
      if (node->value.is_implicit)
        fputs(" is_implicit=1", out);
      break;
    case ND_CASE:
      if (node->case_.is_default)
        fputs(" default=1", out);
      break;
    case ND_NUM:
      if (node->ty && libcp_is_flonum(node->ty))
        fprintf(out, " fval=%La", node->value.fval);
      else
        fprintf(out, " val=%lld", (long long)node->value.val);
      break;
    case ND_ASM:
      if (node->asm_.asm_str) {
        fputs(" asm_str=", out);
        ast_quoted(out, node->asm_.asm_str, strlen(node->asm_.asm_str));
      }
      break;
    default: break;
  }

  if (node->ty_op) {
    fputs(" ty_op=", out);
    ast_type_sig(out, node->ty_op);
  }

  if (fam == LIBCP_NF_VALUE && node->value.ty_op2) {
    fputs(" ty_op2=", out);
    ast_type_sig(out, node->value.ty_op2);
  }

  if (fam == LIBCP_NF_DECL && node->decl.name_tok) {
    fputs(" name=", out);
    ast_quoted(out, node->decl.name_tok->loc, node->decl.name_tok->len);
  }

  // 解析器读到的声明属性. 固化进 dump, 使存储类或对齐的偏差无需 sema
  // 就能看出来.
  if (node->kind == ND_DECL || node->kind == ND_GVAR_DECL || node->kind == ND_FUNCDEF) {
    char* sep = "";
    fputs(" attr=", out);
    if (node->decl.attr.is_typedef) {
      fputs("typedef", out);
      sep = ",";
    }
    if (node->decl.attr.is_static) {
      fprintf(out, "%sstatic", sep);
      sep = ",";
    }
    if (node->decl.attr.is_extern) {
      fprintf(out, "%sextern", sep);
      sep = ",";
    }
    if (node->decl.attr.is_inline) {
      fprintf(out, "%sinline", sep);
      sep = ",";
    }
    if (node->decl.attr.is_tls) {
      fprintf(out, "%stls", sep);
      sep = ",";
    }
    if (!*sep)
      fputs("none", out);
    if (node->decl.attr.align)
      fprintf(out, " align=%d", node->decl.attr.align);
    if (node->decl.attr.align_ty) {
      fputs(" align_ty=", out);
      ast_type_sig(out, node->decl.attr.align_ty);
    }
    if (node->decl.attr.align_expr)
      fputs(" align=expr", out);
  }
}

static char* ast_init_kind_name(int kind) {
  switch (kind) {
    case INIT_LIST: return "LIST";
    case INIT_EXPR: return "EXPR";
    case INIT_STR: return "STR";
  }
  return "?";
}

static void ast_node(FILE* out, const LibcpNode* node, int depth);
static void ast_initializer(FILE* out, const LibcpInitializer* init, int depth);

static void ast_chain(FILE* out, const LibcpNode* node, int depth) {
  for (; node; node = node->next)
    ast_node(out, node, depth);
}

static void ast_initializer(FILE* out, const LibcpInitializer* init, int depth) {
  if (!init)
    return;

  ast_indent(out, depth);
  fprintf(out, "INIT kind=%s ", ast_init_kind_name(init->kind));
  ast_span(out, init->tok);
  if (init->eq_tok) {
    fputs(" eq=", out);
    ast_span(out, init->eq_tok);
  }
  fputc('\n', out);

  if (init->kind == INIT_EXPR) {
    if (init->expr)
      ast_node(out, init->expr, depth + 1);
    return;
  }

  if (init->kind == INIT_STR) {
    ast_indent(out, depth + 1);
    fputs("STR ", out);
    ast_token(out, init->str_tok);
    fputc('\n', out);
    return;
  }

  for (const LibcpInitItem* item = init->items; item; item = item->next) {
    ast_indent(out, depth + 1);
    fputs("ITEM", out);
    if (item->comma_tok) {
      fputs(" comma=", out);
      ast_span(out, item->comma_tok);
    }
    fputc('\n', out);

    for (const LibcpInitDesig* d = item->desigs; d; d = d->next) {
      ast_indent(out, depth + 2);
      fputs("DESIG", out);
      if (d->name) {
        fputs(" name=", out);
        ast_quoted(out, d->name->loc, d->name->len);
        fputc(' ', out);
        ast_span(out, d->tok);
      } else {
        fputs(" idx ", out);
        ast_span(out, d->tok);
      }
      fputc('\n', out);
      if (d->begin)
        ast_node(out, d->begin, depth + 3);
      if (d->end)
        ast_node(out, d->end, depth + 3);
    }

    ast_initializer(out, item->init, depth + 2);
  }
}

static void ast_node(FILE* out, const LibcpNode* node, int depth) {
  if (!node)
    return;

  ast_indent(out, depth);
  fprintf(out, "%s ", ast_node_kind_name(node->kind));
  ast_token(out, node->tok);
  ast_fields(out, node);
  fputc('\n', out);

  // 子节点顺序由公共枚举决定: 列表字段沿 next 走链, 单个字段只打印
  // 一个节点. 初始化器记录与属性侧对齐表达式不是结构子节点, 在枚举
  // 之后单独打印.
  for (int i = 0, count = libcp_node_child_count(node); i < count; i++) {
    LibcpNodeChild child = libcp_node_child_at(node, i);
    if (child.is_list)
      ast_chain(out, child.node, depth + 1);
    else
      ast_node(out, child.node, depth + 1);
  }

  LibcpNodeFamily fam = libcp_node_family(node->kind);
  if (fam == LIBCP_NF_DECL && node->decl.decl_init)
    ast_initializer(out, node->decl.decl_init, depth + 1);
  if ((node->kind == ND_DECL || node->kind == ND_GVAR_DECL || node->kind == ND_FUNCDEF) &&
      node->decl.attr.align_expr)
    ast_node(out, node->decl.attr.align_expr, depth + 1);
}

// 公共的 token dump 入口. 只读输入, 只写 `out`; ctx 供 token 种类名与
// 错误边界使用 (dump 自身不分配).
void libcp_token_dump(LibcpContext* ctx, const LibcpToken* tok, FILE* out) {
  ENTER_VOID(ctx);
  print_token_dump(ctx, tok, out);
  LEAVE(ctx);
}

// 公共的 AST dump 入口: 顶层沿 next 链打印, 子节点按公共枚举下降.
void libcp_ast_dump(LibcpContext* ctx, const LibcpNode* node, FILE* out) {
  ENTER_VOID(ctx);
  ast_chain(out, node, 0);
  LEAVE(ctx);
}
