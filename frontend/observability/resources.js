import {drawChart, palette, timestamp} from "./chart.js";
import {renderFunctionPerformance} from "./function_performance.js";
import {renderCommitReport} from "./commit_report.js";

const cpuLabels={user:"用户态",nice:"低优先级用户态",system:"内核态",irq:"硬中断",softirq:"软中断",
  iowait:"I/O 等待",steal:"虚拟化抢占",idle:"空闲"};
const fields={cpuPct:"进程 CPU（单核=100%）",userPct:"用户态 CPU",systemPct:"内核态 CPU",
  capacityPct:"占整机容量",busyPct:"占整机已忙 CPU",rssKiB:"RSS",pssKiB:"PSS",rssPct:"RSS / 物理内存",
  majorPerSec:"主缺页 / 秒",minorPerSec:"次缺页 / 秒",readBytesPerSec:"磁盘读 B/s",writeBytesPerSec:"磁盘写 B/s",
  runtimeNsPerSec:"主线程上 CPU ms/s",waitNsPerSec:"主线程就绪等待 ms/s",
  voluntaryPerSec:"主线程主动切换 / 秒",involuntaryPerSec:"主线程被抢占 / 秒"};
const units={cpuPct:"%",userPct:"%",systemPct:"%",capacityPct:"%",busyPct:"%",rssPct:"%",rssKiB:"KiB",pssKiB:"KiB"};
const fmt=(n,u="")=>n==null?"—":Number(n).toLocaleString("zh-CN",{maximumFractionDigits:3})+(u?" "+u:"");

function el(tag,text,cls) {
  const n=document.createElement(tag);
  if(text!=null)n.textContent=text;
  if(cls)n.className=cls;
  return n;
}
function button(text,fn) {
  const b=el("button",text);b.type="button";b.onclick=fn;return b;
}
function select(options,current) {
  const s=el("select");
  for(const [key,label] of options)s.append(new Option(label,key));
  if(current!=null)s.value=current;
  return s;
}
function section(title) {
  const s=el("section",null,"panel builder-box");
  s.append(el("h2",title));return s;
}
function table(columns,rows) {
  const t=el("table"),head=el("thead"),top=el("tr"),body=el("tbody"),wrap=el("div",null,"table-wrap");
  top.append(...columns.map(value=>el("th",value)));head.append(top);
  for(const row of rows) {
    const tr=el("tr");
    for(const value of row) {
      const td=el("td");
      if(value instanceof Node) td.append(value);else td.textContent=value;
      tr.append(td);
    }
    body.append(tr);
  }
  t.append(head,body);wrap.append(t);return wrap;
}
function label(text,component) {
  const l=el("label",text,"builder-field");l.append(component);return l;
}
function link(text,url) {
  const a=el("a",text);a.href=url;return a;
}

function panelChart(root,title,values,{bounds,visuals,zoom,bus,unit=""}) {
  const box=section(title), plot=el("div",null,"chart");
  if(values.some(line=>line.points.some(p=>p[1]!=null))) {
    box.append(plot);
    visuals.push(drawChart(plot,{model:{title,unit,area:false},bounds,series:values,markers:[],bus,zoom}));
    const legend=el("div",null,"legend");
    for(const line of values) {
      const label=el("span",line.name,"legend-item");
      label.style.setProperty("--series",line.color);legend.append(label);
    }
    box.append(legend);
  } else box.append(el("p","选定时段没有样本","empty"));
  root.append(box);
}

