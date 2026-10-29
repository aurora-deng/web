/* Real browser -> existing native companion -> isolated Linux, no dependency downloads. */
const {chromium}=require(process.env.PLAYWRIGHT_MODULE);
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
  const input=JSON.parse(fs.readFileSync(0,'utf8'));
  const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true});
  const context=await browser.newContext();await context.addCookies(input.cookies);
  const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
  try{
    await page.goto(input.base+'/phase11/',{waitUntil:'networkidle'});
    await page.waitForFunction(()=>document.querySelector('#header-ws-state')?.textContent.includes('已连接'),null,{timeout:25000});
    for(const size of[{width:1440,height:900},{width:1920,height:1080},{width:390,height:844}]){
      await page.setViewportSize(size);await page.waitForTimeout(200);
      const bounds=await page.evaluate(()=>({width:document.documentElement.scrollWidth,viewport:innerWidth}));
      assert(bounds.width<=bounds.viewport+2,`horizontal overflow at ${size.width}`);
    }
    await page.setViewportSize({width:1440,height:900});
    const result=await page.evaluate(async(peerId)=>{
      const me=await fetch('/api/me').then(r=>r.json());
      const created=await fetch('/api/conversations',{method:'POST',headers:{'Content-Type':'application/json','X-CSRF-Token':me.csrfToken},body:JSON.stringify({kind:'group',title:'本机流式代理验证',memberIds:String(peerId)})}).then(async r=>{if(!r.ok)throw Error('group '+r.status);return r.json();});
      const conversation=created.conversation||created;
      const sse=await new Promise((resolve,reject)=>{const events=new EventSource('/events');const timer=setTimeout(()=>{events.close();reject(Error('SSE proxy timeout'));},10000);events.onopen=()=>{clearTimeout(timer);events.close();resolve(true);};events.onerror=()=>{clearTimeout(timer);events.close();reject(Error('SSE proxy rejected'));};});
      const ack=await new Promise((resolve,reject)=>{const socket=new WebSocket(`ws://${location.host}/ws`);const id='proxy-'+Array.from(crypto.getRandomValues(new Uint8Array(16)),n=>n.toString(16).padStart(2,'0')).join('');const timer=setTimeout(()=>{socket.close();reject(Error('WS ACK timeout'));},15000);socket.onopen=()=>socket.send(JSON.stringify({v:1,type:'chat.send',id,conversationId:Number(conversation.id),content:'通过原生TLS代理发送的测试消息'}));socket.onmessage=event=>{const value=JSON.parse(event.data);if(value.replyTo===id){clearTimeout(timer);socket.close();resolve(value.type);}};socket.onerror=()=>{clearTimeout(timer);socket.close();reject(Error('WS proxy rejected'));};});
      return{sse,ack};
    },input.peerId);
    assert.equal(result.sse,true);assert.match(result.ack,/accepted|ack/);
    await page.locator('.workspace-assistant').click();await page.waitForFunction(()=>document.querySelector('.assistant-drawer .assistant-status')?.textContent.includes('私人空间已连接'));
    await page.locator('.assistant-drawer [data-tab=memories]').click();await page.waitForTimeout(200);
    await page.screenshot({path:path.resolve('build-client/integration/real-proxy-browser.png')});
    await page.locator('.assistant-drawer [data-tab=settings]').click();
    await page.getByRole('button',{name:'音色、云语音与试听',exact:true}).click();
    await page.waitForSelector('.speech-settings[open]');
    await page.locator('.speech-settings select[name=provider]').selectOption('gemini');
    assert.equal(await page.locator('.speech-settings [data-gemini]').isVisible(),true);
    for(const size of[{width:1440,height:900},{width:1920,height:1080},{width:390,height:844}]){
      await page.setViewportSize(size);const rect=await page.locator('.speech-settings').boundingBox();assert(rect.x>=0&&rect.x+rect.width<=size.width+2&&rect.height<=size.height,'speech dialog exceeds viewport');
    }
    await page.screenshot({path:path.resolve('build-client/integration/speech-settings-mobile.png')});
    await page.locator('.speech-settings [data-close]').click();await page.setViewportSize({width:1440,height:900});
    assert.deepEqual(errors,[]);console.log('PASS real browser HTTP login restore, WebSocket persisted ACK, SSE open, responsive navigation and private sidebar');
  }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
