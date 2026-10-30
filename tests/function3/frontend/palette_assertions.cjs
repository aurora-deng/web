const assert=require('node:assert/strict');
/** Test-only effective solid backgrounds; gradients are reported separately. */
exports.assertPalette=async function(page,selectors){
  const result=await page.evaluate(selectors=>{
    function parse(value){
      if(value==='transparent')return [0,0,0,0];
      const numbers=value.match(/[\d.]+/g)?.map(Number);
      if(value.startsWith('color(srgb'))return [...numbers.slice(0,3),numbers[3]??1];
      if(value.startsWith('rgb'))return [...numbers.slice(0,3).map(v=>v/255),numbers[3]??1];
      // CSS transitions may expose OKLab even when the declared palette is RGB.
      // Convert through a test-only 1px canvas; production rendering does no readback.
      const canvas=document.createElement('canvas');canvas.width=canvas.height=1;
      const context=canvas.getContext('2d',{willReadFrequently:true});
      context.fillStyle=value;context.fillRect(0,0,1,1);
      return [...context.getImageData(0,0,1,1).data].map(v=>v/255);
    }
    const blend=(a,b)=>a.slice(0,3).map((v,i)=>v*a[3]+b[i]*(1-a[3])).concat(1);
    const luminance=c=>c.slice(0,3).map(v=>v<=.04045?v/12.92:((v+.055)/1.055)**2.4).reduce((v,x,i)=>v+x*[.2126,.7152,.0722][i],0);
    return selectors.flatMap(selector=>[...document.querySelectorAll(selector)].filter(e=>e.getBoundingClientRect().width&&e.getBoundingClientRect().height).map(e=>{
      const ancestors=[];for(let n=e;n;n=n.parentElement)ancestors.push(n);
      if(ancestors.some(n=>getComputedStyle(n).backgroundImage!=='none'))return {selector,gradient:true};
      let bg=[1,1,1,1];for(const n of ancestors.reverse())bg=blend(parse(getComputedStyle(n).backgroundColor),bg);
      const color=parse(getComputedStyle(e).color),fg=blend(color,bg),a=luminance(fg),b=luminance(bg);
      return {selector,contrast:(Math.max(a,b)+.05)/(Math.min(a,b)+.05),foreground:getComputedStyle(e).color,background:bg};
    }));
  },selectors);
  for(const row of result)if(!row.gradient)assert(row.contrast>=4.5,`Unreadable text: ${JSON.stringify(row)}`);
  assert(result.some(r=>!r.gradient),'No solid text/background pair was measured');
  return result;
};
