/** A shared time cursor and a bounded set of SVG paths keep the dashboard usable offline. */
import {exportButton,saveTimeChart} from "./chart_export.js";
export const palette = ["#73bf69", "#56b9d6", "#f2cc60", "#ff8e68", "#ad94e6", "#ed719e", "#8ab8ff", "#b9e28c"];
const clock = {hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false};
const timeFormat = new Intl.DateTimeFormat("zh-CN", clock);
const dateFormat = new Intl.DateTimeFormat("zh-CN", {...clock, month: "2-digit", day: "2-digit"});
const ordinaryNumber = new Intl.NumberFormat("zh-CN", {maximumFractionDigits: 2});
const fractionalNumber = new Intl.NumberFormat("zh-CN", {maximumFractionDigits: 4});
export const timestamp = (seconds, date = false) => {
  const value=new Date(seconds*1000);
  return Number.isNaN(value.getTime())?"Invalid Date":(date?dateFormat:timeFormat).format(value);
};
export function formatValue(value, unit) {
  if (value == null || !Number.isFinite(value)) return "—";
  if (unit === "ms" && Math.abs(value) >= 1000) return ordinaryNumber.format(value / 1000) + " s";
  if (unit === "B/秒" && Math.abs(value) >= 1024 ** 2) return ordinaryNumber.format(value / 1024 ** 2) + " MiB/s";
  if (unit === "B/秒" && Math.abs(value) >= 1024) return ordinaryNumber.format(value / 1024) + " KiB/s";
  const format = value !== 0 && Math.abs(value) < 0.01 ? fractionalNumber : ordinaryNumber;
  return format.format(value) + (unit ? " " + unit : "");
}

const ns = "http://www.w3.org/2000/svg";
function svgNode(tag, props = {}, label) {
  const el = document.createElementNS(ns, tag);
  for (const [key, value] of Object.entries(props)) el.setAttribute(key, value);
  if (label !== undefined) el.textContent = label;
  return el;
}

function nearest(points, time, step) {
  let lo = 0, hi = points.length;
  while (lo < hi) {
    const middle = Math.floor((lo + hi) / 2);
    if (points[middle][0] < time) lo = middle + 1;
    else hi = middle;
  }
  const a = points[lo - 1], b = points[lo];
  const result = !a || (b && b[0] - time < time - a[0]) ? b : a;
  return result && Math.abs(result[0] - time) <= step * 0.65 ? result : null;
}

export function extrema(series) {
  let count=0, min=Infinity, max=-Infinity, sum=0;
  for(const [,value] of series.points) if(value!=null) {
    count++; min=Math.min(min,value); max=Math.max(max,value); sum+=value;
  }
  return {count,last:series.points.at(-1)?.[1]??null,min:count?min:null,max:count?max:null,mean:count?sum/count:null};
}

function niceStep(max) {
  const magnitude = 10 ** Math.floor(Math.log10(max / 4));
  return ([1, 2, 2.5, 5, 10].find(n => n * magnitude >= max / 4) ?? 10) * magnitude;
}

function curvePaths(points,step,model,x,y) {
  const stroke=[],fill=[],dots=[];
  let shape=[],start,previous;
  const flush=()=>{
    if(shape.length===1) {
      dots.push("M"+(x(start[0])-2.4).toFixed(1)+","+y(start[1]).toFixed(1)+
        " a2.4,2.4 0 1 0 4.8,0 a2.4,2.4 0 1 0 -4.8,0");
    } else if(shape.length>1) {
      const path=shape.join(" L");
      stroke.push("M"+path);
      if(model.area)fill.push("M"+x(start[0])+","+y(0)+" L"+path+" L"+x(previous[0])+","+y(0)+" Z");
    }
    shape=[];previous=undefined;
  };
  for(const point of points) {
    const [stamp,value]=point;
    if(value==null||(previous&&stamp-previous[0]>step*1.6))flush();
    if(value==null)continue;
    const xx=x(stamp).toFixed(1), yy=y(value).toFixed(1);
    if(!previous)start=point;
    if(model.stepped&&previous)shape.push(xx+","+y(previous[1]).toFixed(1));
    shape.push(xx+","+yy);
    previous=point;
  }
  flush();
  return [fill.join(" "),stroke.join(" "),dots.join(" ")];
}

