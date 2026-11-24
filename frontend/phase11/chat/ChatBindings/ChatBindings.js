import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=9fcaa747c305a8cc';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=9fcaa747c305a8cc';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=9fcaa747c305a8cc';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=9fcaa747c305a8cc';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=9fcaa747c305a8cc';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=9fcaa747c305a8cc';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=9fcaa747c305a8cc';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=9fcaa747c305a8cc';
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
