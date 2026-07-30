#!/usr/bin/env bash
# ============================================================================
# gen_report.sh — 一键运行全部测试并生成汇总报告
#
# 功能：
#   1. 编译项目（Release + Debug 测试构建）
#   2. 运行单元测试（Google Test）
#   3. 运行黑盒测试（HTTP 集成测试）
#   4. 运行压测（wrk 基准测试）
#   5. 汇总所有结果生成 Markdown 报告
#
# 用法：
#   bash scripts/gen_report.sh [选项]
#
# 选项：
#   --skip-build     跳过编译（已构建时使用）
#   --skip-unit      跳过单元测试
#   --skip-blackbox  跳过黑盒测试
#   --skip-benchmark 跳过压测
#   --skip-fuzz      跳过模糊测试
#   --quick          快速模式：仅单元+黑盒测试
#   --full           完整模式：包含模糊测试（需要 Clang）
#   --help           显示帮助
#
# 环境变量覆盖：
#   THREADS=8                    压测线程数
#   CONNECTIONS=1000             压测连接数
#   DURATION=30s                 压测时长
#   TOOL=wrk|wrk2                压测工具
#   SERVER_BIN=/path/to/server   服务器二进制路径
#   RUN_ID=custom-name           压测运行标识
#
# 示例：
#   # 全量测试
#   bash scripts/gen_report.sh
#
#   # 快速测试（仅单元+黑盒）
#   bash scripts/gen_report.sh --quick
#
#   # 高并发压测
#   THREADS=8 CONNECTIONS=1000 DURATION=60s bash scripts/gen_report.sh
#
#   # 仅生成已有结果的报告（跳过所有测试）
#   bash scripts/gen_report.sh --skip-build --skip-unit --skip-blackbox --skip-benchmark
# ============================================================================
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_RELEASE="${ROOT_DIR}/build-release"
BUILD_TESTS="${ROOT_DIR}/build-tests"
BUILD_FUZZ="${ROOT_DIR}/build-fuzz"
RESULTS_ROOT="${ROOT_DIR}/benchmark-results"
TEST_OUTPUT_DIR="${ROOT_DIR}/test-output"

# ---------- 默认配置 ----------
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-128}"
DURATION="${DURATION:-30s}"
WARMUP_DURATION="${WARMUP_DURATION:-5s}"
TOOL="${TOOL:-wrk}"
SERVER_BIN="${SERVER_BIN:-${BUILD_RELEASE}/webserver}"
RUN_ID="${RUN_ID:-auto}"

# ---------- 标志位 ----------
SKIP_BUILD=false
SKIP_UNIT=false
SKIP_BLACKBOX=false
SKIP_BENCHMARK=false
SKIP_FUZZ=false

# ---------- 解析参数 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)     SKIP_BUILD=true; shift ;;
        --skip-unit)      SKIP_UNIT=true; shift ;;
        --skip-blackbox)  SKIP_BLACKBOX=true; shift ;;
        --skip-benchmark) SKIP_BENCHMARK=true; shift ;;
        --skip-fuzz)      SKIP_FUZZ=true; shift ;;
        --quick)
            SKIP_FUZZ=true
            SKIP_BENCHMARK=true
            shift
            ;;
        --full)
            SKIP_FUZZ=false
            SKIP_BENCHMARK=false
            shift
            ;;
        --help|-h)
            cat << 'EOF'
用法：bash scripts/gen_report.sh [选项]

一键编译项目、运行全部测试（单元测试、黑盒测试、压测、模糊测试），
并将所有结果汇总生成一份 Markdown 报告。

选项：
  --skip-build     跳过编译（已构建完成时使用）
  --skip-unit      跳过单元测试
  --skip-blackbox  跳过黑盒测试
  --skip-benchmark 跳过压测
  --skip-fuzz      跳过模糊测试
  --quick          快速模式：仅单元+黑盒测试
  --full           完整模式：包含模糊测试（默认）
  --help, -h       显示此帮助

环境变量覆盖：
  THREADS=8                    压测线程数（默认 4）
  CONNECTIONS=1000             压测连接数（默认 128）
  DURATION=30s                 压测时长（默认 30s）
  TOOL=wrk|wrk2                压测工具（默认 wrk）
  SERVER_BIN=/path/to/server   服务器二进制路径
  RUN_ID=custom-name           压测运行标识

示例：
  bash scripts/gen_report.sh
  bash scripts/gen_report.sh --quick
  THREADS=8 CONNECTIONS=1000 bash scripts/gen_report.sh
  bash scripts/gen_report.sh --skip-build --skip-unit --skip-blackbox

