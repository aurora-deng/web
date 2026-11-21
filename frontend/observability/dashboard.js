import {drawChart, extrema, formatValue, palette, timestamp} from "./chart.js";
import {readSeries,queryFailed,emptyState} from "./series.js";
import {DashboardQueries} from "./dashboard_queries.js";

const transactionNames = {http:"会话列表", history:"消息历史"};
const failure = {auth_rejected:"鉴权拒绝", origin_rejected:"来源限制", rate_limited:"限流拒绝",
  db_failed:"数据库任务失败", outbox_failed:"Outbox 投递失败"};
const traffic = {http:"全部 HTTP", http_rate:"会话列表", history_rate:"消息历史"};
const latency = {avg:"平均", p50:"P50", p90:"P90", p95:"P95", p99:"P99"};
const stages = {chat_queue:"任务排队", chat_pool:"取连接", chat_before_commit:"提交前累计",
  chat_commit:"COMMIT", chat_ack_ready:"ACK 就绪累计"};
const resource = {network_tx:"发送", network_rx:"接收"};
const healthLabels = {alive:"服务存活", ready:"业务就绪", scrape:"指标抓取", monitor:"Observer"};
const sourceNames = {preview:"模拟预览", local:"NEBULA 原始指标 / Linux", prometheus:"Prometheus / 业务探针"};
const metricsCache = new Map();

function el(tag, text, cls) {
  const node = document.createElement(tag);
  if (text != null) node.textContent = text;
  if (cls) node.className = cls;
  return node;
}

function setText(node,text) {
  if(node.textContent!==text)node.textContent=text;
}
function button(label, cls, onClick) {
  const btn = el("button",label,cls);
  btn.type = "button"; btn.onclick = onClick;
  return btn;
}

function definitions(view, transaction) {
  const latencyKeys = Object.fromEntries(Object.entries(latency).map(([key,title])=>[transaction+"_"+key,title]));
  if (view === "overview") return [
    {title:"请求速率", keys:traffic, unit:"次/秒", area:true, id:"traffic"},
    {title:transactionNames[transaction]+" · 响应耗时", keys:latencyKeys, unit:"ms", id:"latency"},
    {title:"主机 CPU / 内存使用率", keys:{cpu:"CPU",memory:"内存"}, unit:"%",cap:100, area:true,id:"host"},
    {title:"实时连接", keys:{ws:"WebSocket",sse:"SSE"}, unit:"连接",area:true,id:"connections"},
    {title:"主机网络吞吐", keys:resource, unit:"B/秒", area:true,id:"network"},
    {title:"NEBULA 进程线程", keys:{threads:"线程"},unit:"线程",area:true,stepped:true,id:"threads"},
  ];
  return [
    {title:"请求速率", keys:traffic, unit:"次/秒", area:true,id:"traffic"},
    {title:transactionNames[transaction]+" · 响应耗时", keys:latencyKeys, unit:"ms", id:"latency"},
    {title:"主机网络吞吐",keys:resource,unit:"B/秒",area:true,id:"network"},
    {title:"NEBULA 进程线程",keys:{threads:"线程"},unit:"线程",area:true,stepped:true,id:"threads"},
    {title:"聊天服务端处理阶段 · P99",keys:stages,unit:"ms",id:"chat"},
    {title:"数据库队列 / 连接池",keys:{queue:"排队",pool:"借出连接"},unit:"",area:true,id:"database"},
    {title:"已埋点失败 / 拒绝速率",keys:failure,unit:"次/秒",id:"errors"},
    {title:"Outbox 投递速率",keys:{outbox:"已投递"},unit:"条/秒",area:true,id:"outbox"},
  ];
}

function labelsName(labels) {
  return [labels.instance, ...Object.entries(labels)
    .filter(([key])=>!["instance","job","__name__"].includes(key)).map(([key,value])=>key+"="+value)].filter(Boolean).join(" · ");
}

const seriesId = (key, labels) => key + JSON.stringify(Object.entries(labels).sort((a,b)=>a[0].localeCompare(b[0])));
const hiddenSeries = new Set();

