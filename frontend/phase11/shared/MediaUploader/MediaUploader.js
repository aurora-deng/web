import { uploadCapabilities } from '../UploadCapabilities/UploadCapabilities.js?v=4ec58ab3c1e38adb';
/** 业务媒体复用原上传/校验协议；WeakMap 保留同一 File 的成功结果以便幂等重试。 */
export class MediaUploader {
  constructor(api) { this.api=api;this.uploads=new WeakMap(); }
  async upload(file) {
    if(this.uploads.has(file))return this.uploads.get(file);
    const uploads=await uploadCapabilities(this.api);
    const image=uploads.treeImage.mediaTypes.includes(file.type.split(';')[0]);
    const voice=uploads.treeVoice.mediaTypes.includes(file.type.split(';')[0]);
    const limit=(image?uploads.treeImage:uploads.treeVoice).maxBytes;
    if((!image&&!voice)||!file.size||file.size>limit)throw new Error(`媒体格式不支持或超过 ${limit/(1024*1024)} MiB。`);
    const promise=this.perform(file);this.uploads.set(file,promise);
    try{return await promise;}catch(error){this.uploads.delete(file);throw error;}
  }
  async perform(file) {
    const api=this.api;
    const meta=await api.request('/api/tree/media/uploads',{method:'POST',data:{name:file.name,size:String(file.size),mediaType:file.type}});
    const chunkBytes=(await uploadCapabilities(api)).chat.chunkBytes;
    for(let offset=0;offset<file.size;offset+=chunkBytes) {
      const response=await fetch(`/api/files/${meta.id}/content`,{method:'PATCH',credentials:'same-origin',
        headers:{'X-CSRF-Token':api.csrf,'Content-Type':'application/octet-stream','Upload-Offset':String(offset)},body:file.slice(offset,offset+chunkBytes)});
      if(!response.ok)throw new Error('媒体上传失败，可重试。');
    }
    await api.request(`/api/files/${meta.id}/complete`,{method:'POST',data:{}});
    await api.request(`/api/tree/media/${meta.id}/validate`,{method:'POST',data:{}});return meta.id;
  }
}