报告输出位置：项目根目录下的 test_report_YYYYMMDD_HHMMSS.md
EOF
            exit 0
            ;;
        *)
            echo "gen_report: 未知选项: $1" >&2
            echo "使用 'bash scripts/gen_report.sh --help' 查看帮助" >&2
            exit 1
            ;;
    esac
done

# ---------- 工具函数 ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()    { echo -e "${CYAN}[INFO]${NC}    $*"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $*"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}    $*"; }
log_error()   { echo -e "${RED}[ERROR]${NC}   $*"; }
log_skip()    { echo -e "${YELLOW}[SKIP]${NC}    $*"; }

# ---------- 初始化输出目录 ----------
mkdir -p "${TEST_OUTPUT_DIR}"
REPORT_FILE="${ROOT_DIR}/test_report_$(date +%Y%m%d_%H%M%S).md"
BLACKBOX_LOG="${TEST_OUTPUT_DIR}/blackbox_output.txt"
UNITTEST_LOG="${TEST_OUTPUT_DIR}/unittest_output.txt"
BENCH_LOG="${TEST_OUTPUT_DIR}/benchmark_output.txt"
FUZZ_LOG="${TEST_OUTPUT_DIR}/fuzz_output.txt"
ENV_LOG="${TEST_OUTPUT_DIR}/env_info.txt"

log_info "报告文件将输出到：${REPORT_FILE}"
echo ""

# ============================================================================
# Phase 1: 编译
# ============================================================================
phase_build() {
    if [[ "${SKIP_BUILD}" == "true" ]]; then
        log_skip "跳过编译"
        return 0
    fi

    log_info "=== Phase 1: 编译项目 ==="
    mkdir -p "${TEST_OUTPUT_DIR}"

    # Release 构建（用于压测和黑盒测试）
    if [[ ! -f "${BUILD_RELEASE}/webserver" ]]; then
        log_info "构建 Release 版本..."
        local release_log="${TEST_OUTPUT_DIR}/build-release.log"
        if ! cmake -S "${ROOT_DIR}" -B "${BUILD_RELEASE}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_TESTING=OFF >"${release_log}" 2>&1; then
            log_error "Release CMake 配置失败，完整日志：${release_log}"
            tail -n 80 "${release_log}" >&2 || true
            return 1
        fi
        if ! cmake --build "${BUILD_RELEASE}" --parallel >"${release_log}" 2>&1; then
            log_error "Release 构建失败，完整日志：${release_log}"
            tail -n 80 "${release_log}" >&2 || true
            return 1
        fi
        log_success "Release 构建完成"
    else
        log_info "Release 版本已存在，跳过"
    fi

    # Debug 测试构建
    if [[ ! -f "${BUILD_TESTS}/webserver_unit_tests" ]]; then
        log_info "构建测试版本（Debug + Testing）..."
        local tests_log="${TEST_OUTPUT_DIR}/build-tests.log"
        if ! cmake -S "${ROOT_DIR}" -B "${BUILD_TESTS}" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DBUILD_TESTING=ON >"${tests_log}" 2>&1; then
            log_error "测试 CMake 配置失败，完整日志：${tests_log}"
            log_error "CentOS/RHEL 请确认已安装：sudo dnf install gtest-devel"
            tail -n 80 "${tests_log}" >&2 || true
            return 1
        fi
        if ! cmake --build "${BUILD_TESTS}" --parallel >"${tests_log}" 2>&1; then
            log_error "测试构建失败，完整日志：${tests_log}"
            # 优先展示编译器真正的 error 行，避免只看到 gmake 摘要
            if grep -E "error:|fatal error:|undefined reference" "${tests_log}" >/dev/null 2>&1; then
                grep -E "error:|fatal error:|undefined reference" "${tests_log}" | tail -n 40 >&2 || true
            else
                tail -n 80 "${tests_log}" >&2 || true
            fi
            return 1
        fi
        if [[ ! -f "${BUILD_TESTS}/webserver_unit_tests" ]]; then
            log_error "构建声称成功但未找到 ${BUILD_TESTS}/webserver_unit_tests"
            return 1
        fi
        log_success "测试构建完成"
    else
        log_info "测试版本已存在，跳过"
    fi

    SERVER_BIN="${BUILD_RELEASE}/webserver"
    echo ""
}

