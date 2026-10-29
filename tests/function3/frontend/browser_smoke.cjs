/* 真实浏览器+真实前端，HTTP 数据为明确的测试替身；不代替 Linux 业务集成测试。 */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=path.resolve(__dirname,'../../..');
let role='administrator',writes=[],modelState='candidate';
const categories=[{id:'1',name:'生活',active:true},{id:'2',name:'学习',active:true}];
const posts=Array.from({length:24},(_,i)=>({id:String(24-i),authorId:'2',authorName:i%2?'小满':'测试用户',body:['今天的小确幸：终于读懂了那段代码。','在树下，留住一段平凡又闪亮的日常。','晚风温柔，生活也在慢慢变好。'][i%3],categoryId:'1',mood:'平静',tags:['日常'],media:[],createdAt:Date.now()-i*60000,likes:String(i),repliers:'2',liked:false}));
const server=http.createServer(async(req,res)=>{
 const url=new URL(req.url,'http://localhost');const json=(value,status=200)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(value));};
 if(url.pathname==='/events'){res.writeHead(200,{'Content-Type':'text/event-stream'});res.write(': ready\n\n');return;}
 if(req.method==='POST'){let body='';for await(const chunk of req)body+=chunk;const fields=JSON.parse(body||'{}');writes.push({path:url.pathname,fields,csrf:req.headers['x-csrf-token']});
  if(url.pathname==='/api/activities')return json({...posts[0],id:'25',body:fields.body},201);
  if(url.pathname==='/api/admin/speech/preview')return json({state:'pending'});
  if(url.pathname==='/api/admin/models/1/versions/2'){modelState='active';return json({operationId:'1',state:'pending'},202);}
  if(url.pathname.endsWith('/like')){const p=posts.find(p=>url.pathname.includes('/'+p.id+'/'));p.liked=fields.liked==='true';return json(p);}
  return json({ok:true});
 }
 if(url.pathname==='/api/me')return json({user:{id:'1',displayName:'测试账号'},csrfToken:'synthetic-csrf'});
 if(url.pathname==='/api/admin/me')return json({id:'1',role});
 if(url.pathname.startsWith('/api/admin/speech/preview/'))return json({state:'ready',url:'/preview.wav'});
 if(url.pathname.startsWith('/health/'))return json({status:'ok'});
 if(url.pathname==='/metrics'){res.writeHead(200,{'Content-Type':'text/plain'});return res.end('webserver_connections_active 12\n');}
 if(url.pathname==='/api/tree/categories'||url.pathname==='/api/admin/tree/categories')return json(categories);
 if(url.pathname==='/api/tree/posts')return json({posts,total:'100000',next:'1'});
 if(url.pathname==='/api/tree/ranking')return json({version:'1',settledAt:Date.now(),entries:posts.slice(0,10).map((p,i)=>({postId:p.id,score:30-i}))});
 if(url.pathname==='/api/me/notification-settings')return json({sound:false,voice:'zf_001'});
 if(url.pathname==='/api/announcements')return json([]);
 if(url.pathname==='/api/admin/models')return json([{id:'1',name:'测试模型',activeVersion:'0'}]);
 if(url.pathname==='/api/admin/models/1/versions')return json([{id:'2',runtime:'onnx-inprocess',state:modelState,checksum:'fixture'}]);
 if(url.pathname==='/api/admin/model-operations/1')return json({id:'1',state:'completed'});
 if(url.pathname==='/api/admin/users')return json([{id:'2',name:'测试用户',username:'test_user',role:'user',active:true}]);
 if(url.pathname.endsWith('/replies'))return json([{id:'1',authorId:'2',authorName:'小满',body:'愿你的每一天都有小小的快乐。',createdAt:Date.now()}]);
 if(/^\/api\/(?:tree\/posts|activities)\/\d+$/.test(url.pathname))return json(posts.find(p=>p.id===url.pathname.split('/').at(-1))||posts[0]);
 if(url.pathname.startsWith('/api/'))return json([]);
 if(url.pathname==='/virtual-test'){res.writeHead(200,{'Content-Type':'text/html'});return res.end(`<style>#list{height:500px;overflow:auto;padding:0;margin:0;list-style:none;display:flex;flex-direction:column;gap:19px}li{flex-shrink:0}.virtual-spacer{margin:0;pointer-events:none}</style><ul id="list"></ul><script type="module">import{VirtualMessageList}from'/phase11/chat/VirtualMessageList/VirtualMessageList.js';window.list=document.querySelector('#list');window.view=new VirtualMessageList(list,m=>{const n=document.createElement('li');n.dataset.messageId=m.id;n.textContent=m.body;n.style.height=(m.id%3?52:140)+'px';return n;});window.items=Array.from({length:10000},(_,i)=>({id:String(i+1),body:'历史消息'+(i+1)}));view.setMessages(items);</script>`);}
 let file=path.resolve(root,'frontend',url.pathname.replace(/^\/phase11\//,'phase11/'));if(url.pathname.endsWith('/'))file=path.join(file,'index.html');
 if(!file.startsWith(path.join(root,'frontend')+path.sep)||!fs.existsSync(file)){res.writeHead(404);return res.end();}
 res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html'});fs.createReadStream(file).pipe(res);
});
(async()=>{await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true,args:['--use-angle=swiftshader','--enable-unsafe-swiftshader']});
 try{const page=await browser.newPage({viewport:{width:1600,height:1000}}),errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(base+'/phase11/tree/');await page.waitForSelector('.post-card',{state:'attached'});assert.equal(await page.locator('.post-card').count(),24);
  await page.waitForFunction(()=>document.querySelector('#scene canvas')||document.body.classList.contains('list-mode'));
  assert.equal(await page.locator('#scene canvas').count(),1,'real WebGL scene must render');
  await page.waitForTimeout(700);assert.equal(await page.locator('#nebula-navigation nav a').count(),3);await page.screenshot({path:path.join(root,'build-function3/tree-desktop.png'),fullPage:true});
  await page.locator('#show-publish').click();await page.locator('[name=body]').fill('一枚来自浏览器回归测试的果实');await page.locator('#record-voice').click();await page.waitForFunction(()=>document.querySelector('#record-state').textContent.length>0);
  // 不授予真实麦克风权限，文字发布仍可以正常完成。
  await page.locator('[name=publicConfirmed]').check();await page.locator('#publish-submit').click();await page.waitForFunction(()=>!document.querySelector('#publish-dialog').open).catch(async error=>{console.error({status:await page.locator('#publish-status').textContent(),errors,invalid:await page.locator(':invalid').evaluateAll(items=>items.map(n=>({name:n.name,message:n.validationMessage})))});throw error;});
  const write=writes.find(w=>w.path==='/api/activities');assert.equal(write.csrf,'synthetic-csrf');assert.ok(write.fields.requestId);assert.equal(write.fields.body,'一枚来自浏览器回归测试的果实');
  await page.locator('[data-tree-tab=feed]').first().click();await page.screenshot({path:path.join(root,'build-function3/tree-feed.png'),fullPage:false});await page.locator('#post-list .post-body').first().click();await page.waitForSelector('#post-dialog[open]');assert.equal(await page.locator('.reply').count(),1);await page.screenshot({path:path.join(root,'build-function3/tree-comments.png'),fullPage:false});
  await page.locator('.reply-tools button').first().click();await page.locator('#reply-body').fill('这是一条有明确回复对象的评论');await page.locator('#reply-form button[type=submit]').click();await page.waitForFunction(()=>!document.querySelector('#reply-target').hidden===false);assert.equal(writes.find(w=>w.path.endsWith('/replies')).fields.parentReplyId,'1');
  await page.locator('#post-detail .detail-actions button').filter({hasText:'举报'}).click();await page.locator('#report-dialog input[value=垃圾广告]').check();await page.locator('#report-dialog textarea').fill('回归验证');await page.locator('#report-form button[type=submit]').click();await page.waitForFunction(()=>!document.querySelector('#report-dialog').open);assert.ok(writes.some(w=>w.path.endsWith('/reports')&&w.fields.reason==='垃圾广告：回归验证'));
  await page.locator('[data-close=post-dialog]').click();
  await page.setViewportSize({width:390,height:844});await page.screenshot({path:path.join(root,'build-function3/tree-mobile.png'),fullPage:true});assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));
  await page.goto(base+'/virtual-test');await page.waitForFunction(()=>window.view);assert.ok(await page.locator('[data-message-id]').count()<=160);
  for(let i=0;i<4;i++){await page.evaluate(()=>list.scrollTop=list.scrollHeight);await page.waitForTimeout(100);}
  assert.ok(await page.locator('[data-message-id="10000"]').count(),'last item reachable');assert.ok(await page.locator('[data-message-id]').count()<=160);
  await page.evaluate(()=>{list.style.overflowAnchor='none';list.scrollTop=list.scrollHeight*.5;});await page.waitForTimeout(300);
  const anchor=await page.evaluate(()=>{const top=list.getBoundingClientRect().top,nodes=[...list.querySelectorAll('[data-message-id]')],index=nodes.findIndex(n=>n.getBoundingClientRect().bottom>=top),node=nodes[index];if(index<1)throw Error('overscan row required');const result={id:node.dataset.messageId,offset:node.getBoundingClientRect().top-top};nodes[index-1].style.height='420px';return result;});
  await page.waitForTimeout(300);const offset=await page.evaluate(id=>document.querySelector(`[data-message-id="${id}"]`).getBoundingClientRect().top-list.getBoundingClientRect().top,anchor.id);assert.ok(Math.abs(offset-anchor.offset)<3,'resized row above viewport must preserve reading anchor');
  await page.goto(base+'/phase11/admin/');await page.waitForSelector('table');assert.ok((await page.locator('#content').innerText()).includes('test_user'));
  page.on('dialog',dialog=>dialog.type()==='prompt'?dialog.accept('浏览器测试原因'):dialog.accept());
  await page.locator('[data-view=models]').click();await page.getByRole('button',{name:'查看版本与评测'}).click();await page.getByRole('button',{name:'启用候选版本'}).click();await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('模型切换已由服务器确认'));
  assert.equal(writes.find(w=>w.path==='/api/admin/models/1/versions/2').csrf,'synthetic-csrf');
  await page.locator('[data-view=messaging]').click();await page.waitForFunction(()=>document.querySelector('#content').textContent.includes('消费者失败'));
  await page.locator('[data-view=runtime]').click();await page.waitForFunction(()=>document.querySelector('#content').textContent.includes('webserver_connections_active'));
  await page.locator('[data-view=speech]').click();await page.getByRole('button',{name:'试听预置音色'}).first().click();await page.waitForSelector('#content audio');assert.equal(writes.filter(w=>w.path==='/api/admin/speech/preview').length,1);
  role='moderator';await page.reload();await page.waitForFunction(()=>document.querySelector('[data-view=users]').hidden);assert.ok(await page.locator('[data-view=models]').isHidden());
  role='user';await page.reload();await page.waitForFunction(()=>document.querySelector('#status').textContent.includes('没有站点管理权限'));assert.equal(await page.locator('#content button').count(),0);
  assert.deepEqual(errors,[]);console.log('Tree WebGL/list/mobile, publish/CSRF, detail, 10k virtual DOM and admin UI smoke passed');
 }finally{await browser.close();server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(error=>{console.error(error);process.exitCode=1;server.closeAllConnections();server.close();});
