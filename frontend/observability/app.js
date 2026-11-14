"use strict";

const $ = id => document.getElementById(id);
const views = {overview:"运行总览", service:"业务性能", traces:"链路追踪", runs:"研发实验", extensions:"扩展遥测"};
const sections = {
  overview:["http","cpu","memory","disk","ws","sse"],
  service:["http_p99","history_p99","queue","pool","outbox","chat_queue","chat_pool","chat_before_commit","chat_commit","chat_ack_ready"],
};
const states = {complete:"查询成功",partial:"部分链路",missing:"未采到链路",unavailable:"追踪数据源不可用",
  success:"成功",failed:"失败",timeout:"超时",started:"已开始",retry:"重试",skipped:"跳过",queued:"等待",estop:"急停",
  fault:"故障注入",recovered:"恢复",finished:"已结束",version:"版本变更",sent:"已下发",ack:"已确认",error:"报错"};
const names = {"dag.run":"DAG 运行","dag.node":"节点执行","dag.edge":"依赖边",
  "agent.plan":"规划","agent.skill":"技能调用","agent.memory":"记忆操作","agent.decision":"策略判断","agent.evaluation":"评测标记",
  "device.window":"设备统计窗口","device.command":"设备指令","device.safety":"设备安全","experiment.marker":"实验标记"};
const params = new URL(location.href).searchParams;
let view = Object.hasOwn(views, params.get("view")) ? params.get("view") : "overview";
let fixed = params.has("from") && params.has("to") ? {from:Number(params.get("from")),to:Number(params.get("to"))} : null;
let traceId = params.get("trace") || "";
let paused = !!fixed, version = 0, request, charts = [], catalog, domain = "dag", cursor = "", eventPages = [];
const group = "nebula-charts";
const availableCharts = () => typeof window.echarts !== "undefined";

