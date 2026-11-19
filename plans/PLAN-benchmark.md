# PLAN-benchmark.md - libcp 前端性能基线(benchmark)执行计划(已归档)

> 归档说明(2026-11-19): 本文件是已完成的"libcp 前端性能基线(benchmark)"线的执行计划,
> 随该线收官归档; 现行计划见 `PLAN.md`(libcp 分配模型整改线). 文中"RESULT.md"指本线的
> 执行记录, 已归档为 `RESULT-benchmark.md`.

以下为原文.

# PLAN.md - libcp 前端性能基线(benchmark)执行计划

本文件是 libcp 前端 benchmark 线的执行计划. 目标是为 tokenize / preprocess / parse / sema
建立可重复的分阶段时间与内存基线, 为后续优化决策提供事实依据. 本线只交付测量工具与基线
报告, 不修改库逻辑; 优化留给后续独立的线. 前一条主线(库边界固化与产品化验证)已完成,
计划与记录归档为 `PLAN-library.md` / `RESULT-library.md`.

新会话恢复方法: 通读 AGENTS.md -> 查看本文件勾选状态 -> `git log --oneline` 确认最后完成
的步骤 -> 从第一个未勾选项继续; 开工前先跑 B0.1 的基线复验确认全绿.

## 用户拍板(2026-11-10)

- 目的: 内部优化决策基线, 不做 CI 性能闸门, 本线不改库代码.
- 测量: 单进程顺序调用 `reset -> tokenize -> preprocess -> parse -> sema`; 分阶段计时 +
  reset 单列 + 总时间.
- 统计: 每文件预热 1 轮, 计时 5 轮, 报 min 与 median.
- 指标: 时间 + 分配器精确统计(alloc bytes / alloc count / peak live bytes) + 规模计数
  (input bytes / token 数 / 顶层声明数).
- 构建: 独立 `BENCH_CFLAGS = CFLAGS + -O2`; 一份 shim 构建同时出时间与内存; 默认
  `CFLAGS` 与出货构建不变.
- 语料: 仓库自身(`test/*.c` + `test/common`, `libcp/src/*.c`, `main.c` + `codegen.c`)
  + `thirdparty/sqlite/sqlite3.c`; 全部 raw source.
- 失败处理: 失败或缺失的语料不输出表格行, 诊断丢弃, 继续, 进程退出码 0.
- 工具: 新顶层 `bench/` 目录; C harness + 薄 shell 包装; `make bench` 构建并运行,
  `make bench-tool` 只构建.
- 输出: 固定列宽的人类可读表格; `run.sh` 先打印环境块; 不做 CSV/JSON, 不做 baseline diff.
- 平台: 仅 Linux; `run.sh` 用 uname 守卫; include 路径用 `cc -E -v -x c /dev/null` 发现.
- 仓库: 归档旧主线; 新 `PLAN.md` / `RESULT.md`; 只做最小 `AGENTS.md` 更新; `bench/`
  不进 `make format`; CI 只加 `make bench-tool`.

## 现状锚点(开工前实测)

- 开工 HEAD: `56a64d6` (Update the docs to match the current code), 工作树干净.
- 库源码 + 驱动器: `libcp/src/*.c` + `main.c` + `codegen.c` 共 15,588 行.
- 语料规模: `test/*.c` 41 个, 另有 `test/common`(无 `.c` 后缀的合法 TU); 公共头 10 个.
- `thirdparty/sqlite/sqlite3.c` 存在; 版本由 `test/thirdparty/sqlite.sh` 固定在
  `86f477edaa17767b39c7bae5b67cac8580f7a8c1`.
- 本机: Ubuntu 24.04 devcontainer, `cc` 13.3.0, nproc 2.
- 现有工具: `tools/fmt.c` 与 `tools/c2c.c` 只消费公共头; `make format` 不覆盖 `tools/`.
- 默认 `CFLAGS` 无 `-O`; benchmark 用独立 `BENCH_CFLAGS`, 不改默认构建.

