"""Explain an empty local panel using observations from its requested window."""
from tools.observability.observer.contracts.operation_catalog import FUNCTIONS


def panel_family(key):
    if key.startswith("function_"):
        prefix=next((name for name in sorted(FUNCTIONS,key=len,reverse=True) if key.startswith(name+'_')),None)
        return "nebula_function_seconds", prefix+"_rate" if prefix else None
    if key.startswith("chat_"):
        return "nebula_chat_stage_seconds", None
    if key.startswith(("http_", "history_")) and key != "http_5xx_pct":
        return "nebula_http_handler_seconds", key.split("_", 1)[0]+"_rate"
    return None, None


def empty_diagnostic(key, rows, end, retention_floor, step=5):
    def result(reason, message, guide):
        return {"reason": reason, "message": message, "guide": guide,
                "sampleAt": rows[-1][0] if rows else None}

    if end < retention_floor:
        return result("expired", "这个窗口已超出保留期", "选择最近七天内的时间范围。")
    if not rows:
        return result("no_samples", "这个时间段没有采样", "扩大时间范围，或等待采集器写入新样本。")
    stamp, latest = rows[-1]
    if end-stamp > max(15,3*step):
        return result("stale", "窗口末端采样已中断", "查看服务目标与采集质量，检查采集进程和数据源连接。")
    capture = latest.get("_metrics", {})
    reliability = latest.get("_reliability", {})
    if capture.get("status") in (401, 403):
        return result("scrape_denied", "指标采集鉴权失败", "核对指标采集凭据及运维访问权限。")
    if reliability.get("scrape") is False:
        return result("scrape_failed", "当前指标采集失败", "检查业务就绪、指标接口和采集连接。")
    family, rate = panel_family(key)
    if family=='nebula_function_seconds' and 'functions' in capture and rate:
        symbol=FUNCTIONS.get(rate.removesuffix('_rate'))
        if symbol not in capture['functions']:
            return result('missing_function','当前服务未提供这个业务埋点','核对业务程序版本；已有聊天埋点不代表其他业务已经启用。')
        if key.endswith(('_error_rate','_in_flight')) and capture.get('functionOutcomes') is False:
            return result('missing_outcomes','当前服务未提供函数完成状态指标','核对业务程序版本；不能将缺少失败或执行中指标显示为零。')
    if family and "histograms" in capture and family not in capture["histograms"]:
        return result("missing_metric", "当前采样未收到所需耗时指标",
                      "核对服务版本和业务埋点设置，再触发对应业务请求。")
    rates = [values[rate] for _, values in rows if rate and rate in values]
    if rates and all(value == 0 for value in rates):
        return result("no_traffic", "采样区间内没有新增业务调用", "调用对应业务后等待两次采样，或扩大时间范围。")
    if family and family in capture.get("histograms", ()) and not rates:
        return result("warming", "尚未形成有效耗时区间", "触发对应业务并等待两次有效采样。")
    return result("no_samples", "这个时间段暂无有效样本", "查看查询详情，核对指标、实例和时间范围。")
