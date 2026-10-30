const {assertPalette}=require('./palette_assertions.cjs');
/* 浏览器测试使用真实前端 + 可控协议替身，专门制造 ACK 丢失和断线。
 * 它不替代真实服务器/数据库黑盒测试，也不下载浏览器或第三方依赖。
 * 设置 PLAYWRIGHT_MODULE 与 CHAT_TEST_BROWSER 指向已经安装的工具。
 */
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const root = path.resolve(__dirname, '../../..');
let muted=false;const writes=[];
const messages = [];
const sockets = new Set();
const events = new Set();
const commands = [];
const errors = [];
let offline = false;
let sendMode = 'normal';
const members = [
  [1, 'owner'], [2, 'administrator'], [3, 'member'], [4, 'administrator'],
].map(([userId, role]) => ({ userId, role, username: `user${userId}`,
  displayName: `用户${userId}`, lastReadSequence: 0, lastDeliveredSequence: 0 }));
function add(senderId = 3, key = `seed-${messages.length + 1}`, body = `消息 ${messages.length + 1}`) {
  const sequence = messages.length + 1;
  const message = { id: sequence, sequence, conversationId: 1, senderId,
    clientMessageId: key, body, state: 'active', createdAt: Date.now(), attachments: [] };
  messages.push(message);
  return message;
}
for (let i = 1; i <= 431; i++) {const m=add(3);m.createdAt=Date.now()-Math.floor((431-i)/150)*86400000;m.body+=i%7===0?' 很长的文字'.repeat(70):'';if(i%11===0)m.attachments=[{fileId:i,name:'附件.txt',size:100,url:'/api/files/'+i}];}
function summary() { return { id: 1, kind: 'group', title: '可靠性测试群', state: 'active',
  createdBy: 1, nextSequence: messages.length + 1, unreadCount: 0, lastReadSequence:messages.length, notificationsMuted:muted }; }
function broadcast(message) { sockets.forEach(socket => socket.send(JSON.stringify(message))); }
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const userId = Number(/user=(\d+)/.exec(req.headers.cookie || '')?.[1] || 2);
  const json = value => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(value)); };
  if (url.pathname === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store' });
    res.write(': connected\n\n'); events.add(res); req.on('close', () => events.delete(res)); return;
  }
  if (url.pathname === '/api/me') return json({ user: { id: userId, username: `user${userId}`,
    displayName: `用户${userId}` }, csrfToken: 'test-csrf' });
  if (url.pathname === '/api/conversations') return json([summary()]);
  if (url.pathname.endsWith('/members')) return json(members);
  if (url.pathname.endsWith('/messages/by-client-id')) return json(messages.find(message =>
    message.senderId === userId && message.clientMessageId === url.searchParams.get('clientMessageId')) || null);
  if (url.pathname.endsWith('/messages')) {
    const limit = Number(url.searchParams.get('limit') || 100);
    if (url.searchParams.has('before')) {
      const before = Number(url.searchParams.get('before'));
      return json(messages.filter(message => !before || message.sequence < before).slice(-limit));
    }
    return json(messages.filter(message => message.sequence > Number(url.searchParams.get('after') || 0)).slice(0, limit));
  }
  if(url.pathname==='/api/client-capabilities')return json({uploads:{chat:{maxBytes:26214400,chunkBytes:262144},avatar:{maxBytes:10485760}},userSearch:true});
  if(url.pathname==='/api/users/search')return json({items:[{id:'2',username:'bob',displayName:'Bob',friendship:true}],nextCursor:'0'});
  if(url.pathname.endsWith('/preferences')) {if(req.method==='PATCH'){let raw='';req.on('data',c=>raw+=c);req.on('end',()=>{const d=JSON.parse(raw);muted=d.notificationsMuted==='true';writes.push(d);json({notificationsMuted:muted,version:'1'});});return;}return json({notificationsMuted:muted,version:'1'});}
  if(url.pathname==='/api/search/messages/page')return json({items:messages.filter(m=>m.id<=(Number(url.searchParams.get('before'))||11)&&m.id>=7).reverse().slice(0,2),nextCursor:url.searchParams.get('before')==='9'?'0':'9'});
  if(url.pathname.endsWith('/context')){const id=Number(url.pathname.split('/').at(-2));return json({target:messages[id-1],items:messages.slice(id-2,id+1)});}
  if (url.pathname.startsWith('/api/')) return json([]);
  const relative = url.pathname === '/phase11/' ? 'index.html' : url.pathname.slice('/phase11/'.length);
  const file = path.join(root, 'frontend/phase11', relative);
  if (!url.pathname.startsWith('/phase11/') || !fs.existsSync(file)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, { 'Content-Type': file.endsWith('.js') ? 'text/javascript' :
    file.endsWith('.css') ? 'text/css' : 'text/html', 'Cache-Control': 'no-store' });
  fs.createReadStream(file).pipe(res);
});


