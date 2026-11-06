// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=144f1e6009a3d461";
export const speechApi=defineEndpoints("speech",[["getMeSpeechAudioKey","GET","/api/me/speech/audio/:key"],["postMeSpeechPreview","POST","/api/me/speech/preview"],["getMeSpeechPreviewVoice","GET","/api/me/speech/preview/:voice"],["getMeSpeechStatus","GET","/api/me/speech/status"]]);
