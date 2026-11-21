const states={waiting:"等待首轮采样",paused:"已暂停",normal:"正常",pending:"等待持续条件",firing:"已触发",
  no_data:"暂无新数据",error:"查询失败"};
const operators={gt:">",gte:"≥",lt:"<",lte:"≤"};
const reducers={last:"最近值",avg:"窗口平均",max:"窗口最大",min:"窗口最小"};
const severities={info:"信息",warning:"警告",critical:"严重"};
const presets=[
  ["会话列表响应较慢","http_p99","gt",250,"ms","用户打开会话列表可能变慢，查看会话链路和数据库调用。"],
  ["业务服务器响应错误增加","http_5xx_pct","gt",5,"%","部分业务操作可能失败，检查 5xx 与同期数据库就绪。"],
  ["主机 CPU 持续升高","cpu","gt",80,"%","定位同一时段占用最高的进程，核对客户操作是否变慢。"],
];

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null)node.textContent=text;
  if(cls)node.className=cls;
  return node;
}

function input(type,value="",name="") {
  const control=el("input");
  control.type=type;control.value=value;
  if(name)control.setAttribute("aria-label",name);
  return control;
}

function select(choices,value) {
  const control=el("select");
  control.append(...choices.map(([key,title])=>new Option(title,key)));
  if(value!=null)control.value=value;
  return control;
}

function field(title,control) {
  const label=el("label",null,"builder-field");
  label.append(el("span",title),control);return label;
}

function button(text,action) {
  const node=el("button",text);node.type="button";node.onclick=action;return node;
}

const number=(value,unit="")=>value==null?"—":Number(value).toLocaleString("zh-CN",{maximumFractionDigits:3})+(unit?" "+unit:"");
const displayTime=value=>value?new Date(value*1000).toLocaleString("zh-CN",{hour12:false}):"—";
const labelText=labels=>Object.entries(labels||{}).map(([name,value])=>name+"="+value).join(" · ");
const statusClass=state=>state==="firing"||state==="error"?"bad":state==="normal"?"ok":"muted";
const brief=(result,unit)=>[states[result.state]||result.state,
  result.value!=null?"读数 "+number(result.value,unit):"",result.samples!=null?result.samples+" 个点":"",
  result.sampleAt?"最新 "+displayTime(result.sampleAt):"",result.partial?"部分序列":""].filter(Boolean).join(" · ");

function showRule(rule,ctx) {
  const panel=el("article",null,"panel builder-box alert-card"), header=el("div",null,"strip");
  header.append(el("h3",rule.name),el("span",severities[rule.severity],"pill"),
    el("strong",states[rule.result.state],statusClass(rule.result.state)));
  const command=el("p",reducers[rule.reducer]+" "+operators[rule.op]+" "+number(rule.threshold,rule.unit)+
    " · 窗口 "+rule.windowSeconds+" 秒 · 持续 "+rule.pendingSeconds+" 秒","muted");
  const actions=el("div",null,"strip"), feedback=el("p",null,"notice");
  async function perform(op,body) {
    try {await ctx.post(op,body,ctx.signal);ctx.refresh();}
    catch(error) {feedback.textContent=error.message;}
  }
  const query=el("details");
  const source=ctx.sources.find(item=>item.id===rule.source);
  query.append(el("summary","查看数据来源与查询"),el("p",source?.name||"数据源已失效"),el("code",rule.expr));
  if(rule.source==="nebula") {
    const open=el("a","查看指标");
    open.href="/?"+new URLSearchParams({view:"explore",metric:rule.expr});actions.append(open);
  }
  actions.append(button("编辑",()=>editRule(rule,ctx)),button(rule.enabled?"暂停":"启用",
    ()=>perform("alert/save",{...rule,enabled:!rule.enabled})),button("删除",()=>{
    if(confirm("删除告警规则「"+rule.name+"」？"))perform("alert/delete",{id:rule.id,revision:rule.revision});
  }));
  panel.append(header,command,el("p",brief(rule.result,rule.unit),"notice"));
  if(rule.impact)panel.append(el("p",rule.impact));
  if(rule.result.labels)panel.append(el("p",labelText(rule.result.labels),"muted"));
  if(rule.result.error)panel.append(el("p",rule.result.error,"bad"));
  panel.append(actions,query,feedback);
  return panel;
}

