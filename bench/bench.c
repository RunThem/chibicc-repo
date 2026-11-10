// 前端 benchmark 的 harness (PLAN D1/D5): 对一个或多个文件顺序执行
// reset -> tokenize -> preprocess -> parse -> sema, 分阶段计时并采样
// 分配器统计, 输出固定列宽的表格. 只使用公共头; 分配器数字来自
// bench/alloc_shim.c, 与库一起链接进 bench/libcp-bench.
//
// 失败或缺失的语料不产生表格行, 诊断丢弃, 进程仍以 0 退出; 参数错误
// 以 2 退出.

#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include <libcp/libcp.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "alloc_shim.h"

enum {
  ST_RESET,
  ST_TOKENIZE,
  ST_PREPROCESS,
  ST_PARSE,
  ST_SEMA,
  ST_TOTAL,
  ST_COUNT,
};

static const char* stage_names[ST_COUNT] = {
    "reset", "tokenize", "preprocess", "parse", "sema", "total",
};

typedef struct {
  const char* group;
  char** files;
  int files_len;
  char** includes;
  int includes_len;
  char** defines;
  int defines_len;
  int repeat;
} Options;

// 一个文件的结果: 每阶段的 min / median 纳秒耗时与第一轮计时轮的
// 分配器采样, 以及预热轮统计的规模计数. 输出时时间按 ns -> us -> ms
// -> s -> m 自动选单位, 内存与文件大小按 B -> KB -> MB -> GB 自动选
// 单位, 都保留两位小数并把单位打印在单元格里.
typedef struct {
  const char* file;
  unsigned long long bytes;
  long tokens;
  long decls;
  uint64_t min_ns[ST_COUNT];
  uint64_t med_ns[ST_COUNT];
  BenchAllocStats mem[ST_COUNT];
} FileResult;

static void usage(void) {
  fprintf(stderr,
          "usage: bench/libcp-bench --group NAME [--repeat N] [--include DIR]...\n"
          "                        [--define NAME[=VALUE]]... [--file PATH]...\n");
}

static void push(char*** arr, int* len, char* item) {
  char** grown = realloc(*arr, sizeof(char*) * (size_t)(*len + 1));
  if (!grown) {
    fprintf(stderr, "bench: out of memory\n");
    exit(2);
  }
  grown[(*len)++] = item;
  *arr            = grown;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
  uint64_t x = *(const uint64_t*)a;
  uint64_t y = *(const uint64_t*)b;
  return (x > y) - (x < y);
}

// 语料自己的诊断一律丢弃: 失败的文件只是不出现在表格里.
static void discard_diagnostic(void* user, const LibcpDiagnostic* diag) {
  (void)user;
  (void)diag;
}

static void setup_context(LibcpContext* ctx, const Options* opt) {
  libcp_context_set_diagnostic(ctx, discard_diagnostic, NULL);

  for (int i = 0; i < opt->includes_len; i++)
    libcp_context_add_include_path(ctx, opt->includes[i]);

  for (int i = 0; i < opt->defines_len; i++) {
    char* def = opt->defines[i];
    char* eq  = strchr(def, '=');
    if (eq) {
      *eq = '\0';
      libcp_context_define(ctx, def, eq + 1);
      *eq = '=';
    } else {
      libcp_context_define(ctx, def, "1");
    }
  }
}

static long count_tokens(const LibcpToken* tok) {
  long n = 0;
  for (; tok; tok = tok->next)
    if (tok->kind != TK_EOF)
      n++;
  return n;
}

static long count_decls(const LibcpNode* node) {
  long n = 0;
  for (; node; node = node->next)
    n++;
  return n;
}

