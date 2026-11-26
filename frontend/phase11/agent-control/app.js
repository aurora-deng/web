"use strict";
const $ = id => document.getElementById(id);
const effects = {read: "只读", proposal: "人工提案", external: "真实写入（须审批）"};
const states = {pending: "待审批", approved: "已批准", rejected: "已拒绝", revoked: "已撤销", expired: "已过期", executing: "执行中", submitted: "已提交异步操作", succeeded: "已完成", failed: "失败", uncertain: "结果待核实"};
const errors = {denied: "无权限或缺少确认", not_found: "对象不存在或不可见", invalid: "参数不符合约束", conflict: "状态冲突，请刷新后重试", unavailable: "能力未配置或暂不可用", timeout: "请求超时，请先核实执行状态"};
const descriptions = {
  "fleet.get_task_status": "查询一个真实任务的当前状态。",
  "fleet.fleet.overview": "列出最多 16 条任务，不代表机器人清单或全局统计。",
  "fleet.review_failures": "为最多 3 个失败或状态不确定的任务生成诊断提案。",
  "fleet.propose_sim_reschedule": "为一个排队中的模拟任务提出重排；审批后才创建替代模拟任务。",
  "fleet.task.reassign": "仅记录改派提案，不直接下发车队命令。",
  "fleet.robot.quarantine": "仅记录隔离提案，不直接控制机器人。",
  "workflow.run.trace": "读取一个运行的持久化检查点，最多 64 个节点。",
  "metrics.query": "只读取已配置且实际采样的指标，不生成模拟数值。",
  "chat.recall": "在当前账号工作区或指定会话中进行有界检索。",
  "chat.summarize": "围绕一个真实消息生成有界摘录式摘要，不读取整段历史。",
  "chat.draft_reply": "基于有界上下文生成回复草稿，不发送消息。",
  "chat.remind": "为指定消息生成提醒提案，不发送通知。",
  "chat.escalate": "记录人工接手提案，不联系外部系统。",
  "study.search_material": "检索当前账号的资料元数据，不扫描整个附件。",
  "study.explain": "读取指定资料定位处的一段有界内容。",
  "study.quiz": "选择一道已保存且尚未掌握的题目。",
  "study.log_progress": "审批后向当前账号写入一条学习进度；同一幂等键防止重复记账。",
  "study.mistake_add": "审批后向当前账号写入一条错题记录。",
  "study.plan_day": "根据已有进度与偏好生成每日计划提案，不写入日历。"
};
let catalog = [], access = null, busy = false;
function show(value) { $("result").textContent = JSON.stringify(value, null, 2); }
function fail(error) { show({error: error.message}); $("status").textContent = error.message; }
function localDate() {
  const now = new Date();
  return [now.getFullYear(), String(now.getMonth() + 1).padStart(2, "0"), String(now.getDate()).padStart(2, "0")].join("-");
}
function freshKey() { const bytes = new Uint8Array(16); crypto.getRandomValues(bytes); return "agent-" + Array.from(bytes, value => value.toString(16).padStart(2, "0")).join(""); }
function parametersFor(name) {
  const samples = {
    "fleet.get_task_status": {taskId: ""}, "fleet.fleet.overview": {entity: "tasks", limit: 16},
    "fleet.review_failures": {taskIds: [""]}, "fleet.propose_sim_reschedule": {taskId: ""},
    "fleet.task.reassign": {taskId: "", robotId: ""}, "fleet.robot.quarantine": {robotId: "", reason: ""},
    "workflow.run.trace": {runId: "", nodeLimit: 16}, "metrics.query": {names: ["fleet.active_tasks"], windowSeconds: 60},
    "chat.recall": {query: "", scope: "workspace", limit: 10},
    "chat.summarize": {conversationId: "", messageId: "", radius: 1},
    "chat.draft_reply": {conversationId: "", messageId: "", radius: 1},
    "chat.remind": {conversationId: "", messageId: ""}, "chat.escalate": {conversationId: "", reason: ""},
    "study.search_material": {query: "", subject: "", type: "note", limit: 5},
    "study.explain": {materialId: "", anchor: {kind: "page", offset: 1}}, "study.quiz": {subject: ""},
    "study.log_progress": {date: localDate(), subject: "", itemId: "", minutes: 25},
    "study.mistake_add": {subject: "", itemId: "", text: ""}, "study.plan_day": {date: localDate(), subject: ""}
  };
  const result = samples[name] || {};
  if (name === "study.log_progress" || name === "study.mistake_add") result.idempotencyKey = freshKey();
  return result;
}
async function api(path, body) {
  const options = {credentials: "same-origin", headers: {Accept: "application/json"}};
  if (body !== undefined) {
    options.method = "POST";
    options.headers["Content-Type"] = "application/json";
    if (access?.csrfToken) options.headers["X-CSRF-Token"] = access.csrfToken;
    options.body = JSON.stringify(body);
  }
  const response = await fetch("/api/private/agent-control/" + path, options);
  let result;
  try { result = await response.json(); } catch { throw new Error("服务未返回有效 JSON，请检查登录状态与网关连接。"); }
  if (!response.ok) {
    const code = result?.error?.code || result?.message;
    throw new Error(errors[code] || `请求失败（HTTP ${response.status}）`);
  }
  return result;
}
function selected() { return catalog.find(item => item.name === $("skill").value); }
function updateActions() {
  const spec = selected();
  const unavailable = !access || !spec || spec.availability === "unavailable";
  $("plan").disabled = busy || unavailable;
  $("invoke").disabled = busy || unavailable;
  $("confirmed").disabled = busy || unavailable || spec.effect !== "external";
  $("invoke").textContent = spec?.effect === "external" ? "提交写入申请" : spec?.effect === "proposal" ? "生成人工提案" : "执行只读查询";
}
function select() {
  const spec = selected();
  if (!spec) { updateActions(); return; }
  $("parameters").value = JSON.stringify(parametersFor(spec.name), null, 2);
  $("description").textContent = descriptions[spec.name] || spec.description || "";
  $("schema").textContent = JSON.stringify(spec.parameters, null, 2);
  $("confirmed").checked = false;
  $("constraints").replaceChildren();
  for (const value of [effects[spec.effect] || spec.effect, "v" + spec.version, spec.timeoutMs + " ms", ...(spec.preconditions || [])]) {
    const chip = document.createElement("span"); chip.textContent = value; $("constraints").append(chip);
  }
  $("status").textContent = spec.availability === "unavailable" ? "该能力未配置，不能执行；不会返回虚构结果。" : "请填写当前账号可见的真实对象编号；空字符串是待填写项，不是演示数据。";
  updateActions();
}
function button(text, handler, cls = "secondary") {
  const element = document.createElement("button"); element.textContent = text; element.className = cls;
  element.onclick = async () => {
    element.disabled = true;
    try { await handler(); } catch (error) { fail(error); } finally { element.disabled = false; }
  };
  return element;
}
function metadata(row, value) {
  const detail = document.createElement("details"), summary = document.createElement("summary"), pre = document.createElement("pre");
  summary.textContent = "检查申请内容"; pre.textContent = JSON.stringify(value, null, 2); detail.append(summary, pre); row.append(detail);
}
async function refresh() {
  const [requests, proposals] = await Promise.all([api("requests"), api("proposals")]);
  const box = $("proposals"); box.replaceChildren();
  const reqs = requests.requests || [], staged = proposals.proposals || [];
  if (!reqs.length && !staged.length) {
    const text = document.createElement("p"); text.className = "muted"; text.textContent = "当前没有可见的写入申请或人工提案。"; box.append(text);
  }
  for (const item of reqs) {
    const row = document.createElement("div"), text = document.createElement("div"), small = document.createElement("small");
    row.className = "proposal"; text.textContent = `写入申请 · ${item.operation} · ${states[item.state] || item.state}`;
    small.textContent = `${item.id} · 到期 ${new Date(item.expires * 1000).toLocaleString()}`; text.append(small); row.append(text);
    metadata(row, {actor: item.actor, parameters: item.parameters, reason: item.reason, result: item.result});
    const controls = document.createElement("div"); controls.className = "actions";
    const own = item.actor === String(access?.id);
    if (item.state === "approved" && own) controls.append(button("执行已批准申请", async () => { show(await api("execute", {requestId: item.id})); await refresh(); }));
    if (item.state === "pending" && access?.approver && !own) {
      controls.append(button("批准", async () => { show(await api("decide", {requestId: item.id, action: "approve"})); await refresh(); }));
      controls.append(button("拒绝", async () => { show(await api("decide", {requestId: item.id, action: "reject"})); await refresh(); }, "danger"));
    }
    if (["pending", "approved"].includes(item.state) && own) controls.append(button("撤销申请", async () => { show(await api("decide", {requestId: item.id, action: "revoke"})); await refresh(); }, "danger"));
    if (item.operation.startsWith("ai.") && item.state === "submitted" && own) controls.append(button("核实异步结果", async () => { show(await api("reconcile", {requestId: item.id})); await refresh(); }));
    row.append(controls); box.append(row);
  }
  for (const item of staged) {
    const row = document.createElement("div"); row.className = "proposal";
    row.textContent = `人工提案 · ${item.skill} · ${states[item.state] || item.state} · ${item.id}`;
    metadata(row, {parameters: item.parameters, artifact: item.artifact, result: item.result});
    if (item.state === "pending" && access?.approver) {
      row.append(button("批准提案", async () => { show(await api("proposal/decide", {proposalId: item.id, approve: true})); await refresh(); }),
        button("拒绝提案", async () => { show(await api("proposal/decide", {proposalId: item.id, approve: false})); await refresh(); }, "danger"));
    }
    box.append(row);
  }
}
async function run(plan) {
  if (busy) return;
  busy = true; updateActions();
  try {
    const spec = selected();
    if (!access || !spec || spec.availability === "unavailable") throw new Error("请先登录并选择已配置的技能。");
    const parameters = JSON.parse($("parameters").value);
    if (plan) { show(await api("plan", {intent: spec.name, parameters})); return; }
    if (spec.effect === "external") {
      if (!$("confirmed").checked) throw new Error("请先确认真实账号写入申请的影响。");
      const key = parameters.idempotencyKey;
      if (typeof key !== "string" || !/^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/.test(key)) throw new Error("请填写有效幂等键；重试同一写入应保留原键。");
      show(await api("requests", {operation: spec.name, parameters, reason: "用户在账号控制中心明确确认的写入申请", idempotencyKey: key, expiresSeconds: 900}));
    } else show(await api("invoke", {skillId: spec.name, version: spec.version, parameters}));
    await refresh();
  } catch (error) { fail(error); } finally { busy = false; updateActions(); }
}
async function requestAi() {
  if (!$("ai-confirmed").checked) throw new Error("请先确认模型切换的影响；此操作会创建真实管理员审批申请。");
  const op = $("ai-operation").value, nodeId = $("ai-node").value.trim(), versionId = $("ai-version").value.trim(), reason = $("ai-reason").value.trim();
  if (!/^[1-9][0-9]*$/.test(nodeId) || !/^[1-9][0-9]*$/.test(versionId) || !reason) throw new Error("请填写真实模型节点、版本编号和申请原因。");
  const parameters = {nodeId, versionId, reason};
  const signature = JSON.stringify({op, parameters});
  // A timeout is not evidence that a request was not saved. Preserve the key on retry.
  if (requestAi.signature !== signature) { requestAi.signature = signature; requestAi.key = freshKey(); }
  show(await api("requests", {operation: op, parameters, reason, idempotencyKey: requestAi.key, expiresSeconds: 900}));
  await refresh();
}
function addAiPanel() {
  const panel = document.createElement("section"); panel.className = "panel";
  panel.innerHTML = '<div class="section-head"><h2>AI 模型管理</h2><span class="muted">真实管理员能力，审批不等于执行成功</span></div><div class="actions"><button id="ai-refresh" class="secondary">读取模型节点</button><input id="ai-node" inputmode="numeric" placeholder="真实模型节点 ID"><input id="ai-version" inputmode="numeric" placeholder="真实版本 ID"><select id="ai-operation"><option value="ai.model.activate">申请激活版本</option><option value="ai.model.rollback">申请回滚版本</option></select><input id="ai-reason" maxlength="512" placeholder="申请原因"><label><input type="checkbox" id="ai-confirmed"> 我确认申请真实模型变更</label><button id="ai-request">提交模型变更申请</button></div><pre id="ai-result">尚未读取模型数据。</pre>';
  document.querySelector("main").prepend(panel);
  $("ai-refresh").onclick = async () => { try { $("ai-result").textContent = JSON.stringify(await api("ai"), null, 2); } catch (error) { fail(error); } };
  $("ai-request").onclick = async () => {
    $("ai-request").disabled = true;
    try { await requestAi(); } catch (error) { fail(error); } finally { $("ai-request").disabled = false; }
  };
}
async function load() {
  updateActions();
  try {
    access = await api("access");
    $("identity").textContent = `当前账号 ${access.id} · ${access.approver ? "可审批" : "申请者"}`;
    const data = await api("catalog"); catalog = data.skills;
    $("catalog-summary").textContent = `${catalog.length} 个已注册技能 · 规则 Planner · 车队仅模拟模式`;
    const selector = $("skill"); selector.replaceChildren();
    for (const spec of catalog) { const option = document.createElement("option"); option.value = spec.name; option.textContent = `${spec.name} v${spec.version} · ${effects[spec.effect] || spec.effect}`; selector.append(option); }
    selector.onchange = select; select();
    if (access.administrator) addAiPanel();
    await refresh();
  } catch (error) { access = null; fail(error); $("status").textContent = "控制中心未就绪，请检查 Nebula 登录、专属权限和网关连接。"; updateActions(); }
}
$("plan").onclick = () => run(true);
$("invoke").onclick = () => run(false);
$("refresh").onclick = async () => { try { await refresh(); } catch (error) { fail(error); } };
load();
