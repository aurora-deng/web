import * as THREE from 'three';
import { TreeAccents } from '../TreeAccents/TreeAccents.js?v=144f1e6009a3d461';
import { fruitGeometry, fruitMaps, fruitStemGeometry, fruitColor } from '../FruitAppearance/FruitAppearance.js?v=144f1e6009a3d461';
import { OrbitControls } from '../../vendor/three-r186/OrbitControls.js?v=144f1e6009a3d461';
import { mergeWood } from './WoodGeometry.js?v=144f1e6009a3d461';
const point=p=>new THREE.Vector3(...p);
// 固定种子只用于造型；果实身份、数量和名次全部来自真实业务数据。
function random(seed=2317){return()=>{seed=(1664525*seed+1013904223)>>>0;return seed/4294967296;};}
function flowerTexture(){
  const canvas=document.createElement('canvas');canvas.width=canvas.height=128;const c=canvas.getContext('2d');
  c.translate(64,64);for(let i=0;i<5;i++){c.save();c.rotate(i*Math.PI*2/5);const g=c.createLinearGradient(0,-54,0,0);g.addColorStop(0,'#fff4f8');g.addColorStop(.55,'#f7c4dc');g.addColorStop(1,'#cb689e');c.fillStyle=g;c.beginPath();c.moveTo(0,5);c.bezierCurveTo(-33,-15,-23,-60,0,-45);c.bezierCurveTo(23,-60,33,-15,0,5);c.fill();c.restore();}
  c.fillStyle='#f7ddaf';c.beginPath();c.arc(0,0,6,0,Math.PI*2);c.fill();const texture=new THREE.CanvasTexture(canvas);texture.colorSpace=THREE.SRGBColorSpace;return texture;
}
function barkTexture(){const canvas=document.createElement('canvas');canvas.width=128;canvas.height=512;const c=canvas.getContext('2d'),rng=random(17);c.fillStyle='#8f7a78';c.fillRect(0,0,128,512);for(let i=0;i<180;i++){c.strokeStyle=i%3?'#50474a45':'#d3aaa84d';c.lineWidth=.4+rng()*2;c.beginPath();let x=rng()*128;c.moveTo(x,0);for(let y=0;y<520;y+=25)c.lineTo(x+Math.sin(i+y*.02)*3,y);c.stroke();}const texture=new THREE.CanvasTexture(canvas);texture.wrapS=texture.wrapT=THREE.RepeatWrapping;texture.repeat.set(3,2);texture.colorSpace=THREE.SRGBColorSpace;return texture;}
export class TreeScene {
  constructor(container,selected){
    this.container=container;this.selected=selected;this.posts=[];this.total=-1;this.reduced=matchMedia('(prefers-reduced-motion: reduce)').matches;this.mobile=matchMedia('(max-width:760px)').matches;
    this.scratch=new THREE.Object3D();this.visible=true;this.inViewport=true;this.dirty=true;this.renderedFrames=0;this.fruitBuilds=0;this.lastFrame=-Infinity;
    this.renderer=new THREE.WebGLRenderer({antialias:!this.mobile,alpha:true,powerPreference:'low-power'});this.renderer.setPixelRatio(Math.min(devicePixelRatio,this.mobile?1.25:1.75));this.renderer.outputColorSpace=THREE.SRGBColorSpace;this.renderer.toneMapping=THREE.ACESFilmicToneMapping;this.renderer.toneMappingExposure=1.05;container.append(this.renderer.domElement);
    this.scene=new THREE.Scene();this.scene.fog=new THREE.FogExp2(0x202535,.025);this.camera=new THREE.PerspectiveCamera(37,1,.01,300);this.controls=new OrbitControls(this.camera,this.renderer.domElement);this.controls.enableDamping=!this.reduced;this.controls.enablePan=false;this.controls.minPolarAngle=Math.PI*.22;this.controls.maxPolarAngle=Math.PI*.51;
    // Ordinary wheel scroll belongs to page navigation; Alt+wheel explicitly zooms the tree.
    this.onWheelMode=event=>{this.controls.enableZoom=event.altKey&&!event.ctrlKey;};
    this.renderer.domElement.addEventListener('wheel',this.onWheelMode,{capture:true,passive:true});
    this.sky=new THREE.HemisphereLight(0xe6ddec,0x313047,2);this.scene.add(this.sky);const sun=new THREE.DirectionalLight(0xffe4e8,2.6);sun.position.set(-4,10,8);this.scene.add(sun);const rim=new THREE.DirectionalLight(0xb4beff,2.2);rim.position.set(5,7,-7);this.scene.add(rim);
    this.world=new THREE.Group();this.scene.add(this.world);this.growTree();this.ray=new THREE.Raycaster();this.pointer=new THREE.Vector2();
    this.handleDown=e=>{this.downX=e.clientX;this.downY=e.clientY;};this.handleClick=e=>{if(Math.hypot(e.clientX-(this.downX??e.clientX),e.clientY-(this.downY??e.clientY))>6)return;const r=container.getBoundingClientRect();if(!r.width||!r.height)return;this.pointer.set((e.clientX-r.left)/r.width*2-1,-(e.clientY-r.top)/r.height*2+1);this.ray.setFromCamera(this.pointer,this.camera);this.world.updateMatrixWorld(true);const hit=this.fruits&&this.ray.intersectObject(this.fruits)[0];if(hit&&this.posts[hit.instanceId]){this.selectedId=this.posts[hit.instanceId].id;this.transitionDone=false;this.invalidate();selected(this.selectedId);}};
    container.addEventListener('pointerdown',this.handleDown);container.addEventListener('click',this.handleClick);this.observer=new ResizeObserver(()=>this.resize());this.observer.observe(container);
    // 花瓣只需24/30fps；静态/减弱动画模式按需绘制，离屏与后台完全停止。
    this.animate=time=>{this.frame=null;if(!this.canRender())return;this.dirty=false;this.controls.update();const distant=this.camera.position.distanceTo(this.controls.target)>(this.height||1.2)*2.8;this.crown.count=distant?Math.ceil(this.crown.instanceMatrix.count*.55):this.crown.instanceMatrix.count;this.blossoms.visible=!distant;this.petals.visible=!this.reduced&&!distant;this.accents.update(time,this.reduced,distant);this.animateFruits(time);if(!this.reduced&&!distant)this.fallPetals(time);this.renderer.render(this.scene,this.camera);this.renderedFrames++;this.lastFrame=time;if(!this.reduced||this.dirty||!this.transitionDone)this.schedule();};
    this.onControl=()=>this.invalidate();this.controls.addEventListener('change',this.onControl);
    this.onVisibility=()=>{if(document.hidden)this.pause();else this.invalidate();};document.addEventListener('visibilitychange',this.onVisibility);
    this.intersection=new IntersectionObserver(entries=>{this.inViewport=entries[0]?.isIntersecting??true;if(this.inViewport)this.invalidate();else this.pause();});this.intersection.observe(container);
    this.onTheme=()=>{const light=document.documentElement.dataset.theme==='light';this.scene.fog.color.set(light?0xece5f1:0x202535);this.sky.groundColor.set(light?0x998a91:0x313047);this.renderer.toneMappingExposure=light?1.12:1.05;this.invalidate();};window.addEventListener('nebula:theme',this.onTheme);this.onTheme();
    this.update([],0,[]);this.invalidate();
  }
  canRender(){return !this.disposed&&this.visible&&this.inViewport&&!document.hidden&&this.container.offsetWidth>0&&this.container.offsetHeight>0;}
  schedule(){if(this.frame!=null||this.timer!=null||!this.canRender())return;const wait=Math.max(0,1000/(this.mobile?24:30)-(performance.now()-this.lastFrame));this.timer=setTimeout(()=>{this.timer=null;if(this.canRender())this.frame=requestAnimationFrame(this.animate);},wait);}
  invalidate(){this.dirty=true;this.schedule();}
  pause(){clearTimeout(this.timer);this.timer=null;if(this.frame!=null)cancelAnimationFrame(this.frame);this.frame=null;}
  setVisible(visible){this.visible=visible;if(visible)this.invalidate();else this.pause();}
  statistics(){return{renderedFrames:this.renderedFrames,fruitBuilds:this.fruitBuilds,drawCalls:this.renderer.info.render.calls,triangles:this.renderer.info.render.triangles,geometries:this.renderer.info.memory.geometries,pixelRatio:this.renderer.getPixelRatio()};}
  zoom(factor){const distance=this.camera.position.distanceTo(this.controls.target);const next=Math.max(this.controls.minDistance,Math.min(this.controls.maxDistance,distance*factor));this.camera.position.sub(this.controls.target).multiplyScalar(next/distance).add(this.controls.target);this.controls.update();this.invalidate();}
  growTree(){
    const rng=random(),wood=new THREE.MeshStandardMaterial({color:0xbaa19a,map:barkTexture(),roughness:.96});const tips=[];
    const branch=(positions,radius)=>{const curve=new THREE.CatmullRomCurve3(positions.map(point)),segments=24,sides=9,geometry=new THREE.TubeGeometry(curve,segments,radius,sides,false),vertices=geometry.attributes.position;
      for(let ring=0;ring<=segments;ring++){const center=curve.getPointAt(ring/segments),taper=Math.max(.055,(1-ring/segments)**.82);for(let side=0;side<=sides;side++){const index=ring*(sides+1)+side,v=new THREE.Vector3().fromBufferAttribute(vertices,index).sub(center).multiplyScalar(taper).add(center);vertices.setXYZ(index,v.x,v.y,v.z);}}geometry.computeVertexNormals();geometry.computeBoundingSphere();this.world.add(new THREE.Mesh(geometry,wood));return curve;};
    // The trunk bends away from the shore and carries a layered, uneven canopy.
    branch([[0,-.2,0],[-.56,1.05,.1],[-.43,2.35,.15],[.3,3.65,.05],[1.05,4.9,-.13],[1.48,6.15,-.26],[1.58,7.62,-.35]],.78);
    const limbs=[
      [[-.4,2.6,.12],[.55,3.62,.65],[2.65,4.74,1.4],[4.3,5.3,1.53]],
      [[.25,3.6,.06],[1.4,4.8,-.85],[3.12,5.92,-1.58],[4.65,6.36,-1.4]],
      [[.95,4.72,-.1],[2.0,5.77,.51],[3.0,6.75,.95],[3.85,7.09,.76]],
      [[-.38,2.7,.15],[-1.22,3.86,.71],[-2.55,5.15,1.05],[-3.58,5.72,.72]],
      [[.1,3.45,.06],[-.66,4.8,-.88],[-1.88,6.08,-2.0],[-2.85,6.55,-2.38]],
      [[.81,4.52,-.08],[.32,5.67,-1.6],[.2,6.4,-2.5],[.65,6.8,-2.78]],
      [[1.19,5.25,-.16],[.8,6.29,.45],[.0,7.22,1.02],[-.96,7.71,1.15]],
      [[1.36,5.79,-.22],[1.68,6.75,-.04],[2.12,7.51,-.58],[2.58,8.07,-.9]],
    ];
    limbs.forEach((limb,index)=>{
      const curve=branch(limb,index<4?.29:.22);
      tips.push(limb.at(-1));
      for(let twig=0;twig<3;twig++){
        const along=.43+twig*.18;
        const start=curve.getPointAt(along);
        const end=curve.getPointAt(Math.min(1,along+.21));
        const spread=twig%2===0?1:-1;
        end.x+=(rng()-.38)*.95;
        end.y+=.63+rng()*.52;
        end.z+=spread*(.55+rng()*.6);
        const middle=start.clone().lerp(end,.55);
        middle.y-=.13;
        branch([start.toArray(),middle.toArray(),end.toArray()],.09);
        tips.push(end.toArray());
      }
    });
    const count=this.mobile?2600:6900,texture=flowerTexture(),material=new THREE.MeshStandardMaterial({map:texture,transparent:false,alphaTest:.30,side:THREE.DoubleSide,roughness:.9,emissive:0xa85b79,emissiveIntensity:.25});
    this.crown=new THREE.InstancedMesh(new THREE.PlaneGeometry(1,1),material,count);const object=new THREE.Object3D();
    for(let i=0;i<count;i++){const center=tips[i%tips.length],a=rng()*Math.PI*2,z=rng()*2-1,r=Math.sqrt(1-z*z),radius=Math.cbrt(rng());object.position.set(center[0]+Math.cos(a)*r*radius*.92,center[1]+z*radius*.66,center[2]+Math.sin(a)*r*radius*.8);object.rotation.set((rng()-.5)*Math.PI,rng()*Math.PI*2,rng()*Math.PI*2);const scale=.19+rng()*.19;object.scale.setScalar(scale);object.updateMatrix();this.crown.setMatrixAt(i,object.matrix);this.crown.setColorAt(i,new THREE.Color().setHSL(.91+rng()*.05,.18+rng()*.18,.68+rng()*.23));}this.world.add(this.crown);
    this.blossoms=new THREE.InstancedMesh(new THREE.PlaneGeometry(1,1),material,this.mobile?110:280);
    for(let i=0;i<this.blossoms.count;i++){const center=tips[i%tips.length];object.position.set(center[0]+(rng()-.5)*1.3,center[1]+(rng()-.5)*.75,center[2]+.5);object.rotation.set(.12,rng()*.4-.2,rng()*6);object.scale.setScalar(.26+rng()*.16);object.updateMatrix();this.blossoms.setMatrixAt(i,object.matrix);}this.world.add(this.blossoms);
    const floor=new THREE.Mesh(new THREE.SphereGeometry(3.7,48,16),new THREE.MeshStandardMaterial({color:0x51464e,roughness:.95}));floor.scale.set(1.06,.065,.82);floor.position.set(-.35,-.45,0);this.world.add(floor);this.accents=new TreeAccents(this.world,tips,material,wood,this.mobile,rng);mergeWood(this.world,wood);
    const stones=new THREE.InstancedMesh(new THREE.DodecahedronGeometry(.28,0),new THREE.MeshStandardMaterial({color:0x776b70,roughness:1}),35);
    for(let i=0;i<stones.count;i++){
      const angle=i*2.399;
      const radius=1.6+rng()*1.85;
      object.position.set(Math.cos(angle)*radius-.35,-.22,Math.sin(angle)*radius*.78);
      object.scale.set(.55+rng()*1.15,.42+rng()*.63,.55+rng()*1.1);
      object.rotation.set(rng(),rng()*4,rng());
      object.updateMatrix();
      stones.setMatrixAt(i,object.matrix);
    }
    this.world.add(stones);
    const petal=new THREE.Shape();
    petal.moveTo(0,-.055);
    petal.quadraticCurveTo(-.048,-.02,-.031,.034);
    petal.quadraticCurveTo(-.015,.065,.006,.045);
    petal.quadraticCurveTo(.04,.067,.043,.028);
    petal.quadraticCurveTo(.039,-.016,0,-.055);
    const n=this.mobile?55:150;
    this.petalData=Array.from({length:n},()=>({x:(rng()-.5)*10,z:(rng()-.5)*8,y:rng()*9,speed:.2+rng()*.3,angle:rng()*6}));
    this.petals=new THREE.InstancedMesh(new THREE.ShapeGeometry(petal,4),new THREE.MeshBasicMaterial({color:0xf3c4dc,side:THREE.DoubleSide,transparent:true,opacity:.7}),n);
    this.world.add(this.petals);
  }
  fallPetals(time){const object=this.scratch;this.petalData.forEach((p,i)=>{const t=time*.001;object.position.set(p.x+Math.sin(t*.4+p.angle)*.35,8-((p.y+t*p.speed)%8.5),p.z);object.rotation.set(p.angle+t*.4,p.angle+t*.15,p.angle);object.scale.setScalar(1);object.updateMatrix();this.petals.setMatrixAt(i,object.matrix);});this.petals.instanceMatrix.needsUpdate=true;}
  clearFruits(){for(const name of ['fruits','halos','stems','labels','calyx']){const node=this[name];if(!node)continue;this.world.remove(node);node.traverse(o=>{o.geometry?.dispose();if(o.material)for(const m of Array.isArray(o.material)?o.material:[o.material]){m.map?.dispose();m.bumpMap?.dispose();m.dispose();}});}}
  update(posts,total,ranks){
    const lookup=new Map(ranks.map((r,i)=>[r.postId,i+1]));
    const key=JSON.stringify(posts.map(p=>[p.id,lookup.get(p.id)||0,p.media?.some(m=>m.kind==='audio')||false]));
    this.posts=posts;
    if(key===this.geometryKey&&total===this.total)return; // 点赞/正文/分数更新不重建GPU资源。
    this.geometryKey=key;this.fruitBuilds++;
    const old=new Map(this.positions?.map(p=>[p.id,p.position])||[]);this.clearFruits();this.labels=new THREE.Group();this.positions=[];this.fruits=new THREE.InstancedMesh(fruitGeometry(),new THREE.MeshStandardMaterial({...fruitMaps(),bumpScale:.013,roughness:.32,metalness:.12,emissive:0x493657,emissiveIntensity:.10}),Math.max(1,posts.length));this.fruits.count=posts.length;this.calyx=new THREE.InstancedMesh(fruitStemGeometry(),new THREE.MeshStandardMaterial({color:0xc5aa72,roughness:.53,metalness:.14}),Math.max(1,posts.length));this.calyx.count=posts.length;
    const ranked=posts.filter(p=>lookup.has(p.id)).length;this.halos=new THREE.InstancedMesh(new THREE.TorusGeometry(.31,.009,6,48),new THREE.MeshBasicMaterial({color:0xf1d29c,transparent:true,opacity:.55}),Math.max(1,ranked));this.halos.count=ranked;let halo=0;const lines=[];
    posts.forEach((post,i)=>{const rank=lookup.get(post.id),a=(rank?((rank-1)/10)*Math.PI*1.45+.15:i*2.399),radius=rank?2.8:1.1+(i%4)*.72,position=new THREE.Vector3(Math.cos(a)*radius,5.3+(i%3)*.4,Math.sin(a)*radius*.7+1);this.positions.push({id:post.id,position,from:old.get(post.id)||position.clone().add(new THREE.Vector3(0,-.25,0)),rank,halo:rank?halo++:0});this.fruits.setColorAt(i,new THREE.Color(fruitColor(post)));lines.push(position.x,position.y+.39,position.z,position.x+.02,position.y+.7,position.z);
      if(rank){const canvas=document.createElement('canvas');canvas.width=canvas.height=96;const c=canvas.getContext('2d');c.fillStyle='#493a50db';c.strokeStyle='#eed6a4';c.lineWidth=2;c.beginPath();c.arc(48,48,24,0,Math.PI*2);c.fill();c.stroke();c.fillStyle='#f6dfb4';c.font='bold 27px system-ui';c.textAlign='center';c.textBaseline='middle';c.fillText(String(rank),48,49);const texture=new THREE.CanvasTexture(canvas);texture.colorSpace=THREE.SRGBColorSpace;const label=new THREE.Sprite(new THREE.SpriteMaterial({map:texture,depthTest:false}));label.position.copy(position).add(new THREE.Vector3(0,.58,0));label.scale.set(.5,.5,.5);this.labels.add(label);this.positions.at(-1).label=label;}
    });
    const geometry=new THREE.BufferGeometry();geometry.setAttribute('position',new THREE.Float32BufferAttribute(lines,3));this.stems=new THREE.LineSegments(geometry,new THREE.LineBasicMaterial({color:0xa79b99,transparent:true,opacity:.65}));this.world.add(this.fruits,this.halos,this.labels,this.stems,this.calyx);this.transitionStart=performance.now();this.transitionDone=false;
    const height=total===0?1.2:.08*total;this.world.scale.setScalar(height/8.7);this.scene.fog.density=.02/(height/8.7);if(this.total!==total){this.total=total;this.height=height;this.reset();}this.animateFruits(performance.now());this.invalidate();
  }
  animateFruits(time){const t=this.reduced?1:Math.min(1,Math.max(0,(time-this.transitionStart)/700));if(this.transitionDone&&t===1)return;const object=this.scratch,smooth=t*t*(3-2*t);this.positions?.forEach((p,i)=>{object.position.lerpVectors(p.from,p.position,smooth);object.rotation.set(0,0,0);const selected=p.id===this.selectedId?1.15:1;object.scale.setScalar((p.rank?1.18:1)*selected);object.updateMatrix();this.fruits.setMatrixAt(i,object.matrix);object.rotation.y=i*2.399;object.updateMatrix();this.calyx.setMatrixAt(i,object.matrix);if(p.rank){p.label.position.copy(object.position).add(new THREE.Vector3(0,.58,0));object.position.y-=.17;object.rotation.set(Math.PI/2,0,0);object.scale.setScalar(.82);object.updateMatrix();this.halos.setMatrixAt(p.halo,object.matrix);}});this.calyx.instanceMatrix.needsUpdate=true;this.fruits.instanceMatrix.needsUpdate=true;this.halos.instanceMatrix.needsUpdate=true;this.fruits.computeBoundingSphere();this.calyx.computeBoundingSphere();this.halos.computeBoundingSphere();this.transitionDone=t===1;}
  reset(){const h=this.height||1.2;this.controls.target.set(h*.045,h*.45,0);this.framing=Math.max(1,.86/this.camera.aspect);this.camera.position.set(h*.31,h*.62,h*2.08).sub(this.controls.target).multiplyScalar(this.framing).add(this.controls.target);this.camera.near=h*.001;this.camera.far=h*25;this.camera.updateProjectionMatrix();this.controls.minDistance=h*.55;this.controls.maxDistance=h*5;this.controls.update();this.invalidate();}
  resize(){const{width,height}=this.container.getBoundingClientRect();if(!width||!height)return;this.renderer.setSize(width,height,false);this.camera.aspect=width/height;const framing=Math.max(1,.86/this.camera.aspect);if(this.framing&&framing!==this.framing)this.camera.position.sub(this.controls.target).multiplyScalar(framing/this.framing).add(this.controls.target);this.framing=framing;this.camera.updateProjectionMatrix();this.invalidate();}
  dispose(){this.disposed=true;this.pause();this.observer.disconnect();this.intersection.disconnect();document.removeEventListener('visibilitychange',this.onVisibility);window.removeEventListener('nebula:theme',this.onTheme);this.controls.removeEventListener('change',this.onControl);this.renderer.domElement.removeEventListener('wheel',this.onWheelMode,true);this.controls.dispose();this.container.removeEventListener('pointerdown',this.handleDown);this.container.removeEventListener('click',this.handleClick);const geometries=new Set(),materials=new Set(),textures=new Set();this.scene.traverse(o=>{if(o.geometry)geometries.add(o.geometry);if(o.material)for(const m of Array.isArray(o.material)?o.material:[o.material]){materials.add(m);if(m.map)textures.add(m.map);if(m.bumpMap)textures.add(m.bumpMap);}});geometries.forEach(g=>g.dispose());textures.forEach(t=>t.dispose());materials.forEach(m=>m.dispose());this.renderer.dispose();this.renderer.domElement.remove();}
}
