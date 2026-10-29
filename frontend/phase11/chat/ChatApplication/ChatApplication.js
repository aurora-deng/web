import { createChatContext } from '../ChatContext/ChatContext.js?v=4ec58ab3c1e38adb';
import { bindChat } from '../ChatBindings/ChatBindings.js?v=4ec58ab3c1e38adb';
import { createFeedbackController } from '../FeedbackController/FeedbackController.js?v=4ec58ab3c1e38adb';
import { createConversationController } from '../ConversationController/ConversationController.js?v=4ec58ab3c1e38adb';
import { createGroupController } from '../GroupController/GroupController.js?v=4ec58ab3c1e38adb';
import { createAttachmentController } from '../AttachmentController/AttachmentController.js?v=4ec58ab3c1e38adb';
import { createMessageController } from '../MessageController/MessageController.js?v=4ec58ab3c1e38adb';
import { createRealtimeController } from '../RealtimeController/RealtimeController.js?v=4ec58ab3c1e38adb';
import { createContactsController } from '../ContactsController/ContactsController.js?v=4ec58ab3c1e38adb';
import { createAiController } from '../AiController/AiController.js?v=4ec58ab3c1e38adb';
import { createKnowledgeController } from '../KnowledgeController/KnowledgeController.js?v=4ec58ab3c1e38adb';
let active;
/** Composition only: domain controllers own behavior, context owns mutable resources. */
export function startChatApplication() {
  if(active)return active;
  const actions={},context=createChatContext(actions);
  Object.assign(actions,createFeedbackController(context));
  Object.assign(actions,createConversationController(context));
  Object.assign(actions,createGroupController(context));
  Object.assign(actions,createAttachmentController(context));
  Object.assign(actions,createMessageController(context));
  Object.assign(actions,createRealtimeController(context));
  Object.assign(actions,createContactsController(context));
  Object.assign(actions,createAiController(context));
  Object.assign(actions,createKnowledgeController(context));
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
