/** Public acceptance ledger; implementation evidence is separate from runtime health. */
export const verification={
  phase:'49',updated:'2026-10-31',baselineCheckpoint:'42b3f8f',
  results:{source:'33项前端逻辑、14项工程测试通过',contracts:'119项HTTP方法/路径、18个领域绑定；字段/权限另用契约和真实测试验证',release:'115/115',debug:'115/115',sanitizers:'ASan/LSan/UBSan 115/115',lifecycle:'三种构建各4项启动失败清理检查通过',windows:'Release/Debug各5项契约；真实TLS/WS/SSE/私人模型、迁移、Agent和语音回退通过',realBusiness:'10组真实业务通过，包含HTTP/2 ALPN、同连接Stream重置、PG权限、群AI与网页AI',palette:'七组浏览器回归，三种尺寸、深浅主题与抽样对比度通过',performance:'未通过：本轮/fast中位下降3.89%，ACK第3轮8.710→15.545ms（+78.47%），业务错误0；历史失败保留'},
  knownLimits:['本轮没有GitHub推送或日常部署，原业务库未写入，无新增迁移','CI定义与本地矩阵通过，远程执行待环境启用','路由目录不是完整字段Schema；Windows本地接口和外部模型API属于独立边界','旧Store门面、聚合头与少数兼容委托保留，未将所有遗留对象强制重命名为DO/VO','云模型/语音缺真实Key仍未验收，音频生成不代表自然度达标','包含词搜索不是中文全文检索','通知/批次观测不是逐消息commit到网络发送关联','目录拆分本身不证明性能收益；代理资源只计原生进程']
};
