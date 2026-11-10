# RESULT.md - libcp 前端性能基线执行记录

本文件记录 `PLAN.md`(libcp 前端性能基线线)各步骤的实际执行结果, 供审核与后续会话接续
参考. 前一条主线(库边界固化与产品化验证)的记录见归档 `RESULT-library.md`; 本线开工 HEAD
为 `56a64d6`.

## 状态

- B0.1 已完成; B1-B4 尚未开工.
- 计划与归档文件已于 2026-11-10 建立: `PLAN.md` / `RESULT.md`; 旧主线归档为
  `PLAN-library.md` / `RESULT-library.md` (提交 `70b8ebf`).
- 计划: `PLAN.md`.

## 闸门口径(本线生效)

- 每步: 构建零新告警.
- B1/B2: 单文件, 单组, sqlite 三种路径都能跑; 失败文件不产生行且退出码 0.
- B3: `make test` 与 `make check-context` 全绿; `make clean` 后无 bench 产物.
- B4: `make bench` 产出 `tests` / `lib` / `driver` / `sqlite` 四组; 记录环境块, 表格
  与 shim 开销对照.
- 全程: `libcp/`, `main.c`, `codegen.c` 零 diff.

## 开工基线(B0 前)

- HEAD: `56a64d6` (Update the docs to match the current code).
- 工作树: 干净.
- 默认构建: `CFLAGS` 无 `-O`; benchmark 将使用独立 `-O2` 构建.
- 语料: `test/*.c` 41 个 + `test/common`; `libcp/src/*.c`; `main.c` + `codegen.c`;
  `thirdparty/sqlite/sqlite3.c` 存在.
- 本机: Ubuntu 24.04 devcontainer, `cc` 13.3.0, nproc 2.

## 执行记录

### B0.1 文档与基线复验

- 改动: `AGENTS.md` 的全局约定新增"计划与记录"条目(现行线 `plans/PLAN.md` /
  `plans/RESULT.md`, 已完成的线归档为 `PLAN-<主题>.md` / `RESULT-<主题>.md`).
  `PLAN.md` / `RESULT.md` 的建立与旧线归档已在上一步 `70b8ebf` 完成.
- 闸门: 本步在 `70b8ebf` 上运行, 工作树只含 `AGENTS.md` 改动.
  - `make -j2`: 退出码 0, `-Wall` 无告警.
  - `make test`: 退出码 0; 41 个 `test/*.c` 用例全部运行, 其后各 `test/*.sh`
    行为锁全绿(`cst-stats` 45 UNPARSED / 48 文件, `library` ok, `allocator` ok,
    `fmt` 48 文件幂等, `c2c` 81 个 golden 样本).
  - `make check-context`: 退出码 0 (codegen 无 `cg_ctx`, `.a` 无访问器符号).
- 偏差: B0.1 的"更新 AGENTS.md 的最小条目"按归档语境解释为新增计划 / 记录约定;
  `bench` 的 `make bench` / `make bench-tool` 条目留到 B3.3, 避免提前写入尚不存在的
  目标.

### B1.1 allocator shim

- 改动: 新增 `bench/alloc_shim.h` / `bench/alloc_shim.c`. 四个 `LIBCP_*` 宏指向
  `bench_alloc_*`; 每次分配带一个由 `long double` 联合体构成 16 字节 header, 记录
  请求大小; payload 与 header 同对齐. 计数为 `bytes` / `count` / `live` / `peak`,
  用 `bench_alloc_reset_counters` / `bench_alloc_reset_peak` 分段,
  `bench_alloc_snapshot` 取值. 分配器本身用 C 库作后端, 不递归.
- 验收: `cc -std=c11 -Wall -Wextra` 编译零告警; 临时自测(不提交)验证
  `malloc` / `calloc` 指针 16 字节对齐, `calloc` 清零, 一次分配释放序列后
  `bytes=110 count=2 live=110 peak=110`, `realloc` 后 `bytes=1000 count=1
  live=1100`, 全部释放后 `live=0`; 断言全过.
- 偏差: 无.

### B1.2 harness 核心

