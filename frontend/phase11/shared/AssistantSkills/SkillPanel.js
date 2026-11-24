import { decimalId, draftLimits, skillById, skillMessages, skills, tools } from './SkillCatalog.js?v=1b40f8c19ed29802';
import { readSkillTool, requireSkillOwner } from './SkillTools.js?v=1b40f8c19ed29802';
import { stageSkillDraft } from './DraftHandoff.js?v=1b40f8c19ed29802';

export class SkillPanel {
  constructor({ api, startInference }) {
    this.api = api;
    this.startInference = startInference;
    this.lifecycle = new AbortController();
    this.host = document.createElement('details');
    this.host.className = 'assistant-skills';
    this.host.innerHTML = `
      <summary>小研技能</summary>
      <div class="assistant-skill-fields">
        <label>用途<select data-skill></select></label>
        <p data-purpose></p>
        <label>本次需求<textarea data-brief rows="3" maxlength="1500"></textarea></label>
        <label data-study hidden><input type="checkbox" data-include-study>参考研途任务数量</label>
        <small>所填需求和勾选的统计会发送给当前模型。仅使用本次输入。</small>
        <div class="assistant-skill-controls">
          <button type="button" data-run-skill>生成草稿</button>
          <button type="button" data-stop-skill disabled>停止</button>
        </div>
        <p role="status" data-skill-status></p>
        <div data-skill-result hidden>
          <label>编辑草稿<textarea data-skill-draft rows="6"></textarea></label>
          <div class="assistant-skill-controls">
            <button type="button" data-copy-skill>复制草稿</button>
            <a data-open-study href="/phase11/study/">打开研途</a>
          </div>
          <div data-target-box hidden>
            <label><span data-target-label></span><select data-skill-target disabled></select></label>
            <div class="assistant-skill-controls">
              <button type="button" data-read-targets>读取选项</button>
              <button type="button" data-handoff disabled>带入编辑框</button>
            </div>
          </div>
        </div>
      </div>`;
    this.find = key => this.host.querySelector(`[data-${key}]`);
    this.selection = this.find('skill');
    this.brief = this.find('brief');
    this.draft = this.find('skill-draft');
    this.target = this.find('skill-target');
    this.status = this.find('skill-status');
    for (const skill of skills) this.selection.append(new Option(skill.title, skill.id));
    this.selection.onchange = () => this.updateSelection();
    this.find('run-skill').onclick = () => this.run();
    this.find('stop-skill').onclick = () => this.stop();
    this.find('read-targets').onclick = () => this.loadTargets();
    this.find('copy-skill').onclick = () => this.copy();
    this.find('handoff').onclick = () => this.handoff();
    this.target.onchange = () => this.updateButtons();
    this.updateSelection();
  }

  get skill() { return skillById(this.selection.value); }
  get busy() { return Boolean(this.controller); }

  setOwner(value) {
    const owner = decimalId(value);
    if (this.owner && this.owner !== owner) {
      this.controller?.abort();
      this.brief.value = '';
      this.updateSelection();
    }
    this.owner = owner;
    this.updateButtons();
  }

  updateSelection() {
    const skill = this.skill;
    this.targetRead?.abort();
    this.find('purpose').textContent = skill.purpose;
    this.brief.placeholder = skill.placeholder;
    this.find('study').hidden = !skill.study;
    this.find('include-study').checked = false;
    this.find('skill-result').hidden = true;
    this.find('target-box').hidden = !skill.destination;
    this.find('open-study').hidden = !skill.study;
    this.draft.value = '';
    this.draft.maxLength = draftLimits[skill.destination] || 6000;
    this.status.textContent = '';
    this.setTargets([]);
    this.find('target-label').textContent = skill.destination === 'chat' ? '发送到哪个会话' : '广场主题分类';
    this.find('handoff').textContent = skill.destination === 'chat' ? '带入聊天框' : '打开发帖编辑器';
    this.updateButtons();
  }

