/** Cookie 仍由浏览器保管；这里只缓存当前页面的 CSRF，不持久化密码和 Session。 */
export class BusinessApi {
  constructor() { this.csrf = ''; this.user = null; }
  async restore() { const data = await this.request('/api/me'); this.csrf = data.csrfToken; this.user = data.user; return this.user; }
  async request(path, { method = 'GET', data, signal, headers = {} } = {}) {
    if (data !== undefined) headers['Content-Type'] = 'application/json';
    if (!['GET', 'HEAD'].includes(method)) headers['X-CSRF-Token'] = this.csrf;
    const response = await fetch(path, { method, credentials: 'same-origin', cache: 'no-store', signal, headers,
      body: data === undefined ? undefined : JSON.stringify(data) });
    const result = await response.json().catch(() => ({}));
    if (!response.ok) { const error = new Error(result.message || `请求失败 (${response.status})`); error.status = response.status; throw error; }
    return result;
  }
}
