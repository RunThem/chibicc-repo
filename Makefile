CFLAGS=-std=c11 -g -fno-common -Wall -Wno-switch

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

TEST_SRCS=$(wildcard test/*.c)
TESTS=$(TEST_SRCS:.c=.exe)

# Stage 1

chibicc: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJS): chibicc.h

test/%.exe: chibicc test/%.c
	./chibicc -Iinclude -Itest -c -o test/$*.o test/$*.c
	$(CC) -pthread -o $@ test/$*.o -xc test/common

test: $(TESTS)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./chibicc

test-all: test test-stage2

# Stage 2

stage2/chibicc: $(OBJS:%=stage2/%)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

stage2/%.o: chibicc %.c
	mkdir -p stage2/test
	./chibicc -c -o $(@D)/$*.o $*.c

stage2/test/%.exe: stage2/chibicc test/%.c
	mkdir -p stage2/test
	./stage2/chibicc -Iinclude -Itest -c -o stage2/test/$*.o test/$*.c
	$(CC) -pthread -o $@ stage2/test/$*.o -xc test/common

test-stage2: $(TESTS:test/%=stage2/test/%)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done
	test/driver.sh ./stage2/chibicc

# Misc.

clean:
	rm -rf chibicc tmp* $(TESTS) test/*.s test/*.exe stage2
	find * -type f '(' -name '*~' -o -name '*.o' ')' -exec rm {} ';'

# Docker (amd64 Linux) 测试环境: 本机是 macOS ARM64, chibicc 生成的 x86-64 代码
# 无法在本机汇编/运行, 完整测试集经由 OrbStack 的 Rosetta 在 amd64 容器中运行.
# 实测 tcc 的 -run/bcheck 运行时(SIGSEGV)只认 overlayfs: 在 virtiofs 挂载路径
# 或 docker volume(ext4)上都会崩溃, 失败点还会漂移(tinycc 112_backtrace).
# 因此仓库只读挂载, 源码拷贝到容器可写层(/work)后构建运行; 这同时保证本机的
# darwin 构建产物不受影响.

THIRDPARTY ?= tinycc libpng git sqlite cpython

# 拷贝源码到容器可写层(排除本机构建产物与 thirdparty 缓存)
BOOT = mkdir -p /work && tar -C /src --exclude=./.git --exclude=./.cache --exclude=./thirdparty --exclude=./chibicc --exclude=./stage2 --exclude=./compile_commands.json --exclude=./test/*.exe --exclude="*.o" --exclude="*.s" -cf - . | tar -C /work -xf -

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

.PHONY: test clean test-stage2 docker-image docker-test docker-test-thirdparty
