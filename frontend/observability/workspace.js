import {renderDashboard} from "./dashboard.js";
import {renderExplore} from "./explore.js";
import {renderCustom, workspacePost} from "./custom_dashboards.js";
import {renderDistributed} from "./distributed.js";
import {renderResources} from "./resources.js";
import {renderTraceDetails} from "./trace_details.js";
import {renderCustomAlerts} from "./alert_rules.js";
import {renderReliability} from "./reliability.js";
import {renderLogs} from "./logs.js";
import {criticalPaths} from "./dag_critical_path.js";

const $ = id => document.getElementById(id);
const sideNav=document.querySelector(".sidebar nav");
const views = {overview:"运行总览", service:"业务性能", resources:"主机与进程",profiles:"函数与内核剖析",boards:"自定义仪表盘",sources:"数据源",
  explore:"指标探索",alerts:"告警规则",traces:"链路追踪",distributed:"分布式排障",runs:"研发实验",extensions:"扩展遥测",
  reliability:"服务目标与采集质量",logs:"日志工作台"};
const states = {complete:"查询成功",partial:"部分链路",missing:"未采到链路",unavailable:"追踪数据源不可用",
  success:"成功",failed:"失败",timeout:"超时",started:"已开始",retry:"重试",skipped:"跳过",queued:"等待",estop:"急停",invalid:"校验失败",blocked:"已拦截",flagged:"评测标记",cancelled:"取消",uncertain:"待对账",
  fault:"故障注入",recovered:"恢复",finished:"已结束",version:"版本变更",sent:"已下发",ack:"已确认",error:"报错"};
const names = {"dag.run":"DAG 运行","dag.node":"节点执行","dag.edge":"依赖边",
  "agent.plan":"规划","agent.skill":"技能调用","agent.memory":"记忆操作","agent.decision":"策略判断","agent.evaluation":"评测标记",
  "device.window":"设备统计窗口","device.command":"设备指令","device.safety":"设备安全","experiment.marker":"实验标记",
  "service.event":"服务探针"};
const params = new URL(location.href).searchParams;
let view = Object.hasOwn(views, params.get("view")) ? params.get("view") : "overview";
let fixed = params.has("from") && params.has("to") ? {from:Number(params.get("from")),to:Number(params.get("to"))} : null;
let traceId = params.get("trace") || "";
let paused = !!fixed, version = 0, request, visuals = [], cursor = "", eventPages = [], liveView;
let domain=["dag","agent","device","service"].includes(params.get("domain"))?params.get("domain"):"dag";
let producer=params.get("source")||"";
let panel = params.get("panel") || "", instance = params.get("instance") || "";
let bc=params.get("bc")||"", app=params.get("app")||"", scopes=[], learnedInstances=[];
let scopeRequest, lastScopeCheck=-Infinity;
let transaction = params.get("transaction") === "history" ? "history" : "http";
let refreshing = false, extensionKind = "", extensionOrigin = "";
const isDashboard = () => ["overview","service"].includes(view);
const isCommitArchive = () => view==="profiles"&&new URL(location.href).searchParams.get("section")==="commit";
let health, healthRequest, observedClock, healthFailed=false;
const observerNow=()=>Math.floor((observedClock ? observedClock.utc+performance.now()-observedClock.at : Date.now())/1000);
function observeTime(response) {
  const utc=Date.parse(response.headers.get("Date"));
  if(Number.isFinite(utc)) observedClock={utc,at:performance.now()};
}
function showEnvironment() {
  const badge=$("environment");
  const mode=health?.source;
  const archive=isCommitArchive();
  const localLabel=health?.localScope==="daily"?"本机 · 日常实时":"本机 · 隔离实时";
  badge.textContent=archive?"实验归档":health?.simulated?"模拟数据预览":mode==="local"?localLabel:
    mode==="prometheus"?"Prometheus · 实时":healthFailed?"连接失败":"连接中";
  badge.dataset.source=archive?"archive":mode||"";
  badge.title=health?.assetVersion?"资源版本 "+health.assetVersion:"";
}
async function loadProfile() {
  if(health) return health;
  healthRequest??=fetch("/health",{cache:"no-store",signal:AbortSignal.timeout(5000)}).then(async response=>{
    if(!response.ok) throw new Error("Observer 未就绪");
    observeTime(response);
    health=await response.json();
    health.source ||= health.simulated?"preview":"prometheus";
    healthFailed=false;
    showEnvironment();
    return health;
  }).catch(error=>{
    healthFailed=true;
    showEnvironment();
    throw error;
  }).finally(()=>{healthRequest=null;});
  return healthRequest;
}

