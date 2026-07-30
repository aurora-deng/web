# Scripts 目录说明

本目录包含 HTTP 服务器的测试、压测和报告生成脚本。

## 脚本列表

| 文件 | 用途 | 说明 |
|------|------|------|
| `gen_report.sh` | **一键测试+报告** | 编译→单元测试→黑盒测试→压测→模糊测试→生成 Markdown 报告 |
| `benchmark.sh` | 自动化压测驱动 | 启动服务器 → wrk 施压 → 采样资源 → 生成原始数据 |
| `run_tests.sh` | 编译+运行单元测试 | CMake 配置构建 → 编译全部测试 → CTest 执行 |
| `wrk_report.lua` | wrk Lua 输出适配器 | 将 wrk 内部数据转为机器可读的 KEY=VALUE 格式 |

## 🚀 快速开始（推荐）

```bash
# 进入项目目录
cd /path/to/web

# 一键完成：编译 → 全部测试 → 生成汇总报告
bash scripts/gen_report.sh

# 报告生成位置：
#   test_report_YYYYMMDD_HHMMSS.md  （项目根目录）
#   test-output/                    （原始测试输出）
```

### 快速模式（仅单元+黑盒测试）

```bash
bash scripts/gen_report.sh --quick
```

### 自定义压测参数

```bash
THREADS=8 CONNECTIONS=1000 DURATION=60s bash scripts/gen_report.sh
```

### 跳过部分测试

```bash
# 已编译过，直接运行测试
bash scripts/gen_report.sh --skip-build

# 仅生成报告（不运行任何测试）
bash scripts/gen_report.sh --skip-build --skip-unit --skip-blackbox --skip-benchmark
```

---

## gen_report.sh 详细说明

### 功能

一键完成从编译到报告生成的完整流程：

```
Phase 1: 编译项目（Release + Debug/Test）
Phase 2: 采集环境信息
Phase 3: 运行单元测试（Google Test）
Phase 4: 运行黑盒测试（HTTP 集成测试）
Phase 5: 运行压测（wrk 基准测试）
Phase 6: 运行模糊测试（Clang libFuzzer，可选）
Phase 7: 生成汇总 Markdown 报告
```

### 命令行选项

| 选项 | 说明 |
|------|------|
| `--skip-build` | 跳过编译（已构建时使用） |
| `--skip-unit` | 跳过单元测试 |
| `--skip-blackbox` | 跳过黑盒测试 |
| `--skip-benchmark` | 跳过压测 |
| `--skip-fuzz` | 跳过模糊测试 |
| `--quick` | 快速模式：仅单元+黑盒测试 |
| `--full` | 完整模式：包含模糊测试（默认） |
| `--help` / `-h` | 显示帮助 |

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `THREADS` | `4` | 压测线程数 |
| `CONNECTIONS` | `128` | 压测连接数 |
| `DURATION` | `30s` | 压测时长 |
| `WARMUP_DURATION` | `5s` | 预热时长 |
| `TOOL` | `wrk` | 压测工具（`wrk` 或 `wrk2`） |
| `SERVER_BIN` | `build-release/webserver` | 服务器二进制路径 |

### 输出文件

```
web/
├── test_report_YYYYMMDD_HHMMSS.md   ← 汇总报告
├── test-output/
│   ├── env_info.txt                 ← 环境信息
│   ├── unittest_output.txt           ← 单元测试输出
│   ├── blackbox_output.txt          ← 黑盒测试输出
│   ├── benchmark_output.txt         ← 压测输出
│   └── fuzz_output.txt              ← 模糊测试输出
└── benchmark-results/
    └── 20260730T120000Z/
        ├── summary.tsv              ← 压测核心指标
        ├── environment.txt          ← 压测环境快照
        ├── commands.txt             ← 执行的命令
        ├── raw/current.txt          ← wrk 原始输出
        └── samples/current.tsv      ← 每秒资源采样
```

### 报告内容

生成的 Markdown 报告包含以下章节：

1. **执行摘要** — 所有测试阶段的状态一览
2. **环境信息** — 系统、编译器、内核参数、二进制哈希
3. **单元测试结果** — 测试统计、详细输出
4. **黑盒测试结果** — 12 个测试场景说明 + 输出
5. **压测结果** — 核心指标表格 + 指标说明
6. **模糊测试结果** — fuzz 输出（如有）
7. **完整启动流程指南** — 环境准备、编译、测试、报告
8. **操作命令速查** — 所有命令参考

---

## 单独运行各测试

### 单元测试

```bash
bash scripts/run_tests.sh

# 结果位置：build-tests/Testing/Temporary/LastTest.log
```

### 黑盒测试