  updateButtons() {
    const busy = this.busy || this.opening;
    this.find('run-skill').disabled = !this.owner || busy;
    this.find('stop-skill').disabled = !this.busy;
    this.selection.disabled = busy;
    this.brief.disabled = busy;
    this.find('include-study').disabled = busy;
    this.draft.disabled = busy;
    this.target.disabled = busy || !this.choices.length;
    this.find('read-targets').disabled = busy || Boolean(this.targetRead);
    this.find('handoff').disabled = busy || !this.choices.some(choice => choice.id === this.target.value);
  }

  setTargets(choices) {
    this.choices = choices;
    this.target.replaceChildren(new Option('请选择', ''), ...choices.map(choice => new Option(choice.label, choice.id)));
  }

  async run() {
    if (!this.owner || this.busy || this.opening) return;
    const skill = this.skill;
    const brief = this.brief.value;
    if (!brief.trim()) { this.status.textContent = '请填写这次要处理的内容。'; this.brief.focus(); return; }
    this.targetRead?.abort();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 120000);
    const signal = controller.signal;
    this.controller = controller;
    this.updateButtons();
    this.status.textContent = '正在起草…';
    try {
      const predict = await this.startInference();
      await requireSkillOwner(this.api, this.owner, signal);
      const progress = skill.study && this.find('include-study').checked
        ? await readSkillTool(tools.study, { api: this.api, owner: this.owner, signal }) : null;
      signal.throwIfAborted();
      const answer = (await predict(skillMessages(skill, brief, progress), { signal, maxTokens: 320 })).trim();
      await requireSkillOwner(this.api, this.owner, signal);
      signal.throwIfAborted();
      if (!answer) throw new Error('模型没有返回草稿。');
      this.draft.value = answer;
      this.find('skill-result').hidden = false;
      this.status.textContent = answer.length > this.draft.maxLength ? '草稿超长。请先编辑至页面允许的长度。' : '草稿已生成，可以编辑。';
    } catch (error) {
      if (!this.lifecycle.signal.aborted) this.status.textContent = signal.aborted ? '生成已停止。' : error.message;
    } finally {
      clearTimeout(timer);
      this.controller = null;
      this.updateButtons();
    }
  }

  async loadTargets() {
    if (!this.skill.destination || this.busy || this.opening || this.targetRead) return;
    const controller = new AbortController();
    const destination = this.skill.destination;
    this.targetRead = controller;
    this.updateButtons();
    try {
      const choices = await readSkillTool(tools[destination], {
        api: this.api, owner: this.owner, signal: controller.signal,
      });
      controller.signal.throwIfAborted();
      this.setTargets(choices);
      this.status.textContent = this.choices.length ? '请选择目标。' : '暂无可选目标。';
    } catch (error) {
      if (!controller.signal.aborted) this.status.textContent = error.message;
    } finally {
      this.targetRead = null;
      this.updateButtons();
    }
  }

  async copy() {
    try {
      await navigator.clipboard.writeText(this.draft.value);
      this.status.textContent = '已复制草稿。';
    } catch {
      this.draft.select();
      this.status.textContent = '草稿已选中。按 Ctrl+C 复制。';
    }
  }

  async handoff() {
    if (this.busy || this.opening || !this.choices.some(choice => choice.id === this.target.value)) return;
    this.opening = true;
    this.updateButtons();
    const draft = { owner: this.owner, destination: this.skill.destination, target: this.target.value, body: this.draft.value };
    try {
      await requireSkillOwner(this.api, draft.owner, this.lifecycle.signal);
      this.lifecycle.signal.throwIfAborted();
      location.assign(stageSkillDraft(draft));
    } catch (error) {
      if (!this.lifecycle.signal.aborted) this.status.textContent = error.message;
    } finally {
      this.opening = false;
      this.updateButtons();
    }
  }

  stop() { this.controller?.abort(); }

  dispose() {
    this.lifecycle.abort();
    this.stop();
    this.targetRead?.abort();
  }
}
