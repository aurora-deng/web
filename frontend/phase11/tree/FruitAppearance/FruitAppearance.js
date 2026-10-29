import * as THREE from 'three';

/** 所有果实共享形体与旋纹；实例颜色决定品种，不逐果实新增材质/灯光。 */
export function fruitGeometry() {
  const geometry=new THREE.SphereGeometry(.23,32,24),p=geometry.attributes.position;
  for(let i=0;i<p.count;i++){
    const x=p.getX(i),y=p.getY(i),z=p.getZ(i),angle=Math.atan2(z,x);
    const lobe=1+.055*Math.cos(angle*6)*Math.sin(Math.acos(Math.max(-1,Math.min(1,y/.23))))**2;
    p.setXYZ(i,x*lobe,y*.96,z*lobe);
  }
  geometry.computeVertexNormals();geometry.computeBoundingSphere();return geometry;
}

/** UV 表面重复的内卷旋纹：彩色底图＋共享凹凸图，近看有浮雕，远看有清晰剪影。 */
export function fruitMaps(){
  const canvas=document.createElement('canvas');canvas.width=512;canvas.height=256;
  const bump=document.createElement('canvas');bump.width=512;bump.height=256;
  const c=canvas.getContext('2d'),b=bump.getContext('2d');
  const gradient=c.createLinearGradient(0,0,0,256);gradient.addColorStop(0,'#fff7e5');gradient.addColorStop(.4,'#f4eafa');gradient.addColorStop(1,'#a39bb9');c.fillStyle=gradient;c.fillRect(0,0,512,256);
  b.fillStyle='#797979';b.fillRect(0,0,512,256);
  const spiral=(ctx,x,y,radius)=>{ctx.beginPath();for(let i=0;i<=72;i++){const t=i/72,a=t*Math.PI*3.7,r=radius*(1-t)*.94+1.5;const px=x+Math.cos(a)*r,py=y+Math.sin(a)*r;if(!i)ctx.moveTo(px,py);else ctx.lineTo(px,py);}ctx.stroke();};
  for(let row=0;row<4;row++)for(let col=-1;col<=8;col++){
    const x=col*64+(row%2)*32,y=32+row*64;
    c.lineCap=b.lineCap='round';c.lineWidth=6;c.strokeStyle='#51405e91';spiral(c,x,y+1,25);
    c.lineWidth=2.2;c.strokeStyle='#fff7f0d9';spiral(c,x-1,y-1,25);
    b.lineWidth=6;b.strokeStyle='#dddddd';spiral(b,x,y,25);b.lineWidth=2;b.strokeStyle='#fff';spiral(b,x-1,y-1,25);
  }
  const map=new THREE.CanvasTexture(canvas),bumpMap=new THREE.CanvasTexture(bump);map.colorSpace=THREE.SRGBColorSpace;
  map.wrapS=bumpMap.wrapS=THREE.RepeatWrapping;return{map,bumpMap};
}

/** 一根卷曲的藤柄也作为实例批次，避免几十个 TubeMesh 的绘制开销。 */
export function fruitStemGeometry(){
  const curve=new THREE.CatmullRomCurve3([[0,.19,0],[-.025,.30,0],[.025,.39,0],[.12,.40,0],[.145,.34,0],[.10,.30,0],[.065,.335,0],[.09,.36,0]].map(p=>new THREE.Vector3(...p)));
  return new THREE.TubeGeometry(curve,32,.016,6,false);
}
export function fruitColor(post){
  // ID 稳定映射，点赞/排名改变不会让果实突然换品种。
  const colors=[0xb290ed,0xe3a1cc,0x88c5b1,0xe9ad75,0x8dbce7];
  return colors[Number(BigInt(post.id)%BigInt(colors.length))];
}
