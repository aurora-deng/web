/** 只有用户点击才申请麦克风；公告播放不使用此组件，也不需要录音授权。 */
export class VoiceRecorder {
  constructor(button, status, completed) {
    Object.assign(this,{button,status,completed});button.onclick=()=>this.toggle();
    this.hide=()=>this.stop();addEventListener('pagehide',this.hide,{once:true});
  }
  async toggle(){
    if(this.recorder?.state==='recording'){this.stop();return;}
    if(!isSecureContext||!navigator.mediaDevices?.getUserMedia){this.status.textContent='录音需要可信 HTTPS；你也可以直接上传已有语音文件。';return;}
    this.button.disabled=true;
    try{
      this.stream=await navigator.mediaDevices.getUserMedia({audio:true});
      const mime=['audio/webm;codecs=opus','audio/ogg;codecs=opus','audio/webm'].find(type=>MediaRecorder.isTypeSupported(type));
      this.recorder=new MediaRecorder(this.stream,mime?{mimeType:mime}:{});const chunks=[];let size=0;
      this.recorder.ondataavailable=event=>{if(event.data.size){chunks.push(event.data);size+=event.data.size;if(size>10*1024*1024)this.stop();}};
      this.recorder.onstop=()=>{
        clearTimeout(this.timer);this.stream?.getTracks().forEach(t=>t.stop());this.button.textContent='● 录制一段语音';document.dispatchEvent(new CustomEvent('nebula:recording',{detail:false}));
        if(size>10*1024*1024){this.status.textContent='录音超过10MiB，请重新录制。';return;}
        const type=this.recorder.mimeType,extension=type.includes('ogg')?'ogg':'webm';
        this.completed(new File(chunks,`recording-${Date.now()}.${extension}`,{type}));this.status.textContent='录音已准备好，发布时会在服务器验证时长。';
      };
      this.recorder.start(1000);this.timer=setTimeout(()=>this.stop(),60000);this.button.textContent='■ 停止录音';this.status.textContent='正在录音，最长60秒…';document.dispatchEvent(new CustomEvent('nebula:recording',{detail:true}));
    }catch(error){this.stream?.getTracks().forEach(t=>t.stop());this.status.textContent=error.name==='NotAllowedError'?'麦克风权限被拒绝，你仍可输入文字或上传文件。':'无法启动录音，请检查设备后重试。';}
    finally{this.button.disabled=false;}
  }
  stop(){if(this.recorder?.state==='recording')this.recorder.stop();}
}