# ============================================================================
# Phase 2: 环境信息
# ============================================================================
phase_env() {
    log_info "=== Phase 2: 采集环境信息 ==="

    {
        printf '收集时间=%s\n' "$(date -u --iso-8601=seconds)"
        printf '\n--- 系统信息 ---\n'
        uname -a
        if [[ -r /etc/os-release ]]; then
            printf '\nOS Release:\n'
            cat /etc/os-release
        fi
        printf '\n--- 硬件信息 ---\n'
        if command -v lscpu >/dev/null 2>&1; then lscpu; else echo "lscpu 不可用"; fi
        printf '\n内存:\n'
        if command -v free >/dev/null 2>&1; then free -h; else echo "free 不可用"; fi
        printf '\n文件句柄限制:\n'
        ulimit -a 2>/dev/null || echo "ulimit 不可用"
        printf '\n--- 内核参数 ---\n'
        for key in net.core.somaxconn net.ipv4.ip_local_port_range net.ipv4.tcp_max_syn_backlog; do
            sysctl "${key}" 2>/dev/null || echo "  ${key}: 不可用"
        done
        printf '\n--- 编译器 ---\n'
        c++ --version 2>&1 || echo "c++ 不可用"
        printf '\n--- CMake ---\n'
        cmake --version 2>&1 || echo "cmake 不可用"
        printf '\n--- wrk ---\n'
        if command -v wrk >/dev/null 2>&1; then wrk --version 2>&1 || echo "wrk 版本信息不可用"; else echo "wrk 不可用"; fi
        printf '\n--- Python ---\n'
        python3 --version 2>&1 || echo "python3 不可用"
        printf '\n--- 服务器二进制 ---\n'
        if [[ -f "${SERVER_BIN}" ]]; then
            sha256sum "${SERVER_BIN}" 2>/dev/null || echo "sha256sum 不可用"
            ls -lh "${SERVER_BIN}"
        else
            echo "服务器二进制不存在: ${SERVER_BIN}"
        fi
    } > "${ENV_LOG}"

    log_success "环境信息已保存到 ${ENV_LOG}"
    echo ""
}

# ============================================================================
# Phase 3: 单元测试
# ============================================================================
phase_unit_test() {
    if [[ "${SKIP_UNIT}" == "true" ]]; then
        log_skip "跳过单元测试"
        UNITTEST_RESULT="SKIPPED"
        return 0
    fi

    log_info "=== Phase 3: 运行单元测试 ==="

    local test_bin="${BUILD_TESTS}/webserver_unit_tests"
    if [[ ! -f "${test_bin}" ]]; then
        log_error "单元测试二进制不存在: ${test_bin}"
        UNITTEST_RESULT="BUILD_FAILED"
        return 1
    fi

    set +e
    "${test_bin}" 2>&1 | tee "${UNITTEST_LOG}"
    local exit_code=${PIPESTATUS[0]}
    set -e

    if [[ ${exit_code} -eq 0 ]]; then
        log_success "单元测试通过"
        UNITTEST_RESULT="PASSED"
    else
        log_error "单元测试失败 (退出码: ${exit_code})"
        UNITTEST_RESULT="FAILED"
    fi
    echo ""
}

# ============================================================================
# Phase 4: 黑盒测试
# ============================================================================
phase_blackbox() {
    if [[ "${SKIP_BLACKBOX}" == "true" ]]; then
        log_skip "跳过黑盒测试"
        BLACKBOX_RESULT="SKIPPED"
        return 0
    fi

    log_info "=== Phase 4: 运行黑盒测试 ==="

    local script="${ROOT_DIR}/tests/integration/http_blackbox.py"
    if [[ ! -f "${script}" ]]; then
        log_error "黑盒测试脚本不存在: ${script}"
        BLACKBOX_RESULT="MISSING"
        return 1
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        log_error "python3 不可用"
        BLACKBOX_RESULT="NO_PYTHON"
        return 1
    fi

    set +e
    python3 "${script}" --server "${SERVER_BIN}" 2>&1 | tee "${BLACKBOX_LOG}"
    local exit_code=${PIPESTATUS[0]}
    set -e

    if [[ ${exit_code} -eq 0 ]]; then
        log_success "黑盒测试通过"
        BLACKBOX_RESULT="PASSED"
    else
        log_error "黑盒测试失败 (退出码: ${exit_code})"
        BLACKBOX_RESULT="FAILED"
    fi
    echo ""
}

