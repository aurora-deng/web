/** 业务媒体复用原上传/校验协议；WeakMap 保留同一 File 的成功结果以便幂等重试。 */
export class MediaUploader {
  constructor(api) { this.api=api;this.uploads=new WeakMap(); }
  async upload(file) {
    if(this.uploads.has(file))return this.uploads.get(file);
    if(!file.size||file.size>10*1024*1024)throw new Error('媒体须非空且不超过 10 MiB。');
    const promise=this.perform(file);this.uploads.set(file,promise);
    try{return await promise;}catch(error){this.uploads.delete(file);throw error;}
  }
  async perform(file) {
    const api=this.api;
    const meta=await api.request('/api/tree/media/uploads',{method:'POST',data:{name:file.name,size:String(file.size),mediaType:file.type}});
    for(let offset=0;offset<file.size;offset+=256*1024) {
      const response=await fetch(`/api/files/${meta.id}/content`,{method:'PATCH',credentials:'same-origin',
        headers:{'X-CSRF-Token':api.csrf,'Content-Type':'application/octet-stream','Upload-Offset':String(offset)},body:file.slice(offset,offset+256*1024)});
      if(!response.ok)throw new Error('媒体上传失败，可重试。');
    }
    await api.request(`/api/files/${meta.id}/complete`,{method:'POST',data:{}});
    await api.request(`/api/tree/media/${meta.id}/validate`,{method:'POST',data:{}});return meta.id;
  }
}
