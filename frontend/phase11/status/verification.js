/** Public acceptance ledger; implementation evidence is separate from runtime health. */
export const verification={
  phase:'48',updated:'2026-10-30',baselineCheckpoint:'1c3c7e6',
  results:{source:'28项前端逻辑、12项工程测试通过',release:'115/115',debug:'115/115',sanitizers:'ASan/LSan/UBSan 115/115',windows:'Release/Debug各5项契约；真实TLS/WS/SSE/私人模型链路通过',realWorkspace:'HTTP/WS/PostgreSQL、并发幂等与新观测指标通过',palette:'三种尺寸、深浅主题、动态弹窗与抽样文字对比度通过',performance:'未通过：最终对最初基线第3轮ACK P99 +23.51%；对直接上一版也有两轮超标，业务错误0'},
  knownLimits:['本轮没有GitHub推送或日常部署，原业务库未写入','CI定义与本地矩阵通过，远程执行待环境启用','云模型/语音缺真实Key仍未验收，生成音频不代表自然度达标','包含词搜索不是中文全文检索','通知调度/批次观测不是精确逐消息commit到网络发送关联','此前性能通过组属于较早构建，不能替代最终失败记录','代理资源仅计原生进程，延迟不能推广为公网承诺']
};
