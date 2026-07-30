-- wrk/wrk2 共用的机器可读报告适配器。
-- 将工具内部 summary/latency 对象转换为稳定的 KEY=VALUE 行，使 shell 脚本
-- 无需解析易随版本或本地化变化的人类输出。wrk 的延迟与 duration 均以微秒计。
--
-- done 是 wrk 在压测完成后调用的标准钩子；requests 参数保留在签名中以兼容
-- wrk/wrk2 Lua API，当前统计只需要 summary 和 latency。
done = function(summary, latency, requests)
  -- 某些版本在没有对应错误时省略 errors 或具体字段；归零可确保报告列稳定，
  -- 同时仍将连接、读、写、HTTP 状态和超时分开，便于定位性能下降的性质。
  local errors = summary.errors or {}
  local connect = errors.connect or 0
  local read = errors.read or 0
  local write = errors.write or 0
  local status = errors.status or 0
  local timeout = errors.timeout or 0
  local total_errors = connect + read + write + status + timeout
  local qps = 0

  -- duration 为零或缺失时不执行除法，以 0 明确表示不可计算；正常情况下
  -- 乘 1,000,000 将“每微秒请求数”转换为每秒请求数。
  if summary.duration and summary.duration > 0 then
    qps = summary.requests * 1000000 / summary.duration
  end

  -- 固定小数精度和字段名构成 benchmark.sh 的内部数据契约。平均、P95、P99
  -- 同时输出可区分整体成本和尾延迟风险，为后续性能门禁提供可比较指标。
  io.write(string.format("BENCH_REQUESTS=%d\n", summary.requests or 0))
  io.write(string.format("BENCH_QPS=%.6f\n", qps))
  io.write(string.format("BENCH_LATENCY_AVG_US=%.3f\n", latency.mean or 0))
  io.write(string.format("BENCH_LATENCY_P95_US=%.3f\n", latency:percentile(95.0)))
  io.write(string.format("BENCH_LATENCY_P99_US=%.3f\n", latency:percentile(99.0)))
  io.write(string.format("BENCH_ERRORS_TOTAL=%d\n", total_errors))
  io.write(string.format("BENCH_ERRORS_CONNECT=%d\n", connect))
  io.write(string.format("BENCH_ERRORS_READ=%d\n", read))
  io.write(string.format("BENCH_ERRORS_WRITE=%d\n", write))
  io.write(string.format("BENCH_ERRORS_STATUS=%d\n", status))
  io.write(string.format("BENCH_ERRORS_TIMEOUT=%d\n", timeout))
end
