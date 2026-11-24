#define _POSIX_C_SOURCE 200809L

#include "driver.h"

#include <assert.h>
#include <errno.h>
#include <glob.h>
#include <libgen.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
  FILE_NONE,
  FILE_C,
  FILE_ASM,
  FILE_OBJ,
  FILE_AR,
  FILE_DSO,
} FileType;

bool opt_fcommon = true;
bool opt_fpic;

static FileType opt_x;
static LibcpStringArray opt_include;
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
static char* opt_MF;
static char* opt_MT;
static char* opt_o;

static LibcpStringArray ld_extra_args;

char* base_file;
static char* output_file;
static LibcpContext* ctx;

static LibcpStringArray input_paths;
static LibcpStringArray tmpfiles;

// 驱动器本地工具 (issue #28): 公共面只暴露翻译单元需要的入口. printf
// 风格格式化, 字符串数组增长, 文件存在判定与致命诊断留在驱动器, 内存
// 随进程生命周期, 不进入库的 per-TU 登记链.
static char* format(char* fmt, ...) __attribute__((format(printf, 1, 2)));

static char* format(char* fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char* buf = calloc(1, len + 1);
  vsnprintf(buf, len + 1, fmt, ap);
  va_end(ap);
  return buf;
}

static void strarray_push(LibcpStringArray* arr, char* s) {
  if (!arr->data) {
    arr->data     = calloc(8, sizeof(char*));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->capacity *= 2;
    arr->data = realloc(arr->data, sizeof(char*) * arr->capacity);
  }

  arr->data[arr->len++] = s;
}

static bool file_exists(char* path) {
  struct stat st;
  return !stat(path, &st);
}

// 致命诊断: 与库内部的 error 路径共用同一套格式化与回调交付, 但驱动器
// 不在公共入口内, 出错时没有可以返回的翻译单元, 因此报告后结束进程.
static noreturn void error(char* fmt, ...) __attribute__((format(printf, 1, 2)));

static noreturn void error(char* fmt, ...) {
  va_list ap;
  va_list ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);

  char* msg = calloc(1, len + 1);
  vsnprintf(msg, len + 1, fmt, ap);
  va_end(ap);

  libcp_diagnostic_report(ctx, NULL, LIBCP_DIAG_ERROR, "%s", msg);
  exit(1);
}

static void usage(int status) {
  fprintf(stderr, "Usage: chibicc [options] file...\n");
  if (status != 0)
    fprintf(stderr, "Try 'chibicc --help' for more information.\n");
  exit(status);
}

