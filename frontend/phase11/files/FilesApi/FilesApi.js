// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=0613976363093f95";
export const filesApi=defineEndpoints("files",[["postConversationsConversationFileMessages","POST","/api/conversations/:conversation/file-messages"],["getFilesFile","GET","/api/files/:file"],["postFilesFileComplete","POST","/api/files/:file/complete"],["patchFilesFileContent","PATCH","/api/files/:file/content"],["postFilesUploads","POST","/api/files/uploads"]]);
