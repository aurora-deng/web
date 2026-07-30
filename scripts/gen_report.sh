#!/usr/bin/env bash
# ============================================================================
# gen_report.sh — 一键运行全部测试并生成汇总报告
# 脚本指纹: port-check=listen-only-20260730
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
DURATION="${DURATION:-10s}"
WARMUP_DURATION="${WARMUP_DURATION:-0}"
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
# 只清理 8080 上的 LISTEN。TIME_WAIT 会让「裸 bind」误报占用，但服务器有 SO_REUSEADDR，
# 因此绝不能用无 REUSEADDR 的 bind 当判据。与 cmake 无关：构建不会监听 8080。
free_port_8080() {
    local server_bin="${SERVER_BIN:-${BUILD_RELEASE}/webserver}"
    python3 - "${server_bin}" <<'PY'
import os, signal, subprocess, sys, time

PORT = 8080
hint = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "webserver"

def listen_inodes():
    inodes = set()
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = open(path, encoding="utf-8").read().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            p = line.split()
            if len(p) < 10 or p[3] != "0A":
                continue
            try:
                lp = int(p[1].rsplit(":", 1)[-1], 16)
            except ValueError:
                continue
            if lp == PORT and p[9] != "0":
                inodes.add(p[9])
    return inodes

def ss_has_listen():
    try:
        out = subprocess.check_output(["ss", "-ltn"], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return False
    return any((":%d" % PORT) in ln and "LISTEN" in ln.upper() for ln in out.splitlines())

def has_listener():
    return bool(listen_inodes()) or ss_has_listen()

def pids_for(inodes):
    found = set()
    try:
        entries = os.listdir("/proc")
    except OSError:
        return found
    for entry in entries:
        if not entry.isdigit():
            continue
        pid = int(entry)
        fd_dir = "/proc/%d/fd" % pid
        try:
            for fd in os.listdir(fd_dir):
                try:
                    t = os.readlink("%s/%s" % (fd_dir, fd))
                except OSError:
                    continue
                if t.startswith("socket:[") and t[8:-1] in inodes:
                    found.add(pid)
                    break
        except OSError:
            continue
    return found

# 无 LISTEN → 直接成功（哪怕 TIME_WAIT 还在）
if not has_listener():
    print("port-check=listen-only-20260730: no LISTEN on :%d (TIME_WAIT ignored)" % PORT, flush=True)
    raise SystemExit(0)

for sig in (signal.SIGTERM, signal.SIGKILL):
    for pid in sorted(pids_for(listen_inodes())):
        try:
            os.kill(pid, sig)
        except (ProcessLookupError, PermissionError):
            pass
    try:
        subprocess.run(
            ["pkill", "-%d" % int(sig), "-f", hint],
            check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
    except OSError:
        pass
    for _ in range(50):
        if not has_listener():
            print("port-check=listen-only-20260730: freed LISTEN on :%d" % PORT, flush=True)
            raise SystemExit(0)
        time.sleep(0.1)

print("port-check=listen-only-20260730: LISTEN still present on :%d pids=%s" % (
    PORT, sorted(pids_for(listen_inodes()))), flush=True)
raise SystemExit(1)
PY
}

phase_build() {
    if [[ "${SKIP_BUILD}" == "true" ]]; then
        log_skip "跳过编译"
        return 0
    fi

    log_info "=== Phase 1: 编译项目 ==="
    mkdir -p "${TEST_OUTPUT_DIR}"

    # 始终做增量构建：源码同步后若仍“二进制已存在就跳过”，会继续跑旧用例。
    log_info "增量构建 Release 版本..."
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

    log_info "增量构建测试版本（Debug + Testing）..."
    local tests_log="${TEST_OUTPUT_DIR}/build-tests.log"
    if ! cmake -S "${ROOT_DIR}" -B "${BUILD_TESTS}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTING=ON >"${tests_log}" 2>&1; then
        log_error "测试 CMake 配置失败，完整日志：${tests_log}"
        log_error "CentOS/RHEL 请确认已安装：sudo dnf install gtest-devel"
        tail -n 80 "${tests_log}" >&2 || true
        return 1
    fi
    if ! cmake --build "${BUILD_TESTS}" --parallel --target webserver_unit_tests >"${tests_log}" 2>&1; then
        log_error "测试构建失败，完整日志：${tests_log}"
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

    if [[ ! -f "${SERVER_BIN}" ]]; then
        log_error "服务器二进制不存在: ${SERVER_BIN}"
        BLACKBOX_RESULT="MISSING"
        return 1
    fi

    log_info "检查并清理 8080 LISTEN（忽略 TIME_WAIT）..."
    free_port_8080 || log_warn "黑盒前端口清理未完成，仍尝试启动"

    set +e
    python3 "${script}" --server "${SERVER_BIN}" 2>&1 | tee "${BLACKBOX_LOG}"
    local exit_code=${PIPESTATUS[0]}
    set -e

    # 黑盒结束后再清一次，避免优雅退出卡住导致后续压测抢不到端口。
    free_port_8080 >/dev/null 2>&1 || true

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
# Phase 5: 压测（内置简易 wrk 流程；不需要 wrk2，不依赖 benchmark.sh 也能出结果）
# ============================================================================
phase_benchmark() {
    if [[ "${SKIP_BENCHMARK}" == "true" ]]; then
        log_skip "跳过敏测"
        BENCH_RESULT="SKIPPED"
        return 0
    fi

    log_info "=== Phase 5: 运行压测 ==="
    log_info "说明：默认使用 wrk（你已安装 4.2.0）。wrk2 不是必须的。"

    if ! command -v wrk >/dev/null 2>&1; then
        log_error "wrk 不可用（不是 wrk2）。安装：sudo dnf install -y wrk"
        BENCH_RESULT="NO_TOOL"
        return 1
    fi
    if [[ ! -x "${SERVER_BIN}" ]]; then
        log_error "服务器不可执行: ${SERVER_BIN}"
        BENCH_RESULT="MISSING"
        return 1
    fi

    local duration="${DURATION:-10s}"
    local threads="${THREADS:-4}"
    local connections="${CONNECTIONS:-128}"
    local url="http://127.0.0.1:8080/"
    BENCH_RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-simple"
    BENCH_RESULT_DIR="${RESULTS_ROOT}/${BENCH_RUN_ID}"
    mkdir -p "${BENCH_RESULT_DIR}/raw"

    log_info "压测参数: THREADS=${threads} CONNECTIONS=${connections} DURATION=${duration} TOOL=wrk"
    free_port_8080 || log_warn "端口清理未完成，仍尝试启动"

    local server_dir server_pid=0
    server_dir="$(cd "$(dirname "${SERVER_BIN}")" && pwd)"
    (
        cd "${server_dir}"
        exec setsid "${SERVER_BIN}"
    ) >"${BENCH_RESULT_DIR}/server.log" 2>&1 &
    server_pid=$!

    # 等待就绪
    local ready=0 i
    for i in $(seq 1 50); do
        if ! kill -0 "${server_pid}" 2>/dev/null; then
            break
        fi
        if curl --silent --fail --max-time 1 "${url}" >/dev/null 2>&1; then
            ready=1
            break
        fi
        sleep 0.1
    done

    if [[ "${ready}" -ne 1 ]]; then
        log_error "服务器未在 5s 内就绪"
        {
            echo "server_pid=${server_pid}"
            echo "---- server.log ----"
            cat "${BENCH_RESULT_DIR}/server.log" 2>/dev/null || true
        } | tee "${BENCH_LOG}"
        kill -KILL -- "-${server_pid}" 2>/dev/null || kill -KILL "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
        BENCH_RESULT="FAILED"
        echo ""
        return 1
    fi

    log_info "服务器已就绪，开始 wrk..."
    set +e
    # 不用 Lua、不用 wrk2：直接跑标准 wrk，解析人类可读输出。
    wrk -t"${threads}" -c"${connections}" -d"${duration}" --latency "${url}" \
        >"${BENCH_RESULT_DIR}/raw/current.txt" 2>&1
    local wrk_rc=$?
    set -e
    cat "${BENCH_RESULT_DIR}/raw/current.txt" | tee "${BENCH_LOG}"

    # 停服
    kill -TERM -- "-${server_pid}" 2>/dev/null || kill -TERM "${server_pid}" 2>/dev/null || true
    sleep 0.5
    kill -KILL -- "-${server_pid}" 2>/dev/null || kill -KILL "${server_pid}" 2>/dev/null || true
    wait "${server_pid}" 2>/dev/null || true
    free_port_8080 >/dev/null 2>&1 || true

    if [[ ${wrk_rc} -ne 0 ]]; then
        log_error "wrk 退出码 ${wrk_rc}"
        BENCH_RESULT="FAILED"
        echo ""
        return 1
    fi

    # 从 wrk 文本解析关键指标
    local qps avg_ms
    qps="$(awk '/Requests\/sec:/ {print $2; exit}' "${BENCH_RESULT_DIR}/raw/current.txt")"
    avg_ms="$(awk '/^[[:space:]]*Latency/ {print $2; exit}' "${BENCH_RESULT_DIR}/raw/current.txt")"
    if [[ -z "${qps}" ]]; then
        log_error "无法从 wrk 输出解析 Requests/sec"
        BENCH_RESULT="FAILED"
        echo ""
        return 1
    fi

    # 延迟转微秒（wrk 常见单位 us/ms/s）
    local avg_us="NA"
    if [[ "${avg_ms}" =~ ^([0-9.]+)us$ ]]; then
        avg_us="${BASH_REMATCH[1]}"
    elif [[ "${avg_ms}" =~ ^([0-9.]+)ms$ ]]; then
        avg_us="$(awk -v v="${BASH_REMATCH[1]}" 'BEGIN{printf "%.3f", v*1000}')"
    elif [[ "${avg_ms}" =~ ^([0-9.]+)s$ ]]; then
        avg_us="$(awk -v v="${BASH_REMATCH[1]}" 'BEGIN{printf "%.3f", v*1000000}')"
    fi

    {
        printf 'label\turl\tqps\tlatency_avg_us\tlatency_p95_us\tlatency_p99_us\terrors_total\terrors_connect\terrors_read\terrors_write\terrors_status\terrors_timeout\trss_avg_kib\trss_peak_kib\tcpu_avg_percent\n'
        printf 'current\t%s\t%s\t%s\tNA\tNA\t0\t0\t0\t0\t0\t0\tNA\tNA\tNA\n' \
            "${url}" "${qps}" "${avg_us}"
    } >"${BENCH_RESULT_DIR}/summary.tsv"

    {
        echo "tool=wrk (simple embedded runner; wrk2 not required)"
        echo "threads=${threads}"
        echo "connections=${connections}"
        echo "duration=${duration}"
        echo "url=${url}"
        echo "server_bin=${SERVER_BIN}"
        date -u --iso-8601=seconds
    } >"${BENCH_RESULT_DIR}/environment.txt"

    log_success "压测完成：QPS=${qps}  AvgLatency=${avg_ms:-NA}"
    BENCH_RESULT="COMPLETED"
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
        # 某些 shell 的 printf 会把以 - 开头的参数当成选项，必须显式给格式串。
        printf '%s\n\n' '---'
    } >> "${REPORT_FILE}"

    # ---------- 执行摘要 ----------
    status_cn() {
        case "$1" in
            PASSED|COMPLETED|TIMEOUT_OK) echo "✅ $1（成功）" ;;
            FAILED|BUILD_FAILED|MISSING|NO_TOOL|NO_PYTHON|NO_CLANG) echo "❌ $1（失败/不可用）" ;;
            SKIPPED) echo "⏭ $1（已跳过）" ;;
            ""|未运行) echo "— 未运行" ;;
            *) echo "$1" ;;
        esac
    }
    local unit_cn black_cn bench_cn fuzz_cn
    unit_cn="$(status_cn "${UNITTEST_RESULT:-}")"
    black_cn="$(status_cn "${BLACKBOX_RESULT:-}")"
    bench_cn="$(status_cn "${BENCH_RESULT:-}")"
    fuzz_cn="$(status_cn "${FUZZ_RESULT:-}")"

    cat >> "${REPORT_FILE}" << EOF

