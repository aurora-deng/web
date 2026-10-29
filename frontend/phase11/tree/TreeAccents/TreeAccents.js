import * as THREE from 'three';

/** 固定上限的装饰与业务果实分离：动态再多，也不增加流苏和根系绘制量。 */
export class TreeAccents {
  constructor(world, tips, flowerMaterial, wood, mobile, random) {
    this.group = new THREE.Group();world.add(this.group);
    this.tassels = new THREE.Group();this.group.add(this.tassels);
    const chains = mobile ? 14 : 34;
    const flowers = new THREE.InstancedMesh(new THREE.PlaneGeometry(1,1),flowerMaterial,chains*18);
    const object = new THREE.Object3D();const lines=[];
    for(let chain=0;chain<chains;chain++) {
      const tip=tips[chain%tips.length];
      const x=tip[0]+(random()-.5)*.6,z=tip[2]+(random()-.5)*.6;
      const start=tip[1]-.25,length=.8+random()*1.45;
      // 前排流苏较短，给果实、树干和点选射线留出通道。
      const drop=z>1.1?Math.min(length,.95):length;
      for(let j=0;j<18;j++) {
        const t=j/17,curve=Math.sin(t*2.5+chain)*.055;
        object.position.set(x+curve,start-drop*t,z+Math.sin(t*3)*.04);
        object.rotation.set(.2,chain*.83,j*.91);
        object.scale.setScalar((.12+random()*.05)*(1-t*.62));object.updateMatrix();
        flowers.setMatrixAt(chain*18+j,object.matrix);
      }
      lines.push(x,start,z,x,start-drop,z);
    }
    this.tassels.add(flowers,new THREE.LineSegments(
      new THREE.BufferGeometry().setAttribute('position',new THREE.Float32BufferAttribute(lines,3)),
      new THREE.LineBasicMaterial({color:0xd3a5b9,transparent:true,opacity:.3})));
    // 少量散落花瓣与根颈围成自然的花毯；一个实例批次，不增加逐花绘制。
    const fallen=new THREE.InstancedMesh(new THREE.PlaneGeometry(1,1),flowerMaterial,mobile?90:220);
    for(let i=0;i<fallen.count;i++){
      const a=random()*Math.PI*2,r=.85+Math.sqrt(random())*2.35;
      object.position.set(Math.cos(a)*r,-.145,Math.sin(a)*r*.85);
      object.rotation.set(-Math.PI/2,0,random()*Math.PI*2);object.scale.setScalar(.09+random()*.075);object.updateMatrix();fallen.setMatrixAt(i,object.matrix);
      fallen.setColorAt(i,new THREE.Color().setHSL(.94,.22,.61+random()*.2));
    }
    this.group.add(fallen);

    // 根颈由宽到窄接入主干，板状根落入岩土，而不是立在悬浮圆盘上。
    const neck = new THREE.Mesh(new THREE.LatheGeometry([
      new THREE.Vector2(1.12,-.20),new THREE.Vector2(.88,.05),
      new THREE.Vector2(.59,.46),new THREE.Vector2(.43,1.05),
    ],32),wood);
    this.group.add(neck);
    for(let i=0;i<8;i++) {
      const angle=i*Math.PI/4+.15,x=Math.cos(angle),z=Math.sin(angle);
      const curve=new THREE.CatmullRomCurve3([
        new THREE.Vector3(x*.32,.85,z*.32),new THREE.Vector3(x*.75,.24,z*.75),
        new THREE.Vector3(x*1.6,-.10,z*1.6),new THREE.Vector3(x*2.65,-.21,z*2.65),
      ]);
      const geometry=new THREE.TubeGeometry(curve,18,.29,8,false);
      const positions=geometry.attributes.position;
      for(let segment=0;segment<=18;segment++) {
        const center=curve.getPointAt(segment/18),taper=1-segment/18*.95;
        for(let side=0;side<=8;side++) {
          const index=segment*9+side;
          const vertex=new THREE.Vector3().fromBufferAttribute(positions,index).sub(center);
          vertex.x*=taper;vertex.z*=taper;vertex.y*=1.2*taper;vertex.add(center);
          positions.setXYZ(index,vertex.x,vertex.y,vertex.z);
        }
      }
      geometry.computeVertexNormals();this.group.add(new THREE.Mesh(geometry,wood));
    }
    const canvas=document.createElement('canvas');canvas.width=canvas.height=128;
    const ctx=canvas.getContext('2d'),gradient=ctx.createRadialGradient(64,64,8,64,64,64);
    gradient.addColorStop(0,'#090810bf');gradient.addColorStop(1,'#09081000');
    ctx.fillStyle=gradient;ctx.fillRect(0,0,128,128);
    const shadow=new THREE.Mesh(new THREE.PlaneGeometry(7.5,6.5),new THREE.MeshBasicMaterial({
      map:new THREE.CanvasTexture(canvas),transparent:true,depthWrite:false,
    }));shadow.rotation.x=-Math.PI/2;shadow.position.y=-.19;this.group.add(shadow);
  }
  update(time,reduced,distant) {
    this.tassels.visible=!distant;
    this.tassels.rotation.z=reduced?0:Math.sin(time*.00045)*.006;
    this.tassels.rotation.x=reduced?0:Math.sin(time*.00031)*.004;
  }
}