function series(name,index,samples,read) {
  return {name,color:palette[index%palette.length],step:Math.max(5,samples[1]?Math.ceil(samples[1].at-samples[0].at):5),
    points:samples.map(s=>[s.at,read(s)??null])};
}
function pickedReport(report,root) {
  const summary=section(report.label);
  const info=report.baseline||{};
  const plain=info.plain,pinned=info.pinned;
  summary.append(el("p","内核 "+report.kernel+" · 二进制 SHA256 "+report.binarySha256,"muted"),
    el("p","未绑核 P99 中位数 "+fmt(plain?.p99Ms,"ms")+"（"+(plain?.rounds||0)+" 轮） · 绑核 "+
      fmt(pinned?.p99Ms,"ms")+"（"+(pinned?.rounds||0)+" 轮）","notice"));
  const list=el("div",null,"strip"), result=el("div");
  if(report.ackBreakdown) {
    const a=report.ackBreakdown, detail=el("details",null,"ack-breakdown");
    const stages={queueUs:"排队",poolAcquireUs:"取连接",commitStartUs:"到 COMMIT 开始",
      commitUs:"COMMIT",ackReadyUs:"任务开始到 ACK 就绪"};
    const ms=n=>n==null || n<0?"—":fmt(n/1000);
    const remaining=row=>{
      if(![row.rttMs,row.queueUs,row.ackReadyUs].every(n=>Number.isFinite(n)&&n>=0)) return "—";
      const outside=row.rttMs-(row.queueUs+row.ackReadyUs)/1000;
      return outside>=0?fmt(outside):"—";
    };
    detail.append(el("summary","混合负载 ACK 逐条分段 · "+a.workers+" 工作端"),
      el("p","独立混合负载实验 · 配对 "+a.paired+" 条 · 客户端 ACK P99 "+fmt(a.rttP99Ms,"ms"),"notice"),
      table(["阶段","P99 ms"],Object.entries(stages).map(([k,name])=>[name,ms(a.stageP99Us[k])])),
      el("p","以下按客户端 RTT 排序。未细分耗时从同一条 RTT 减去排队和任务至 ACK 就绪，可能包含收发、事件循环调度和客户端处理。","muted"),
      table(["客户端 RTT ms",...Object.values(stages).map(name=>name+" ms"),"未细分 ms"],a.slowest.map(row=>
        [fmt(row.rttMs),...Object.keys(stages).map(key=>ms(row[key])),remaining(row)])));
    summary.append(detail);
  }
  function runDisplay(run) {
    result.replaceChildren(el("h3",run.mode+" · 第 "+run.round+" 轮"),
      el("p","样本 "+run.ack.samples+" · P50 "+fmt(run.ack.p50Ms,"ms")+" · P95 "+fmt(run.ack.p95Ms,"ms")+
        " · P99 "+fmt(run.ack.p99Ms,"ms")+" · CPU 亲和性 "+run.affinity,"notice"));
    if(run.instrumented) result.append(el("p","本轮有跟踪或插桩开销，仅用于定位成本。","muted"));
    if(run.timeline?.length) {
      result.append(el("h3","ACK 与同区间资源"),
        el("p","分桶样本少时 P99 接近最大值。内核项以主机为范围。","muted"),
        table(["区间结束","样本数","ACK P99 ms","进程 CPU %","内核态 CPU %","RSS KiB","主机切换/s","TCP 重传/s","主缺页/s"],
          run.timeline.map(p=>[timestamp(p.at,true),p.ack.samples,fmt(p.ack.p99Ms),fmt(p.cpuPct),fmt(p.systemPct),
            fmt(p.rssKiB),fmt(p.contextSwitches),fmt(p.retransmits),fmt(p.majorFaults)])));
    }
    const r=run.resource;
    if(r) {
      result.append(table(["服务 / PID","CPU（单核）","用户态","内核态","RSS","PSS"],
        r.processes.map(p=>[p.service+" / "+p.pid,fmt(p.cpuPct,"%"),fmt(p.userPct,"%"),fmt(p.systemPct,"%"),
          fmt(p.rssKiB,"KiB"),fmt(p.pssKiB,"KiB")])));
      result.append(table(["内核计数器","每秒增量"],Object.entries(r.rates).map(([k,v])=>[k,fmt(v)])));
    }
    if(run.syscalls?.length) result.append(el("h3","Syscall 墙钟时间"),
      table(["调用","次数","错误","累计秒","平均 μs"],run.syscalls.map(s=>[s.name,s.calls,s.errors,s.wallSeconds,s.usecPerCall])));
    if(run.functions) {
      result.append(el("h3","函数自身指令量"),
        table(["函数","指令数","占本轮自身指令比例"],run.functions.instructions.map(f=>[f.symbol,fmt(f.value),
          fmt(run.functions.totalInstructions?f.value/run.functions.totalInstructions*100:null,"%")])));
      result.append(el("h3","函数归因的系统调用时间"),
        table(["函数","累计 syscall 墙钟时间 ms"],run.functions.syscallNs.map(f=>[f.symbol,fmt(f.value/1e6)])),
        table(["函数","累计 syscall 内核 CPU ms"],run.functions.syscallCpuNs.map(f=>[f.symbol,fmt(f.value/1e6)])));
    }
    if(run.heap) {
      result.append(el("h3","分配调用栈与堆内存"),
        el("p","累计分配 "+fmt(run.heap.totals.tb,"B")+" · 全局堆峰值时仍存活 "+
          fmt(run.heap.totals.gb,"B")+" · 退出前仍存活 "+fmt(run.heap.totals.eb,"B"),"notice"));
      const heapTable=items=>table(["调用栈","累计分配 B","分配次数","全局峰值时存活 B","退出前存活 B"],items.map(f=>{
        const stack=el("details");stack.append(el("summary",f.stack[0]||"未知调用栈"),el("pre",f.stack.join("\n")));
        return [stack,f.tb,f.tbk,f.gb,f.eb];
      }));
      result.append(heapTable(run.heap.allocations));
      if(run.heap.retained.length) result.append(el("h3","退出前存活量最大的分配栈"),heapTable(run.heap.retained));
    }
  }
  for(const run of report.runs) list.append(button(run.mode+" #"+run.round,()=>runDisplay(run)));
  if(Object.keys(report.skipped||{}).length) summary.append(el("p","未完成 "+JSON.stringify(report.skipped),"notice"));
  summary.append(list,result);
  runDisplay(report.runs[0]);root.replaceChildren(summary);
}

