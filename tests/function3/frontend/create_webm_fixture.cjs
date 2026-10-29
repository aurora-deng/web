/* Create synthetic Opus/WebM using installed Edge MediaRecorder; no microphone or downloads. */
const fs=require('node:fs'),path=require('node:path');const{chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
(async()=>{
 const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true,args:['--autoplay-policy=no-user-gesture-required']});
 try{
  const page=await browser.newPage();await page.goto('about:blank');
  for(const[name,seconds]of[['short',1.2],['long',61.5]]){
   const data=await page.evaluate(async seconds=>{
    const context=new AudioContext(),destination=context.createMediaStreamDestination(),tone=context.createOscillator(),gain=context.createGain();gain.gain.value=.02;tone.connect(gain);gain.connect(destination);tone.start();await context.resume();
    const recorder=new MediaRecorder(destination.stream,{mimeType:'audio/webm;codecs=opus'}),chunks=[];recorder.ondataavailable=event=>chunks.push(event.data);
    const stopped=new Promise(resolve=>recorder.onstop=resolve);recorder.start();await new Promise(resolve=>setTimeout(resolve,seconds*1000));recorder.stop();await stopped;tone.stop();await context.close();
    const blob=new Blob(chunks,{type:'audio/webm'}),bytes=new Uint8Array(await blob.arrayBuffer());let binary='';for(const value of bytes)binary+=String.fromCharCode(value);return btoa(binary);
   },seconds);
   const root=path.resolve(__dirname,'../../../build-function3/media-fixtures');fs.mkdirSync(root,{recursive:true});fs.writeFileSync(path.join(root,name+'.webm'),Buffer.from(data,'base64'));console.log(name,'WebM fixture written');
  }
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
