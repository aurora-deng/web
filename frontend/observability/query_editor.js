import {drawChart,palette,extrema,formatValue} from "./chart.js";

const presets={http:["HTTP 路由延迟","nebula_http_handler_seconds","route"],
  function:["C++ 函数延迟","nebula_function_seconds","function"],chat:["聊天处理阶段","nebula_chat_stage_seconds","stage"]};
const values={http:["conversations","history"],function:["ChatService::conversations","ChatService::history","ChatService::historyBefore","ChatService::sendMessage"],
  chat:["queue","pool","before_commit","commit","ack_ready"]};
function el(tag,text,cls){const n=document.createElement(tag);if(text!=null)n.textContent=text;if(cls)n.className=cls;return n;}
function expression(group,q,value="") {
  const [,family,label]=presets[group];
  return `histogram_quantile(${q}, sum by (le, ${label}) (rate(${family}_bucket${value?`{${label}="${value}"}`:""}[5m])))`;
}
function legend(template,labels,fallback) {
  return template.trim()?template.replace(/\{\{([\w.]+)\}\}/g,(_,key)=>labels[key]??"—"):
    labels.route||labels.function||labels.stage||labels.instance||fallback;
}

export async function renderQueryEditor(root,{api,post,bounds,signal,visuals,zoom}) {
  const config=await api("workspace",null,signal);if(signal.aborted)return;
  const box=el("section",null,"panel query-workbench"),bar=el("div",null,"strip"),rows=el("div"),
    output=el("div",null,"query-results"),add=el("button","＋ 添加查询"),run=el("button","运行全部查询");
  add.type=run.type="button";bar.append(add,run);
  box.append(el("h2","Explore · PromQL 与直方图"),el("p","Builder 构造延迟查询，Code 编辑表达式。完整 PromQL 使用外部 Prometheus；本机支持已登记的直方图表达式。","muted"),bar,rows,output);
  root.append(box);
  let models=[],version=0,controller;const charts=new Map();
  const dispose=id=>{charts.get(id)?.dispose();charts.delete(id);};
  const button=(text,action)=>{const b=el("button",text);b.type="button";b.onclick=action;return b;};
  function append(copy={}) {
    if(models.length>=4)return;
    const ref=[..."ABCD"].find(name=>!models.some(m=>m.ref===name));
    const model={ref,source:"nebula",group:"http",q:"0.99",value:"",expr:expression("http","0.99"),mode:"builder",visible:true,
      legend:"",format:"series",step:"",queryType:"range",...copy,ref};
    models.push(model);renderRow(model);add.disabled=models.length>=4;
  }
  function renderRow(model) {
    const row=el("section",null,"query-row"),header=el("div",null,"query-row-header"),source=el("select"),
      builder=button("Builder",()=>mode("builder")),code=button("Code",()=>mode("code")),editor=el("textarea"),
      controls=el("div",null,"query-builder"),group=el("select"),percentile=el("select"),target=el("select"),
      explanation=el("p",null,"muted"),options=el("details",null,"query-options"),body=el("div",null,"query-row-body");
    row.dataset.queryRef=model.ref;
    source.setAttribute("aria-label",model.ref+" 数据源");
    source.append(new Option("NEBULA · 本机延迟桶","nebula"),...config.sources.filter(s=>s.type==="prometheus").map(s=>new Option(s.name,s.id)));
    if(![...source.options].some(o=>o.value===model.source))model.source="nebula";
    source.value=model.source;source.onchange=()=>{model.source=source.value;explain();};
    editor.value=model.expr;editor.rows=3;editor.maxLength=1024;editor.spellcheck=false;editor.className="promql-editor";
    editor.setAttribute("aria-label",model.ref==="A"?"PromQL 表达式":model.ref+" PromQL 表达式");editor.oninput=()=>{model.expr=editor.value;explain();};
    group.setAttribute("aria-label",model.ref+" 指标族");
    group.append(...Object.entries(presets).map(([key,p])=>new Option(p[0],key)));group.value=model.group;
    percentile.setAttribute("aria-label",model.ref+" 分位数");percentile.append(new Option("P99","0.99"),new Option("P95","0.95"),new Option("P50","0.5"));percentile.value=model.q;
    target.setAttribute("aria-label",model.ref+" 标签筛选");
    function targets(){target.replaceChildren(new Option("全部 "+presets[group.value][2],""),...values[group.value].map(v=>new Option(v,v)));target.value=model.value;}
    function build(){model.group=group.value;model.q=percentile.value;model.value=target.value;model.expr=expression(model.group,model.q,model.value);editor.value=model.expr;explain();}
    group.onchange=()=>{model.value="";targets();build();};percentile.onchange=target.onchange=build;targets();
    controls.append(group,percentile,target);
    function explain(){explanation.textContent=model.source==="nebula"?"本机：五分钟增量桶估计分位数，单位为秒；无调用时不产生延迟样本。":"Prometheus 执行完整表达式；Range 返回区间，Instant 在窗口结束时求值。";}
    function mode(value){model.mode=value;controls.hidden=value!=="builder";editor.readOnly=value==="builder";builder.setAttribute("aria-pressed",String(value==="builder"));code.setAttribute("aria-pressed",String(value==="code"));}
    const hidden=button(model.visible?"隐藏":"显示",()=>{model.visible=!model.visible;hidden.textContent=model.visible?"隐藏":"显示";row.dataset.hidden=String(!model.visible);if(!model.visible){dispose(model.ref);output.querySelector('[data-result-ref="'+model.ref+'"]')?.remove();}});
    hidden.setAttribute("aria-label",model.ref+" 显隐查询");
    const duplicate=button("复制",()=>append({...model}));duplicate.setAttribute("aria-label",model.ref+" 复制查询");
    const remove=button("删除",()=>{++version;controller?.abort();models=models.filter(m=>m!==model);dispose(model.ref);row.remove();output.querySelector('[data-result-ref="'+model.ref+'"]')?.remove();add.disabled=false;});
    remove.setAttribute("aria-label",model.ref+" 删除查询");
    const execute=button("运行查询",()=>executeQueries([model]));
    header.append(el("strong",model.ref,"query-reference"),source,el("span","查询编辑器","muted"),builder,code,execute,duplicate,hidden,remove);
    options.append(el("summary","Options · 图例、格式、步长与查询类型"));
    const settings=el("div",null,"strip"),name=el("input"),format=el("select"),step=el("input"),type=el("select");
    name.placeholder="图例，如 {{function}}";name.value=model.legend;name.maxLength=120;name.setAttribute("aria-label",model.ref+" 图例");name.oninput=()=>model.legend=name.value;
    format.append(new Option("Time series","series"),new Option("Table","table"),new Option("Histogram","histogram"));format.value=model.format;
    format.setAttribute("aria-label",model.ref+" 结果格式");format.onchange=()=>model.format=format.value;
    step.type="number";step.min=5;step.max=3600;step.placeholder="步长 auto（秒）";step.value=model.step;step.setAttribute("aria-label",model.ref+" 查询步长");step.oninput=()=>model.step=step.value;
    type.append(new Option("Range · 区间","range"),new Option("Instant · 瞬时","instant"));type.value=model.queryType;type.setAttribute("aria-label",model.ref+" 查询类型");type.onchange=()=>model.queryType=type.value;
    settings.append(name,format,step,type);options.append(settings);body.append(controls,editor,explanation,options);row.append(header,body);rows.append(row);mode(model.mode);explain();
  }
  function showResult(model,data,section) {
    section.append(el("h3",model.ref+" · 查询结果"),el("p",`${data.queryType||model.queryType} · 步长 ${data.step} 秒 · 后端 ${data.queryMs} ms · ${data.truncated?"结果已限额":"有界查询"}`,"muted"));
    if(data.status!=="ok"){section.append(el("p",data.diagnostic?.guide||"所选窗口无有效样本。核对目标埋点、调用和采集权限。","empty"));return;}
    const unit=data.unit||"",series=data.series.map((row,i)=>({...row,name:legend(model.legend,row.labels,model.ref),color:palette[i%palette.length],step:data.step}));
    if(model.format==="series"){
      const plot=el("div",null,"chart");section.append(plot);
      const chart=drawChart(plot,{model:{title:model.ref+" 查询",unit,area:false},bounds,series,markers:[],bus:new EventTarget(),zoom});charts.set(model.ref,chart);chart.paint?.();
    } else if(model.format==="table") {
      const wrap=el("div",null,"table-wrap"),table=el("table"),head=el("thead"),tr=el("tr"),body=el("tbody");
      tr.append(...["序列","时间","数值"].map(v=>el("th",v)));head.append(tr);let count=0;
      for(const line of series)for(const [stamp,value] of line.points.slice().reverse())if(count++<600){const row=el("tr");row.append(el("td",line.name),el("td",new Date(stamp*1000).toLocaleString("zh-CN")),el("td",formatValue(value,unit)));body.append(row);}
      table.append(head,body);wrap.append(table);section.append(wrap,el("p","表格最多展示 600 个实际点。","muted"));
    }
    for(const line of series){const stat=extrema(line);section.append(el("p",line.name+" · "+stat.count+" 点 · 最新 "+formatValue(stat.last,unit)));}
    if(model.format==="histogram")for(const line of series){
      if(!line.histogram){section.append(el("p",line.name+"：该数据源只返回计算后的时间序列，没有原始桶。可改用 Table 或 Time series。","muted"));continue;}
      const bars=el("div",null,"histogram-rows");section.append(el("h4",line.name+" · "+line.histogram.count+" 个观测"));let lower=0;
      for(const [upper,count] of line.histogram.buckets){const row=el("div",null,"histogram-bin"),meter=el("meter");meter.max=Math.max(1,line.histogram.count);meter.value=count;row.append(el("span",lower+"–"+(upper==="+Inf"?"∞":upper*1000)+" ms"),meter,el("span",count));bars.append(row);if(upper!=="+Inf")lower=upper*1000;}
      section.append(bars);
    }
    section.append(el("code",model.expr));
  }
  async function executeQueries(selected=models) {
    controller?.abort();controller=new AbortController();const current=++version;
    const activeSignal=AbortSignal.any([signal,controller.signal]);
    await Promise.all(selected.filter(m=>m.visible).map(async model=>{
      dispose(model.ref);output.querySelector('[data-result-ref="'+model.ref+'"]')?.remove();
      const section=el("section",null,"query-result");section.dataset.resultRef=model.ref;section.append(el("p",model.ref+" · 正在查询…"));output.append(section);
      try {
        if(!model.expr.trim()||model.expr.length>1024)throw Error("表达式长度需介于 1 至 1024 字符。");
        if(model.step!==""&&(!/^\d+$/.test(model.step)||Number(model.step)<5||Number(model.step)>3600))throw Error("步长需为 5 至 3600 秒。");
        const data=await post("query",{...bounds,source:model.source,expr:model.expr,queryMode:"promql",queryType:model.queryType,...(model.step?{stepSeconds:Number(model.step)}:{})},activeSignal);
        if(activeSignal.aborted||current!==version||!models.includes(model)||!model.visible)return;
        section.replaceChildren();showResult(model,data,section);
      }catch(error){if(!activeSignal.aborted&&current===version)section.replaceChildren(el("p",model.ref+" · 查询失败："+error.message,"error"));}
    }));
  }
  add.onclick=()=>append();run.onclick=()=>executeQueries();
  visuals.push({paint:()=>{for(const chart of charts.values())chart.paint?.();},dispose:()=>{controller?.abort();for(const chart of charts.values())chart.dispose();charts.clear();}});
  append();
}
