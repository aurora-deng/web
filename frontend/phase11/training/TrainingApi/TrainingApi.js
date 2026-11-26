// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=0613976363093f95";
export const trainingApi=defineEndpoints("training",[["getTrainingCandidates","GET","/api/training/candidates"],["postTrainingCandidates","POST","/api/training/candidates"],["postTrainingCandidatesCandidateRevoke","POST","/api/training/candidates/:candidate/revoke"]]);