function flame(root,profile) {
  root.replaceChildren();
  if(!profile.total||!profile.levels.length) {root.append(el("p","此时间窗口没有匹配的样本","empty"));return;}
  const info=el("p","点击色块查看调用栈读数。样本单位 "+profile.unit,"notice");
  const graph=el("div",null,"resource-flame");
  for(const [depth,level] of profile.levels.entries()) {
    const strip=el("div",null,"resource-flame-row");
    for(const node of level) {
      const b=button(node.name,()=>{info.textContent=node.name+" · 含子调用 "+fmt(node.total,profile.unit)+
        " · 自身 "+fmt(node.self,profile.unit)+" · "+fmt(node.total/profile.total*100,"%");});
      b.style.left=(node.x/profile.total*100)+"%";b.style.width=(node.total/profile.total*100)+"%";
      b.style.background=palette[depth%palette.length];
      b.title=node.name;strip.append(b);
    }
    graph.append(strip);
  }
  const flat=profile.levels.flat();
  const own=new Map();
  for(const node of flat) own.set(node.name,(own.get(node.name)||0)+node.self);
  root.append(info,graph,table(["函数","自身样本","占全部样本"],[...own.entries()]
    .sort((a,b)=>b[1]-a[1]).slice(0,30).map(([name,n])=>[name,fmt(n,profile.unit),fmt(n/profile.total*100,"%")])));
}

function pyroscope(root,sources,{post,bounds,signal}) {
  const box=section("Pyroscope 持续剖析");
  const source=select([["","请选择数据源"],...sources.map(p=>[p.id,p.name])],"");
  const kind=select([["","加载类型后选择"]]), selector=el("input"), state=el("p",null,"notice"), result=el("div");
  selector.placeholder='{service_name="nebula-webserver"}';
  const types=button("加载 Profile 类型",async()=> {
    try {
      const data=await post("profile",{...bounds,source:source.value,action:"catalog"},signal);
      kind.replaceChildren(...data.types.map(t=>new Option(t.id,t.id)));
      state.textContent=data.types.length?"":"这个时间窗没有 profile 类型";
    } catch(error) {state.textContent=error.message;}
  });
  const query=button("查看函数火焰图",async()=>{
    state.textContent="正在查询…";
    try {
      const data=await post("profile",{...bounds,source:source.value,profileType:kind.value,selector:selector.value},signal);
      state.textContent="";flame(result,data);
    } catch(error) {state.textContent=error.message;}
  });
  source.onchange=()=>kind.replaceChildren(new Option("加载类型后选择",""));
  box.append(el("p","CPU、分配字节与存活堆取决于实际采集的 Profile 类型。","muted"),
    label("数据源",source),types,label("类型",kind),label("服务标签选择器",selector),query,state,result);
  if(!sources.length) {types.disabled=query.disabled=true;box.append(link("先配置 Pyroscope 数据源","/?view=sources"));}
  root.append(box);
}

