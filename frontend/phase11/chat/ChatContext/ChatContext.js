import { chatApi } from "../ChatApi/ChatApi.js?v=dc39cdb5489c964d";
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=dc39cdb5489c964d';
import { UserPicker } from '../UserPicker/UserPicker.js?v=dc39cdb5489c964d';
import { groupMessages } from '../MessageGrouping/MessageGrouping.js?v=dc39cdb5489c964d';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=dc39cdb5489c964d';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=dc39cdb5489c964d';
import { UnreadWatermarks } from '../UnreadWatermarks/UnreadWatermarks.js?v=dc39cdb5489c964d';
import { PreferenceSnapshots } from '../PreferenceSnapshots/PreferenceSnapshots.js?v=dc39cdb5489c964d';
import { ChatHistory, canRecallMessage } from '../ChatHistory.js?v=dc39cdb5489c964d';
import { ReliableSender } from '../ReliableSender.js?v=dc39cdb5489c964d';
import { VirtualMessageList } from '../VirtualMessageList/VirtualMessageList.js?v=dc39cdb5489c964d';
import { RecoveryBudget, ReconnectBackoff } from '../../shared/RecoveryBudget/RecoveryBudget.js?v=dc39cdb5489c964d';
import { formatChatTime, formatDayKey, formatDateDivider } from '../../shared/TimeFormat/TimeFormat.js?v=dc39cdb5489c964d';

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
const history = new ChatHistory((conversationId, query, signal) => pageBudget.run(() => api(chatApi.getConversationsIdMessages({"id":(conversationId)}, String(new URLSearchParams(query))), { signal },
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

// 审核发生在服务器命令行工具里（tools/phase11_training_admin.cpp:141-144），
// 网页只负责提交与撤销。文案必须让用户知道「不是我这里出了问题」。
const trainingStateLabels = {
  submitted: '已提交 · 等待管理员在管理工具中离线审核',
  approved: '已通过审核',
  rejected: '未通过审核',
  exported: '已进入数据集',
  revoked: '已撤销',
};

  return {actions,businessApi,userPickers,unreadWatermarks,pageBudget,wsBackoff,sseBackoff,$,elements,state,messageView,history,sender,trainingStateLabels,runtime,types:{mountNavigation,UserPicker,groupMessages,BusinessApi,AnnouncementPlayer,UnreadWatermarks,ChatHistory,canRecallMessage,ReliableSender,VirtualMessageList,RecoveryBudget,ReconnectBackoff,formatChatTime,formatDayKey,formatDateDivider}};
}