// 跑一遍完整流水线. 成功返回 true. times 非 NULL 时写入五个阶段的
// 纳秒耗时; mem 非 NULL 时写入各阶段的分配器采样; tokens / decls 非
// NULL 时写入规模计数(预热轮使用).
static bool run_pipeline(LibcpContext* ctx,
                         const char* path,
                         uint64_t times[ST_COUNT],
                         BenchAllocStats mem[ST_COUNT],
                         long* tokens,
                         long* decls) {
  bench_alloc_reset_counters();
  bench_alloc_reset_peak();
  uint64_t t0 = now_ns();
  libcp_context_reset(ctx);
  uint64_t t1 = now_ns();
  if (times)
    times[ST_RESET] = t1 - t0;
  if (mem)
    mem[ST_RESET] = bench_alloc_snapshot();

  bench_alloc_reset_counters();
  bench_alloc_reset_peak();
  t0                = now_ns();
  LibcpToken* tok   = libcp_tokenize_file(ctx, (char*)path);
  t1                = now_ns();
  if (!tok || libcp_context_failed(ctx))
    return false;
  if (times)
    times[ST_TOKENIZE] = t1 - t0;
  if (mem)
    mem[ST_TOKENIZE] = bench_alloc_snapshot();
  if (tokens)
    *tokens = count_tokens(tok);

  bench_alloc_reset_counters();
  bench_alloc_reset_peak();
  t0 = now_ns();
  tok = libcp_preprocess(ctx, tok);
  t1  = now_ns();
  if (!tok || libcp_context_failed(ctx))
    return false;
  if (times)
    times[ST_PREPROCESS] = t1 - t0;
  if (mem)
    mem[ST_PREPROCESS] = bench_alloc_snapshot();

  bench_alloc_reset_counters();
  bench_alloc_reset_peak();
  t0              = now_ns();
  LibcpNode* node = libcp_parse(ctx, tok);
  t1              = now_ns();
  if (libcp_context_failed(ctx))
    return false;
  if (times)
    times[ST_PARSE] = t1 - t0;
  if (mem)
    mem[ST_PARSE] = bench_alloc_snapshot();
  if (decls)
    *decls = count_decls(node);

  bench_alloc_reset_counters();
  bench_alloc_reset_peak();
  t0 = now_ns();
  libcp_sema(ctx, node);
  t1 = now_ns();
  if (libcp_context_failed(ctx))
    return false;
  if (times)
    times[ST_SEMA] = t1 - t0;
  if (mem)
    mem[ST_SEMA] = bench_alloc_snapshot();

  return true;
}

static bool measure_file(const Options* opt, const char* path, FileResult* out) {
  memset(out, 0, sizeof *out);
  out->file = path;

  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  out->bytes = (unsigned long long)st.st_size;

  LibcpContext* ctx = libcp_context_new();
  setup_context(ctx, opt);

  // 预热 1 轮: 统计规模计数, 不计时.
  if (!run_pipeline(ctx, path, NULL, NULL, &out->tokens, &out->decls)) {
    libcp_context_free(ctx);
    return false;
  }

  int repeat        = opt->repeat;
  uint64_t* samples = calloc((size_t)ST_COUNT * (size_t)repeat, sizeof(uint64_t));
  uint64_t* tmp     = calloc((size_t)repeat, sizeof(uint64_t));
  if (!samples || !tmp) {
    free(samples);
    free(tmp);
    libcp_context_free(ctx);
    return false;
  }

  bool ok = true;
  for (int r = 0; r < repeat && ok; r++) {
    uint64_t times[ST_COUNT] = {0};
    BenchAllocStats mem[ST_COUNT];
    memset(mem, 0, sizeof mem);

    if (!run_pipeline(ctx, path, times, mem, NULL, NULL)) {
      ok = false;
      break;
    }

    uint64_t total = 0;
    for (int s = 0; s < ST_TOTAL; s++) {
      samples[s * repeat + r] = times[s];
      total += times[s];
    }
    samples[ST_TOTAL * repeat + r] = total;
    if (r == 0)
      memcpy(out->mem, mem, sizeof mem);
  }

  if (ok) {
    for (int s = 0; s < ST_COUNT; s++) {
      const uint64_t* v = &samples[s * repeat];
      uint64_t lo       = v[0];
      for (int r = 1; r < repeat; r++)
        if (v[r] < lo)
          lo = v[r];

      memcpy(tmp, v, sizeof(uint64_t) * (size_t)repeat);
      qsort(tmp, (size_t)repeat, sizeof(uint64_t), cmp_u64);
      uint64_t med = repeat % 2 ? tmp[repeat / 2]
                                : (tmp[repeat / 2 - 1] + tmp[repeat / 2]) / 2;

      out->min_ns[s] = lo;
      out->med_ns[s] = med;
    }

    for (int s = 0; s < ST_TOTAL; s++) {
      out->mem[ST_TOTAL].bytes += out->mem[s].bytes;
      out->mem[ST_TOTAL].count += out->mem[s].count;
      if (out->mem[ST_TOTAL].peak < out->mem[s].peak)
        out->mem[ST_TOTAL].peak = out->mem[s].peak;
    }
  }

  free(samples);
  free(tmp);
  libcp_context_free(ctx);
  return ok;
}

