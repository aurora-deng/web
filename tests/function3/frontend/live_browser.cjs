/* Real VM UI check. Credentials are read locally, never printed or captured in screenshots. */
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=path.resolve(__dirname,'../../..'),base=process.env.PHASE35_BASE_URL||'http://192.168.239.135:18090';
if(!['127.0.0.1','192.168.239.135'].includes(new URL(base).hostname))throw Error('Only the explicitly configured learning VM is allowed');
function password(){
  if(process.env.PHASE35_DEMO_PASSWORD_FILE)return fs.readFileSync(process.env.PHASE35_DEMO_PASSWORD_FILE,'utf8').trim();
  const note=fs.readFileSync(path.join(root,'docs/database/phase11-app-data.local.md'),'utf8');let headers=[];
  for(const line of note.split('\n')){if(!line.startsWith('|'))continue;const cells=line.split('|').slice(1,-1).map(x=>x.replace(/`/g,'').trim());if(cells.includes('用户名')&&cells.includes('密码'))headers=cells;if(cells.includes('nebula_owner')&&headers.includes('密码')){const value=cells[headers.indexOf('密码')];if(value&&value.length>=8)return value;}}
  throw Error('Set PHASE35_DEMO_PASSWORD_FILE to the existing demo password file');
}
(async()=>{
  const secret=password(),browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true,args:['--use-angle=swiftshader','--enable-unsafe-swiftshader']});
  const errors=[],directory=path.join(root,'build-function3/phase35');fs.mkdirSync(directory,{recursive:true});
  try{
    const page=await browser.newPage({viewport:{width:1440,height:1000}});page.on('pageerror',e=>errors.push(e.message));
    await page.goto(base+'/phase11/');await page.locator('#login-form [name=username]').fill('nebula_owner');await page.locator('#login-form [name=password]').fill(secret);await page.locator('#login-form button[type=submit]').click();await page.waitForSelector('#chat-panel:not(.hidden)');
    assert.equal(await page.locator('#nebula-navigation nav a').count(),3);await page.screenshot({path:path.join(directory,'chat-live.png')});
    await page.locator('#nebula-navigation a[href="/phase11/tree/"]').click();await page.waitForSelector('#scene canvas');await page.waitForTimeout(800);await page.screenshot({path:path.join(directory,'tree-live.png')});
    await page.locator('.sound-menu summary').click();await page.waitForSelector('.preview-button');await page.locator('.preview-button').click();await page.waitForFunction(()=>{const a=document.querySelector('.announcement-sound audio');return a&&!a.paused&&a.currentTime>.1;},null,{timeout:120000});
    const audio=await page.locator('.announcement-sound audio').evaluate(a=>({currentTime:a.currentTime,paused:a.paused,muted:a.muted,volume:a.volume,readyState:a.readyState}));assert.ok(!audio.muted&&audio.volume>0);await page.screenshot({path:path.join(directory,'speech-live.png')});
    await page.locator('#nebula-navigation a[href="/phase11/admin/"]').click();await page.waitForSelector('#content table');await page.screenshot({path:path.join(directory,'admin-live.png')});
    await page.locator('[data-view=runtime]').click();await page.waitForFunction(()=>document.querySelector('#content').textContent.includes('Redis'));assert.ok((await page.locator('#content').innerText()).includes('"enabled":true'));
    await page.setViewportSize({width:390,height:844});
    for(const suffix of ['/phase11/','/phase11/tree/','/phase11/admin/']){await page.goto(base+suffix);await page.waitForSelector('#nebula-navigation');await page.waitForTimeout(600);assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1),'mobile overflow '+suffix);}
    assert.deepEqual(errors,[]);const report={originalAccountLogin:true,threeAreaNavigation:true,realAudio:audio,mobileAreas:3,pageErrors:errors,businessContentPublished:false};fs.writeFileSync(path.join(directory,'live-browser.json'),JSON.stringify(report,null,2));console.log(JSON.stringify(report));
  }finally{await browser.close();}
})().catch(e=>{console.error(e.message);process.exitCode=1;});
