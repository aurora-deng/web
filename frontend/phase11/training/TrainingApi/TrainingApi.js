// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=dc39cdb5489c964d";
export const trainingApi=defineEndpoints("training",[["getTrainingCandidates","GET","/api/training/candidates"],["postTrainingCandidates","POST","/api/training/candidates"],["postTrainingCandidatesCandidateRevoke","POST","/api/training/candidates/:candidate/revoke"]]);