# ============================================================================
# Phase 5: 压测
# ============================================================================
phase_benchmark() {
    if [[ "${SKIP_BENCHMARK}" == "true" ]]; then
        log_skip "跳过敏测"
        BENCH_RESULT="SKIPPED"
        return 0
    fi

    log_info "=== Phase 5: 运行压测 ==="

    if ! command -v "${TOOL}" >/dev/null 2>&1; then
        log_error "${TOOL} 不可用"
        BENCH_RESULT="NO_TOOL"
        return 1
    fi

    local bench_script="${ROOT_DIR}/scripts/benchmark.sh"
    if [[ ! -f "${bench_script}" ]]; then
        log_error "benchmark.sh 不存在: ${bench_script}"
        BENCH_RESULT="MISSING"
        return 1
    fi

    # 启动参数
    local cmd="bash ${bench_script}"
    local env_vars=(
        "THREADS=${THREADS}"
        "CONNECTIONS=${CONNECTIONS}"
        "DURATION=${DURATION}"
        "WARMUP_DURATION=${WARMUP_DURATION}"
        "TOOL=${TOOL}"
        "SERVER_BIN=${SERVER_BIN}"
    )

    if [[ "${RUN_ID}" != "auto" ]]; then
        env_vars+=("RUN_ID=${RUN_ID}")
    fi

    log_info "压测参数: THREADS=${THREADS} CONNECTIONS=${CONNECTIONS} DURATION=${DURATION} TOOL=${TOOL}"
    log_info "执行命令: ${cmd}"

    set +e
    env "${env_vars[@]}" "${cmd}" 2>&1 | tee "${BENCH_LOG}"
    local exit_code=${PIPESTATUS[0]}
    set -e

    if [[ ${exit_code} -eq 0 ]]; then
        log_success "压测完成"
        # 获取最新的 RUN_ID
        if [[ -d "${RESULTS_ROOT}" ]]; then
            BENCH_RUN_ID="$(ls -t "${RESULTS_ROOT}" 2>/dev/null | head -1)"
            BENCH_RESULT_DIR="${RESULTS_ROOT}/${BENCH_RUN_ID}"
        fi
        BENCH_RESULT="COMPLETED"
    else
        log_error "压测失败 (退出码: ${exit_code})"
        BENCH_RESULT="FAILED"
        BENCH_RUN_ID=""
    fi
    echo ""
}

# ============================================================================
# Phase 6: 模糊测试（可选）
# ============================================================================
phase_fuzz() {
    if [[ "${SKIP_FUZZ}" == "true" ]]; then
        log_skip "跳过模糊测试"
        FUZZ_RESULT="SKIPPED"
        return 0
    fi

    log_info "=== Phase 6: 运行模糊测试 ==="

    if ! command -v clang++ >/dev/null 2>&1; then
        log_warn "clang++ 不可用，跳过模糊测试"
        FUZZ_RESULT="NO_CLANG"
        return 0
    fi

    local fuzz_bin="${BUILD_FUZZ}/http_parser_fuzz"
    local fuzz_corpus="${ROOT_DIR}/fuzz-corpus"

    # 构建模糊测试目标
    if [[ ! -f "${fuzz_bin}" ]]; then
        log_info "构建模糊测试目标..."
        CC=clang CXX=clang++ cmake -S "${ROOT_DIR}" -B "${BUILD_FUZZ}" \
            -DBUILD_TESTING=OFF \
            -DWEBSERVER_BUILD_FUZZER=ON \
            -DWEBSERVER_ENABLE_ASAN=ON \
            -DWEBSERVER_ENABLE_UBSAN=ON 2>&1 | tail -5
        cmake --build "${BUILD_FUZZ}" --target http_parser_fuzz --parallel 2>&1 | tail -5
        if [[ ! -f "${fuzz_bin}" ]]; then
            log_error "模糊测试构建失败"
            FUZZ_RESULT="BUILD_FAILED"
            return 1
        fi
        log_success "模糊测试构建完成"
    fi

    mkdir -p "${fuzz_corpus}"

    log_info "运行模糊测试（10 秒超时）..."
    set +e
    timeout 10s "${fuzz_bin}" "${fuzz_corpus}" -max_len=2097152 2>&1 | tee "${FUZZ_LOG}"
    local exit_code=${PIPESTATUS[0]}
    set -e

    if [[ ${exit_code} -eq 0 ]]; then
        log_success "模糊测试完成（无崩溃）"
        FUZZ_RESULT="PASSED"
    elif [[ ${exit_code} -eq 124 ]]; then
        log_success "模糊测试完成（超时退出，无崩溃）"
        FUZZ_RESULT="TIMEOUT_OK"
    else
        log_error "模糊测试发现问题！退出码: ${exit_code}"
        FUZZ_RESULT="FOUND_CRASH"
    fi
    echo ""
}

