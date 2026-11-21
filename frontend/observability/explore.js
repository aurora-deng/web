import {renderQueryEditor} from "./query_editor.js";
import {drawChart, formatValue, extrema, palette} from "./chart.js";
import {emptyState} from "./series.js";

function node(tag, content) {
  const element=document.createElement(tag);
  if (content!==undefined) element.textContent=content;
  return element;
}

export async function renderExplore(root,{api,bounds,signal,visuals,zoom,post}) {
  const catalog=new Map((await api("catalog",null,signal)).map(def=>[def.id,def]));
  const form=node("form"), select=node("select"), output=node("section");
  form.className="strip";
  select.setAttribute("aria-label","查询指标");
  for(const def of catalog.values()) select.append(new Option(def.title,def.id));
  const key=new URL(location.href).searchParams.get("metric");
  select.value=catalog.has(key)?key:"http";
  form.append(select,node("button","查询指标"));
  root.append(node("h2","指标目录查询"),form,output);
  let revision=0, chart;
  visuals.push({dispose:()=>chart?.dispose()});
  async function query() {
    const current=++revision, def=catalog.get(select.value);
    const url=new URL(location.href);
    url.searchParams.set("metric",def.id);
    history.replaceState(null,"",url);
    output.replaceChildren(node("p","正在查询…"));
    chart?.dispose();
    try {
      const payload=await api("series",{...bounds,panel:def.id},signal);
      if(current!==revision || signal.aborted) return;
      output.replaceChildren(node("h3",def.title),node("p",def.guide),
        node("p","采样步长 "+payload.step+" 秒 · 后端查询 "+payload.queryMs+" ms"));
      if(payload.status!=="ok") {
        const diagnostic=emptyState([payload]);
        output.append(node("p",diagnostic.message+" · "+diagnostic.guide));
        return;
      }
      const plot=node("div");
      plot.className="chart";
      output.append(plot);
      const series=payload.series.map((row,index)=>({...row, name:row.labels.instance || def.title,
        step:payload.step,color:palette[index%palette.length]}));
      chart=drawChart(plot,{model:{...def,area:true},bounds,series,markers:[],bus:new EventTarget(),zoom});
      for(const line of series) {
        const stat=extrema(line);
        output.append(node("p",line.name+" · "+stat.count+" 样本 · 平均 "+formatValue(stat.mean,def.unit)+
          " · 最小 "+formatValue(stat.min,def.unit)+" · 最大 "+formatValue(stat.max,def.unit)));
      }
      const details=node("details");
      details.append(node("summary","完整 Prometheus 查询表达式"),node("code",def.query));
      output.append(details);
    } catch(error) {
      if(current===revision && !signal.aborted) output.replaceChildren(node("p","指标查询失败 · "+error.message));
    }
  }
  form.onsubmit=event=>{event.preventDefault();query();};
  await Promise.all([query(),renderQueryEditor(root,{api,bounds,signal,visuals,zoom,post})]);
}
