# 可复现性能实验

本文给出实验协议和结果模板。虚拟机上一次 `GET /` 快照见 [../closeout/CLOSEOUT.md](../closeout/CLOSEOUT.md)；
**不能**外推为物理机上限或与 nginx 的正式对比结论。

## 阅读目标与设计动机

本文的功能不是为项目提供“高性能”标签，而是规定如何得到可复核的性能证据：先保证响应语义正确，
再固定环境和负载，最后同时观察吞吐、尾延迟、错误与资源占用。采用重复实验、同机同参 baseline、
原始输出留存和固定到达率场景，是为了隔离环境漂移、错误流量及 coordinated omission，避免把偶然峰值
误写成框架能力。

它与 [../architecture/ARCHITECTURE.md](../architecture/ARCHITECTURE.md) 和 [../testing/TESTING.md](../testing/TESTING.md) 形成“设计假设—正确性门禁—
性能证据”的闭环：架构文档提出 Reactor、协程、背压和发送路径的取舍，测试先确认行为未被破坏，
本实验再判断取舍是否值得。这个闭环让 Web 项目可以依据数据完成收口和有限优化，也可直接迁移到
ROS 2/DDS 通信、实时周期与设备网关的延迟、抖动、丢包和 CPU 评估，为机器人方向保留同一套
可复现、不过度宣称的工程方法。

## 目标与原则

- 回答“在相同机器、网络、响应语义和负载下，吞吐、尾延迟、错误、CPU、RSS 有何差异”，
  而不是只追求一个最高 QPS 数字。
- 当前服务与 baseline 必须由同一台压测机、同一工具版本、同一 URL 响应大小、同一线程/连接/时长
  驱动；每个场景至少独立重复 5 次。
- 先做正确性检查，再预热，再记录；错误不为 0 的结果不能作为最大可持续吞吐。
- 保存原始输出和环境，禁止手工补齐缺失指标。脚本无法观测远端 baseline 进程时，RSS/CPU 保持 `NA`。
- 服务端与负载发生器最好分机部署；单机实验必须同时报告负载发生器是否打满 CPU。

## 准备

建议使用独占的物理 Linux 主机，关闭无关任务并记录电源策略。安装示例：

```bash
sudo apt update
sudo apt install build-essential cmake python3 libgtest-dev \
  wrk linux-tools-common linux-tools-generic flamegraph
```

wrk2 通常需要从其上游源码构建，生成的二进制也可能名为 `wrk`。这时使用
`TOOL=wrk2 TOOL_BIN=/absolute/path/to/wrk2-or-wrk`，并记录源码 revision。不要把普通 wrk
误标为 wrk2。

先完成 Release 构建和正确性测试：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build-release --parallel

