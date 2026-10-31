import { trainingApi } from "../../training/TrainingApi/TrainingApi.js?v=dc39cdb5489c964d";
import { createChatScope } from '../../chat/ChatScope/ChatScope.js?v=dc39cdb5489c964d';
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","createClientId","setButtonBusy","api","setRealtimeState","openTrainingConsent","loadTrainingCandidates","sendSocket","loadAiModels"],"types":[],"events":true};
/** Ai UI subscriptions. All listeners belong to the page lifecycle. */
export function bindAi(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,listen}=scope;
  const {setStatus,toast,createClientId,setButtonBusy,api,setRealtimeState,openTrainingConsent,loadTrainingCandidates,sendSocket,loadAiModels}=scope.actions;
listen($('#toggle-ai'), 'click', event => {
  const active = event.currentTarget.getAttribute('aria-pressed') !== 'true';
  event.currentTarget.setAttribute('aria-pressed', String(active));
  $('#ai-target-wrap').classList.toggle('hidden', !active);
  elements.messageForm.elements.message.placeholder = active
    ? '输入给 AI 的提示词，Enter 发送'
    : '输入消息，Enter 发送，Shift + Enter 换行';
  // 只在打开时读一次；失败不抛，交给提交时的空值分支给出可操作提示。
  if (active) loadAiModels();
});

// 只有真实用户操作才会触发 change（脚本赋值不会），用它区分「手选」与「自动识别」。
listen(elements.messageForm.elements.aiUserId, 'change', () => {
  state.aiTargetManual = true;
});

listen($('#cancel-generation'), 'click', event => {
  const generationId = event.currentTarget.dataset.generationId;
  if (!generationId) return;
  sendSocket({ v: 1, type: 'ai.cancel', id: createClientId(), replyTo: generationId });
});






listen($('#show-training'), 'click', () => openTrainingConsent());
listen($('#training-form'), 'submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const sourceMessageId = String(form.elements.sourceMessageId.value || '').trim();
  if (!sourceMessageId) {
    setStatus($('#training-status'),
      '请先在一条已完成的 AI 回复下点击“授权训练”。', 'error');
    return;
  }
  if (!form.elements.consentGranted.checked) {
    setStatus($('#training-status'), '需要明确勾选训练授权。', 'error');
    return;
  }
  setButtonBusy(button, true, '正在提交…');
  try {
    await api(trainingApi.postTrainingCandidates({}), {
      method: 'POST',
      body: JSON.stringify({
        sourceMessageId,
        sanitizedPrompt: form.elements.sanitizedPrompt.value.trim(),
        sanitizedResponse: form.elements.sanitizedResponse.value.trim(),
        consentGranted: 'true',
      }),
    });
    form.reset();
    $('#training-source-hint').textContent =
      '样本已提交；审核由管理员在服务器命令行工具执行，网页不提供审核入口。';
    setStatus($('#training-status'), '训练样本已提交，等待离线审核。', 'success');
    toast('训练授权已记录', 'success');
    await loadTrainingCandidates();
  } catch (error) {
    setStatus($('#training-status'), error.message, 'error');
  } finally {
    setButtonBusy(button, false);
  }
});
listen($('#show-model-status'), 'click', () => {
  setRealtimeState('ws', state.realtime.ws);
  setRealtimeState('sse', state.realtime.sse);
  elements.modelDialog.showModal();
});
}