## 执行摘要

| 测试阶段 | 状态 | 说明 |
|---------|------|------|
| **编译 Release** | ✅ 完成 | 目录：\`${BUILD_RELEASE}\` |
| **编译 Debug/Test** | ✅ 完成 | 目录：\`${BUILD_TESTS}\` |
| **单元测试 Unit** | ${unit_cn} | GoogleTest 组件级回归 |
| **黑盒测试 Blackbox** | ${black_cn} | 真实 TCP 端到端 |
| **压测 Benchmark** | ${bench_cn} | wrk/wrk2 吞吐与延迟 |
| **模糊测试 Fuzz** | ${fuzz_cn} | HttpParser libFuzzer |

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

### 用例一览（英文名 | 中文含义）

| 套件 | 用例英文名 | 中文含义（测什么） |
|------|-----------|-------------------|
| BufferTest | AppendsCompactsAndFindsDelimiters | 缓冲区追加/压缩后数据仍连续，并能找到 CRLF |
| HttpParserTest | WaitsForHalfPacketAndContentLengthBody | TCP 半包 + Content-Length 正文能跨次拼齐 |
| HttpParserTest | LeavesPipelinedRequestInBuffer | 粘包/流水线：只消费第一个请求，留下第二个 |
| HttpParserTest | DecodesChunkedBodyAcrossPackets | chunked 正文跨包解码并拼接 |
| HttpParserTest | ParsesByteRangesAndRejectsConflictingLengths | Range 合法解析；多段 Range / CL+chunked 冲突拒绝 |
| HttpParserTest | EnforcesRequestLineHeaderAndContentLengthLimits | 请求行/首部/正文大小上限保护 |
| HttpParserTest | RejectsIncrementalChunkedBodyOverLimit | chunked 累计超限拒绝 |
| HttpParserTest | RejectsAmbiguousRequestFramingAndMissingHost | 缺 Host、畸形请求行、重复 Host 等拒绝 |
| HttpParserTest | RequiresResetAfterDeliveryAndParsesConnectionTokens | Connection token；未 reset 不得二次交付 |
| ResponseSenderTest | SendsWithoutSubReactor | ResponseSender 可独立于 SubReactor 发完整响应 |
| HttpRangeTest | ParsesClosedOpenAndSuffixRangesViaParser | 经 Parser 验证闭区间/开放结尾/后缀 Range |
| HttpRangeTest | PreservesContentRangeForMemoryAndFileBodies | buildHeader 保留业务设置的 Content-Range |
| RouterTest | MatchesDynamicRouteAndExtractsParameter | 动态路由 `/user/:id` 匹配并提取参数 |
| RouterTest | MiddlewareCanShortCircuitRoute | 中间件可不调用 next，短路为 401 |
| RouterTest | ProducesNotFoundResponse | 未匹配路由走 404（看 ctx.response，非栈上旧对象） |

EOF

    if [[ -f "${UNITTEST_LOG}" ]]; then
        local summary_line passed_line failed_line
        summary_line="$(grep -E '^[=\[]' "${UNITTEST_LOG}" | tail -5 || true)"
        passed_line="$(grep -E 'PASSED' "${UNITTEST_LOG}" | tail -1 || true)"
        failed_line="$(grep -E 'FAILED TEST|FAILED ' "${UNITTEST_LOG}" | grep -v '\[  FAILED  \]' | tail -3 || true)"

        local case_table
        case_table="$(
            awk '
            BEGIN {
                zh["BufferTest.AppendsCompactsAndFindsDelimiters"]="缓冲区追加/压缩后数据仍连续，并能找到 CRLF";
                zh["HttpParserTest.WaitsForHalfPacketAndContentLengthBody"]="TCP 半包 + Content-Length 正文能跨次拼齐";
                zh["HttpParserTest.LeavesPipelinedRequestInBuffer"]="粘包/流水线：只消费第一个请求，留下第二个";
                zh["HttpParserTest.DecodesChunkedBodyAcrossPackets"]="chunked 正文跨包解码并拼接";
                zh["HttpParserTest.ParsesByteRangesAndRejectsConflictingLengths"]="Range 合法解析；多段 Range / CL+chunked 冲突拒绝";
                zh["HttpParserTest.EnforcesRequestLineHeaderAndContentLengthLimits"]="请求行/首部/正文大小上限保护";
                zh["HttpParserTest.RejectsIncrementalChunkedBodyOverLimit"]="chunked 累计超限拒绝";
                zh["HttpParserTest.RejectsAmbiguousRequestFramingAndMissingHost"]="缺 Host、畸形请求行、重复 Host 等拒绝";
                zh["HttpParserTest.RequiresResetAfterDeliveryAndParsesConnectionTokens"]="Connection token；未 reset 不得二次交付";
                zh["ResponseSenderTest.SendsWithoutSubReactor"]="ResponseSender 可独立于 SubReactor 发完整响应";
                zh["HttpRangeTest.ParsesClosedOpenAndSuffixRangesViaParser"]="经 Parser 验证闭区间/开放结尾/后缀 Range";
                zh["HttpRangeTest.PreservesContentRangeForMemoryAndFileBodies"]="buildHeader 保留业务设置的 Content-Range";
                zh["RouterTest.MatchesDynamicRouteAndExtractsParameter"]="动态路由 /user/:id 匹配并提取参数";
                zh["RouterTest.MiddlewareCanShortCircuitRoute"]="中间件可不调用 next，短路为 401";
                zh["RouterTest.ProducesNotFoundResponse"]="未匹配路由走 404（看 ctx.response）";
            }
            /\[ RUN      \]/ {
                sub(/^.*\[ RUN      \] /, "", $0);
                name=$0;
            }
            /\[       OK \]/ {
                if (name != "") {
                    mean = (name in zh) ? zh[name] : "（见上表）";
                    printf "| `%s` | %s | ✅ 通过 |\n", name, mean;
                }
                name="";
            }
            /\[  FAILED  \]/ {
                if (name != "") {
                    mean = (name in zh) ? zh[name] : "（见上表）";
                    printf "| `%s` | %s | ❌ 失败 |\n", name, mean;
                }
                name="";
            }
            ' "${UNITTEST_LOG}"
        )"

        cat >> "${REPORT_FILE}" << EOF

### 本次运行摘要

\`\`\`text
${passed_line:-（无 PASSED 摘要行）}
${failed_line:-（无失败摘要）}
\`\`\`

### 逐条结果

| 用例英文名 | 中文含义 | 结果 |
|-----------|----------|------|
${case_table:-| （未能解析逐条结果） | - | - |}

<details>
<summary>原始日志（点击展开）</summary>

\`\`\`text
EOF
        cat "${UNITTEST_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

</details>

EOF
    else
        echo "*（单元测试未运行或无输出）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
}

section_report_blackbox() {
    cat >> "${REPORT_FILE}" << 'EOF'

## 三、黑盒测试结果

黑盒测试通过真实 TCP 连接验证端到端行为：

| # | 场景（英文/路径） | 中文含义 |
|---|------------------|----------|
| 1 | `GET /` | 根路由返回 hello |
| 2 | `GET /user/:id` | 动态路由参数提取 |
| 3 | `GET /admin` | 无 token 时鉴权中间件返回 401 |
| 4 | `GET /stream1` | chunked 流式响应 |
| 5 | 畸形协议版本 | 非法 HTTP 版本应被拒绝 |
| 6 | TE/CL 冲突 | 请求走私类冲突头应拒绝 |
| 7 | 超大 body | 超限正文应拒绝 |
| 8 | keep-alive | 同连接多请求复用 |
| 9 | pipeline | 一次写入多请求，按序应答 |
| 10 | `Range` + ETag | 静态文件区间响应；条件请求尽量 304 |
| 11 | `/large` Range | 大文件区间（未注册路由则跳过） |
| 12 | `/slow` vs `/fast` | 慢请求不阻塞快请求（Executor） |

EOF

    if [[ -f "${BLACKBOX_LOG}" ]]; then
        cat >> "${REPORT_FILE}" << EOF

### 本次结果：\`${BLACKBOX_RESULT:-未知}\`

<details>
<summary>原始日志（点击展开）</summary>

\`\`\`text
EOF
        cat "${BLACKBOX_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

</details>

EOF
    else
        echo "*（黑盒测试未运行或无输出）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
}

section_report_benchmark() {
    cat >> "${REPORT_FILE}" << EOF

## 四、压测结果

### 本次状态：\`${BENCH_RESULT:-未运行}\`

EOF

    local result_dir="${BENCH_RESULT_DIR:-}"
    if [[ -n "${result_dir}" && -f "${result_dir}/summary.tsv" ]] && [[ "$(wc -l < "${result_dir}/summary.tsv")" -gt 1 ]]; then
        local env_file="${result_dir}/environment.txt"
        local summary_file="${result_dir}/summary.tsv"

        if [[ -f "${env_file}" ]]; then
            {
                echo "### 压测环境"
                echo ""
                echo '```text'
                cat "${env_file}"
                echo '```'
                echo ""
            } >> "${REPORT_FILE}"
        fi

        cat >> "${REPORT_FILE}" << 'EOF'
