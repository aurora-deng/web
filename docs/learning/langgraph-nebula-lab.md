# 从 NEBULA 的审批流程学 LangGraph

本练习把「读取工单、提议、暂停审批、交接」用 Python 表达。项目中的真实工单由 C++ DAG、gRPC 和 SQLite 处理。LangGraph 练习使用合成工单，适合先弄懂图、状态与恢复，再阅读项目的实现。 [NEBULA Agent 实战讲义](nebula-fleet-agent-and-skills.md) · [LangGraph 图接口](https://docs.langchain.com/oss/python/langgraph/graph-api)

## 1. 五个概念

| 概念 | 怎么理解 | 在 NEBULA 里的对应物 |
|---|---|---|
| `State` | 每一步共用的工单夹，里面写事实、草稿与审批结果 | 本次调用的 `Request`、`TaskEvidence`、`Proposal` 和保存后的审批记录 |
| 节点 | 填写工单夹的一道工序，返回需要更新的字段 | `read_task_status`、`verify_proposal`、`stage_review` 和 `approve_review` |
| 条件边 | 看当前记录决定走哪条出口 | 无合法候选时退出，有草稿时进入人工确认，批准后进入交接 |
| `interrupt()` | 把工单夹存到检查点并暂停，让人处理下一步 | SQLite 中保存的待审批工单，让 CLI 退出后仍可通过 `inspect` 查看 |
| `Command(resume=...)` | 把人给出的决定送回同一条工作流 | `approve` 或 `reject` 只需提交同一个提案 ID，协调器读取已经固定的内容 |

`thread_id` 指定要从哪条图执行历史恢复。`traceId` 主要用于查一次调用的事件链，现有实验也用它作为不可变提案 ID。用户身份和审批权限仍需单独校验。LangGraph 可通过 reducer 定义多节点对同一字段的合并，例如对事件列表追加；若两个并行节点都产生同一字段，先想清是追加、覆盖还是拆成两个字段。 [状态与节点](https://docs.langchain.com/oss/python/langgraph/graph-api) · [检查点持久化](https://docs.langchain.com/oss/python/langgraph/persistence)

## 2. 最小可执行练习

在已有 LangGraph 的独立 Python 环境里，将下面的代码保存为 `langgraph_nebula_lab.py`，执行 `python langgraph_nebula_lab.py`。它使用 `InMemorySaver`，运行过程只携带合成数据。本项目当前环境未安装 LangGraph，这个示例以官方接口和语法校验为证据。

```python
from typing import TypedDict

from langgraph.checkpoint.memory import InMemorySaver
from langgraph.graph import END, START, StateGraph
from langgraph.types import Command, interrupt


class State(TypedDict, total=False):
    task_id: str
    phase: str
    epoch: int
    proposal_id: str
    approved: bool
    result: str


def load_evidence(state: State) -> dict:
    # 合成夹具。这个节点在项目里对应只读 gRPC Status。
    return {"phase": "queued" if state["task_id"] == "demo" else "leased", "epoch": 8}


def plan(state: State) -> dict:
    return {"proposal_id": f"{state['task_id']}-{state['epoch']}"}


def ask_operator(state: State) -> dict:
    decision = interrupt({"proposal_id": state["proposal_id"], "task_id": state["task_id"]})
    matches = isinstance(decision, dict) and decision.get("proposal_id") == state["proposal_id"]
    return {"approved": matches and decision.get("approved") is True}


def handoff(_: State) -> dict:
    return {"result": "handoff_ready"}


def refuse(_: State) -> dict:
    return {"result": "stop"}


builder = StateGraph(State)
builder.add_node("read", load_evidence)
builder.add_node("plan", plan)
builder.add_node("review", ask_operator)
builder.add_node("handoff", handoff)
builder.add_node("refuse", refuse)
builder.add_edge(START, "read")
builder.add_conditional_edges("read", lambda state: "plan" if state["phase"] == "queued" else "refuse",
                              {"plan": "plan", "refuse": "refuse"})
builder.add_edge("plan", "review")
builder.add_conditional_edges("review", lambda state: "handoff" if state["approved"] else "refuse",
                              {"handoff": "handoff", "refuse": "refuse"})
builder.add_edge("handoff", END)
builder.add_edge("refuse", END)
graph = builder.compile(checkpointer=InMemorySaver())

for thread, answer, expected in [("approved", "demo-8", "handoff_ready"), ("wrong-id", "other", "stop")]:
    config = {"configurable": {"thread_id": thread}}
    paused = graph.invoke({"task_id": "demo"}, config)
    assert paused["__interrupt__"][0].value["proposal_id"] == "demo-8"
    resumed = graph.invoke(Command(resume={"approved": True, "proposal_id": answer}), config)
    assert resumed["result"] == expected
    print(thread, resumed["result"])

leased = graph.invoke({"task_id": "busy"}, {"configurable": {"thread_id": "busy"}})
assert leased["result"] == "stop"
```

## 3. 恢复、幂等和权限一起理解

1. **恢复仍会再进节点。** 使用同一个 `thread_id` 恢复时，含 `interrupt()` 的节点从开头再次执行。把外部写入放到确认后的独立节点。遇到可能重复执行的写入，给业务端稳定的幂等键。 [Interrupts](https://docs.langchain.com/oss/python/langgraph/interrupts)
2. **进程退出与节点恢复是两件事。** `InMemorySaver` 随进程退出丢失。持久化 saver 保存图状态，协调器的审批账本保存授权和事务结果。即便以后使用持久化 saver，也要在实际派发时用同一提案 ID 向协调器核对。 [持久化指南](https://docs.langchain.com/oss/python/langgraph/persistence) · [NEBULA 审批事务](../../backend/fleet/infrastructure/SqliteFleetLedger/SimulationReviewSql.cpp)
3. **结构化输出降低解析成本。** Python 练习把规划写成固定函数。接 Ollama 后向 Provider 提供 JSON Schema，并由 C++ 重读状态、核对 Skill ID、版本和来源。真实运行可以用 `modelStatus` 和 `sameAsRuleDraft` 分开记录 Provider 是否响应、候选是否与规则一致。 [Ollama 结构化输出](https://docs.ollama.com/capabilities/structured-outputs) · [现有 Provider 适配](../../tools/agent/ollama_planner.py)
4. **代码决定操作权限。** 图中的 `approved=True` 只是一段数据。真实系统由协调器核对只读、起草、审批三种凭据，最后才写事务。把 `wrong-id` 分支改成 `approved=False`，再观察同一个 `refuse` 节点如何承接人工拒绝。

理解到 D2 时，能画出成功路径的四个节点和两条失败出口。到 D3 时，能解释检查点丢失、重复审批与设备停机分别由谁处理，并在隔离进程里复现一个失败。交接是这节课的终点，项目中的适配入口是 ROS 2 Action。 [M01 Agent 目标](../roadmap/embodied-systems/m01-agent.md) · [M02 Skill 合同](../roadmap/embodied-systems/skills/README.md)
