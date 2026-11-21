import {drawChart, extrema, formatValue, palette, timestamp} from "./chart.js";
import {templates, importDashboard} from "./dashboard_templates.js";

const kinds={nebula:"NEBULA 内置",prometheus:"Prometheus API",metrics:"Prometheus 文本 /metrics",http:"HTTP 可用性",tempo:"Tempo 链路",
  loki:"Loki 日志",jaeger:"Jaeger v3 链路",pyroscope:"Pyroscope 持续剖析"};
const views={line:"折线",area:"面积",stat:"数值",gauge:"仪表",table:"表格"};
const colors=Object.fromEntries(["green","blue","yellow","orange","purple","pink"].map((key,i)=>[key,palette[i]]));
const uid=()=>new URL(location.href).searchParams.get("board");
const emptyPanel=()=>({title:"新面板",source:"nebula",view:"line",color:"green",width:1,unit:"",max:100,queries:[{expr:"http",alias:""}]});
const clone=value=>JSON.parse(JSON.stringify(value));
const filters=new Map();
const resourceLabels=["bc","app","cluster","namespace","service","pod","node","instance","job"];
const recent=new Map();

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null) node.textContent=text;
  if(cls) node.className=cls;
  return node;
}
function btn(text,action) {
  const button=el("button",text);
  button.type="button";
  button.onclick=action;
  return button;
}
function input(value="",max=100) {
  const field=el("input");
  field.value=value;field.maxLength=max;return field;
}
function field(label,control) {
  const wrap=el("label",null,"builder-field");
  wrap.append(el("span",label),control);return wrap;
}
function select(values,current) {
  const node=el("select");
  for(const [value,label] of values) node.append(new Option(label,value));
  node.value=current;
  if(node.selectedIndex<0 && node.options.length) node.selectedIndex=0;
  return node;
}
function section(title) {
  const box=el("section",null,"panel builder-box");
  box.append(el("h2",title));
  return box;
}
function rows(head,values) {
  const table=el("table"), top=el("thead"), heading=el("tr"), body=el("tbody");
  heading.append(...head.map(value=>el("th",value)));top.append(heading);
  for(const row of values) {
    const line=el("tr");line.append(...row.map(value=>el("td",value)));body.append(line);
  }
  table.append(top,body);
  const scroll=el("div",null,"table-wrap");
  scroll.append(table);return scroll;
}
function dialog(title) {
  const modal=el("dialog",null,"builder-dialog"), head=el("header",null,"drawer-header"), content=el("div",null,"builder-modal");
  head.append(el("h2",title),btn("关闭",()=>modal.close()));
  modal.append(head,content);
  modal.addEventListener("close",()=>modal.remove(),{once:true});
  document.body.append(modal);
  modal.showModal();
  return {modal,content};
}
async function post(route,value,signal) {
  const response=await fetch("/api/v1/workspace/"+route,{method:"POST",headers:{
    "Content-Type":"application/json","X-Observer-Workspace":"1"},body:JSON.stringify(value),
    signal:signal ? AbortSignal.any([signal,AbortSignal.timeout(12000)]) : AbortSignal.timeout(12000)});
  const payload=await response.json();
  if(!response.ok) throw new Error(payload.error || "请求失败");
  return payload;
}
function feedback(status,action) {
  return async event=>{
    const button=event?.currentTarget;
    if(button) button.disabled=true;
    status.textContent="正在处理…";
    try {await action();}
    catch(error) {status.textContent=error.message;}
    finally {if(button) button.disabled=false;}
  };
}
function setBoard(id) {
  const url=new URL(location.href);
  if(id) url.searchParams.set("board",id); else url.searchParams.delete("board");
  history.replaceState(null,"",url);
}

