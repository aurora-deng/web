import {timestamp} from "./chart.js";
import {exportButton,saveCommitChart} from "./chart_export.js";

const format=new Intl.NumberFormat("zh-CN",{maximumFractionDigits:3});
const measure=(value,unit="")=>value==null?"—":format.format(value)+(unit?" "+unit:"");
const ms=value=>measure(value,"ms");
const micros=value=>ms(value==null?null:value/1000);
const events=counts=>Object.entries(counts).sort((a,b)=>b[1]-a[1]||a[0].localeCompare(b[0]));

function node(tag,text,className) {
  const element=document.createElement(tag);
  if(text!=null) element.textContent=text;
  if(className) element.className=className;
  return element;
}

function facts(rows,className) {
  const result=node("dl",null,className);
  for(const [label,value,note] of rows) {
    const item=node("div");
    item.append(node("dt",label),node("dd",value));
    if(note) item.append(node("small",note));
    result.append(item);
  }
  return result;
}

function diskReadings(devices,delta) {
  if(!Object.keys(delta).length) return node("p","区间内没有足够的可比较磁盘样本。","commit-subtle");
  const wrap=node("div",null,"table-wrap");
  wrap.tabIndex=0;
  wrap.setAttribute("role","region");
  wrap.setAttribute("aria-label","磁盘读数");
  const table=node("table"), head=node("thead"), body=node("tbody");
  const headings=node("tr");
  for(const title of ["设备","采样间隔","写入次数","写入扇区","写入累计时间","刷新次数","刷新累计时间"])
    headings.append(node("th",title));
  head.append(headings);
  for(const [id,change] of Object.entries(delta)) {
    const row=node("tr");
    const values=[devices[id]+" ("+id+")",ms(change.observedMs),measure(change.writes),
      measure(change.sectorsWritten),ms(change.writeMs),measure(change.flushes),ms(change.flushMs)];
    for(const value of values) row.append(node("td",value));
    body.append(row);
  }
  table.append(head,body);
  wrap.append(table);
  return wrap;
}

function waitList(counts,emptyMessage) {
  const sorted=events(counts);
  if(!sorted.length) return node("p",emptyMessage,"commit-subtle");
  const list=node("ul",null,"commit-wait-list");
  for(const [event,count] of sorted) {
    const item=node("li");
    item.append(node("code",event==="active"?"执行中（未报告等待事件）":event),node("strong",measure(count,"次")));
    list.append(item);
  }
  return list;
}

function individualCommit(report) {
  const box=node("section",null,"commit-analysis");
  const list=node("div",null,"commit-samples");
  const heading=node("div",null,"commit-samples-heading");
  heading.append(node("h3","最慢提交"),node("small","按 COMMIT 耗时排序 · "+report.longest.length+" 条"));
  heading.append(exportButton("最慢提交",()=>saveCommitChart(report)));
  list.append(heading);
  const detail=node("div",null,"commit-sample-detail");
  detail.id="commit-sample-detail";
  const widest=report.longest[0].commitUs || 1;
  const buttons=[];
  function choose(index) {
    buttons.forEach((button,i)=>button.setAttribute("aria-pressed",String(i===index)));
    const sample=report.longest[index];
    const observed=Object.values(sample.backendWaitSamples).reduce((sum,count)=>sum+count,0);
    detail.replaceChildren(node("h3","第 "+(index+1)+" 条 · COMMIT "+micros(sample.commitUs)),
      facts([
        ["客户端 ACK",ms(sample.ackMs)],
        ["业务排队",micros(sample.queueUs)],
        ["提交开始偏移",ms(sample.commitStartOffsetMs)],
        ["区间内匹配样本",observed+" / "+sample.pollsWithinCommit],
      ],"commit-sample-facts"),
      node("h4","数据库进程状态"),
      waitList(sample.backendWaitSamples,"这次提交没有匹配到数据库进程样本。"),
      node("h4","提交区间内磁盘变化"),
      diskReadings(report.devices,sample.diskDelta));
  }
  const scroller=node("div",null,"commit-sample-list");
  report.longest.forEach((sample,index)=>{
    const button=node("button",null,"commit-sample");
    button.type="button";
    button.setAttribute("aria-controls",detail.id);
    const track=node("span",null,"commit-sample-track");
    track.setAttribute("aria-hidden","true");
    const bar=node("span");
    bar.style.width=100*sample.commitUs/widest+"%";
    track.append(bar);
    button.append(node("span",String(index+1).padStart(2,"0"),"commit-sample-rank"),
      node("strong",micros(sample.commitUs)),track,
      node("small",Object.keys(sample.backendWaitSamples).length?"有采样":"未匹配"));
    button.onclick=()=>{
      choose(index);
      const url=new URL(location.href);
      url.searchParams.set("profile",report.id);
      url.searchParams.set("sample",String(index+1));
      history.replaceState(null,"",url);
      if(matchMedia("(max-width:850px)").matches) detail.scrollIntoView({block:"nearest"});
    };
    buttons.push(button);
    scroller.append(button);
  });
  list.append(scroller);
  box.append(list,detail);
  const params=new URL(location.href).searchParams;
  const sample=params.get("profile")===report.id?Number(params.get("sample")):0;
  choose(Number.isInteger(sample)&&sample>=1&&sample<=buttons.length?sample-1:0);
  return box;
}