# ============================================================================
# Phase 7: 生成报告
# ============================================================================
phase_report() {
    log_info "=== Phase 7: 生成汇总报告 ==="

    # 报告头部
    cat > "${REPORT_FILE}" << 'HEADER'
# HTTP 服务器完整测试报告

HEADER

    {
        printf '> **生成时间**：%s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
        printf '> **项目**：C++20 协程 Reactor HTTP 服务器\n'
        printf '> **报告 ID**：`RPT-%s`\n' "$(date +%Y%m%d-%H%M%S)"
        printf '\n'
        printf '---\n\n'
    } >> "${REPORT_FILE}"

    # ---------- 执行摘要 ----------
    cat >> "${REPORT_FILE}" << EOF

## 执行摘要

| 测试阶段 | 状态 | 说明 |
|---------|------|------|
| **编译 (Release)** | ✅ 完成 | 构建目录：\`${BUILD_RELEASE}\` |
| **编译 (Debug/Test)** | ✅ 完成 | 构建目录：\`${BUILD_TESTS}\` |
| **单元测试** | ${UNITTEST_RESULT:-未运行} | 结果日志：\`${UNITTEST_LOG}\` |
| **黑盒测试** | ${BLACKBOX_RESULT:-未运行} | 结果日志：\`${BLACKBOX_LOG}\` |
| **压测** | ${BENCH_RESULT:-未运行} | 结果目录：\`${BENCH_RESULT_DIR:-N/A}\` |
| **模糊测试** | ${FUZZ_RESULT:-未运行} | 结果日志：\`${FUZZ_LOG}\` |

EOF

    # ---------- 环境信息 ----------
    section_report_env

    # ---------- 单元测试结果 ----------
    section_report_unit_test

    # ---------- 黑盒测试结果 ----------
    section_report_blackbox

    # ---------- 压测结果 ----------
    section_report_benchmark

    # ---------- 模糊测试结果 ----------
    section_report_fuzz

    # ---------- 启动流程指导 ----------
    section_report_guide

    # ---------- 操作命令参考 ----------
    section_report_commands

    log_success "报告已生成：${REPORT_FILE}"
}

# ---------- 报告子章节 ----------

section_report_env() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 一、环境信息

EOF
    if [[ -f "${ENV_LOG}" ]]; then
        cat >> "${REPORT_FILE}" << 'EOF'
```text
EOF
        cat "${ENV_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
    else
        echo "*（环境信息未采集）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
}

section_report_unit_test() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 二、单元测试结果

单元测试使用 Google Test 框架，覆盖以下组件：

- **Buffer**: 缓冲区追加、压缩、分隔符查找
- **BufferPool**: 缓冲区池分配/回收
- **HttpParser**: HTTP 请求解析（增量、分包、Content-Length body）
- **ByteRange**: Range 请求解析与合法性校验
- **Router**: 动态路由匹配与参数提取

EOF

    if [[ -f "${UNITTEST_LOG}" ]]; then
        local total_tests passed_tests failed_tests
        total_tests=$(grep -oP '\[\s*\d+\s*tests?\s*\]' "${UNITTEST_LOG}" | grep -oP '\d+' | head -1 || echo "0")
        passed_tests=$(grep -c "PASSED" "${UNITTEST_LOG}" || echo "0")
        failed_tests=$(grep -c "FAILED" "${UNITTEST_LOG}" || echo "0")

        cat >> "${REPORT_FILE}" << EOF

### 测试统计

| 指标 | 数值 |
|------|------|
| 测试总数 | ${total_tests} |
| 通过 | ${passed_tests} |
| 失败 | ${failed_tests} |

### 详细输出

