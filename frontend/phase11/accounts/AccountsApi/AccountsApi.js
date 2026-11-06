// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=144f1e6009a3d461";
export const accountsApi=defineEndpoints("accounts",[["postAuthLogin","POST","/api/auth/login"],["postAuthLogout","POST","/api/auth/logout"],["postAuthRegister","POST","/api/auth/register"],["getMe","GET","/api/me"],["patchMe","PATCH","/api/me"],["postMeAvatar","POST","/api/me/avatar"]]);
