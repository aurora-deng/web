import { apiCatalog } from './ApiCatalog.js?v=dc39cdb5489c964d';

/** A route contract is a transport descriptor, never an authorization decision. */
const patterns=apiCatalog.map(row=>({...row,pattern:new RegExp('^'+row.path.split('/').map(part=>part.startsWith(':')?'[^/?#]+':part.replace(/[.*+?^${}()|[\]\\]/g,'\\$&')).join('/')+'$')}));
// Literal subresources such as /activities/favorites precede /activities/:post.
patterns.sort((a,b)=>(a.path.match(/:/g)||[]).length-(b.path.match(/:/g)||[]).length||b.path.length-a.path.length);
export function assertApplicationRoute(path,method='GET') {
  if(typeof path!=='string'||!path.startsWith('/')||path.startsWith('//')||path.includes('#'))throw new TypeError('接口必须使用已登记的站内路径');
  const pathname=path.split('?')[0];
  const row=patterns.find(row=>row.method===method&&row.pattern.test(pathname));
  if(!row)throw new TypeError(`未登记的业务接口：${method} ${pathname}`);
  return row;
}
export function defineEndpoints(domain,definitions) {
  return Object.freeze(Object.fromEntries(definitions.map(([operation,method,template])=>[operation,(parameters={},query='')=>{
    const path=template.replace(/:([A-Za-z][A-Za-z0-9_]*)/g,(_,name)=>{
      const value=parameters[name];
      if(value===undefined||value===null||String(value)==='')throw new TypeError(`接口参数缺失：${operation}.${name}`);
      return encodeURIComponent(String(value));
    });
    const suffix=String(query??'').replace(/^\?/,'');
    return Object.freeze({domain,operation,method,path:path+(suffix?'?'+suffix:'')});
  }])));
}
export function normalizeApplicationRequest(input,options={}) {
  const descriptor=typeof input==='string'?null:input;
  if(descriptor&&(!descriptor.path||!descriptor.method))throw new TypeError('无效的业务接口描述');
  const path=descriptor?descriptor.path:input;
  const method=String(options.method||descriptor?.method||'GET').toUpperCase();
  if(descriptor&&method!==descriptor.method)throw new TypeError('接口方法与契约不一致');
  const contract=assertApplicationRoute(path,method);
  if(descriptor&&(descriptor.domain!==contract.domain||descriptor.operation!==contract.operation))throw new TypeError('接口领域与契约不一致');
  return {path,method,contract};
}