// 帮助正文. 选项清单必须与 parse_args() 保持同步; test/driver.sh 会检查
// 每个受支持选项都出现在这里. 选项列的宽度固定为 26 个字符.
static const char help_text[] =
    "Usage: chibicc [options] file...\n"
    "       chibicc -h | --help\n"
    "\n"
    "输入文件: .c 为 C 源文件, .s 为汇编, .o 为目标文件, .a 为静态库,\n"
    ".so 为共享库; - 表示标准输入, 需要 -x 或 -E 指定语言. 默认动作是\n"
    "编译, 汇编并链接.\n"
    "\n"
    "输出与模式:\n"
    "  -c                        只编译并汇编, 不链接.\n"
    "  -S                        只编译, 输出汇编.\n"
    "  -E                        只预处理, 输出 C 源.\n"
    "  -o <path>                 输出到 <path>; 也接受 -o<path>.\n"
    "                            默认: 链接为 a.out, -c 为 <文件>.o, -S 为 <文件>.s,\n"
    "                            -E 与只读通道为标准输出.\n"
    "                            多个输入时, -o 只用于链接.\n"
    "  -x <lang>                 把所有输入当作 <lang>: c, assembler 或 none.\n"
    "                            也接受 -x<lang>; none 恢复按扩展名判断;\n"
    "                            最后一个 -x 生效; -E 总是按 C.\n"
    "  -###                      向 stderr 打印每个子进程命令行.\n"
    "  -h, --help                打印本帮助并退出.\n"
    "\n"
    "预处理:\n"
    "  -I <dir>                  把 <dir> 加入 include 搜索路径; 也接受 -I<dir>.\n"
    "  -idirafter <dir>          在所有 -I 目录之后加入 <dir>; 内建系统路径排在它之后.\n"
    "  -D <name>[=<value>]       定义 <name> 为 <value>, 省略 =<value> 时为 1;\n"
    "                            也接受 -D<name>[=<value>].\n"
    "  -U <name>                 取消 <name> 的定义; 也接受 -U<name>.\n"
    "  -include <file>           在主源文件之前处理 <file>.\n"
    "\n"
    "依赖生成:\n"
    "  -M                        写出 make 依赖后停止; 输出到 -MF, 否则 -o, 否则标准输出.\n"
    "  -MD                       写出依赖并继续编译; 输出到 -MF, 否则 <输出名>.d.\n"
    "  -MMD                      同 -MD, 但省略系统目录中的头文件.\n"
    "  -MF <file>                依赖写入 <file>.\n"
    "  -MT <target>              设置规则目标为 <target>; 可重复.\n"
    "  -MQ <target>              同 -MT, 但转义 make 特殊字符.\n"
    "  -MP                       为每个被包含的头文件生成伪目标.\n"
    "\n"
    "链接:\n"
    "  -l<lib>                   链接库 <lib>.\n"
    "  -L <dir>                  把 <dir> 加入库搜索路径; 也接受 -L<dir>.\n"
    "  -Wl,<arg>[,<arg>]         把逗号分隔的参数传给链接器.\n"
    "  -Xlinker <arg>            把一个参数传给链接器.\n"
    "  -s                        去掉符号表.\n"
    "  -static                   生成静态链接的可执行文件.\n"
    "  -shared                   生成共享对象.\n"
    "\n"
    "代码生成:\n"
    "  -fcommon                  暂定定义放入 common (默认).\n"
    "  -fno-common               暂定定义生成为零初始化定义.\n"
    "  -fpic, -fPIC              生成位置无关代码.\n"
    "\n"
    "接受但忽略:\n"
    "  -O<n>, -W<name>, -g<n>, -std=<std>\n"
    "  -ffreestanding, -fno-builtin, -fno-omit-frame-pointer,\n"
    "  -fno-stack-protector, -fno-strict-aliasing\n"
    "  -m64, -mno-red-zone, -w\n"
    "                            这些选项为兼容性接受, 没有效果. -O, -W, -g 的任意后缀\n"
    "                            都被忽略; -Wl,... 是链接选项, 不属于被忽略的警告选项.\n"
    "\n"
    "只读通道:\n"
    "  -dump-tokens              预处理前打印 token 与 trivia.\n"
    "  -reconstruct              按 token 流重建原始字节.\n"
    "  -dump-cst                 打印 lossless CST.\n"
    "  -reconstruct-cst          按 CST 叶子重建原始字节.\n"
    "  -verify-cst               校验 CST 叶子与 token 流.\n"
    "  -dump-expansions          打印宏展开 provenance.\n"
    "  -verify-expansions        校验 provenance 不变量.\n"
    "  -dump-ast                 在 sema 之前打印忠实 AST.\n"
    "                            这些通道在汇编与链接之前停止; 输出到 -o 或标准输出.\n"
    "\n"
    "内部选项:\n"
    "  -cc1                      在库前端中运行一个翻译单元.\n"
    "  -cc1-input <file>         设置 -cc1 的基文件.\n"
    "  -cc1-output <file>        设置 -cc1 的汇编输出.\n"
    "                            驱动器用它们重新调用自身; 不要直接使用.\n"
    "\n"
    "退出码: 成功为 0, 出错为 1.\n";

static void print_help(void) {
  printf("chibicc %s - C11 compiler driver (libcp)\n\n", LIBCP_VERSION);
  fputs(help_text, stdout);
  exit(0);
}

