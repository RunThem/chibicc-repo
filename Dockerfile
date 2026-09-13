# chibicc 的 amd64 Linux 测试环境(在 macOS ARM64 上经由 OrbStack/Rosetta 运行).
# chibicc 生成 x86-64 System V / GAS / ELF 汇编, 且 main.c 按 Ubuntu 路径
# (/usr/include/x86_64-linux-gnu 等)查找 crt/libgcc, 因此基础镜像固定为
# linux/amd64 的 ubuntu:22.04.
FROM --platform=linux/amd64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# build-essential: gcc/as/ld 与 crt/libgcc (chibicc 自身编译与链接探测)
# git + ca-certificates: test/thirdparty/*.sh 需要从 GitHub clone
# file: test/driver.sh 用它检查 -static 产物
# zlib1g-dev: git, libpng, cpython
# libssl-dev: git, cpython
# libcurl4-openssl-dev + libexpat1-dev: git 默认构建 http-push/remote-curl
# gettext: git 的 msgfmt
# perl: git 的测试套件(prove)
# tcl: sqlite 的测试套件
# autoconf/automake/pkg-config/libffi-dev: cpython 的 autoreconf 与 configure
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git ca-certificates file \
      zlib1g-dev libssl-dev libcurl4-openssl-dev libexpat1-dev \
      gettext perl tcl \
      autoconf automake pkg-config libffi-dev \
    && rm -rf /var/lib/apt/lists/*

# test/thirdparty/*.sh 里写的是 git@github.com: SSH 地址, 容器内没有 SSH key,
# 全局重写为 HTTPS.
RUN git config --global url."https://github.com/".insteadOf "git@github.com:"

WORKDIR /src