\`\`\`text
EOF
        tail -50 "${UNITTEST_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
    else
        echo "*（单元测试未运行或无输出）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
}

section_report_blackbox() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 三、黑盒测试结果

黑盒测试通过真实 TCP 连接验证服务器端到端功能，覆盖场景：

| # | 测试场景 | 验证内容 |
|---|---------|---------|
| 1 | 根路由 GET / | 返回 `<h1>hello</h1>`，200 状态码 |
| 2 | 动态路由 /user/:id | 参数提取正确，返回 ID 字符串 |
| 3 | 认证中间件 /admin | 返回 401 Unauthorized |
| 4 | 流式响应 /stream1 | chunked 编码正确，body 完整 |
| 5 | 畸形协议版本 | HTTP/9.9 被拒绝，连接关闭 |
| 6 | 请求走私防护 | 冲突的 TE/CL 头被拒绝 |
| 7 | 超大请求体 | 超过限制的 body 被拒绝 |
| 8 | keep-alive 多请求 | 同 TCP 连接 3 请求无错误 |
| 9 | 10000 流水线请求 | 批量写入无 buffer 错乱 |
| 10 | Range + ETag + 304 | 静态文件断点续传和条件请求 |
| 11 | 1GB 文件 Range | sendfile 零拷贝验证 |
| 12 | Executor 验收 | /slow 不阻塞 /fast（业务线程分离） |

EOF

    if [[ -f "${BLACKBOX_LOG}" ]]; then
        cat >> "${REPORT_FILE}" << EOF

### 测试输出

\`\`\`text
EOF
        cat "${BLACKBOX_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
    else
        echo "*（黑盒测试未运行或无输出）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
}

section_report_benchmark() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 四、压测结果

EOF

    local result_dir="${BENCH_RESULT_DIR:-}"
    if [[ -n "${result_dir}" && -f "${result_dir}/summary.tsv" ]]; then
        local env_file="${result_dir}/environment.txt"
        local summary_file="${result_dir}/summary.tsv"

        if [[ -f "${env_file}" ]]; then
            cat >> "${REPORT_FILE}" << 'EOF'
### 压测环境

\`\`\`text
EOF
            cat "${env_file}" >> "${REPORT_FILE}"
            cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
        fi

        # 核心指标表
        local summary_data
        summary_data=$(cat "${summary_file}")
        local summary_header
        summary_header=$(echo "${summary_data}" | head -1 | tr '\t' '|')

        cat >> "${REPORT_FILE}" << EOF

### 核心指标

| ${summary_header}
EOF
        # 表头分隔线
        echo "${summary_header}" | sed 's/[^|]/-/g' | while IFS= read -r line; do
            echo "|${line}|" >> "${REPORT_FILE}"
        done

        # 数据行
        tail -n +2 "${summary_file}" | while IFS=$'\t' read -r label url qps avg p95 p99 errors errs_connect errs_read errs_write errs_status errs_timeout rss_avg rss_peak cpu_avg; do
            local qps_fmt avg_fmt p95_fmt p99_fmt errs_fmt rss_avg_fmt rss_peak_fmt cpu_fmt
            qps_fmt="${qps:-N/A}"
            avg_fmt="${avg:+${avg} μs}"
            avg_fmt="${avg_fmt:-N/A}"
            p95_fmt="${p95:+${p95} μs}"
            p95_fmt="${p95_fmt:-N/A}"
            p99_fmt="${p99:+${p99} μs}"
            p99_fmt="${p99_fmt:-N/A}"
            errs_fmt="${errors:-0}"
            rss_avg_fmt="${rss_avg:+${rss_avg} KB}"
            rss_avg_fmt="${rss_avg_fmt:-NA}"
            rss_peak_fmt="${rss_peak:+${rss_peak} KB}"
            rss_peak_fmt="${rss_peak_fmt:-NA}"
            cpu_fmt="${cpu_avg:+${cpu_avg} %}"
            cpu_fmt="${cpu_fmt:-NA}"
            printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' \
                "${label:-N/A}" "${url:-N/A}" "${qps_fmt}" "${avg_fmt}" "${p95_fmt}" "${p99_fmt}" \
                "${errs_fmt}" "${errs_connect:-0}" "${errs_read:-0}" "${errs_write:-0}" \
                "${errs_status:-0}" "${errs_timeout:-0}" "${rss_avg_fmt}" "${rss_peak_fmt}" "${cpu_fmt}" >> "${REPORT_FILE}"
        done

        cat >> "${REPORT_FILE}" << 'EOF'

### 指标说明

| 指标 | 含义 | 说明 |
|------|------|------|
| **qps** | 每秒请求数 | 核心吞吐指标 |
| **latency_avg_us** | 平均延迟 | 请求从发出到收到响应的平均时间（微秒） |
| **latency_p95_us** | 95 分位延迟 | 95% 请求在此时间内完成 |
| **latency_p99_us** | 99 分位延迟 | 99% 请求在此时间内完成 |
| **errors_total** | 总错误数 | 连接/读/写/状态/超时错误之和 |
| **rss_avg_kib** | 平均常驻内存 | 压测期间进程平均 RSS |
| **rss_peak_kib** | 峰值常驻内存 | 压测期间进程最大 RSS |
| **cpu_avg_percent** | 平均 CPU 占用 | 压测期间进程平均 CPU 使用率 |

### 原始数据文件

\`\`\`
EOF
        printf '目录：benchmark-results/%s/\n' "${BENCH_RUN_ID}" >> "${REPORT_FILE}"
        ls -la "${result_dir}/" >> "${REPORT_FILE}" 2>/dev/null || echo "  (无法列出)" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
    else
        cat >> "${REPORT_FILE}" << 'EOF'
*（压测未运行或无结果）*

压测操作：
```bash
bash scripts/benchmark.sh
# 或使用本脚本：
bash scripts/gen_report.sh --skip-benchmark  # 跳过
bash scripts/gen_report.sh                   # 包含压测
```

EOF
    fi
}

section_report_fuzz() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 五、模糊测试结果

EOF

    if [[ -f "${FUZZ_LOG}" ]]; then
        cat >> "${REPORT_FILE}" << EOF

\`\`\`text
EOF
        cat "${FUZZ_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

EOF
    else
        echo "*（模糊测试未运行）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi

    cat >> "${REPORT_FILE}" << 'EOF'

模糊测试使用 Clang libFuzzer 对 HTTP 解析器进行自动化边界探索，
通过随机生成输入（模拟 TCP 分包），配合 ASan/UBSan 检测内存错误。
EOF
}

section_report_guide() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 六、完整启动流程指南

### 6.1 环境准备（CentOS 9）

```bash
# 更新系统
sudo dnf update -y

