// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=0613976363093f95";
export const webAiApi=defineEndpoints("webAi",[["postAiWebGenerations","POST","/api/ai/web/generations"],["deleteAiWebGenerationsGeneration","DELETE","/api/ai/web/generations/:generation"],["getAiWebGenerationsGeneration","GET","/api/ai/web/generations/:generation"],["getAiWebModels","GET","/api/ai/web/models"]]);