## 设计决定

### D1 测量模型

- 一个 context 处理一个文件; 每轮开始 `libcp_context_reset`, 进程结束时
  `libcp_context_free`.
- 每轮顺序调用 `libcp_tokenize_file` / `libcp_preprocess` / `libcp_parse` /
  `libcp_sema`.
- reset 单独计时; total = reset + 四阶段. tokenize 含文件读取, 这是消费者真实用法.
- 每文件先跑 1 轮预热(统计规模计数, 不计时), 再跑 `--repeat` 轮计时.
- 计时用 `clock_gettime(CLOCK_MONOTONIC)`; `BENCH_CFLAGS` 加
  `-D_POSIX_C_SOURCE=200809L`.

### D2 统计与指标

- 每阶段记录每轮耗时; 报 min 与 median; total 按每轮求和后再取 min / median.
- 分配器 shim 记录每阶段 `alloc_bytes`(请求字节总和), `alloc_count`(调用次数),
  `peak_live_kb`(阶段内 live 峰值).
- 规模计数: 文件字节数, 非 EOF token 数, 顶层声明数; 在预热轮统计一次.
- 分配器数字取第一个计时轮次; 如果各轮不一致, 取最大值并在 `RESULT.md` 标注.

### D3 分配器 shim

- `bench/alloc_shim.h` 定义 `LIBCP_MALLOC/CALLOC/REALLOC/FREE` 为 `bench_alloc_*`;
  库对象用 `-include bench/alloc_shim.h` 构建.
- `bench/alloc_shim.c` 用 libc 分配器作后端; 每次分配带 16 字节 header 保存请求大小,
  返回 16 字节对齐的指针.
- 维护 `total_bytes`, `count`, `live`, `peak_live`; 提供
  `bench_alloc_reset_peak` / `bench_alloc_snapshot`.
- shim 只包库的分配点; harness 自己的分配直接用 libc, 不计入.
- 开销对照: 基线阶段用默认分配器的同一 harness 对 sqlite 跑一次, 总时间差异应在噪声内;
  否则回头重议"一份构建"的决定.

### D4 语料与配置

- `tests`: `test/*.c` + `test/common`; flags `-Iinclude -Itest`.
- `lib`: `libcp/src/*.c`; flags `-Ilibcp/include -Ilibcp/src`.
- `driver`: `main.c` + `codegen.c`; flags `-I.`.
- `sqlite`: `thirdparty/sqlite/sqlite3.c`; flags `-Ithirdparty/sqlite -D_GNU_SOURCE`.
- 所有语料走 raw source 与真实 include; 系统头路径由 `run.sh` 从
  `cc -E -v -x c /dev/null` 的搜索列表提取.
- 每个组由 `run.sh` 调用 harness 一次, 传入该组全部 `--file`; harness 在组内逐文件测量
  并输出组聚合行.
- 失败文件: 不输出行, 诊断回调在 harness 内设为丢弃, 继续下一个; 进程退出码 0.

### D5 harness 与输出

- `bench/bench.c` 只用公共头; 参数: `--group NAME`, `--file PATH`(可重复),
  `--include DIR`(可重复), `--define NAME[=VALUE]`(可重复), `--repeat N`.
- 行数据: `group, file, stage, min_us, med_us, alloc_bytes, alloc_count, peak_live_kb,
  bytes, tokens, decls`.
- `stage`: `reset|tokenize|preprocess|parse|sema|total`; 规模列只在 `tokenize` 行填写.
- 每组输出: 每文件每阶段一行 + 每文件 total 行 + 每组每阶段 `sum-of-per-file-min`
  聚合行.
- 表格固定列宽, `LC_ALL=C`; 只写 stdout; 不做 CSV/JSON, 不做 baseline diff.
- `run.sh` 先打印环境块: git commit 与 dirty 标记, `uname -srm`, CPU model, nproc,
  `cc --version` 首行, libc 版本, `BENCH_CFLAGS`, `libcp_version()`, 每个语料的 bytes
  与 SHA-256.

