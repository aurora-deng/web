import {drawChart, palette, timestamp, formatValue} from "./chart.js";
import {createFunctionTraceView} from "./function_traces.js";
import {readSeries,emptyState} from "./series.js";

const domains={chat:"聊天",accounts:"账号",tree:"苦情树",ranking:"排名",ai:"AI / RAG",speech:"语音",files:"文件",database:"社区数据库"};
const metrics={rate:["调用速率","次/秒"],avg:["平均墙钟","ms"],p95:["P95","ms"],p99:["P99","ms"],
  error_rate:["失败速率","次/秒"],in_flight:["执行中","项"]};

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null)node.textContent=text;
  if(cls)node.className=cls;
  return node;
}
const latest=line=>line?.points.findLast(point=>point[1]!=null);
const lastPoint=(report)=>{
  let selected;
  for(const series of report?.series||[]) {
    const value=latest(series);
    if(value&&(!selected||value[0]>selected.at))selected={at:value[0],series};
  }
  return selected;
};

export async function renderFunctionPerformance(root,{api,bounds,signal,visuals,zoom,batch}) {
  const box=el("section",null,"panel builder-box");
  box.append(el("h2","NEBULA 业务函数 · 墙钟测量"),
    el("p","用趋势看一段时间的分布，用调用链看某一次函数执行。","muted"));
  root.append(box);
  const [instrument,catalog]=await Promise.all([
    api("instrumentation",bounds,signal).catch(error=>({status:"unavailable",guide:error.message})),
    api("function-catalog",null,signal),
  ]);
  const functions=catalog.functions;
  if(signal.aborted)return;
  const target=el("div",null,"instrumentation-status");
  target.dataset.state=instrument.available?"ready":"warn";
  target.append(el("strong","当前目标："+(instrument.target?.instance||(instrument.target?.multiple?"全部实例":"外部数据源"))),
    el("p",instrument.available?"已收到 nebula_function_seconds。下表只显示所选窗口真实调用的耗时。":
      "当前目标未提供函数耗时数据；主机 CPU 有数据并不代表已采到函数耗时。"),el("p",instrument.guide||"","muted"));
  if(Array.isArray(instrument.observedFunctions))target.append(el("p",
    "当前服务提供 "+instrument.observedFunctions.length+" / "+functions.length+" 个目录埋点；未启用与没有调用分别显示。","muted"));
  if(instrument.relatedObserver&&!instrument.available) {
    const link=el("a","打开已埋点的隔离实例 · 查看真实函数耗时 ↗");
    link.href=instrument.relatedObserver+"/?"+new URLSearchParams({view:"profiles",...bounds});
    target.append(link,el("p","此入口是独立测试实例，其结果不代表上面的日常目标。","muted"));
  }
  box.append(target);
  const panelId=(key,metric)=>"function_"+key+"_"+metric;
  const metricKeys=Object.keys(metrics);
  const samples=await readSeries(api,functions.flatMap(({key})=>metricKeys.map(metric=>panelId(key,metric))),
                                 bounds,signal,batch);
  if(signal.aborted)return;
  const rows=functions.map(({key,title,symbol,domain})=>{
    const reports=metricKeys.map(metric=>samples.get(panelId(key,metric)));
    const scoped=reports.map(report=>report?{...report,series:report.series.filter(line=>
      !instrument.target?.instance||line.labels.instance===instrument.target.instance)}:report);
    const identity=lastPoint(scoped[3])||lastPoint(scoped[1])||lastPoint(scoped[0])||lastPoint(scoped[5]);
    const instance=identity?.series.labels?.instance;
    const series=Object.fromEntries(Object.keys(metrics).map((metric,i)=>[metric,reports[i]?.series.find(line=>line.labels.instance===instance)]));
    return {key,title,symbol,domain,instance,series,reports};
  });
  const toolbar=el("div",null,"strip"), filter=el("input"), order=el("select");
  const domain=el("select");domain.setAttribute("aria-label","业务域");
  domain.append(new Option("全部业务",""),...Object.entries(domains).map(([key,title])=>new Option(title,key)));
  filter.placeholder="搜索业务动作或函数名";filter.setAttribute("aria-label","搜索业务函数");
  order.setAttribute("aria-label","函数排序");
  order.append(new Option("按 P99 排序","p99"),new Option("按 P95 排序","p95"),
    new Option("按平均耗时排序","avg"),new Option("按调用速率排序","rate"));
  toolbar.append(domain,filter,order);
  const wrap=el("div",null,"table-wrap"), table=el("table"), header=el("thead"), head=el("tr"), body=el("tbody");
  table.className="function-stats";
  head.append(...["业务动作 / 函数","调用速率","平均墙钟","P95","P99","失败速率","执行中","最近延迟观测","操作"].map(name=>el("th",name)));
  header.append(head);table.append(header,body);wrap.append(table);
  const detail=el("section",null,"function-trend");
  const tracePanel=el("section",null,"function-traces");
  const lookup=el("a","查找其他已采样的函数");
  lookup.href="/?"+new URLSearchParams({view:"traces",...bounds});
  const traces=createFunctionTraceView(tracePanel,{api,bounds,signal});
  const locationParams=new URL(location.href).searchParams;
  const restored=rows.find(row=>row.key===locationParams.get("fn"));
  let chart=null,selected=null;
  const bus=new EventTarget();
  visuals.push({paint:()=>chart?.paint?.(),dispose:()=>chart?.dispose()});
  const reading=(row,metric)=>latest(row.series[metric])?.[1];
  function trend(row) {
    selected=row;
    chart?.dispose();chart=null;
    detail.replaceChildren(el("h3",row.title+" · 延迟趋势"),el("p",row.symbol+(row.instance?" · "+row.instance:""),"muted"));
    const curves=["avg","p95","p99"].flatMap((metric,index)=>{
      const source=row.series[metric];
      if(!latest(source))return [];
      return [{name:metrics[metric][0],color:palette[index],points:source.points,step:row.reports[index+1].step}];
    });
    if(curves.length) {
      const view=el("div",null,"chart"), legend=el("div",null,"legend");
      for(const trace of curves) {
        const label=el("span",trace.name,"legend-item");label.style.setProperty("--series",trace.color);legend.append(label);
      }
      detail.append(view,legend);
      chart=drawChart(view,{model:{title:row.title,unit:"ms"},bounds,series:curves,markers:[],bus,zoom});
      chart.paint?.();
    } else {
      const diagnostic=emptyState(row.reports);
      detail.append(el("p",diagnostic.message+" · "+diagnostic.guide,"empty"));
    }
    const link=el("a","在指标探索中打开 P99");
    link.href="/?"+new URLSearchParams({view:"explore",metric:"function_"+row.key+"_p99",...bounds});
    detail.append(link);
  }
  function repaint() {
    const sorted=[...rows].sort((a,b)=>(reading(b,order.value)??-1)-(reading(a,order.value)??-1));
    const term=filter.value.trim().toLowerCase();
    const matched=sorted.filter(row=>(!domain.value||domain.value===row.domain)&&
      (row.title+" "+row.symbol).toLowerCase().includes(term));
    if(!matched.includes(selected)) {
      if(selected)traces.clear();
      if(matched.length)trend(matched.find(row=>reading(row,order.value)!=null)||matched[0]);
      else {selected=null;chart?.dispose();chart=null;detail.replaceChildren();}
    }
    body.replaceChildren();
    for(const row of matched) {
      const line=el("tr"), name=el("td"), button=el("button","看趋势");
      line.dataset.domain=row.domain;line.dataset.function=row.key;
      name.append(el("strong",row.title),el("small",(domains[row.domain]||row.domain)+" · "+row.symbol,"muted"));
      button.type="button";button.onclick=()=>{
        if(row!==selected)traces.clear();
        trend(row);repaint();
      };
      button.setAttribute("aria-pressed",String(row===selected));
      const active=latest(row.series.p99)||latest(row.series.avg);
      const cells=Object.keys(metrics).map(metric=>el("td",formatValue(reading(row,metric),metrics[metric][1])));
      const action=el("td",null,"function-actions"), trace=el("button","看调用链");
      trace.type="button";
      trace.onclick=()=>{
        if(row!==selected)trend(row);
        repaint();
        void traces.show(row);
        tracePanel.scrollIntoView({block:"start"});
      };
      action.append(button,trace);
      const diagnostic=emptyState(row.reports);
      const observation=el("td",active?timestamp(active[0],true):diagnostic.message);
      if(!active)observation.title=diagnostic.guide;
      line.append(name,...cells,observation,action);
      body.append(line);
    }
    if(!matched.length) {
      const line=el("tr"), cell=el("td","未匹配到函数。");cell.colSpan=9;
      line.append(cell);body.append(line);
    }
  }
  box.append(toolbar,wrap,detail,tracePanel,lookup,
    el("p","函数墙钟包含子调用和等待。失败包含业务拒绝，不等同 HTTP 5xx；取消和超时按独立结果记录。异步模型计到最终结果。文件下载只计授权与定位，不能当作网络传输时间。","muted"),
    el("p","线程 CPU、指令量和堆分配可继续在下方报告查看。","muted"));
  if(restored)trend(restored);
  filter.oninput=order.onchange=domain.onchange=repaint;repaint();
  if(restored)await traces.show(restored,{trace:locationParams.get("call"),span:locationParams.get("span")});
}
