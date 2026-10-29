/** Public acceptance ledger; implementation evidence is separate from runtime health. */
export const verification={
  phase:'43–47',updated:'2026-10-29',baselineCheckpoint:'6dafe1e',
  results:{source:'24项前端逻辑、8项工程测试通过',release:'114/114',debug:'114/114',sanitizers:'ASan/LSan/UBSan 114/114',windows:'Release/Debug各5项契约；真实TLS/WS/SSE/私人模型链路通过',realWorkspace:'HTTP/WS/PostgreSQL及并发幂等通过',performance:'未通过：第2轮ACK P99增加23.33%，门槛20%；混合业务错误0'},
  knownLimits:['本轮没有GitHub推送或日常部署','CI定义已准备，远程实跑待环境启用','云模型/语音缺真实Key仍未验收','包含词搜索不是中文全文检索','精确逐事件commit到网络发送的关联仍待补齐','代理测速包含本机HTTP、Windows调度、SSH与VM，不能推广为公网承诺']
};
