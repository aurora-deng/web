import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=dc39cdb5489c964d';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=dc39cdb5489c964d';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=dc39cdb5489c964d';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=dc39cdb5489c964d';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=dc39cdb5489c964d';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=dc39cdb5489c964d';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=dc39cdb5489c964d';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=dc39cdb5489c964d';
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
