import {formatValue, timestamp} from "./chart.js";
import {renderTraceDetails} from "./trace_details.js";

const validTrace=id=>/^[a-f0-9]{32}$/.test(id||"");

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null)node.textContent=text;
  if(cls)node.className=cls;
  return node;
}

function rememberFunction(key="",trace="",span="") {
  const url=new URL(location.href);
  for(const [name,value] of Object.entries({fn:key,call:trace,span})) {
    if(value)url.searchParams.set(name,value);
    else url.searchParams.delete(name);
  }
  history.replaceState(null,"",url);
}

export function createFunctionTraceView(root,{api,bounds,signal}) {
  let queryVersion=0, detailVersion=0;

  function clear() {
    ++queryVersion;
    ++detailVersion;
    root.replaceChildren();
    rememberFunction();
  }

  async function show(row,{trace:previous,span:previousSpan}={}) {
    const current=++queryVersion;
    ++detailVersion;
    const active=()=>current===queryVersion&&!signal.aborted;
    const trace=validTrace(previous)?previous:"";
    rememberFunction(row.key,trace,trace?previousSpan:"");
    const state=el("p","正在查询调用样本…","muted");
    state.setAttribute("role","status");
    const detail=el("div");
    const picker=el("select");
    const label=el("label","调用样本","builder-field");
    label.append(picker);
    label.hidden=true;
    root.replaceChildren(el("h3",row.symbol+" · 单次调用"),state,label,detail);

    async function open(id,spanId,source,preferred="") {
      const revision=++detailVersion;
      const latest=()=>active()&&revision===detailVersion;
      rememberFunction(row.key,id,preferred||spanId);
      detail.replaceChildren(el("p","正在加载 Trace…","muted"));
      try {
        const data=await api("traces/"+id,null,signal);
        if(!latest())return;
        const selected=data.spans.find(span=>span.id===spanId&&span.service==="nebula-webserver"&&span.name===row.symbol)
          ||data.spans.find(span=>span.service==="nebula-webserver"&&span.name===row.symbol);
        if(!selected) {
          const message={missing:"这条 Trace 已过期或尚未收录。",unavailable:"追踪数据源暂不可用。"}[data.status]
            ||"已收到的 span 尚未包含这个函数。";
          detail.replaceChildren(el("p",message,"empty"));
          return;
        }
        const full=el("a","在链路追踪中打开");
        full.href="/?"+new URLSearchParams({view:"traces",...bounds,trace:id,span:selected.id});
        detail.replaceChildren(full);
        renderTraceDetails(detail,data.spans,{traceId:id,partial:data.status==="partial",
          selectedSpan:data.spans.some(span=>span.id===preferred)?preferred:selected.id,
          traceSource:source==="local"?"nebula":"",
          onSelect:({id:currentSpan})=>rememberFunction(row.key,id,currentSpan)});
      } catch(error) {
        if(latest())detail.replaceChildren(el("p","Trace 加载失败 · "+error.message,"notice"));
      }
    }

    let report;
    try {
      report=await api("traces",{...bounds,function:row.symbol,limit:12},signal);
    } catch(error) {
      if(!active())return;
      state.textContent="调用样本查询失败 · "+error.message;
      if(trace)await open(trace,"","",previousSpan);
      return;
    }
    if(!active())return;
    const samples=report.items.filter(item=>validTrace(item.traceId));
    if(trace&&!samples.some(item=>item.traceId===trace))samples.unshift({traceId:trace});
    if(!samples.length) {
      state.textContent=report.source==="unconfigured"?"当前未连接追踪数据源。"
        :"所选时间范围没有采到这个函数的 span。执行对应操作后刷新，或扩大时间范围。";
      if(row.key==="send"&&report.source!=="unconfigured")root.append(el("p",
        "若通过 WebSocket 发送，请确认服务启用了聊天 Trace 采样。","muted"));
      return;
    }
    for(const sample of samples) {
      const when=sample.startedAt?timestamp(Number(sample.startedAt)/1e9,true):"链接指定的 Trace";
      const duration=sample.durationMs==null?"":formatValue(sample.durationMs,"ms")+" · ";
      picker.append(new Option(when+" · "+duration+sample.traceId.slice(0,12),sample.traceId));
    }
    label.hidden=false;
    picker.value=trace||samples[0].traceId;
    state.textContent="最多列出 12 条匹配样本。选择一条查看父子调用、时间和关联数据。";
    const choose=(preferred="")=>{
      if(!active())return;
      const sample=samples.find(item=>item.traceId===picker.value);
      if(sample) return open(sample.traceId,sample.spanId||"",report.source,preferred);
    };
    picker.onchange=()=>choose();
    await choose(previousSpan);
  }

  return {show,clear};
}