function buildSeries(model, samples, instance) {
  let index = 0;
  return Object.entries(model.keys).flatMap(([key, name]) => {
    const data = samples.get(key);
    return (data?.series || []).map(line => {
      const id=seriesId(key,line.labels);
      return {...line, key, id, step:data.step, color:palette[index++ % palette.length],
        name:(data.series.length>1 && labelsName(line.labels)) ? name+" · "+labelsName(line.labels) : name,
        hidden:hiddenSeries.has(id)};
    }).filter(line=>!instance || line.labels.instance === instance);
  });
}

function dataState(model, samples, series) {
  if (series.some(s=>s.points.some(p=>p[1]!==null))) return "ok";
  return Object.keys(model.keys).every(key=>queryFailed(samples.get(key))) ? "unavailable" : "empty";
}
function makeTable(head, rows, cls = "") {
  const wrap=el("div",null,"table-wrap "+cls);
  const table=el("table"), header=el("thead"), titles=el("tr"), body=el("tbody");
  for(const column of head) titles.append(el("th",column));
  header.append(titles);
  for(const row of rows){
    const record=el("tr");
    for(const value of row) record.append(el("td",value));
    body.append(record);
  }
  table.append(header,body);wrap.append(table);return wrap;
}

function csvText(rows) {
  const safe = value => {
    let cell=String(value ?? "");
    if (typeof value === "string" && /^[\t\r\n ]*[=+\-@]/.test(cell)) cell="'"+cell;
    return '"'+cell.replaceAll('"','""')+'"';
  };
  return rows.map(row=>row.map(safe).join(",")).join("\r\n");
}