### D6 构建与仓库接入

- 新目录 `bench/`: `bench.c`, `run.sh`, `alloc_shim.h`, `alloc_shim.c`.
- 库对象编到 `bench/obj/`, 用 `BENCH_CFLAGS` 与 `-include bench/alloc_shim.h`; 归档为
  `bench/libcp_bench.a`; harness 链接 `bench/libcp-bench`.
- `make bench-tool` 只构建; `make bench` 构建并运行; 不进 `all` / `test` / `install`.
- `make clean` 删除 `bench/obj`, `bench/libcp_bench.a`, `bench/libcp-bench`;
  `.gitignore` 忽略这些产物.
- `bench/` 不进 `make format`; CI 只在 Linux job 加 `make bench-tool` 编译检查, 不跑计时.
- `AGENTS.md` 只做最小更新: 构建与测试加 `make bench` / `make bench-tool`, 代码地图加
  `bench/`.
- 归档旧 `PLAN.md` / `RESULT.md` 为 `PLAN-library.md` / `RESULT-library.md`; 本文件与
  新的 `RESULT.md` 建立.

## 阶段 B0: 归档与本线建立

- [x] **B0.1 文档与基线复验**:
  旧 `PLAN.md` / `RESULT.md` 已 `git mv` 为 `PLAN-library.md` / `RESULT-library.md` 并加
  归档说明; 本文件与新的 `RESULT.md` 建立. 剩余: 更新 `AGENTS.md` 的最小条目; 跑一次
  `make` / `make test` / `make check-context`, 确认工作树只含本步文档改动; 记录到
  `RESULT.md`. 一步一提交.
  验收: 文档与代码一致; 基线日志留档; `git status` 只显示本步改动.

## 阶段 B1: shim 与 harness

- [x] **B1.1 allocator shim**:
  新增 `bench/alloc_shim.h` / `bench/alloc_shim.c`; 四个 `LIBCP_*` 宏指向
  `bench_alloc_*`; header 保存请求大小并保持 16 字节对齐; 计数器覆盖 bytes / count /
  live / peak.
  验收: 单独编译通过; 一个最小分配/释放循环的 bytes 与 live 计数正确; 指针对齐满足
  `LibcpToken` / `LibcpNode` 的 `long double` 要求.

- [x] **B1.2 harness 核心**:
  新增 `bench/bench.c`; 解析 `--group` / `--file` / `--include` / `--define` /
  `--repeat`; 建立 context, 逐文件执行预热与计时轮; 计时四阶段与 reset; 输出固定列宽
  表格.
  验收: 对单个 `test/*.c` 与 sqlite 都能跑通; `--repeat` 生效; reset 与 total 列出现.

- [x] **B1.3 统计与内存列**:
  记录每阶段的每轮耗时并计算 min / median; 接入 shim 的 `alloc_bytes` /
  `alloc_count` / `peak_live_kb`; 在预热轮统计 bytes / tokens / decls.
  验收: 手算的小样本 min / median 与输出一致; 内存列在 `lib` / `sqlite` 上非零.

## 阶段 B2: 语料与包装

- [x] **B2.1 平台守卫与 include 发现**:
  `bench/run.sh` 用 `uname -s` / `uname -m` 守卫为 Linux; 用
  `cc -E -v -x c /dev/null` 提取系统 include 路径; 失败时打印明确原因.
  验收: 在 devcontainer 上路径列表非空; 在非 Linux 宿主上拒绝运行.

- [x] **B2.2 语料组与 flags**:
  `run.sh` 持有 `tests` / `lib` / `driver` / `sqlite` 四组的文件数组与 flags; sqlite
  文件不存在时跳过该组; 每组调用 harness 一次.
  验收: 四组都能被枚举; `tests` 包含 `test/common`; sqlite 缺失时其余组仍正常.

