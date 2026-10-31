import { accountsApi } from "../../accounts/AccountsApi/AccountsApi.js?v=dc39cdb5489c964d";
import { normalizeApplicationRequest } from '../ApiContracts/ApiContracts.js?v=dc39cdb5489c964d';
/** Cookie 仍由浏览器保管；这里只缓存当前页面的 CSRF，不持久化密码和 Session。 */
export class BusinessApi {
  constructor({validateContracts=true}={}) { this.csrf = ''; this.user = null; this.validateContracts=validateContracts; }
  async restore() { const data = await this.request(accountsApi.getMe({})); this.csrf = data.csrfToken; this.user = data.user; return this.user; }
  async request(path, { method, data, body, signal, headers: supplied = {}, ...options } = {}) {
    // A descriptor owns its HTTP verb; a caller cannot silently turn a read into a write.
    if(this.validateContracts){const request=normalizeApplicationRequest(path,{method});path=request.path;method=request.method;}
    else method = (method||'GET').toUpperCase();
    const headers = new Headers(supplied);
    const writing = !['GET', 'HEAD'].includes(method);
    if (data !== undefined) body = JSON.stringify(data);
    if (body !== undefined && !headers.has('Content-Type')) headers.set('Content-Type', 'application/json');
    if (writing && this.csrf) headers.set('X-CSRF-Token', this.csrf);
    let response;
    try {
      response = await fetch(path, { ...options, method, credentials: 'same-origin', cache: 'no-store', signal, headers, body });
    } catch (cause) {
      if (cause.name === 'AbortError') throw cause;
      const error = new Error(writing ? '网络中断，操作结果尚未确认；请先查询结果，再决定重试' : '无法连接服务器，请检查网络或服务状态');
      error.uncertain = writing; error.cause = cause; throw error;
    }
    const text = await response.text();
    let result = null;
    try { result = text ? JSON.parse(text) : null; } catch { result = { message: text }; }
    if (!response.ok) {
      const fallback = {401:'登录已过期，请重新登录',403:'没有执行此操作的权限',409:'内容或状态冲突，请刷新后确认',429:'操作太频繁，请稍后重试'};
      const error = new Error(result?.message || result?.error || fallback[response.status] || `请求失败 (${response.status})`);
      error.status = response.status; error.retryAfter = response.headers.get('Retry-After');
      // HTTP failure is definitive; network failure above may have committed a write.
      error.uncertain = false; throw error;
    }
    return result;
  }
}
