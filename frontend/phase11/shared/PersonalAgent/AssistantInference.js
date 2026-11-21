const id = () => Array.from(crypto.getRandomValues(new Uint8Array(20)), number => number.toString(16).padStart(2, '0')).join('');

function waitForUpdate(signal) {
  return new Promise((resolve, reject) => {
    if (signal.aborted) return reject(signal.reason);
    const abort = () => { clearTimeout(timer); reject(signal.reason); };
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, 450);
    signal.addEventListener('abort', abort, { once: true });
  });
}

export async function assistantInference(api, messages, { model, signal, maxTokens, onText = () => {}, web = false }) {
  if (!model) throw new Error('先在模型连接中选择一个可用模型。');
  const requestId = id();
  const payload = { requestId, aiUserId: model, messageCount: String(messages.length), maxOutputTokens: String(maxTokens) };
  if (web) payload.nonConfidentialConsent = 'true';
  messages.forEach((message, index) => {
    payload[`role${index}`] = message.role;
    payload[`content${index}`] = message.content;
  });
  let created = false;
  let answer = '';
  let after = 0;
  try {
    await api.start(payload, signal);
    created = true;
    while (!signal.aborted) {
      const result = await api.poll(requestId, after, signal);
      answer += result.pieces.join('');
      after = result.next;
      onText(answer);
      if (!['queued', 'running'].includes(result.state) && result.pieces.length < 64) {
        if (result.state !== 'completed') throw new Error(result.state === 'cancelled' ? '生成已停止' : '模型未完成回答，请测试连接。');
        return answer;
      }
      await waitForUpdate(signal);
    }
    throw signal.reason;
  } finally {
    if (created || signal.aborted) api.stop(requestId).catch(() => {});
  }
}
