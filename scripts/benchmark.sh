#!/usr/bin/env bash
# 可复现的 Linux HTTP 基准测试驱动：启动当前服务器，以 wrk/wrk2 施压，
# 同步采集吞吐、延迟、错误和进程资源，并保存环境与原始证据供跨版本比较。
# 所有配置均来自环境变量，避免命令行位置参数导致结果难以复现。
set -Eeuo pipefail

# 工具与负载参数。TOOL 表示输出语义，TOOL_BIN 允许 wrk2 仍以 “wrk” 文件名安装。
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TOOL="${TOOL:-wrk}"
TOOL_BIN="${TOOL_BIN:-${TOOL}}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-128}"
DURATION="${DURATION:-30s}"
WARMUP_DURATION="${WARMUP_DURATION:-5s}"
RATE="${RATE:-}"
CURRENT_URL="${CURRENT_URL:-http://127.0.0.1:8080/}"
BASELINE_URL="${BASELINE_URL:-}"
BASELINE_PID="${BASELINE_PID:-}"
SERVER_BIN="${SERVER_BIN:-${ROOT_DIR}/build-release/webserver}"
RESULTS_ROOT="${RESULTS_ROOT:-${ROOT_DIR}/benchmark-results}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RESULT_DIR="${RESULTS_ROOT}/${RUN_ID}"
LUA_REPORT="${LUA_REPORT:-${ROOT_DIR}/scripts/wrk_report.lua}"

server_pid=""