function showSource(root,records,simulated,refresh,connection) {
  const sectionBox=section("已连接的数据源");
  const toolbar=el("div",null,"strip");
  const note=el("p",simulated?"预览入口可使用内置模拟数据。请到实时入口配置 URL。":
    "服务地址由 Observer 后端访问。远端主机需要在允许网段内。","muted");
  if(connection) sectionBox.append(el("p","当前 NEBULA 指标地址 "+connection,"muted"));
  toolbar.append(btn("添加数据源",()=>openSource({},refresh,connection)));
  toolbar.firstChild.disabled=simulated;
  sectionBox.append(note,toolbar);
  for(const data of [{name:"NEBULA 内置",type:"nebula"},...records]) {
    const row=el("div",null,"row");
    const last=data.lastCheck?.revision===data.revision ? data.lastCheck : null;
    row.append(el("span",data.name),el("small",kinds[data.type]),el("small",data.url || "当前 Observer 采集"),
      el("small",last ? ((last.ok?"采集成功":"采集异常")+" · "+timestamp(last.ts,true)+(last.message?" · "+last.message:"")) :
        (["prometheus","tempo","loki","jaeger","pyroscope"].includes(data.type)?"查询时连接":data.id?"等待首次采集":"当前实例")));
    if(data.id) {
      const state=el("small");
      row.append(btn("测试",feedback(state,async()=>{
        const result=await post("source/test",data);
        state.textContent=(result.ok?"连接成功":"端点异常")+" · HTTP "+result.code+" · "+result.durationMs+" ms";
      })),btn("编辑",()=>openSource(data,refresh,connection)),btn("删除",async()=> {
        if (!confirm("删除数据源「"+data.name+"」？")) return;
        await feedback(state,async()=>{await post("source/delete",data);refresh();})();
      }),state);
    }
    sectionBox.append(row);
  }
  root.append(sectionBox);
}

function openSource(source,refresh,connection) {
  const {modal,content}=dialog(source.id?"编辑数据源":"添加数据源");
  const name=input(source.name,70), url=input(source.url,512), metric=el("textarea");
  url.placeholder="http://127.0.0.1:9090";
  metric.rows=3;metric.value=(source.metrics || []).join("\n");metric.maxLength=4000;
  const type=select(Object.entries(kinds).slice(1),source.type), state=el("p","","notice"), discovered=el("div",null,"strip");
  const head=el("div",null,"builder-form");
  head.append(field("名称",name),field("类型",type),field("服务地址（URL）",url));
  const metricField=field("要采集的指标（最多 24 个，每行一个）",metric);
  const read=()=>({...source,name:name.value,url:url.value,type:type.value,metrics:metric.value.split(/[\s,]+/).filter(Boolean)});
  const actions=el("div",null,"strip");
  actions.append(btn("测试连接",feedback(state,async()=>{
    const result=await post("source/test",{...read(),name:name.value||"连接测试",metrics:[]});
    state.textContent=(result.ok?"连接成功":"端点异常")+" · HTTP "+result.code+" · "+result.durationMs+" ms";
    discovered.replaceChildren();
    if(type.value==="metrics") for(const item of result.metrics) {
      discovered.append(btn("+ "+item,()=>{
        const current=read().metrics;
        if(!current.includes(item) && current.length<24) metric.value=[...current,item].join("\n");
      }));
    }
  })),btn("保存数据源",feedback(state,async()=>{
    if(type.value==="metrics"&&!read().metrics.length) throw new Error("先测试连接并选择至少一个要采集的指标");
    await post("source/save",read());
    modal.close();refresh();
  })));
  if(connection) actions.append(btn("填入当前 NEBULA /metrics",()=>{
    type.value="metrics";url.value=connection;if(!name.value) name.value="NEBULA 原始指标"; sync();
  }));
  const sync=()=>{
    metricField.hidden=type.value!=="metrics";
    discovered.replaceChildren();
    url.placeholder=({prometheus:"http://127.0.0.1:9090",metrics:"http://127.0.0.1:9100/metrics",
      http:"http://127.0.0.1:8080/health",tempo:"http://127.0.0.1:3200",loki:"http://127.0.0.1:3100",
      jaeger:"http://127.0.0.1:16686",pyroscope:"http://127.0.0.1:4040"})[type.value];
  };
  type.onchange=sync;sync();
  content.append(head,metricField,actions,el("p","Prometheus、Tempo、Loki 填服务基地址，文本指标和 HTTP 探测填写完整路径。","muted"),state,discovered);
}

