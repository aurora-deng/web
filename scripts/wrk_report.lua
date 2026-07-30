-- wrk/wrk2 共用的机器可读报告适配器。
-- 将工具内部 summary/latency 对象转换为稳定的 KEY=VALUE 行。
-- 对缺少 percentile 的构建做保护，避免 warm-up/正式压测因 Lua 报错直接失败。

local function safe_percentile(latency, n)
  if latency == nil then
    return 0
  end
  if type(latency.percentile) == "function" then
    local ok, value = pcall(function()
      return latency:percentile(n)
    end)
    if ok and type(value) == "number" then
      return value
    end
  end
  -- 回退：用 mean 占位，保证 shell 侧总能读到字段
  if type(latency.mean) == "number" then
    return latency.mean
  end
  return 0
end

done = function(summary, latency, requests)
  local errors = summary.errors or {}
  local connect = errors.connect or 0
  local read = errors.read or 0
  local write = errors.write or 0
  local status = errors.status or 0
  local timeout = errors.timeout or 0
  local total_errors = connect + read + write + status + timeout
  local qps = 0

  if summary.duration and summary.duration > 0 then
    qps = (summary.requests or 0) * 1000000 / summary.duration
  end

  io.write(string.format("BENCH_REQUESTS=%d\n", summary.requests or 0))
  io.write(string.format("BENCH_QPS=%.6f\n", qps))
  io.write(string.format("BENCH_LATENCY_AVG_US=%.3f\n", (latency and latency.mean) or 0))
  io.write(string.format("BENCH_LATENCY_P95_US=%.3f\n", safe_percentile(latency, 95.0)))
  io.write(string.format("BENCH_LATENCY_P99_US=%.3f\n", safe_percentile(latency, 99.0)))
  io.write(string.format("BENCH_ERRORS_TOTAL=%d\n", total_errors))
  io.write(string.format("BENCH_ERRORS_CONNECT=%d\n", connect))
  io.write(string.format("BENCH_ERRORS_READ=%d\n", read))
  io.write(string.format("BENCH_ERRORS_WRITE=%d\n", write))
  io.write(string.format("BENCH_ERRORS_STATUS=%d\n", status))
  io.write(string.format("BENCH_ERRORS_TIMEOUT=%d\n", timeout))
end