```bash
# 需要先启动服务器
./build-release/webserver &
sleep 1

# 运行黑盒测试
python3 tests/integration/http_blackbox.py --server ./build-release/webserver

# 测试场景：
#   1. 根路由 GET / → <h1>hello</h1>
#   2. 动态路由 /user/123 → 123
#   3. 认证中间件 /admin → 401
#   4. 流式响应 /stream1 → chunked 编码
#   5. 畸形协议版本 → 连接关闭
#   6. 请求走私防护 → 连接关闭
#   7. 超大请求体 → 连接关闭
#   8. keep-alive 多请求 → 3 请求无错误
#   9. 10000 流水线请求 → 无 buffer 错乱
#  10. Range + ETag + 304 → 静态文件续传
#  11. 1GB 文件 Range → sendfile 零拷贝
#  12. Executor 验收 → /slow 不阻塞 /fast
```

### 压测

```bash
# 默认参数：4线程 128连接 30秒
bash scripts/benchmark.sh

# 高并发
THREADS=8 CONNECTIONS=1000 DURATION=60s bash scripts/benchmark.sh

# 对比两个版本
BASELINE_URL=http://127.0.0.1:8081/ bash scripts/benchmark.sh

# 查看结果
cat benchmark-results/*/summary.tsv
```

### 模糊测试

```bash
# 需要 Clang 编译器
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
    -DBUILD_TESTING=OFF \
    -DWEBSERVER_BUILD_FUZZER=ON \
    -DWEBSERVER_ENABLE_ASAN=ON \
    -DWEBSERVER_ENABLE_UBSAN=ON
cmake --build build-fuzz --target http_parser_fuzz

# 运行
mkdir -p fuzz-corpus
./build-fuzz/http_parser_fuzz fuzz-corpus -max_len=2097152
```

---

## benchmark.sh 配置

所有参数通过环境变量传入：

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `THREADS` | `4` | 负载生成线程数 |
| `CONNECTIONS` | `128` | 并发连接数 |
| `DURATION` | `30s` | 正式压测时长 |
| `WARMUP_DURATION` | `5s` | 预热时长（设 `0` 禁用） |
| `RATE` | - | wrk2 专用：目标请求速率 |
| `CURRENT_URL` | `http://127.0.0.1:8080/` | 待测服务 URL |
| `BASELINE_URL` | - | 对比基线 URL |
| `SERVER_BIN` | `build-release/webserver` | 服务器二进制 |
| `RESULTS_ROOT` | `benchmark-results` | 结果输出目录 |
| `RUN_ID` | 自动生成 | UTC 时间戳 |

### summary.tsv 字段

| 字段 | 含义 |
|------|------|
| `label` | `current` 或 `baseline` |
| `url` | 测试目标 URL |
| `qps` | 每秒请求数 |
| `latency_avg_us` | 平均延迟（微秒） |
| `latency_p95_us` | 95 分位延迟（微秒） |
| `latency_p99_us` | 99 分位延迟（微秒） |
| `errors_total` | 总错误数 |
| `rss_avg_kib` | 平均常驻内存（KB） |
| `rss_peak_kib` | 峰值常驻内存（KB） |
| `cpu_avg_percent` | 平均 CPU 占用（%） |

---

## wrk_report.lua

将 wrk/wrk2 的内部统计数据转换为机器可读的 KEY=VALUE 格式：

```
BENCH_REQUESTS=99852
BENCH_QPS=3320.130000
BENCH_LATENCY_AVG_US=415.080
BENCH_LATENCY_P95_US=900.000
BENCH_LATENCY_P99_US=1610.000
BENCH_ERRORS_TOTAL=0
...
```

供 `benchmark.sh` 和 `gen_report.sh` 解析。

---

## 完整工作流

```bash
# Step 1: 进入项目目录
cd /path/to/web

# Step 2: 一键全量测试（推荐）
bash scripts/gen_report.sh

# Step 3: 查看报告
cat test_report_*.md

# Step 4: 单独验证服务器
./build-release/webserver &
sleep 1
curl -i http://127.0.0.1:8080/          # 根路由
curl -i http://127.0.0.1:8080/user/123  # 动态路由
curl -i http://127.0.0.1:8080/admin     # 认证（返回 401）
curl -i http://127.0.0.1:8080/logo      # 静态文件
curl -i http://127.0.0.1:8080/stream1   # 流式响应
kill $(pgrep -n webserver)

# Step 5: 性能分析（可选）
sudo perf record -F 199 -g -p $(pgrep -n webserver) -- sleep 30
sudo perf script | ~/FlameGraph/stackcollapse-perf.pl | ~/FlameGraph/flamegraph.pl > cpu.svg
```

## 注意事项

1. **Linux 环境**：所有脚本仅支持 Linux（CentOS 9 等），依赖 `setsid`、`ps`、`awk`、`sysctl` 等。
2. **端口占用**：压测前确保 8080 端口空闲。
3. **编译依赖**：需要 g++、CMake、Google Test、Python 3、wrk。
4. **模糊测试**：需要 Clang 编译器（非必需，默认跳过）。
5. **结果可复现**：每次压测使用独立目录（UTC 时间戳命名），不会覆盖历史结果。