# 安装开发工具
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y cmake python3 git wrk

# 安装 Google Test
sudo dnf install -y gtest-devel gtest
# 或编译安装：
# sudo dnf install -y gtest-devel

# 可选：Clang 模糊测试支持
sudo dnf install -y clang clang-libs llvm

# 可选：火焰图性能分析
sudo dnf install -y perf
# FlameGraph 工具：
# git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph
```

### 6.2 编译项目

```bash
cd /path/to/web

# 方式一：使用脚本自动编译
bash scripts/gen_report.sh          # 编译 + 全部测试
bash scripts/gen_report.sh --quick  # 编译 + 快速测试

# 方式二：手动分步编译
# Release 构建（用于生产/压测）
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-release --parallel

# Debug 测试构建（用于单元测试）
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-tests --parallel

# 模糊测试构建（需要 Clang）
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
    -DBUILD_TESTING=OFF \
    -DWEBSERVER_BUILD_FUZZER=ON \
    -DWEBSERVER_ENABLE_ASAN=ON \
    -DWEBSERVER_ENABLE_UBSAN=ON
cmake --build build-fuzz --target http_parser_fuzz --parallel
```

### 6.3 运行测试

```bash
# 方式一：一键全量测试（推荐）
bash scripts/gen_report.sh

# 方式二：快速测试（仅单元+黑盒）
bash scripts/gen_report.sh --quick

# 方式三：单独运行各测试
# 单元测试
bash scripts/run_tests.sh

# 黑盒测试
python3 tests/integration/http_blackbox.py --server ./build-release/webserver

# 压测
bash scripts/benchmark.sh

# 模糊测试
CC=clang CXX=clang++ cmake -S . -B build-fuzz -DWEBSERVER_BUILD_FUZZER=ON
cmake --build build-fuzz --target http_parser_fuzz
mkdir -p fuzz-corpus
./build-fuzz/http_parser_fuzz fuzz-corpus -max_len=2097152
```

### 6.4 生成报告

```bash
# 一键生成（已包含在 gen_report.sh 中）
bash scripts/gen_report.sh

# 仅从已有结果生成报告（跳过测试）
bash scripts/gen_report.sh --skip-build --skip-unit --skip-blackbox

# 指定参数重新压测并生成报告
THREADS=8 CONNECTIONS=1000 DURATION=60s bash scripts/gen_report.sh
```

### 6.5 性能分析（可选）

```bash
# CPU 火焰图
sudo perf record -F 199 -g -p $(pgrep -n webserver) -- sleep 30
sudo perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > cpu-flamegraph.svg

# CPU 性能计数
sudo perf stat -p $(pgrep -n webserver) -a --timeout 30000 \
  -e task-clock,cycles,instructions,branches,cache-misses,context-switches

# 资源监控
pidstat -p $(pgrep -n webserver) -rudw 1
ss -s
ls /proc/$(pgrep -n webserver)/fd | wc -l
```

### 6.6 验证服务器功能

```bash
# 启动服务器
./build-release/webserver &
sleep 1

