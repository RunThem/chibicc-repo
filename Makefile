CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch -Ilibchibicc/include

# Keep VERSION in sync with CHIBICC_VERSION in the public umbrella.
VERSION=0.1.0
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL ?= install

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)
LIB_SRCS=$(filter-out main.c codegen.c,$(SRCS))
LIB_OBJS=$(LIB_SRCS:.c=.o)
PUBLIC_HDRS=$(wildcard libchibicc/include/chibicc/*.h)

TEST_SRCS=$(wildcard test/*.c)
TESTS=$(TEST_SRCS:.c=.exe)

# Linux also builds the shared library (PLAN L3.3b); macOS only promises
# the archive.
ifeq ($(shell uname -s),Linux)
ALL=chibicc libchibicc.so
CHECK_LIBS=libchibicc.a libchibicc.so
else
ALL=chibicc
CHECK_LIBS=libchibicc.a
endif

all: $(ALL)

# Stage 1

chibicc: libchibicc.a main.o codegen.o
	$(CC) $(CFLAGS) -o $@ main.o codegen.o libchibicc.a $(LDFLAGS)

libchibicc.a: $(LIB_OBJS)
	$(AR) rcs $@ $^

# The shared library (PLAN L3.3b): Linux only, PIC objects built with
# hidden visibility, D7 exported through CHIBICC_API. macOS only
# promises the archive.
ifeq ($(shell uname -s),Linux)
libchibicc.so.0: $(LIB_SRCS:.c=.pic.o)
	$(CC) $(CFLAGS) -shared -Wl,-soname,libchibicc.so.0 -o $@ $^

libchibicc.so: libchibicc.so.0
	ln -sf libchibicc.so.0 $@

%.pic.o: %.c chibicc.h $(PUBLIC_HDRS)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $@ $<
endif

$(OBJS): chibicc.h $(PUBLIC_HDRS)

test/%.exe: chibicc test/%.c
	./chibicc -Iinclude -Itest -c -o test/$*.o test/$*.c
	$(CC) -pthread -o $@ test/$*.o -xc test/common

test: $(TESTS)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./chibicc
	test/diagnostic.sh ./chibicc
	test/shape.sh ./chibicc
	test/trivia.sh ./chibicc
	test/cst.sh ./chibicc
	test/expand.sh ./chibicc
	test/library.sh

test-all: test test-stage2

# The public-surface gate (PLAN L3.1d): header self-containment, a
# consumer that sees only libchibicc/include, the symbol checks, and the
# install / pkg-config layout (L3.3c).
check-library: $(CHECK_LIBS) libchibicc.pc
	test/library.sh
	test/public-api.sh
	test/install.sh

# Stage 2

stage2/chibicc: $(OBJS:%=stage2/%)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stage2/libchibicc.a: $(LIB_OBJS:%=stage2/%)
	$(AR) rcs $@ $^

stage2/%.o: chibicc %.c
	mkdir -p stage2/test
	./chibicc -Ilibchibicc/include -c -o $(@D)/$*.o $*.c

stage2/test/%.exe: stage2/chibicc test/%.c
	mkdir -p stage2/test
	./stage2/chibicc -Iinclude -Itest -c -o stage2/test/$*.o test/$*.c
	$(CC) -pthread -o $@ stage2/test/$*.o -xc test/common

test-stage2: libchibicc.a stage2/libchibicc.a $(TESTS:test/%=stage2/test/%)
	for i in $(TESTS:test/%=stage2/test/%); do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./stage2/chibicc
	test/diagnostic.sh ./stage2/chibicc
	test/shape.sh ./stage2/chibicc
	test/trivia.sh ./stage2/chibicc
	test/cst.sh ./stage2/chibicc
	test/expand.sh ./stage2/chibicc
	CHIBICC_LIB="$(CURDIR)/stage2/libchibicc.a" test/library.sh

# Packaging (PLAN L3.3c).

libchibicc.pc: libchibicc.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< > $@

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include/chibicc
	$(INSTALL) -m 644 $(PUBLIC_HDRS) $(DESTDIR)$(PREFIX)/include/chibicc
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 libchibicc.a $(DESTDIR)$(PREFIX)/lib
ifeq ($(shell uname -s),Linux)
	$(INSTALL) -m 755 libchibicc.so.0 $(DESTDIR)$(PREFIX)/lib
	ln -sf libchibicc.so.0 $(DESTDIR)$(PREFIX)/lib/libchibicc.so
endif
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
	  libchibicc.pc.in > libchibicc.pc.tmp
	$(INSTALL) -m 644 libchibicc.pc.tmp \
	  $(DESTDIR)$(PREFIX)/lib/pkgconfig/libchibicc.pc
	rm -f libchibicc.pc.tmp

# Misc.

clean:
	rm -rf chibicc libchibicc.a libchibicc.so libchibicc.so.0 libchibicc.pc tmp* $(TESTS) test/*.s test/*.exe stage2
	find * -type f '(' -name '*~' -o -name '*.o' ')' -exec rm {} ';'

# Docker (amd64 Linux) 测试环境: 本机是 macOS ARM64, chibicc 生成的 x86-64 代码
# 无法在本机汇编/运行, 完整测试集经由 OrbStack 的 Rosetta 在 amd64 容器中运行.
# 实测 tcc 的 -run/bcheck 运行时(SIGSEGV)只认 overlayfs: 在 virtiofs 挂载路径
# 或 docker volume(ext4)上都会崩溃, 失败点还会漂移(tinycc 112_backtrace).
# 因此仓库只读挂载, 源码拷贝到容器可写层(/work)后构建运行; 这同时保证本机的
# darwin 构建产物不受影响.

THIRDPARTY ?= tinycc libpng git sqlite cpython

# 拷贝源码到容器可写层(排除本机构建产物与 thirdparty 缓存)
BOOT = mkdir -p /work && tar -C /src --exclude=./.git --exclude=./.cache --exclude=./thirdparty --exclude=./chibicc --exclude=./libchibicc.a --exclude="./libchibicc.so*" --exclude=./stage2 --exclude=./compile_commands.json --exclude=./test/*.exe --exclude="*.o" --exclude="*.s" -cf - . | tar -C /work -xf -

docker-image:
	docker build --platform linux/amd64 -t chibicc:amd64 .

docker-test: docker-image
	docker run --rm --platform linux/amd64 -v $(CURDIR):/src:ro chibicc:amd64 \
	  bash -c '$(BOOT) && cd /work && make -j$$(nproc) test-all'

# 默认依次跑五个 thirdparty 脚本(由小到大), 可用 THIRDPARTY=git 或
# THIRDPARTY="git sqlite" 选择子集. tinycc 实测约 15 s; git/sqlite/cpython
# 的测试套件需要数小时, 首次运行还需要网络 clone. clone 缓存保存在仓库的
# thirdparty/ 目录(已被 .gitignore 忽略), 容器启动时拷入本地盘, 结束后同步回.
# test/thirdparty/make 会在 PATH 中拦截 tinycc 的 make test, 跳过 Rosetta 下
# 不稳定的 106_pthread/112_backtrace/113_btdll 三个用例(详见该文件注释).

docker-test-thirdparty: docker-image
	docker run --rm --platform linux/amd64 -v $(CURDIR):/src chibicc:amd64 \
	  bash -c '$(BOOT) && mkdir -p /work/thirdparty && cd /work && chmod +x /work/test/thirdparty/make && export PATH=/work/test/thirdparty:$$PATH && make -j$$(nproc) chibicc || exit 1; rc=0; for t in $(THIRDPARTY); do echo "=== thirdparty: $$t ==="; [ -d /src/thirdparty/$$t ] && cp -a /src/thirdparty/$$t /work/thirdparty/; bash test/thirdparty/$$t.sh || { echo "thirdparty/$$t.sh FAILED"; rc=1; }; mkdir -p /src/thirdparty && cp -an /work/thirdparty/$$t /src/thirdparty/ 2>/dev/null; done; exit $$rc'

# 同一个 thirdparty 脚本集的本地版(devcontainer 就是这类环境: x86_64 Linux,
# chibicc 生成的 x86-64 汇编直接由 as/ld 消费, 不需要容器). 与 docker 版的两点
# 差别, 都是故意的:
# 1. 不把 test/thirdparty 放进 PATH - 那里的 make shim 只为 Rosetta 跳过
#    106_pthread/112_backtrace/113_btdll 三个用例, 真 x86-64 上应让它们真跑;
#    tinycc.sh 首行的 git reset --hard 会恢复此前被 shim 删掉的三个源文件.
# 2. 不把源码拷到 /work - overlayfs 只对 tcc 的 -run/bcheck 运行时必要(见上),
#    与原生执行无关.
# 仅在 x86-64 Linux 上有意义, 其它平台报错并指向 docker-test-thirdparty.
# 用法: make test-thirdparty THIRDPARTY=tinycc (或 "git sqlite", 默认全跑).
test-thirdparty: chibicc
	@[ "$$(uname -s)/$$(uname -m)" = "Linux/x86_64" ] || { \
	  echo "test-thirdparty: 需要 x86-64 Linux (本机 $$(uname -s)/$$(uname -m)); 请改用 make docker-test-thirdparty"; exit 1; }
	@bad=`find thirdparty -mindepth 1 ! -user $$(id -un) -print -quit 2>/dev/null`; [ -z "$$bad" ] || { \
	  echo "test-thirdparty: $$bad 属于其它用户 - docker-test-thirdparty 以 root 回写 clone 缓存, 先跑:"; \
	  echo "  sudo chown -R $$(id -un):$$(id -gn) thirdparty"; exit 1; }
	@rc=0; for t in $(THIRDPARTY); do echo "=== thirdparty: $$t ==="; \
	  bash test/thirdparty/$$t.sh || { echo "thirdparty/$$t.sh FAILED"; rc=1; }; done; exit $$rc

# 汇编快照(拆分线的等价性基线): 在 docker 内构建 chibicc 后对全部 test/*.c
# 跑 ./chibicc -S, 把 .s 存档到 .cache/snapshot(已被 .gitignore 忽略).
# 每步重构结束跑 docker-snapshot-diff, 与基线逐字节 diff, 防行为回归;
# 基线需要重置时(如刻意的行为变化提交后)跑 docker-snapshot.
# 快照在容器内生成: 本机没有 x86-64 Linux 的系统头, -S 需要它们做预处理.

# 容器内构建 chibicc 并生成快照, 以 tar 流输出到 stdout, 由宿主机解包.
# 快照用固定值覆盖 __TIMESTAMP__/__DATE__/__TIME__: test/macro.c 会把它们编进
# 字符串字面量, 时钟值会让逐字节 diff 每次都失败. 固定值格式与内置宏一致
# (11/8/24 字符), 不影响 macro.c 的 strlen 断言; 行为测试(docker-test)不受影响.
SNAPSHOT_DATEFLAGS = -D__TIMESTAMP__="\"Mon Jan  1 00:00:00 2024\"" -D__DATE__="\"Jan  1 2024\"" -D__TIME__="\"00:00:00\""

SNAPSHOT_RUN = docker run --rm --platform linux/amd64 -v $(CURDIR):/src:ro chibicc:amd64 \
  bash -c '$(BOOT) && cd /work && make -j$$(nproc) chibicc >&2 && mkdir snapshot \
    && for f in test/*.c; do ./chibicc -Iinclude -Itest $(SNAPSHOT_DATEFLAGS) -S -o snapshot/$$(basename $$f .c).s $$f || exit 1; done \
    && tar -C /work -cf - snapshot'

docker-snapshot: docker-image
	@mkdir -p .cache && rm -rf .cache/snapshot
	@$(SNAPSHOT_RUN) | tar -C .cache -xf -
	@echo "snapshot: `ls .cache/snapshot | wc -l | tr -d ' '` files -> .cache/snapshot"

docker-snapshot-diff: docker-image
	@rm -rf .cache/.snaptmp .cache/snapshot.new && mkdir -p .cache/.snaptmp
	@$(SNAPSHOT_RUN) | tar -C .cache/.snaptmp -xf - \
	  && mv .cache/.snaptmp/snapshot .cache/snapshot.new && rmdir .cache/.snaptmp
	@if [ ! -d .cache/snapshot ]; then rm -rf .cache/snapshot.new; \
	  echo "snapshot diff: no baseline (.cache/snapshot); run 'make docker-snapshot' first"; exit 1; fi
	@rc=0; diff -ru .cache/snapshot .cache/snapshot.new || rc=1; rm -rf .cache/snapshot.new; \
	  [ $$rc -eq 0 ] && echo "snapshot diff: empty" || { echo "snapshot diff: NON-EMPTY (see diff above)"; exit 1; }

# 归一化 diff (PLAN R0.2): 与 docker-snapshot-diff 同源生成新快照, 但 diff 前
# 对基线与新快照施加同一变换(snapshot-normalize.awk), 吃掉三类预期字节变化:
# .loc/.file 行折叠, 末段为纯数字的 .L 标签按首现顺序重编号, 负 rbp 偏移按
# 每函数首现顺序序号化. [重组] 步骤以本目标为形状闸门; raw 闸门
# (docker-snapshot-diff)与本目标并存, 基线仍是 .cache/snapshot 的 raw 快照.
docker-snapshot-ndiff: docker-image
	@rm -rf .cache/.ndtmp && mkdir -p .cache/.ndtmp
	@$(SNAPSHOT_RUN) | tar -C .cache/.ndtmp -xf - \
	  && mv .cache/.ndtmp/snapshot .cache/.ndtmp/snapshot.new
	@if [ ! -d .cache/snapshot ]; then rm -rf .cache/.ndtmp; \
	  echo "snapshot ndiff: no baseline (.cache/snapshot); run 'make docker-snapshot' first"; exit 1; fi
	@mkdir -p .cache/.ndtmp/base .cache/.ndtmp/new
	@rc=0; for b in `ls .cache/snapshot`; do \
	  if [ ! -f .cache/.ndtmp/snapshot.new/$$b ]; then echo "snapshot ndiff: file only in baseline: $$b"; rc=1; continue; fi; \
	  awk -f snapshot-normalize.awk .cache/snapshot/$$b > .cache/.ndtmp/base/$$b || exit 1; \
	  awk -f snapshot-normalize.awk .cache/.ndtmp/snapshot.new/$$b > .cache/.ndtmp/new/$$b || exit 1; \
	done; \
	for b in `ls .cache/.ndtmp/snapshot.new`; do [ -f .cache/snapshot/$$b ] || \
	  { echo "snapshot ndiff: file only in new snapshot: $$b"; rc=1; }; done; \
	if [ $$rc -eq 1 ]; then rm -rf .cache/.ndtmp; echo "snapshot ndiff: FILE-SET MISMATCH"; exit 1; fi
	@rc=0; diff -ru .cache/.ndtmp/base .cache/.ndtmp/new || rc=1; rm -rf .cache/.ndtmp; \
	  [ $$rc -eq 0 ] && echo "snapshot ndiff: empty" || { echo "snapshot ndiff: NON-EMPTY (see diff above)"; exit 1; }

.PHONY: all test clean test-stage2 test-thirdparty check-library install docker-image docker-test docker-test-thirdparty docker-snapshot docker-snapshot-diff docker-snapshot-ndiff