function el(tag, text, className) {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

async function api(path, args, signal) {
  const query = args ? "?" + new URLSearchParams(args) : "";
  const response = await fetch("/api/v1/" + path + query, {signal, cache:"no-store"});
  const data = await response.json();
  if (!response.ok && !(path.startsWith("traces/") && response.status === 404))
    throw new Error(data.error || "查询失败");
  return data;
}

function windowParams() {
  const now = Math.floor(Date.now()/1000);
  return fixed || {from:now - Number($("range").value || 900), to:now};
}

function remember() {
  const url = new URL(location.href);
  url.search = "";
  url.searchParams.set("view",view);
  if (fixed) for (const [k,v] of Object.entries(fixed)) url.searchParams.set(k,v);
  if (view === "traces" && traceId) url.searchParams.set("trace",traceId);
  history.replaceState(null,"",url);
}

function at(value) {
  return new Date(value).toLocaleString("zh-CN",{hour12:false});
}

function alertPanel(alerts) {
  const labels = {NebulaUnreachable:"NEBULA 服务不可达",NebulaUnready:"业务暂未就绪",NebulaScrapeFailed:"指标抓取失败",NebulaQueueBacklog:"数据库任务持续排队"};
  const box = el("div",undefined,"list");
  if (!alerts.length) box.append(el("p","当前没有触发的规则","empty"));
  for (const alert of alerts) {
    const row = el("div",undefined,"row");
    row.append(el("span",labels[alert.labels.alertname] || alert.labels.alertname));
    row.append(el("strong",alert.state === "firing" ? "已触发" : "观察中","bad"));
    const button = el("button","查看相关时段");
    button.addEventListener("click",() => {
      const end = Math.floor(Date.now()/1000), start = Math.floor(Date.parse(alert.activeAt)/1000);
      fixed = {from:Math.max(end-7*86400,(Number.isFinite(start) ? start : end)-300),to:end};
      paused = true;
      go("service");
    });
    row.append(button);
    box.append(row,el("p",alert.annotations?.guide || "对照同一时间段的健康探针、资源和数据库曲线。","muted"));
  }
  return box;
}

function metricPanel(definition, sample, markers, bounds) {
  const panel = el("section",undefined,"panel");
  panel.append(el("h3",definition.title));
  const plot = el("div",undefined,"chart");
  panel.append(plot);
  const info = el("details");
  info.append(el("summary","指标解释与查询"),el("p",definition.guide),el("code",definition.query));
  panel.append(info);
  if (!sample || !sample.series.length || !sample.series.some(line => line.points.some(point => point[1] !== null))) {
    plot.textContent = sample ? "这个时段暂无指标样本" : "指标数据源暂不可用";
    plot.className = "empty";
    return panel;
  }
  if (!availableCharts()) {
    basicPlot(plot, sample, markers, bounds, definition);
    return panel;
  }
  requestAnimationFrame(() => {
    if (!plot.isConnected) return;
    const graph = window.echarts.init(plot);
    graph.group = group;
    charts.push(graph);
    graph.setOption({
      animation:false, backgroundColor:"transparent", color:["#64d8be","#8cb8ff","#edbc79"],
      grid:{left:56,right:16,top:22,bottom:38}, tooltip:{trigger:"axis",axisPointer:{type:"cross"}},
      xAxis:{type:"time",min:bounds.from*1000,max:bounds.to*1000,axisLabel:{color:"#a9bacd"}},
      yAxis:{type:"value",name:definition.unit,nameTextStyle:{color:"#a9bacd"},axisLabel:{color:"#a9bacd"},splitLine:{lineStyle:{color:"#2a3b50"}}},
      dataZoom:[{type:"inside",zoomOnMouseWheel:"ctrl",moveOnMouseWheel:false}],
      toolbox:{right:10,feature:{dataZoom:{yAxisIndex:"none",title:{zoom:"框选时间",back:"还原范围"}}}},
      series:sample.series.map((line,index) => {
        const data = [];
        for (const p of line.points) {
          if (data.length && p[0]*1000-data.at(-1)[0] > sample.step*1500) data.push([data.at(-1)[0]+sample.step*1000,null]);
          data.push([p[0]*1000,p[1]]);
        }
        return {type:"line",showSymbol:false,connectNulls:false,data,name:line.labels.instance || definition.title,
          ...(index === 0 && markers.length ? {markLine:{symbol:"none",label:{show:false},data:markers.map(x=>({name:x.fields.state,xAxis:Number(BigInt(x.occurredAt)/1000000n)}))}} : {})};
      })
    });
    window.echarts.connect(group);
  });
  return panel;
}

function basicPlot(plot, sample, markers, bounds, definition) {
  // The standard-library development mode can still inspect a time series.
  const ns = "http://www.w3.org/2000/svg";
  const make = (tag, attributes) => {
    const node = document.createElementNS(ns,tag);
    for (const [name,value] of Object.entries(attributes)) node.setAttribute(name,value);
    return node;
  };
  const svg = make("svg",{viewBox:"0 0 700 225",role:"img","aria-label":definition.title});
  svg.classList.add("basic-chart");
  const valid = sample.series.flatMap(line=>line.points.map(p=>p[1]).filter(v=>v!==null));
  const low=Math.min(0,...valid), high=Math.max(1,...valid);
  const x = stamp => 61+618*(stamp-bounds.from)/(bounds.to-bounds.from);
  const y = value => 185-154*(value-low)/(high-low);
  for(let i=0;i<=4;i++) {
    const val=low+(high-low)*i/4;
    const yy=y(val);
    const label=make("text",{x:54,y:yy+4,"text-anchor":"end",fill:"#a9bacd"});
    label.textContent=Number(val.toPrecision(3));
    svg.append(make("line",{x1:61,x2:679,y1:yy,y2:yy,stroke:"#2a3b50"}),label);
  }
  markers.forEach(marker => {
    const xx=x(Number(BigInt(marker.occurredAt)/1000000000n));
    svg.append(make("line",{x1:xx,x2:xx,y1:31,y2:185,stroke:"#edbc79","stroke-dasharray":"3 4"}));
  });
  sample.series.forEach((series,index)=>{
    const color=["#64d8be","#8cb8ff","#edbc79"][index%3];
    let d="", previous=null;
    for(const [stamp,value] of series.points) {
      if(value===null) {previous=null;continue;}
      d+=(previous===null||stamp-previous>sample.step*1.5?" M":" L")+x(stamp)+" "+y(value);
      previous=stamp;
    }
    svg.append(make("path",{d,fill:"none",stroke:color,"stroke-width":2,"vector-effect":"non-scaling-stroke"}));
  });
  const t=make("text",{x:61,y:211,fill:"#a9bacd"});
  t.textContent=at(bounds.from*1000)+"  —  "+at(bounds.to*1000)+"  ·  "+definition.unit;
  svg.append(t);
  plot.append(svg);
}

async function renderMetrics(root,bounds,signal) {
  if (!catalog) catalog = await api("catalog",null,signal);
  const collect = path => api(path,null,signal).catch(() => null);
  const keys = sections[view];
  const [overview, alerts, runtime, markers, ...samples] = await Promise.all([
    collect("overview"), collect("alerts"), collect("version"), api("markers",bounds,signal).catch(()=>null),
    ...keys.map(panel => api("series",{...bounds,panel},signal).catch(()=>null)),
  ]);
  if (signal.aborted) return;
  const cards = el("div",undefined,"cards");
  for (const [key,title] of Object.entries({alive:"服务存活",ready:"业务就绪",scrape:"指标抓取",monitor:"Observer"})) {
    const card = el("div",undefined,"card"), point = overview?.signals?.[key];
    card.append(el("span",title,"muted"),el("strong",point ? (point[1] === 1 ? "正常" : "异常") : "未知",point?.[1] === 1 ? "ok" : ""));
    if (point) card.append(el("small","查询时间 "+at(point[0]*1000)));
    cards.append(card);
  }
  const fingerprint = runtime?.sourceFingerprint?.slice(0,12) || "未知";
  root.append(el("p","运行版本 "+(runtime?.revision || "未知")+" · 源码摘要 "+fingerprint,"muted"),
    cards,el("h2","告警"),alerts ? alertPanel(alerts) : el("p","无法加载告警","empty"),el("h2","指标趋势"));
  const panels = el("div",undefined,"panels");
  for (const [i,key] of keys.entries())
    panels.append(metricPanel(catalog.find(d=>d.id===key),samples[i],markers || [],bounds));
  if (markers?.length) {
    const box=el("div",undefined,"list");
    for (const marker of markers) {
      const row=el("div",undefined,"row");
      row.append(el("span",at(Number(BigInt(marker.occurredAt)/1000000n))+" · "+marker.fields.scenario),
        el("strong",states[marker.fields.state]||marker.fields.state));
      if (marker.fields.recordId) {
        const expand=el("button","查看实验记录");
        expand.onclick=async()=>{
          try {
            const data=await api("runs/"+marker.fields.recordId,null,signal);
            output.textContent="ACK P99 "+(data.result.ackP99Ms??"—")+" ms · "+
              (data.result.ackSamples??"—")+" 样本 · 源码 "+(data.identity.commit||data.identity.sourceFingerprint||"未知");
          } catch(err){output.textContent=err.message;}
        };
        const output=el("p","","muted");
        row.append(expand,output);
      }
      box.append(row);
    }
    root.append(el("h2","实验与故障标注"),box);
  }
  root.append(panels);
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

function timeline(spans, events) {
  const panel = el("div",undefined,"timeline");
  if (!spans.length) { panel.append(el("div","没有可绘制的 span","empty")); return panel; }
  const spansSorted = spans.slice().sort((a,b) => Number(BigInt(a.start)-BigInt(b.start)));
  const start = BigInt(spansSorted[0].start), end = spansSorted.reduce((n,s) => BigInt(s.end) > n ? BigInt(s.end) : n,start);
  const total = Math.max(1,Number(end-start));
  const nodeMap = new Map(events.filter(e=>e.eventType==="dag.node" && e.spanId).map(e=>[e.spanId,e.fields]));
  const more = el("div","选择一个时间条查看详情","trace-info");
  for (const item of spansSorted) {
    const row = el("div",undefined,"span-row"), track=el("div",undefined,"span-track");
    const info = nodeMap.get(item.id), title = info ? info.nodeId + " · 第 " + info.attempt + " 次" : item.name;
    const length = Math.max(0,Number(BigInt(item.end)-BigInt(item.start)));
    const bar = el("button",undefined,"span-bar" + ([2,"STATUS_CODE_ERROR"].includes(item.status) ? " bad" : ""));
    bar.style.left = Math.max(0,Number(BigInt(item.start)-start)/total*100) + "%";
    bar.style.width = Math.max(.4,length/total*100) + "%";
    bar.title = title;
    bar.setAttribute("aria-label",title);
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
  const nodes = [...new Set(edges.flat())];
  if (!nodes.length) return null;
  const ns = "http://www.w3.org/2000/svg";
  const wrap = el("div",undefined,"topology");
  const svg = document.createElementNS(ns,"svg");
  svg.setAttribute("height","135"); svg.setAttribute("width",String(Math.max(350,nodes.length*120)));
  svg.setAttribute("aria-label","DAG 节点依赖图");
  const depth = Object.fromEntries(nodes.map(n=>[n,0]));
  for (let round=0; round<nodes.length-1; ++round)
    for (const [a,b] of edges) depth[b] = Math.min(nodes.length-1,Math.max(depth[b],depth[a]+1));
  const tiers = {};
  const points = {};
  for (const n of nodes) { const d=depth[n]; tiers[d]=(tiers[d]||0)+1; points[n]=[45+110*d,30+42*(tiers[d]-1)]; }
  svg.setAttribute("height",String(Math.max(120,55+42*Math.max(...Object.values(tiers)))));
  for (const [a,b] of edges) {
    const l = document.createElementNS(ns,"line");
    for (const [name,v] of Object.entries({x1:points[a][0]+25,y1:points[a][1],x2:points[b][0]-27,y2:points[b][1],stroke:"#64d8be","stroke-width":2})) l.setAttribute(name,v);
    svg.append(l);
  }
  for (const n of nodes) {
    const [x,y] = points[n], label = document.createElementNS(ns,"text");
    label.setAttribute("x",x); label.setAttribute("y",y+4); label.setAttribute("text-anchor","middle");
    label.setAttribute("fill","#e8eef7"); label.setAttribute("tabindex","0"); label.textContent=n;
    const select = () => [...waterfall.querySelectorAll(".span-bar")].find(bar=>bar.dataset.node===n)?.click();
    label.addEventListener("click",select); label.addEventListener("keydown",e=>{if(e.key==="Enter")select();});
    svg.append(label);
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

async function renderTraces(root,signal) {
  const form = el("form",undefined,"strip"), input = el("input");
  input.id="traceId"; input.required=true; input.pattern="[a-f0-9]{32}"; input.value=traceId;
  input.setAttribute("aria-label","Trace ID");
  form.append(el("label","Trace ID"),input,el("button","查询"));
  form.onsubmit = event => {event.preventDefault(); traceId=input.value.trim(); run();};
  root.append(form);
  if (!/^[a-f0-9]{32}$/.test(traceId)) { root.append(el("p","从扩展事件或探针报告打开一个 trace ID。","notice")); return; }
  const data = await api("traces/"+traceId,null,signal);
  if (signal.aborted) return;
  const waterfall = timeline(data.spans,data.items);
  root.append(el("h2",states[data.status] || data.status));
  const topology = graph(data.items,waterfall);
  if (topology) root.append(el("h2","节点依赖"),topology);
  root.append(el("h2",topology ? "执行甘特图" : "链路瀑布图"),waterfall);
  if (data.items.some(e=>e.eventType.startsWith("agent."))) {
    const benefits = data.agent;
    root.append(el("h2","Agent 统计"),el("p","执行尝试 "+benefits.attempts+
      " · 输入 Token "+(benefits.tokens.inputTokens ?? "未知")+" · 输出 Token "+(benefits.tokens.outputTokens ?? "未知")+
      " · 缺失 Token 字段 "+benefits.unknownTokenFields,"notice"));
    for (const [skill,s] of Object.entries(benefits.skills)) root.append(el("p",skill+"　成功 "+s.succeeded+" / "+s.finished,"muted"));
    root.append(el("p",benefits.cost ? "估算费用 "+benefits.cost.estimate+" "+benefits.cost.currency :
      "估算费用未知 · 需要完整 Token 记录与配置费率","muted"));
  }
  root.append(el("h2","关联事件"),eventList(data.items));
}

async function renderRuns(root,bounds,signal) {
  const records = await api("runs",bounds,signal), list = el("div",undefined,"list");
  root.append(el("h2","实验记录"),list);
  if (!records.length) list.append(el("p","此时段没有导入实验记录","notice"));
  const selected = new Set();
  for (const r of records) {
    const row = el("label",undefined,"row"), check = el("input");
    check.type="checkbox";
    check.onchange = () => { if (check.checked && selected.size < 2) selected.add(r.id); else {selected.delete(r.id);check.checked=false;} };
    row.append(check,el("span",r.scenario+" · "+at(Date.parse(r.startedAt))),
      el("span","ACK P99 "+(r.result.ackP99Ms ?? "—")+" ms · "+(r.result.ackSamples ?? "—")+" 样本"));
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
  for (const [key,title] of [["dag","DAG 调度"],["agent","Agent"],["device","设备遥测"]]) {
    const button = el("button",title); button.setAttribute("aria-pressed",String(key===domain));
    button.onclick=()=>{domain=key;run();}; strip.append(button);
  }
  const data = await api("events",{...bounds,domain,limit:100,...(append&&cursor?{after:cursor}:{})},signal);
  if (signal.aborted) return;
  eventPages = append ? [...eventPages,...data.items] : data.items;
  cursor = data.next ? data.next.join(",") : "";
  root.append(strip,eventList(eventPages));
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

async function run(append=false) {
  const current=++version;
  request?.abort();
  request=new AbortController();
  const signal=request.signal;
  remember();
  $("title").textContent=views[view];
  for(const button of document.querySelectorAll("nav button")) {
    if (button.dataset.view===view) button.setAttribute("aria-current","page"); else button.removeAttribute("aria-current");
  }
  $("pause").setAttribute("aria-pressed",String(paused));
  $("pause").textContent=paused?"继续自动刷新":"暂停刷新";
  const bounds=windowParams(), draft=el("div");
  $("status").textContent="正在查询…";
  try {
    if (sections[view]) await renderMetrics(draft,bounds,signal);
    else if(view==="traces") await renderTraces(draft,signal);
    else if(view==="runs") await renderRuns(draft,bounds,signal);
    else await renderExtensions(draft,bounds,signal,append);
    if (current!==version) return;
    charts.splice(0).forEach(c=>c.dispose());
    $("body").replaceChildren(...draft.childNodes);
    $("status").textContent="更新于 "+at(Date.now());
  } catch(error) {
    if(current!==version) return;
    $("status").textContent="查询失败 · "+error.message;
  }
}

function go(next) {view=next;cursor="";eventPages=[];run();}
document.querySelectorAll("nav button").forEach(button=>button.onclick=()=>go(button.dataset.view));
$("refresh").onclick=()=>run();
$("pause").onclick=()=>{paused=!paused;run();};
$("range").onchange=()=>{$("custom").hidden=$("range").value!=="custom";if($("custom").hidden){fixed=null;run();}};
$("custom").onsubmit=event=>{
  event.preventDefault();
  const from=Math.floor(new Date($("from").value).getTime()/1000);
  const to=Math.floor(new Date($("to").value).getTime()/1000);
  if (!Number.isFinite(from)||!Number.isFinite(to)||from>=to||to-from>604800||to>Date.now()/1000+60) {
    $("status").textContent="请输入有效时间，范围最多 7 天。";return;
  }
  fixed={from,to}; run();
};
if(fixed){$("range").value="custom";$("custom").hidden=false;}
addEventListener("resize",()=>charts.forEach(c=>c.resize()));
setInterval(()=>{ if(!paused&&!document.hidden&&sections[view])run(); },15000);
fetch("/health").then(res=>res.json()).then(state=>{
  if(state.simulated){
    $("environment").textContent="模拟数据预览";
    document.querySelector(".eyebrow").textContent="模拟数据预览";
  }
}).catch(()=>{});
run();