bash scripts/run_tests.sh
curl -i http://127.0.0.1:8080/
```

若需固定 CPU，记录并按机器实际拓扑选择核：

```bash
sudo cpupower frequency-set --governor performance
taskset -c 0-7 bash scripts/benchmark.sh
```

不要在未披露的情况下为某一方单独调整编译器参数、CPU 亲和性、连接限制或内核参数。

## 脚本行为与输出

`scripts/benchmark.sh` 在 Linux 上执行以下流程：

1. 校验工具和参数，确认 8080 未被占用；
2. 从仓库根目录以独立进程组启动当前服务，并轮询 HTTP 就绪；
3. 使用同一参数预热，然后运行 wrk 或 wrk2；
4. 通过 `scripts/wrk_report.lua` 从工具内部直出 QPS、平均/P95/P99 延迟和分类错误；
5. 每秒用 `ps` 采样服务进程 RSS 与 `%CPU`；
6. TERM 整个服务进程组，超时后 KILL，任何退出路径都执行清理；
7. 如果设置 `BASELINE_URL`，再以同一负载测试 baseline。

默认输出目录为 `benchmark-results/<UTC 时间>/`：

- `environment.txt`：OS、内核、CPU、内存、ulimit、关键 sysctl、工具版本和服务二进制 SHA-256；
- `commands.txt`：可直接审计的完整 warm-up/测量命令；
- `raw/*.txt`：wrk/wrk2 原始输出和 Lua 指标；
- `samples/*.tsv`：时间戳、RSS KiB、进程 `%CPU`；
- `summary.tsv`：URL、QPS、平均/P95/P99（微秒）、总错误、平均/峰值 RSS、平均 CPU。

`ps %CPU` 是进程从启动至采样时刻的累计平均值，多线程进程可能超过 100%；脚本对这些观测值再取平均。
需要精确时间窗 CPU 时，应以 `pidstat`/perf stat 结果为准。baseline 在本机运行时可通过
`BASELINE_PID` 提供待采样 PID；远端或容器内 baseline 默认显示 `NA`。

常用运行方式：

```bash
# 固定并发，比较本机 8081 的 baseline
TOOL=wrk THREADS=4 CONNECTIONS=128 DURATION=30s WARMUP_DURATION=10s \
  BASELINE_URL=http://127.0.0.1:8081/ BASELINE_PID="$(pgrep -n nginx)" \
  RUN_ID=root-c128-r1 bash scripts/benchmark.sh

# 固定到达率；逐级提高 RATE 找到错误为 0、P99 可接受的最大档位
TOOL=wrk2 TOOL_BIN=/opt/wrk2/wrk RATE=20000 \
  THREADS=4 CONNECTIONS=128 DURATION=60s WARMUP_DURATION=10s \
  RUN_ID=root-r20k-r1 bash scripts/benchmark.sh
```

`CURRENT_URL` 可改变压测路径，但当前服务固定绑定 `127.0.0.1:8080` 对应的端口。运行 `/logo`
时应先确认 baseline 返回同样的文件字节数。`/stream2` 含同步 sleep，不用于吞吐基准。

## 实验矩阵

每项都应对 current 与 baseline 做同序号重复，并随机化二者先后顺序以减小温度/缓存漂移。

### A. 固定并发吞吐（wrk）

- 路由：`/`（小内存响应）、`/user/123`（动态路由）、`/logo`（小静态文件）。
- 连接数：1、16、64、128、512、1000。
- wrk 线程数：`min(4, 压测机逻辑核数)`，另做 1 线程对照。
- 时长：预热 10 秒，测量 60 秒。
- 重复：每个组合 5 次；报告中位数，并给出最小/最大或 MAD。

### B. 固定到达率尾延迟（wrk2）

- 路由：先使用 `/`，确认后再扩展其他路由。
- 连接数：128 和 512。
- RATE：从低负载开始，每档约增加 25%，直到错误出现或 P99 超过事先定义的 SLO。
- 时长：预热 10 秒，测量至少 60 秒；接近饱和点时建议 5 分钟。
- 重点：wrk2 用恒定到达率减少 coordinated omission，比较同 RATE 下的 P95/P99，而非只比较实际 QPS。

### C. 稳定性与资源

- 固定在最大无错误负载的 70%～80%；
- 持续 30 分钟、2 小时，必要时 24 小时；
- 每分钟记录 RSS、CPU、fd 数、上下文切换和错误；
- 判断 RSS 是否预热后稳定、连接是否回收、P99 是否随时间漂移。

### D. 功能边界（不计入性能排名）

- keep-alive 与 `Connection: close`；
- 1 MiB 附近请求体、慢读/慢写客户端；
- Range、404、未授权 `/admin`；
- handler 阻塞对同一 Reactor 上其他连接的影响。

## 结果模板

为每个场景复制以下区块，不要在未执行时填 `0`：

```text
实验 ID：
日期/操作者：
服务版本或二进制 SHA-256：
baseline 名称、版本、配置 revision：
服务端机器：
压测机：
网络拓扑：
CPU governor / taskset：
内核和 ulimit：
完整命令：
响应路径、状态、字节数：
预热 / 测量时长：
重复次数：

current:
  QPS（各轮；中位数；离散度）：
  latency avg / P95 / P99：
  connect/read/write/status/timeout errors：
  RSS avg / peak：
  CPU（ps 与 pidstat/perf stat 口径）：

baseline:
  QPS（各轮；中位数；离散度）：
  latency avg / P95 / P99：
  connect/read/write/status/timeout errors：
  RSS avg / peak（不可观测则 NA）：
  CPU（不可观测则 NA）：

差异：
  QPS 比值：
  同 RATE 下 P99 比值：
  每 10k QPS CPU：
  每连接 RSS：

异常与失效轮次（必须保留原始输出并说明剔除理由）：
结论（限定在本环境、本路径、本负载）：
后续验证：
```

推荐用独立脚本从多个 `summary.tsv` 计算统计量，计算脚本与输入一起保存。不要复制终端显示后手算，
不要选择性报告最好一轮。

## nginx 对比

nginx 是成熟事件驱动服务器，适合作为静态/小响应参考，不应宣称两者功能完全等价。示例配置：

```nginx
worker_processes auto;
worker_rlimit_nofile 200000;

events {
    worker_connections 65535;
    use epoll;
}

http {
    access_log off;
    sendfile on;
    keepalive_timeout 30;

    server {
        listen 8081;
        location = / {
            default_type text/html;
            return 200 "<h1>hello</h1>";
        }
        location = /logo {
            alias /absolute/path/to/repository/static/logo.svg;
        }
    }
}
```

验证响应语义与大小后启动并传入 master 或 worker PID：

```bash
curl -sS -D- http://127.0.0.1:8080/ -o /tmp/current.body
curl -sS -D- http://127.0.0.1:8081/ -o /tmp/nginx.body
cmp /tmp/current.body /tmp/nginx.body

BASELINE_URL=http://127.0.0.1:8081/ \
BASELINE_PID="$(pgrep -n 'nginx')" \
RUN_ID=nginx-root-c128-r1 bash scripts/benchmark.sh
```

nginx 多 worker 时单个 PID 的 RSS/CPU 不代表总量，应对 worker 集合求和，或用 systemd/cgroup
统计整个服务。记录 nginx 版本、完整配置、worker 数、是否开启日志和 sendfile。

## muduo 对比

muduo 是网络库而非固定 HTTP benchmark。公平比较需要创建并保存一个最小基准程序：

1. 使用 `muduo::net::HttpServer`（或版本对应实现）监听 8081；
2. `/` 返回与当前服务完全相同的状态、body、Content-Type 和 keep-alive 行为；
3. EventLoopThread 数量设为与当前 SubReactor 数量一致，并记录该数值；
4. Release 构建，使用相同编译器和优化级别，关闭同步访问日志；
5. 保存 muduo commit、benchmark 源码、链接参数和启动命令；
6. 先用 curl/cmp 和协议检查确认等价，再设置 `BASELINE_URL` 与 `BASELINE_PID` 运行同一矩阵。

如果 muduo 示例把业务投递到线程池而当前实现在线程内执行，应分别做“纯事件循环空 handler”和
“相同业务线程模型”两组，不能混为一个结论。

## perf、火焰图与系统观测

先获取服务 PID。脚本运行时可用 `pgrep -n webserver`，也可手动启动 Release 服务：

```bash
cd /absolute/path/to/repository
./build-release/webserver &
SERVER_PID=$!
```

### perf stat

在稳定测量窗口记录 CPU 和调度指标：

```bash
sudo perf stat -p "${SERVER_PID}" -a --timeout 30000 \
  -e task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations,page-faults
```

部分 perf 版本不允许同时使用 `-p` 和 `-a`；此时去掉 `-a`。重点解释 IPC、cache miss、
上下文切换和 task-clock 是否随 QPS 线性变化，不要仅凭单一计数判断优化有效。

### CPU 火焰图

```bash
sudo perf record -F 199 -g -p "${SERVER_PID}" -- sleep 30
sudo perf script > out.perf
stackcollapse-perf.pl out.perf > out.folded
flamegraph.pl --title "webserver CPU" out.folded > cpu-flamegraph.svg
```

**读图教学与本仓实战解读**（含样本图）：  
[../testing/learning/测试学习综合讲义和笔记/火焰图生成命令流与读图教学.md](../testing/learning/测试学习综合讲义和笔记/火焰图生成命令流与读图教学.md)

若调用栈大量为 `[unknown]`，用 `RelWithDebInfo` 重建并保留 frame pointer：

```bash
cmake -S . -B build-profile \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer"
cmake --build build-profile --parallel
```

### off-CPU（可选）

有 BCC/bpftrace 环境时再记录 off-CPU；先验证脚本与当前内核兼容：

```bash
sudo offcputime-bpfcc -df -p "${SERVER_PID}" 30 > offcpu.folded
flamegraph.pl --color=io --title "webserver off-CPU" offcpu.folded > offcpu-flamegraph.svg
```

### 辅助观测

```bash
pidstat -p "${SERVER_PID}" -rudw 1
ss -s
ls "/proc/${SERVER_PID}/fd" | wc -l
cat "/proc/${SERVER_PID}/status"
```

## 解释规范

- 先报告正确性和错误。吞吐增加但 timeout/status error 同时增加不是有效提升。
- wrk 用于固定并发下的完成速率；接近过载时会受 coordinated omission 影响。尾延迟结论优先使用
  wrk2 固定 RATE，并说明实际完成 QPS 是否跟上目标 RATE。
- 平均延迟不能代替 P95/P99。至少同时报告三者及测试时长；长尾稀有事件需要更长时间窗。
- 比较吞吐时同时报告 CPU；比较内存时说明是单进程 RSS、worker 总 RSS 还是 cgroup memory。
- 火焰图的宽度代表采样占比，不直接等于“函数很慢”。结合调用次数、perf stat 和 A/B 复测定位。
- 发现热点后一次只改一个变量，至少重复 5 次；优化收益小于环境波动时结论应为“不显著”。
- 所有结论限定到具体硬件、内核、编译器、响应路径和负载。
- 虚拟机快照（约 2.3 万～3 万 QPS）见 [../closeout/CLOSEOUT.md](../closeout/CLOSEOUT.md)；未做同机 nginx/muduo 正式对比前，
  不得宣称优于竞品。c1000 延迟升高若符合 Little 定律（连接数/QPS），应解释为排队而非回归。