function download(model, rows) {
  const url=URL.createObjectURL(new Blob(["\uFEFF"+csvText([["时间(UTC)","序列","值","单位"],...rows])],
    {type:"text/csv;charset=utf-8;"}));
  const link=el("a"); link.href=url; link.download="nebula-"+model.id+".csv"; link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

function openInspector(model, definitions, source, samples, series, bounds) {
  const dialog=document.getElementById("inspector");
  const heading=document.getElementById("inspect-title");
  const body=document.getElementById("inspect-body");
  const tabs=el("div",null,"strip");
  const stats=el("div",null,"inspect-content");
  const scope=el("p",timestamp(bounds.from,true)+" — "+timestamp(bounds.to,true)+" · 本地时间","muted");
  heading.textContent=model.title;
  const describe = key=>{
    const def=definitions.get(key), data=samples.get(key);
    const info=el("div",null,"query-block");
    info.append(el("strong",def.title),el("p",def.guide,"muted"),
      el("p",sourceNames[source]+" · "+(queryFailed(data)?"请求失败":
        (data.batch?"批量查询 ":"后端查询 ")+data.queryMs+" ms / 步长 "+data.step+" s"),"muted"));
    if(queryFailed(data)) info.append(el("p",emptyState([data]).guide,"notice"));
    else if(data.diagnostic) info.append(el("p",data.diagnostic.message+" · "+data.diagnostic.guide,"notice"));
    if (source==="prometheus") info.append(el("code",def.query));
    if (source==="local") info.append(el("code","/api/v1/series?"+new URLSearchParams({...bounds,panel:key})),
      el("p","当前读取本机采样；下面的表达式供完整栈复现。","muted"),el("code",def.query));
    if (source==="preview") info.append(el("p","模拟数据。对应完整栈表达式：","muted"),el("code",def.query));
    if(data?.truncated) info.append(el("p","查询超过八个序列，本次返回前八个。","notice"));
    return info;
  };
  const columns=["序列","样本","最新","平均","最小","最大"];
  const raw=series.filter(line=>!line.hidden).flatMap(line=>line.points.map(([t,v])=>({t,v,name:line.name})))
    .sort((a,b)=>b.t-a.t || a.name.localeCompare(b.name,"zh-CN"));
  const csv=raw.map(row=>[new Date(row.t * 1000).toISOString(),row.name,row.v,model.unit]);
  const renderers = {
    "统计": () => makeTable(columns,series.filter(line=>!line.hidden).map(line=>{
      const stat=extrema(line);
      return [line.name,stat.count,...[stat.last,stat.mean,stat.min,stat.max].map(v=>formatValue(v,model.unit))];
    })),
    "原始数据": () => {
      const wrap=el("div"), bar=el("div",null,"strip");
      const pageSize=100; let offset=0;
      const table=el("div");
      const prev=button("上一页","",()=>{offset-=pageSize;repaint();});
      const next=button("下一页","",()=>{offset+=pageSize;repaint();});
      const count=el("small");
      function repaint(){
        table.replaceChildren(makeTable(["时间","序列","数值"],raw.slice(offset,offset+pageSize)
          .map(row=>[timestamp(row.t,true),row.name,formatValue(row.v,model.unit)])));
        prev.disabled=offset===0;
        next.disabled=offset+pageSize>=raw.length;
        count.textContent=raw.length?"第 "+(offset+1)+"–"+Math.min(raw.length,offset+pageSize)+" 行 / 共 "+raw.length+" 行":"暂无数据";
      }
      bar.append(button("下载 CSV","",()=>download(model,csv)),prev,next,count);
      bar.firstChild.disabled=!raw.length;
      wrap.append(bar,table);
      repaint(); return wrap;
    },
    "查询详情": () => {
      const wrap=el("div");
      for(const key of Object.keys(model.keys)) wrap.append(describe(key));
      return wrap;
    },
  };
  function select(tab) {
    for(const btn of tabs.children) btn.setAttribute("aria-pressed",String(btn.textContent===tab));
    stats.replaceChildren(renderers[tab]());
  }
  for(const tab of Object.keys(renderers)) tabs.append(button(tab,"",()=>select(tab)));
  body.replaceChildren(scope,tabs,stats); select("统计");
  if (!dialog.open) dialog.showModal();
}

function panel(model, context) {
  const {catalog, source, instance, bus, zoom, focus, visuals} = context;
  let samples, bounds, series=[], graph;
  const panel=el("section",null,"panel metric-panel");
  panel.id=model.id;
  const head=el("div",null,"panel-header");
  const title=el("h3",model.title);
  const actions=el("div",null,"panel-actions");
  const resources=el("a","查资源占比","minimal");
  if(model.id==="host" || model.id==="threads") actions.append(resources);
  const inspect=button("检查","minimal",()=>openInspector(model,catalog,source,samples,series,bounds));
  actions.append(inspect,button(context.panel ? "返回" : "放大","minimal",()=>focus(context.panel ? "" : model.id)));
  head.append(title,actions);
  const warning=el("small",null,"panel-warning");
  const host=el("div",null,"chart"), legend=el("div",null,"legend"), buttons=new Map();
  panel.append(head,warning,host,legend);
  const view={
    element:panel,
    update(nextSamples,nextBounds,markers){
      samples=nextSamples;bounds=nextBounds;
      series=buildSeries(model,samples,instance);
      const state=dataState(model,samples,series);
      warning.textContent=Object.keys(model.keys).some(key=>queryFailed(samples.get(key))) ?
        (state==="unavailable"?"数据源不可用":"部分指标未能查询") :
        Object.keys(model.keys).some(key=>samples.get(key)?.truncated)?"仅显示每指标前八个序列":"";
      warning.hidden=!warning.textContent;
      resources.href="/?"+new URLSearchParams({view:"resources",...bounds});
      host.classList.toggle("empty-chart",state!=="ok");
      legend.hidden=state!=="ok";
      if(state!=="ok") {
        graph?.dispose();graph=null;
        const diagnostic=emptyState(Object.keys(model.keys).map(key=>samples.get(key)));
        host.replaceChildren(el("strong",diagnostic.message),el("small",diagnostic.guide));
        return;
      }
      const names=new Set(series.map(line=>line.id));
      for(const [id,item] of buttons) if(!names.has(id)) {item.button.remove();buttons.delete(id);}
      series.forEach((line,index)=>{
        let item=buttons.get(line.id);
        if(!item) {
          item={button:button("","legend-item",()=>{
            const active=item.line;
            active.hidden=!active.hidden;
            if(active.hidden) hiddenSeries.add(active.id);else hiddenSeries.delete(active.id);
            item.button.setAttribute("aria-pressed",String(!active.hidden));
            graph?.repaint();
          })};
          buttons.set(line.id,item);
        }
        item.line=line;
        setText(item.button,line.name+"  "+formatValue(line.points.at(-1)?.[1],model.unit));
        item.button.style.setProperty("--series",line.color);
        item.button.setAttribute("aria-pressed",String(!line.hidden));
        if(legend.children[index]!==item.button) legend.insertBefore(item.button,legend.children[index]||null);
      });
      if(graph) graph.update({bounds,series,markers});
      else graph=drawChart(host,{model,bounds,series,markers,bus,zoom,
        essential:["traffic","latency","network","threads"].includes(model.id)&&visualPointers()});
    },
    setMarkers:markers=>graph?.setMarkers(markers),
    paint:()=>graph?.paint(),
    interacting:()=>graph?.interacting(),
    dispose:()=>graph?.dispose(),
  };
  visuals.push(view);
  return view;
}

function visualPointers() {
  return matchMedia("(pointer: fine) and (min-width: 1000px)").matches;
}

function card(label, unit, color) {
  const stats=el("article",null,"stat-card");
  stats.style.setProperty("--value-color",color);
  stats.append(el("span",label));
  const gauge=unit==="%"?el("meter",null,"stat-meter"):null;
  if(gauge) {
    gauge.min=0; gauge.max=100;
    gauge.setAttribute("aria-label",label);
    stats.append(gauge);
  }
  const value=el("strong"), note=el("small");
  stats.append(value,note);
  return {element:stats,update(number,description){
    if(gauge) {
      gauge.hidden=number==null;
      if(number!=null&&gauge.value!==number)gauge.value=number;
    }
    setText(value,formatValue(number,unit));
    setText(note,description);
  }};
}

function showHealth(badges, report) {
  for(const [key,badge] of badges) {
    const value=report?.signals?.[key]?.[1];
    setText(badge,value==null?"数据暂不可用":value===1?"正常":"异常");
    const cls="stat-health "+(value==null?"unknown":value===1?"ok":"bad");
    if(badge.className!==cls)badge.className=cls;
  }
}

function summarize(model, samples, instance) {
  const rows=buildSeries(model,samples,instance).map(extrema).filter(row=>row.count);
  return {stats:rows.length===1?rows[0]:null,seriesCount:rows.length};
}

function latestStamp(samples) {
  let max=0;
  for(const report of samples.values()) for(const line of report?.series || []) {
    const point=line.points.findLast(p=>p[1]!=null);
    if(point) max=Math.max(max,point[0]);
  }
  return max;
}

export async function renderDashboard(root, settings) {
  const {api, view, signal, transaction, bounds, reading, alertPanel, focus, zoom, now,
    panel: selectedPanel, instance, allowed, setInstances, visuals} = settings;
  const health=await reading;
  const models=definitions(view,transaction);
  const selected=models.find(model=>model.id===selectedPanel);
  const statModels=[
    ["HTTP 请求速率","http","次/秒",palette[0],"区间内采样平均"],
    [transactionNames[transaction]+" · 平均耗时",transaction+"_avg","ms",palette[3],"窗口末尾 · 五分钟样本"],
    [transactionNames[transaction]+" · P99",transaction+"_p99","ms",palette[3],"窗口末尾 · 五分钟样本"],
    ["主机网络发送","network_tx","B/秒",palette[2],"区间内采样平均"],
    ["主机网络接收","network_rx","B/秒",palette[1],"区间内采样平均"],
    view==="overview" ? ["CPU 使用率","cpu","%",palette[4],"窗口末尾"] : ["进程线程","threads","线程",palette[4],"窗口末尾"],
  ];
  const keys=[...new Set((selected?[selected]:models).flatMap(model=>Object.keys(model.keys))
    .concat(selected?[]:statModels.map(row=>row[1])))];
  const queries=selected?{markers:"故障标注"}:
    {overview:"采集状态",alerts:"告警",version:"版本信息",markers:"故障标注"};
  const catalogRequest=metricsCache.get(health.source)??api("catalog",null,signal).then(rows=>{
    const catalog=new Map(rows.map(item=>[item.id,item]));
    metricsCache.set(health.source,catalog);
    return catalog;
  });
  const [samples,catalog]=await Promise.all([readSeries(api,keys,bounds,signal,health.seriesBatch),catalogRequest]);
  if(signal.aborted) return null;
  const instances=[...new Set([...samples.values()].flatMap(report=>report?.series.map(line=>line.labels.instance).filter(Boolean) || []))].sort();
  setInstances(instances);
  if(allowed) for(const [name,report] of samples) {
    if(report)samples.set(name,{...report,series:report.series.filter(line=>allowed.has(line.labels.instance))});
  }
  const key=JSON.stringify([health.source,view,transaction,selectedPanel,instance,allowed?[...allowed].sort():null,
    bounds.to-bounds.from,settings.fixed?bounds.to:null]);
  const reused=settings.previous?.key===key&&settings.previous.element.isConnected;
  const live=reused?settings.previous:mountDashboard(root,{
    key,view,selected,models,statModels,catalog,health,instance,zoom,focus,selectedPanel,visuals,api,queries,
  });
  live.bounds=bounds;
  const generation=Symbol();
  live.generation=generation;
  const active=()=>!signal.aborted&&live.generation===generation;
  if(!selected) statModels.forEach(([label,key,,,note],index)=>{
    const {stats,seriesCount}=summarize({keys:{[key]:label}},samples,instance);
    const value=note.includes("采样平均")?stats?.mean:stats?.last;
    const missing=stats?"最新采样为空":seriesCount>1?"多条序列，请筛选实例":emptyState([samples.get(key)]).message;
    live.stats[index].update(value,value==null?missing:note);
  });
  const marksInRange=()=>live.marks.filter(marker=>marker.time>=bounds.from&&marker.time<=bounds.to);
  const visibleMarks=marksInRange();
  for(const chart of live.charts)chart.update(samples,bounds,visibleMarks);
  if(live.errors) {
    const errors=Object.entries(failure).flatMap(([key,name])=>buildSeries({keys:{[key]:name}},samples,instance).map(line=>{
      const values=extrema(line);
      return [line.name,formatValue(values.last,"次/秒"),formatValue(values.max,"次/秒"),values.count];
    }));
    const signature=JSON.stringify(errors);
    if(signature!==live.errorRows) {
      live.errorRows=signature;
      live.errors.replaceChildren(errors.length?makeTable(["原因","最新速率","峰值速率","采样点"],errors):
        el("p","这个时间段暂无失败指标样本","empty"));
    }
  }

  function showMarks(visible) {
    const signature=JSON.stringify(visible);
    if(signature===live.visibleMarks)return;
    live.visibleMarks=signature;
    for(const chart of live.charts)chart.setMarkers(visible);
    if(selected)return;
    live.markHost.hidden=!visible.length;
    const list=el("section",null,"marker-list");
    for(const entry of visible) {
      const row=el("div",null,"row");
      row.append(el("span",timestamp(entry.time,true)+" · "+entry.label),
        button("查看附近时段","",()=>zoom({from:Math.max(0,entry.time-120),to:Math.min(now(),entry.time+120)})));
      if(entry.fields.recordId) {
        const result=el("span",null,"muted");
        row.append(button("查看实验记录","",async()=>{
          try {
            const record=await api("runs/"+entry.fields.recordId,null,live.queries.controller.signal);
            if(!row.isConnected)return;
            result.textContent="ACK P99 "+formatValue(record.result.ackP99Ms,"ms")+
              " · "+(record.result.ackSamples??"—")+" 样本";
          } catch(error) { if(row.isConnected)result.textContent=error.message; }
        }),result);
      }
      list.append(row);
    }
    live.markHost.replaceChildren(el("h2","实验与故障标注"),list);
  }

  showMarks(visibleMarks);
  live.queries.listen(states=>{
    if(!active())return;
    for(const [path,state] of states) {
      if(!state.loaded)continue;
      const signature=JSON.stringify([state.value,state.error]);
      if(signature===live.applied.get(path))continue;
      live.applied.set(path,signature);
      try {
        const value=state.value;
        if(path==="overview")showHealth(live.healthBadges,value);
        if(path==="version")setText(live.revision,sourceNames[health.source]+" · 运行版本 "+(value?.revision||"未知"));
        if(path==="alerts")live.alerts.replaceChildren(state.error?
          el("p","告警查询失败；不能据此判断没有告警。","empty"):alertPanel(value));
        if(path==="markers"&&!state.error) {
          live.marks=(value||[]).map(row=>({time:Number(BigInt(row.occurredAt)/1000000000n),
            label:row.fields.scenario+" · "+row.fields.state,fields:row.fields}));
          showMarks(marksInRange());
        }
      } catch(error) {
        state.error=error.message||"附加数据格式无效";
        if(path==="alerts")live.alerts.replaceChildren(el("p","告警数据格式无效；不能据此判断没有告警。","empty"));
      }
    }
    const waiting=[...states].filter(([,state])=>!state.loaded).map(([path])=>queries[path]);
    const errors=[...states].filter(([,state])=>state.error);
    live.metadata.hidden=!!selected&&!errors.length;
    live.background.dataset.state=errors.length?"warn":waiting.length?"loading":"ready";
    live.background.hidden=!waiting.length&&!errors.length;
    setText(live.backgroundText,[waiting.length?"指标查询已完成；附加查询中："+waiting.join("、"):"",
      errors.length?errors.map(([path,state])=>queries[path]+"查询失败 · "+state.error).join("；"):""].filter(Boolean).join("。 "));
    live.retry.hidden=!errors.length;
  });
  live.queries.refresh(bounds);
  return {sampleAt:latestStamp(samples),failed:[...samples.values()].filter(queryFailed).length,live,reused};
}

function mountDashboard(root,{key,view,selected,models,statModels,catalog,health,instance,zoom,focus,selectedPanel,visuals,api,queries}) {
  const element=el("div",null,"dashboard-view"), metadata=el("div",null,"dashboard-meta");
  const cards=el("div",null,"stat-grid"), panels=el("div",null,"panels"+(selected?" focus-panel":"")), tail=el("div");
  const available=el("div",null,"status-signals");
  const healthBadges=new Map(Object.entries(healthLabels).map(([key,label])=>{
    const row=el("div",null,"signal"), badge=el("span","查询中","stat-health unknown");
    row.append(el("span",label),badge);available.append(row);
    return [key,badge];
  }));
  const revision=el("p",sourceNames[health.source]+" · 运行版本正在查询…","muted");
  const background=el("div",null,"dashboard-background"), backgroundText=el("small");
  background.setAttribute("role","status");background.setAttribute("aria-live","polite");
  const loader=new DashboardQueries(api,queries);
  const retry=button("重试失败的附加查询","minimal",()=>loader.retry(live.bounds));
  background.append(backgroundText,retry);
  metadata.append(available,revision,background);
  available.hidden=!!selected;revision.hidden=!!selected;cards.hidden=!!selected;
  const stats=selected?[]:statModels.map(([label,,unit,color])=>card(label,unit,color));
  cards.append(...stats.map(stat=>stat.element));

  const context={catalog,source:health.source,instance,bus:new EventTarget(),zoom,focus,panel:selectedPanel,visuals};
  const charts=(selected?[selected]:models).map(model=>panel(model,context));
  panels.append(...charts.map(chart=>chart.element));
  const errors=!selected&&view==="service"?el("div"):null;
  if(errors)tail.append(el("h2","失败与拒绝"),
    el("p","这里按原因展示。业务 5xx 比例以共用业务适配器的响应为统计范围，可在告警规则中查看。","muted"),errors);
  const alerts=el("div"), markHost=el("section",null,"dashboard-markers");
  markHost.hidden=true;
  if(!selected) {
    const section=el("section",null,"dashboard-alerts");
    alerts.append(el("p","正在查询告警…","muted"));
    section.append(el("h2","告警"),alerts);
    tail.append(section,markHost);
  }
  element.append(metadata,cards,panels,tail);
  root.append(element);
  visuals.push({dispose:()=>loader.dispose()});
  const live={key,element,metadata,healthBadges,revision,background,backgroundText,retry,stats,charts,errors,
    alerts,markHost,marks:[],applied:new Map(),queries:loader};
  return live;
}