# 输出环境变量接口而不执行任何测试，供开发者和 CI 自助发现配置能力。
usage() {
    cat <<'EOF'
Usage: [VARIABLE=value ...] bash scripts/benchmark.sh

Key variables:
  TOOL=wrk|wrk2            Load generator (default: wrk)
  TOOL_BIN=/path/to/wrk    Executable; wrk2 is often installed as "wrk"
  THREADS=4                Load-generator threads
  CONNECTIONS=128          Open connections
  DURATION=30s             Measured duration
  WARMUP_DURATION=5s       Unrecorded warm-up; set 0 to disable
  RATE=50000               Required for wrk2, requests/second
  CURRENT_URL=http://127.0.0.1:8080/
  SERVER_BIN=build-release/webserver
  BASELINE_URL=http://127.0.0.1:8081/   Optional comparison target
  BASELINE_PID=1234        Optional local baseline PID for RSS/CPU sampling
  RESULTS_ROOT=benchmark-results
  RUN_ID=custom-name

The current server always binds port 8080. Run this script on Linux from a
quiet host. Results are written as raw output, environment.txt, commands.txt,
samples/*.tsv and summary.tsv. No missing metric is estimated.
EOF
}

die() {
    # 统一错误前缀和非零退出，便于自动化日志检索。
    printf 'benchmark: %s\n' "$*" >&2
    exit 1
}

stop_server() {
    # EXIT trap 可多次触发清理，因此空 PID 和已退出进程都应安全地视为成功。
    if [[ -z "${server_pid}" ]]; then
        return
    fi
    if kill -0 "${server_pid}" 2>/dev/null; then
        # 优先终止由 setsid 建立的整个进程组，若平台不支持负 PID 再回退到
        # 主进程。宽限期允许连接和日志正常收尾，避免污染下一次测量。
        kill -TERM -- "-${server_pid}" 2>/dev/null || kill -TERM "${server_pid}" 2>/dev/null || true
        for _ in {1..50}; do
            local process_state
            process_state="$(ps -p "${server_pid}" -o stat= 2>/dev/null | awk 'NF {print $1}')"
            # 僵尸进程已不再服务请求，但 kill -0 仍可能成功；立即 wait 回收，
            # 否则会无意义等待完整宽限期。
            if [[ -z "${process_state}" || "${process_state}" == Z* ]]; then
                wait "${server_pid}" 2>/dev/null || true
                server_pid=""
                return
            fi
            sleep 0.1
        done
        # 仅在优雅终止超时后升级 SIGKILL，给卡死服务器一个有界清理保证。
        kill -KILL -- "-${server_pid}" 2>/dev/null || kill -KILL "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    server_pid=""
}

trap stop_server EXIT
# 将交互中断转换为惯例退出码，同时仍由 EXIT trap 回收服务进程。
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
[[ $# -eq 0 ]] || die "configuration uses environment variables; see --help"
# 在创建结果目录前完成平台、参数和依赖验证，避免失败运行留下似乎有效的报告。
[[ "$(uname -s)" == "Linux" ]] || die "this benchmark runner requires Linux"
[[ "${TOOL}" == "wrk" || "${TOOL}" == "wrk2" ]] || die "TOOL must be wrk or wrk2"
[[ "${THREADS}" =~ ^[1-9][0-9]*$ ]] || die "THREADS must be a positive integer"
[[ "${CONNECTIONS}" =~ ^[1-9][0-9]*$ ]] || die "CONNECTIONS must be a positive integer"
if [[ "${TOOL}" == "wrk2" ]]; then
    # wrk2 是恒定吞吐模型，缺少目标速率时结果无定义；普通 wrk 不接受该参数。
    [[ "${RATE}" =~ ^[1-9][0-9]*$ ]] || die "RATE must be a positive integer for wrk2"
fi

for command_name in "${TOOL_BIN}" curl python3 ps setsid awk; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done
[[ -f "${LUA_REPORT}" ]] || die "missing Lua report script: ${LUA_REPORT}"
[[ -x "${SERVER_BIN}" ]] || die "server is not executable: ${SERVER_BIN}"

printf 'benchmark: fingerprint=listen-reclaim-20260730\n' >&2
printf 'benchmark: SERVER_BIN=%s TOOL=%s THREADS=%s CONNECTIONS=%s DURATION=%s\n' \
    "${SERVER_BIN}" "${TOOL}" "${THREADS}" "${CONNECTIONS}" "${DURATION}" >&2

mkdir -p "${RESULT_DIR}/raw" "${RESULT_DIR}/samples"

# 记录影响性能解释的系统、工具链和二进制身份。报告保留原始文本而非尝试
# 规范化所有平台字段，使回归分析能核对内核参数、资源限制和构建产物。
record_environment() {
    local output="${RESULT_DIR}/environment.txt"
    {
        printf 'run_id=%s\n' "${RUN_ID}"
        printf 'started_utc=%s\n' "$(date -u --iso-8601=seconds)"
        printf 'root_dir=%s\n' "${ROOT_DIR}"
        printf 'tool_mode=%s\n' "${TOOL}"
        printf 'tool_bin=%s\n' "$(command -v "${TOOL_BIN}")"
        printf 'tool_version='
        "${TOOL_BIN}" --version 2>&1 || true
        printf '\nuname:\n'
        uname -a
        # 文件仅在可读时采集，保证精简容器缺失该元数据也不阻断基准。
        if [[ -r /etc/os-release ]]; then
            printf '\nos-release:\n'
            cat /etc/os-release
        fi
        printf '\ncpu:\n'
        if command -v lscpu >/dev/null 2>&1; then lscpu; else printf 'lscpu unavailable\n'; fi
        printf '\nmemory:\n'
        if command -v free >/dev/null 2>&1; then free -h; else printf 'free unavailable\n'; fi
        printf '\nlimits:\n'
        ulimit -a
        printf '\nkernel:\n'
        # TCP 参数会显著影响监听队列和本机压测端口供给；读取失败不伪造
        # 默认值，而是保留缺失并继续执行。
        for key in net.core.somaxconn net.ipv4.ip_local_port_range net.ipv4.tcp_max_syn_backlog; do
            if command -v sysctl >/dev/null 2>&1; then sysctl "${key}" 2>/dev/null || true; fi
        done
        printf '\ncompiler:\n'
        c++ --version 2>&1 || true
        printf '\ncmake:\n'
        cmake --version 2>&1 || true
        printf '\nserver_binary:\n'
        # 哈希把结果绑定到确切二进制，避免仅凭目录名误判比较对象。
        if command -v sha256sum >/dev/null 2>&1; then sha256sum "${SERVER_BIN}"; fi
    } >"${output}"
}

# 只关心是否仍有 LISTEN；TIME_WAIT 不阻挡（服务器使用 SO_REUSEADDR）。
# 若仍有本仓库 webserver 的 LISTEN，先杀掉再测。
reclaim_our_listener() {
    python3 - "${SERVER_BIN}" <<'PY'
import os, signal, subprocess, sys, time

PORT = 8080
hint = os.path.abspath(sys.argv[1])

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

def has_listen():
    if listen_inodes():
        return True
    try:
        out = subprocess.check_output(["ss", "-ltn"], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return False
    return any((":%d" % PORT) in ln and "LISTEN" in ln.upper() for ln in out.splitlines())

def pids_for(inodes):
    found = set()
    for entry in os.listdir("/proc"):
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

if not has_listen():
    print("benchmark: :%d has no LISTEN (ok)" % PORT, flush=True)
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
        if not has_listen():
            print("benchmark: freed LISTEN on :%d" % PORT, flush=True)
            raise SystemExit(0)
        time.sleep(0.1)

print("benchmark: LISTEN still on :%d — ss -ltn:" % PORT, flush=True)
try:
    out = subprocess.check_output(["ss", "-ltn"], text=True, stderr=subprocess.DEVNULL)
    for ln in out.splitlines():
        if (":%d" % PORT) in ln:
            print("  " + ln, flush=True)
except (OSError, subprocess.CalledProcessError):
    pass
raise SystemExit(1)
PY
}

assert_port_8080_free() {
    reclaim_our_listener || die "port 8080 still has LISTEN after reclaim; refuse to start second server"
}

# 在独立会话启动待测服务并轮询真实 URL。就绪探测同时验证监听、HTTP 处理和
# 路由响应，避免把启动阶段的低吞吐混入预热与正式测量。
start_server() {
    printf 'benchmark: starting server %s\n' "${SERVER_BIN}" >&2
    assert_port_8080_free
    local server_dir
    server_dir="$(cd "$(dirname "${SERVER_BIN}")" && pwd)"
    (
        # 在可执行文件目录启动，保证 ./static 等相对路径与 POST_BUILD 产物一致。
        cd "${server_dir}"
        exec setsid "${SERVER_BIN}"
    ) >"${RESULT_DIR}/server.log" 2>&1 &
    server_pid=$!
    printf 'benchmark: server_pid=%s cwd=%s\n' "${server_pid}" "${server_dir}" >&2

    local deadline=$((SECONDS + 15))
    while (( SECONDS < deadline )); do
        if ! kill -0 "${server_pid}" 2>/dev/null; then
            wait "${server_pid}" 2>/dev/null || true
            printf 'benchmark: server log ----\n' >&2
            tail -n 50 "${RESULT_DIR}/server.log" >&2 || true
            die "server exited before becoming ready; inspect ${RESULT_DIR}/server.log"
        fi
        if curl --silent --show-error --fail --max-time 1 "${CURRENT_URL}" >/dev/null 2>&1; then
            printf 'benchmark: server ready at %s\n' "${CURRENT_URL}" >&2
            return
        fi
        sleep 0.1
    done
    printf 'benchmark: server log ----\n' >&2
    tail -n 50 "${RESULT_DIR}/server.log" >&2 || true
    die "server did not become ready at ${CURRENT_URL} within 15 seconds"
}

# 以数组构造命令，确保 URL、路径中的特殊字符不会被二次分词；同一函数同时
# 服务预热和正式阶段，保证除时长外负载参数完全一致。
build_command() {
    local duration="$1"
    local url="$2"
    # 短选项兼容性更好；部分发行版 wrk 对长选项解析更挑剔。
    BENCH_COMMAND=(
        "${TOOL_BIN}"
        -t "${THREADS}"
        -c "${CONNECTIONS}"
        -d "${duration}"
        --latency
        -s "${LUA_REPORT}"
    )
    if [[ "${TOOL}" == "wrk2" ]]; then
        BENCH_COMMAND+=(--rate "${RATE}")
    fi
    BENCH_COMMAND+=("${url}")
}

# 以 shell 可转义格式保存实际命令，报告使用者可直接复现每个阶段，
# 也能确认环境变量最终映射成了哪些负载参数。
write_command() {
    local label="$1"
    shift
    {
        printf '%s\t' "${label}"
        printf '%q ' "$@"
        printf '\n'
    } >>"${RESULT_DIR}/commands.txt"
}

# 从 Lua 输出的 KEY=VALUE 行提取最后一个值。取最后值可容忍工具输出中出现
# 重复摘要，同时不根据人类可读 wrk 文本猜测单位。
metric_from() {
    local file="$1"
    local key="$2"
    awk -F= -v key="${key}" '$1 == key { value=$2 } END { if (value != "") print value }' "${file}"
}

# 汇总每秒资源样本：RSS 给出平均与峰值，CPU 给出平均。无 PID（例如远程
# baseline）时明确输出 NA，不用 0 冒充测量值，避免报告产生错误结论。
summarize_samples() {
    local file="$1"
    if [[ ! -s "${file}" ]]; then
        printf 'NA\tNA\tNA'
        return
    fi
    awk -F'\t' '
        NR > 1 {
            count++
            rss_sum += $2
            cpu_sum += $3
            if ($2 > rss_peak) rss_peak = $2
        }
        END {
            if (count == 0) printf "NA\tNA\tNA"
            else printf "%.0f\t%.0f\t%.3f", rss_sum/count, rss_peak, cpu_sum/count
        }
    ' "${file}"
}

# 执行单个目标的预热、正式压测、资源采样和机器可读汇总。
# label 隔离 current/baseline 证据，sample_pid 为空时仍保留协议性能指标。
run_one() {
    local label="$1"
    local url="$2"
    local sample_pid="$3"
    local raw="${RESULT_DIR}/raw/${label}.txt"
    local samples="${RESULT_DIR}/samples/${label}.tsv"

    if [[ "${WARMUP_DURATION}" != "0" && "${WARMUP_DURATION}" != "0s" ]]; then
        build_command "${WARMUP_DURATION}" "${url}"
        write_command "${label}-warmup" "${BENCH_COMMAND[@]}"
        set +e
        "${BENCH_COMMAND[@]}" >"${RESULT_DIR}/raw/${label}-warmup.txt" 2>&1
        local warm_status=$?
        set -e
        if [[ ${warm_status} -ne 0 ]]; then
            printf 'benchmark: warm-up returned %s (continue); see %s\n' \
                "${warm_status}" "${RESULT_DIR}/raw/${label}-warmup.txt" >&2
            tail -n 30 "${RESULT_DIR}/raw/${label}-warmup.txt" >&2 || true
        fi
    fi

    build_command "${DURATION}" "${url}"
    write_command "${label}" "${BENCH_COMMAND[@]}"
    printf 'epoch_seconds\trss_kib\tcpu_percent\n' >"${samples}"

    # 压测进程后台运行期间每秒采样服务，而不是采样负载生成器；时间戳让
    # 后续工具可与系统监控对齐。
    "${BENCH_COMMAND[@]}" >"${raw}" 2>&1 &
    local load_pid=$!
    while kill -0 "${load_pid}" 2>/dev/null; do
        if [[ -n "${sample_pid}" ]] && kill -0 "${sample_pid}" 2>/dev/null; then
            local observation
            observation="$(ps -p "${sample_pid}" -o rss=,%cpu= 2>/dev/null | awk 'NF == 2 {print $1 "\t" $2}')"
            if [[ -n "${observation}" ]]; then
                printf '%s\t%s\n' "$(date +%s)" "${observation}" >>"${samples}"
            fi
        fi
        sleep 1
    done

    # 临时关闭 errexit 以捕获并报告压测器退出码，随后立即恢复严格模式；
    # 否则 bash 会在 wait 处直接退出，丢失更具体的诊断信息。
    set +e
    wait "${load_pid}"
    local load_status=$?
    set -e
    [[ ${load_status} -eq 0 ]] || die "${label} benchmark failed with exit ${load_status}; inspect ${raw}"

    local qps avg p95 p99 errors errors_connect errors_read errors_write errors_status errors_timeout
    qps="$(metric_from "${raw}" BENCH_QPS)"
    avg="$(metric_from "${raw}" BENCH_LATENCY_AVG_US)"
    p95="$(metric_from "${raw}" BENCH_LATENCY_P95_US)"
    p99="$(metric_from "${raw}" BENCH_LATENCY_P99_US)"
    errors="$(metric_from "${raw}" BENCH_ERRORS_TOTAL)"
    errors_connect="$(metric_from "${raw}" BENCH_ERRORS_CONNECT)"
    errors_read="$(metric_from "${raw}" BENCH_ERRORS_READ)"
    errors_write="$(metric_from "${raw}" BENCH_ERRORS_WRITE)"
    errors_status="$(metric_from "${raw}" BENCH_ERRORS_STATUS)"
    errors_timeout="$(metric_from "${raw}" BENCH_ERRORS_TIMEOUT)"
    # 核心指标缺失意味着 Lua API/工具版本不兼容，不能将不完整结果写成成功报告。
    [[ -n "${qps}" && -n "${avg}" && -n "${p95}" && -n "${p99}" && -n "${errors}" ]] ||
        die "${label} output lacks machine-readable metrics; check wrk/wrk2 Lua compatibility"

    local rss_avg rss_peak cpu_avg
    # 进程替换避免管道子 shell 对变量赋值的影响，并按 TSV 一次读取三个统计值。
    IFS=$'\t' read -r rss_avg rss_peak cpu_avg < <(summarize_samples "${samples}")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${label}" "${url}" "${qps}" "${avg}" "${p95}" "${p99}" \
        "${errors}" "${errors_connect}" "${errors_read}" "${errors_write}" "${errors_status}" "${errors_timeout}" \
        "${rss_avg}" "${rss_peak}" "${cpu_avg}" >>"${RESULT_DIR}/summary.tsv"
}

# 先固化环境和表头，再启动服务，确保即使后续失败也留下可诊断的运行上下文。
record_environment
printf 'label\turl\tqps\tlatency_avg_us\tlatency_p95_us\tlatency_p99_us\terrors_total\terrors_connect\terrors_read\terrors_write\terrors_status\terrors_timeout\trss_avg_kib\trss_peak_kib\tcpu_avg_percent\n' \
    >"${RESULT_DIR}/summary.tsv"
: >"${RESULT_DIR}/commands.txt"

start_server
run_one current "${CURRENT_URL}" "${server_pid}"
stop_server

if [[ -n "${BASELINE_URL}" ]]; then
    # baseline 在 current 完全停止后运行，避免两个本地服务争用 CPU；若调用方
    # 提供其 PID 则同样采样资源，否则资源指标保持 NA。
    run_one baseline "${BASELINE_URL}" "${BASELINE_PID}"
fi

printf 'Benchmark completed. Results: %s\n' "${RESULT_DIR}"
