import { MediaUploader } from '../MediaUploader/MediaUploader.js';
/** 树和主页共用一条发布链路；公开范围确认进入服务器契约，不能只改 UI 提示。 */
export class ActivityPublisher {
  constructor(form,api,{initialDestination='profile',files=()=>[],status,onPublished}) {
    this.form=form;this.api=api;this.files=files;this.status=status;this.onPublished=onPublished;this.uploader=new MediaUploader(api);
    const box=document.createElement('section');box.className='publisher-audience';
    box.innerHTML='<label>发布到<select name="destination"><option value="profile">个人主页</option><option value="tree">苦情树</option><option value="both">主页与苦情树</option></select></label><label>谁可以看<select name="visibility"><option value="friends">好友可见</option><option value="members">登录用户可见</option><option value="private">仅自己</option></select></label><label class="publisher-public-confirm" hidden><input name="publicConfirmed" type="checkbox">我确认：苦情树内容对所有登录用户可见</label>';
    const header=form.querySelector('header');if(header)header.after(box);else form.prepend(box);
    form.elements.destination.value=initialDestination;form.elements.visibility.value=initialDestination==='profile'?'friends':'members';
    const change=()=>{
      const tree=form.elements.destination.value!=='profile';
      form.elements.visibility.disabled=tree;
      if(tree)form.elements.visibility.value='members';
      box.querySelector('.publisher-public-confirm').hidden=!tree;
      form.elements.publicConfirmed.required=tree;
    };
    form.elements.destination.addEventListener('change',()=>{form.elements.publicConfirmed.checked=false;if(form.elements.destination.value==='profile')form.elements.visibility.value='friends';change();});
    this.updateAudience=change;change();form.addEventListener('submit',event=>{event.preventDefault();this.submit();});
  }
  async submit() {
    if(this.busy||!this.form.reportValidity())return;
    const form=this.form,values=new FormData(form),body=String(values.get('body')||'').trim();
    const destination=form.elements.destination.value,visibility=form.elements.visibility.value;
    if([...body].length>2000){this.status.textContent='正文最多 2,000 字。';return;}
    const data={body,destination,visibility,categoryId:String(values.get('categoryId')||'0'),mood:String(values.get('mood')||''),
      tags:String(values.get('tags')||'').trim().split(/\s+/u).filter(Boolean).join('\n'),fileIds:'',publicConfirmed:String(form.elements.publicConfirmed.checked)};
    if(destination!=='profile'&&data.categoryId==='0'){this.status.textContent='发布到苦情树时请选择主题分类。';return;}
    this.status.classList.remove('error');
    const submit=form.querySelector('button[type="submit"],button:not([type])');this.busy=true;submit.disabled=true;
    try {
      const ids=[];for(const file of this.files()){this.status.textContent=`正在上传并校验 ${file.name}`;ids.push(await this.uploader.upload(file));}
      data.fileIds=ids.join(',');const signature=JSON.stringify(data);
      if(this.signature!==signature){this.signature=signature;this.requestId=Array.from(crypto.getRandomValues(new Uint8Array(16)),n=>n.toString(16).padStart(2,'0')).join('');}data.requestId=this.requestId;
      const post=await this.api.request('/api/activities',{method:'POST',data});
      this.signature='';this.status.textContent='已发布';form.reset();
      form.elements.destination.value=destination;form.elements.visibility.value=visibility;this.updateAudience();
      form.dispatchEvent(new CustomEvent('activity:published',{detail:post}));await this.onPublished(post);
    }catch(error){this.status.textContent=error.message;this.status.classList.add('error');}
    finally{this.busy=false;submit.disabled=false;}
  }
}
