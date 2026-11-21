// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=4364119c78f60e44";
export const trainingApi=defineEndpoints("training",[["getTrainingCandidates","GET","/api/training/candidates"],["postTrainingCandidates","POST","/api/training/candidates"],["postTrainingCandidatesCandidateRevoke","POST","/api/training/candidates/:candidate/revoke"]]);