(async()=>{
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const origin='http://127.0.0.1:'+server.address().port;
 const out=path.join(root,'build-function3/phase48-ui/chat');fs.mkdirSync(out,{recursive:true});
 const browser=await chromium.launch({headless:true,executablePath:process.env.CHAT_TEST_BROWSER});
 const context=await browser.newContext({viewport:{width:1440,height:900}}),page=await context.newPage();
 const report={kind:'synthetic browser/API fixture',passed:false,viewports:[]};
 page.on('pageerror',e=>errors.push(e.stack));
 await page.routeWebSocket('**/ws',socket=>socket.onMessage(()=>{}));
 try{
  await page.goto(origin+'/phase11/');await page.locator('#conversation-list button').first().click();
  await page.waitForSelector('[data-message-id="431"]');
  for(let i=0;i<4;i++)await page.locator('.history-load-button').click();
  await page.waitForFunction(()=>document.querySelector('.history-load-button')?.textContent==='已到最早消息');
  assert(await page.locator('#message-list .message').count()<=160,'virtual DOM unbounded');
  const boundary=await page.evaluate(async()=>{const{groupMessages}=await import('/phase11/chat/MessageGrouping/MessageGrouping.js');return groupMessages([{id:1,senderId:3,createdAt:100000},{id:2,senderId:3,createdAt:101000},{id:3,senderId:3,createdAt:100000000}]).map(x=>({grouped:x.grouped,day:!!x.dateBreak}));});
  assert(boundary[1].grouped&&!boundary[2].grouped&&boundary[2].day);
  for(const size of [{width:1440,height:900},{width:1920,height:1080},{width:390,height:844}]){
   await page.setViewportSize(size);
   for(const theme of ['dark','light']){
    await page.evaluate(t=>{document.documentElement.dataset.theme=t;},theme);await page.waitForTimeout(80);
    const b=await page.evaluate(()=>{const box=s=>{const r=document.querySelector(s).getBoundingClientRect();return{top:r.top,bottom:r.bottom,height:r.height};};return{pane:box('.conversation-pane'),list:box('#message-list'),input:box('#message-form'),width:document.documentElement.scrollWidth};});
    assert(Math.abs(b.pane.bottom-b.input.bottom)<2);assert(b.list.height>200);assert(b.width<=size.width+2);
    (report.palette??=[]).push({size,theme,pairs:await assertPalette(page,['#conversation-title','.conversation-copy strong','.composer-context','.private-friend-entry strong','.composer textarea'])});
    await page.screenshot({path:path.join(out,'chat-'+size.width+'-'+theme+'.png')});
   }report.viewports.push(size);
  }
  await page.setViewportSize({width:1440,height:900});
  await page.locator('#show-friends').click();await page.locator('#friend-request-form .user-picker input').fill('bo');
  await page.getByRole('button',{name:'Bob @bob · 好友',exact:true}).click();assert.equal(await page.locator('#friend-request-form [name=receiverId]').inputValue(),'2');
  await page.locator('#friend-dialog [data-close-dialog]').click();
  await page.locator('#toggle-details').click();await page.locator('#conversation-muted').check();
  await page.waitForFunction(()=>!document.querySelector('#conversation-muted').disabled);assert(muted);assert.equal(writes.length,1);
  await page.locator('#close-details').click();
  await page.locator('#search-messages').click();await page.locator('#search-form [name=scope]').selectOption('all');
  await page.locator('#search-form [name=query]').fill('消息');await page.locator('#search-form [type=submit]').click();
  await page.waitForFunction(()=>document.querySelectorAll('#search-results li').length===2);await page.locator('#search-more').click();
  await page.waitForFunction(()=>document.querySelectorAll('#search-results li').length===4);
  await page.locator('#search-results button').first().click();await page.waitForSelector('[data-message-id="11"].search-hit');
  assert.equal(await page.locator('#message-list [data-message-id]').count(),3);
  await page.locator('.history-newer-button').click();await page.waitForSelector('[data-message-id="12"]');
  assert.deepEqual(errors,[]);report.passed=true;report.checks=['431 mixed messages over three days, bounded virtual DOM','six layout/theme screenshots','user picker writes existing request DTO','group mute confirmed by backend fixture','global search cursor and bounded context jump','newer paging from search context','no module errors'];
  console.log(JSON.stringify(report));
 }finally{fs.writeFileSync(path.join(out,'summary.json'),JSON.stringify(report,null,2));await browser.close();server.closeAllConnections();events.forEach(e=>e.end());server.close();}
})().catch(e=>{console.error(e);server.closeAllConnections();server.close();process.exitCode=1;});