function machine(root,data,ctx) {
  const last=data.latest;
  if(!last) {
    root.append(el("p",data.collecting?"正在等待 Linux 采样或此时段没有历史。":"此实例没有启用 Linux 采集。","notice"));
    return;
  }
  const host=section(last.host+" · "+last.kernel);
  host.append(el("p","采样 "+timestamp(last.at,true)+" · 区间 "+fmt(last.interval,"s")+
    " · 逻辑 CPU "+fmt(last.logicalCpuCount??(Object.keys(last.cpu).length-1||null)),"muted"));
  const charts=el("div",null,"panels");
  panelChart(charts,"主机 CPU 时间构成",Object.entries(cpuLabels).map(([k,name],i)=>
    series(name,i,data.series,s=>s.cpu.cpu[k])),{...ctx,unit:"%"});
  panelChart(charts,"主机物理内存与交换区",[
    series("可用物理内存",0,data.series,s=>s.gauges.MemAvailable),
    series("已使用交换区",1,data.series,s=>s.gauges.SwapTotal==null || s.gauges.SwapFree==null
      ?null:s.gauges.SwapTotal-s.gauges.SwapFree),
  ],{...ctx,unit:"KiB"});
  host.append(charts);
  host.append(el("p","物理内存 "+fmt(last.gauges.MemTotal,"KiB")+" · 可用 "+fmt(last.gauges.MemAvailable,"KiB")+
    " · 交换区 "+fmt(last.gauges.SwapTotal,"KiB")+" · 剩余 "+fmt(last.gauges.SwapFree,"KiB"),"muted"));
  if(Object.keys(last.cpu).some(id=>id!=="cpu")) {
    const detail=el("details");detail.append(el("summary","查看各逻辑 CPU 时间构成"));
    detail.append(table(["CPU",...Object.values(cpuLabels)],Object.entries(last.cpu).map(([id,values])=>
      [id,...Object.keys(cpuLabels).map(key=>fmt(values[key],"%"))])));
    host.append(detail);
  } else host.append(el("p","当前时间窗只保留主机汇总。逻辑 CPU 和线程详情可从最新一分钟查询。","muted"));
  const kernel=section("内核观测");
  const metrics=[
    ["就绪任务","procs_running","g"],["阻塞任务","procs_blocked","g"],
    ["上下文切换 / 秒","ctxt"],["中断 / 秒","intr"],["主缺页 / 秒","pgmajfault"],
    ["Swap 入页 / 秒","pswpin"],["Swap 出页 / 秒","pswpout"],["OOM 次数 / 秒","oom_kill"],
    ["TCP 重传段 / 秒","Tcp:RetransSegs"],["监听队列溢出 / 秒","TcpExt:ListenOverflows"],
    ["监听丢弃 / 秒","TcpExt:ListenDrops"],["IP 入丢弃 / 秒","Ip:InDiscards"],
  ];
  if(Object.hasOwn(last.gauges,"psi_cpu_some")) {
    panelChart(kernel,"内核压力 PSI · 10 秒均值",["cpu","memory","io"].map((k,i)=>
      series(k,i,data.series,s=>s.gauges["psi_"+k+"_some"])),{...ctx,unit:"%"});
  } else kernel.append(el("p","当前内核没有暴露 PSI 采集接口。","notice"));
  kernel.append(table(["计数器","当前值"],metrics.map(([name,k,kind])=>
    [name,fmt((kind==="g"?last.gauges:last.rates)[k])])));
  const kernelCharts=el("div",null,"panels");
  for(const [title,keys] of [["调度与中断",["ctxt","intr"]],
    ["TCP 重传及监听溢出",["Tcp:RetransSegs","TcpExt:ListenOverflows","TcpExt:ListenDrops"]],
    ["主要缺页及 Swap",["pgmajfault","pswpin","pswpout"]]]) panelChart(kernelCharts,title,
      keys.map((key,i)=>series(metrics.find(m=>m[1]===key)[0],i,data.series,s=>s.rates[key])),ctx);
  kernel.append(kernelCharts);
  root.append(host);

  const proc=section("服务与进程");
  const processes=[...last.processes].sort((a,b)=>(b.cpuPct??-1)-(a.cpuPct??-1));
  proc.append(el("p","进程 100% = 一颗逻辑 CPU 的整段时间。已忙占比与整机容量分母不同。PSS 分摊共享页，RSS 可能重复计算。","muted"));
  proc.append(table(["服务","进程","单核 CPU","主机容量占比","已忙 CPU 占比","RSS","PSS","线程"],processes.map(p=>[
    p.service,p.name+" / "+p.pid,fmt(p.cpuPct,"%"),fmt(p.capacityPct,"%"),fmt(p.busyPct,"%"),fmt(p.rssKiB,"KiB"),
    fmt(p.pssKiB,"KiB"),p.threads])));
  const pid=select(processes.map(p=>[p.key,p.service+" · "+p.name+" / "+p.pid]),
    processes.some(p=>p.key===ctx.chosen)?ctx.chosen:processes.find(p=>p.service==="NEBULA")?.key);
  const inset=el("div"), plots=[];
  function show() {
    plots.splice(0).forEach(graph=>graph.dispose());
    inset.replaceChildren();
    const p=processes.find(p=>p.key===pid.value);
    if(!p)return;
    ctx.choose(p.key);
    inset.append(el("p","CPU 亲和性 "+(p.affinity||"—")+" · 最近执行核 "+(p.lastCpu??"—")+" · 文件描述符 "+(p.fds??"—"),"notice"),
      table(["指标","值"],Object.entries(fields).map(([key,name])=>[name,fmt(p[key],units[key])]) ));
    if(!last.schedstats) inset.append(el("p","schedstats 未开启，无法给出就绪排队耗时。","muted"));
    const local=el("div",null,"panels");
    panelChart(local,"进程 CPU",["cpuPct","userPct","systemPct"].map((key,i)=>
      series(fields[key],i,data.series,s=>s.processes.find(q=>q.key===p.key)?.[key])),{...ctx,visuals:plots,unit:"%"});
    panelChart(local,"进程内存",["rssKiB","pssKiB"].map((key,i)=>
      series(fields[key],i,data.series,s=>s.processes.find(q=>q.key===p.key)?.[key])),{...ctx,visuals:plots,unit:"KiB"});
    inset.append(local);
    if(p.threadStats?.length) inset.append(el("h3","线程 · 最近采样"+(p.threadTruncated?"（已截断）":"")),
      table(["TID / 名称","单核 CPU","用户态","内核态","最近 CPU","就绪等待 ms/s","被抢占 / 秒"],
        [...p.threadStats].sort((a,b)=>(b.cpuPct??-1)-(a.cpuPct??-1)).map(t=>[t.pid+" / "+t.name,fmt(t.cpuPct,"%"),
          fmt(t.userPct,"%"),fmt(t.systemPct,"%"),t.lastCpu,fmt(t.waitNsPerSec),fmt(t.involuntaryPerSec)])));
    for(const graph of plots)graph.paint?.();
  }
  ctx.visuals.push({paint:()=>plots.forEach(graph=>graph.paint?.()),dispose:()=>plots.forEach(graph=>graph.dispose())});
  pid.onchange=show;
  proc.append(label("查看进程",pid),inset);show();root.append(proc,kernel);
}