function editRule(rule,{sources,catalog,post,signal,refresh}) {
  const dialog=el("dialog",null,"builder-dialog"), title=el("h2",rule?"编辑告警规则":"新建告警规则");
  const header=el("header",null,"drawer-header");
  header.append(title,button("关闭",()=>dialog.close()));
  const body=el("div",null,"builder-modal"), form=el("form",null,"builder-form");
  const name=input("text",rule?.name||"","规则名称");
  name.required=true;name.maxLength=80;
  const source=select(sources.map(s=>[s.id,s.name]),rule?.source||"nebula");
  const metricContainer=el("div"), impact=el("textarea");
  impact.rows=2;impact.maxLength=200;impact.value=rule?.impact||"";
  const unit=input("text",rule?.unit||"");unit.maxLength=24;
  const operator=select(Object.entries(operators),rule?.op||"gt");
  const reducer=select(Object.entries(reducers),rule?.reducer||"last");
  const severity=select(Object.entries(severities),rule?.severity||"warning");
  const threshold=input("number",rule?.threshold??"","触发阈值");
  threshold.step="any";threshold.required=true;
  const window=select([[30,"30 秒"],[60,"1 分钟"],[300,"5 分钟"],[900,"15 分钟"]],rule?.windowSeconds??60);
  const pending=input("number",rule?.pendingSeconds??30,"持续秒数");
  pending.min=0;pending.max=3600;pending.required=true;
  const starter=select([["","从常用规则开始"],...presets.map(([text],i)=>[i,text])],"");
  let metric;
  function inputForSource(value) {
    const record=sources.find(s=>s.id===source.value);
    if(record?.type==="nebula") {
      metric=select(catalog.map(item=>[item.id,item.title]),value||"http_p99");
      metric.onchange=()=>{unit.value=catalog.find(def=>def.id===metric.value)?.unit||"";};
    } else if(record?.type==="http") {
      metric=select([["probe_success","探测成功"],["probe_duration_ms","探测耗时"],["probe_http_status","HTTP 状态码"]],value||"probe_success");
    } else if(record?.type==="metrics"&&record.metrics?.length) {
      metric=select(record.metrics.map(id=>[id,id]),value||record.metrics[0]);
    } else {
      metric=el("textarea");metric.value=value||"";metric.rows=2;metric.maxLength=1024;metric.required=true;
    }
    metric.setAttribute("aria-label","告警查询");
    metricContainer.replaceChildren(field("告警查询",metric));
  }
  source.onchange=()=>{inputForSource();unit.value="";metric.onchange?.();};
  inputForSource(rule?.expr);
  starter.onchange=()=>{
    if(!starter.value)return;
    const [heading,expr,op,value,sampleUnit,copy]=presets[Number(starter.value)];
    name.value=heading;source.value="nebula";inputForSource(expr);operator.value=op;threshold.value=value;
    unit.value=sampleUnit;impact.value=copy;
  };
  form.append(field("规则名称",name),field("数据源",source),metricContainer,field("统计方式",reducer),
    field("触发条件",operator),field("触发阈值",threshold),field("单位",unit),field("告警等级",severity),
    field("计算窗口",window),field("持续秒数",pending),field("对用户的影响与处理",impact));
  const actions=el("div",null,"strip"), out=el("p",null,"notice");
  out.setAttribute("role","status");
  const request=()=>({...rule,name:name.value,source:source.value,expr:metric.value,
    reducer:reducer.value,op:operator.value,threshold:Number(threshold.value),unit:unit.value,
    severity:severity.value,windowSeconds:Number(window.value),pendingSeconds:Number(pending.value),
    impact:impact.value,enabled:rule?.enabled??true});
  let busy=false;
  async function apply(preview) {
    if(busy||!form.reportValidity())return;
    busy=true;out.textContent=preview?"正在读取真实数据…":"正在保存…";
    try {
      const data=await post(preview?"alert/preview":"alert/save",request(),signal);
      if(preview)out.textContent="预览 · "+brief(data,unit.value)+" · 仅试算本次";
      else {dialog.close();refresh();}
    } catch(error) {out.textContent=error.message;}
    finally {busy=false;}
  }
  form.onsubmit=event=>{event.preventDefault();apply(false);};
  actions.append(button("试算当前数据",()=>apply(true)),button("保存规则",()=>apply(false)));
  body.append(field("常用起点",starter),form,actions,out);
  dialog.append(header,body);document.body.append(dialog);
  dialog.addEventListener("close",()=>dialog.remove(),{once:true});dialog.showModal();
}

export async function renderCustomAlerts(root,{api,post,signal,refresh}) {
  const [data,workspace,catalog]=await Promise.all([api("alert-rules",null,signal),api("workspace",null,signal),api("catalog",null,signal)]);
  if(signal.aborted)return;
  const sources=[{id:"nebula",name:"NEBULA 内置指标",type:"nebula"},...workspace.sources.filter(s=>["prometheus","metrics","http"].includes(s.type))];
  const ctx={sources,catalog,post,signal,refresh};
  const head=el("div",null,"strip"), add=button("新建告警规则",()=>editRule(null,ctx));
  add.disabled=workspace.simulated || data.rules.length>=12;
  head.append(el("h2","自定义规则"),add);
  root.append(head);
  if(!data.rules.length)root.append(el("p",workspace.simulated?"到实时实例可新建和试算告警规则。":"暂无自定义规则。","empty"));
  const grid=el("div",null,"alert-cards");grid.append(...data.rules.map(rule=>showRule(rule,ctx)));root.append(grid);
  if(data.history.length) {
    const history=el("section",null,"panel builder-box");
    history.append(el("h3","最近状态变化"));
    const list=el("div",null,"list");
    for(const record of data.history.slice(0,20)) {
      const row=el("div",null,"row");
      row.append(el("span",record.name),el("small",displayTime(record.at)),
        el("strong",states[record.state],statusClass(record.state)),
        el("small",number(record.value,record.unit)));
      list.append(row);
    }
    history.append(list);root.append(history);
  }
}
