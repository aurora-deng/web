import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=4ec58ab3c1e38adb';
import { UserPicker } from '../UserPicker/UserPicker.js?v=4ec58ab3c1e38adb';
import { groupMessages } from '../MessageGrouping/MessageGrouping.js?v=4ec58ab3c1e38adb';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=4ec58ab3c1e38adb';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=4ec58ab3c1e38adb';
import { UnreadWatermarks } from '../UnreadWatermarks/UnreadWatermarks.js?v=4ec58ab3c1e38adb';
import { PreferenceSnapshots } from '../PreferenceSnapshots/PreferenceSnapshots.js?v=4ec58ab3c1e38adb';
import { ChatHistory, canRecallMessage } from '../ChatHistory.js?v=4ec58ab3c1e38adb';
import { ReliableSender } from '../ReliableSender.js?v=4ec58ab3c1e38adb';
import { VirtualMessageList } from '../VirtualMessageList/VirtualMessageList.js?v=4ec58ab3c1e38adb';
import { RecoveryBudget, ReconnectBackoff } from '../../shared/RecoveryBudget/RecoveryBudget.js?v=4ec58ab3c1e38adb';
import { formatChatTime, formatDayKey, formatDateDivider } from '../../shared/TimeFormat/TimeFormat.js?v=4ec58ab3c1e38adb';
/** Single owner of account state, queues and views. Controllers share references, not copies. */
export function createChatContext(actions) {
  const runtime={sseTimer:null,suppressMessageRender:false,announcementPlayer:null,lifecycle:new AbortController(),accountAbort:new AbortController()};
  const savePendingMessages = (...args) => actions.savePendingMessages(...args);
  const verifyOwnMessage = (...args) => actions.verifyOwnMessage(...args);
  const settleHistoryPending = (...args) => actions.settleHistoryPending(...args);
  const scheduleCatchup = (...args) => actions.scheduleCatchup(...args);
  const recoverRealtime = (...args) => actions.recoverRealtime(...args);
  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const initials = (...args) => actions.initials(...args);
  const createClientId = (...args) => actions.createClientId(...args);
  const rememberMessageId = (...args) => actions.rememberMessageId(...args);
  const advanceConversationSequence = (...args) => actions.advanceConversationSequence(...args);
  const conversationTitle = (...args) => actions.conversationTitle(...args);
  const conversationOrder = (...args) => actions.conversationOrder(...args);
  const bumpUnread = (...args) => actions.bumpUnread(...args);
  const myMembership = (...args) => actions.myMembership(...args);
  const clearConversationView = (...args) => actions.clearConversationView(...args);
  const setButtonBusy = (...args) => actions.setButtonBusy(...args);
  const api = (...args) => actions.api(...args);
  const declaredMediaType = (...args) => actions.declaredMediaType(...args);
  const uploadFile = (...args) => actions.uploadFile(...args);
  const formatBytes = (...args) => actions.formatBytes(...args);
  const withDateDividers = (...args) => actions.withDateDividers(...args);
  const paintAvatar = (...args) => actions.paintAvatar(...args);
  const updateProfileView = (...args) => actions.updateProfileView(...args);
  const setRealtimeState = (...args) => actions.setRealtimeState(...args);
  const showChat = (...args) => actions.showChat(...args);
  const showLoggedOut = (...args) => actions.showLoggedOut(...args);
  const renderConversations = (...args) => actions.renderConversations(...args);
  const loadConversations = (...args) => actions.loadConversations(...args);
  const paintConversationChrome = (...args) => actions.paintConversationChrome(...args);
  const openConversation = (...args) => actions.openConversation(...args);
  const renderConversationMembers = (...args) => actions.renderConversationMembers(...args);
  const renderGroupGovernance = (...args) => actions.renderGroupGovernance(...args);
  const refreshCurrentGroup = (...args) => actions.refreshCurrentGroup(...args);
  const updateMemberRole = (...args) => actions.updateMemberRole(...args);
  const updateMemberMute = (...args) => actions.updateMemberMute(...args);
  const removeMember = (...args) => actions.removeMember(...args);
  const deliveryLabel = (...args) => actions.deliveryLabel(...args);
  const canRecall = (...args) => actions.canRecall(...args);
  const requestRecall = (...args) => actions.requestRecall(...args);
  const createMessageElement = (...args) => actions.createMessageElement(...args);
  const renderCurrentMessages = (...args) => actions.renderCurrentMessages(...args);
  const scrollMessages = (...args) => actions.scrollMessages(...args);
  const openCitationDetails = (...args) => actions.openCitationDetails(...args);
  const previousPromptForMessage = (...args) => actions.previousPromptForMessage(...args);
  const openTrainingConsent = (...args) => actions.openTrainingConsent(...args);
  const renderTrainingCandidates = (...args) => actions.renderTrainingCandidates(...args);
  const loadTrainingCandidates = (...args) => actions.loadTrainingCandidates(...args);
  const existingMessage = (...args) => actions.existingMessage(...args);
  const clearSearchHit = (...args) => actions.clearSearchHit(...args);
  const ensureSequenceLoaded = (...args) => actions.ensureSequenceLoaded(...args);
  const revealMessageRow = (...args) => actions.revealMessageRow(...args);
  const jumpToMessage = (...args) => actions.jumpToMessage(...args);
  const messageByClientId = (...args) => actions.messageByClientId(...args);
  const refreshMessageReceipts = (...args) => actions.refreshMessageReceipts(...args);
  const generationElement = (...args) => actions.generationElement(...args);
  const appendMessage = (...args) => actions.appendMessage(...args);
  const acceptPendingMessage = (...args) => actions.acceptPendingMessage(...args);
  const applyRecall = (...args) => actions.applyRecall(...args);
  const failPendingMessage = (...args) => actions.failPendingMessage(...args);
  const sendSocket = (...args) => actions.sendSocket(...args);
  const acknowledge = (...args) => actions.acknowledge(...args);
  const selectAiTarget = (...args) => actions.selectAiTarget(...args);
  const applyAiModels = (...args) => actions.applyAiModels(...args);
  const loadAiModels = (...args) => actions.loadAiModels(...args);
  const readAiTarget = (...args) => actions.readAiTarget(...args);
  const refreshCancelButton = (...args) => actions.refreshCancelButton(...args);
  const onRealtimeMessage = (...args) => actions.onRealtimeMessage(...args);
  const connectRealtime = (...args) => actions.connectRealtime(...args);
  const connectSocket = (...args) => actions.connectSocket(...args);
  const connectEvents = (...args) => actions.connectEvents(...args);
  const refreshPendingBadge = (...args) => actions.refreshPendingBadge(...args);
  const actionListEmpty = (...args) => actions.actionListEmpty(...args);
  const contactName = (...args) => actions.contactName(...args);
  const contactPresence = (...args) => actions.contactPresence(...args);
  const saveContact = (...args) => actions.saveContact(...args);
  const contactRow = (...args) => actions.contactRow(...args);
  const sidebarFriendRow = (...args) => actions.sidebarFriendRow(...args);
  const blockedRow = (...args) => actions.blockedRow(...args);
  const renderContactLists = (...args) => actions.renderContactLists(...args);
  const renderFriendPreview = (...args) => actions.renderFriendPreview(...args);
  const loadFriendsPreview = (...args) => actions.loadFriendsPreview(...args);
  const openFriendConversation = (...args) => actions.openFriendConversation(...args);
  const friendRequestRow = (...args) => actions.friendRequestRow(...args);
  const loadContacts = (...args) => actions.loadContacts(...args);
  const createDirectConversation = (...args) => actions.createDirectConversation(...args);
  const openConversationDialog = (...args) => actions.openConversationDialog(...args);
  const showSelectedFile = (...args) => actions.showSelectedFile(...args);
  const knowledgeScope = (...args) => actions.knowledgeScope(...args);
  const renderKnowledgeDocuments = (...args) => actions.renderKnowledgeDocuments(...args);
  const loadKnowledgeDocuments = (...args) => actions.loadKnowledgeDocuments(...args);
  const updateKnowledgeScopeHint = (...args) => actions.updateKnowledgeScopeHint(...args);
  const startAnnouncements = (...args) => actions.startAnnouncements(...args);
  const loadWorkspaceCapabilities = (...args) => actions.loadWorkspaceCapabilities(...args);
  const searchWorkspace = (...args) => actions.searchWorkspace(...args);

// 新增：全站唯一时间格式化（会话列表 / 消息流 / 搜索结果共用同一实现）

const businessApi = new BusinessApi();
const userPickers = [];
const unreadWatermarks = new UnreadWatermarks();
const pageBudget = new RecoveryBudget();
const wsBackoff = new ReconnectBackoff();
const sseBackoff = new ReconnectBackoff();


const $ = selector => document.querySelector(selector);

const elements = {
  authPanel: $('#auth-panel'),
  chatPanel: $('#chat-panel'),
  authStatus: $('#auth-status'),
  chatStatus: $('#chat-status'),
  conversationList: $('#conversation-list'),
  sidebarFriendList: $('#sidebar-friend-list'),
  conversationEmpty: $('#conversation-empty'),
  conversationLoading: $('#conversation-loading'),
  messageList: $('#message-list'),
  messageForm: $('#message-form'),
  emptyConversation: $('#empty-conversation'),
  registerDialog: $('#register-dialog'),
  friendDialog: $('#friend-dialog'),
  conversationDialog: $('#conversation-dialog'),
  profileDialog: $('#profile-dialog'),
  searchDialog: $('#search-dialog'),
  memberDialog: $('#member-dialog'),
  groupProfileDialog: $('#group-profile-dialog'),
  knowledgeDialog: $('#knowledge-dialog'),
  citationDialog: $('#citation-dialog'),
  trainingDialog: $('#training-dialog'),
  modelDialog: $('#model-dialog'),
  detailPane: $('#detail-pane'),
};

const state = {
  preferenceSnapshots:new PreferenceSnapshots(),
  me: null,
  capabilities: null,
  search: { version:0, cursor:'0', query:'', conversation:'0' },
  csrf: '',
  conversation: null,
  conversations: [],
  friends: [],
  blocked: [],
  socket: null,
  events: null,
  reconnectTimer: null,
  activeGenerations: new Map(),
  unread: new Map(),
  seenMessageIds: new Set(),
  pendingRecalls: new Map(),
  members: [],
  aiConversations: new Set(),
  conversationFilter: 'all',
  conversationQuery: '',
  selectedFile: null,
  uploadingFile: false,
  realtime: { ws: 'offline', sse: 'offline' },
  leaving: false,
  viewVersion: 0,
  listVersion: 0,
  catchupTimer: null,
  recovery: null,
  recoverRequested: false,
  transientMessages: new Map(),
  searchHit: null,
  bumped: null,               // 最近一次有新消息的会话，用于一次性上浮动效
  aiModels: null,             // null = 还没读过；[] = 读过但为空或失败
  aiModelError: '',           // 模型列表读取失败的原因（用于下拉里的兜底选项文案）
  conversationAiMember: 0,    // 当前会话自动识别到的 AI 成员 userId，0 = 没有
  aiTargetManual: false,      // 用户是否手选过模型：手选优先级高于自动识别，且不跨会话保留
};

// 跳转定位期间要连续翻十几页历史，逐页重绘会让列表抖动；
// 只在这一段屏蔽重绘，定位结束后统一渲染一次。


const messageView = new VirtualMessageList(elements.messageList, createMessageElement);
const history = new ChatHistory((conversationId, query, signal) => pageBudget.run(() => api(
  `/api/conversations/${conversationId}/messages?${new URLSearchParams(query)}`, { signal },
), signal), () => renderCurrentMessages());



const sender = new ReliableSender({
  send: payload => Boolean(state.me && !state.leaving) && sendSocket(payload),
  verify: verifyOwnMessage,
  changed: () => { savePendingMessages(); renderCurrentMessages(); },
  persisted: (entry, message) => {
    savePendingMessages();
    if (!state.me) return;
    const cached = history.records.get(String(message.id));
    const full = !message.createdAt && cached ? cached : { ...message, senderId: state.me.id };
    appendMessage(full);
    advanceConversationSequence(message.conversationId, message.sequence);
    renderConversations();
    if (Number(message.conversationId) === Number(state.conversation?.id) &&
        Number(message.sequence) > history.cursor) scheduleCatchup();
    // 精简 accepted 没有服务端时间，查询完整记录后才能正确判断撤回时间窗。
    if (!full.createdAt) verifyOwnMessage(entry.payload).then(found => {
      if (found && Number(found.senderId) === Number(state.me?.id)) appendMessage(found);
    }).catch(() => scheduleCatchup());
  },
});



/** 重连不仅取新消息，也重查可见窗口，恢复断线期间发生的撤回和权限变化。 */




/**
 * 为聊天消息生成客户端幂等 ID。
 *
 * crypto.randomUUID() 只保证出现在安全上下文（HTTPS/localhost）里；开发环境从
 * http://<虚拟机IP> 打开时部分浏览器没有这个函数。getRandomValues 在这种页面仍可用，
 * 因此手工组装 RFC 4122 v4 UUID，并保留极旧浏览器的时间戳兜底。
 */

/**
 * 记录已经处理过的持久化消息，阻止 WebSocket 与 SSE 的同一事件重复渲染。
 * Set 像一个临时“验票册”，只保留最近 4096 张票，避免页面长期打开后无限占用内存。
 * 返回 true 表示该消息此前已经处理过。
 */



/**
 * 会话列表的全序：未读优先 → 最后消息时间倒序 → id 倒序。
 * 末位用 id 兜底是为了让「同一毫秒到达的两条消息」也有确定顺序，
 * 否则每次重绘都可能交换位置，用户会看到列表闪。
 */

/** 按已接受的序号推进；乱序/重复不会多加，已确认的已读不会倒退。 */




/** Shared status/cancellation/uncertain-write policy; never automatically replays writes. */




/**
 * 把消息序列映射成「带日期分隔标记的行」。
 * 不改 VirtualMessageList：日期标记作为行对象的附加字段随行一起渲染，
 * 因此 heights / prefix / range 的下标语义完全不变。
 * 只有需要分隔的行才创建新对象，其余行保持原引用，
 * 这样 VirtualMessageList 的 `JSON.stringify(message)` 签名缓存不会被无谓击穿。
 */








/**
 * 会话头部与详情栏的身份区。会话列表、头部、详情栏、输入区目的地四处必须同源，
 * 都走 conversationTitle() 与 paintAvatar()，否则备注/头像会在某一处漏掉。
 */












/** 统一重排历史、待确认命令和 AI 流式气泡；保留阅读锚点，加载旧页不跳回底部。 */





// 审核发生在服务器命令行工具里（tools/phase11_training_admin.cpp:141-144），
// 网页只负责提交与撤销。文案必须让用户知道「不是我这里出了问题」。
const trainingStateLabels = {
  submitted: '已提交 · 等待管理员在管理工具中离线审核',
  approved: '已通过审核',
  rejected: '未通过审核',
  exported: '已进入数据集',
  revoked: '已撤销',
};




/** 清除上一次的搜索高亮（含未到期的定时器），避免连续跳转留下多个高亮行。 */

/**
 * 把目标序号拉进当前历史窗口。
 * ChatHistory 只有「从 cursor 往后补」和「从 oldest 往前翻」两条路，
 * 目标在窗口之外时只能反复 older()；每轮 100 条，受 pageBudget 并发闸门约束。
 */

/**
 * 让目标行进入视口并高亮 2000ms。
 * .message-list 有 scroll-behavior:smooth，平滑滚动会让「先设位置再测量」全部失效，
 * 所以定位期间临时改成 auto，定位完成后恢复原值。
 */

/** 搜索结果 → 定位 → 关闭弹窗 → 高亮。失败时把原因留在搜索弹窗里，不清空结果。 */
/** Context lookup is bounded and re-authorized; it does not fetch dozens of intervening pages. */










/**
 * 把 AI 目标选中到指定账号；options 里没有就先补一个，避免 select.value 静默变空。
 * `<select>` 的 .value 赋一个不在 options 里的值不会报错，只会静默变成空串，
 * 症状是「AI 模式发不出消息」且提示文案对不上原因——所以必须显式补 option。
 */

/**
 * 用模型列表重建 options，并优先保住当前该有的选中项。
 * 优先级：用户手选 > 会话自动识别 > 第一个可用模型。
 * 不能拿「上一次会话遗留的 select.value」当默认值，否则会把上个会话的
 * `会话 AI 成员 #N` 带进一个没有 AI 成员的会话里，用户看到的是错的。
 * 也绝不在「有明确目标」时静默换成第一个模型。
 */

/** 只在第一次打开 AI 模式时打一次接口；失败不阻塞手填兜底。 */

/** 手填的「高级」输入优先；否则用下拉值。 */























  return {actions,businessApi,userPickers,unreadWatermarks,pageBudget,wsBackoff,sseBackoff,$,elements,state,messageView,history,sender,trainingStateLabels,runtime,types:{mountNavigation,UserPicker,groupMessages,BusinessApi,AnnouncementPlayer,UnreadWatermarks,ChatHistory,canRecallMessage,ReliableSender,VirtualMessageList,RecoveryBudget,ReconnectBackoff,formatChatTime,formatDayKey,formatDateDivider}};
}
