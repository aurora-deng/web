import { filesApi } from "../FilesApi/FilesApi.js?v=dc39cdb5489c964d";
export const dependencies={"resources":["$","state"],"commands":["api","formatBytes"],"types":[]};
/** AttachmentController: explicit use cases; shared state/other controllers arrive through context. */
export function createAttachmentController(context) {
  const {$,state,actions}=context;

  const api = (...args) => actions.api(...args);
  const formatBytes = (...args) => actions.formatBytes(...args);
function declaredMediaType(file) {
  if (file.type) return file.type;
  const extension = file.name.toLowerCase().split('.').pop();
  return ({
    md: 'text/markdown', txt: 'text/plain', json: 'application/json',
    zip: 'application/zip', bin: 'application/octet-stream',
    docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  })[extension] || 'application/octet-stream';
}

async function uploadFile(file, onProgress = () => {}) {
  const created = await api(filesApi.postFilesUploads({}), {
    method: 'POST',
    body: JSON.stringify({
      name: file.name,
      mediaType: declaredMediaType(file),
      size: String(file.size),
    }),
  });
  const chunkSize = state.capabilities?.uploads.chat.chunkBytes || 256 * 1024;
  let offset = Number(created.uploadedSize || 0);
  while (offset < file.size) {
    const chunk = file.slice(offset, Math.min(offset + chunkSize, file.size));
    let response;
    try {
      response = await fetch(filesApi.patchFilesFileContent({"file":(created.id)}) .path, {
        method: 'PATCH',
        credentials: 'same-origin',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-CSRF-Token': state.csrf,
          'Upload-Offset': String(offset),
        },
        body: chunk,
      });
    } catch {
      throw new Error('文件分片上传失败，请检查网络后重试');
    }
    const text = await response.text();
    let result = {};
    try { result = text ? JSON.parse(text) : {}; } catch { result = { message: text }; }
    if (!response.ok) throw new Error(result.message || `上传失败（HTTP ${response.status}）`);
    const next = Number(result.uploadedSize);
    if (!Number.isSafeInteger(next) || next <= offset)
      throw new Error('服务器返回了无效的上传游标');
    offset = next;
    onProgress(offset / file.size);
  }
  return api(filesApi.postFilesFileComplete({"file":(created.id)}), { method: 'POST' });
}

function showSelectedFile(file, progress = null) {
  state.selectedFile = file || null;
  $('#selected-file').classList.toggle('hidden', !file);
  $('#selected-file-name').textContent = file ? `${file.name} · ${formatBytes(file.size)}` : '';
  $('#selected-file-progress').textContent = progress === null ? '' : `${Math.round(progress * 100)}%`;
}

  return {declaredMediaType,uploadFile,showSelectedFile};
}
