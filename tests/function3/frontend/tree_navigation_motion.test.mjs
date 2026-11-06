import test from 'node:test';
import assert from 'node:assert/strict';
import { TreePageNavigation } from '../../../frontend/phase11/tree/TreePageNavigation/TreePageNavigation.js';
import { TreePageTransition } from '../../../frontend/phase11/tree/TreePageTransition/TreePageTransition.js';

function gesture(){
  let current='tree';const changes=[],hints=[],handlers=new Map();
  const root={documentElement:{scrollHeight:900},querySelector:()=>null,addEventListener:(k,v)=>handlers.set(k,v),removeEventListener:k=>handlers.delete(k)};
  const viewport={scrollY:0,innerHeight:900};
  const nav=new TreePageNavigation({current:()=>current,change:(view,options)=>{current=view;changes.push({view,options});},progress:v=>hints.push(v),root,viewport});
  return {nav,root,viewport,changes,hints,handlers};
}
test('a giant wheel tick only previews; continuous momentum cannot skip a second page',()=>{
  const x=gesture();x.nav.step(2000,100);assert.equal(x.changes.length,0);assert(x.hints.at(-1).amount<1);
  x.nav.step(120,180);assert.equal(x.changes[0].view,'feed');
  for(let time=200;time<=2000;time+=30)x.nav.step(20,time);
  assert.equal(x.changes.length,1);
  x.nav.step(120,2400);x.nav.step(120,2500);assert.equal(x.changes[1].view,'rank');x.nav.dispose();
});
test('reading a long feed remains native, including the tail of the gesture that reaches its end',()=>{
  const x=gesture();x.nav.step(120,0,{explicit:true});x.root.documentElement.scrollHeight=4000;
  assert.equal(x.nav.step(120,1000),false);
  x.viewport.scrollY=3100;x.nav.step(120,1050);x.nav.step(120,1100);assert.equal(x.changes.length,1);
  x.nav.step(120,1450);x.nav.step(120,1550);assert.equal(x.changes[1].view,'rank');x.nav.dispose();
});
test('PageDown is explicit; horizontal scrolling and canvas touch gestures do not navigate',()=>{
  const x=gesture();let prevented=false;
  x.handlers.get('wheel')({deltaX:300,deltaY:30,target:{closest:()=>false},timeStamp:0,preventDefault:()=>prevented=true});assert(!prevented);
  x.handlers.get('keydown')({key:'PageDown',target:{closest:()=>false},timeStamp:10,preventDefault:()=>prevented=true});assert(prevented);assert.equal(x.changes[0].options.source,'keyboard');
  x.handlers.get('touchstart')({touches:[{clientX:20,clientY:300}],target:{closest:selector=>selector==='canvas'}});
  x.handlers.get('touchend')({changedTouches:[{clientX:20,clientY:0}],target:{closest:()=>false},timeStamp:1000});assert.equal(x.changes.length,1);x.nav.dispose();
});

function transition({reduced=false,controlled=false}={}){
  let current='tree';const changes=[],animations=[],scrolls=[],attributes=new Map();
  const node=()=>({style:{},setAttribute(){},append(){},remove(){}});
  const root={createElement:node,body:{append(){}},documentElement:{scrollHeight:3000}};
  const viewport={scrollY:420,innerHeight:900,matchMedia:()=>({matches:reduced}),scrollTo:({top})=>{scrolls.push(top);viewport.scrollY=top;}};
  const surface={setAttribute:(k,v)=>attributes.set(k,v),removeAttribute:k=>attributes.delete(k),querySelector:()=>null,
    animate:(frames,options)=>{let resolve,reject;const finished=controlled?new Promise((a,b)=>{resolve=a;reject=b;}):Promise.resolve();const a={frames,options,finished,resolve,cancel:()=>reject?.(new Error('cancelled'))};animations.push(a);return a;}};
  const view=new TreePageTransition({surface,root,viewport,current:()=>current,apply:v=>{current=v;changes.push(v);}});
  return {view,surface,root,viewport,changes,animations,scrolls,attributes};
}
test('transition fades the surface and restores reading position when using tabs',async()=>{
  const x=transition();await x.view.navigate('feed',{edge:'start',direction:1});assert.equal(x.animations.length,2);assert.equal(x.scrolls[0],0);
  x.viewport.scrollY=900;await x.view.navigate('rank',{edge:'start'});await x.view.navigate('feed');assert.equal(x.scrolls.at(-1),900);assert(!x.attributes.has('aria-busy'));x.view.dispose();
});
test('reduced motion applies immediately; an upward page change starts at the previous bottom',async()=>{
  const x=transition({reduced:true});await x.view.navigate('feed',{edge:'end',direction:-1});assert.equal(x.animations.length,0);assert.equal(x.scrolls.at(-1),2100);x.view.dispose();
});
test('a later tab selection cancels pending animation without applying an obsolete page',async()=>{
  const x=transition({controlled:true});const old=x.view.navigate('feed');const latest=x.view.navigate('rank');
  x.animations[1].resolve();await Promise.resolve();await Promise.resolve();
  assert.deepEqual(x.changes,['rank']);x.animations.at(-1).resolve();await latest;assert.equal(await old,false);assert(!x.attributes.has('aria-busy'));x.view.dispose();
});
test('clicking the current tab or disposing cancels a pending page change',async()=>{
  const x=transition({controlled:true});const old=x.view.navigate('feed');await x.view.navigate('tree');assert.equal(await old,false);assert.equal(x.changes.length,0);
  const pending=x.view.navigate('rank');x.view.dispose();assert.equal(await pending,false);assert.equal(x.changes.length,0);
});
