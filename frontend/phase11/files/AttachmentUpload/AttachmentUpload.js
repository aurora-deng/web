import { filesApi } from '../FilesApi/FilesApi.js?v=1f08ad808b467825';

export function declaredMediaType(file) {
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

export async function uploadAttachment(file, { request, csrf, chunkSize, onProgress = () => {} }) {
  if (!Number.isSafeInteger(chunkSize) || chunkSize <= 0) throw new Error('服务器上传规则未就绪。');
  const created = await request(filesApi.postFilesUploads({}), {
    method: 'POST',
    body: JSON.stringify({ name: file.name, mediaType: declaredMediaType(file), size: String(file.size) }),
  });
  let offset = Number(created.uploadedSize || 0);
  while (offset < file.size) {
    const chunk = file.slice(offset, Math.min(offset + chunkSize, file.size));
    let response;
    try {
      response = await fetch(filesApi.patchFilesFileContent({ file: created.id }).path, {
        method: 'PATCH',
        credentials: 'same-origin',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-CSRF-Token': csrf,
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
    if (!Number.isSafeInteger(next) || next <= offset || next > file.size)
      throw new Error('服务器返回了无效的上传游标');
    offset = next;
    onProgress(offset / file.size);
  }
  return request(filesApi.postFilesFileComplete({ file: created.id }), { method: 'POST' });
}
