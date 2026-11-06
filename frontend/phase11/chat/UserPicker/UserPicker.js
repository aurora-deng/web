import { contactsApi } from "../../contacts/ContactsApi/ContactsApi.js?v=144f1e6009a3d461";
/** Bounded public-directory picker. Selection writes the existing DTO field; server still authorizes it. */
export class UserPicker {
  constructor(input, request, { multiple = false } = {}) {
    Object.assign(this, { input, request, multiple });
    this.selected = new Map(); this.revision = 0;
    this.box = document.createElement('section'); this.box.className = 'user-picker';
    this.search = document.createElement('input'); this.search.type = 'search';
    this.search.placeholder = '搜索用户名或显示名称'; this.search.setAttribute('aria-label', '搜索用户');
    this.results = document.createElement('div'); this.results.className = 'user-picker-results';
    this.chips = document.createElement('div'); this.chips.className = 'user-picker-selected';
    this.status = document.createElement('small'); this.status.setAttribute('role', 'status');
    this.box.append(this.search, this.chips, this.results, this.status);
    input.closest('label').before(this.box);
    input.closest('label').classList.add('advanced-id-input');
    input.placeholder = '也可手填已知用户 ID';
    this.listener = () => { clearTimeout(this.timer); this.controller?.abort(); ++this.revision; this.timer = setTimeout(() => this.lookup(), 250); };
    this.search.addEventListener('input', this.listener);
    this.onReset = () => { ++this.revision; this.controller?.abort(); this.selected.clear(); this.search.value=''; this.results.replaceChildren(); this.chips.replaceChildren(); this.status.textContent=''; };
    input.form.addEventListener('reset', this.onReset);
  }
  async lookup() {
    const q = this.search.value.trim(), version = ++this.revision;
    this.controller?.abort(); this.controller = new AbortController();
    this.results.replaceChildren();
    if (!q) { this.status.textContent=''; return; }
    this.status.textContent='正在搜索…';
    try {
      const page = await this.request(contactsApi.getUsersSearch({}, String(new URLSearchParams({q, limit:20}))), {signal:this.controller.signal});
      if (version !== this.revision) return;
      for (const user of page.items) {
        const button=document.createElement('button'); button.type='button'; button.className='user-picker-option';
        button.textContent=`${user.displayName} @${user.username}${user.friendship ? ' · 好友' : user.pendingRequest ? ' · 申请待处理' : ''}`;
        button.onclick=()=>this.choose(user); this.results.append(button);
      }
      this.status.textContent=page.items.length ? (page.nextCursor!=='0' ? '仅显示前20位，请细化关键词' : '') : '没有匹配用户';
    } catch(error) { if(version===this.revision && error.name!=='AbortError') this.status.textContent=error.message; }
  }
  choose(user) {
    if(!this.multiple) this.selected.clear();
    this.selected.set(String(user.id),user); this.paint();
  }
  paint() {
    this.input.value=[...this.selected.keys()].join(',');
    this.input.dispatchEvent(new Event('input',{bubbles:true}));
    this.chips.replaceChildren(...[...this.selected.values()].map(user=>{
      const chip=document.createElement('button'); chip.type='button'; chip.className='mini-button';
      chip.textContent=`${user.displayName} ×`; chip.onclick=()=>{this.selected.delete(String(user.id));this.paint();}; return chip;
    }));
  }
  dispose() { ++this.revision; clearTimeout(this.timer); this.controller?.abort(); this.search.removeEventListener('input',this.listener); this.input.form.removeEventListener('reset',this.onReset); this.box.remove(); }
}