static bool take_arg(char* arg) {
  char* x[] = {
      "-o",
      "-I",
      "-idirafter",
      "-include",
      "-x",
      "-MF",
      "-MT",
      "-Xlinker",
  };

  for (int i = 0; i < sizeof(x) / sizeof(*x); i++)
    if (!strcmp(arg, x[i]))
      return true;
  return false;
}

static void add_default_include_paths(char* argv0) {
  // 假定 chibicc 专用的 include 文件安装在相对于 argv[0] 的 ./include 下.
  libcp_context_add_include_path_with_origin(ctx,
                                             format("%s/include", dirname(strdup(argv0))),
                                             LIBCP_INCLUDE_SYSTEM);

  // 添加标准 include 路径. 它们与 chibicc 自带的 include 目录都属于
  // SYSTEM 桶, 排在 -I 与 -idirafter 之后.
  libcp_context_add_include_path_with_origin(ctx, "/usr/local/include", LIBCP_INCLUDE_SYSTEM);
  libcp_context_add_include_path_with_origin(ctx,
                                             "/usr/include/x86_64-linux-gnu",
                                             LIBCP_INCLUDE_SYSTEM);
  libcp_context_add_include_path_with_origin(ctx, "/usr/include", LIBCP_INCLUDE_SYSTEM);
}

static void define(char* str) {
  char* eq = strchr(str, '=');
  if (eq) {
    char* name = strndup(str, eq - str);
    libcp_context_define(ctx, name, eq + 1);
  } else {
    libcp_context_define(ctx, str, "1");
  }
}

static FileType parse_opt_x(char* s) {
  if (!strcmp(s, "c"))
    return FILE_C;
  if (!strcmp(s, "assembler"))
    return FILE_ASM;
  if (!strcmp(s, "none"))
    return FILE_NONE;
  error("<command line>: unknown argument for -x: %s", s);
}

static char* quote_makefile(char* s) {
  char* buf = calloc(1, strlen(s) * 2 + 1);

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
      default: buf[j++] = s[i]; break;
    }
  }
  return buf;
}

static void parse_args(int argc, char** argv) {
  // -h / --help 优先于其他参数, 且不依赖已解析的配置.
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
      print_help();

  // 确保所有带参数的命令行选项都有参数.
  for (int i = 1; i < argc; i++)
    if (take_arg(argv[i]))
      if (!argv[++i])
        usage(1);

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-###")) {
      opt_hash_hash_hash = true;
      continue;
    }

    if (!strcmp(argv[i], "-cc1")) {
      opt_cc1 = true;
      continue;
    }

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

    if (!strcmp(argv[i], "-I")) {
      libcp_context_add_include_path(ctx, argv[++i]);
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
      strarray_push(&opt_include, argv[++i]);
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
      strarray_push(&input_paths, argv[i]);
      continue;
    }

    if (!strcmp(argv[i], "-Xlinker")) {
      strarray_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strcmp(argv[i], "-s")) {
      strarray_push(&ld_extra_args, "-s");
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
        opt_MT = format("%s %s", opt_MT, argv[++i]);
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
        opt_MT = format("%s %s", opt_MT, quote_makefile(argv[++i]));
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
      // 路径表按 origin 分桶, 不需要再收集后置缓冲.
      libcp_context_add_include_path_with_origin(ctx, argv[++i], LIBCP_INCLUDE_AFTER);
      continue;
    }

    if (!strcmp(argv[i], "-static")) {
      opt_static = true;
      strarray_push(&ld_extra_args, "-static");
      continue;
    }

    if (!strcmp(argv[i], "-shared")) {
      opt_shared = true;
      strarray_push(&ld_extra_args, "-shared");
      continue;
    }

    if (!strcmp(argv[i], "-L")) {
      strarray_push(&ld_extra_args, "-L");
      strarray_push(&ld_extra_args, argv[++i]);
      continue;
    }

    if (!strncmp(argv[i], "-L", 2)) {
      strarray_push(&ld_extra_args, "-L");
      strarray_push(&ld_extra_args, argv[i] + 2);
      continue;
    }

    // 这些选项目前被忽略.
    if (!strncmp(argv[i], "-O", 2) || !strncmp(argv[i], "-W", 2) || !strncmp(argv[i], "-g", 2) ||
        !strncmp(argv[i], "-std=", 5) || !strcmp(argv[i], "-ffreestanding") ||
        !strcmp(argv[i], "-fno-builtin") || !strcmp(argv[i], "-fno-omit-frame-pointer") ||
        !strcmp(argv[i], "-fno-stack-protector") || !strcmp(argv[i], "-fno-strict-aliasing") ||
        !strcmp(argv[i], "-m64") || !strcmp(argv[i], "-mno-red-zone") || !strcmp(argv[i], "-w"))
      continue;

    if (argv[i][0] == '-' && argv[i][1] != '\0')
      error("unknown argument: %s", argv[i]);

    strarray_push(&input_paths, argv[i]);
  }

  if (input_paths.len == 0)
    error("no input files");

  // -E 意味着输入是 C 宏语言, 因此按 C 源文件处理.
  if (opt_E)
    opt_x = FILE_C;
}