static void print_header(void) {
  printf("%-8s %-34s %-10s %11s %11s %11s %11s %11s %11s %9s %7s\n",
         "group",
         "file",
         "stage",
         "min",
         "med",
         "alloc",
         "calls",
         "peak",
         "size",
         "tokens",
         "decls");
}

static void fmt_number(char* buf, size_t size, bool present, unsigned long long value) {
  if (present)
    snprintf(buf, size, "%llu", value);
  else
    snprintf(buf, size, "-");
}

// 时间: ns 起步, 每 1000 升一档(ns -> us -> ms -> s), 到 s 后按 60
// 升到 m.
static void fmt_time(char* buf, size_t size, bool present, double ns) {
  if (!present) {
    snprintf(buf, size, "-");
    return;
  }

  static const char* units[] = {"ns", "us", "ms", "s", "m"};
  int unit                   = 0;
  while (ns >= 1000.0 && unit < 3) {
    ns /= 1000.0;
    unit++;
  }
  if (unit == 3 && ns >= 60.0) {
    ns /= 60.0;
    unit = 4;
  }
  snprintf(buf, size, "%.2f %s", ns, units[unit]);
}

// 大小: B 起步, 每 1024 升一档.
static void fmt_size(char* buf, size_t size, bool present, double bytes) {
  if (!present) {
    snprintf(buf, size, "-");
    return;
  }

  static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit                   = 0;
  while (bytes >= 1024.0 && unit < 4) {
    bytes /= 1024.0;
    unit++;
  }
  snprintf(buf, size, "%.2f %s", bytes, units[unit]);
}

static void print_row(const Options* opt,
                      const char* file,
                      int stage,
                      uint64_t min_ns,
                      bool has_med,
                      uint64_t med_ns,
                      const BenchAllocStats* mem,
                      bool has_scale,
                      unsigned long long bytes,
                      long tokens,
                      long decls) {
  char min_buf[32], med_buf[32], alloc_buf[32], calls_buf[32], peak_buf[32];
  char size_buf[32], token_buf[32], decl_buf[32];
  fmt_time(min_buf, sizeof min_buf, true, (double)min_ns);
  fmt_time(med_buf, sizeof med_buf, has_med, (double)med_ns);
  fmt_size(alloc_buf, sizeof alloc_buf, true, (double)mem->bytes);
  fmt_number(calls_buf, sizeof calls_buf, true, mem->count);
  fmt_size(peak_buf, sizeof peak_buf, true, (double)mem->peak);
  fmt_size(size_buf, sizeof size_buf, has_scale, (double)bytes);
  fmt_number(token_buf, sizeof token_buf, has_scale, (unsigned long long)tokens);
  fmt_number(decl_buf, sizeof decl_buf, has_scale, (unsigned long long)decls);

  printf("%-8s %-34s %-10s %11s %11s %11s %11s %11s %11s %9s %7s\n",
         opt->group,
         file,
         stage_names[stage],
         min_buf,
         med_buf,
         alloc_buf,
         calls_buf,
         peak_buf,
         size_buf,
         token_buf,
         decl_buf);
}