export function drawChart(host, {model, bounds, series, markers, bus, zoom, essential = false}) {
  const svg = svgNode("svg", {class:"time-chart", role:"img", "aria-label":model.title});
  const tooltip = document.createElement("div");
  tooltip.className = "chart-tooltip";
  tooltip.hidden = true;
  let width, height, plot, scaleX, scaleY, drag, timeAt, cursorInfo, gridKey;
  const grid = svgNode("g"), curves = svgNode("g"), flags = svgNode("g"), paths=new Map();
  const band = svgNode("rect",{class:"brush",visibility:"hidden"});
  const cursor = svgNode("line",{class:"cursor-line",visibility:"hidden"});
  const dots = svgNode("g",{class:"cursor-dots"});
  svg.append(grid,curves,flags,band,cursor,dots);
  const shown = () => series.filter(line => !line.hidden);
  const save=exportButton(model.title,()=>saveTimeChart(svg,model,bounds,shown()));
  host.replaceChildren(svg, tooltip, save);
  const drawMarkers = () => {
    if(!plot) return;
    const layer=document.createDocumentFragment();
    for(const marker of markers) {
      const display=scaleX(marker.time);
      if(display<plot.l||display>plot.r)continue;
      const mark=svgNode("line",{x1:display,x2:display,y1:plot.t,y2:plot.b,class:"marker-line"});
      mark.append(svgNode("title",{},marker.label));
      layer.append(mark);
    }
    flags.replaceChildren(layer);
  };
  const repaint = (rect=host.getBoundingClientRect()) => {
    width = Math.max(240, Math.round(rect.width));
    height = Math.max(160, Math.round(rect.height));
    const left = width < 420 ? 55 : 72;
    plot = {l:left, r:width-13, t:26, b:height-26};
    svg.setAttribute("viewBox", "0 0 " + width + " " + height);
    const lines = shown();
    save.hidden=!lines.some(line=>line.points.some(([,value])=>value!=null));
    let min=0,max=0;
    for(const series of lines) for(const [,v] of series.points) if(v!=null) {min=Math.min(min,v);max=Math.max(max,v);}
    if(min===max) max=1;
    const tick = model.cap ? model.cap / 4 : niceStep(max-min);
    const ceiling = model.cap || Math.ceil(max / tick) * tick;
    const floor = model.cap ? 0 : Math.floor(min / tick) * tick;
    scaleX = t => plot.l + (t - bounds.from) / (bounds.to - bounds.from) * (plot.r - plot.l);
    scaleY = v => plot.b - (v-floor) / (ceiling-floor) * (plot.b - plot.t);
    timeAt = x => Math.max(bounds.from, Math.min(bounds.to, bounds.from +
      (x - plot.l) * (bounds.to - bounds.from) / (plot.r - plot.l)));
    const key=[width,height,bounds.from,bounds.to,floor,ceiling,tick].join(",");
    if(key!==gridKey) {
      gridKey=key;
      const layer=document.createDocumentFragment();
      for(let value=floor;value<=ceiling+tick/100;value+=tick) {
        const yy=scaleY(value);
        layer.append(svgNode("line",{x1:plot.l,x2:plot.r,y1:yy,y2:yy,class:"grid-line"}),
          svgNode("text",{x:plot.l-8,y:yy+4,"text-anchor":"end"},formatValue(value,model.unit)));
      }
      const columns=Math.max(2,Math.floor((plot.r-plot.l)/100));
      for(let i=0;i<=columns;i++) {
        const seconds=bounds.from+(bounds.to-bounds.from)*i/columns, xx=scaleX(seconds);
        layer.append(svgNode("line",{x1:xx,x2:xx,y1:plot.t,y2:plot.b,class:"grid-line"}),
          svgNode("text",{x:xx,y:plot.b+17,"text-anchor":i===0?"start":i===columns?"end":"middle"},timestamp(seconds).slice(0,5)));
      }
      grid.replaceChildren(layer);
    }
    const visibleKeys=new Set();
    lines.forEach((line,index)=>{
      const id=line.id??index;
      visibleKeys.add(id);
      let layer=paths.get(id);
      if(!layer) {
        layer=svgNode("g");
        layer.append(svgNode("path",{opacity:".13"}),
          svgNode("path",{fill:"none","stroke-width":1.45}),svgNode("path"));
        paths.set(id,layer);
      }
      const colors=["fill","stroke","fill"];
      curvePaths(line.points,line.step,model,scaleX,scaleY).forEach((d,i)=>{
        const path=layer.children[i];
        if(d!==path.getAttribute("d"))path.setAttribute("d",d);
        if(line.color!==path.getAttribute(colors[i]))path.setAttribute(colors[i],line.color);
      });
      if(curves.children[index]!==layer)curves.insertBefore(layer,curves.children[index]||null);
    });
    for(const [id,layer] of paths) if(!visibleKeys.has(id)) {
      layer.remove();paths.delete(id);
    }
    drawMarkers();
    band.setAttribute("y",plot.t);
    band.setAttribute("height",plot.b-plot.t);
    cursor.setAttribute("y1",plot.t);
    cursor.setAttribute("y2",plot.b);
    showCursor(cursorInfo);
  };
  const pointAt = event => (event.clientX - svg.getBoundingClientRect().left) * width / svg.getBoundingClientRect().width;
  const working = new AbortController();
  function showCursor(info) {
    if (!plot) return;
    tooltip.hidden = true;
    dots.replaceChildren();
    if (info == null || info.time < bounds.from || info.time > bounds.to) {
      cursor.setAttribute("visibility","hidden");
      return;
    }
    const x = scaleX(info.time);
    cursor.setAttribute("x1",x); cursor.setAttribute("x2",x);
    cursor.setAttribute("visibility","visible");
    const readings = shown().map(line => [line, nearest(line.points, info.time, line.step)]);
    for (const [line, value] of readings) {
      if (value?.[1] != null) dots.append(svgNode("circle",{cx:scaleX(value[0]),cy:scaleY(value[1]),r:3,
        fill:line.color,stroke:"#18191d","stroke-width":1.3}));
    }
    if (info.owner !== host && !essential) return;
    const time = document.createElement("strong");
    time.textContent = timestamp(info.time,true);
    tooltip.replaceChildren(time);
    for (const [line, reading] of readings.slice(0,8)) {
      const row = document.createElement("div");
      row.style.setProperty("--series",line.color);
      const label = document.createElement("b");
      label.textContent = formatValue(reading?.[1],model.unit);
      row.append(document.createTextNode(line.name),label);
      tooltip.append(row);
    }
    tooltip.hidden = false;
    tooltip.style.left = Math.max(0,Math.min(width-tooltip.offsetWidth,x+12))+"px";
    tooltip.style.top = Math.max(0,Math.min(height-tooltip.offsetHeight,16))+"px";
  }
  bus.addEventListener("cursor",event=>{
    cursorInfo=event.detail;
    showCursor(cursorInfo);
  },{signal:working.signal});
  svg.addEventListener("pointermove",event=>{
    if (event.pointerType === "touch") return;
    const x = pointAt(event);
    if (drag) {
      band.setAttribute("x",Math.max(plot.l,Math.min(x,drag.x)));
      band.setAttribute("width",Math.abs(scaleX(timeAt(x))-drag.x));
      band.setAttribute("visibility","visible");
    }
    bus.dispatchEvent(new CustomEvent("cursor",{detail:{time:timeAt(x),owner:host}}));
  });
  svg.addEventListener("pointerleave",()=>bus.dispatchEvent(new CustomEvent("cursor")));
  svg.addEventListener("pointerdown",event=>{
    if (event.button!==0 || event.pointerType==="touch") return;
    const x=pointAt(event);
    if (x < plot.l || x > plot.r) return;
    drag={x,time:timeAt(x)};
    svg.setPointerCapture(event.pointerId);
  });
  svg.addEventListener("pointerup",event=>{
    if (!drag) return;
    const end=timeAt(pointAt(event)), start=drag.time;
    drag=null;
    band.setAttribute("visibility","hidden");
    if (Math.abs(end-start) >= Math.max(15,(bounds.to-bounds.from)*0.017))
      zoom({from:Math.floor(Math.min(start,end)),to:Math.ceil(Math.max(start,end))});
  });
  svg.addEventListener("pointercancel",()=>{drag=null;band.setAttribute("visibility","hidden");});
  const watcher = new ResizeObserver(([entry])=>{
    const rect=entry.contentRect;
    if(Math.max(240,Math.round(rect.width))!==width||Math.max(160,Math.round(rect.height))!==height)repaint(rect);
  });
  watcher.observe(host);
  return {
    repaint,
    paint(){if(host.isConnected) repaint();},
    update(next){
      bounds=next.bounds;series=next.series;markers=next.markers;
      if(host.isConnected) repaint();
    },
    setMarkers(next){
      if(next.length===markers.length&&next.every((marker,i)=>marker.time===markers[i].time&&marker.label===markers[i].label))return;
      markers=next;
      drawMarkers();
    },
    interacting:()=>!!drag,
    dispose(){watcher.disconnect();working.abort();},
  };
}