- 改动: 新增 `bench/bench.c`, 只包含公共头. 参数 `--group` / `--file` /
  `--include` / `--define` / `--repeat`(默认 5), 另有 `--version` / `--help`.
  每个文件新建 context 并配置 include / define, 预热 1 轮后按 `--repeat` 计时;
  每轮顺序执行 `libcp_context_reset` / `libcp_tokenize_file` /
  `libcp_preprocess` / `libcp_parse` / `libcp_sema`, 五个阶段分别计时, total
  是同一轮五段之和再取 min / median. 输出为固定列宽表格: 每文件每阶段一行,
  加每文件 total 行与每组 `(sum)` 聚合行. 诊断回调丢弃消息; 失败文件不产生
  行, 进程退出码 0; 参数错误退出码 2.
- 验收: 单文件 `test/arith.c`, lib 组的 `libcp/src/preprocess.c`, driver 组的
  `main.c`, sqlite 组的 `thirdparty/sqlite/sqlite3.c` 均输出完整表格;
  `--repeat 1/2/4` 生效; reset 与 total 行出现; 不存在的文件不产生行且退出码
  0; `--repeat 0` 报错退出码 2; `--version` 输出 `libcp 0.1.0`.
- 偏差: 语料配置需要三处补充(已用于验收, 由 B2.2 固化进 `run.sh`):
  所有组前置仓库的 `include/`(驱动器为 chibicc 自带头做的同一件事, 否则
  gcc 内部 `stdarg.h` 会被选中); `lib` / `driver` 组追加
  `--define LIBCP_DEFAULT_ALLOCATOR`(库源码的 `allocator.h` 在内部构建下要求
  四宏或默认开关); driver 组除 `-I.` 外还需 `-Ilibcp/src -Ilibcp/include`
  (`driver.h` 包含内部 umbrella).

### B1.3 统计与内存列

- 改动: 每阶段每轮记录纳秒耗时, min 与 median(偶数轮取中间两轮均值)按
  微秒截断输出; 分配器数字取第一个计时轮的 shim 采样, 列为
  `alloc_bytes` / `alloc_calls` / `peak_live_kb`(请求字节峰值向上取整到 KiB);
  total 行的分配字节与调用次数是五段之和, peak 取五段最大值. 预热轮统计
  文件字节数, 非 EOF token 数与顶层 AST 声明数, 只填在 tokenize 行.
  聚合行: 时间是 sum-of-per-file-min, `med_us` 不聚合打印 `-`, 分配字节与
  调用次数求和, peak 取各文件最大值.
- 验收: `--repeat 4` 的 `test/arith.c` 两次运行中 reset 为 `15/17` 与
  `16/20`, total 为 `1735/1782` 与 `1781/1954`, median 均落在中间两轮之间;
  `lib` 组(`preprocess.c` 峰值约 44 MB)与 sqlite 组(`sqlite3.c`)的内存列
  非零; tokenize 行给出 bytes / tokens / decls, 其余阶段与(除 tokenize 的)
  聚合行打印 `-`.
- 偏差: 无.

### B2.1 平台守卫与 include 发现

- 改动: 新增 `bench/run.sh`(可执行). `LC_ALL=C` 固定输出环境, 切到仓库根;
  `uname -s` 不是 Linux 时报错退出 1; 用 `cc -E -v -x c /dev/null` 的
  "#include <...> search starts here:" 区块提取系统 include, 去掉行首空格与
  macOS 的 "(framework directory)" 后缀; 列表为空时明确报错.
- 验收: devcontainer 上列出 `/usr/lib/gcc/x86_64-linux-gnu/13/include`,
  `/usr/local/include`, `/usr/include/x86_64-linux-gnu`, `/usr/include` 四条,
  退出码 0; 用导出的 bash 函数伪造 `uname` 返回 Darwin 时脚本拒绝运行,
  打印"只支持 Linux", 退出码 1.
- 偏差: 无.

### B2.2 语料组与 flags

