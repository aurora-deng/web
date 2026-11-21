const detailKeys=["http.request.method","http.response.status_code","rpc.system","db.system","error.type"];
const resourceKeys=["service.name","service.namespace","service.instance.id","host.name","k8s.cluster.name",
  "k8s.namespace.name","k8s.node.name","k8s.pod.name"];
const kinds=["未采集","内部调用","服务端","客户端","生产者","消费者"];
const kindNames=["UNSPECIFIED","INTERNAL","SERVER","CLIENT","PRODUCER","CONSUMER"];
const colors=["#86c4a7","#80b6fb","#b7a6ee","#f1c574","#72cbc8","#e8a9bb"];
const isError=span=>[2,"STATUS_CODE_ERROR"].includes(span.status);
const duration=span=>Number(span.end-span.start)/1e6;
const ms=value=>value.toFixed(value<1?3:2)+" ms";
const tags=entry=>Object.fromEntries((entry?.attributes||[]).map(a=>[a.key,Object.values(a.value||{})[0]]));
const allowed=(obj,keys)=>Object.fromEntries(keys.filter(key=>["string","number","boolean"].includes(typeof obj?.[key]))
  .map(key=>[key,typeof obj[key]==="string"?obj[key].slice(0,256):obj[key]]));

function el(tag,text,cls) {
  const node=document.createElement(tag);
  if(text!=null)node.textContent=text;
  if(cls)node.className=cls;
  return node;
}

function windowFor(from,to,padding=30) {
  const start=Math.floor(Number(from)/1e9);
  const end=Math.ceil(Number(to)/1e9);
  const now=Math.floor(Date.now()/1000);
  const capped=Math.min(end+padding,now+59);
  return {from:Math.min(capped-1,Math.max(0,capped-604800,start-padding)),to:capped};
}

export function fromOtlp(payload) {
  const result=[];
  const trace=payload.trace||payload;
  for(const group of trace.resourceSpans || trace.batches || []) {
    const resource=tags(group.resource);
    const place=["k8s.cluster.name","k8s.namespace.name","k8s.node.name","host.name","k8s.pod.name"]
      .map(key=>resource[key]).filter(Boolean).join(" / ");
    for(const scope of group.scopeSpans || group.instrumentationLibrarySpans || []) {
      for(const s of scope.spans || []) {
        if(result.length>=1000) return result;
        const attributes=tags(s);
        result.push({id:s.spanId,parent:s.parentSpanId,name:s.name,service:resource["service.name"],
          start:s.startTimeUnixNano,end:s.endTimeUnixNano,status:s.status?.code,kind:s.kind,place,
          links:(s.links||[]).map(link=>link.spanId),attributes:allowed(attributes,detailKeys),
          resourceAttributes:allowed(resource,resourceKeys),cpuNs:attributes["nebula.thread_cpu_ns"]});
      }
    }
  }
  return result;
}

function readSpans(input) {
  const list=new Map();
  for(const value of input.slice(0,1000)) {
    try {
      const start=BigInt(value.start),end=BigInt(value.end);
      if(!value.id || start<=0n || end<start || end>8640000000000000000000n || end-start>604800000000000n) continue;
      list.set(value.id,{...value,name:value.name||"未知操作",service:value.service||"未知服务",start,end,children:[],
        attributes:allowed(value.attributes,detailKeys),resourceAttributes:allowed(value.resourceAttributes,resourceKeys)});
    } catch { /* Omit malformed intervals and surface missing parents in the tree. */ }
  }
  return [...list.values()].sort((a,b)=>a.start<b.start?-1:a.start>b.start?1:a.id.localeCompare(b.id));
}

function outsideChildren(span) {
  let uncovered=span.end-span.start, last=span.start;
  for(const child of span.children) {
    const left=child.start>last?child.start:last, right=child.end<span.end?child.end:span.end;
    if(right<=left) continue;
    uncovered-=right-left;
    last=right;
  }
  return Number(uncovered)/1e6;
}

