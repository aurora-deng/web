/** A shared time cursor and a bounded set of SVG paths keep the dashboard usable offline. */
import {exportButton,saveTimeChart} from "./chart_export.js";
export const palette = ["#73bf69", "#56b9d6", "#f2cc60", "#ff8e68", "#ad94e6", "#ed719e", "#8ab8ff", "#b9e28c"];
export const timestamp = (seconds, date = false) => new Date(seconds * 1000).toLocaleString("zh-CN", {
  ...(date ? {month: "2-digit", day: "2-digit"} : {}), hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false,
});
export function formatValue(value, unit) {
  if (value == null || !Number.isFinite(value)) return "—";
  const number = (n, digits = 2) => n.toLocaleString("zh-CN", {maximumFractionDigits: digits});
  if (unit === "ms" && Math.abs(value) >= 1000) return number(value / 1000) + " s";
  if (unit === "B/秒" && Math.abs(value) >= 1024 ** 2) return number(value / 1024 ** 2) + " MiB/s";
  if (unit === "B/秒" && Math.abs(value) >= 1024) return number(value / 1024) + " KiB/s";
  const digits = value !== 0 && Math.abs(value) < 0.01 ? 4 : 2;
  return number(value, digits) + (unit ? " " + unit : "");
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
  const values = series.points.filter(([, value]) => value != null).map(([, value]) => value);
  return {count: values.length, last: series.points.at(-1)?.[1] ?? null,
    min: values.length ? Math.min(...values) : null, max: values.length ? Math.max(...values) : null,
    mean: values.length ? values.reduce((a, b) => a + b, 0) / values.length : null};
}

function niceStep(max) {
  const magnitude = 10 ** Math.floor(Math.log10(max / 4));
  return ([1, 2, 2.5, 5, 10].find(n => n * magnitude >= max / 4) ?? 10) * magnitude;
}

export function drawChart(host, {model, bounds, series, markers, bus, zoom, essential = false}) {
  const svg = svgNode("svg", {class:"time-chart", role:"img", "aria-label":model.title});
  const tooltip = document.createElement("div");
  tooltip.className = "chart-tooltip";
  tooltip.hidden = true;
  let width, height, plot, cursor, band, dots, scaleX, scaleY, drag, timeAt, cursorInfo;
  const shown = () => series.filter(line => !line.hidden);
  const save=exportButton(model.title,()=>saveTimeChart(svg,model,bounds,shown()));
  host.replaceChildren(svg, tooltip, save);
  const repaint = () => {
    const rect = host.getBoundingClientRect();
    width = Math.max(240, Math.round(rect.width));
    height = Math.max(160, Math.round(rect.height));
    const left = width < 420 ? 55 : 72;
    plot = {l:left, r:width-13, t:26, b:height-26};
    svg.setAttribute("viewBox", "0 0 " + width + " " + height);
    svg.replaceChildren();
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
    for (let value = floor; value <= ceiling + tick / 100; value += tick) {
      const yy = scaleY(value);
      svg.append(svgNode("line",{x1:plot.l,x2:plot.r,y1:yy,y2:yy,class:"grid-line"}),
        svgNode("text",{x:plot.l-8,y:yy+4,"text-anchor":"end"},formatValue(value, model.unit)));
    }
    const columns = Math.max(2, Math.floor((plot.r - plot.l) / 100));
    for (let i = 0; i <= columns; i++) {
      const seconds = bounds.from + (bounds.to - bounds.from) * i / columns;
      const xx = scaleX(seconds);
      svg.append(svgNode("line",{x1:xx,x2:xx,y1:plot.t,y2:plot.b,class:"grid-line"}),
        svgNode("text",{x:xx,y:plot.b+17,"text-anchor":i===0?"start":i===columns?"end":"middle"},timestamp(seconds).slice(0,5)));
    }
    for (const line of lines) {
      let segment = [];
      const flush = () => {
        if (!segment.length) return;
        const shape = segment.map(([t, v], index) => {
          const x=scaleX(t).toFixed(1), y=scaleY(v).toFixed(1);
          return model.stepped && index ? x+","+scaleY(segment[index-1][1]).toFixed(1)+" L"+x+","+y : x+","+y;
        }).join(" L");
        if (segment.length > 1 && model.area) {
          svg.append(svgNode("path",{d:"M"+scaleX(segment[0][0])+","+scaleY(0)+" L"+shape+
            " L"+scaleX(segment.at(-1)[0])+","+scaleY(0)+" Z",fill:line.color,opacity:".13"}));
        }
        if (segment.length > 1) svg.append(svgNode("path",{d:"M"+shape,fill:"none",stroke:line.color,"stroke-width":1.45}));
        else svg.append(svgNode("circle",{cx:scaleX(segment[0][0]),cy:scaleY(segment[0][1]),r:2.4,fill:line.color}));
        segment = [];
      };
      for (const point of line.points) {
        if (point[1] == null || (segment.length && point[0]-segment.at(-1)[0] > line.step*1.6)) flush();
        if (point[1] != null) segment.push(point);
      }
      flush();
    }
    for (const marker of markers) {
      const display = scaleX(marker.time);
      if (display < plot.l || display > plot.r) continue;
      const mark = svgNode("line",{x1:display,x2:display,y1:plot.t,y2:plot.b,class:"marker-line"});
      mark.append(svgNode("title",{},marker.label));
      svg.append(mark);
    }
    band = svgNode("rect",{y:plot.t,height:plot.b-plot.t,class:"brush",visibility:"hidden"});
    cursor = svgNode("line",{y1:plot.t,y2:plot.b,class:"cursor-line",visibility:"hidden"});
    dots = svgNode("g",{class:"cursor-dots"});
    svg.append(band,cursor,dots);
    showCursor(cursorInfo);
  };
  const pointAt = event => (event.clientX - svg.getBoundingClientRect().left) * width / svg.getBoundingClientRect().width;
  const working = new AbortController();
  function showCursor(info) {
    if (!cursor) return;
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
  const watcher = new ResizeObserver(repaint);
  watcher.observe(host);
  return {
    repaint,
    paint(){if(host.isConnected) repaint();},
    update(next){
      bounds=next.bounds;series=next.series;markers=next.markers;
      if(host.isConnected) repaint();
    },
    interacting:()=>!!drag,
    dispose(){watcher.disconnect();working.abort();},
  };
}