function queryControl(target,stored,available,catalog) {
  if(target==="nebula") return select(catalog.map(item=>[item.id,item.title]),stored);
  if(target==="http") return select([["probe_success","成功标记"],["probe_duration_ms","探测耗时"],
    ["probe_http_status","HTTP 状态码"]],stored);
  if(target==="metrics") return select(available.metrics.map(name=>[name,name]),stored);
  const expr=el("textarea");
  expr.value=stored;expr.rows=2;expr.maxLength=1024;expr.placeholder='例如 up 或 rate(http_requests_total[5m])';
  return expr;
}

function editPanel(board,index,catalog,sources,refresh,bounds,scope) {
  const current=board.panels[index] || emptyPanel();
  const data=clone(current), {modal,content}=dialog("配置面板");
  let transfer;
  const title=input(data.title), unit=input(data.unit,24), maximum=input(data.max,20);
  maximum.type="number";maximum.min="0.001";maximum.step="any";
  const source=select(sources.map(s=>[s.id,s.name]),data.source), view=select(Object.entries(views),data.view);
  const width=select([["1","半宽"],["2","整行"]],String(data.width)), color=select(Object.keys(colors).map((name,i)=>[name,["绿色","蓝色","黄色","橙色","紫色","粉色"][i]]),data.color);
  const form=el("div",null,"builder-form");
  form.append(field("面板名称",title),field("数据源",source),field("图形",view),field("宽度",width),
    field("单位",unit),field("颜色",color),field("仪表最大值",maximum));
  const list=el("div",null,"builder-queries"), actions=el("div",null,"strip"), state=el("p","","notice");
  const add=btn("添加查询",()=>{
    transfer();
    if(data.queries.length<4) data.queries.push({expr:"",alias:""});
    render();
  });
  function render() {
    list.replaceChildren();
    const target=sources.find(s=>s.id===source.value);
    const controls=data.queries.map(({expr,alias},i)=>{
      const line=el("div",null,"builder-query");
      const expression=queryControl(target.type,expr,target,catalog), legend=input(alias,60);
      const remove=btn("移除",()=>{transfer();data.queries.splice(i,1);render();});
      remove.disabled=data.queries.length<=1;
      line.append(field("查询 "+String.fromCharCode(65+i),expression),field("图例（可选）",legend),remove);
      list.append(line);
      return {expression,legend};
    });
    transfer=()=>{data.queries=controls.map(c=>({expr:c.expression.value,alias:c.legend.value}));};
    add.disabled=data.queries.length>=4;
  }
  source.onchange=()=>{data.queries=[{expr:"",alias:""}];render();};
  render();
  actions.append(add,btn("运行查询",feedback(state,async()=>{
    transfer();
    const result=[];
    for(const {expr} of data.queries) {
      const payload=await post("query",{source:source.value,expr,scope,...bounds});
      result.push(payload.status==="empty"?"无样本":
        payload.series.length+" 条序列 / "+payload.series.reduce((n,line)=>n+extrema(line).count,0)+" 个样本");
    }
    state.textContent=result.join(" · ");
  })),btn("保存面板",feedback(state,async()=>{
    transfer();
    Object.assign(data,{title:title.value,unit:unit.value,max:Number(maximum.value),width:Number(width.value),
      view:view.value,source:source.value,color:color.value});
    const next=clone(board);
    next.panels[index]=data;
    await post("board/save",next);modal.close();refresh();
  })));
  content.append(form,list,actions,state);
}

