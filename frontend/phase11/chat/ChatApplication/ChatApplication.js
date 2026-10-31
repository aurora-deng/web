import { createChatScope, assertChatAssembly } from '../ChatScope/ChatScope.js?v=dc39cdb5489c964d';
import { createChatContext } from '../ChatContext/ChatContext.js?v=dc39cdb5489c964d';
import { bindChat } from '../ChatBindings/ChatBindings.js?v=dc39cdb5489c964d';
import { createFeedbackController, dependencies as createFeedbackControllerDependencies } from '../FeedbackController/FeedbackController.js?v=dc39cdb5489c964d';
import { createConversationController, dependencies as createConversationControllerDependencies } from '../ConversationController/ConversationController.js?v=dc39cdb5489c964d';
import { createGroupController, dependencies as createGroupControllerDependencies } from '../GroupController/GroupController.js?v=dc39cdb5489c964d';
import { createAttachmentController, dependencies as createAttachmentControllerDependencies } from '../../files/AttachmentController/AttachmentController.js?v=dc39cdb5489c964d';
import { createMessageController, dependencies as createMessageControllerDependencies } from '../MessageController/MessageController.js?v=dc39cdb5489c964d';
import { createRealtimeController, dependencies as createRealtimeControllerDependencies } from '../RealtimeController/RealtimeController.js?v=dc39cdb5489c964d';
import { createContactsController, dependencies as createContactsControllerDependencies } from '../../contacts/ContactsController/ContactsController.js?v=dc39cdb5489c964d';
import { createAiController, dependencies as createAiControllerDependencies } from '../../ai/AiController/AiController.js?v=dc39cdb5489c964d';
import { createKnowledgeController, dependencies as createKnowledgeControllerDependencies } from '../../knowledge/KnowledgeController/KnowledgeController.js?v=dc39cdb5489c964d';
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
  bindChat(context);
  const dispose=()=>{
    if(active?.context!==context)return;
    context.state.leaving=true;context.runtime.lifecycle.abort();
    actions.showLoggedOut();context.history.reset();context.sender.clear();context.messageView.dispose();
    context.state.search.controller?.abort();active=null;
  };
  active={context,dispose};window.addEventListener('pagehide',dispose,{once:true});return active;
}
