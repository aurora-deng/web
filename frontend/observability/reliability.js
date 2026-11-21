import {formatValue,timestamp} from "./chart.js";

const states={evaluated:"可判定",low_volume:"流量太少",no_traffic:"没有请求",
  partial:"覆盖不足",no_data:"无可用数据",data:"连续采样"};
const reasons={no_samples:"尚未采到样本",stale:"窗口末端采样过旧",before_capture:"这段历史没有原始计数",
  scrape_failed:"指标抓取失败",missing_counter:"缺少所需计数器",gap:"采样中断",
  epoch_changed:"服务实例或采样进程发生切换",reset:"计数器回退",inconsistent_bucket:"延迟桶增量不一致",
  warming:"尚未覆盖整个窗口"};
const number=value=>formatValue(value,"");
const pct=value=>value==null?"—":formatValue(value*100,"%");
const multiplier=value=>value==null?"—":number(value)+"×";
const seconds=value=>value%3600===0&&value?value/3600+" 小时":value%60===0&&value?value/60+" 分钟":value+" 秒";

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null)node.textContent=text;
  if(cls)node.className=cls;
  return node;
}

function link(text,view,bounds,options={}) {
  const item=el("a",text);
  item.href="/?"+new URLSearchParams({view,...bounds,...options});
  return item;
}

function badge(text,tone="muted") {
  return el("span",text,"signal "+tone);
}

function values(items) {
  const line=el("dl",null,"reliability-facts");
  for(const [name,value] of items) {
    const group=el("div");
    group.append(el("dt",name),el("dd",value));
    line.append(group);
  }
  return line;
}

function rangeSummary(window) {
  return states[window.state]+" · 覆盖 "+seconds(window.coverageSeconds)
    +(window.reason?" · "+reasons[window.reason]:"");
}

function collection(data) {
  const panel=el("section",null,"panel builder-box");
  const health=el("div",null,"strip");
  for(const [key,title,success,failure] of [
    ["live","进程","存活","无法确认存活"],["ready","业务","就绪","未就绪"],
    ["scrape","抓取","成功","失败"],["persisted","样本","已保存","未持久化"],
  ]) {
    const reading=data.collecting[key];
    health.append(badge(title+" · "+(reading==null?"未知":reading?success:failure),
      reading==null?"muted":reading?"ok":"bad"));
  }
  const trace=data.telemetry, counts=trace.counts||{};
  const flow=counts.dropped>0||counts.queueFull>0?"检测到丢弃或队列满":
    counts.exported>0?"窗口内有成功导出":trace.counts?"窗口内导出增量为零":"暂无增量";
  panel.append(el("h2","采集质量"),health,
    el("p",data.lastSampleAt?"最近样本 "+timestamp(data.lastSampleAt,true):"所选时段暂无采样","muted"),
    el("h3","应用侧 Trace 导出 · "+flow),
    values([["导出 span",number(counts.exported)],["丢弃 span",number(counts.dropped)],
      ["队列满次数",number(counts.queueFull)]]),
    el("p",rangeSummary(trace)+"。成功导出表示接收端接受了批次，端到端可查询性需回读确认。","muted"));
  const sampling=data.httpTraceSampling;
  if(sampling?.counts) panel.append(el("h3","HTTP 本地采样"),
    values([["按规则跳过的请求",number(sampling.counts.skipped)],["补记失败入口",number(sampling.counts.failureRoots)]]),
    el("p",rangeSummary(sampling)+"。按规则跳过和导出丢弃分别统计。补记的失败入口只包含请求级耗时。","muted"));
  return panel;
}

