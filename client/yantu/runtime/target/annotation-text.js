(() => {
    const VERSION=1, rendered=new Map(), images=new Map(), decoded=new WeakMap(), pending=new WeakMap();
    let loading, fontCSS;
    const styles='.annotation-rich{font-family:"Microsoft YaHei","PingFang SC",sans-serif;line-height:1.5;text-align:left;overflow-wrap:anywhere;white-space:normal;letter-spacing:0}.annotation-rich p{margin:0 0 .65em}.annotation-rich p:last-child{margin-bottom:0}.annotation-rich ul,.annotation-rich ol{margin:.4em 0;padding-left:1.5em}.annotation-rich li{margin:.25em 0}.annotation-rich .katex{font-size:1.05em}.annotation-rich .katex-display{margin:.4em 0;text-align:left}.annotation-rich .katex-html{white-space:nowrap}.annotation-rich pre{white-space:pre-wrap}.annotation-rich a{color:inherit}';
    function script(path,ready){
        if(ready())return Promise.resolve();
        return new Promise((resolve,reject)=>{const tag=document.createElement('script');tag.src=path;tag.onload=resolve;tag.onerror=()=>{tag.remove();reject(new Error('公式组件未就绪，请连接电脑后重试；批注原文仍保留'));};document.head.append(tag);});
    }
    async function libraries(){
        if(!loading)loading=Promise.all([
            script(YantuEnvironment.pathname('/vendor/marked-17.0.5/marked.umd.js'),()=>Boolean(window.marked)),
            script(YantuEnvironment.pathname('/vendor/dompurify-3.4.17/purify.min.js'),()=>Boolean(window.DOMPurify)),
            script(YantuEnvironment.pathname('/vendor/katex-0.19.0/dist/katex.min.js'),()=>Boolean(window.katex))
        ]).catch(error=>{loading=null;throw error;});
        await loading;await cssReady;
    }
    // Protect TeX before Markdown consumes its backslashes or underscores.
    function tokenize(source){
        const tokens=[];
        const text=String(source).replace(/(```[\s\S]*?```|`[^`\n]*`)|((\\{1,2})\[([\s\S]*?)\3\]|(\\{1,2})\(([\s\S]*?)\5\)|\$\$([\s\S]*?)\$\$|(?<![\\$])\$(?!\$)([^\n$]+?)\$(?!\$))/g,(whole,code,math,blockSlash,block,inlineSlash,inline,dollars,short)=>{
            if(code)return code;
            const tex=String(block??inline??dollars??short).replace(/\\_(?=\{)/g,'_');
            const key='YANTUMATHPLACEHOLDER'+tokens.length+'END';
            tokens.push({key,tex,display:block!==undefined||dollars!==undefined});return key;
        });
        return {text,tokens};
    }
    function rich(text){return tokenize(text).tokens.length>0||/(\*\*|^\s*(?:[-*]|\d+\.)\s)/m.test(text||'');}
    async function render(source,target){
        const token={};target._annotationRender=token;target.textContent=source;await libraries();
        if(target._annotationRender!==token)return;
        const parsed=tokenize(source),html=marked.parse(parsed.text,{async:false,breaks:true});
        target.innerHTML=DOMPurify.sanitize(html,{USE_PROFILES:{html:true},FORBID_TAGS:['img','svg','math','iframe','video','audio','form','input','style','script','object','embed','base','link','meta'],FORBID_ATTR:['style','srcset','id']});
        const walker=document.createTreeWalker(target,NodeFilter.SHOW_TEXT),nodes=[];while(walker.nextNode())nodes.push(walker.currentNode);
        for(const node of nodes){
            if(node.parentElement.closest('code,pre'))continue;
            const parts=node.textContent.split(/(YANTUMATHPLACEHOLDER\d+END)/);if(parts.length===1)continue;
            const fragment=document.createDocumentFragment();
            for(const part of parts){
                const math=parsed.tokens.find(value=>value.key===part);
                if(!math){fragment.append(document.createTextNode(part));continue;}
                const span=document.createElement('span');span.className='annotation-formula';
                try{katex.render(math.tex,span,{displayMode:math.display,throwOnError:true,trust:false,strict:'ignore',maxExpand:500,maxSize:20,output:'html'});}
                catch(_){span.className='annotation-formula-error';span.textContent=math.tex;span.title='公式语法尚未完整，原文已保留';}
                fragment.append(span);
            }
            node.replaceWith(fragment);
        }
        for(const link of target.querySelectorAll('a'))link.replaceWith(...link.childNodes);
        target.classList.add('annotation-rich');
    }
    async function embeddedFonts(){
        if(!fontCSS)fontCSS=(async()=>{
            const response=await fetch(YantuEnvironment.pathname('/vendor/katex-0.19.0/dist/katex.min.css'));if(!response.ok)throw new Error('公式字体读取失败');
            let css=await response.text();const urls=[...new Set([...css.matchAll(/url\((fonts\/[^)]+\.woff2)\)/g)].map(match=>match[1]))];
            for(const url of urls){
                const result=await fetch(YantuEnvironment.pathname('/vendor/katex-0.19.0/dist/')+url);if(!result.ok)throw new Error('公式字体读取失败');
                const blob=await result.blob(),data=await new Promise((resolve,reject)=>{const reader=new FileReader();reader.onload=()=>resolve(reader.result);reader.onerror=reject;reader.readAsDataURL(blob);});
                css=css.replace(new RegExp('src:url\\('+url.replace(/[.*+?^${}()|[\]\\]/g,'\\$&')+'\\)[^;}]*','g'),`src:url(${data}) format("woff2")`);
            }
            return css;
        })().catch(error=>{fontCSS=null;throw error;});
        return fontCSS;
    }
    const valid=stroke=>stroke.mathRaster?.version===VERSION&&stroke.mathRaster.source===stroke.text&&stroke.mathRaster.color===stroke.color&&stroke.mathRaster.fontSize===(stroke.fontSize||.025);
    async function decode(raster){
        if(decoded.has(raster))return decoded.get(raster);
        if(images.has(raster.png)){const image=await images.get(raster.png);decoded.set(raster,image);return image;}
        const task=new Promise((resolve,reject)=>{const image=new Image();image.onload=()=>resolve(image);image.onerror=()=>reject(new Error('公式批注图像读取失败'));image.src=raster.png;});
        images.set(raster.png,task);try{const image=await task;images.set(raster.png,image);decoded.set(raster,image);while(images.size>48)images.delete(images.keys().next().value);return image;}catch(error){images.delete(raster.png);throw error;}
    }
    async function rasterize(stroke){
        const size=stroke.fontSize||.025,maxEm=Math.max(1,(1-stroke.points[0][0])/size),key=JSON.stringify([stroke.text,stroke.color,size,maxEm]);
        if(rendered.has(key))return rendered.get(key);
        const work=(async()=>{
            const host=document.createElement('div');host.className='annotation-rich annotation-measure';
            Object.assign(host.style,{position:'fixed',left:'-10000px',top:'0',width:Math.min(1600,maxEm*32)+'px',fontSize:'32px',color:stroke.color,padding:'2px',boxSizing:'border-box',background:'transparent'});
            document.body.append(host);
            try{
                await render(stroke.text,host);await document.fonts.ready;
                const width=Math.ceil(Math.max(host.scrollWidth,host.getBoundingClientRect().width)),height=Math.ceil(host.scrollHeight);
                if(width>4096||height>16000)throw new Error('这条批注排版过长，请拆成两条；原文仍保留');
                const css=await embeddedFonts(),scale=Math.min(2,2048/width,4096/height,Math.sqrt(3900000/(width*height)));
                const inner=host.cloneNode(true);inner.removeAttribute('style');inner.setAttribute('xmlns','http://www.w3.org/1999/xhtml');inner.style.cssText=`font-size:32px;color:${stroke.color};width:${width}px;padding:2px;box-sizing:border-box;background:transparent;`;
                const svg=`<svg xmlns="http://www.w3.org/2000/svg" width="${Math.ceil(width*scale)}" height="${Math.ceil(height*scale)}" viewBox="0 0 ${width} ${height}"><foreignObject width="100%" height="100%"><div xmlns="http://www.w3.org/1999/xhtml"><style>${css+styles}</style>${new XMLSerializer().serializeToString(inner)}</div></foreignObject></svg>`;
                const image=await new Promise((resolve,reject)=>{const value=new Image();value.onload=()=>resolve(value);value.onerror=()=>reject(new Error('公式批注排版失败，原文仍保留'));value.src='data:image/svg+xml;charset=utf-8,'+encodeURIComponent(svg);});
                const canvas=document.createElement('canvas');canvas.width=image.width;canvas.height=image.height;canvas.getContext('2d').drawImage(image,0,0);
                const pixels=canvas.getContext('2d').getImageData(0,0,canvas.width,canvas.height).data;let right=1,bottom=1;
                for(let y=0;y<canvas.height;y++)for(let x=0;x<canvas.width;x++)if(pixels[(y*canvas.width+x)*4+3]){right=Math.max(right,x+2);bottom=Math.max(bottom,y+2);}
                const trimmed=document.createElement('canvas');trimmed.width=Math.min(canvas.width,right);trimmed.height=Math.min(canvas.height,bottom);trimmed.getContext('2d').drawImage(canvas,0,0);
                const png=trimmed.toDataURL('image/png');if(png.length>1400000)throw new Error('公式批注过大，请拆分后保存');
                return {version:VERSION,source:stroke.text,color:stroke.color,fontSize:size,width:trimmed.width/scale/32,height:trimmed.height/scale/32,png};
            }finally{host.remove();}
        })();
        rendered.set(key,work);try{const value=await work;while(rendered.size>32)rendered.delete(rendered.keys().next().value);return value;}catch(error){rendered.delete(key);throw error;}
    }
    async function prepare(stroke){
        if(stroke.tool!=='text')return;if(!rich(stroke.text)){delete stroke.mathRaster;return;}
        if(pending.has(stroke))return pending.get(stroke);
        const work=(async()=>{
            while(true){
                if(!rich(stroke.text)){delete stroke.mathRaster;return;}
                if(!valid(stroke)){
                    const snapshot={text:stroke.text,color:stroke.color,fontSize:stroke.fontSize,points:stroke.points.map(point=>[...point])},raster=await rasterize(snapshot);
                    if(stroke.text!==snapshot.text||stroke.color!==snapshot.color||stroke.fontSize!==snapshot.fontSize)continue;
                    stroke.mathRaster=raster;
                }
                const raster=stroke.mathRaster;await decode(raster);if(stroke.mathRaster===raster&&valid(stroke))return;
            }
        })();
        pending.set(stroke,work);try{await work;}finally{pending.delete(stroke);}
    }
    async function prepareEdit(edit){for(const stroke of edit?.strokes||[])await prepare(stroke);}
    function paint(ctx,stroke,width,height){
        if(!valid(stroke))return false;const image=decoded.get(stroke.mathRaster);if(!image)return false;
        const size=(stroke.fontSize||.025)*width,[x,y]=stroke.points[0];ctx.drawImage(image,x*width,y*height,stroke.mathRaster.width*size,stroke.mathRaster.height*size);return true;
    }
    const cssReady=new Promise((resolve,reject)=>{const css=document.createElement('link');css.rel='stylesheet';css.href=YantuEnvironment.pathname('/vendor/katex-0.19.0/dist/katex.min.css');css.onload=resolve;css.onerror=()=>reject(new Error('公式样式读取失败'));document.head.append(css);});
    const sheet=document.createElement('style');sheet.textContent=styles;document.head.append(sheet);
    const ready=edit=>(edit?.strokes||[]).every(stroke=>stroke.tool!=='text'||!rich(stroke.text)||(valid(stroke)&&decoded.has(stroke.mathRaster)));
    window.AnnotationText={render,rich,tokenize,prepare,prepareEdit,paint,valid,ready};
})();