function el(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

async function api(path, args, signal) {
  const query = args ? "?" + new URLSearchParams(args) : "";
  const timeout=AbortSignal.timeout(8000);
  const requestSignal=signal?AbortSignal.any([signal,timeout]):timeout;
  try {
    const response=await fetch("/api/v1/"+path+query,{signal:requestSignal,cache:"no-store"});
    observeTime(response);
    const data=await response.json();
    if(!response.ok&&!(path.startsWith("traces/")&&response.status===404))
      throw new Error(data.error||"查询失败");
    return data;
  } catch(error) {
    if(timeout.aborted&&!signal?.aborted) throw new Error("查询超时，请缩小时间范围后重试。");
    throw error;
  }
}

function windowParams() {
  const now = observerNow();
  return fixed || {from:now - (Number($("range").value) || 900), to:now};
}

function remember() {
  const url = new URL(location.href);
  const metric=url.searchParams.get("metric");
  const board=url.searchParams.get("board");
  const incident=url.searchParams.get("incident");
  const span=url.searchParams.get("span");
  const traceSource=url.searchParams.get("traceSource");
  const profile=url.searchParams.get("profile");
  const section=url.searchParams.get("section");
  const sample=url.searchParams.get("sample");
  const traceFilters=["operation","failed","q","minMs","sort","statusClass"].map(key=>[key,url.searchParams.get(key)]);
  const fn=url.searchParams.get("fn"), call=url.searchParams.get("call");
  const logTrace=url.searchParams.get("logTrace"),logLevel=url.searchParams.get("level");
  url.search = "";
  url.searchParams.set("view",view);
  if (fixed) for (const [k,v] of Object.entries(fixed)) url.searchParams.set(k,v);
  if (panel && isDashboard()) url.searchParams.set("panel",panel);
  for(const [name,value] of Object.entries({bc,app,instance})) if(value && isDashboard())url.searchParams.set(name,value);
  if (transaction==="history" && isDashboard()) url.searchParams.set("transaction",transaction);
  if (view === "traces" && traceId) url.searchParams.set("trace",traceId);
  if (view === "traces") for(const [key,value] of traceFilters) if(value)url.searchParams.set(key,value);
  if (view === "explore" && metric) url.searchParams.set("metric",metric);
  if (view === "logs") {
    if(logTrace)url.searchParams.set("logTrace",logTrace);
    if(logLevel)url.searchParams.set("level",logLevel);
  }
  if (view === "boards" && board) url.searchParams.set("board",board);
  if (view === "distributed" && incident) url.searchParams.set("incident",incident);
  if (view === "distributed" && traceSource) url.searchParams.set("traceSource",traceSource);
  if ((view==="traces"||view==="distributed"||(view==="profiles"&&call)) && span)url.searchParams.set("span",span);
  if (view==="extensions") {
    url.searchParams.set("domain",domain);
    if(producer)url.searchParams.set("source",producer);
  }
  if (view === "profiles") {
    if(section==="commit") {
      url.searchParams.set("section",section);
      if(profile&&/^(?:[1-9]|1[0-9]|20)$/.test(sample)) url.searchParams.set("sample",sample);
    }
    if(profile!==null) url.searchParams.set("profile",profile);
  }
  if (view === "profiles" && fn) {
    url.searchParams.set("fn",fn);
    if(call)url.searchParams.set("call",call);
  }
  history.replaceState(null,"",url);
}

function revealActiveNavigation() {
  const active=sideNav.querySelector("button[aria-current]");
  if(!active||sideNav.scrollWidth<=sideNav.clientWidth) return;
  const item=active.getBoundingClientRect(), frame=sideNav.getBoundingClientRect();
  sideNav.scrollLeft+=item.right>frame.right?item.right-frame.right:Math.min(0,item.left-frame.left);
}

function at(value) {
  return new Date(value).toLocaleString("zh-CN",{hour12:false});
}

function alertPanel(alerts, includeInactive=false) {
  const labels = {NebulaUnreachable:"NEBULA 服务不可达",NebulaUnready:"业务暂未就绪",NebulaScrapeFailed:"指标抓取失败",NebulaQueueBacklog:"数据库任务持续排队",NebulaHistoryWriteFailed:"历史指标写入失败"};
  const box = el("div",undefined,"list");
  const visible=includeInactive?alerts:alerts.filter(item=>["firing","pending"].includes(item.state));
  if (!visible.length) box.append(el("p","当前没有触发的规则","empty"));
  for (const alert of visible) {
    const row = el("div",undefined,"row");
    row.append(el("span",alert.annotations?.title || labels[alert.labels.alertname] || alert.labels.alertname));
    const state={firing:"已触发",pending:"等待持续条件",normal:"正常",no_data:"窗口暂无数据"};
    row.append(el("strong",state[alert.state]||alert.state,alert.state==="firing"?"bad":"muted"));
    if(alert.annotations?.window) row.append(el("small","判定窗口 "+alert.annotations.window));
    if(alert.value!=null)row.append(el("small","当前 "+Number(alert.value).toLocaleString("zh-CN",{maximumFractionDigits:2})+
      " "+(alert.annotations?.unit||"")));
    if(alert.annotations?.pending)row.append(el("small","持续 "+alert.annotations.pending+" 秒"));
    const button = el("button","查看相关时段");
    button.addEventListener("click",() => {
      if(alert.annotations?.custom) { go("alerts"); return; }
      const end = observerNow(), start = Math.floor(Date.parse(alert.activeAt)/1000);
      fixed = {from:Math.max(end-7*86400,(Number.isFinite(start) ? start : end)-300),to:end};
      paused = true;
      const target={NebulaHttpHighErrors:"http_5xx_pct",NebulaDatabaseMaxSlow:"db_lifetime_max"}[alert.labels.alertname];
      if(target) {
        const url=new URL(location.href);
        url.searchParams.set("metric",target);history.replaceState(null,"",url);go("explore");
      } else go("service");
    });
    row.append(button);
    if(alert.labels.alertname==="NebulaHttpHighErrors") {
      const impact=el("button","查看请求影响");
      impact.onclick=()=>{
        const end=observerNow(), started=Math.floor(Date.parse(alert.activeAt)/1000);
        fixed={from:Math.max(0,end-604800,(Number.isFinite(started)?Math.min(started,end):end)-300),to:end};
        paused=true;syncRange();go("reliability");
      };
      row.append(impact);
    }
    box.append(row,el("p",alert.annotations?.guide || "对照同一时间段的健康探针、资源和数据库曲线。","muted"));
    if(alert.annotations?.expression) {
      const query=el("details");
      query.append(el("summary","触发规则"),el("code",alert.annotations.expression));
      box.append(query);
    }
  }
  return box;
}

async function renderAlerts(root,signal) {
  const alerts=await api("alerts",null,signal);
  if(signal.aborted)return;
  await renderCustomAlerts(root,{api,post:workspacePost,signal,refresh:()=>run()});
  if(signal.aborted)return;
  root.append(el("h2","内置告警规则"),alertPanel(alerts.filter(item=>!item.annotations?.custom),true));
}

function detail(event) {
  const labels = {runId:"运行",state:"状态",workflow:"工作流",predecessor:"前置",successor:"后继",
    nodeId:"节点",attempt:"尝试次数",durationMs:"耗时（ms）",inputCount:"输入项",outputCount:"输出项",
    agentRunId:"Agent 运行",callId:"调用",skill:"技能",decision:"决策",reasonCode:"原因码",operation:"操作",
    evaluator:"评测来源",inputTokens:"输入 Token",outputTokens:"输出 Token",deviceId:"设备",sessionId:"会话",
    stream:"数据流",samples:"样本",gaps:"缺帧",duplicates:"重复",late:"迟到",clockQuality:"时钟质量",
    offsetMs:"时钟偏差（ms）",commandId:"指令"};
  return Object.entries(event.fields).map(([k,v]) => (labels[k] || k) + " " + (states[v] || v)).join("　");
}

function eventIntervals(events) {
  const groups=new Map(), terminal=new Set(["success","failed","timeout","skipped","invalid","cancelled","uncertain"]);
  for(const event of events){
    if(!event.spanId || !["dag.node","agent.skill"].includes(event.eventType)) continue;
    if(!groups.has(event.spanId)) groups.set(event.spanId,{});
    const block=groups.get(event.spanId);
    if (event.fields.state==="started") block.first=event;
    if (terminal.has(event.fields.state)) block.last=event;
  }
  return [...groups.values()].flatMap(({first,last})=>{
    if(!last || (!first && last.fields.state!=="skipped")) return [];
    const f=last.fields;
    return [{id:last.spanId,name:f.nodeId || f.skill+" · 第 "+f.attempt+" 次",service:last.source,
      start:first?.occurredAt || last.occurredAt,end:last.occurredAt,links:[],
      status:f.state==="success" || f.state==="skipped" ? 0 : 2}];
  });
}

function timeline(spans, events) {
  const panel = el("div",undefined,"timeline");
  if (!spans.length) {
    panel.append(el("div","暂无具备起止时间的执行记录","empty")); return panel;
  }
  const spansSorted = spans.slice().sort((a,b) => Number(BigInt(a.start)-BigInt(b.start)));
  const start = BigInt(spansSorted[0].start), end = spansSorted.reduce((n,s) => BigInt(s.end) > n ? BigInt(s.end) : n,start);
  const total = Math.max(1,Number(end-start));
  const terminal = new Set(["success","failed","timeout","skipped","cancelled","uncertain"]);
  const paths = criticalPaths(events);
  const critical = new Map(paths.filter(path => !path.partial).map(path => [path.runId, new Set(path.nodes)]));
  const nodeMap = new Map();
  for (const event of events.filter(e=>e.eventType==="dag.node" && e.spanId)) {
    const known=nodeMap.get(event.spanId);
    if (!known || !terminal.has(known.state) || terminal.has(event.fields.state)) nodeMap.set(event.spanId,event.fields);
  }
  const options = el("div",null,"strip timeline-filters");
  const reset=el("button","显示全部节点");
  const ruler=el("div",null,"span-row span-ruler");
  const ticks=el("div",null,"ruler-ticks");
  for(const part of [0,.25,.5,.75,1]) ticks.append(el("span",(total*part/1e6).toFixed(0)+" ms"));
  ruler.append(el("span","跨度 · 相对开始"),ticks,el("span","耗时"));
  options.append(reset);
  panel.append(options);
  for (const path of paths) {
    const text = path.nodes.length ? path.nodes.join(" → ") + " · " + path.ms.toFixed(2) + " ms" : "缺少节点时间";
    panel.append(el("p", (path.partial ? "部分记录 · " : "关键依赖路径 · ") + text, "dag-critical-summary"));
  }
  panel.append(ruler);
  panel.addEventListener("node-filter",event=>{
    const target=event.detail;
    let first;
    for(const row of panel.querySelectorAll(".span-row[data-node]")) {
      row.hidden=Boolean(target && row.dataset.node!==target);
      if(!row.hidden) first ??= row.querySelector(".span-bar");
    }
    if(target && first) first.click();
    if(target && !first) more.replaceChildren(el("p","这个节点尚未收到 span。请查看下方节点事件。","muted"));
  });
  reset.onclick=()=>panel.dispatchEvent(new CustomEvent("node-filter",{detail:""}));
  const more = el("div","选择一个时间条查看详情","trace-info");
  for (const item of spansSorted) {
    const row = el("div",undefined,"span-row"), track=el("div",undefined,"span-track");
    const info = nodeMap.get(item.id), title = info ? info.nodeId + " · 第 " + info.attempt + " 次" +
      (info.workerId ? " · " + info.workerId : "") : item.name;
    const length = Math.max(0,Number(BigInt(item.end)-BigInt(item.start)));
    const bar = el("button",undefined,"span-bar" + ([2,"STATUS_CODE_ERROR"].includes(item.status) ? " bad" : ""));
    row.dataset.node=info?.nodeId || "";
    if (info?.state==="skipped") bar.classList.add("skipped");
    if (info && critical.get(info.runId)?.has(info.nodeId)) {
      bar.classList.add("critical");
    }
    bar.style.left = Math.max(0,Number(BigInt(item.start)-start)/total*100) + "%";
    bar.style.width = Math.max(.4,length/total*100) + "%";
    bar.title = title;
    bar.setAttribute("aria-label",title+(info?.state?" · "+(states[info.state]||info.state):""));
    bar.dataset.node = info?.nodeId || "";
    bar.onclick = () => {
      panel.querySelectorAll(".span-bar.active").forEach(x=>x.classList.remove("active"));
      bar.classList.add("active");
      more.replaceChildren(el("strong",title),el("p",item.service+" · "+item.id,"muted"),
        el("p","前置 span "+(item.links.join(", ") || "—"),"muted"));
      for (const e of events.filter(e=>e.spanId===item.id)) more.append(el("p",detail(e)));
    };
    track.append(bar);
    row.append(el("div",title,"span-label"),track,el("span",(length/1e6).toFixed(2)+" ms"));
    panel.append(row);
  }
  panel.append(more);
  return panel;
}

function graph(events, waterfall) {
  const edges = events.filter(e=>e.eventType==="dag.edge").map(e=>[e.fields.predecessor,e.fields.successor]);
  const work = events.filter(e=>e.eventType==="dag.node");
  const nodes = [...new Set([...edges.flat(),...work.map(e=>e.fields.nodeId)])];
  if (!nodes.length) return null;
  const wrap=el("div",null,"topology");
  const create=(tag,attrs={},value)=>{
    const node=document.createElementNS("http://www.w3.org/2000/svg",tag);
    for(const [key,v] of Object.entries(attrs)) node.setAttribute(key,v);
    if(value!==undefined) node.textContent=value;
    return node;
  };
  const svg=create("svg",{"aria-label":"DAG 节点依赖图"});
  const terminal=new Set(["success","failed","timeout","skipped","cancelled","uncertain"]);
  const latest=new Map(), attempts=new Map();
  for(const {fields:item} of work){
    if(!attempts.has(item.nodeId)) attempts.set(item.nodeId,new Set());
    attempts.get(item.nodeId).add(item.attempt);
    const known=latest.get(item.nodeId);
    if(!known || item.attempt>known.attempt ||
       (item.attempt===known.attempt && (!terminal.has(known.state)||terminal.has(item.state)))) latest.set(item.nodeId,item);
  }
  const depth=new Map(nodes.map(n=>[n,0]));
  for(let round=0;round<nodes.length-1;round++)
    for(const [a,b] of edges) depth.set(b,Math.min(nodes.length-1,Math.max(depth.get(b),depth.get(a)+1)));
  const tiers=new Map(), points=new Map();
  for(const n of nodes){
    const d=depth.get(n), r=tiers.get(d)||0;
    points.set(n,[87+177*d,42+69*r]); tiers.set(d,r+1);
  }
  svg.setAttribute("width",Math.max(340,...[...points.values()].map(p=>p[0]+88)));
  svg.setAttribute("height",Math.max(...tiers.values())*69+14);
  const defs=create("defs"), arrow=create("marker",{id:"arrow",viewBox:"0 0 10 10",refX:9,refY:5,
    markerWidth:6,markerHeight:6,orient:"auto-start-reverse"});
  arrow.append(create("path",{d:"M 0 0 L 10 5 L 0 10 z",fill:"#838ca1"})); defs.append(arrow);svg.append(defs);
  for(const [a,b] of edges){
    const [x,y]=points.get(a), [u,v]=points.get(b);
    svg.append(create("path",{d:"M"+(x+63)+","+y+" C"+(x+89)+","+y+" "+(u-89)+","+v+" "+(u-64)+","+v,
      fill:"none",stroke:"#788299","stroke-width":1.5,"marker-end":"url(#arrow)"}));
  }
  for(const n of nodes){
    const [x,y]=points.get(n), item=latest.get(n), stateLabel=states[item?.state] || item?.state || "无事件";
    const tag=create("g",{role:"button",tabindex:0,"aria-label":n+" · "+stateLabel,"data-dag-node":n,
      "data-node-state":item?.state || "missing"});
    tag.append(create("rect",{x:x-63,y:y-23,width:126,height:46,rx:5,fill:"#23252b",stroke:"#495164"}),
      create("text",{x:x-52,y:y-4,fill:"#e9eaf0","font-size":13},n.length>13?n.slice(0,12)+"…":n),
      create("text",{x:x-52,y:y+13,fill:"#b4bacb","font-size":10},stateLabel+" · "+(attempts.get(n)?.size||0)+" 次尝试"));
    const select=()=>waterfall.dispatchEvent(new CustomEvent("node-filter",{detail:n}));
    tag.addEventListener("click",select);
    tag.addEventListener("keydown",e=>{if(e.key==="Enter"||e.key===" "){e.preventDefault();select();}});
    svg.append(tag);
  }
  wrap.append(svg);
  return wrap;
}

function eventList(items) {
  const list = el("div",undefined,"list");
  if (!items.length) list.append(el("p","这个时间段暂无事件","empty"));
  for (const event of items) {
    const row = el("div",undefined,"row");
    row.append(el("span",at(Number(BigInt(event.occurredAt)/1000000n))+" · "+(names[event.eventType]||event.eventType)),
      el("small", event.source, "muted"),
      el("strong",event.simulated?"模拟":"真实",event.simulated?"muted":"ok"));
    if (event.traceId && view !== "traces") {
      const button = el("button","打开链路");
      button.onclick = () => {traceId=event.traceId;go("traces");};
      row.append(button);
    }
    list.append(row,el("p",detail(event),"muted"));
  }
  return list;
}

async function renderTraces(root,bounds,signal) {
  const form = el("form",undefined,"strip"), input = el("input");
  input.id="traceId"; input.required=true; input.pattern="[a-f0-9]{32}"; input.value=traceId;
  input.setAttribute("aria-label","Trace ID");
  form.append(el("label","Trace ID"),input,el("button","查询"));
  form.onsubmit = event => {event.preventDefault(); traceId=input.value.trim(); run();};
  root.append(form);
  if (!/^[a-f0-9]{32}$/.test(traceId)) {
    await traceSearch(root,bounds,signal);
    return;
  }
  const back=el("button","返回链路列表");back.onclick=()=>{traceId="";run();};
  root.append(back);
  const data = await api("traces/"+traceId,null,signal);
  if (signal.aborted) return;
  const investigate=el("a","分布式排障与日志");
  const deepLink=new URLSearchParams({view:"distributed",incident:traceId,...bounds});
  investigate.href="/?"+deepLink;
  root.append(investigate);
  const inferred = data.spans.length===0 ? eventIntervals(data.items) : [];
  root.append(el("h2",states[data.status] || data.status));
  if(data.spans.length)renderTraceDetails(root,data.spans,{traceId,partial:data.status==="partial",
    traceSource:data.source==="local"?"nebula":""});
  if(inferred.length||data.items.some(item=>item.eventType.startsWith("dag."))) {
    const waterfall=timeline(inferred.length?inferred:data.spans,data.items);
    const topology=graph(data.items,waterfall);
    if(topology)root.append(el("h2","节点依赖"),topology);
    root.append(el("h2",inferred.length?"上报事件计时":"执行甘特图"),waterfall);
  }
  if (inferred.length) root.append(el("p","时间条使用已收到的开始和结束事件。跨进程父子 span 需接入追踪源。","muted"));
  if (data.items.some(e=>e.eventType.startsWith("agent."))) {
    const benefits = data.agent;
    const box=el("div",null,"agent-grid");
    const cards=[
      ["工具尝试",benefits.attempts],
      ["工具最终成功",Object.values(benefits.skills).reduce((total,skill)=>total+skill.succeeded,0)],
      ["输入 Token",benefits.tokens.inputTokens ?? "未知"],
      ["输出 Token",benefits.tokens.outputTokens ?? "未知"],
      ["权限 / 拒绝拦截",data.items.filter(e=>e.eventType==="agent.decision"&&e.fields.state==="blocked").length],
    ];
    for(const [label,value] of cards){
      const card=el("div",null,"stat-card");
      card.append(el("span",label),el("strong",value));
      box.append(card);
    }
    root.append(el("h2","Agent 执行统计"),box);
    for(const [skill,s] of Object.entries(benefits.skills))
      root.append(el("p",skill+" · 最终成功 "+s.succeeded+" / "+s.finished,"muted"));
    const completeness=benefits.unknownTokenFields ? " · "+benefits.unknownTokenFields+" 个 Token 字段未知" : "";
    root.append(el("p",(benefits.cost ? "估算费用 "+benefits.cost.estimate+" "+benefits.cost.currency :
      "估算费用未知 · 需要完整 Token 记录与配置费率")+completeness,"muted"));
  }
  root.append(el("h2","关联事件"),eventList(data.items));
}

async function traceSearch(root,bounds,signal) {
  const filters=new URL(location.href).searchParams;
  const form=el("form",null,"strip"), operation=el("select"), only=el("input"), failed=el("label","仅错误");
  operation.setAttribute("aria-label","链路操作");
  for(const [value,label] of [["","所有操作"],["GET /api/conversations","会话列表服务端"],
    ["probe GET /api/conversations","会话列表探针"],["GET /api/conversations/:id/messages","HTTP 历史读取"],
    ["POST /api/conversations/:id/messages","HTTP 写入消息"],["WS chat.send","WebSocket 写入消息"]])
    operation.append(new Option(label,value));
  const catalog=await api("function-catalog",null,signal);
  if(signal.aborted)return;
  const registered=new Set([...operation.options].map(option=>option.value));
  for(const name of catalog.httpOperations)if(!registered.has(name))operation.append(new Option(name,name));
  if([...operation.options].some(option=>option.value===filters.get("operation")))operation.value=filters.get("operation");
  only.type="checkbox"; failed.prepend(only);
  only.checked=filters.get("failed")==="1";
  const local=(await loadProfile()).source==="local";
  const keyword=el("input"), minimum=el("input"), sort=el("select"), httpStatus=el("select");
  httpStatus.setAttribute("aria-label","响应状态");
  httpStatus.append(new Option("全部 HTTP 状态",""),new Option("HTTP 4xx","4xx"),new Option("HTTP 5xx","5xx"));
  httpStatus.value=["4xx","5xx"].includes(filters.get("statusClass"))?filters.get("statusClass"):"";
  keyword.placeholder="服务名或函数名";keyword.maxLength=80;keyword.setAttribute("aria-label","链路关键字");
  minimum.type="number";minimum.min="0";minimum.step="any";minimum.placeholder="至少 ms";
  minimum.setAttribute("aria-label","链路最短毫秒数");
  sort.append(new Option("时间最新","recent"),new Option("最慢优先","slowest"));
  keyword.value=(filters.get("q")||"").slice(0,80);
  if(Number(filters.get("minMs"))>0)minimum.value=filters.get("minMs");
  if(filters.get("sort")==="slowest")sort.value="slowest";
  sort.setAttribute("aria-label","链路排序");
  form.append(operation,failed,...(local?[httpStatus,keyword,minimum,sort]:[]),el("button","筛选链路"));
  const result=el("section",null,"list");
  root.append(el("h2","所选时段的链路"), form, result);
  let requestId=0;
  async function refresh() {
    const current=++requestId;
    const options={operation:operation.value,failed:only.checked?"1":"",
      ...(local?{q:keyword.value.trim(),minMs:minimum.value,sort:sort.value==="slowest"?"slowest":"",
        statusClass:httpStatus.value}:{})};
    const url=new URL(location.href);
    for(const [key,value] of Object.entries(options)) {
      if(value)url.searchParams.set(key,value);else url.searchParams.delete(key);
    }
    history.replaceState(null,"",url);
    result.replaceChildren(el("p","正在查询…","muted"));
    try {
      const data=await api("traces",{...bounds,...options,limit:40},signal);
      if(current!==requestId || signal.aborted) return;
      const rows=data.items.map(item=>{
        const row=el("div",null,"row");
        row.append(el("span",item.name),el("small",item.service+" · "+at(Number(BigInt(item.startedAt)/1000000n))),
          el("strong",item.durationMs.toFixed(2)+" ms",item.error?"bad":"muted"));
        if(item.httpStatus!=null)row.append(el("small","HTTP "+item.httpStatus,item.httpStatus>=400?"bad":"muted"));
        const open=el("button","打开链路");
        open.onclick=()=>{
          const url=new URL(location.href);
          if(item.spanId)url.searchParams.set("span",item.spanId);
          else url.searchParams.delete("span");
          history.replaceState(null,"",url);
          traceId=item.traceId;run();
        };
        row.append(open);
        return row;
      });
      result.replaceChildren(...(rows.length?rows:[el("p",data.source==="unconfigured"?
        "此数据源没有链路索引。可以从扩展遥测打开一组事件。":"当前时段没有符合筛选条件的链路。","empty")]));
    } catch(error) {if(!signal.aborted && current===requestId) result.replaceChildren(el("p",error.message,"notice"));}
  }
  form.onsubmit=event=>{event.preventDefault();refresh();};
  await refresh();
}

async function renderRuns(root,bounds,signal) {
  const records = await api("runs",bounds,signal), list = el("div",undefined,"list");
  root.append(el("h2","实验记录"),list);
  const profileLink=el("a","查看绑核、Syscall 和函数分析报告");
  profileLink.href="/?view=profiles";root.append(profileLink);
  if (!records.length) {
    const latest=el("button","查看近 7 天");
    latest.onclick=()=>{$("range").value="604800";$("range").dispatchEvent(new Event("change"));};
    list.append(el("p","此时段没有实验记录。可以查看近 7 天或导入已执行的实验报告。","notice"),latest);
  }
  const selected = new Set();
  for (const r of records) {
    const row = el("div",undefined,"row"), check = el("input"), label=el("label");
    check.type="checkbox";
    check.onchange = () => { if (check.checked && selected.size < 2) selected.add(r.id); else {selected.delete(r.id);check.checked=false;} };
    label.append(check,document.createTextNode(" "+r.scenario+" · "+at(Date.parse(r.startedAt))));
    row.append(label,
      el("span","ACK P99 "+(r.result.ackP99Ms ?? "—")+" ms · "+(r.result.ackSamples ?? "—")+" 样本"));
    const detail=el("details");
    detail.append(el("summary","报告详情"));
    detail.addEventListener("toggle",async()=>{
      if (!detail.open || detail.children.length>1) return;
      try {
        const data=await api("runs/"+r.id,null,signal);
        detail.append(el("pre",JSON.stringify(data,null,2)));
      } catch(error){if(!signal.aborted) detail.append(el("p",error.message));}
    });
    row.append(detail);
    list.append(row);
  }
  const compare = el("button","按勾选顺序比较两轮"), result = el("p","","notice");
  compare.onclick = async () => {
    if (selected.size!==2) {result.textContent="请选择基线和候选各一条。";return;}
    const [baseline,candidate] = selected;
    try {
      const data=await api("runs/compare",{baseline,candidate},signal);
      result.textContent=data.status==="comparable" ? "ACK P99 变化 "+data.ackP99DeltaPercent+"%" : "证据不可比 · "+data.reasons.join("、");
    } catch(error){result.textContent=error.message;}
  };
  root.append(compare,result);
}

async function renderExtensions(root,bounds,signal,append) {
  const strip = el("div",undefined,"strip");
  for (const [key,title] of [["dag","DAG 调度"],["agent","Agent"],["device","设备遥测"],["service","服务事件"]]) {
    const button = el("button",title); button.setAttribute("aria-pressed",String(key===domain));
    button.onclick=()=>{domain=key;producer="";extensionKind="";cursor="";eventPages=[];run();}; strip.append(button);
  }
  const data = await api("events",{...bounds,domain,limit:100,...(producer?{source:producer}:{}),...(append&&cursor?{after:cursor}:{})},signal);
  if (signal.aborted) return;
  eventPages = append ? [...eventPages,...data.items] : data.items;
  cursor = data.next ? data.next.join(",") : "";
  const origin=el("select"), kind=el("select"), visible=el("div"), count=el("small");
  origin.setAttribute("aria-label","数据来源");
  origin.append(new Option("全部来源",""),new Option("真实上报","real"),new Option("模拟上报","simulated"));
  kind.setAttribute("aria-label","事件类型");
  kind.append(new Option("全部事件",""));
  for(const key of [...new Set(eventPages.map(event=>event.eventType))]) kind.append(new Option(names[key]||key,key));
  origin.value=extensionOrigin;
  kind.value=extensionKind;
  const applyFilter=()=>{
    extensionOrigin=origin.value;
    extensionKind=kind.value;
    const items=eventPages.filter(e=>(!kind.value||e.eventType===kind.value) &&
      (!origin.value||e.simulated===(origin.value==="simulated")));
    visible.replaceChildren(eventList(items));
    count.textContent="已加载 "+eventPages.length+" 条 · 显示 "+items.length+" 条";
  };
  origin.onchange=kind.onchange=applyFilter;
  const connection=el("form",null,"strip"), sourceInput=el("input");
  sourceInput.value=producer;sourceInput.maxLength=80;sourceInput.pattern="[A-Za-z0-9_.\\-]*";
  sourceInput.placeholder="来源名，例如 dag-sim";sourceInput.setAttribute("aria-label","扩展来源");
  connection.append(sourceInput,el("button","核验该来源"));
  connection.onsubmit=event=>{event.preventDefault();producer=sourceInput.value.trim();cursor="";eventPages=[];run();};
  if(producer)connection.append(el("small",data.items.length?"已收到该来源的持久化事件":"当前时段尚无该来源事件","muted"));
  root.append(connection);
  strip.append(origin,kind,count);
  applyFilter();
  root.append(strip,visible);
  if (domain === "device" && data.commands?.length) {
    const box=el("div",undefined,"list");
    for(const c of data.commands) box.append(el("p",c.device+" / "+c.session+" · "+c.id+
      " · "+c.states.map(state=>states[state]||state).join(" → ")+(c.ackMs===undefined?"":" · ACK "+c.ackMs+" ms"),"row"));
    root.append(el("h2","控制指令与执行回执"),box);
  }
  if (cursor && eventPages.length < 500) {
    const next=el("button","加载下一页"); next.onclick=()=>run(true);root.append(next);
  }
}

function fmtInput(seconds) {
  const date=new Date(seconds*1000);
  return new Date(date.getTime()-date.getTimezoneOffset()*60000).toISOString().slice(0,16);
}

function syncRange() {
  $("custom").hidden=!fixed;
  if(!fixed) return;
  $("range").value="custom";
  $("from").value=fmtInput(fixed.from);
  $("to").value=fmtInput(fixed.to);
}

function zoom(bounds) {
  fixed=bounds; paused=true; syncRange(); run();
}

function focus(name) {
  panel=name; run();
}

function setInstances(instances) {
  learnedInstances=instances;
  const set=(id,values,chosen,placeholder)=>{
    const field=$(id), names=[...new Set(values.filter(Boolean))];
    if(chosen&&!names.includes(chosen))names.push(chosen);
    if(field.options.length!==names.length+1||names.some((name,index)=>field.options[index+1].value!==name))
      field.replaceChildren(new Option(placeholder,""),...names.map(value=>new Option(value,value)));
    if(field.value!==chosen)field.value=chosen;
  };
  set("bc",scopes.map(target=>target.bc),bc,"全部 BC");
  const apps=scopes.filter(target=>!bc||target.bc===bc);
  set("app",apps.map(target=>target.app),app,"全部 APP");
  const matches=apps.filter(target=>!app||target.app===app).map(target=>target.instance);
  set("instance",scopes.length?[...matches,...(!bc&&!app?instances:[])]:instances,instance,"全部实例");
  $("app").disabled=!apps.length;
}

function loadScopes() {
  if(scopeRequest)return scopeRequest;
  if(performance.now()-lastScopeCheck<30000)return Promise.resolve();
  scopeRequest=api("scopes").then(catalog=>{
    if(!Array.isArray(catalog?.items))return;
    lastScopeCheck=performance.now();
    scopes=catalog.items;
    if(isDashboard())setInstances(learnedInstances);
  }).catch(()=>{}).finally(()=>{scopeRequest=null;});
  return scopeRequest;
}

async function run(append=false,refreshData=false) {
  const current=++version;
  request?.abort();
  request=new AbortController();
  const signal=request.signal;
  remember();
  const commitReport=isCommitArchive();
  $("title").textContent=commitReport?"提交诊断":views[view];
  showEnvironment();
  $("range").parentElement.hidden=commitReport;
  $("interval").hidden=commitReport;
  $("pause").hidden=commitReport;
  if(commitReport) $("custom").hidden=true;
  const activeButton=sideNav.querySelector('button[data-view="'+view+'"]');
  if(!activeButton.hasAttribute("aria-current")) {
    sideNav.querySelector("button[aria-current]")?.removeAttribute("aria-current");
    activeButton.setAttribute("aria-current","page");
    revealActiveNavigation();
  }
  $("scope").hidden=!isDashboard();
  $("pause").setAttribute("aria-pressed",String(paused || !!fixed || Number($("interval").value)===0));
  $("pause").textContent=paused || fixed || Number($("interval").value)===0 ? "恢复实时" : "暂停";
  const draft=el("div"), nextVisuals=[];
  let applied=false;
  const status=$("status");
  if(!$("body").hasChildNodes()) {
    status.textContent="正在查询…";
    status.dataset.state="loading";
  }
  refreshing=true;
  try {
    if(!health) await loadProfile().catch(()=>null);
    if(signal.aborted) return;
    const bounds=windowParams();
    let result;
    if (isDashboard()) {
      const scopeRead=loadScopes();
      const profile=await loadProfile();
      if(bc||app)await scopeRead;
      if(signal.aborted)return;
      const available=scopes.filter(target=>(!bc||target.bc===bc)&&(!app||target.app===app)).map(target=>target.instance);
      const allowed=(bc||app)?new Set(available):null;
      result=await renderDashboard(draft,{api, view, signal, transaction, bounds, fixed, reading:profile,now:observerNow,previous:liveView,
        alertPanel, focus, zoom, panel, instance, allowed, setInstances, visuals:nextVisuals});
    } else if(view==="resources" || view==="profiles") result=await renderResources(draft,{api,post:workspacePost,bounds,
      signal,visuals:nextVisuals,zoom,view,readHealth:loadProfile});
    else if (view==="reliability") result=await renderReliability(draft,{api,bounds,signal});
    else if (view==="boards" || view==="sources") result=await renderCustom(draft,{api,bounds,signal,visuals:nextVisuals,zoom,
      refresh:()=>run(),view,previous:liveView,refreshData});
    else if (view==="distributed") await renderDistributed(draft,{api,bounds,signal,post:workspacePost});
    else if (view==="explore") await renderExplore(draft,{api,bounds,signal,visuals:nextVisuals,zoom,post:workspacePost});
    else if (view==="logs") await renderLogs(draft,{api,post:workspacePost,bounds,signal,visuals:nextVisuals,now:observerNow});
    else if (view==="alerts") await renderAlerts(draft,signal);
    else if (view==="traces") await renderTraces(draft,bounds,signal);
    else if (view==="runs") await renderRuns(draft,bounds,signal);
    else await renderExtensions(draft,bounds,signal,append);
    if (current!==version) return;
    if(!result?.reused) {
      visuals.splice(0).forEach(visual=>visual.dispose());
      visuals=nextVisuals;
      $("body").replaceChildren(...draft.childNodes);
    }
    liveView=result?.live;
    if(view==="distributed"&&location.hash==="#trace-logs")$("trace-logs")?.scrollIntoView();
    // SVGs are painted in the same frame as the DOM swap to avoid an empty chart frame.
    nextVisuals.forEach(visual=>visual.paint?.());
    applied=true;
    if(result?.load)await result.load();
    if(current!==version)return;
    const update=new Date().toLocaleTimeString("zh-CN",{hour12:false});
    const state=fixed ? "固定窗口" : paused || Number($("interval").value)===0 ? "已暂停" : "自动刷新";
    const stale=isDashboard() && !fixed && result?.sampleAt && bounds.to-result.sampleAt>Math.max(45,3*Number($("interval").value));
    status.dataset.state=result?.failed || stale ? "warn" : "ready";
    status.textContent=result?.sampleAt!==undefined ? state+" · "+(result.sampleAt ? "最近样本 "+new Date(result.sampleAt*1000).toLocaleTimeString("zh-CN",{hour12:false}) : "无指标样本")+
      " · "+(result.failed ? result.failed+" 项查询失败" : stale ? "采样延迟" : "查询于 "+update) : "查询于 "+update;
  } catch(error) {
    if (current!==version) return;
    status.dataset.state="warn";
    status.textContent="查询失败 · "+error.message;
  } finally {
    if(current===version) refreshing=false;
    if(!applied) nextVisuals.forEach(visual=>visual.dispose());
  }
}

function go(next) {
  view=next; panel=""; cursor=""; eventPages=[];
  liveView=null;
  visuals.splice(0).forEach(visual=>visual.dispose());
  $("body").replaceChildren();
  run();
}

let intervalHandle;
function schedule() {
  clearInterval(intervalHandle);
  const every=Number($("interval").value);
  if(!every) return;
  intervalHandle=setInterval(()=>{
    if(!paused&&!fixed&&!refreshing&&!document.hidden&&(isDashboard() || view==="resources" || view==="alerts" || view==="reliability" ||
       view==="boards"&&new URL(location.href).searchParams.has("board"))&&
       !document.querySelector("dialog[open]")&&!visuals.some(visual=>visual.interacting?.())&&
       !$("body").querySelector(isDashboard()||view==="boards"?"input:focus-visible, select:focus-visible, textarea:focus-visible, details[open]:hover":
         ".chart:hover, :focus-visible, details[open]:hover")) run(false,true);
  },every*1000);
}
sideNav.querySelectorAll("button").forEach(button=>button.onclick=()=>go(button.dataset.view));
new ResizeObserver(revealActiveNavigation).observe(sideNav);
$("refresh").onclick=()=>run(false,true);
$("pause").onclick=()=>{
  if (fixed) {fixed=null; $("range").value="900"; syncRange();}
  if (Number($("interval").value)===0) $("interval").value="15";
  paused=$("pause").getAttribute("aria-pressed")==="false";
  schedule(); run();
};
$("interval").onchange=()=>{paused=Number($("interval").value)===0; schedule(); run(false,true);};
$("transaction").value=transaction;
$("transaction").onchange=()=>{transaction=$("transaction").value;run();};
$("bc").onchange=()=>{bc=$("bc").value;app="";instance="";setInstances(learnedInstances);run();};
$("app").onchange=()=>{app=$("app").value;instance="";setInstances(learnedInstances);run();};
$("instance").onchange=()=>{instance=$("instance").value;run();};
$("range").onchange=()=>{
  if($("range").value==="custom") {
    const current=windowParams();
    $("from").value=fmtInput(current.from);
    $("to").value=fmtInput(current.to);
    $("custom").hidden=false; return;
  }
  fixed=null; paused=false; syncRange(); run();
};
for(const [id,direction] of [["back",-1],["forward",1]]) {
  $(id).onclick=()=>{
    const {from,to}=windowParams();
    const end=observerNow();
    const span=to-from;
    const nextTo=Math.max(span,Math.min(end,to+span*direction));
    zoom({from:nextTo-span,to:nextTo});
  };
}
$("custom").onsubmit=event=>{
  event.preventDefault();
  const from=Math.floor(new Date($("from").value).getTime()/1000);
  const to=Math.floor(new Date($("to").value).getTime()/1000);
  if (!Number.isFinite(from)||!Number.isFinite(to)||from<0||from>=to||to-from>604800||to>observerNow()+60) {
    $("status").textContent="请输入有效时间，范围最多 7 天。";return;
  }
  zoom({from,to});
};
loadProfile().catch(()=>null).then(()=>{
  if(fixed && (!Number.isFinite(fixed.from) || !Number.isFinite(fixed.to) || fixed.from<0 || fixed.to<=fixed.from ||
     fixed.to-fixed.from>604800 || fixed.to>observerNow()+60)) {fixed=null;paused=false;}
  syncRange();
  $("boot-status").hidden=true;
  schedule();
  run();
});