static FILE* open_file(char* path) {
  if (!path || strcmp(path, "-") == 0)
    return stdout;

  FILE* out = fopen(path, "w");
  if (!out)
    error("cannot open output file: %s: %s", path, strerror(errno));
  return out;
}

static bool endswith(char* p, char* q) {
  int len1 = strlen(p);
  int len2 = strlen(q);
  return (len1 >= len2) && !strcmp(p + len1 - len2, q);
}

// 替换文件扩展名.
static char* replace_extn(char* tmpl, char* extn) {
  char* filename = basename(strdup(tmpl));
  char* dot      = strrchr(filename, '.');
  if (dot)
    *dot = '\0';
  return format("%s%s", filename, extn);
}

static void cleanup(void) {
  for (int i = 0; i < tmpfiles.len; i++)
    unlink(tmpfiles.data[i]);
}

static char* create_tmpfile(void) {
  char* path = strdup("/tmp/chibicc-XXXXXX");
  int fd     = mkstemp(path);
  if (fd == -1)
    error("mkstemp failed: %s", strerror(errno));
  close(fd);

  strarray_push(&tmpfiles, path);
  return path;
}

static void run_subprocess(char** argv) {
  // 若给出 -###, 则打印子进程的命令行.
  if (opt_hash_hash_hash) {
    fprintf(stderr, "%s", argv[0]);
    for (int i = 1; argv[i]; i++)
      fprintf(stderr, " %s", argv[i]);
    fprintf(stderr, "\n");
  }

  pid_t pid = fork();
  if (pid < 0)
    error("fork failed: %s", strerror(errno));

  if (pid == 0) {
    // 子进程: 运行新命令.
    execvp(argv[0], argv);
    fprintf(stderr, "exec failed: %s: %s\n", argv[0], strerror(errno));
    _exit(1);
  }

  // 只等待这个子进程 - 宿主进程的其它子进程不能在这里被回收 - 并解码
  // 状态, 而不是把原始的 wait 状态字当作退出码 (C11). EINTR 会重试,
  // 不会被误认为等待结束.
  int status;
  while (waitpid(pid, &status, 0) == -1) {
    if (errno != EINTR)
      error("waitpid failed: %s", strerror(errno));
  }

  // 被信号终止和退出状态非零一样算失败.
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    exit(1);
}

