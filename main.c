#include "driver.h"

typedef enum {
  FILE_NONE, FILE_C, FILE_ASM, FILE_OBJ, FILE_AR, FILE_DSO,
} FileType;

bool opt_fcommon = true;
bool opt_fpic;

static FileType opt_x;
static StringArray opt_include;
static bool opt_E;
static bool opt_M;
static bool opt_MD;
static bool opt_MMD;
static bool opt_MP;
static bool opt_S;
static bool opt_c;
static bool opt_cc1;
static bool opt_hash_hash_hash;
static bool opt_dump_tokens;
static bool opt_reconstruct;
static bool opt_dump_cst;
static bool opt_reconstruct_cst;
static bool opt_verify_cst;
static bool opt_dump_expansions;
static bool opt_verify_expansions;
static bool opt_dump_ast;
static bool opt_static;
static bool opt_shared;
static char *opt_MF;
static char *opt_MT;
static char *opt_o;

static StringArray ld_extra_args;
static StringArray std_include_paths;

char *base_file;
static char *output_file;
static LibcpContext *ctx;

static StringArray input_paths;
static StringArray tmpfiles;

static void usage(int status) {
  fprintf(stderr, "chibicc [ -o <path> ] <file>\n");
  exit(status);
}

static bool take_arg(char *arg) {
  char *x[] = {
    "-o", "-I", "-idirafter", "-include", "-x", "-MF", "-MT", "-Xlinker",
  };

  for (int i = 0; i < sizeof(x) / sizeof(*x); i++)
    if (!strcmp(arg, x[i]))
      return true;
  return false;
}

static void add_default_include_paths(char *argv0) {
  // We expect that chibicc-specific include files are installed
  // to ./include relative to argv[0].
  libcp_context_add_include_path(ctx, format(ctx, "%s/include", dirname(strdup(argv0))));

  // Add standard include paths.
  libcp_context_add_include_path(ctx, "/usr/local/include");
  libcp_context_add_include_path(ctx, "/usr/include/x86_64-linux-gnu");
  libcp_context_add_include_path(ctx, "/usr/include");

  // Keep a copy of the standard include paths for -MMD option.
  for (int i = 0; i < ctx->include_paths_len; i++)
    strarray_push(ctx, &std_include_paths, ctx->include_paths[i]);
}

static void define(char *str) {
  char *eq = strchr(str, '=');
  if (eq) {
    char *name = strndup(str, eq - str);
    libcp_context_define(ctx, name, eq + 1);
  } else {
    libcp_context_define(ctx, str, "1");
  }
}

static FileType parse_opt_x(char *s) {
  if (!strcmp(s, "c"))
    return FILE_C;
  if (!strcmp(s, "assembler"))
    return FILE_ASM;
  if (!strcmp(s, "none"))
    return FILE_NONE;
  error(ctx, "<command line>: unknown argument for -x: %s", s);
}

static char *quote_makefile(char *s) {
  char *buf = calloc(1, strlen(s) * 2 + 1);

  for (int i = 0, j = 0; s[i]; i++) {
    switch (s[i]) {
    case '$':
      buf[j++] = '$';
      buf[j++] = '$';
      break;
    case '#':
      buf[j++] = '\\';
      buf[j++] = '#';
      break;
    case ' ':
    case '\t':
      for (int k = i - 1; k >= 0 && s[k] == '\\'; k--)
        buf[j++] = '\\';
      buf[j++] = '\\';
      buf[j++] = s[i];
      break;
    default:
      buf[j++] = s[i];
      break;
    }
  }
  return buf;
}

