import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=7f8c7f1b31e4435c';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=7f8c7f1b31e4435c';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=7f8c7f1b31e4435c';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=7f8c7f1b31e4435c';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=7f8c7f1b31e4435c';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=7f8c7f1b31e4435c';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=7f8c7f1b31e4435c';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=7f8c7f1b31e4435c';
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
