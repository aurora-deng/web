// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=7f8c7f1b31e4435c";
export const speechApi=defineEndpoints("speech",[["getMeSpeechAudioKey","GET","/api/me/speech/audio/:key"],["postMeSpeechPreview","POST","/api/me/speech/preview"],["getMeSpeechPreviewVoice","GET","/api/me/speech/preview/:voice"],["getMeSpeechStatus","GET","/api/me/speech/status"]]);
