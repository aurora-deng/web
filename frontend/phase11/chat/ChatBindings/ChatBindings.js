import { bindAccount } from '../../accounts/AccountBindings/AccountBindings.js?v=144f1e6009a3d461';
import { bindComposer } from '../ComposerBindings/ComposerBindings.js?v=144f1e6009a3d461';
import { bindAi } from '../../ai/AiBindings/AiBindings.js?v=144f1e6009a3d461';
import { bindContacts } from '../../contacts/ContactsBindings/ContactsBindings.js?v=144f1e6009a3d461';
import { bindKnowledge } from '../../knowledge/KnowledgeBindings/KnowledgeBindings.js?v=144f1e6009a3d461';
import { bindConversation } from '../ConversationBindings/ConversationBindings.js?v=144f1e6009a3d461';
import { bindGroup } from '../GroupBindings/GroupBindings.js?v=144f1e6009a3d461';
import { bindDialog } from '../DialogBindings/DialogBindings.js?v=144f1e6009a3d461';
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
