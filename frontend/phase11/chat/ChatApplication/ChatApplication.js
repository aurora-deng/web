import { createChatScope, assertChatAssembly } from '../ChatScope/ChatScope.js?v=144f1e6009a3d461';
import { createChatContext } from '../ChatContext/ChatContext.js?v=144f1e6009a3d461';
import { bindChat } from '../ChatBindings/ChatBindings.js?v=144f1e6009a3d461';
import { createFeedbackController, dependencies as createFeedbackControllerDependencies } from '../FeedbackController/FeedbackController.js?v=144f1e6009a3d461';
import { createConversationController, dependencies as createConversationControllerDependencies } from '../ConversationController/ConversationController.js?v=144f1e6009a3d461';
import { createGroupController, dependencies as createGroupControllerDependencies } from '../GroupController/GroupController.js?v=144f1e6009a3d461';
import { createAttachmentController, dependencies as createAttachmentControllerDependencies } from '../../files/AttachmentController/AttachmentController.js?v=144f1e6009a3d461';
import { createMessageController, dependencies as createMessageControllerDependencies } from '../MessageController/MessageController.js?v=144f1e6009a3d461';
import { createRealtimeController, dependencies as createRealtimeControllerDependencies } from '../RealtimeController/RealtimeController.js?v=144f1e6009a3d461';
import { createContactsController, dependencies as createContactsControllerDependencies } from '../../contacts/ContactsController/ContactsController.js?v=144f1e6009a3d461';
import { createAiController, dependencies as createAiControllerDependencies } from '../../ai/AiController/AiController.js?v=144f1e6009a3d461';
import { createKnowledgeController, dependencies as createKnowledgeControllerDependencies } from '../../knowledge/KnowledgeController/KnowledgeController.js?v=144f1e6009a3d461';
import { watchRecallExpiry } from '../MessageRecall/MessageRecall.js?v=144f1e6009a3d461';
let active;
/** Composition only: domain controllers own behavior, context owns mutable resources. */
export function startChatApplication() {
  if(active)return active;
  const actions={},context=createChatContext(actions);
  Object.assign(actions,createFeedbackController(createChatScope(context,createFeedbackControllerDependencies)));
  Object.assign(actions,createConversationController(createChatScope(context,createConversationControllerDependencies)));
  Object.assign(actions,createGroupController(createChatScope(context,createGroupControllerDependencies)));
  Object.assign(actions,createAttachmentController(createChatScope(context,createAttachmentControllerDependencies)));
  Object.assign(actions,createMessageController(createChatScope(context,createMessageControllerDependencies)));
  Object.assign(actions,createRealtimeController(createChatScope(context,createRealtimeControllerDependencies)));
  Object.assign(actions,createContactsController(createChatScope(context,createContactsControllerDependencies)));
  Object.assign(actions,createAiController(createChatScope(context,createAiControllerDependencies)));
  Object.assign(actions,createKnowledgeController(createChatScope(context,createKnowledgeControllerDependencies)));
  assertChatAssembly(context,[
    createFeedbackControllerDependencies,createConversationControllerDependencies,
    createGroupControllerDependencies,createAttachmentControllerDependencies,
    createMessageControllerDependencies,createRealtimeControllerDependencies,
    createContactsControllerDependencies,createAiControllerDependencies,createKnowledgeControllerDependencies,
  ]);
  context.types.mountNavigation('chat');
  watchRecallExpiry(context.elements.messageList,{signal:context.runtime.lifecycle.signal});
  bindChat(context);
  const dispose=()=>{
    if(active?.context!==context)return;
    context.state.leaving=true;context.runtime.lifecycle.abort();
    actions.showLoggedOut();context.history.reset();context.sender.clear();context.messageView.dispose();
    context.state.search.controller?.abort();active=null;
  };
  active={context,dispose};window.addEventListener('pagehide',dispose,{once:true});return active;
}
