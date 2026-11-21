import {fromOtlp, renderTraceDetails} from "./trace_details.js";
/* Trace waterfall, service dependencies and trace-correlated Loki queries. */
const labels=["cluster","namespace","service","pod","instance","node"];
function node(tag,text,className) {
  const n=document.createElement(tag);
  if(text!=null)n.textContent=text;
  if(className)n.className=className;
  return n;
}
function select(options) {
  const field=node("select");
  for(const [key,label] of options) field.append(new Option(label,key));
  return field;
}
function field(text,control) {
  const label=node("label",text,"builder-field");label.append(control);return label;
}
function table(root,columns,rows) {
  const wrap=node("div",null,"table-wrap"), t=node("table"), header=node("tr"), body=node("tbody"), top=node("thead");
  columns.forEach(c=>header.append(node("th",c)));top.append(header);
  for(const row of rows) {
    const line=node("tr");
    for(const value of row) {
      const cell=node("td");
      if(value instanceof Node) cell.append(value); else cell.textContent=value;
      line.append(cell);
    }
    body.append(line);
  }
  t.append(top,body);wrap.append(t);root.append(wrap);
}
const nano=value=>BigInt(value || "0");

export async function renderDistributed(root,{api,bounds,signal,post}) {
  const workspace=await api("workspace",null,signal);
  const box=node("section",null,"panel builder-box");
  const tempo=workspace.sources.filter(s=>s.type==="tempo"||s.type==="jaeger"), loki=workspace.sources.filter(s=>s.type==="loki");
  if(workspace.internalTrace) tempo.unshift({id:"nebula",name:"当前 NEBULA 链路"});
  const make=records=>select([["","请选择已连接的数据源"],...records.map(s=>[s.id,s.name])]);
  const traceSource=make(tempo),logSource=make(loki),trace=node("input"),expr=node("textarea"), log=node("textarea");
  trace.placeholder="32 位 Trace ID";trace.maxLength=32;
  trace.value=new URL(location.href).searchParams.get("incident")||"";
  const initialSource=new URL(location.href).searchParams.get("traceSource");
  if(tempo.some(source=>source.id===initialSource)) traceSource.value=initialSource;
  else if(workspace.internalTrace) traceSource.value="nebula";
  expr.placeholder='{ resource.service.name = "checkout" && status = error }';
  log.placeholder='{namespace="production",app="checkout"}';
  expr.rows=2;log.rows=2;expr.maxLength=1024;log.maxLength=1024;
  const result=node("div"), logs=node("div"), traceStatus=node("p",null,"muted"), logStatus=node("p",null,"muted");
  const action=(title,status,fn)=>{
    const b=node("button",title);b.type="button";
    b.onclick=async()=>{b.disabled=true;status.textContent="正在查询…";
      try {await fn();status.textContent="查询完成";}
      catch(error){if(!signal.aborted)status.textContent=error.message;}
      finally{b.disabled=false;}
    };return b;
  };
  let context=null;
  async function open() {
    if(!traceSource.value||!trace.value.trim()) throw new Error("先选择链路数据源并填写 Trace ID");
    const ident=trace.value.trim();
    const data=await post("investigate",{...bounds,source:traceSource.value,trace:ident},signal);
    if(signal.aborted)return;
    result.replaceChildren();
    const url=new URL(location.href);
    url.searchParams.set("incident",ident);
    url.searchParams.set("traceSource",traceSource.value);
    history.replaceState(null,"",url);
    const spanWindow=renderTraceDetails(result,fromOtlp(data),{traceId:ident,
      traceSource:traceSource.value,partial:[2,"PARTIAL","STATUS_PARTIAL"].includes(data.status),
      onSelect:selection=>{
        context={...selection,ident};
        const link=new URL(location.href);link.searchParams.set("span",selection.id);history.replaceState(null,"",link);
      }});
    if(spanWindow) {
      const link=node("a","查看同一时段的业务指标");
      link.href="/?"+new URLSearchParams({view:"service",from:spanWindow.from,to:spanWindow.to});
      const resources=node("a",traceSource.value==="nebula"?"查看同一时段的主机与进程":"查看 Observer 本机资源");
      resources.href="/?"+new URLSearchParams({view:"resources",from:spanWindow.from,to:spanWindow.to});
      const related=node("div",null,"strip");
      related.append(link,resources);
      result.prepend(related);
    }
  }
  const errors=node("input");errors.type="checkbox";
  const traceButtons=node("div",null,"strip");
  traceButtons.append(action("搜索链路",traceStatus,async()=>{
    if(!traceSource.value) throw new Error("先选择链路数据源");
    const data=await post("investigate",{...bounds,source:traceSource.value,expr:expr.value.trim(),failed:errors.checked},signal);
    result.replaceChildren();
    if(!data.traces?.length) {result.append(node("p","当前时间范围内没有匹配链路","empty"));return;}
    const slow=data.traces.slice(0,30).sort((a,b)=>b.durationMs-a.durationMs);
    table(result,["根服务 / 操作（本页按耗时排序）","耗时","Trace ID",""],slow.map(row=>[
      (row.rootServiceName||"未知")+" · "+(row.rootTraceName||"未知"),
      row.durationMs+" ms",row.traceID,action("下钻",traceStatus,async()=>{trace.value=row.traceID.padStart(32,"0");await open();})]));
  }),action("查看 Trace",traceStatus,open));
  function sync() {
    expr.disabled=traceSource.value==="nebula";
    errors.disabled=!expr.disabled;
    expr.placeholder=tempo.find(s=>s.id===traceSource.value)?.type==="jaeger"?"填写准确服务名，例如 checkout":
      '{ resource.service.name = "checkout" && status = error }';
  }
  traceSource.onchange=()=>{context=null;sync();};sync();
  const connections=node("a","管理数据源");connections.href="/?view=sources";
  box.append(node("h2","分布式排障"),node("p","用 Tempo 查错误和慢请求，按服务、节点与 Pod 拆分，再用同一 Trace ID 查 Loki 日志。","muted"),
    connections,field("链路数据源",traceSource),field("Tempo TraceQL / Jaeger 服务名称",expr),field("仅错误（内置）",errors),field("Trace ID",trace),traceButtons,traceStatus,result);
  const logBox=node("section",null,"panel builder-box");
  logBox.id="trace-logs";
  const queryLogs=action("查询关联日志",logStatus,async()=>{
      if(!logSource.value||!log.value.trim()) throw new Error("先选择 Loki 并填写日志流选择器");
      const active=context?.ident===trace.value.trim() ? {from:context.from,to:context.to} : bounds;
      const data=await post("investigate",{...active,source:logSource.value,expr:log.value.trim(),trace:trace.value.trim()},signal);
      logs.replaceChildren();
      if(!data.logs?.length) {logs.append(node("p","无匹配日志。确认应用日志含 Trace ID，并核对选定时间。","empty"));return;}
      table(logs,["时间","节点 / 服务","日志"],data.logs.map(row=>[
        new Date(Number(nano(row.time)/1000000n)).toLocaleString("zh-CN"),
        labels.filter(key=>row.labels[key]).map(key=>key+"="+row.labels[key]).join(" · "),
        row.line]));
    });
  logBox.append(node("h2","关联日志"));
  if(!loki.length) {
    const configure=node("a","连接 Loki 数据源");configure.href="/?view=sources";
    logBox.append(node("p","当前未连接 Loki。日志需包含对应的 Trace ID 才能关联。","notice"),configure);
    queryLogs.disabled=true;
  } else if(loki.length===1) logSource.value=loki[0].id;
  logBox.append(field("Loki 数据源",logSource),field("LogQL",log),queryLogs,logStatus,logs);
  root.append(box,logBox);
  if(traceSource.value && trace.value) {
    try {await open();} catch(error){if(!signal.aborted)traceStatus.textContent=error.message;}
  }
}