function objectiveCard(objective,bounds,period,minimumEvents) {
  const {title,target,description,observed,budget,burns}=objective;
  const card=el("section",null,"panel builder-box reliability-objective");
  card.dataset.objective=objective.id;
  card.dataset.status=observed.state;
  const tone=observed.state==="evaluated"?(observed.goodFraction>=target?"ok":"bad"):"muted";
  const titleRow=el("div",null,"trace-heading");
  titleRow.append(el("h2",title),badge(states[observed.state],tone));
  card.append(titleRow,el("p",description,"muted"),
    values([["窗口内达标比例",pct(observed.goodFraction)],["总样本",number(observed.events)],
      ["未达标样本",number(observed.bad)],["示例目标",pct(target)]]),
    el("p",rangeSummary(observed)+(observed.state==="low_volume"?" · 判定至少需 "+minimumEvents+" 条":""),
      "reliability-coverage muted"));
  const budgetText=budget.budgetRemainingFraction==null?rangeSummary(budget):"剩余 "+pct(budget.budgetRemainingFraction);
  card.append(el("p",seconds(period)+"滚动错误预算 · "+budgetText,"reliability-budget"));
  const table=el("table"), header=el("thead"), tr=el("tr"), rows=el("tbody"), wrap=el("div",null,"table-wrap");
  for(const label of ["预算消耗速度","短窗口","长窗口","阈值","状态"])tr.append(el("th",label));
  header.append(tr);
  for(const burn of burns) {
    const row=el("tr");
    const cell=reading=> {
      const node=el("td",seconds(reading.to-reading.from)+" · "+multiplier(reading.burnRate));
      node.title=rangeSummary(reading)+(reading.events==null?"":" · "+reading.events+" 条");
      return node;
    };
    const label={burning:"超速",within:"未触发",insufficient:"证据不足"}[burn.state];
    row.append(el("td",burn.name==="fast"?"快速":"缓慢"),cell(burn.short),cell(burn.long),
      el("td",multiplier(burn.threshold)),el("td",label,burn.state==="burning"?"bad":"muted"));
    rows.append(row);
  }
  table.append(header,rows);wrap.append(table);card.append(wrap);
  const actions=el("div",null,"strip reliability-actions");
  const traceFilter=objective.operation?{operation:objective.operation,minMs:objective.latencyMs,sort:"slowest"}:{statusClass:"5xx"};
  actions.append(link("查看同窗指标","explore",bounds,{metric:objective.panel}),
    link(objective.operation?"查找同窗慢 Trace":"查找同窗 HTTP 5xx Trace","traces",bounds,traceFilter));
  card.append(actions);
  return card;
}

export async function renderReliability(root,{api,bounds,signal}) {
  const instance=new URLSearchParams(location.search).get("instance")||"";
  const scoped=instance?{...bounds,instance}:bounds;
  const data=await api("service-levels",scoped,signal);
  if(signal.aborted)return;
  if(data.status==="unsupported") {
    root.append(el("p",data.source==="preview"?"在 Linux 轻量实时档采集真实数据后这里会显示服务目标。":
      "服务目标当前使用轻量档的原始计数快照。此档可用 PromQL 查询对应指标。","notice"));
    return;
  }
  if(data.status==="choose_instance") {
    const picker=el("section",null,"panel builder-box");
    picker.append(el("h2","选择 NEBULA 实例"));
    const choices=el("div",null,"strip");
    for(const candidate of data.instances)choices.append(link(candidate,"reliability",bounds,{instance:candidate}));
    if(!data.instances.length)picker.append(el("p","所选时段暂无实例采集记录。","muted"));
    picker.append(choices);
    root.append(picker);
    return;
  }
  const header=el("section",null,"panel builder-box");
  const actions=el("div",null,"strip");
  actions.append(link("固定这次观测","reliability",scoped),link("主机与进程","resources",bounds));
  if(data.instances?.length>1)
    for(const candidate of data.instances.filter(name=>name!==data.instance))
      actions.append(link("切换到 "+candidate,"reliability",bounds,{instance:candidate}));
  header.append(el("h2","服务目标与采集质量"),
    el("p","NEBULA 隔离实例 · "+(data.instance||"暂无实例数据")+" · "+
      timestamp(data.from,true)+" — "+timestamp(data.to,true),"muted"),
    el("p","预算消耗速度 1× 表示按目标允许的比例消耗。短、长窗口都达到阈值才显示超速。","muted"),actions);
  root.append(header,collection(data));
  const grid=el("div",null,"reliability-grid");
  for(const objective of data.objectives)grid.append(objectiveCard(objective,bounds,data.periodSeconds,data.minimumEvents));
  root.append(grid);
  return {sampleAt:data.lastSampleAt,failed:data.collecting.scrape===false||data.collecting.persisted===false?1:0};
}
