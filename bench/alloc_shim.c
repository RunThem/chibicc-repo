// 分配器 shim 的实现: 以 C 库分配器为后端, 每次分配带一个 16 字节
// header 记录请求大小. benchmark 是单进程单线程工具, 计数放在文件域
// 静态变量里; 这不是库代码.

#include "alloc_shim.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// header 与 payload 都保持 16 字节对齐: LibcpToken 与 LibcpNode 内嵌
// long double. 联合体把 sizeof 与对齐抬到 long double 的要求.
typedef union bench_alloc_header {
  long double align;
  struct {
    size_t size;  // 请求字节数
    size_t pad;   // 补齐到 16 字节
  } meta;
} BenchAllocHeader;

static BenchAllocStats stats;

static void record_alloc(size_t size) {
  stats.bytes += size;
  stats.count++;
  stats.live += size;
  if (stats.peak < stats.live)
    stats.peak = stats.live;
}

void* bench_alloc_malloc(size_t size) {
  if (size > SIZE_MAX - sizeof(BenchAllocHeader))
    return NULL;

  BenchAllocHeader* h = malloc(sizeof(BenchAllocHeader) + size);
  if (!h)
    return NULL;
  h->meta.size = size;
  h->meta.pad  = 0;
  record_alloc(size);
  return h + 1;
}

void* bench_alloc_calloc(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size)
    return NULL;

  size_t n = count * size;
  void* p  = bench_alloc_malloc(n);
  if (p)
    memset(p, 0, n);
  return p;
}

void* bench_alloc_realloc(void* ptr, size_t size) {
  if (!ptr)
    return bench_alloc_malloc(size);

  // realloc(p, 0) 按"释放并返回 NULL"处理; 库的调用点不依赖其他语义.
  if (size == 0) {
    bench_alloc_free(ptr);
    return NULL;
  }
  if (size > SIZE_MAX - sizeof(BenchAllocHeader))
    return NULL;

  BenchAllocHeader* h   = (BenchAllocHeader*)ptr - 1;
  size_t old_size       = h->meta.size;
  BenchAllocHeader* nh  = realloc(h, sizeof(BenchAllocHeader) + size);
  if (!nh)
    return NULL;

  nh->meta.size = size;
  nh->meta.pad  = 0;
  stats.bytes += size;
  stats.count++;
  stats.live = stats.live - old_size + size;
  if (stats.peak < stats.live)
    stats.peak = stats.live;
  return nh + 1;
}

void bench_alloc_free(void* ptr) {
  if (!ptr)
    return;

  BenchAllocHeader* h = (BenchAllocHeader*)ptr - 1;
  stats.live -= h->meta.size;
  free(h);
}

void bench_alloc_reset_counters(void) {
  stats.bytes = 0;
  stats.count = 0;
}

void bench_alloc_reset_peak(void) {
  stats.peak = stats.live;
}

BenchAllocStats bench_alloc_snapshot(void) {
  return stats;
}
