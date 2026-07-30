#!/usr/bin/env bash
# 统一的 Linux 测试入口：配置独立测试构建目录、编译全部测试目标并交由 CTest
# 执行。脚本保持无项目外状态，便于开发机和 CI 使用完全一致的验证流程。
set -euo pipefail

# 从脚本位置而非调用者当前目录推导仓库根目录，保证任意路径执行都能找到源码。
# BUILD_DIR 可覆盖，便于 CI 隔离不同编译器或配置；默认目录避免污染常规构建产物。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${root_dir}/build-tests}"

# 显式开启 BUILD_TESTING，默认 Debug 保留断言和易读堆栈；"$@" 原样转交额外
# CMake 选项，使 Sanitizer、工具链文件等质量配置无需复制另一套脚本。
cmake -S "${root_dir}" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" \
    -DBUILD_TESTING=ON \
    "$@"
# 先完成所有测试目标构建，再由 CTest 统一执行并在失败时打印测试输出，
# 为后续新增单元、集成或模糊测试 smoke case 提供单一接入点。
cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
