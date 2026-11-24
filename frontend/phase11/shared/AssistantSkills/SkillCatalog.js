export const skills = Object.freeze([
  Object.freeze({
    id: 'kaoyan-week-plan', title: '考研周计划', purpose: '把目标和空闲时间拆成一周可执行的任务。',
    placeholder: '例如：准备 408 和英语二，工作日每天 2 小时，周末 4 小时，先补计网。',
    study: true, instructions: '制定七天计划。先列本周目标，再按天列至多三件事、预计分钟和完成标准。预留补漏与错题复盘时间。资料不足时标注假设，禁止编造分数、考试日期和已完成任务。',
  }),
  Object.freeze({
    id: 'kaoyan-review', title: '错题复盘', purpose: '把错因、重做步骤和下次自测写成复盘清单。',
    placeholder: '例如：我把 TCP 的拥塞窗口和接收窗口混了，做题时把两个限制直接相加。',
    study: true, instructions: '根据输入整理「错因、正确思路、重做步骤、两道自测题、复习时间」。区分本人给出的事实与待验证猜测。题干不完整时说明缺失信息。',
  }),
  Object.freeze({
    id: 'chat-draft', title: '聊天起草', purpose: '根据你给的场景和语气写一条可编辑消息。',
    placeholder: '例如：礼貌告诉同组同学，我周五晚可以帮忙过一下演示流程。',
    destination: 'chat', instructions: '写一条可以直接放进聊天框的中文正文，保持用户指定语气。只输出消息正文，避免擅自承诺、添加联系人、猜测收件人或编造事实。',
  }),
  Object.freeze({
    id: 'plaza-draft', title: '广场动态', purpose: '起草一段发到缘生-广场的个人记录。',
    placeholder: '例如：写一段今天终于搞懂 epoll LT 和 ET 的学习动态，轻松一点。',
    destination: 'plaza', instructions: '用第一人称写一段适合缘生-广场的动态，只输出正文。把用户给出的经历写具体，避免编造经历、披露联系方式或私人信息。禁止声称已经发布。',
  }),
]);

export const tools = Object.freeze({
  study: 'study.read_progress',
  chat: 'chat.list_conversations',
  plaza: 'plaza.list_categories',
});

export const draftLimits = Object.freeze({ chat: 16384, plaza: 2000 });

export function decimalId(value) {
  if ((typeof value !== 'string' && !Number.isSafeInteger(value)) || !/^[1-9][0-9]{0,19}$/.test(String(value))) return '';
  return String(value);
}

export function skillById(id) {
  return skills.find(skill => skill.id === id);
}

export function skillMessages(skill, brief, progress) {
  if (skillById(skill?.id) !== skill || typeof brief !== 'string' || !brief.trim() || brief.length > 1500) {
    throw new Error('请选择技能，并输入 1 至 1500 字的需求。');
  }
  const stats = skill.study && progress
    ? '\n已授权的研途统计：' + JSON.stringify(safeStudyProgress(progress)) : '';
  return [
    { role: 'system', content: '你是 NEBULA 小研。只返回供用户审核的文本，不执行写入或外部操作。' + skill.instructions },
    { role: 'user', content: brief.trim() + stats },
  ];
}

export function safeStudyProgress(value) {
  const counts = ['todayPlanned', 'todayCompleted', 'overdue', 'nextSevenDays'];
  if (!value || typeof value.day !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value.day)
      || counts.some(key => !Number.isSafeInteger(value[key]) || value[key] < 0)
      || value.todayCompleted > value.todayPlanned) throw new Error('研途统计格式无效。');
  return { day: value.day, ...Object.fromEntries(counts.map(key => [key, value[key]])) };
}
