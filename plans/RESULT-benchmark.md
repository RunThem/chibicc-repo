# RESULT-benchmark.md - libcp 前端性能基线执行记录(已归档)

> 归档说明(2026-11-19): 本文件是已完成的"libcp 前端性能基线(benchmark)"线的执行记录,
> 随该线收官归档; 现行执行记录见 `RESULT.md`(libcp 分配模型整改线). 文中"PLAN.md"指本线
> 的计划, 已归档为 `PLAN-benchmark.md`.

以下为原文.

# RESULT.md - libcp 前端性能基线执行记录

本文件记录 `PLAN.md`(libcp 前端性能基线线)各步骤的实际执行结果, 供审核与后续会话接续
参考. 前一条主线(库边界固化与产品化验证)的记录见归档 `RESULT-library.md`; 本线开工 HEAD
为 `56a64d6`.

## 状态

- B0.1 至 B4.3 全部完成, 本线收官; benchmark 工具与基线已就位.
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

### B3.3 AGENTS 最小更新

- 改动: `AGENTS.md` 的"构建与测试"新增 `make bench` / `make bench-tool`
  条目(仅 Linux, `-O2` + 分配器 shim, 不进入 `all` / `test` / `install`);
  "代码地图"新增 `bench/bench.c`, `bench/run.sh`, `bench/alloc_shim.{h,c}`.
  未新增 `docs/` 文件.
- 验收: 只读 `AGENTS.md` 即可找到 benchmark 的构建与运行入口, 以及
  `bench/` 的四个文件位置.
- 偏差: 无.

### B4.1 全量基线

- 运行: `make bench`(默认 `--repeat 5`), 退出码 0, 用时约 8.6 s; 完整 stdout
  存档在 `.cache/bench/2026-11-10.txt`(449 行, `.cache/` 不进版本库).
- 环境块: `git 7e4600c`(干净), `Linux 6.8.0-1064-azure x86_64`,
  `AMD EPYC 7763 64-Core Processor`, nproc 2, `cc 13.3.0`,
  `GLIBC 2.39`, `libcp 0.1.0`, BENCH_CFLAGS 为
  `-std=c11 -O2 -g -fno-common -Wall -Wno-switch -D_POSIX_C_SOURCE=200809L
  -Ilibcp/include -Ilibcp/src -include bench/alloc_shim.h`.
- 行数核对: `tests` 42 个文件(含 `test/common`)258 行, `lib` 12 个文件 78 行,
  `driver` 2 个文件 18 行, `sqlite` 1 个文件 12 行; 四组都有 `(sum)` 聚合行,
  无空组.
- 组聚合(`(sum)` 行, 时间单位 us, sum-of-per-file-min):

  | group  | reset | tokenize | preprocess | parse | sema  | total  |
  |--------|-------|----------|------------|-------|-------|--------|
  | tests  | 3204  | 9862     | 52275      | 12249 | 4363  | 84000  |
  | lib    | 19724 | 14413    | 344206     | 49673 | 18816 | 448641 |
  | driver | 4376  | 4947     | 60937      | 11652 | 5848  | 88126  |
  | sqlite | 499   | 230104   | 131969     | 96690 | 63663 | 545318 |

- 组 total 的分配器采样(请求字节总和 / live 峰值):
  `tests` 108,108,176 B / 16,799 KiB; `lib` 517,729,440 B / 50,269 KiB;
  `driver` 95,642,129 B / 49,692 KiB; `sqlite` 704,689,969 B / 688,175 KiB.
- 偏差: 无.

### B4.2 shim 开销对照

- 方法: 同一 `bench/bench.c` 与同一组 flags(`-O2`, `_POSIX_C_SOURCE`)编出
  两份库, 唯一差别是分配器: 对照版定义 `LIBCP_DEFAULT_ALLOCATOR`(走 C 库
  `malloc` / `calloc` / `realloc` / `free`), 计时版用
  `-include bench/alloc_shim.h`. 对照版把 `bench/alloc_shim.c` 一并归档只为
  满足 harness 的统计符号, 其计数保持 0. 对 sqlite 以 `--repeat 5` 交替运行
  两组共 6 次, 取每轮 total 的 min.
- 结果(total min, us): 默认分配器 `620300 / 638046 / 614404` 与
  `627911 / 621774 / 621191`; shim `529857 / 527715 / 539540` 与
  `533944 / 531617 / 527127`. 分阶段: tokenize 约 229-234 ms 对 234 ms,
  preprocess 约 134 ms 对 134 ms(基本一致); parse 137-138 ms 对 96-98 ms,
  sema 98-101 ms 对 62-64 ms, reset 9.2 ms 对 0.5 ms. 一次 `--repeat 5`
  运行的 CPU 时间: 默认分配器 user 3.49 s + sys 0.77 s, shim user 3.32 s +
  sys 0.48 s.
- 结论: shim 构建比默认分配器构建快约 13-15%, 方向与"shim 抬高计时"相反;
  差异集中在 parse / sema / reset 与系统时间, 来自两份构建的分配器布局与
  页错误行为不同, 不是计数的开销. 因此 D3 的"一份构建同时出时间与内存"
  保持不变, 后续优化决策可用这组数字, 只需知道它相对默认分配器偏保守.
- 偏差: 无.

### B4.3 终态闸门

- 闸门: `make bench-tool` 退出码 0; `make bench`(默认 repeat 5)退出码 0,
  输出四组; `make test` 退出码 0(41 个 `test/*.c` 用例与全部行为锁, 结尾
  `c2c` 81 个 golden 样本全过); `make check-context` 退出码 0; `make clean`
  退出码 0.
- 终态: `bench/` 只剩 `bench.c`, `run.sh`, `alloc_shim.h`, `alloc_shim.c`;
  `git status` 为空. `git diff --stat 70b8ebf -- libcp main.c codegen.c`
  为空, 本线对库与驱动器零 diff, 未新增 `exit()` 或库内静态全局.
- 提交: 本线按步提交 `ee0f1ca`(B0.1), `c55b55d`(B1.1), `cd00568`(B1.2+B1.3),
  `7811df2`(B2.1), `855fd69`(B2.2), `9897703`(B2.3), `d421571`(B3.1),
  `00bea18`(B3.2), `7e4600c`(B3.3), `a3c1dc8`(B4.1), `914e74d`(B4.2), 以及
  本记录所在的收尾提交.
- 偏差: 无.

### 后续调整: 表格单位与小数位 (2026-11-10)

- 用户要求单位随数值自动选择并打印在单元格里, 暴露两位小数: 时间按
  ns -> us -> ms -> s -> m 自动升档(前三级 1000 进制, s -> m 按 60),
  内存与文件大小按 B -> KB -> MB -> GB 自动升档(1024 进制), 单元格形如
  `141.77 us` / `243.05 ms` / `313.78 MB`; `calls` / `tokens` / `decls`
  仍是整数. 表头相应改为 `min` / `med` / `alloc` / `calls` / `peak` /
  `size`. `(sum)` 行口径不变.
- B4.1 表里的数字是换算前的原始值(微秒与字节), 与新单位的单元格一一对应;
  引用存档 `.cache/bench/2026-11-10.txt` 已按新格式重新生成.
