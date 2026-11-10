// benchmark 的分配器 shim (PLAN D3): 库对象用 -include 本头文件构建,
// 四个 LIBCP_* 宏改指向下面的统计分配器. harness 直接调用这些函数读取
// 计数; 它们不是 libcp 的 API.

#ifndef BENCH_ALLOC_SHIM_H
#define BENCH_ALLOC_SHIM_H

#include <stddef.h>

#define LIBCP_MALLOC(size) bench_alloc_malloc(size)
#define LIBCP_CALLOC(count, size) bench_alloc_calloc(count, size)
#define LIBCP_REALLOC(ptr, size) bench_alloc_realloc(ptr, size)
#define LIBCP_FREE(ptr) bench_alloc_free(ptr)

void* bench_alloc_malloc(size_t size);
void* bench_alloc_calloc(size_t count, size_t size);
void* bench_alloc_realloc(void* ptr, size_t size);
void bench_alloc_free(void* ptr);

// 一次采样. `bytes` 与 `count` 自 bench_alloc_reset_counters 起累计,
// `live` 是当前存活的请求字节, `peak` 是自 bench_alloc_reset_peak
// 起的 live 峰值.
typedef struct bench_alloc_stats {
  size_t bytes;
  size_t count;
  size_t live;
  size_t peak;
} BenchAllocStats;

void bench_alloc_reset_counters(void);
void bench_alloc_reset_peak(void);
BenchAllocStats bench_alloc_snapshot(void);

#endif
