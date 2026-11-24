import { declaredMediaType, uploadAttachment } from '../AttachmentUpload/AttachmentUpload.js?v=9fcaa747c305a8cc';
export const dependencies={"resources":["$","state"],"commands":["api","formatBytes"],"types":[]};
/** AttachmentController: explicit use cases; shared state/other controllers arrive through context. */
export function createAttachmentController(context) {
  const {$,state,actions}=context;

  const api = (...args) => actions.api(...args);
  const formatBytes = (...args) => actions.formatBytes(...args);
function uploadFile(file, onProgress) {
  return uploadAttachment(file, {
    request: api, csrf: state.csrf, chunkSize: state.capabilities?.uploads.chat.chunkBytes || 256 * 1024, onProgress,
  });
}

function showSelectedFile(file, progress = null) {
  state.selectedFile = file || null;
  $('#selected-file').classList.toggle('hidden', !file);
  $('#selected-file-name').textContent = file ? `${file.name} · ${formatBytes(file.size)}` : '';
  $('#selected-file-progress').textContent = progress === null ? '' : `${Math.round(progress * 100)}%`;
}

  return {declaredMediaType,uploadFile,showSelectedFile};
}