- [x] **B2.3 环境块与哈希**:
  `run.sh` 在表格前打印环境块; 逐语料记录 bytes 与 SHA-256; `sha256sum` 缺失时回退
  `shasum -a 256`; 全程 `LC_ALL=C`.
  验收: 保存的 stdout 自描述; 同一文件两次运行的哈希一致.

## 阶段 B3: 构建与仓库接入

- [x] **B3.1 Makefile 目标**:
  加 `BENCH_CFLAGS`, `bench/obj` 规则, `bench/libcp_bench.a`, `bench/libcp-bench`;
  `make bench-tool` 只构建; `make bench` 构建并运行 `bench/run.sh`.
  验收: `make bench-tool` 零告警; `make bench` 能跑完至少一个组.

- [x] **B3.2 clean / gitignore / CI**:
  `make clean` 删除 bench 产物; `.gitignore` 忽略 `bench/obj/`,
  `bench/libcp_bench.a`, `bench/libcp-bench`; Linux CI job 加 `make bench-tool`.
  验收: `make clean` 后无残留; CI 构建步骤通过.

- [x] **B3.3 AGENTS 最小更新**:
  `AGENTS.md` 的构建与测试加 `make bench` / `make bench-tool`; 代码地图加
  `bench/bench.c`, `bench/run.sh`, `bench/alloc_shim.{h,c}`.
  验收: 新会话只读 AGENTS.md 就能找到 benchmark 入口; 不新增 `docs/` 文件.

## 阶段 B4: 基线运行与收口

- [x] **B4.1 全量基线**:
  在 devcontainer 跑 `make bench`; 记录 `tests` / `lib` / `driver` / `sqlite` 四组
  结果到 `RESULT.md`; 附环境块与 stdout 存档路径.
  验收: 表格包含四阶段, reset 与 total; sqlite 组出现; 表格行数与预期语料数一致,
  无空组.

- [x] **B4.2 shim 开销对照**:
  用默认分配器的同一 harness 对 sqlite 跑一次; 比较总时间与 shim 构建; 差异超出噪声时
  记录并回到 D3 重议.
  验收: `RESULT.md` 写明对照数字与结论.

- [x] **B4.3 终态闸门**:
  `make bench-tool`, `make bench`, `make test`, `make check-context`, `make clean`;
  确认 `libcp/`, `main.c`, `codegen.c` 零 diff; 提交全部 bench 改动.
  验收: 全部命令退出码 0; 工作树干净; 无行为变化提交.

## 闸门口径(本线生效)

- 每步: 构建零新告警(`-Wall`).
- B1/B2: 单文件, 单组, sqlite 三种路径都能跑; 失败文件不产生行且退出码 0.
- B3: `make test` 与 `make check-context` 全绿; `make clean` 后无 bench 产物;
  `make format` 不受影响.
- B4: `make bench` 在一次运行内产出 `tests` / `lib` / `driver` / `sqlite` 四组;
  `RESULT.md` 记录环境块, 表格与 shim 开销对照.
- 全程: `libcp/`, `main.c`, `codegen.c` 零 diff; 不新增 `exit()` / static 全局.

## 非目标

- 不优化 libcp, 不改公共头, 不新增公共 API.
- 不测 codegen / 驱动器 / CST / provenance / formatter / c2c.
- 不测多 context 并发, 不测 RSS.
- 不做 CSV/JSON, baseline diff, CI 计时闸门.
- 不在 macOS 或 Docker 内计时.
- 不新增 `docs/13-benchmark.md`.

## 已知限制

- 失败或缺失语料完全不出现在输出里, 退出码 0; 空表或短表可能是 include 路径或宏配置
  错误.
- `-O2` 数字与默认 `-O0` 出货构建不同.
- 绝对数字只在同一宿主可比.
- shim 统计的是请求字节, 不是 malloc 实际可用字节.
- 聚合行是 "sum of per-file min", 不是一次整体运行的 min.