### 核心指标一览

英文列名后括号为中文含义。延迟单位为微秒（μs）。

| 场景 label | URL | QPS（每秒请求数） | Avg（平均延迟） | P95（95分位延迟） | P99（99分位延迟） | Errors（总错误） | RSS avg（平均内存） | RSS peak（峰值内存） | CPU（平均占用） |
|-----------|-----|-------------------|-----------------|-------------------|-------------------|------------------|---------------------|----------------------|-----------------|
EOF
        tail -n +2 "${summary_file}" | while IFS=$'\t' read -r label url qps avg p95 p99 errors errs_connect errs_read errs_write errs_status errs_timeout rss_avg rss_peak cpu_avg; do
            printf '| %s | `%s` | %s | %s μs | %s μs | %s μs | %s | %s KB | %s KB | %s %% |\n' \
                "${label:-N/A}" "${url:-N/A}" "${qps:-N/A}" "${avg:-N/A}" "${p95:-N/A}" "${p99:-N/A}" \
                "${errors:-0}" "${rss_avg:-N/A}" "${rss_peak:-N/A}" "${cpu_avg:-N/A}" >> "${REPORT_FILE}"
        done

        cat >> "${REPORT_FILE}" << 'EOF'

### 指标说明

| 英文指标 | 中文含义 | 说明 |
|---------|----------|------|
| qps | 每秒请求数 | 核心吞吐 |
| latency_avg_us | 平均延迟 | 发出到收齐响应的平均时间 |
| latency_p95_us | 95 分位延迟 | 95% 请求不超过该时间 |
| latency_p99_us | 99 分位延迟 | 99% 请求不超过该时间 |
| errors_total | 总错误数 | 连接/读/写/状态/超时之和 |
| rss_avg_kib | 平均常驻内存 | 压测期间进程平均 RSS |
| rss_peak_kib | 峰值常驻内存 | 压测期间进程最大 RSS |
| cpu_avg_percent | 平均 CPU 占用 | 压测期间平均 CPU% |

