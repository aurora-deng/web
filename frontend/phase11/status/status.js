import { mountNavigation } from '../shared/AppNavigation/AppNavigation.js?v=dc39cdb5489c964d';
import { verification } from './verification.js?v=dc39cdb5489c964d';
mountNavigation('status');
const ledger=document.createElement('pre');ledger.textContent=JSON.stringify(verification,null,2);document.querySelector('main').append(ledger);
for(const [id,url] of [['runtime','/health/version'],['assets','/phase11/asset-manifest.json']]) {
  fetch(url,{cache:'no-store',credentials:'same-origin'}).then(async response=>{
    if(!response.ok)throw new Error('当前运行版本未提供此信息：HTTP '+response.status);
    document.getElementById(id).textContent=JSON.stringify(await response.json(),null,2);
  }).catch(error=>{document.getElementById(id).textContent=error.message;});
}
