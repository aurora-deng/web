import { mountNavigation } from '../shared/AppNavigation/AppNavigation.js?v=0613976363093f95';
import { verification } from './verification.js?v=0613976363093f95';
mountNavigation('status');
const ledger=document.createElement('pre');ledger.textContent=JSON.stringify(verification,null,2);document.querySelector('main').append(ledger);
for(const [id,url] of [['runtime','/health/version'],['assets','/phase11/asset-manifest.json']]) {
  fetch(url,{cache:'no-store',credentials:'same-origin'}).then(async response=>{
    if(!response.ok)throw new Error('当前运行版本未提供此信息：HTTP '+response.status);
    const result=await response.json();
    document.getElementById(id).textContent=JSON.stringify(result,null,2);
    if(id==='runtime')document.getElementById('migration-summary').textContent=Number.isInteger(result.requiredMigration)
      ? `当前运行程序要求迁移 ${String(result.requiredMigration).padStart(3,'0')}。` : '当前运行程序未提供迁移版本。';
  }).catch(error=>{document.getElementById(id).textContent=error.message;
    if(id==='runtime')document.getElementById('migration-summary').textContent='迁移要求暂不可查。';
  });
}