static void print_file(const Options* opt, const FileResult* r) {
  for (int s = 0; s < ST_TOTAL; s++)
    print_row(opt,
              r->file,
              s,
              r->min_ns[s],
              true,
              r->med_ns[s],
              &r->mem[s],
              s == ST_TOKENIZE,
              r->bytes,
              r->tokens,
              r->decls);

  print_row(opt,
            r->file,
            ST_TOTAL,
            r->min_ns[ST_TOTAL],
            true,
            r->med_ns[ST_TOTAL],
            &r->mem[ST_TOTAL],
            false,
            0,
            0,
            0);
}

// 每组收尾的聚合行: 时间是 sum-of-per-file-min; 分配字节与调用次数
// 是求和; peak 取各文件峰值的最大值; med 不聚合, 打印 "-".
static void print_aggregate(const Options* opt, const FileResult* results, int n) {
  uint64_t min_ns[ST_COUNT] = {0};
  BenchAllocStats mem[ST_COUNT];
  memset(mem, 0, sizeof mem);
  unsigned long long bytes = 0;
  long tokens              = 0;
  long decls               = 0;

  for (int i = 0; i < n; i++) {
    const FileResult* r = &results[i];
    for (int s = 0; s < ST_COUNT; s++) {
      min_ns[s] += r->min_ns[s];
      mem[s].bytes += r->mem[s].bytes;
      mem[s].count += r->mem[s].count;
      if (mem[s].peak < r->mem[s].peak)
        mem[s].peak = r->mem[s].peak;
    }
    bytes += r->bytes;
    tokens += r->tokens;
    decls += r->decls;
  }

  for (int s = 0; s < ST_COUNT; s++)
    print_row(opt,
              "(sum)",
              s,
              min_ns[s],
              false,
              0,
              &mem[s],
              s == ST_TOKENIZE,
              bytes,
              tokens,
              decls);
}

int main(int argc, char** argv) {
  Options opt = {.repeat = 5};

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--group")) {
      if (++i == argc) {
        usage();
        return 2;
      }
      opt.group = argv[i];
    } else if (!strcmp(argv[i], "--file")) {
      if (++i == argc) {
        usage();
        return 2;
      }
      push(&opt.files, &opt.files_len, argv[i]);
    } else if (!strcmp(argv[i], "--include")) {
      if (++i == argc) {
        usage();
        return 2;
      }
      push(&opt.includes, &opt.includes_len, argv[i]);
    } else if (!strcmp(argv[i], "--define")) {
      if (++i == argc) {
        usage();
        return 2;
      }
      push(&opt.defines, &opt.defines_len, argv[i]);
    } else if (!strcmp(argv[i], "--repeat")) {
      if (++i == argc) {
        usage();
        return 2;
      }
      opt.repeat = atoi(argv[i]);
      if (opt.repeat < 1) {
        fprintf(stderr, "bench: --repeat must be at least 1\n");
        return 2;
      }
    } else if (!strcmp(argv[i], "--version")) {
      printf("libcp %s\n", libcp_version());
      return 0;
    } else if (!strcmp(argv[i], "--help")) {
      usage();
      return 0;
    } else {
      fprintf(stderr, "bench: unknown argument: %s\n", argv[i]);
      usage();
      return 2;
    }
  }

  if (!opt.group) {
    fprintf(stderr, "bench: --group is required\n");
    usage();
    return 2;
  }

  FileResult* results = calloc((size_t)opt.files_len + 1, sizeof *results);
  if (!results) {
    fprintf(stderr, "bench: out of memory\n");
    return 2;
  }

  int n = 0;
  for (int i = 0; i < opt.files_len; i++) {
    FileResult r;
    if (measure_file(&opt, opt.files[i], &r))
      results[n++] = r;
  }

  if (n > 0) {
    print_header();
    for (int i = 0; i < n; i++)
      print_file(&opt, &results[i]);
    print_aggregate(&opt, results, n);
  }

  free(results);
  free(opt.files);
  free(opt.includes);
  free(opt.defines);
  return 0;
}
