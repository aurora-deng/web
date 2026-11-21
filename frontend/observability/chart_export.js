const typeface='"Microsoft YaHei UI", system-ui, sans-serif';
const background="#1c1d22";

export function exportButton(title,save) {
  const button=document.createElement("button");
  button.type="button";
  button.className="chart-export";
  button.textContent="导出 PNG";
  button.setAttribute("aria-label","导出"+title+"为 PNG");
  button.onclick=async()=>{
    button.disabled=true;
    button.textContent="正在导出…";
    try {
      await save();
      button.textContent="导出 PNG";
      button.removeAttribute("title");
    } catch(error) {
      button.textContent="重试导出";
      button.title=error.message;
    } finally {
      button.disabled=false;
    }
  };
  return button;
}

function canvasFor(width,height) {
  const canvas=document.createElement("canvas");
  const scale=Math.min(2,2400/width);
  canvas.width=Math.ceil(width*scale);
  canvas.height=Math.ceil(height*scale);
  const context=canvas.getContext("2d");
  if(!context) throw Error("画布不可用");
  context.scale(scale,scale);
  context.fillStyle=background;
  context.fillRect(0,0,width,height);
  return {canvas,context};
}

async function download(canvas,title) {
  const blob=await new Promise(resolve=>canvas.toBlob(resolve,"image/png"));
  if(!blob) throw Error("图像编码失败");
  const url=URL.createObjectURL(blob);
  const link=document.createElement("a");
  link.href=url;
  const filename=title.replace(/[<>:"/\\|?*\x00-\x1f]/g," ").trim().slice(0,80) || "图表";
  link.download="NEBULA-"+filename+"-"+Date.now()+".png";
  link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}

function timeLabel(value) {
  return new Date(value*1000).toLocaleString("zh-CN",{hour12:false});
}

function svgImage(svg) {
  const copy=svg.cloneNode(true);
  copy.querySelectorAll(".brush,.cursor-line,.cursor-dots").forEach(element=>element.remove());
  const {width,height}=svg.viewBox.baseVal;
  copy.setAttribute("width",width);
  copy.setAttribute("height",height);
  const css=document.createElementNS(svg.namespaceURI,"style");
  css.textContent='text{font:10px '+typeface+';fill:#b4b8c6}.grid-line{stroke:#353740;stroke-width:1}'+
    '.marker-line{stroke:#f2cc60;stroke-width:1;stroke-dasharray:4 4}';
  copy.prepend(css);
  const url=URL.createObjectURL(new Blob([new XMLSerializer().serializeToString(copy)],{type:"image/svg+xml;charset=utf-8"}));
  const image=new Image();
  image.src=url;
  return image.decode().then(()=>image).finally(()=>URL.revokeObjectURL(url));
}

export async function saveTimeChart(svg,model,bounds,lines) {
  const image=await svgImage(svg);
  const width=Math.max(700,image.width);
  const perRow=width>1000?4:3, legendRows=Math.ceil(lines.length/perRow);
  const {canvas,context}=canvasFor(width,image.height+80+legendRows*21);
  context.fillStyle="#e9eaf0";
  context.font="600 17px "+typeface;
  context.fillText(model.title,16,27,width-32);
  context.fillStyle="#b4b8c6";
  context.font="12px "+typeface;
  const zone=Intl.DateTimeFormat().resolvedOptions().timeZone;
  context.fillText(timeLabel(bounds.from)+" — "+timeLabel(bounds.to)+"  "+zone,16,49,width-32);
  context.drawImage(image,(width-image.width)/2,55);
  lines.forEach((line,index)=>{
    const columnWidth=(width-32)/perRow;
    const x=16+index%perRow*columnWidth,y=image.height+75+Math.floor(index/perRow)*21;
    context.fillStyle=line.color;
    context.fillRect(x,y-7,12,3);
    context.fillStyle="#e9eaf0";
    context.fillText(line.name,x+19,y,columnWidth-25);
  });
  await download(canvas,model.title);
}

export async function saveCommitChart(report) {
  const width=960;
  const {canvas,context}=canvasFor(width,104+report.longest.length*26);
  context.fillStyle="#e9eaf0";
  context.font="600 18px "+typeface;
  context.fillText(report.label+" · 最慢 COMMIT",22,31,width-44);
  context.font="13px "+typeface;
  const mode=report.controlDelayMs?"注入延迟 "+report.controlDelayMs+" ms":"自然负载";
  context.fillText(mode+" · 归档于 "+timeLabel(report.started)+" · 排名按 COMMIT 耗时",22,55,width-44);
  context.fillText("名次 / COMMIT 耗时",22,79);
  context.fillText("客户端 ACK / 数据库进程样本",634,79);
  const maximum=report.longest[0].commitUs||1;
  report.longest.forEach((sample,index)=>{
    const y=107+index*26;
    const waits=Object.keys(sample.backendWaitSamples).join("、")||"未匹配";
    context.fillStyle="#e9eaf0";
    context.fillText((index+1)+" · "+sample.commitUs/1000+" ms",22,y,141);
    context.fillText(sample.ackMs+" ms · "+waits,634,y,width-650);
    context.fillStyle="#ffa762";
    context.fillRect(174,y-10,440*sample.commitUs/maximum,8);
  });
  await download(canvas,report.label+"-COMMIT");
}