function fields(values,cls) {
  const list=el("dl",null,cls);
  for(const [key,value] of Object.entries(values))list.append(el("dt",key),el("dd",String(value)));
  return list;
}

function properties(title,values) {
  const group=el("details",null,"trace-attributes");
  const count=Object.keys(values).length;
  group.append(el("summary",title+" · "+count),
    count?fields(values,"trace-property-grid"):el("p","未采集此组属性。","muted"));
  return group;
}

export function renderTraceDetails(root,input,{traceId,partial=false,onSelect,selectedSpan,traceSource=""}={}) {
  const spans=readSpans(input);
  if(!spans.length) {
    root.append(el("p","没有可绘制的 span。","notice"));
    return null;
  }
  const byId=new Map(spans.map(span=>[span.id,span]));
  const services=[...new Set(spans.map(span=>span.service))];
  const colorByService=new Map(services.map((name,i)=>[name,colors[i%colors.length]]));
  const roots=[], edges=new Map();
  for(const span of spans) {
    const parent=byId.get(span.parent);
    if(!parent) {roots.push(span);continue;}
    parent.children.push(span);
    if(parent.service!==span.service) {
      const edge=parent.service+" → "+span.service;
      edges.set(edge,(edges.get(edge)||0)+1);
    }
  }
  const start=spans[0].start, end=spans.reduce((max,span)=>span.end>max?span.end:max,start);
  const elapsed=Math.max(0.001,Number(end-start)/1e6);
  const tree=[], visited=new Set();
  function walk(seed) {
    const pending=[[seed,0]];
    while(pending.length) {
      const [span,depth]=pending.pop();
      if(visited.has(span.id))continue;
      visited.add(span.id);
      tree.push({span,depth});
      for(const child of [...span.children].reverse())pending.push([child,depth+1]);
    }
  }
  roots.forEach(walk);
  const disconnected=tree.length!==spans.length;
  spans.forEach(walk);
  const missing=roots.filter(span=>span.parent).length;
  const warnings=[missing?missing+" 个父 span 未收录":"",disconnected?"存在无法追溯到根的关联":"",
    input.length!==spans.length?"部分 span 格式无效或超过显示上限":""].filter(Boolean);
  const totalErrors=spans.filter(isError).length;
  const summary=el("section",null,"panel builder-box trace-inspect");
  const heading=el("div",null,"trace-heading"), actions=el("div",null,"strip");
  const primary=roots[0]||spans[0];
  heading.append(el("h3",primary.service+" · "+primary.name),actions);
  const identity=el("p",null,"trace-identity muted");
  if(traceId) {
    identity.append("Trace ID ",el("code",traceId));
    const copy=el("button","复制 Trace ID"), save=el("button","导出 JSON"), feedback=el("span",null,"muted");
    copy.type=save.type="button";
    feedback.setAttribute("role","status");
    copy.onclick=async()=>{
      try {await navigator.clipboard.writeText(traceId);feedback.textContent="已复制";}
      catch {feedback.textContent="无法访问剪贴板，可以选中下方 Trace ID 复制。";}
    };
    save.onclick=()=>{
      const exported=spans.map(({children,start,end,...span})=>({...span,start:String(start),end:String(end)}));
      const url=URL.createObjectURL(new Blob([JSON.stringify({traceId,partial:partial||warnings.length>0,spans:exported},null,2)],
        {type:"application/json"}));
      const link=el("a");link.href=url;link.download="trace-"+traceId+".json";link.click();
      setTimeout(()=>URL.revokeObjectURL(url),1000);
    };
    actions.append(copy,save,feedback);
  }
  summary.append(el("h3","实际调用路线"),heading,identity,el("p",
    spans.length+" 个 span · "+services.length+" 个服务 · "+totalErrors+" 个错误 · "+ms(Number(end-start)/1e6)+
    (partial||warnings.length?" · 部分链路":""),"notice"));
  if(warnings.length)summary.append(el("p",warnings.join("；"),"muted"));
  if(edges.size)summary.append(el("p","本链路调用边 · "+[...edges].map(([edge,count])=>edge+" ×"+count).join("；"),"muted"));

  const details=el("div",null,"trace-selected");
  const filterGroup=el("details",null,"trace-filter-group");
  filterGroup.open=true;
  const filter=el("div",null,"strip trace-filters");
  const service=el("select"), word=el("input"), minimum=el("input"), onlyErrors=el("input");
  service.setAttribute("aria-label","按服务筛选 span");
  service.append(new Option("全部服务",""),...services.map(name=>new Option(name,name)));
  word.placeholder="操作、节点或 Span ID";word.setAttribute("aria-label","过滤 span");
  minimum.type="number";minimum.min="0";minimum.step="any";minimum.placeholder="至少 ms";
  minimum.setAttribute("aria-label","Span 最短毫秒数");
  onlyErrors.type="checkbox";const errorLabel=el("label");errorLabel.append(onlyErrors,"仅错误");
  filter.append(service,word,minimum,errorLabel);
  filterGroup.append(el("summary","Span 筛选"),filter);
  const list=el("div",null,"timeline trace-tree");
  const scale=el("div",null,"trace-scale muted");
  scale.append(el("span","0 ms"),...Array.from({length:4},(_,i)=>el("span",ms(elapsed*(i+1)/4))));
  const initial=selectedSpan||new URL(location.href).searchParams.get("span");
  let selected=byId.get(initial)||roots[0]||spans[0];
  const collapsed=new Set();

  function place(bar,span) {
    const left=Number(span.start-start)/1e6/elapsed*100;
    bar.style.left=Math.min(99.7,left)+"%";
    bar.style.width=Math.max(0.3,Math.min(100-left,duration(span)/elapsed*100))+"%";
    bar.style.setProperty("--span-color",isError(span)?"var(--danger)":colorByService.get(span.service));
  }
  const overview=el("div",null,"trace-overview");
  overview.setAttribute("aria-hidden","true");
  for(const span of spans.slice(0,24)) {
    const row=el("div",null,"span-track");
    const bar=el("span",null,"span-bar");place(bar,span);
    row.append(bar);overview.append(row);
  }

  function placeDetails() {
    const row=[...list.children].find(item=>item.dataset.spanId===selected.id);
    (row||list).after(details);
  }

  function inspect(span) {
    selected=span;
    const route=[];
    const ancestors=new Set();
    for(let cursor=span;cursor && !ancestors.has(cursor.id);cursor=byId.get(cursor.parent)) {
      ancestors.add(cursor.id);
      route.unshift(cursor.service+" · "+cursor.name);
    }
    const position=windowFor(span.start,span.end);
    const evidence=el("div",null,"strip");
    for(const [view,label] of [["service","该时段业务指标"],
      ["resources",traceSource==="nebula"?"该时段主机与进程":"Observer 本机资源（同一时段）"],
      ["profiles","该时段函数剖析"]]) {
      const link=el("a",label);
      link.href="/?"+new URLSearchParams({view,...position}); evidence.append(link);
    }
    if(traceId) {
      const structured=el("a","查看结构化业务日志");
      structured.href="/?"+new URLSearchParams({view:"logs",logTrace:traceId,...position});
      evidence.append(structured);
      const link=el("a","此 Span 的直达链接");
      const url=new URL(location.href);
      const identKey={distributed:"incident",profiles:"call"}[url.searchParams.get("view")]||"trace";
      url.searchParams.set(identKey,traceId);
      url.searchParams.set("span",span.id);
      link.href=url;
      evidence.append(link);
      if(url.searchParams.get("view")!=="distributed") {
        const logs=el("a","按 Trace 关联日志");
        const params=new URLSearchParams({view:"distributed",incident:traceId,span:span.id,...position});
        if(traceSource)params.set("traceSource",traceSource);
        logs.href="/?"+params+"#trace-logs";
        evidence.append(logs);
      }
    }
    const status=isError(span)?"错误":[1,"STATUS_CODE_OK"].includes(span.status)?"成功":"未标注";
    const kindIndex=typeof span.kind==="number"?span.kind:kindNames.indexOf(String(span.kind).replace(/^SPAN_KIND_/,""));
    const category=kinds[kindIndex]||kinds[0];
    const at=new Date(Number(span.start/1000000n)).toISOString();
    details.replaceChildren(el("h4",span.name),el("p",route.join(" → "),"trace-route"),
      fields({"服务":span.service,"开始时间":at+" · 距起点 "+ms(Number(span.start-start)/1e6),
        "墙钟耗时":ms(duration(span)),"已知子调用之外":ms(outsideChildren(span)),
        ...(span.cpuNs!=null && Number.isFinite(Number(span.cpuNs))?{"线程 CPU":ms(Number(span.cpuNs)/1e6)}:{}),
        "子 Span":span.children.length,"类型":category,"状态":status,"Span ID":span.id,
        "父 Span":span.parent||"根"}, "trace-property-grid trace-facts"));
    const attributes=allowed(span.attributes,detailKeys);
    if(span.links?.length)attributes["关联 Span"]=span.links.join(" / ");
    details.append(evidence,properties("Span 属性",attributes),
      properties("资源属性",allowed(span.resourceAttributes,resourceKeys)));
    for(const row of list.children) row.classList.toggle("selected",row.dataset.spanId===span.id);
    placeDetails();
    onSelect?.({id:span.id,...position});
  }

  function repaint() {
    list.replaceChildren();
    const q=word.value.trim().toLowerCase(), min=Number(minimum.value)||0;
    const filtering=!!(service.value||onlyErrors.checked||min||q);
    let collapsedDepth=Infinity;
    for(const {span,depth} of tree) {
      if(!filtering&&depth>collapsedDepth)continue;
      collapsedDepth=!filtering&&collapsed.has(span.id)?depth:Infinity;
      if((service.value && span.service!==service.value) || (onlyErrors.checked&&!isError(span)) || duration(span)<min ||
        (q && ![span.name,span.service,span.place,span.id].join(" ").toLowerCase().includes(q))) continue;
      const row=el("div",null,"span-row trace-tree-row"), track=el("div",null,"span-track");
      row.dataset.spanId=span.id;
      row.classList.toggle("selected",span.id===selected.id);
      const nameBox=el("div",null,"trace-name-group");
      if(span.children.length) {
        const open=el("button",collapsed.has(span.id)&&!filtering?"▸":"▾","trace-toggle");
        open.type="button";open.disabled=filtering;
        open.setAttribute("aria-label","展开或折叠 "+span.name);
        open.setAttribute("aria-expanded",String(!collapsed.has(span.id)||filtering));
        open.onclick=()=>{if(collapsed.has(span.id))collapsed.delete(span.id);else collapsed.add(span.id);repaint();};
        nameBox.append(open);
      }
      const label=el("button",(depth?"↳ ":"")+span.name,"trace-span-name");
      label.type="button";label.style.paddingInlineStart=Math.min(depth,6)*10+4+"px";
      label.title=span.service+" · "+span.name;
      nameBox.append(label);
      const bar=el("button",null,"span-bar");
      bar.type="button";
      bar.setAttribute("aria-label",span.name+" "+ms(duration(span)));
      bar.classList.toggle("bad",isError(span));
      place(bar,span);
      label.onclick=bar.onclick=()=>inspect(span);
      track.append(bar);
      row.append(nameBox,track,el("span",ms(duration(span))));
      list.append(row);
    }
    if(!list.children.length)list.append(el("p","没有符合条件的 span。","empty"));
    placeDetails();
  }
  service.onchange=word.oninput=minimum.oninput=onlyErrors.onchange=repaint;
  summary.append(filterGroup,overview,el("h3","调用树与瀑布图"),scale,list,
    el("p","已知子调用之外是扣除已记录子调用重叠时间后的墙钟，可能包含等待和未埋点操作。","muted"));
  root.append(summary);
  repaint();inspect(selected);
  return windowFor(start,end);
}
