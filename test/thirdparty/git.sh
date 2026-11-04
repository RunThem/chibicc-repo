#!/bin/bash
repo='git@github.com:git/git.git'
. test/thirdparty/common
git reset --hard 54e85e7af1ac9e9a92888060d6811ae767fea1bc

$make clean
# test-lib.sh 的 SANITY 探针用不带 who 的 chmod -w/-r, 受 umask 影响; /workspaces 挂载带默认 ACL
# (新建目录恒带 o+w) 时, 探针在 umask 022 下会以 "cannot prepare SANETESTD" 中止, 并留下 rm 不掉的
# 目录. 显式 a 前缀让探针只看权限位, 与 umask 无关; 与 chibicc 无关, 只改测试框架.
sed -i 's/chmod +w SANETESTD/chmod a+w SANETESTD/; s/chmod -w SANETESTD/chmod a-w SANETESTD/; s/chmod -r SANETESTD/chmod a-r SANETESTD/; s/chmod -rx SANETESTD/chmod a-rx SANETESTD/; s/chmod +rwx SANETESTD/chmod a+rwx SANETESTD/' t/test-lib.sh

# git 2.28 的旧 curl 兼容垫片 (http.h: CURLUSESSL_TRY 映射到 CURLFTPSSL_TRY) 在 curl >= 8.5 上
# 失效: CURLUSESSL_* 从枚举改成宏, 两个宏互相重定义后按 hideset 留下裸标识符; gcc 编译同样的
# 代码也报 undeclared, 与编译器无关. conda 等前缀的 curl-config 会选中这类新头文件, 所以这里
# 优先用系统 curl; 系统没有 curl-config 时保持原行为.
curl_config=/usr/bin/curl-config
curl_config_opt=
[ -x "$curl_config" ] && curl_config_opt="CURL_CONFIG=$curl_config"

# 默认 ACL 还会让 t0001.27 与 t1301-shared-repo 的权限位断言必然失败 (newdir 子目录恒为 0777),
# 且 t0001.30 这类 chmod -w 用例需要 umask 000 才能真去掉写位. 这些验证的是 git 运行时的权限
# 语义, 与 chibicc 无关; 有默认 ACL 时用 umask 000 并跳过上述两条, Docker 镜像 (普通文件系统)
# 照常全跑.
probe=$(mktemp -d)
skip_tests=
test_umask=022
if (umask 022; mkdir "$probe/d" && [ "$(stat -c %a "$probe/d")" != 755 ]); then
  skip_tests="t0001.27 t1301"
  test_umask=000
fi
rm -rf "$probe"

(umask "$test_umask"; GIT_SKIP_TESTS="$skip_tests" $make V=1 CC=$chibicc $curl_config_opt test)