function details(panel,series,reports) {
  const {content}=dialog(panel.title+" · 查询检查");
  content.append(rows(["查询","最近","平均","最小","最大","样本"],series.map(line=>{
    const stat=extrema(line);
    return [line.name,...[stat.last,stat.mean,stat.min,stat.max].map(n=>formatValue(n,panel.unit)),stat.count];
  })));
  for(let i=0;i<reports.length;i++) content.append(el("p",panel.queries[i].expr+" · "+
    (reports[i].error || (reports[i].queryMs+" ms · "+reports[i].step+" 秒步长"+(reports[i].truncated?" · 序列已截断":"")))));
  if(series.length) content.append(btn("下载 CSV",()=>{
    const csv=value=>{
      const s=String(value??"");
      const protectedValue=typeof value==="string" && /^\s*[=+@-]/.test(s)?"'"+s:s;
      return '"'+protectedValue.replaceAll('"','""')+'"';
    };
    const lines=series.flatMap(line=>line.points.map(([t,v])=>[new Date(t*1000).toISOString(),line.name,v,panel.unit].map(csv).join(",")));
    download(new Blob(["\uFEFF时间(UTC),序列,值,单位\r\n"+lines.join("\r\n")],{type:"text/csv;charset=utf-8"}),"observer-panel.csv");
  }));
}
function displayPanel(box,panel,series,bounds,visuals,bus,zoom) {
  if (!series.some(line=>line.points.some(([,v])=>v!=null))) {box.append(el("p","当前时间范围没有有效样本","empty"));return;}
  if (panel.view==="line" || panel.view==="area") {
    const target=el("div",null,"chart");
    box.append(target);
    visuals.push(drawChart(target,{model:{...panel,area:panel.view==="area"},bounds,series,markers:[],bus,zoom}));
    const legend=el("div",null,"legend");
    for(const line of series) {
      const item=el("span",line.name,"legend-item");
      item.style.setProperty("--series",line.color);legend.append(item);
    }
    box.append(legend);
    return;
  }
  if(panel.view==="table") {
    const points=series.flatMap(line=>line.points.map(([t,value])=>[t,line.name,value]));
    points.sort((a,b)=>b[0]-a[0]);
    box.append(rows(["时间","序列","值"],points.slice(0,30).map(([t,name,v])=>[timestamp(t,true),name,formatValue(v,panel.unit)])));
    return;
  }
  const grid=el("div",null,"builder-stats");
  for(const line of series) {
    const stat=extrema(line), tile=el("div",null,"stat-card"), value=el("strong",formatValue(stat.last,panel.unit));
    value.style.color=line.color;tile.append(el("span",line.name));
    if(panel.view==="gauge" && stat.last!=null) {
      const gauge=el("meter",null,"stat-meter");gauge.min=0;gauge.max=panel.max;gauge.value=stat.last;
      gauge.setAttribute("aria-label",line.name);tile.append(gauge);
    }
    tile.append(value);grid.append(tile);
  }
  box.append(grid);
}

