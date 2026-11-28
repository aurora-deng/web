import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=1f08ad808b467825';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=1f08ad808b467825';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=1f08ad808b467825';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=1f08ad808b467825';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=1f08ad808b467825';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=1f08ad808b467825';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=1f08ad808b467825';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=1f08ad808b467825';
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