static void run_cc1(int argc, char** argv, char* input, char* output) {
  char** args = calloc(argc + 10, sizeof(char*));
  memcpy(args, argv, argc * sizeof(char*));
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

// 打印 token, 默认写到 stdout, 给出 -o 时写该文件. 供 -E 使用.
static void print_tokens(LibcpToken* tok) {
  FILE* out = open_file(opt_o ? opt_o : "-");

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

// 给出 -M 或 -MD 时, 编译器按 make 能读的格式输出输入文件列表, 用于自动
// 管理文件依赖. 输出位置按 -MF, -MD 时的 .d 文件, -o, stdout 的顺序选择.
static void print_dependencies(void) {
  char* path;
  if (opt_MF)
    path = opt_MF;
  else if (opt_MD)
    path = replace_extn(opt_o ? opt_o : base_file, ".d");
  else if (opt_o)
    path = opt_o;
  else
    path = "-";

  FILE* out = open_file(path);
  if (opt_MT)
    fprintf(out, "%s:", opt_MT);
  else
    fprintf(out, "%s:", quote_makefile(replace_extn(base_file, ".o")));

  int count = libcp_context_input_file_count(ctx);

  for (int i = 0; i < count; i++) {
    LibcpFile* file = libcp_context_input_file_at(ctx, i);
    if (opt_MMD && libcp_context_include_is_system(ctx, file->name))
      continue;
    fprintf(out, " \\\n  %s", file->name);
  }

  fprintf(out, "\n\n");

  if (opt_MP) {
    for (int i = 1; i < count; i++) {
      LibcpFile* file = libcp_context_input_file_at(ctx, i);
      if (opt_MMD && libcp_context_include_is_system(ctx, file->name))
        continue;
      fprintf(out, "%s:\n\n", quote_makefile(file->name));
    }
  }
}

static LibcpToken* must_tokenize_file(char* path) {
  return libcp_tokenize_file(ctx, path);
}

static LibcpToken* append_tokens(LibcpToken* tok1, LibcpToken* tok2) {
  if (!tok1 || tok1->kind == TK_EOF)
    return tok2;

  LibcpToken* t = tok1;
  while (t->next->kind != TK_EOF)
    t = t->next;
  t->next = tok2;
  return tok1;
}

static int cc1(void) {
  // -dump-tokens, -reconstruct 和 CST 通道都作用于基文件的原始流:
  // trivia 契约以及建在它上面的 CST 都作用于 tokenize 的直接输出,
  // 即预处理之前的内容.
  if (opt_dump_tokens || opt_reconstruct || opt_dump_cst || opt_reconstruct_cst || opt_verify_cst) {
    LibcpToken* tok = must_tokenize_file(base_file);
    if (!tok)
      return 1;
    if (opt_dump_tokens)
      libcp_token_dump(ctx, tok, open_file(opt_o ? opt_o : "-"));
    if (opt_reconstruct)
      libcp_reconstruct(ctx, tok, open_file(opt_o ? opt_o : "-"));
    if (opt_dump_cst || opt_reconstruct_cst || opt_verify_cst) {
      LibcpCstNode* root = libcp_cst_parse(ctx, tok);
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

  LibcpToken* tok = NULL;

  // 处理 -include 选项.
  for (int i = 0; i < opt_include.len; i++) {
    char* incl = opt_include.data[i];

    // -include 走公共的 include 解析: 先按原样查文件, 再按 origin 顺序
    // 查路径表; 搜索细节归库内 include 模块.
    char* path = libcp_context_resolve_include(ctx, incl);
    if (!path)
      error("-include: %s: %s", incl, strerror(errno));

    LibcpToken* tok2 = must_tokenize_file(path);
    if (!tok2)
      return 1;
    tok = append_tokens(tok, tok2);
  }

  // 词法分析并预处理.
  LibcpToken* tok2 = must_tokenize_file(base_file);
  if (!tok2)
    return 1;
  tok = append_tokens(tok, tok2);
  tok = libcp_preprocess(ctx, tok);
  if (libcp_context_failed(ctx))
    return 1;

  // provenance 通道读取预处理后的流并在此停止: 不生成目标文件, 不调用
  // 汇编器, 不调用链接器.
  if (opt_dump_expansions || opt_verify_expansions) {
    if (opt_dump_expansions)
      libcp_expansion_dump(ctx, tok, open_file(opt_o ? opt_o : "-"));
    if (opt_verify_expansions && !libcp_expansion_verify(ctx, tok))
      return 1;
    return 0;
  }

  // 给出 -M 或 -MD 时, 打印文件依赖.
  if (opt_M || opt_MD) {
    print_dependencies();
    if (opt_M)
      return 0;
  }

  // 给出 -E 时, 输出预处理后的 C 代码作为结果.
  if (opt_E) {
    print_tokens(tok);
    return 0;
  }

  LibcpNode* ast = libcp_parse(ctx, tok);
  if (libcp_context_failed(ctx))
    return 1;

  // AST 形状通道停在第 3 层与第 4 层之间: 不做 sema, 不生成目标文件,
  // 不调用汇编器.
  if (opt_dump_ast) {
    libcp_ast_dump(ctx, ast, open_file(opt_o ? opt_o : "-"));
    return 0;
  }

  LibcpObj* prog = libcp_sema(ctx, ast);
  if (libcp_context_failed(ctx))
    return 1;

  // 打开临时输出缓冲区.
  char* buf;
  size_t buflen;
  FILE* output_buf = open_memstream(&buf, &buflen);

  // 遍历 AST, 发射汇编.
  codegen(ctx, prog, output_buf);
  fclose(output_buf);

  // 把汇编文本写入文件.
  FILE* out = open_file(output_file);
  fwrite(buf, buflen, 1, out);
  fclose(out);
  return 0;
}

static void assemble(char* input, char* output) {
  char* cmd[] = {"as", "-c", input, "-o", output, NULL};
  run_subprocess(cmd);
}

static char* find_file(char* pattern) {
  char* path = NULL;
  glob_t buf = {};
  glob(pattern, 0, NULL, &buf);
  if (buf.gl_pathc > 0)
    path = strdup(buf.gl_pathv[buf.gl_pathc - 1]);
  globfree(&buf);
  return path;
}

static char* find_libpath(void) {
  if (file_exists("/usr/lib/x86_64-linux-gnu/crti.o"))
    return "/usr/lib/x86_64-linux-gnu";
  if (file_exists("/usr/lib64/crti.o"))
    return "/usr/lib64";
  error("library path is not found");
}

static char* find_gcc_libpath(void) {
  char* paths[] = {
      "/usr/lib/gcc/x86_64-linux-gnu/*/crtbegin.o",
      "/usr/lib/gcc/x86_64-pc-linux-gnu/*/crtbegin.o",  // 用于 Gentoo.
      "/usr/lib/gcc/x86_64-redhat-linux/*/crtbegin.o",  // 用于 Fedora.
  };

  for (int i = 0; i < sizeof(paths) / sizeof(*paths); i++) {
    char* path = find_file(paths[i]);
    if (path)
      return dirname(path);
  }

  error("gcc library path is not found");
}

static void run_linker(LibcpStringArray* inputs, char* output) {
  LibcpStringArray arr = {};

  strarray_push(&arr, "ld");
  strarray_push(&arr, "-o");
  strarray_push(&arr, output);
  strarray_push(&arr, "-m");
  strarray_push(&arr, "elf_x86_64");

  char* libpath     = find_libpath();
  char* gcc_libpath = find_gcc_libpath();

  if (opt_shared) {
    strarray_push(&arr, format("%s/crti.o", libpath));
    strarray_push(&arr, format("%s/crtbeginS.o", gcc_libpath));
  } else {
    strarray_push(&arr, format("%s/crt1.o", libpath));
    strarray_push(&arr, format("%s/crti.o", libpath));
    strarray_push(&arr, format("%s/crtbegin.o", gcc_libpath));
  }

  strarray_push(&arr, format("-L%s", gcc_libpath));
  strarray_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(&arr, "-L/usr/lib64");
  strarray_push(&arr, "-L/lib64");
  strarray_push(&arr, "-L/usr/lib/x86_64-linux-gnu");
  strarray_push(&arr, "-L/usr/lib/x86_64-pc-linux-gnu");
  strarray_push(&arr, "-L/usr/lib/x86_64-redhat-linux");
  strarray_push(&arr, "-L/usr/lib");
  strarray_push(&arr, "-L/lib");

  if (!opt_static) {
    strarray_push(&arr, "-dynamic-linker");
    strarray_push(&arr, "/lib64/ld-linux-x86-64.so.2");
  }

  for (int i = 0; i < ld_extra_args.len; i++)
    strarray_push(&arr, ld_extra_args.data[i]);

  for (int i = 0; i < inputs->len; i++)
    strarray_push(&arr, inputs->data[i]);

  if (opt_static) {
    strarray_push(&arr, "--start-group");
    strarray_push(&arr, "-lgcc");
    strarray_push(&arr, "-lgcc_eh");
    strarray_push(&arr, "-lc");
    strarray_push(&arr, "--end-group");
  } else {
    strarray_push(&arr, "-lc");
    strarray_push(&arr, "-lgcc");
    strarray_push(&arr, "--as-needed");
    strarray_push(&arr, "-lgcc_s");
    strarray_push(&arr, "--no-as-needed");
  }

  if (opt_shared)
    strarray_push(&arr, format("%s/crtendS.o", gcc_libpath));
  else
    strarray_push(&arr, format("%s/crtend.o", gcc_libpath));

  strarray_push(&arr, format("%s/crtn.o", libpath));
  strarray_push(&arr, NULL);

  run_subprocess(arr.data);
}

static FileType get_file_type(char* filename) {
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

  error("<command line>: unknown file extension: %s", filename);
}

int main(int argc, char** argv) {
  atexit(cleanup);
  ctx = libcp_context_new();
  parse_args(argc, argv);
  // -D 宏体格式错误时不再由库内退出; 诊断已经打印, 因此像以前一样
  // 在这里停止.
  if (libcp_context_failed(ctx))
    exit(1);
  if (base_file)
    libcp_context_set_base_file(ctx, base_file);

  if (opt_cc1) {
    add_default_include_paths(argv[0]);
    return cc1();
  }

  if (input_paths.len > 1 && opt_o && (opt_c || opt_S || opt_E))
    error("cannot specify '-o' with '-c,' '-S' or '-E' with multiple files");

  LibcpStringArray ld_args = {};

  for (int i = 0; i < input_paths.len; i++) {
    char* input = input_paths.data[i];

    if (!strncmp(input, "-l", 2)) {
      strarray_push(&ld_args, input);
      continue;
    }

    if (!strncmp(input, "-Wl,", 4)) {
      char* s   = strdup(input + 4);
      char* arg = strtok(s, ",");
      while (arg) {
        strarray_push(&ld_args, arg);
        arg = strtok(NULL, ",");
      }
      continue;
    }

    char* output;
    if (opt_o)
      output = opt_o;
    else if (opt_S)
      output = replace_extn(input, ".s");
    else
      output = replace_extn(input, ".o");

    FileType type = get_file_type(input);

    // 处理 .o, .a 或 .so.
    if (type == FILE_OBJ || type == FILE_AR || type == FILE_DSO) {
      strarray_push(&ld_args, input);
      continue;
    }

    // 处理 .s.
    if (type == FILE_ASM) {
      if (!opt_S)
        assemble(input, output);
      continue;
    }

    assert(type == FILE_C);

    // 只运行 libcp_preprocess (或 dump/reconstruct 原始 token 流或 CST 流,
    // 或运行 provenance 通道).
    if (opt_E || opt_M || opt_dump_tokens || opt_reconstruct || opt_dump_cst ||
        opt_reconstruct_cst || opt_verify_cst || opt_dump_expansions || opt_verify_expansions ||
        opt_dump_ast) {
      run_cc1(argc, argv, input, NULL);
      continue;
    }

    // 编译.
    if (opt_S) {
      run_cc1(argc, argv, input, output);
      continue;
    }

    // 编译并汇编.
    if (opt_c) {
      char* tmp = create_tmpfile();
      run_cc1(argc, argv, input, tmp);
      assemble(tmp, output);
      continue;
    }

    // 编译, 汇编并链接.
    char* tmp1 = create_tmpfile();
    char* tmp2 = create_tmpfile();
    run_cc1(argc, argv, input, tmp1);
    assemble(tmp1, tmp2);
    strarray_push(&ld_args, tmp2);
    continue;
  }

  if (ld_args.len > 0)
    run_linker(&ld_args, opt_o ? opt_o : "a.out");
  return 0;
}