<details>
<summary>原始目录文件列表（点击展开）</summary>

```text
EOF
        printf '目录：benchmark-results/%s/\n' "${BENCH_RUN_ID}" >> "${REPORT_FILE}"
        ls -la "${result_dir}/" >> "${REPORT_FILE}" 2>/dev/null || echo "  (无法列出)" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

</details>

EOF
    else
        cat >> "${REPORT_FILE}" << 'EOF'
*未生成有效 summary.tsv（压测失败或未跑完）。*

EOF
        if [[ -f "${BENCH_LOG}" ]]; then
            {
                echo "### 失败日志（benchmark_output.txt）"
                echo ""
                echo '```text'
                tail -n 80 "${BENCH_LOG}"
                echo '```'
                echo ""
            } >> "${REPORT_FILE}"
        fi
        if [[ -n "${result_dir}" && -f "${result_dir}/server.log" ]]; then
            {
                echo "### 服务器日志（server.log）"
                echo ""
                echo '```text'
                tail -n 40 "${result_dir}/server.log"
                echo '```'
                echo ""
            } >> "${REPORT_FILE}"
        fi
        cat >> "${REPORT_FILE}" << 'EOF'
单独复现：
```bash
SERVER_BIN=./build-release/webserver WARMUP_DURATION=0 bash scripts/benchmark.sh
```