export async function renderResources(root,{api,post,bounds,signal,visuals,zoom,view,readHealth}) {
  if(view==="resources") {
    const data=await api("resources",bounds,signal);
    if(signal.aborted) return;
    const ctx={bounds,visuals,zoom,bus:new EventTarget(),chosen:sessionStorage.getItem("nebula-resource-pid"),
      choose:key=>sessionStorage.setItem("nebula-resource-pid",key)};
    machine(root,data,ctx);
    root.append(link("查看同一时段的内核、函数与堆","/?"+new URLSearchParams({view:"profiles",...bounds})));
    return {sampleAt:data.latest?.at};
  }
  const url=new URL(location.href), commit=url.searchParams.get("section")==="commit";
  const routes=el("nav",null,"profile-sections");
  routes.setAttribute("aria-label","剖析视图");
  for(const [name,isCommit] of [["函数与内核",false],["提交诊断",true]]) {
    const target=new URL(url);
    if(isCommit) target.searchParams.set("section","commit");else target.searchParams.delete("section");
    for(const key of ["profile","sample","fn","call","span"]) target.searchParams.delete(key);
    const item=link(name,target);
    if(isCommit===commit) item.setAttribute("aria-current","page");
    routes.append(item);
  }
  root.append(routes);
  const reports=section(commit?"PostgreSQL 提交诊断":"压测与函数剖析报告");
  if(commit) {
    reports.classList.add("commit-report");
    await reportPicker(reports,{api,signal,commit});
    root.append(reports);
    return;
  }
  const currentFunctions=el("div"), profiles=el("div");
  root.append(currentFunctions,reports,profiles);
  await Promise.all([
    reportPicker(reports,{api,signal,commit}),
    readHealth().catch(()=>null).then(health=>
      renderFunctionPerformance(currentFunctions,{api,bounds,signal,visuals,zoom,batch:health?.seriesBatch})),
    api("workspace",null,signal).then(workspace=>{
      if(!signal.aborted) pyroscope(profiles,workspace.sources.filter(s=>s.type==="pyroscope"),{post,bounds,signal});
    }).catch(error=>{
      if(!signal.aborted) profiles.append(el("p","无法读取 Pyroscope 数据源 · "+error.message,"notice"));
    }),
  ]);
}

