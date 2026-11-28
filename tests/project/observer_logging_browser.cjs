/* Edge CDP smoke against a real Linux lab with an imported resource report. */
const assert=require("node:assert/strict");
const {spawn}=require("node:child_process");
const fs=require("node:fs");
const path=require("node:path");
const base=process.env.OBSERVER_RD_URL || "http://127.0.0.1:18793";
const functionWindow=process.env.OBSERVER_RD_TRACE_WINDOW;
const commitReport=process.env.OBSERVER_RD_COMMIT_REPORT;
const commitControl=process.env.OBSERVER_RD_COMMIT_CONTROL;
const liveTrend=process.env.OBSERVER_RD_LIVE_TREND;
const pageBoard=process.env.OBSERVER_RD_PAGE_BOARD;
const directory=path.resolve("build-function3","obs-qa","logging-browser-"+Date.now());
const wait=ms=>new Promise(done=>setTimeout(done,ms));

async function eventually(test,label) {
  for(let attempt=0;attempt<110;attempt++) {
    if(await test()) return;
    await wait(130);
  }
  throw Error("Timed out: "+label);
}

async function run() {
  fs.mkdirSync(directory,{recursive:true});
  const edge=spawn(process.env.OBSERVER_RD_EDGE || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    ["--headless=new","--no-first-run","--disable-extensions","--user-data-dir="+directory,"--remote-debugging-port=0","about:blank"],
    {windowsHide:true,stdio:"ignore"});
  let socket;
  try {
    const portFile=path.join(directory,"DevToolsActivePort");
    await eventually(()=>fs.existsSync(portFile),"CDP port");
    const port=fs.readFileSync(portFile,"utf8").split(/\r?\n/)[0];
    const tabs=await (await fetch("http://127.0.0.1:"+port+"/json/list")).json();
    socket=new WebSocket(tabs.find(tab=>tab.type==="page").webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    let id=0;
    const pending=new Map(),errors=[];
    socket.onmessage=({data})=>{
      const message=JSON.parse(data);
      if(message.method==="Runtime.exceptionThrown") errors.push(message.params.exceptionDetails);
      const done=pending.get(message.id);
      if(done){pending.delete(message.id);message.error?done.reject(message.error):done.resolve(message.result);}
    };
    const call=(method,params={})=>new Promise((resolve,reject)=>{
      const seq=++id;pending.set(seq,{resolve,reject});socket.send(JSON.stringify({id:seq,method,params}));
    });
    const js=async expression=>{
      const result=await call("Runtime.evaluate",{expression,returnByValue:true,awaitPromise:true});
      assert(!result.exceptionDetails,result.exceptionDetails?.exception?.description);
      return result.result.value;
    };
    const present=(label,selector="#body")=>eventually(()=>js("document.querySelector("+JSON.stringify(selector)+")?.textContent.includes("+JSON.stringify(label)+")"),label);
    const click=(label,selector="#body button")=>js("[...document.querySelectorAll("+JSON.stringify(selector)+")].find(x=>x.textContent.trim()==="+JSON.stringify(label)+")?.click()");
    const page=async(view)=>call("Page.navigate",{url:base+"/?view="+view});
    const width=async(width,height)=>call("Emulation.setDeviceMetricsOverride",{width,height,mobile:false,deviceScaleFactor:1});
    const screenshot=async name=>{
      const result=await call("Page.captureScreenshot",{format:"png"});
      fs.writeFileSync(path.join(directory,name+".png"),Buffer.from(result.data,"base64"));
    };
    const exportPng=async selector=>{
      await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:directory});
      const earlier=new Set(fs.readdirSync(directory));
      await eventually(()=>js("!!document.querySelector("+JSON.stringify(selector)+")"),"PNG export control");
      await js("document.querySelector("+JSON.stringify(selector)+").click()");
      let downloaded;
      try {
        await eventually(()=>{
          downloaded=fs.readdirSync(directory).find(name=>name.startsWith("NEBULA-")&&name.endsWith(".png")&&!earlier.has(name));
          return downloaded&&fs.statSync(path.join(directory,downloaded)).size>512;
        },"PNG file downloaded");
      } catch(error) {
        const failure=await js("document.querySelector("+JSON.stringify(selector)+")?.title");
        throw new Error(failure||error.message);
      }
      const png=fs.readFileSync(path.join(directory,downloaded));
      assert.equal(png.subarray(0,8).toString("hex"),"89504e470d0a1a0a");
      assert(png.readUInt32BE(16)>500 && png.readUInt32BE(20)>160);
    };

    await call("Page.enable");await call("Runtime.enable");
    await width(1440,900);
    await page("logs");await present("日志工作台");
    await present("查询成功",".query-feedback");
    await eventually(()=>js('document.querySelectorAll(".log-table tbody tr[data-log-id]").length>0'),"real diagnostic rows");
    assert(await js('!!document.querySelector(".log-table a[href*=traces]")'));
    const before=await js('window.originalLogTable=document.querySelector(".log-table");true');
    await wait(2300);
    assert(await js('document.querySelector(".log-table")===window.originalLogTable'));
    await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:directory});
    await click("导出当前日志 JSON");
    let downloaded;
    await eventually(()=>{downloaded=fs.readdirSync(directory).find(name=>name.startsWith("NEBULA-logs-")&&name.endsWith(".json"));return !!downloaded;},"structured log export");
    const exported=JSON.parse(fs.readFileSync(path.join(directory,downloaded),"utf8"));
    assert(exported.items.length>0&&exported.items.every(row=>typeof row.timeUnixNano==="string"));
    // A safe rejected request produces a correlated warning, without storing the query text.
    const requestId=await js('fetch("/api/v1/series?panel=invalid-log-smoke").then(r=>r.headers.get("X-Observer-Request-ID"))');
    assert(/^[0-9a-f]{16}$/.test(requestId));
    await js('document.querySelector("[aria-label=日志级别]").value="WARN";document.querySelector("[aria-label=日志关键词]").value="校验拒绝";document.querySelector(".log-filters").requestSubmit()');
    await present("监控请求被校验拒绝",".log-table");
    await eventually(()=>js('document.querySelectorAll(".log-table tbody tr[data-log-id]").length>0&&[...document.querySelectorAll(".log-table tbody tr[data-log-id]")].every(r=>r.dataset.level==="WARN")'),"WARN filter completed");
    await screenshot("structured-logs");
    await js('document.querySelector("[aria-label=日志级别]").value="";document.querySelector("[aria-label=日志关键词]").value='+JSON.stringify(requestId)+';document.querySelector(".log-filters").requestSubmit()');
    await eventually(()=>js('document.querySelectorAll(".log-table tbody tr[data-log-id]").length===1&&document.querySelector(".log-table tbody").textContent.includes('+JSON.stringify(requestId)+')'),"request ID lookup");
    await screenshot("request-id-lookup");
    await js('document.querySelector("[aria-label=日志级别]").value="";document.querySelector("[aria-label=日志关键词]").value="";document.querySelector(".log-filters").requestSubmit()');
    await present("查询成功",".query-feedback");
    const logLink=await js('document.querySelector(".log-table a[href*=traces]")?.href');
    assert(logLink);await call("Page.navigate",{url:logLink});
    await present("查看结构化业务日志");
    const reverse=await js('[...document.querySelectorAll("#body a")].find(n=>n.textContent==="查看结构化业务日志").href');
    assert(reverse.includes("logTrace="));await call("Page.navigate",{url:reverse});
    await present("查询成功",".query-feedback");
    assert(await js('document.querySelectorAll(".log-table tbody tr[data-log-id]").length>0'));
    await width(390,844);await screenshot("logs-mobile");
    assert(await js('document.documentElement.scrollWidth<=390'));
    await width(1440,900);await page("explore");await present("Explore · PromQL 与直方图");
    await js(`(()=>{const group=document.querySelector('[aria-label="A 指标族"]');group.value="function";group.dispatchEvent(new Event("change"));[...document.querySelectorAll('[data-query-ref=A] button')].find(n=>n.textContent==="运行查询").click();})()`);
    await eventually(()=>js('document.querySelector("[data-result-ref=A]")?.textContent.includes("ChatService::")'),"actual function histogram expression");
    await js(`document.querySelector('[aria-label="A 复制查询"]').click()`);
    assert.equal(await js('document.querySelectorAll(".query-row").length'),2);
    await js(`(()=>{const b=document.querySelector("[data-query-ref=B]");for(const [label,value] of [["B 结果格式","table"],["B 查询类型","instant"]]){const n=b.querySelector('[aria-label="'+label+'"]');n.value=value;n.dispatchEvent(new Event("change"));}[...b.querySelectorAll("button")].find(n=>n.textContent==="运行查询").click();})()`);
    await eventually(()=>js('!!document.querySelector("[data-result-ref=B] table tbody tr")'),"instant table result");
    assert(await js('document.querySelector("[data-result-ref=B]").textContent.includes("instant")'));
    await js(`document.querySelector('[aria-label="A 显隐查询"]').click()`);
    assert(await js('!document.querySelector("[data-result-ref=A]")'));
    // A real Prometheus accepts unknown-invalid-expression as subtraction.
    // Use malformed syntax to exercise the explicit query-error state.
    await js('(()=>{const b=document.querySelector("[data-query-ref=B]");[...b.querySelectorAll("button")].find(n=>n.textContent==="Code").click();const q=b.querySelector("textarea");q.value="sum(";q.dispatchEvent(new Event("input"));[...b.querySelectorAll("button")].find(n=>n.textContent==="运行查询").click();})()');
    await present("查询失败","[data-result-ref=B]");
    await js(`document.querySelector('[aria-label="B 删除查询"]').click()`);
    assert.equal(await js('document.querySelectorAll(".query-row").length'),1);
    await js(`document.querySelector('[aria-label="A 显隐查询"]').click();[...document.querySelectorAll("[data-query-ref=A] button")].find(n=>n.textContent==="运行查询").click()`);
    await eventually(()=>js('!!document.querySelector("[data-result-ref=A] svg")'),"valid query recovers");
    await screenshot("query-editor");await width(390,844);await screenshot("query-editor-mobile");
    assert(await js('document.documentElement.scrollWidth<=390'));
    await width(1440,900);await page("profiles");await present("当前目标：");
    await present("已收到 nebula_function_seconds");
    await js('document.querySelector("[aria-label=业务域]").value="chat";document.querySelector("[aria-label=业务域]").dispatchEvent(new Event("change"))');
    await eventually(()=>js('[...document.querySelectorAll(".function-stats tbody tr")].length===4'),"four instrumented functions");
    const quantified=await js('[...document.querySelectorAll(".function-stats tbody tr")].map(r=>({fn:r.cells[0].textContent,p99:r.cells[4].textContent}))');
    assert(quantified.every(row=>row.p99!=="—"),JSON.stringify(quantified));
    await screenshot("actual-function-times");
    const catalog=await (await fetch(base+"/api/v1/function-catalog")).json();
    if(catalog.functions.some(row=>row.key==="tree_list")) {
      const count=catalog.functions.length;
      await present(`${count} / ${count} 个目录埋点`);
      await js('document.querySelector("[aria-label=业务域]").value="tree";document.querySelector("[aria-label=业务域]").dispatchEvent(new Event("change"))');
      await eventually(()=>js('document.querySelectorAll(".function-stats tbody tr").length===7'),"seven tree operations");
      const tree=await js('[...document.querySelectorAll(".function-stats tbody tr")].map(r=>({fn:r.dataset.function,p99:r.cells[4].textContent,inFlight:r.cells[6].textContent}))');
      assert(tree.every(row=>row.p99!=="—"&&row.inFlight!=="—"),JSON.stringify(tree));
      await js('[...document.querySelector("tr[data-function=tree_list]").querySelectorAll("button")].find(n=>n.textContent==="看调用链").click()');
      await present("PostgreSQL command",".function-traces");
      await screenshot("tree-business-sql-trace");
      await width(390,844);await screenshot("business-functions-mobile");
      assert(await js('document.documentElement.scrollWidth<=390'));
      await width(1920,1080);await screenshot("business-functions-1920");
      assert(await js('document.documentElement.scrollWidth<=1920'));
      await width(1440,900);
      await js('document.querySelector("[aria-label=业务域]").value="ai";document.querySelector("[aria-label=业务域]").dispatchEvent(new Event("change"))');
      assert.equal(await js('document.querySelectorAll(".function-stats tbody tr").length'),5);
      assert(await js('[...document.querySelectorAll(".function-stats tbody tr")].every(r=>r.cells[4].textContent==="—")'));
      await screenshot("ai-no-calls-is-not-zero");
      await page("traces");
      await eventually(()=>js('!!document.querySelector("[aria-label=链路操作] option[value="+JSON.stringify("GET /api/tree/posts")+"]")'),"tree HTTP operation selectable");
      await js('document.querySelector("[aria-label=链路操作]").value="GET /api/tree/posts";document.querySelector("[aria-label=链路操作]").form.requestSubmit()');
      await present("GET /api/tree/posts");
    }
    if(process.env.OBSERVER_RD_DAILY_URL){
      await call("Page.navigate",{url:process.env.OBSERVER_RD_DAILY_URL+"/?view=profiles"});
      if(process.env.OBSERVER_RD_DAILY_INSTRUMENTED){
        await present("当前目标：127.0.0.1:18090");
        await present("已收到 nebula_function_seconds");
        await js('document.querySelector("[aria-label=业务域]").value="chat";document.querySelector("[aria-label=业务域]").dispatchEvent(new Event("change"))');
        await eventually(()=>js('[...document.querySelectorAll(".function-stats tbody tr")].filter(r=>!r.cells[0].textContent.includes("sendMessage")).length===3'),"daily read functions");
        const reads=await js('[...document.querySelectorAll(".function-stats tbody tr")].filter(r=>!r.cells[0].textContent.includes("sendMessage")).map(r=>r.cells[4].textContent)');
        assert(reads.every(value=>value!=="—"),JSON.stringify(reads));
        await screenshot("daily-actual-function-times");
        const trace=process.env.OBSERVER_RD_DAILY_TRACE;
        if(trace){
          assert(/^[a-f0-9]{32}$/.test(trace));
          await call("Page.navigate",{url:process.env.OBSERVER_RD_DAILY_URL+"/?view=traces&trace="+trace});
          await present("probe GET /api/conversations");
          await present("PostgreSQL command");
          await screenshot("daily-real-read-trace");
        }
      }else{
        await present("当前目标未提供函数耗时数据");await present("打开已埋点的隔离实例");
        assert(await js('document.querySelector(".instrumentation-status a").href.includes(":18793/")'));
        await screenshot("missing-function-guidance");
      }
    }
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log(JSON.stringify({passed:true,functionTimings:quantified,artifacts:directory}));
  } finally {socket?.close();edge.kill();}
}
run().catch(error=>{console.error(error);process.exitCode=1;});
