# 第三阶段重要知识点 · 09 验收清单

> **标记**：第三阶段重要知识点 · 最终验收  
> **用途**：宣布「第三阶段理论+实现闭环」前自检；进入第四阶段前过一遍。

---

## A. 架构项

| 项 | 期望 | 本仓 |
|----|------|------|
| main 不直接堆砌全部基础设施 | Runtime 管理 | ✅ |
| ReactorGroup / 多 SubReactor | one thread one reactor | ✅ |
| Connection 单 Reactor 亲和 | conns 在 SubReactor 内 | ✅ |
| Session 管 HTTP 轮次 | 状态机 + Keep-Alive 循环 | ✅ |
| Parser / Sender / Executor 对象化 | 边界清晰 | ✅（细节可持续打磨） |
| Completion 回传 | Worker→队列→Reactor resume | ✅ |
| 僵尸协程 | zombieWakes | ✅ |

---

## B. 行为验收

| # | 场景 | 期望 | 证据入口 |
|---|------|------|----------|
| 1 | Keep-Alive 多请求同 fd | 复用连接、行为正确 | 黑盒 keep-alive |
| 2 | Pipeline 批量写入 | 不串包、按序可接受 | 黑盒 pipeline；单元流水线 |
| 3 | `/slow` + `/fast` | fast 立即返回 | 黑盒 Executor |
| 4 | handler 抛异常 | 500，进程不崩 | 单元/设计；可补黑盒 |
| 5 | 背压 | 池满 503 或读暂停；非无声 OOM | 代码路径有；打满池黑盒仍为缺口 |
| 6 | 畸形 / 超限 | 拒绝或断开 | 单元 + 黑盒 |
| 7 | 停服 | 信号后释端口、可重启 | Runtime；实机验证 |

一键回归：`bash scripts/gen_report.sh`

---

## C. 「思想齐」不等于「每个设计图都已实现」

下列可作为**演进清单**，勿与「第三阶段没完成」混为一谈：

- 显式多请求 Response 排序队列（并行执行同连接时才刚需）  
- 写积压独立背压水位产品化  
- 503 / 500 专项黑盒  

---

## D. 通过标准（建议）

- [ ] 能默画 Reactor / Runtime / Session / Completion 四张图  
- [ ] 能讲清 zombie 与 connId  
- [ ] `/slow`+`/fast` 与 gen_report 绿  
- [ ] 书面同意：第四阶段前先读 [10_bridge_to_phase4.md](10_bridge_to_phase4.md)，**本仓停扩协议**仍有效  
