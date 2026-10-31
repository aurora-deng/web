import { sharedApi } from "../SharedApi/SharedApi.js?v=dc39cdb5489c964d";
const cache=new WeakMap();
/** A server contract, not a second client-side source of policy numbers. */
export function uploadCapabilities(api) {
  if(!cache.has(api))cache.set(api,api.request(sharedApi.getClientCapabilities({})).then(value=>{
    if(!value?.uploads?.chat || !value.uploads.treeImage || !value.uploads.treeVoice)throw new Error('服务器上传规则未就绪，请刷新页面后重试');
    return value.uploads;
  }).catch(error=>{cache.delete(api);throw error;}));
  return cache.get(api);
}
export async function paintUploadCapabilities(api,form) {
  const uploads=await uploadCapabilities(api);
  const input=form.querySelector('input[type=file]');
  if(input)input.accept=[...uploads.treeImage.mediaTypes,...uploads.treeVoice.mediaTypes].join(',');
  let note=form.querySelector('[data-upload-policy]');
  if(!note){note=document.createElement('small');note.dataset.uploadPolicy='';input?.closest('label')?.after(note);}
  const mb=bytes=>Number(bytes)/(1024*1024);
  note.textContent=`最多 ${uploads.treeImage.maxCount} 张图片（每张 ${mb(uploads.treeImage.maxBytes)} MiB）与一段语音（${uploads.treeVoice.maxDurationMs/1000} 秒、${mb(uploads.treeVoice.maxBytes)} MiB）。内容不会自动用于训练。`;
  return uploads;
}