async function reportPicker(box,{api,signal,commit}) {
  let reports;
  try {
    reports=(await api("resources/reports",null,signal)).filter(report=>(report.kind==="commit")===commit);
  } catch(error) {
    if(!signal.aborted) box.append(el("p","无法读取报告列表 · "+error.message,"notice"));
    return;
  }
  if(signal.aborted) return;
  const output=el("div"), status=el("p",null,"notice profile-status");
  status.setAttribute("role","status");
  status.hidden=true;
  const requested=new URL(location.href).searchParams.get("profile");
  const chosen=requested??reports[0]?.id??"";
  const list=select([["","选择报告"],...reports.map(r=>[r.id,r.label+" · "+(commit?"归档于 ":"")+timestamp(r.started,true)])],
    reports.some(report=>report.id===chosen)?chosen:"");
  list.disabled=!reports.length;
  box.append(label("实验报告",list),status,output);
  const say=message=>{status.textContent=message;status.hidden=!message;};
  let revision=0;
  async function open(updateUrl) {
    if(signal.aborted) return;
    const current=++revision;
    output.replaceChildren();
    if(updateUrl) {
      const url=new URL(location.href);
      url.searchParams.set("profile",list.value);
      if(commit) url.searchParams.delete("sample");
      history.replaceState(null,"",url);
    }
    if(!reports.length) {
      say(commit?"暂无提交诊断报告。可归档 Linux 隔离实验输出的 report.json。"
        :"暂无实验剖析报告。可用 resource_lab 在 Linux 启动隔离实验。");
      return;
    }
    if(!list.value) {
      say(!updateUrl&&chosen?"这份报告在当前视图中不可用。请选择另一份报告。":"请选择要查看的实验报告。");
      return;
    }
    say("正在读取报告…");
    try {
      const report=await api("resources/reports/"+encodeURIComponent(list.value),null,signal);
      if(current!==revision || signal.aborted) return;
      if(commit) renderCommitReport(output,report);else pickedReport(report,output);
      if(!updateUrl&&requested===null) {
        const url=new URL(location.href);
        url.searchParams.set("profile",report.id);
        history.replaceState(null,"",url);
      }
      say("");
    } catch(error) {
      if(current===revision&&!signal.aborted) say("无法读取报告 · "+error.message);
    }
  }
  list.onchange=()=>open(true);
  await open(false);
}