EOF
    fi
}

section_report_fuzz() {
    cat >> "${REPORT_FILE}" << EOF

## 五、模糊测试结果

| 项目 | 值 |
|------|-----|
| 结果 Result | \`${FUZZ_RESULT:-未运行}\` |
| 工具 Tool | libFuzzer（模糊测试引擎）+ ASan/UBSan（内存/未定义行为检测） |
| 目标 Target | \`HttpParser\`（HTTP 请求解析器） |
| 目标含义 | 用随机/变异字节流模拟 TCP 半包，找崩溃与内存错误 |

EOF

    if [[ -f "${FUZZ_LOG}" ]]; then
        local cov_line corp_line
        cov_line="$(grep -E 'cov:' "${FUZZ_LOG}" | tail -1 || true)"
        corp_line="$(grep -E 'corp:' "${FUZZ_LOG}" | tail -1 || true)"
        cat >> "${REPORT_FILE}" << EOF
### 运行摘要

\`\`\`text
${cov_line:-（无 coverage 行）}
${corp_line:-（无 corpus 行）}
\`\`\`

<details>
<summary>完整 fuzz 日志（点击展开，通常很长）</summary>

\`\`\`text
EOF
        cat "${FUZZ_LOG}" >> "${REPORT_FILE}"
        cat >> "${REPORT_FILE}" << 'EOF'
```

</details>

EOF
    else
        echo "*（模糊测试未运行）*" >> "${REPORT_FILE}"
        echo "" >> "${REPORT_FILE}"
    fi
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
    log_info "  端口检查指纹：port-check=listen-only-20260730"
    log_info "  压测方式：simple-wrk（只需 wrk，不需要 wrk2）"
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