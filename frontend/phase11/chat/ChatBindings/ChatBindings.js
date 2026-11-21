import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=4364119c78f60e44';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=4364119c78f60e44';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=4364119c78f60e44';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=4364119c78f60e44';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=4364119c78f60e44';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=4364119c78f60e44';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=4364119c78f60e44';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=4364119c78f60e44';
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
