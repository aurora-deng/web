(() => {
    let measure;
    function bounds(stroke,image){
        const [x,y]=stroke.points[0],size=stroke.fontSize||.025,raster=window.AnnotationText?.valid(stroke)?stroke.mathRaster:null;
        if(raster)return [x,y,raster.width*size,raster.height*size*image.width/image.height];
        measure||=document.createElement('canvas').getContext('2d');
        const pixels=size*image.width,available=Math.max(pixels,image.width-x*image.width);measure.font=`${pixels}px "Microsoft YaHei", "PingFang SC", sans-serif`;
        let rows=0,width=0;
        for(const paragraph of String(stroke.text||'').slice(0,2000).replace(/\r\n?/g,'\n').split('\n')){
            let line='';
            for(const character of paragraph){if(line&&measure.measureText(line+character).width>available){width=Math.max(width,measure.measureText(line).width);rows++;line='';}line+=character;}
            width=Math.max(width,measure.measureText(line).width);rows++;
        }
        return [x,y,Math.max(pixels,width)/image.width,Math.max(1,rows)*pixels*1.3/image.height];
    }
    function hit(edit,point,image){
        return (edit.strokes||[]).findLast(stroke=>{
            if(stroke.tool!=='text'||!stroke.points?.[0])return false;
            const [x,y,w,h]=bounds(stroke,image),padding=6/image.width;
            return point[0]>=x-padding&&point[0]<=x+w+padding&&point[1]>=y-padding&&point[1]<=y+h+padding;
        });
    }
    function moved(origin,start,current,box,crop=[0,0,1,1]){
        return [0,1].map(index=>Math.max(crop[index],Math.min(Math.max(crop[index],crop[index]+crop[index+2]-box[index+2]),origin[index]+current[index]-start[index])));
    }
    function project(box,image,edit,surface){
        const [cx,cy,cw,ch]=edit.crop||[0,0,1,1],[x,y,w,h]=box;
        const left=Math.max(cx,x),top=Math.max(cy,y),right=Math.min(cx+cw,x+w),bottom=Math.min(cy+ch,y+h);if(right<=left||bottom<=top)return;
        const radians=(edit.rotation||0)*Math.PI/180,cos=Math.cos(radians),sin=Math.sin(radians),scale=surface.clientWidth/surface.width;
        const points=[[left,top],[right,top],[right,bottom],[left,bottom]].map(([px,py])=>{
            const dx=(px-cx-cw/2)*image.width,dy=(py-cy-ch/2)*image.height;
            return [(cos*dx-sin*dy+surface.width/2)*scale,(sin*dx+cos*dy+surface.height/2)*scale];
        });
        const xs=points.map(point=>point[0]),ys=points.map(point=>point[1]);
        return [Math.min(...xs),Math.min(...ys),Math.max(...xs)-Math.min(...xs),Math.max(...ys)-Math.min(...ys)];
    }
    window.AnnotationSelection={bounds,hit,moved,project};
})();