- 改动: `bench/run.sh` 持四组文件数组与 flags, 每组调用 harness 一次:
  - `tests`: `test/*.c` + `test/common`, `-Iinclude -Itest`;
  - `lib`: `libcp/src/*.c`, `-Iinclude -Ilibcp/include -Ilibcp/src`,
    `-DLIBCP_DEFAULT_ALLOCATOR`;
  - `driver`: `main.c codegen.c`, `-Iinclude -I. -Ilibcp/src -Ilibcp/include`,
    `-DLIBCP_DEFAULT_ALLOCATOR`;
  - `sqlite`: `thirdparty/sqlite/sqlite3.c`, `-Iinclude -Ithirdparty/sqlite`,
    `-D_GNU_SOURCE`, 文件不存在时整组跳过.
  每组都在组内 flags 之后追加发现到的系统 include; 端口可经 `BENCH_REPEAT`
  覆盖 `--repeat`(默认 5). 缺少 `bench/libcp-bench` 时提示先跑
  `make bench-tool`.
- 验收: `BENCH_REPEAT=1 bash bench/run.sh` 用时约 3.2 s, 退出码 0, 四组
  `== group:` 标题与 `(sum)` 聚合行齐全; 表格行数 `tests` 258(42 文件 × 6 +
  6 聚合), `lib` 78(12 文件), `driver` 18(2 文件), `sqlite` 12; `tests` 组
  含 `test/common` 行. 临时移走 `thirdparty/sqlite/sqlite3.c` 后脚本只跑前三
  组, 退出码 0, 无 sqlite 行; 文件已原位恢复.
- 偏差: flags 相对 D4 的三处补充见 B1.2 记录的"偏差"段.

### B2.3 环境块与哈希

- 改动: 表格前先打印环境块(git commit 与 dirty 标记, `uname -srm`, CPU model,
  nproc, `cc --version` 首行, libc 版本, `BENCH_CFLAGS`, `libcp_version()`,
  repeat)与语料指纹表(每个输入文件的字节数与 SHA-256). 哈希优先
  `sha256sum`, 缺失时回退 `shasum -a 256`, 两者都没有时写 `unavailable`;
  全程在 `LC_ALL=C` 下输出.
- 验收: 两次 `BENCH_REPEAT=1 bash bench/run.sh` 的 `== corpus` 区块逐字节
  相同, 同一文件的 SHA-256 一致; 环境块在 devcontainer 上给出
  `855fd69-dirty`, `Linux 6.8.0-1064-azure x86_64`,
  `AMD EPYC 7763 64-Core Processor`, nproc 2, `cc 13.3.0`,
  `GLIBC 2.39`, `libcp 0.1.0`.
- 偏差: 无.

### B3.1 Makefile 目标

- 改动: `Makefile` 新增 `BENCH_CFLAGS`(独立 `-O2` 构建, 加
  `-D_POSIX_C_SOURCE=200809L` 与 `-include bench/alloc_shim.h`, 刻意不含
  `ALLOCATOR_CFLAGS`), `bench/obj/*.o` 规则, `bench/libcp_bench.a`,
  `bench/libcp-bench`, 目标 `bench-tool`(只构建)与 `bench`(构建后带
  `BENCH_CFLAGS` 环境变量运行 `bench/run.sh`), 并把两个目标加入 `.PHONY`.
  默认 `all` / `test` / `install` 不受影响.
- 验收: `make bench-tool` 退出码 0, 构建日志 0 处 warning; 删掉产物后重建
  成功; `BENCH_REPEAT=1 make bench` 退出码 0, 输出四组标题, 环境块的
  `bench_cflags` 显示上面那串 flags, `tests` 组包含 `test/common`.
- 偏差: 无.

### B3.2 clean / gitignore / CI

- 改动: `make clean` 删除 `bench/obj`, `bench/libcp_bench.a`,
  `bench/libcp-bench`; `.gitignore` 忽略这三个产物; Linux CI job 在
  `check-library` 之后新增 `Benchmark tool build: make bench-tool`(只编译,
  不计时).
- 验收: `make clean` 后 `bench/` 只剩 `bench.c`, `run.sh`, `alloc_shim.h`,
  `alloc_shim.c` 四个源文件, `git status` 不显示任何 bench 产物; 重新
  `make bench-tool` 退出码 0, 再次 `make clean` 后同样干净, 工作树只有本步
  的三个修改文件.
- 偏差: 无.