function fullRun(report) {
  const section=node("details",null,"commit-evidence");
  const wal=report.walDelta;
  const postgres=report.postgresSettings;
  const pgVersion=Math.floor(postgres.server_version_num/10000)+"."+postgres.server_version_num%10000;
  section.append(node("summary","整轮 WAL、磁盘与采样设置"),node("div",
    "PostgreSQL "+pgVersion+" · fsync "+postgres.fsync+" · synchronous_commit "+postgres.synchronous_commit
      +" · track_wal_io_timing "+postgres.track_wal_io_timing+" · "+postgres.wal_sync_method,"commit-subtle"),
    node("p","二进制 SHA-256 "+report.binarySha256,"commit-subtle"),
    wal ? facts([
      ["WAL 同步",measure(wal.fsyncCount,"次")],
      ["WAL 同步累计",ms(wal.fsyncMilliseconds)],
      ["WAL 写入",measure(wal.writeCount,"次")],
      ["WAL 写入累计",ms(wal.writeMilliseconds)],
      ["WAL 字节",measure(wal.walBytes,"B")],
    ],"commit-sample-facts") : node("p","当前报告没有可用的 WAL 差值。","commit-subtle"),
    diskReadings(report.devices,report.diskDelta),
    node("p","WAL 差值覆盖近似负载窗口。磁盘差值只覆盖表中的采样间隔，逻辑卷、分区与块设备应分别读数。","commit-subtle"));
  return section;
}

export function renderCommitReport(root,report) {
  const header=node("header",null,"commit-report-heading");
  const title=node("div");
  title.append(node("h3",report.label),node("p","归档于 "+timestamp(report.started,true)
    +" · "+report.workers+" 个工作端 · "+measure(report.operations)+" 次混合操作 · 耗时 "+measure(report.durationMs/1000,"秒"),"muted"));
  header.append(title,node("span",report.controlDelayMs?"注入延迟 "+ms(report.controlDelayMs):"自然负载",
    report.controlDelayMs?"commit-kind commit-control":"commit-kind"));
  const stats=facts([
    ["客户端 ACK P99",ms(report.ackMs.p99),"P50 "+ms(report.ackMs.p50)+" · P95 "+ms(report.ackMs.p95)],
    ["COMMIT P99",micros(report.commitUs.p99),"P50 "+micros(report.commitUs.p50)+" · P95 "+micros(report.commitUs.p95)],
    ["慢提交采样覆盖",report.slow?report.slowCovered+" / "+report.slow:"本轮没有",
      "慢提交门槛 ≥ "+ms(report.slowThresholdMs)],
    ["最大轮询间隔",ms(report.maxPollGapMs),
      "目标 "+ms(report.sampleIntervalMs)+" · "+measure(report.waitPolls,"次轮询")],
  ],"commit-stats");
  const coverage=node("section",null,"commit-coverage");
  coverage.append(node("h3","慢提交的等待样本"),
    node("p",measure(report.paired)+" 条已配对，"+measure(report.covered)+" 条匹配到数据库进程样本。采样次数无法换算为各等待的耗时。","commit-subtle"),
    waitList(report.slowWaitSamples,report.slow?"慢提交未匹配到数据库进程样本。":"本轮没有达到门槛的提交。"));
  if(report.sampleErrors||report.sampleLimitReached) {
    const warning=["采样查询错误 "+measure(report.sampleErrors)+" 次"];
    if(report.sampleLimitReached) warning.push("已达到轮询上限，后段覆盖可能不完整");
    coverage.append(node("p",warning.join(" · "),"commit-warning"));
  }
  root.replaceChildren(header,stats,coverage,individualCommit(report),fullRun(report));
}
