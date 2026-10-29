/* Synthetic browser contracts. Provider fixtures are not evidence of a real cloud Key. */
const {chromium}=require(process.env.PLAYWRIGHT_MODULE),http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../../..'),output=path.join(root,'build-client/phase43-ui/workspace');fs.mkdirSync(output,{recursive:true});let generationPosts=0,ownCalls=0;
const server=http.createServer(async(req,res)=>{
  const url=new URL(req.url,'http://localhost');let body='';for await(const part of req)body+=part;let data={};try{data=JSON.parse(body||'{}');}catch{}
  const json=(value,status=200)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(value));};
  if(url.pathname==='/v1/chat/completions'){ownCalls++;return req.headers.authorization==='Bearer fixture-only-key'?json({choices:[{message:{content:'自带 API 真实协议替身回答'}}]}):json({error:'unauthorized'},401);}
  if(url.pathname.startsWith('/api/')||url.pathname==='/events'){
    if(url.pathname==='/api/me')return json({user:{id:'1',username:'traveler',displayName:'树下旅人'},csrfToken:'fixture-csrf'});
    if(url.pathname==='/api/local/session/info')return json({available:false});
    if(url.pathname==='/api/ai/web/models')return json([{accountId:'1',name:'本站模型',runtime:'onnx'}]);
    if(url.pathname==='/api/ai/web/generations'&&req.method==='POST'){generationPosts++;return json({id:data.requestId,state:'queued'},data.nonConfidentialConsent==='true'?202:403);}
    if(url.pathname.startsWith('/api/ai/web/generations/'))return json(req.method==='DELETE'?{ok:true}:{state:'completed',pieces:['本站模型协议替身回答'],next:1});
    if(url.pathname==='/api/admin/me')return json({role:'administrator'});
    if(url.pathname==='/api/admin/users')return json([]);
    if(url.pathname==='/api/tree/categories')return json([{id:'1',name:'生活'}]);
    if(url.pathname==='/api/tree/posts')return json({posts:[],total:'0'});
    if(url.pathname==='/api/tree/ranking')return json({entries:[]});
    if(url.pathname==='/api/me/notification-settings')return json({sound:false,voice:'zf_001'});
    if(url.pathname==='/api/me/speech/status')return json({enabled:false});
    if(url.pathname==='/api/profiles/1')return json({id:'1',username:'traveler',displayName:'树下旅人',self:true,allowTreeFriendRequests:false,audience:'self'});
    if(url.pathname==='/api/tree/posts/42/author')return json({id:'2',username:'author',displayName:'果实作者',canRequestFriendship:true});
    if(url.pathname==='/api/tree/posts/42/friend-request')return json({id:'9',state:'pending'},201);
    return json([]);
  }
  let file=path.resolve(root,'frontend','.'+decodeURIComponent(url.pathname));if(!file.startsWith(path.join(root,'frontend')+path.sep)){res.writeHead(403);return res.end();}
  if(fs.existsSync(file)&&fs.statSync(file).isDirectory())file=path.join(file,'index.html');if(!fs.existsSync(file)){res.writeHead(404);return res.end();}
  res.setHeader('Content-Type',({'.js':'text/javascript','.css':'text/css','.html':'text/html','.svg':'image/svg+xml'})[path.extname(file)]||'application/octet-stream');res.end(fs.readFileSync(file));
});
(async()=>{await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true});const context=await browser.newContext({viewport:{width:1440,height:900}}),page=await context.newPage(),errors=[],report={kind:'synthetic UI/API fixtures',themes:[]};page.on('pageerror',e=>errors.push(e.message));
 try{
  for(const route of ['/phase11/tree/','/phase11/admin/','/phase11/profile/','/phase11/assistant/']){
   await page.goto(base+route);await page.waitForSelector('[data-theme-toggle]');await page.waitForTimeout(200);
   await page.evaluate(()=>localStorage.setItem('nebula:theme','dark'));await page.reload();await page.waitForTimeout(200);
   const color=()=>page.evaluate(()=>({header:getComputedStyle(document.querySelector('#nebula-navigation')).backgroundColor,body:getComputedStyle(document.body).backgroundColor}));const dark=await color();await page.locator('[data-theme-toggle]').click();await page.waitForTimeout(80);const light=await color();assert.notDeepEqual(dark,light,'theme has no visible effect '+route);assert.notEqual(dark.header,light.header);assert.notEqual(dark.body,light.body);
   await page.reload();await page.waitForTimeout(150);assert.equal(await page.locator('html').getAttribute('data-theme'),'light');report.themes.push({route,dark,light});
   const order=await page.locator('.workspace-tools').evaluate(e=>[...e.children].map(n=>n.className));assert(order.indexOf('workspace-theme')<order.indexOf('workspace-assistant'));assert(order.indexOf('workspace-assistant')<order.indexOf('sound-menu'));
   await page.setViewportSize({width:390,height:844});await page.waitForTimeout(150);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+2),'mobile overflow '+route);await page.screenshot({path:path.join(output,route.split('/')[2]+'-390.png')});await page.setViewportSize({width:1440,height:900});
  }
  await page.goto(base+'/phase11/assistant/');await page.waitForFunction(()=>document.querySelector('.assistant-status')?.textContent.includes('网页登录已就绪'));
  await page.locator('.assistant-composer textarea').fill('未确认时不能发送');await page.locator('.assistant-composer [type=submit]').click();assert.equal(generationPosts,0);
  await page.locator('[data-web-consent]').check();await page.locator('.assistant-composer [type=submit]').click();await page.waitForFunction(()=>document.querySelector('.assistant-messages')?.textContent.includes('本站模型协议替身回答'));assert.equal(generationPosts,1);
  await page.locator('[data-connection]').click();await page.locator('[name=provider]').selectOption('custom');await page.locator('[name=endpoint]').fill(base+'/v1/chat/completions');await page.locator('[name=model]').fill('fixture-chat');await page.locator('[name=key]').fill('wrong-key');await page.locator('[name=consent]').check();await page.locator('[data-test]').click();await page.waitForFunction(()=>document.querySelector('.model-connection [role=status]')?.textContent.includes('401'));
  await page.locator('[name=key]').fill('fixture-only-key');await page.locator('[data-test]').click();await page.waitForFunction(()=>document.querySelector('.model-connection [role=status]')?.textContent.includes('连接成功'));await page.locator('.model-connection [type=submit]').click();
  await page.locator('.assistant-composer textarea').fill('自带接口对话');await page.locator('.assistant-composer [type=submit]').click();await page.waitForFunction(()=>document.querySelector('.assistant-status')?.textContent.includes('回答完成'));assert.equal(ownCalls,3);assert.equal(generationPosts,1,'custom API unexpectedly sent to Linux');
  const stored=await page.evaluate(()=>JSON.stringify({...localStorage,...sessionStorage}));assert(!stored.includes('fixture-only-key'));
  await page.locator('.sound-menu summary').click();await page.locator('[data-speech-settings]').click();await page.waitForFunction(()=>document.querySelector('.audio-settings-links p')?.textContent.includes('Windows'));
  await page.evaluate(async()=>{const{openTreeAuthor}=await import('/phase11/shared/TreeAuthorCard/TreeAuthorCard.js');const{BusinessApi}=await import('/phase11/shared/BusinessApi/BusinessApi.js');const api=new BusinessApi();await api.restore();await openTreeAuthor(api,{id:'42'});});
  await page.getByRole('button',{name:'申请加为好友',exact:true}).click();await page.getByRole('button',{name:'申请已发送',exact:true}).waitFor();assert(await page.getByRole('button',{name:'申请已发送',exact:true}).isDisabled());await page.getByRole('button',{name:'关闭作者卡'}).click();
  await page.screenshot({path:path.join(output,'assistant-and-api.png')});assert.deepEqual(errors,[]);report.passed=true;report.checks=['visible light/dark and reload persistence across four pages','mobile no horizontal overflow','theme-assistant-notification order','web consent before upload','server model completion','custom API 401 and successful real HTTP fixture roundtrip','no API key in browser storage or Linux proxy','speech settings discoverability','fruit author friend request UI'];console.log(JSON.stringify(report,null,2));
 }finally{fs.writeFileSync(path.join(output,'summary.json'),JSON.stringify(report,null,2));await browser.close();server.close();}
})().catch(e=>{console.error(e);server.close();process.exitCode=1;});