function download(bytes,name) {
  const url=URL.createObjectURL(bytes), link=el("a");
  link.href=url;link.download=name;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
}
function saveFile(board,sources) {
  const references=sources.filter(source=>board.panels.some(panel=>panel.source===source.id))
    .map(({id,name,type})=>({id,name,type}));
  const bytes=new Blob([JSON.stringify({schema:"nebula-observer/1",title:board.title,panels:board.panels,sources:references},null,2)],
    {type:"application/json"});
  download(bytes,"observer-"+board.id+".json");
}
function importFile(sources,refresh) {
  const {modal,content}=dialog("导入仪表盘");
  const source=select([["","选择 Prometheus 数据源"],...sources.filter(s=>s.type==="prometheus").map(s=>[s.id,s.name])],"");
  const raw=el("textarea"), file=el("input"), state=el("p","","notice"), preview=el("div");
  raw.rows=10;raw.maxLength=262144;raw.placeholder="粘贴 Observer 或 Grafana JSON";
  file.type="file";file.accept="application/json,.json";
  file.onchange=async()=>{
    const selected=file.files[0];
    if(!selected) return;
    if(selected.size>262144) {state.textContent="文件超过 256 KiB";return;}
    raw.value=(await selected.text()).replace(/^\uFEFF/,"");
    preview.replaceChildren();state.textContent=selected.name;
  };
  raw.oninput=source.onchange=()=>preview.replaceChildren();
  content.append(el("p","选择电脑上的 JSON 或粘贴。Observer 模板可重新绑定本机数据源，Grafana 支持常用 Prometheus 正则变量。","muted"),
    field("Grafana 查询绑定",source),field("JSON 文件",file),field("JSON 内容",raw),
    btn("预览导入",feedback(state,async()=>{
      const board=importDashboard(JSON.parse(raw.value),source.value);
      preview.replaceChildren();
      state.textContent=board.title+" · 可导入 "+board.panels.length+" 块 · 跳过 "+board.skipped+" 块 / "+board.skippedQueries+" 条查询";
      const bindings=new Map();
      for(const ref of board.references) {
        const existing=sources.find(item=>item.id===ref.id);
        const kind=ref.id==="nebula"?"nebula":ref.type||existing?.type;
        const compatible=sources.filter(item=>!kind||item.type===kind);
        const control=select([["","选择本机数据源"],...compatible.map(item=>[item.id,item.name])],
          compatible.some(item=>item.id===ref.id)?ref.id:"");
        preview.append(field("数据源绑定 · "+(ref.name||ref.id),control));
        bindings.set(ref.id,control);
      }
      const save=btn("确认导入",feedback(state,async()=>{
        if([...bindings.values()].some(control=>!control.value))throw new Error("请为每个引用选择本机数据源");
        const panels=board.panels.map(panel=>({...panel,source:bindings.get(panel.source)?.value||panel.source}));
        const data=await post("board/save",{title:board.title,panels});
        setBoard(data.id);modal.close();refresh();
      }));
      preview.append(save);
    })),state,preview);
}
function catalogView(root,sources,refresh) {
  const box=section("常用监控模板");
  const grid=el("div",null,"template-grid");
  for(const model of templates) {
    const item=el("article",null,"panel builder-box");
    const matches=sources.filter(s=>s.type===model.kind);
    const targets=select(matches.map(s=>[s.id,s.name]),matches[0]?.id);
    const state=el("small");
    const use=btn("使用模板",feedback(state,async()=>{
      const data=await post("board/save",{title:model.title,panels:model.panels.map(panel=>({...panel,source:targets.value}))});
      setBoard(data.id);refresh();
    }));
    use.disabled=!matches.length;
    item.append(el("h3",model.title),el("p",model.hint,"muted"),
      el("small",model.panels.length+" 个面板"),matches.length?targets:el("small","需先添加匹配的数据源","muted"),use,state);
    grid.append(item);
  }
  box.append(grid);root.append(box);
}

export {post as workspacePost};

async function renderCascade(root,sources,board,scope,bounds,signal,refresh) {
  const strip=el("div",null,"strip"), status=el("small",null,"muted");
  const origin=select(sources.map(item=>[item.id,item.name]),sources[0].id);
  const fields=Object.fromEntries(["bc","app","instance"].map(key=>[key,select([])]));
  let sequence=0;
  const chosen=()=>Object.fromEntries(Object.entries(fields).map(([key,control])=>[key,control.value]));
  const reload=async(first,initial={})=>{
    const revision=++sequence;
    const order=["bc","app","instance"];
    const labels=Object.fromEntries(order.map(key=>[key,fields[key].value]));
    status.textContent="正在读取资源标签…";
    try {
      for(let i=first;i<order.length;i++) {
        const key=order[i];
        const parents=Object.fromEntries(order.slice(0,i).filter(parent=>labels[parent]).map(parent=>[parent,labels[parent]]));
        const data=await post("labels",{...bounds,source:origin.value,label:key,scope:parents},signal);
        if(signal.aborted||revision!==sequence)return;
        const field=fields[key];
        field.replaceChildren(new Option("全部",""),...data.values.map(value=>new Option(value,value)));
        field.value=data.values.includes(initial[key])?initial[key]:"";
        labels[key]=field.value;
      }
      status.textContent="只影响使用相应变量的查询";
    } catch(error) {
      if(!signal.aborted&&revision===sequence)status.textContent=error.message;
    }
  };
  origin.onchange=()=>reload(0);
  fields.bc.onchange=()=>reload(1);
  fields.app.onchange=()=>reload(2);
  const apply=btn("应用 BC / APP / INSTANCE",()=>{
    filters.set(board,{...scope,...chosen()});refresh();
  });
  strip.append(field("变量数据源",origin),...Object.entries(fields).map(([key,control])=>field(key.toUpperCase(),control)),
    apply,status);
  root.append(strip);
  await reload(0,scope);
}