static void parse_args(int argc, char **argv) {
  // Make sure that all command line options that take an argument
  // have an argument.
  for (int i = 1; i < argc; i++)
    if (take_arg(argv[i]))
      if (!argv[++i])
        usage(1);

  StringArray idirafter = {};

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-###")) {
      opt_hash_hash_hash = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1")) {
      opt_cc1 = true;
      continue;
    }

    if (!strcmp(argv[i], "--help"))
      usage(0);

    if (!strcmp(argv[i], "-o")) {
      opt_o = argv[++i];
      continue;
    }

    if (!strncmp(argv[i], "-o", 2)) {
      opt_o = argv[i] + 2;
      continue;
    }

    if (!strcmp(argv[i], "-S")) {
      opt_S = true;
      continue;
    }

    if (!strcmp(argv[i], "-fcommon")) {
      opt_fcommon = true;
      continue;
    }

    if (!strcmp(argv[i], "-fno-common")) {
      opt_fcommon = false;
      continue;
    }

    if (!strcmp(argv[i], "-c")) {
      opt_c = true;
      continue;
    }

    if (!strcmp(argv[i], "-E")) {
      opt_E = true;
      continue;
    }

    if (!strcmp(argv[i], "-dump-tokens")) {
      opt_dump_tokens = true;
      continue;
    }

    if (!strcmp(argv[i], "-reconstruct")) {
      opt_reconstruct = true;
      continue;
    }

    if (!strcmp(argv[i], "-dump-cst")) {
      opt_dump_cst = true;
      continue;
    }

    if (!strcmp(argv[i], "-reconstruct-cst")) {
      opt_reconstruct_cst = true;
      continue;
    }

    if (!strcmp(argv[i], "-verify-cst")) {
      opt_verify_cst = true;
      continue;
    }

    if (!strcmp(argv[i], "-dump-expansions")) {
      opt_dump_expansions = true;
      continue;
    }

    if (!strcmp(argv[i], "-verify-expansions")) {
      opt_verify_expansions = true;
      continue;
    }

    if (!strcmp(argv[i], "-dump-ast")) {
      opt_dump_ast = true;
      continue;
    }

    if (!strncmp(argv[i], "-I", 2)) {
      libcp_context_add_include_path(ctx, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-D")) {
      define(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-D", 2)) {
      define(argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-U")) {
      libcp_context_undef(ctx, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-U", 2)) {
      libcp_context_undef(ctx, argv[i] + 2);
      continue;
    }

    if (!strcmp(argv[i], "-include")) {
      strarray_push(ctx, &opt_include, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-x")) {
      opt_x = parse_opt_x(argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-x", 2)) {
      opt_x = parse_opt_x(argv[i] + 2);
      continue;
    }

    if (!strncmp(argv[i], "-l", 2) || !strncmp(argv[i], "-Wl,", 4)) {
      strarray_push(ctx, &input_paths, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-Xlinker")) {
      strarray_push(ctx, &ld_extra_args, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      strarray_push(ctx, &ld_extra_args, "-s");
      continue;
    }

    if (!strcmp(argv[i], "-M")) {
      opt_M = true;
      continue;
    }

    if (!strcmp(argv[i], "-MF")) {
      opt_MF = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-MP")) {
      opt_MP = true;
      continue;
    }

    if (!strcmp(argv[i], "-MT")) {
      if (opt_MT == NULL)
        opt_MT = argv[++i];
      else
        opt_MT = format(ctx, "%s %s", opt_MT, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-MD")) {
      opt_MD = true;
      continue;
    }

    if (!strcmp(argv[i], "-MQ")) {
      if (opt_MT == NULL)
        opt_MT = quote_makefile(argv[++i]);
      else
        opt_MT = format(ctx, "%s %s", opt_MT, quote_makefile(argv[++i]));
      continue;
    }

    if (!strcmp(argv[i], "-MMD")) {
      opt_MD = opt_MMD = true;
      continue;
    }

    if (!strcmp(argv[i], "-fpic") || !strcmp(argv[i], "-fPIC")) {
      opt_fpic = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1-input")) {
      base_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-cc1-output")) {
      output_file = argv[++i];
      continue;
    }

    if (!strcmp(argv[i], "-idirafter")) {
      strarray_push(ctx, &idirafter, argv[i++]);
      continue;
    }

    if (!strcmp(argv[i], "-static")) {
      opt_static = true;
      strarray_push(ctx, &ld_extra_args, "-static");
      continue;
    }

    if (!strcmp(argv[i], "-shared")) {
      opt_shared = true;
      strarray_push(ctx, &ld_extra_args, "-shared");
      continue;
    }

    if (!strcmp(argv[i], "-L")) {
      strarray_push(ctx, &ld_extra_args, "-L");
      strarray_push(ctx, &ld_extra_args, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-L", 2)) {
      strarray_push(ctx, &ld_extra_args, "-L");
      strarray_push(ctx, &ld_extra_args, argv[i] + 2);
      continue;
    }

    // These options are ignored for now.
    if (!strncmp(argv[i], "-O", 2) ||
        !strncmp(argv[i], "-W", 2) ||
        !strncmp(argv[i], "-g", 2) ||
        !strncmp(argv[i], "-std=", 5) ||
        !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-fno-builtin") ||
        !strcmp(argv[i], "-fno-omit-frame-pointer") ||
        !strcmp(argv[i], "-fno-stack-protector") ||
        !strcmp(argv[i], "-fno-strict-aliasing") ||
        !strcmp(argv[i], "-m64") ||
        !strcmp(argv[i], "-mno-red-zone") ||
        !strcmp(argv[i], "-w"))
      continue;

    if (argv[i][0] == '-' && argv[i][1] != '\0')
      error(ctx, "unknown argument: %s", argv[i]);

    strarray_push(ctx, &input_paths, argv[i]);
  }

  for (int i = 0; i < idirafter.len; i++)
    libcp_context_add_include_path(ctx, idirafter.data[i]);

  if (input_paths.len == 0)
    error(ctx, "no input files");

  // -E implies that the input is the C macro language.
  if (opt_E)
    opt_x = FILE_C;
}

static FILE *open_file(char *path) {
  if (!path || strcmp(path, "-") == 0)
    return stdout;

  FILE *out = fopen(path, "w");
  if (!out)
    error(ctx, "cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

static bool endswith(char *p, char *q) {
  int len1 = strlen(p);
  int len2 = strlen(q);
  return (len1 >= len2) && !strcmp(p + len1 - len2, q);
}

// Replace file extension
static char *replace_extn(char *tmpl, char *extn) {
  char *filename = basename(strdup(tmpl));
  char *dot = strrchr(filename, '.');
  if (dot)
    *dot = '\0';
  return format(ctx, "%s%s", filename, extn);
}

static void cleanup(void) {
  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
}

static char *create_tmpfile(void) {
  char *path = strdup("/tmp/chibicc-XXXXXX");
  int fd = mkstemp(path);
  if (fd == -1)
    error(ctx, "mkstemp failed: %s", strerror(errno));
  close(fd);

  strarray_push(ctx, &tmpfiles, path);
  return path;
}

static void run_subprocess(char **argv) {
  // If -### is given, dump the subprocess's command line.
  if (opt_hash_hash_hash) {
    fprintf(stderr, "%s", argv[0]);
    for (int i = 1; argv[i]; i++)
      fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
  }

  pid_t pid = fork();
  if (pid < 0)
    error(ctx, "fork failed: %s", strerror(errno));

  if (pid == 0) {
    // Child process. Run a new command.
    execvp(argv[0], argv);
    fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
    _exit(1);
  }

  // Wait for this child only - another child of the host must not be
  // reaped here - and decode the status instead of treating the raw
  // wait word as an exit code (C11). EINTR is retried, not mistaken for
  // the end of the wait.
  int status;
  while (waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR)
      error(ctx, "waitpid failed: %s", strerror(errno));
  }

  // A signal death is a failure just like a nonzero exit status.
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    exit(1);
}

static void run_cc1(int argc, char **argv, char *input, char *output) {
  char **args = calloc(argc + 10, sizeof(char *));
  memcpy(args, argv, argc * sizeof(char *));
  args[argc++] = "-cc1";

  if (input) {
    args[argc++] = "-cc1-input";
    args[argc++] = input;
  }

  if (output) {
    args[argc++] = "-cc1-output";
    args[argc++] = output;
  }

  run_subprocess(args);
}

// Print tokens to stdout. Used for -E.
static void print_tokens(Token *tok) {
  FILE *out = open_file(opt_o ? opt_o : "-");

  int line = 1;
  for (; tok->kind != TK_EOF; tok = tok->next) {
    if (line > 1 && tok->at_bol)
      fprintf(out, "\n");
    if (tok->has_space && !tok->at_bol)
      fprintf(out, " ");
    fprintf(out, "%.*s", tok->len, tok->loc);
    line++;
  }
  fprintf(out, "\n");
}

// Print bytes in C literal style: the escapes \n, \t, \r and \\, and
// Print one line of the token dump: a tag, the line number, the byte
// offset into the file, the kind, and the text (omitted for a token
// without a spelling, i.e. EOF).
static void print_dump_line(FILE *out, char *tag, int line, int offset,
                            char *kind, char *text, int len) {
  fprintf(out, "%s %d %d %s", tag, line, offset, kind);
  if (len) {
    fputc(' ', out);
    print_escaped(out, text, len);
  }
  fputc('\n', out);
}

// The raw position after any backslash-continuations starting at p (a
// no-op when there is none).
static char *skip_conts(char *p, char *end) {
  while (p + 1 < end && p[0] == '\\' && is_line_break(p + 1))
    p += 1 + line_break_len(p + 1);
  return p;
}

// Print the raw token stream with the bytes that were skipped between
// the tokens (plain spaces are dumped as \x20, so the texts stay one
// column each). "T" lines are tokens, "R" lines are the runs in front of
// a token, split into the pieces the lexer passed over: a line comment
// (lc), a block comment (bc), a line break (nl), a run of whitespace
// (ws), a backslash-continuation (cont) or the leading BOM (bom). All of
// a token's pieces come before its "T" line. Both are in raw
// coordinates, so the dumped bytes tile the file. Used by test/trivia.sh.
static void print_token_dump(Token *tok) {
  FILE *out = open_file(opt_o ? opt_o : "-");

  for (; tok; tok = tok->next) {
    char *raw = tok->file->raw;
    char *p = tok->trivia;
    char *end = p + tok->trivia_len;

    while (p < end) {
      char *q;
      char *kind;

      if (p[0] == '\\' && p + 1 < end && is_line_break(p + 1)) {
        q = p + 1 + line_break_len(p + 1);
        kind = "cont";
      } else if (is_line_break(p)) {
        q = p + line_break_len(p);
        kind = "nl";
      } else if (p[0] == '/' && p + 1 < end && p[1] == '/') {
        // The comment runs to the next break that a continuation does
        // not join over.
        for (q = p + 2; q < end;) {
          q = skip_conts(q, end);
          if (q >= end)
            break;
          if (is_line_break(q))
            break;
          q++;
        }
        kind = "lc";
      } else if (p[0] == '/' && p + 1 < end && p[1] == '*') {
        // The comment is closed (tokenize() rejects an unclosed one).
        q = p + 2;
        for (;;) {
          q = skip_conts(q, end);
          if (q >= end)
            break;
          if (q[0] == '*') {
            char *s = skip_conts(q + 1, end);
            if (s < end && s[0] == '/') {
              q = s + 1;
              break;
            }
          }
          q++;
        }
        kind = "bc";
      } else if (p == raw && !strncmp(p, "\xef\xbb\xbf", 3)) {
        q = p + 3;
        kind = "bom";
      } else {
        assert(isspace((unsigned char)*p));
        for (q = p; q < end && !is_line_break(q) && q[0] != '\\' &&
                    isspace((unsigned char)q[0]); q++)
          ;
        kind = "ws";
      }

      print_dump_line(out, "R", tok->file->lines[p - raw], p - raw, kind, p, q - p);
      p = q;
    }

    print_dump_line(out, "T", tok->line_no, tok->raw_loc - raw,
                    token_kind_name(ctx, tok->kind), tok->raw_loc, tok->raw_len);
  }
}


static bool in_std_include_path(char *path) {
  for (int i = 0; i < std_include_paths.len; i++) {
    char *dir = std_include_paths.data[i];
    int len = strlen(dir);
    if (strncmp(dir, path, len) == 0 && path[len] == '/')
      return true;
  }
  return false;
}

// If -M options is given, the compiler write a list of input files to
// stdout in a format that "make" command can read. This feature is
// used to automate file dependency management.
static void print_dependencies(void) {
  char *path;
  if (opt_MF)
    path = opt_MF;
  else if (opt_MD)
    path = replace_extn(opt_o ? opt_o : base_file, ".d");
  else if (opt_o)
    path = opt_o;
  else
    path = "-";

  FILE *out = open_file(path);
  if (opt_MT)
    fprintf(out, "%s:", opt_MT);
  else
    fprintf(out, "%s:", quote_makefile(replace_extn(base_file, ".o")));

  File **files = get_input_files(ctx);

  for (int i = 0; files[i]; i++) {
    if (opt_MMD && in_std_include_path(files[i]->name))
      continue;
    fprintf(out, " \\\n  %s", files[i]->name);
  }

  fprintf(out, "\n\n");

  if (opt_MP) {
    for (int i = 1; files[i]; i++) {
      if (opt_MMD && in_std_include_path(files[i]->name))
        continue;
      fprintf(out, "%s:\n\n", quote_makefile(files[i]->name));
    }
  }
}

static Token *must_tokenize_file(char *path) {
  return libcp_tokenize_file(ctx, path);
}

static Token *append_tokens(Token *tok1, Token *tok2) {
  if (!tok1 || tok1->kind == TK_EOF)
    return tok2;

  Token *t = tok1;
  while (t->next->kind != TK_EOF)
    t = t->next;
  t->next = tok2;
  return tok1;
}

//
// -dump-ast: the faithful syntax tree, before sema sees it.
//
// A read-only driver channel (PLAN-pratt-parse 4.5). The text is
// deterministic: no addresses, no map iteration and a fixed field
// order, so two builds that parse the same source to the same shape
// print byte-for-byte identical dumps. A token a macro expansion
// produced prints as @synth: its location is the macro body or a
// synthesized buffer, not the source position of the expression.
//

static char *ast_node_kind_name(NodeKind kind) {
  switch (kind) {
  case ND_NULL_EXPR:       return "ND_NULL_EXPR";
  case ND_ADD:             return "ND_ADD";
  case ND_SUB:             return "ND_SUB";
  case ND_MUL:             return "ND_MUL";
  case ND_DIV:             return "ND_DIV";
  case ND_NEG:             return "ND_NEG";
  case ND_MOD:             return "ND_MOD";
  case ND_BITAND:          return "ND_BITAND";
  case ND_BITOR:           return "ND_BITOR";
  case ND_BITXOR:          return "ND_BITXOR";
  case ND_SHL:             return "ND_SHL";
  case ND_SHR:             return "ND_SHR";
  case ND_EQ:              return "ND_EQ";
  case ND_NE:              return "ND_NE";
  case ND_LT:              return "ND_LT";
  case ND_LE:              return "ND_LE";
  case ND_GT:              return "ND_GT";
  case ND_GE:              return "ND_GE";
  case ND_ASSIGN:          return "ND_ASSIGN";
  case ND_INCDEC:          return "ND_INCDEC";
  case ND_SUBSCRIPT:       return "ND_SUBSCRIPT";
  case ND_COND:            return "ND_COND";
  case ND_COMMA:           return "ND_COMMA";
  case ND_MEMBER:          return "ND_MEMBER";
  case ND_ADDR:            return "ND_ADDR";
  case ND_DEREF:           return "ND_DEREF";
  case ND_NOT:             return "ND_NOT";
  case ND_BITNOT:          return "ND_BITNOT";
  case ND_LOGAND:          return "ND_LOGAND";
  case ND_LOGOR:           return "ND_LOGOR";
  case ND_RETURN:          return "ND_RETURN";
  case ND_IF:              return "ND_IF";
  case ND_FOR:             return "ND_FOR";
  case ND_WHILE:           return "ND_WHILE";
  case ND_DO:              return "ND_DO";
  case ND_SWITCH:          return "ND_SWITCH";
  case ND_CASE:            return "ND_CASE";
  case ND_BLOCK:           return "ND_BLOCK";
  case ND_GOTO:            return "ND_GOTO";
  case ND_BREAK:           return "ND_BREAK";
  case ND_CONTINUE:        return "ND_CONTINUE";
  case ND_GOTO_EXPR:       return "ND_GOTO_EXPR";
  case ND_LABEL:           return "ND_LABEL";
  case ND_LABEL_VAL:       return "ND_LABEL_VAL";
  case ND_FUNCALL:         return "ND_FUNCALL";
  case ND_EXPR_STMT:       return "ND_EXPR_STMT";
  case ND_STMT_EXPR:       return "ND_STMT_EXPR";
  case ND_IDENT:           return "ND_IDENT";
  case ND_VAR:             return "ND_VAR";
  case ND_VLA_PTR:         return "ND_VLA_PTR";
  case ND_NUM:             return "ND_NUM";
  case ND_STRING:          return "ND_STRING";
  case ND_SIZEOF:          return "ND_SIZEOF";
  case ND_ALIGNOF:         return "ND_ALIGNOF";
  case ND_GENERIC:         return "ND_GENERIC";
  case ND_GENERIC_ASSOC:   return "ND_GENERIC_ASSOC";
  case ND_TYPES_COMPATIBLE: return "ND_TYPES_COMPATIBLE";
  case ND_REG_CLASS:       return "ND_REG_CLASS";
  case ND_CAST:            return "ND_CAST";
  case ND_MEMZERO:         return "ND_MEMZERO";
  case ND_DECL:            return "ND_DECL";
  case ND_GVAR_DECL:       return "ND_GVAR_DECL";
  case ND_FUNCDEF:         return "ND_FUNCDEF";
  case ND_TYPEDEF:         return "ND_TYPEDEF";
  case ND_ENUM_CONST:      return "ND_ENUM_CONST";
  case ND_COMPOUND_LITERAL: return "ND_COMPOUND_LITERAL";
  case ND_ASM:             return "ND_ASM";
  case ND_CAS:             return "ND_CAS";
  case ND_EXCH:            return "ND_EXCH";
  }
  return "ND_UNKNOWN";
}

static char *ast_type_kind_name(TypeKind kind) {
  switch (kind) {
  case TY_VOID:    return "void";
  case TY_BOOL:    return "bool";
  case TY_CHAR:    return "char";
  case TY_SHORT:   return "short";
  case TY_INT:     return "int";
  case TY_LONG:    return "long";
  case TY_FLOAT:   return "float";
  case TY_DOUBLE:  return "double";
  case TY_LDOUBLE: return "ldouble";
  case TY_ENUM:    return "enum";
  case TY_PTR:     return "ptr";
  case TY_FUNC:    return "func";
  case TY_ARRAY:   return "array";
  case TY_VLA:     return "vla";
  case TY_TYPEOF:  return "typeof";
  case TY_STRUCT:  return "struct";
  case TY_UNION:   return "union";
  }
  return "type?";
}

// The source-written part of a type: its kind, the base/return chain and,
// for an array, the declared length. Sizes, offsets and addresses are
// sema/codegen conclusions and are deliberately not printed.
static void ast_type_sig(FILE *out, Type *ty) {
  if (!ty) {
    fputc('-', out);
    return;
  }

  if (ty->is_atomic)
    fputs("atomic(", out);

  if (ty->is_unsigned &&
      (ty->kind == TY_CHAR || ty->kind == TY_SHORT ||
       ty->kind == TY_INT || ty->kind == TY_LONG))
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
  default:
    fputs(ast_type_kind_name(ty->kind), out);
    break;
  }

  if (ty->is_atomic)
    fputc(')', out);
}

static void ast_quoted(FILE *out, char *p, int len) {
  fputc('"', out);
  for (int i = 0; i < len; i++) {
    switch (p[i]) {
    case '\n': fputs("\\n", out); break;
    case '\t': fputs("\\t", out); break;
    case '\r': fputs("\\r", out); break;
    case '\\': fputs("\\\\", out); break;
    case '"':  fputs("\\\"", out); break;
    default:   fputc(p[i], out); break;
    }
  }
  fputc('"', out);
}

static void ast_span(FILE *out, Token *tok) {
  if (!tok) {
    fputs("@-", out);
    return;
  }

  if (tok->prov) {
    fputs("@synth", out);
    return;
  }

  char *line = tok->loc;
  while (line > tok->file->contents && line[-1] != '\n')
    line--;
  fprintf(out, "@%s:%d:%d", tok->file->name, tok->line_no,
          (int)(tok->loc - line) + 1);
}

static void ast_token(FILE *out, Token *tok) {
  if (!tok) {
    fputs("@- \"\"", out);
    return;
  }
  ast_span(out, tok);
  fputc(' ', out);
  ast_quoted(out, tok->loc, tok->len);
}

static void ast_indent(FILE *out, int depth) {
  for (int i = 0; i < depth; i++)
    fputs("  ", out);
}

static void ast_fields(FILE *out, Node *node) {
  switch (node->kind) {
  case ND_ASSIGN:
    if (node->op)
      fprintf(out, " op=%s", ast_node_kind_name(node->op));
    break;
  case ND_INCDEC:
    fprintf(out, " is_post=%d addend=%d", node->is_post, node->addend);
    break;
  case ND_COND:
    if (node->is_elvis)
      fputs(" is_elvis=1", out);
    break;
  case ND_MEMBER:
    if (node->arrow_tok)
      fputs(" arrow=1", out);
    break;
  case ND_GOTO:
  case ND_LABEL:
  case ND_LABEL_VAL:
    if (node->label) {
      fputs(" label=", out);
      ast_quoted(out, node->label, strlen(node->label));
    }
    break;
  case ND_BLOCK:
    if (node->is_scope_block)
      fputs(" is_scope_block=1", out);
    break;
  case ND_CAST:
    if (node->is_implicit)
      fputs(" is_implicit=1", out);
    break;
  case ND_CASE:
    if (node->is_default)
      fputs(" default=1", out);
    break;
  case ND_NUM:
    if (node->ty && is_flonum(node->ty))
      fprintf(out, " fval=%La", node->fval);
    else
      fprintf(out, " val=%lld", (long long)node->val);
    break;
  case ND_ASM:
    if (node->asm_str) {
      fputs(" asm_str=", out);
      ast_quoted(out, node->asm_str, strlen(node->asm_str));
    }
    break;
  default:
    break;
  }

  if (node->ty_op) {
    fputs(" ty_op=", out);
    ast_type_sig(out, node->ty_op);
  }

  if (node->ty_op2) {
    fputs(" ty_op2=", out);
    ast_type_sig(out, node->ty_op2);
  }

  if (node->name_tok) {
    fputs(" name=", out);
    ast_quoted(out, node->name_tok->loc, node->name_tok->len);
  }

  // The declaration attributes the parser read. Frozen into the dump so
  // a storage-class or alignment drift is visible without sema.
  if (node->kind == ND_DECL || node->kind == ND_GVAR_DECL ||
      node->kind == ND_FUNCDEF) {
    char *sep = "";
    fputs(" attr=", out);
    if (node->attr.is_typedef) {
      fputs("typedef", out);
      sep = ",";
    }
    if (node->attr.is_static) {
      fprintf(out, "%sstatic", sep);
      sep = ",";
    }
    if (node->attr.is_extern) {
      fprintf(out, "%sextern", sep);
      sep = ",";
    }
    if (node->attr.is_inline) {
      fprintf(out, "%sinline", sep);
      sep = ",";
    }
    if (node->attr.is_tls) {
      fprintf(out, "%stls", sep);
      sep = ",";
    }
    if (!*sep)
      fputs("none", out);
    if (node->attr.align)
      fprintf(out, " align=%d", node->attr.align);
    if (node->attr.align_ty) {
      fputs(" align_ty=", out);
      ast_type_sig(out, node->attr.align_ty);
    }
    if (node->attr.align_expr)
      fputs(" align=expr", out);
  }
}

static char *ast_init_kind_name(int kind) {
  switch (kind) {
  case INIT_LIST: return "LIST";
  case INIT_EXPR: return "EXPR";
  case INIT_STR:  return "STR";
  }
  return "?";
}

static void ast_node(FILE *out, Node *node, int depth);
static void ast_initializer(FILE *out, Initializer *init, int depth);

static void ast_chain(FILE *out, Node *node, int depth) {
  for (; node; node = node->next)
    ast_node(out, node, depth);
}

static void ast_initializer(FILE *out, Initializer *init, int depth) {
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

  for (InitItem *item = init->items; item; item = item->next) {
    ast_indent(out, depth + 1);
    fputs("ITEM", out);
    if (item->comma_tok) {
      fputs(" comma=", out);
      ast_span(out, item->comma_tok);
    }
    fputc('\n', out);

    for (InitDesig *d = item->desigs; d; d = d->next) {
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

static void ast_node(FILE *out, Node *node, int depth) {
  if (!node)
    return;

  ast_indent(out, depth);
  fprintf(out, "%s ", ast_node_kind_name(node->kind));
  ast_token(out, node->tok);
  ast_fields(out, node);
  fputc('\n', out);

  if (node->lhs)
    ast_node(out, node->lhs, depth + 1);
  if (node->rhs)
    ast_node(out, node->rhs, depth + 1);
  if (node->cond)
    ast_node(out, node->cond, depth + 1);
  if (node->then)
    ast_node(out, node->then, depth + 1);
  if (node->els)
    ast_node(out, node->els, depth + 1);
  if (node->init)
    ast_chain(out, node->init, depth + 1);
  if (node->inc)
    ast_node(out, node->inc, depth + 1);
  if (node->body)
    ast_chain(out, node->body, depth + 1);
  if (node->args)
    ast_chain(out, node->args, depth + 1);
  if (node->spec_decls)
    ast_chain(out, node->spec_decls, depth + 1);
  if (node->begin_expr)
    ast_node(out, node->begin_expr, depth + 1);
  if (node->end_expr)
    ast_node(out, node->end_expr, depth + 1);
  if (node->cas_addr)
    ast_node(out, node->cas_addr, depth + 1);
  if (node->cas_old)
    ast_node(out, node->cas_old, depth + 1);
  if (node->cas_new)
    ast_node(out, node->cas_new, depth + 1);
  if (node->decl_init)
    ast_initializer(out, node->decl_init, depth + 1);
  if ((node->kind == ND_DECL || node->kind == ND_GVAR_DECL ||
       node->kind == ND_FUNCDEF) && node->attr.align_expr)
    ast_node(out, node->attr.align_expr, depth + 1);
}

static void print_ast_dump(Node *node) {
  ast_chain(open_file(opt_o ? opt_o : "-"), node, 0);
}

static int cc1(void) {
  // -dump-tokens, -reconstruct and the CST channels work on the raw stream
  // of the base file: the trivia contract, and the CST built on it, cover
  // tokenize()'s direct output, before preprocessing (plans/PLAN.md 判据 3).
  if (opt_dump_tokens || opt_reconstruct ||
      opt_dump_cst || opt_reconstruct_cst || opt_verify_cst) {
    Token *tok = must_tokenize_file(base_file);
    if (!tok)
      return 1;
    if (opt_dump_tokens)
      print_token_dump(tok);
    if (opt_reconstruct)
      libcp_reconstruct(ctx, tok, open_file(opt_o ? opt_o : "-"));
    if (opt_dump_cst || opt_reconstruct_cst || opt_verify_cst) {
      CstNode *root = libcp_cst_parse(ctx, tok);
      if (!root)
        return 1;
      if (opt_dump_cst)
        libcp_cst_dump(ctx, root, open_file(opt_o ? opt_o : "-"));
      if (opt_reconstruct_cst)
        libcp_cst_reconstruct(ctx, root, open_file(opt_o ? opt_o : "-"));
      if (opt_verify_cst && !libcp_cst_verify(ctx, root, tok))
        return 1;
    }
    return 0;
  }

  Token *tok = NULL;

  // Process -include option
  for (int i = 0; i < opt_include.len; i++) {
    char *incl = opt_include.data[i];

    char *path;
    if (file_exists(incl)) {
      path = incl;
    } else {
      path = search_include_paths(ctx, incl);
      if (!path)
        error(ctx, "-include: %s: %s", incl, strerror(errno));
    }

    Token *tok2 = must_tokenize_file(path);
    if (!tok2)
      return 1;
    tok = append_tokens(tok, tok2);
  }

  // Tokenize and parse.
  Token *tok2 = must_tokenize_file(base_file);
  if (!tok2)
    return 1;
  tok = append_tokens(tok, tok2);
  tok = libcp_preprocess(ctx, tok);
  if (ctx->failed)
    return 1;

  // The provenance channels read the preprocessed stream and stop there:
  // no object file, no assembler, no linker (plans/PLAN.md 判据 5).
  if (opt_dump_expansions || opt_verify_expansions) {
    if (opt_dump_expansions)
      libcp_expansion_dump(ctx, tok, open_file(opt_o ? opt_o : "-"));
    if (opt_verify_expansions && !libcp_expansion_verify(ctx, tok))
      return 1;
    return 0;
  }

  // If -M or -MD are given, print file dependencies.
  if (opt_M || opt_MD) {
    print_dependencies();
    if (opt_M)
      return 0;
  }

  // If -E is given, print out preprocessed C code as a result.
  if (opt_E) {
    print_tokens(tok);
    return 0;
  }

  Node *ast = libcp_parse(ctx, tok);
  if (ctx->failed)
    return 1;

  // The AST shape channel stops between layer 3 and layer 4: no sema,
  // no object file, no assembler.
  if (opt_dump_ast) {
    print_ast_dump(ast);
    return 0;
  }

  Obj *prog = libcp_sema(ctx, ast);
  if (ctx->failed)
    return 1;

  // Open a temporary output buffer.
  char *buf;
  size_t buflen;
  FILE *output_buf = open_memstream(&buf, &buflen);

  // Traverse the AST to emit assembly.
  codegen(ctx, prog, output_buf);
  fclose(output_buf);

  // Write the asembly text to a file.
  FILE *out = open_file(output_file);
  fwrite(buf, buflen, 1, out);
  fclose(out);
  return 0;
}

static void assemble(char *input, char *output) {
  char *cmd[] = {"as", "-c", input, "-o", output, NULL};
  run_subprocess(cmd);
}

static char *find_file(char *pattern) {
  char *path = NULL;
  glob_t buf = {};
  glob(pattern, 0, NULL, &buf);
  if (buf.gl_pathc > 0)
    path = strdup(buf.gl_pathv[buf.gl_pathc - 1]);
  globfree(&buf);
  return path;
}

static char *find_libpath(void) {
  if (file_exists("/usr/lib/x86_64-linux-gnu/crti.o"))
    return "/usr/lib/x86_64-linux-gnu";
  if (file_exists("/usr/lib64/crti.o"))
    return "/usr/lib64";
  error(ctx, "library path is not found");
}

static char *find_gcc_libpath(void) {
  char *paths[] = {
    "/usr/lib/gcc/x86_64-linux-gnu/*/crtbegin.o",
    "/usr/lib/gcc/x86_64-pc-linux-gnu/*/crtbegin.o", // For Gentoo
    "/usr/lib/gcc/x86_64-redhat-linux/*/crtbegin.o", // For Fedora
  };

  for (int i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
    char *path = find_file(paths[i]);
    if (path)
      return dirname(path);
  }

  error(ctx, "gcc library path is not found");
}

static void run_linker(StringArray *inputs, char *output) {
  StringArray arr = {};

  strarray_push(ctx, &arr, "ld");
  strarray_push(ctx, &arr, "-o");
  strarray_push(ctx, &arr, output);
  strarray_push(ctx, &arr, "-m");
  strarray_push(ctx, &arr, "elf_x86_64");

  char *libpath = find_libpath();
  char *gcc_libpath = find_gcc_libpath();

  if (opt_shared) {
    strarray_push(ctx, &arr, format(ctx, "%s/crti.o", libpath));
    strarray_push(ctx, &arr, format(ctx, "%s/crtbeginS.o", gcc_libpath));
  } else {
    strarray_push(ctx, &arr, format(ctx, "%s/crt1.o", libpath));
    strarray_push(ctx, &arr, format(ctx, "%s/crti.o", libpath));
    strarray_push(ctx, &arr, format(ctx, "%s/crtbegin.o", gcc_libpath));
  }

  strarray_push(ctx, &arr, format(ctx, "-L%s", gcc_libpath));
  strarray_push(ctx, &arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(ctx, &arr, "-L/usr/lib64");
  strarray_push(ctx, &arr, "-L/lib64");
  strarray_push(ctx, &arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(ctx, &arr, "-L/usr/lib/x86_64-pc-linux-gnu");
  strarray_push(ctx, &arr, "-L/usr/lib/x86_64-redhat-linux");
  strarray_push(ctx, &arr, "-L/usr/lib");
  strarray_push(ctx, &arr, "-L/lib");

  if (!opt_static) {
    strarray_push(ctx, &arr, "-dynamic-linker");
    strarray_push(ctx, &arr, "/lib64/ld-linux-x86-64.so.2");
  }

  for (int i = 0; i < ld_extra_args.len; i++)
    strarray_push(ctx, &arr, ld_extra_args.data[i]);

  for (int i = 0; i < inputs->len; i++)
    strarray_push(ctx, &arr, inputs->data[i]);

  if (opt_static) {
    strarray_push(ctx, &arr, "--start-group");
    strarray_push(ctx, &arr, "-lgcc");
    strarray_push(ctx, &arr, "-lgcc_eh");
    strarray_push(ctx, &arr, "-lc");
    strarray_push(ctx, &arr, "--end-group");
  } else {
    strarray_push(ctx, &arr, "-lc");
    strarray_push(ctx, &arr, "-lgcc");
    strarray_push(ctx, &arr, "--as-needed");
    strarray_push(ctx, &arr, "-lgcc_s");
    strarray_push(ctx, &arr, "--no-as-needed");
  }

  if (opt_shared)
    strarray_push(ctx, &arr, format(ctx, "%s/crtendS.o", gcc_libpath));
  else
    strarray_push(ctx, &arr, format(ctx, "%s/crtend.o", gcc_libpath));

  strarray_push(ctx, &arr, format(ctx, "%s/crtn.o", libpath));
  strarray_push(ctx, &arr, NULL);

  run_subprocess(arr.data);
}

static FileType get_file_type(char *filename) {
  if (opt_x != FILE_NONE)
    return opt_x;

  if (endswith(filename, ".a"))
    return FILE_AR;
  if (endswith(filename, ".so"))
    return FILE_DSO;
  if (endswith(filename, ".o"))
    return FILE_OBJ;
  if (endswith(filename, ".c"))
    return FILE_C;
  if (endswith(filename, ".s"))
    return FILE_ASM;

  error(ctx, "<command line>: unknown file extension: %s", filename);
}

int main(int argc, char **argv) {
  atexit(cleanup);
  ctx = libcp_context_new();
  parse_args(argc, argv);
  if (base_file)
    libcp_context_set_base_file(ctx, base_file);

  if (opt_cc1) {
    add_default_include_paths(argv[0]);
    return cc1();
  }

  if (input_paths.len > 1 && opt_o && (opt_c || opt_S || opt_E))
    error(ctx, "cannot specify '-o' with '-c,' '-S' or '-E' with multiple files");

  StringArray ld_args = {};

  for (int i = 0; i < input_paths.len; i++) {
    char *input = input_paths.data[i];

    if (!strncmp(input, "-l", 2)) {
      strarray_push(ctx, &ld_args, input);
      continue;
    }

    if (!strncmp(input, "-Wl,", 4)) {
      char *s = strdup(input + 4);
      char *arg = strtok(s, ",");
      while (arg) {
        strarray_push(ctx, &ld_args, arg);
        arg = strtok(NULL, ",");
      }
      continue;
    }

    char *output;
    if (opt_o)
      output = opt_o;
    else if (opt_S)
      output = replace_extn(input, ".s");
    else
      output = replace_extn(input, ".o");

    FileType type = get_file_type(input);

    // Handle .o or .a
    if (type == FILE_OBJ || type == FILE_AR || type == FILE_DSO) {
      strarray_push(ctx, &ld_args, input);
      continue;
    }

    // Handle .s
    if (type == FILE_ASM) {
      if (!opt_S)
        assemble(input, output);
      continue;
    }

    assert(type == FILE_C);

    // Just preprocess (or dump/reconstruct the raw token or CST stream,
    // or run a provenance channel)
    if (opt_E || opt_M || opt_dump_tokens || opt_reconstruct ||
        opt_dump_cst || opt_reconstruct_cst || opt_verify_cst ||
        opt_dump_expansions || opt_verify_expansions || opt_dump_ast) {
      run_cc1(argc, argv, input, NULL);
      continue;
    }

    // Compile
    if (opt_S) {
      run_cc1(argc, argv, input, output);
      continue;
    }

    // Compile and assemble
    if (opt_c) {
      char *tmp = create_tmpfile();
      run_cc1(argc, argv, input, tmp);
      assemble(tmp, output);
      continue;
    }

    // Compile, assemble and link
    char *tmp1 = create_tmpfile();
    char *tmp2 = create_tmpfile();
    run_cc1(argc, argv, input, tmp1);
    assemble(tmp1, tmp2);
    strarray_push(ctx, &ld_args, tmp2);
    continue;
  }

  if (ld_args.len > 0)
    run_linker(&ld_args, opt_o ? opt_o : "a.out");
  return 0;
}
