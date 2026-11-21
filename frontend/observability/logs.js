function el(tag,text,cls) {
  const n=document.createElement(tag);if(text!=null)n.textContent=text;if(cls)n.className=cls;return n;
}
const validTrace=id=>/^[0-9a-f]{32}$/.test(id||"");
const logKey=row=>JSON.stringify([row.source,row.recordId]);

export async function renderLogs(root,{api,post,bounds,signal,visuals,now}) {
  const config=await api("workspace",null,signal);
  if(signal.aborted)return;
  const box=el("section",null,"panel log-workspace"), form=el("form",null,"log-filters"),
    source=el("select"),level=el("select"),component=el("input"),term=el("input"),trace=el("input"),
    follow=el("input"),summary=el("p",null,"muted"),status=el("p",null,"query-feedback"),
    wrap=el("div",null,"log-scroll"),table=el("table",null,"log-table"),body=el("tbody");
  const restored=new URL(location.href).searchParams;
  source.setAttribute("aria-label","日志数据源");
  source.append(new Option("本机 · 结构化日志","nebula"),...config.sources.filter(s=>s.type==="loki").map(s=>new Option(s.name,s.id)));
  const gatewaySource=config.gatewayLogSource;
  if(gatewaySource) source.add(new Option("本机 · 网关访问日志",gatewaySource),1);
  const localSource=()=>source.value==="nebula" || source.value===gatewaySource;
  level.setAttribute("aria-label","日志级别");level.append(new Option("全部级别",""),...["TRACE","DEBUG","INFO","WARN","ERROR","FATAL"].map(v=>new Option(v,v)));
  component.placeholder="组件（如 business）";component.setAttribute("aria-label","日志组件");component.maxLength=64;
  term.placeholder="消息、事件代码或请求编号";term.setAttribute("aria-label","日志关键词");term.maxLength=120;
  trace.placeholder="Trace ID（可选）";trace.setAttribute("aria-label","日志 Trace ID");trace.maxLength=32;
  trace.value=restored.get("logTrace")||"";level.value=restored.get("level")||"";
  const loki=el("input");loki.value='{service_name="nebula-webserver"}';loki.maxLength=1024;
  loki.setAttribute("aria-label","LogQL 查询");loki.hidden=true;
  follow.type="checkbox";follow.checked=!restored.has("from");
  const followLabel=el("label","实时跟随 ");followLabel.append(follow);
  const execute=el("button","查询日志"),exporter=el("button","导出当前日志 JSON"),older=el("button","加载更早日志");
  execute.type="submit";exporter.type=older.type="button";older.hidden=true;
  form.append(source,level,component,term,trace,loki,execute,followLabel,exporter);
  const head=el("thead"),tr=el("tr");
  for(const name of ["时间 / 级别","组件 / 进程:线程","事件与消息","耗时 / HTTP","关联与源码"])tr.append(el("th",name));
  head.append(tr);table.append(head,body);wrap.append(table);
  box.append(el("h2","日志工作台"),el("p","结构化输出 → 七天日志索引 → Trace 调用链。默认不采集正文、凭据和 SQL 参数。","muted"),form,summary,status,wrap,older);
  root.append(box);
  let revision=0,controller,items=[],next=null,lastBounds=bounds,busy=false;
  const rows=new Map();
  function renderRow(row) {
    const line=el("tr");line.dataset.logId=logKey(row);line.dataset.level=row.level;
    const at=el("td"),place=el("td"),message=el("td"),measure=el("td"),links=el("td");
    const stamp=Number(BigInt(row.timeUnixNano)/1000000n);
    at.append(el("time",new Date(stamp).toLocaleString("zh-CN",{hour12:false})+"."+String(stamp%1000).padStart(3,"0")),el("span",row.level,"log-level"));
    place.append(el("strong",row.component),el("small",row.source+" · "+(row.processId??"—")+":"+(row.threadId??"—"),"muted"));
    message.append(el("code",row.code),el("div",row.message),el("small",[row.operation,row.errorCode,row.errorType,row.requestId&&"请求 "+row.requestId].filter(Boolean).join(" · "),"muted"));
    measure.textContent=(row.durationUs!=null?(row.durationUs/1000).toFixed(3)+" ms":"—")+(row.status?" · HTTP "+row.status:"");
    if(row.component==="gateway") {
      const timings=[["连接",row.upstreamConnectUs],["响应头",row.upstreamHeaderUs],["响应",row.upstreamResponseUs]];
      const brief=timings.filter(entry=>entry[1]!=null).map(([label,us])=>label+" "+(us/1000).toFixed(3)+" ms");
      if(row.upstreamHeaderUs==null) brief.unshift("无上游响应头计时");
      else if(row.upstreamStatus) brief.unshift("上游 "+row.upstreamStatus);
      const outcomes={upstream_timeout:"上游超时",idle_timeout:"空闲超时",no_healthy_upstream:"无可用上游",
        route_not_found:"找不到路由",route_policy:"入口路由策略",rate_limited:"入口限流"};
      if(outcomes[row.gatewayOutcome])brief.unshift(outcomes[row.gatewayOutcome]);
      measure.append(el("small",brief.join(" · "),"muted"));
    }
    if(validTrace(row.traceId)) {
      const link=el("a","查看调用链 ↗");link.href="/?"+new URLSearchParams({view:"traces",trace:row.traceId,...lastBounds});
      links.append(link,el("small",row.traceId,"muted mono"));
    }
    if(row.sourceFile)links.append(el("small",row.sourceFile+(row.sourceLine?":"+row.sourceLine:""),"muted mono"));
    line.append(at,place,message,measure,links);return line;
  }
  function paint(replace) {
    if(replace){rows.clear();body.replaceChildren();}
    body.querySelector("tr[data-empty]")?.remove();
    for(const row of items)if(!rows.has(logKey(row))){const line=renderRow(row);rows.set(logKey(row),line);}
    const keep=new Set(items.map(logKey));
    for(const [id,line] of rows)if(!keep.has(id)){line.remove();rows.delete(id);}
    // Move existing rows only when ordering changed; live reads retain expanded selection and focus.
    for(let i=0;i<items.length;i++){const line=rows.get(logKey(items[i]));if(body.children[i]!==line)body.insertBefore(line,body.children[i]||null);}
    if(!items.length) {const tr=el("tr"),td=el("td","所选窗口无日志。请检查级别、业务流量和采集目标；未配置的服务不会自动出现日志。");tr.dataset.empty="1";td.colSpan=5;tr.append(td);body.append(tr);}
  }
  async function query({reset=false,more=false}={}) {
    if(busy&&!reset)return;
    controller?.abort();controller=new AbortController();const active=++revision;
    const currentSignal=AbortSignal.any([signal,controller.signal]);busy=true;
    status.textContent="正在读取日志…";
    try {
      if(trace.value&&!validTrace(trace.value))throw Error("Trace ID 需为 32 位十六进制字符。");
      const live=follow.checked&&!more;
      const end=now();const range=live?{from:end-(bounds.to-bounds.from),to:end}:more?lastBounds:bounds;
      let result;
      if(localSource()) result=await api("logs",{...range,source:source.value===gatewaySource?gatewaySource:"",level:level.value,component:component.value.trim(),q:term.value.trim(),trace:trace.value.trim(),limit:100,...(more&&next?{cursor:next}:{})},currentSignal);
      else {
        const raw=await post("investigate",{...range,source:source.value,expr:loki.value,trace:trace.value.trim()},currentSignal);
        result={items:raw.logs.map(row=>({recordId:row.time+JSON.stringify(row.labels),timeUnixNano:String(row.time),source:source.selectedOptions[0].text,
          level:row.labels.level||"INFO",component:row.labels.service_name||"loki",code:"loki.stream",message:row.line})),next:null,counts:{}};
        result.items=result.items.filter(row=>(!level.value||row.level===level.value)&&(!term.value||row.message.includes(term.value)));
      }
      if(signal.aborted||active!==revision)return;
      lastBounds=range;next=result.next;
      const merge=reset||(!live&&!more)?[]:items;
      const unique=new Map(merge.map(row=>[logKey(row),row]));for(const row of result.items)unique.set(logKey(row),row);
      items=[...unique.values()].sort((a,b)=>Number(BigInt(b.timeUnixNano)-BigInt(a.timeUnixNano))).slice(0,400);
      paint(reset);
      older.hidden=!next||items.length>=400;
      summary.textContent="当前显示 "+items.length+" 条（最多 400 条） · "+Object.entries(result.counts||{}).map(([level,count])=>level+" "+count).join(" / ")+
        " · 保留 "+(result.retentionDays||7)+" 天 · 队列丢弃 "+(result.pipeline?.dropped??0)+" · 文件拒收 "+(result.fileRejected??0)+
        (result.lastCleanupAt?" · 最近清理 "+new Date(result.lastCleanupAt*1000).toLocaleTimeString("zh-CN"):"");
      status.textContent=(follow.checked?"实时跟随，每 2 秒读取":"固定窗口")+" · 查询成功 "+new Date().toLocaleTimeString("zh-CN");status.dataset.state="ready";
      const url=new URL(location.href);if(trace.value)url.searchParams.set("logTrace",trace.value);else url.searchParams.delete("logTrace");history.replaceState(null,"",url);
    } catch(error) {
      if(!currentSignal.aborted&&active===revision){status.textContent="日志读取失败 · "+error.message;status.dataset.state="warn";}
    } finally {if(active===revision)busy=false;}
  }
  form.onsubmit=event=>{event.preventDefault();next=null;void query({reset:true});};
  source.onchange=()=>{loki.hidden=localSource();component.disabled=!localSource();void query({reset:true});};
  follow.onchange=()=>{if(follow.checked)void query({reset:true});};
  older.onclick=()=>{follow.checked=false;void query({more:true});};
  exporter.onclick=()=>{
    const url=URL.createObjectURL(new Blob([JSON.stringify({from:lastBounds.from,to:lastBounds.to,exportedAt:new Date().toISOString(),items},null,2)],{type:"application/json"}));
    const link=el("a");link.href=url;link.download="NEBULA-logs-"+lastBounds.to+".json";link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  };
  const timer=setInterval(()=>{if(follow.checked&&!signal.aborted&&!document.hidden)void query();},2000);
  visuals.push({dispose:()=>{clearInterval(timer);controller?.abort();}});
  await query({reset:true});
}