export async function renderCustom(root,{api,bounds,signal,visuals,zoom,refresh,view}) {
  const workspace=await api("workspace",null,signal);
  if(signal.aborted) return;
  if(view==="sources") {
    showSource(root,workspace.sources,workspace.simulated,refresh,workspace.suggestedMetricsUrl);
    return;
  }
  const sources=[{id:"nebula",name:"NEBULA 内置",type:"nebula"},...workspace.sources.filter(s=>["prometheus","metrics","http"].includes(s.type))];
  const current=workspace.boards.find(board=>board.id===uid());
  const actions=el("div",null,"strip");
  const state=el("p","","notice");
  actions.append(btn("新建仪表盘",()=>{
    const {modal,content}=dialog("新建仪表盘");
    const name=input("",80), status=el("p","","notice");
    content.append(field("名称",name),btn("创建",feedback(status,async()=>{
      const board=await post("board/save",{title:name.value,panels:[]});
      setBoard(board.id);modal.close();refresh();
    })),status);
    name.focus();
  }),btn("导入 JSON",()=>importFile(sources,refresh)));
  const choose=select([["","模板与仪表盘列表"],...workspace.boards.map(b=>[b.id,b.title])],current?.id || "");
  choose.onchange=()=>{setBoard(choose.value);refresh();};
  actions.prepend(field("打开仪表盘",choose));
  root.append(actions);
  if(!current) {catalogView(root,sources,refresh);return;}
  const catalog=await api("catalog",null,signal);
  const scope=filters.get(current.id)||{};
  const header=el("div",null,"strip");
  const add=btn("添加面板",()=>editPanel(current,current.panels.length,catalog,sources,refresh,bounds,scope));
  header.append(el("h2",current.title),btn("导出 JSON",()=>saveFile(current,sources)));
  if(current.provisioned)header.append(el("small","由文件管理 · "+current.provisioned+" · r"+current.revision,"muted"));
  else header.append(add,btn("重命名",()=>{
      const {modal,content}=dialog("重命名仪表盘"), name=input(current.title,80), result=el("p","","notice");
      content.append(field("名称",name),btn("保存",feedback(result,async()=>{
        await post("board/save",{...current,title:name.value});modal.close();refresh();
      })),result);
    }),btn("删除仪表盘",async()=>{
      if(!confirm("删除仪表盘「"+current.title+"」？")) return;
      await feedback(state,async()=>{await post("board/delete",current);setBoard("");refresh();})();
    }));
  add.disabled=current.panels.length>=20;
  root.append(header,state);
  const activeProm=sources.filter(s=>s.type==="prometheus" && current.panels.some(p=>p.source===s.id));
  if(activeProm.length) {
    await renderCascade(root,activeProm,current.id,scope,bounds,signal,refresh);
    if(signal.aborted)return;
    const strip=el("div",null,"strip");
    const labelSource=select(activeProm.map(s=>[s.id,s.name]),activeProm[0].id);
    const keys=select(resourceLabels.map(label=>[label,label]),"cluster"), values=select([["","全部"]],"");
    const load=btn("加载标签",feedback(state,async()=>{
      const source=labelSource.value,label=keys.value;
      const result=await post("labels",{source,label,...bounds});
      if(source!==labelSource.value || label!==keys.value) return;
      values.replaceChildren(new Option("全部",""),...result.values.map(v=>new Option(v,v)));
      values.value=scope[label]||"";
      state.textContent=result.values.length ? "" : "这个数据源未返回该标签";
    }));
    keys.onchange=labelSource.onchange=()=>values.replaceChildren(new Option("全部",""));
    strip.append(field("标签来源",labelSource),field("资源标签",keys),values,load,btn("应用筛选",()=>{
      const next={...scope,[keys.value]:values.value};
      filters.set(current.id,next);refresh();
    }),btn("清除筛选",()=>{filters.delete(current.id);refresh();}));
    root.append(strip,el("p",(Object.entries(scope).filter(([,v])=>v).map(([k,v])=>k+"="+v).join(" · ")||"全部资源")+
      " · 使用 ${cluster} 等变量的查询会应用筛选","muted"));
  }
  if(!current.panels.length) root.append(el("p","点击「添加面板」选择数据源、查询和图形。","empty"));
  const grid=el("div",null,"panels custom-panels"), bus=new EventTarget();
  root.append(grid);
  const render=async(panel,index)=>{
    const card=el("section",null,"panel");
    if(panel.width===2) card.classList.add("panel-wide");
    grid.append(card);
    const head=el("div",null,"panel-header");
    const controls=el("div",null,"panel-actions");
    const shift=direction=>feedback(state,async()=>{
      const next=clone(current), to=index+direction;
      [next.panels[index],next.panels[to]]=[next.panels[to],next.panels[index]];
      await post("board/save",next);refresh();
    });
    const prev=btn("前移",shift(-1)), next=btn("后移",shift(1));
    prev.disabled=index===0;next.disabled=index===current.panels.length-1;
    if(!current.provisioned)controls.append(btn("编辑",()=>editPanel(current,index,catalog,sources,refresh,bounds,scope)),prev,next,
      btn("删除",async()=>{
        if(!confirm("删除面板「"+panel.title+"」？")) return;
        await feedback(state,async()=>{
          const board=clone(current);board.panels.splice(index,1);await post("board/save",board);refresh();
        })();
      }));
    head.append(el("h3",panel.title),controls);card.append(head);
    const reports=[];
    for(const query of panel.queries) {
      if(signal.aborted) return;
      const key=JSON.stringify([current.id,current.revision,panel.source,query.expr,scope]);
      try {
        const report=await post("query",{...bounds,scope,source:panel.source,expr:query.expr},signal);
        reports.push(report);
        if(!signal.aborted && report.status==="ok") {
          recent.set(key,report);
          if(recent.size>80) recent.delete(recent.keys().next().value);
        }
      } catch(error) {
        const old=recent.get(key), previous=old?.series.map(line=>({...line,
          points:line.points.filter(([t])=>bounds.from<=t && t<=bounds.to)}));
        reports.push({...old,series:previous,error:error.message+
          (previous?.some(line=>line.points.length) ? " · 暂时显示上次读数" : "")});
      }
    }
    if(signal.aborted) return;
    const series=reports.flatMap((report,i)=>(report.series||[]).map((line,j)=>{
      const labels=Object.entries(line.labels).map(([key,value])=>key+"="+value).join(" · ");
      return {...line,name:(panel.queries[i].alias || panel.queries[i].expr)+(labels?" · "+labels:""),
        step:report.step,color:i+j?palette[(i+j)%palette.length]:colors[panel.color]};
    }));
    controls.prepend(btn("检查",()=>details(panel,series,reports)));
    if(reports.some(item=>item.error)) card.append(el("p",reports.filter(item=>item.error).map(item=>item.error).join(" · "),"notice"));
    displayPanel(card,panel,series,bounds,visuals,bus,zoom);
  };
  // Keep requests within the Observer worker budget.
  for(let i=0;i<current.panels.length;i+=4) {
    if(signal.aborted) return;
    await Promise.all(current.panels.slice(i,i+4).map((panel,j)=>render(panel,i+j)));
  }
}
