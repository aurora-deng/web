import { bindAccount } from '../AccountBindings/AccountBindings.js?v=227af12a6c379d08';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=227af12a6c379d08';
import { bindAi } from '../AiBindings/AiBindings.js?v=227af12a6c379d08';
import { bindContacts } from '../ContactsBindings/ContactsBindings.js?v=227af12a6c379d08';
import { bindKnowledge } from '../KnowledgeBindings/KnowledgeBindings.js?v=227af12a6c379d08';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=227af12a6c379d08';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=227af12a6c379d08';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=227af12a6c379d08';
/** Subscription composition only. Domain handlers live beside their ports. */
export function bindChat(context) {
  bindAccount(context);
  bindComposer(context);
  bindAi(context);
  bindContacts(context);
  bindKnowledge(context);
  bindConversation(context);
  bindGroup(context);
  bindDialog(context);
}
