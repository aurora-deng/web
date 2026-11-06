/* Local UI fixture only. API/ACL correctness is verified against isolated Linux separately. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'../../..'),audio=path.resolve(process.env.SPEECH_TEST_WAVE||path.join(root,'build-function3/speech-fix/probe.wav'));
if(!fs.existsSync(audio))throw new Error('Supply an existing generated WAV via SPEECH_TEST_WAVE; no model downloads.');
const posts=Array.from({length:24},(_,i)=>({id:String(100-i),authorId:'1',authorName:'试听与交互验证',body:'今天的小小快乐，挂在花开处。\n这个页面仅用于界面回归，数据不会写入业务库。'.repeat(i%3+1),categoryId:'1',mood:'平静',tags:['日常'],media:[],createdAt:Date.now()-1000*i,version:'1',likes:'2',repliers:'1',liked:false,capabilities:{delete:true}}));
let sound=false,voice='zf_001',pending=0;
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://127.0.0.1');
  if(url.pathname.startsWith('/api/')){
    if(url.pathname.startsWith('/api/me/speech/audio/')){
      const wave=fs.readFileSync(audio),match=/^bytes=(\d+)-(\d+)$/.exec(req.headers.range||'');
      if(match){const start=Number(match[1]),end=Math.min(Number(match[2]),wave.length-1);if(start>end){res.writeHead(416);return res.end();}
        process.stdout.write(`audio range ${start}-${end}/${wave.length}\n`);res.writeHead(206,{'Content-Type':'audio/wav','Content-Range':`bytes ${start}-${end}/${wave.length}`,'Cache-Control':'no-store'});return res.end(wave.subarray(start,end+1));}
      res.writeHead(200,{'Content-Type':'audio/wav','Cache-Control':'no-store'});return res.end(wave);
    }
    let body='';req.on('data',chunk=>body+=chunk);req.on('end',()=>{
      let result={},status=200;
      if(url.pathname==='/api/me')result={user:{id:'1',username:'fixture',displayName:'界面验证账号'},csrfToken:'fixture-only'};
      else if(url.pathname==='/api/admin/me')result={role:'administrator'};
      else if(url.pathname==='/api/local/session/info')result={available:false};
      else if(url.pathname==='/api/tree/categories')result=[{id:'1',name:'生活'}];
      else if(url.pathname==='/api/tree/ranking')result={version:'1',settledAt:Date.now(),entries:posts.slice(0,10).map((p,i)=>({postId:p.id,score:10-i}))};
      else if(url.pathname==='/api/tree/posts')result={total:'24',posts};
      else if(/^\/api\/tree\/posts\/\d+$/.test(url.pathname))result=posts.find(p=>p.id===url.pathname.split('/').pop());
      else if(url.pathname==='/api/me/notification-settings'){
        if(req.method==='PATCH'){const data=JSON.parse(body);sound=data.sound==='true';voice=data.voice;}result={sound,voice};
      }
      else if(url.pathname==='/api/announcements')result=[];
      else if(url.pathname==='/api/me/speech/status')result={enabled:true};
      else if(url.pathname==='/api/me/speech/preview'){pending=Date.now()+900;result={state:'pending'};}
      else if(url.pathname.startsWith('/api/me/speech/preview/'))result={state:Date.now()<pending?'pending':'ready',speed:.95,url:'/api/me/speech/audio/fixture-wave'};
      else if(url.pathname==='/api/admin/speech/settings')result={available:true,profiles:[{voice:'zf_001',provider:'local',style:'calm',region:'eastasia',speed:.95,pitch:-2,freeTierConfirmed:false,dataUseConsent:false}],runtime:{actualProvider:'local',azureKeyConfigured:false,geminiKeyConfigured:false}};
      else if(url.pathname==='/api/admin/speech/credentials'){status=403;result={message:'站点 Key 需要 HTTPS；此处为 HTTP 界面验证环境。'};}
      res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(result));
    });return;
  }
  if(url.pathname==='/events'){res.writeHead(200,{'Content-Type':'text/event-stream'});res.write(': fixture\n\n');return;}
  const file=path.resolve(root,'frontend','.'+url.pathname);
  if(!file.startsWith(path.join(root,'frontend')+path.sep)){res.writeHead(403);return res.end();}
  const target=fs.existsSync(file)&&fs.statSync(file).isDirectory()?path.join(file,'index.html'):file;
  if(!fs.existsSync(target)){res.writeHead(404);return res.end();}
  res.writeHead(200,{'Content-Type':({'.js':'text/javascript','.css':'text/css','.html':'text/html'})[path.extname(target)]||'application/octet-stream'});res.end(fs.readFileSync(target));
});
const port=Number(process.env.SPEECH_TEST_PORT||18760);
if(!Number.isInteger(port)||port<1024||port>65535)throw new Error('Invalid fixture port');
server.listen(port,'127.0.0.1',()=>process.stdout.write(`Speech/tree UI fixture: http://127.0.0.1:${port}/phase11/tree/\n`));