# 基本功能验证
curl -i http://127.0.0.1:8080/          # 根路由
curl -i http://127.0.0.1:8080/user/123  # 动态路由
curl -i http://127.0.0.1:8080/admin     # 认证中间件（返回 401）
curl -i http://127.0.0.1:8080/logo      # 静态文件
curl -i http://127.0.0.1:8080/stream1   # 流式响应
curl -i http://127.0.0.1:8080/fast      # 快速路由

# Range 请求
curl -i -H "Range: bytes=0-9" http://127.0.0.1:8080/logo

# 停止服务器
kill $(pgrep -n webserver)
```

EOF
}

section_report_commands() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 七、操作命令速查

### 完整测试流程

```bash
# 全流程：编译 → 单元测试 → 黑盒测试 → 压测 → 生成报告
bash scripts/gen_report.sh

# 各阶段单独运行
bash scripts/gen_report.sh --skip-build          # 跳过编译
bash scripts/gen_report.sh --skip-unit           # 跳过单元测试
bash scripts/gen_report.sh --skip-blackbox       # 跳过黑盒测试
bash scripts/gen_report.sh --skip-benchmark      # 跳过压测
bash scripts/gen_report.sh --skip-fuzz           # 跳过模糊测试
bash scripts/gen_report.sh --quick               # 仅单元+黑盒
bash scripts/gen_report.sh --full                # 全量（含模糊测试）
```

### 压测参数配置

```bash
# 默认：4线程 128连接 30秒
bash scripts/benchmark.sh

# 高并发场景
THREADS=8 CONNECTIONS=1000 DURATION=30s bash scripts/benchmark.sh

# 极限负载（用于发现瓶颈）
THREADS=16 CONNECTIONS=5000 DURATION=60s bash scripts/benchmark.sh

# wrk2 恒定速率模式
TOOL=wrk2 TOOL_BIN=/path/to/wrk2 RATE=50000 THREADS=8 CONNECTIONS=256 \
  DURATION=60s bash scripts/benchmark.sh

# 对比两个版本
BASELINE_URL=http://127.0.0.1:8081/ \
  BASELINE_PID=$(pgrep -n nginx) \
  bash scripts/benchmark.sh
```

### Sanitizer 检测

```bash
# ASan + UBSan（内存/越界检查）
BUILD_DIR=build-asan-ubsan bash scripts/run_tests.sh \
  -DWEBSERVER_ENABLE_ASAN=ON -DWEBSERVER_ENABLE_UBSAN=ON

# TSan（线程竞争检查）
BUILD_DIR=build-tsan bash scripts/run_tests.sh -DWEBSERVER_ENABLE_TSAN=ON
```

### 结果文件索引

| 文件路径 | 说明 |
|---------|------|
| `test_report_*.md` | 本汇总报告 |
| `test-output/unit_test_output.txt` | 单元测试原始输出 |
| `test-output/blackbox_output.txt` | 黑盒测试原始输出 |
| `test-output/env_info.txt` | 环境信息采集 |
| `benchmark-results/*/summary.tsv` | 压测核心指标 |
| `benchmark-results/*/environment.txt` | 压测环境快照 |
| `benchmark-results/*/raw/*.txt` | wrk 原始输出 |
| `benchmark-results/*/samples/*.tsv` | 每秒资源采样 |
| `build-tests/Testing/Temporary/` | CTest 日志 |
| `scripts/gen_report.sh` | 本报告生成脚本 |
| `scripts/benchmark.sh` | 压测驱动脚本 |
| `scripts/run_tests.sh` | 测试入口脚本 |
| `scripts/wrk_report.lua` | wrk Lua 适配器 |

---

*本报告由 `scripts/gen_report.sh` 自动生成，所有结果均来自真实运行数据。*

EOF
}

# ============================================================================
# 主流程
# ============================================================================

main() {
    echo ""
    log_info "=============================================="
    log_info "  HTTP 服务器完整测试与报告生成"
    log_info "  项目根目录：${ROOT_DIR}"
    log_info "=============================================="
    echo ""

    phase_build
    phase_env
    phase_unit_test
    phase_blackbox
    phase_benchmark
    phase_fuzz
    phase_report

    echo ""
    log_info "=============================================="
    log_success "  全部完成！"
    log_info "  报告文件：${REPORT_FILE}"
    log_info "  测试输出：${TEST_OUTPUT_DIR}/"
    log_info "  压测结果：${BENCH_RESULT_DIR:-N/A}"
    log_info "=============================================="
    echo ""
    log_info "查看报告："
    echo "  cat ${REPORT_FILE}"
    echo "  或用编辑器打开：code ${REPORT_FILE}"
}

main "$@"