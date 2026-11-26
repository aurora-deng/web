import { createChatScope, assertChatAssembly } from '../ChatScope/ChatScope.js?v=0613976363093f95';
import { createChatContext } from '../ChatContext/ChatContext.js?v=0613976363093f95';
import { bindChat } from '../ChatBindings/ChatBindings.js?v=0613976363093f95';
import { createFeedbackController, dependencies as createFeedbackControllerDependencies } from '../FeedbackController/FeedbackController.js?v=0613976363093f95';
import { createConversationController, dependencies as createConversationControllerDependencies } from '../ConversationController/ConversationController.js?v=0613976363093f95';
import { createGroupController, dependencies as createGroupControllerDependencies } from '../GroupController/GroupController.js?v=0613976363093f95';
import { createAttachmentController, dependencies as createAttachmentControllerDependencies } from '../../files/AttachmentController/AttachmentController.js?v=0613976363093f95';
import { createMessageController, dependencies as createMessageControllerDependencies } from '../MessageController/MessageController.js?v=0613976363093f95';
import { createRealtimeController, dependencies as createRealtimeControllerDependencies } from '../RealtimeController/RealtimeController.js?v=0613976363093f95';
import { createContactsController, dependencies as createContactsControllerDependencies } from '../../contacts/ContactsController/ContactsController.js?v=0613976363093f95';
import { createAiController, dependencies as createAiControllerDependencies } from '../../ai/AiController/AiController.js?v=0613976363093f95';
import { createKnowledgeController, dependencies as createKnowledgeControllerDependencies } from '../../knowledge/KnowledgeController/KnowledgeController.js?v=0613976363093f95';
import { watchRecallExpiry } from '../MessageRecall/MessageRecall.js?v=0613976363093f95';
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
    actions.stopChatSession();context.messageView.dispose();
    context.state.search.controller?.abort();active=null;
  };
  active={context,dispose};window.addEventListener('pagehide',dispose,{once:true});return active;
}
