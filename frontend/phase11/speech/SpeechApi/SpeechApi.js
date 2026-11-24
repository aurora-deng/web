// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=1b40f8c19ed29802";
export const speechApi=defineEndpoints("speech",[["getMeSpeechAudioKey","GET","/api/me/speech/audio/:key"],["postMeSpeechPreview","POST","/api/me/speech/preview"],["getMeSpeechPreviewVoice","GET","/api/me/speech/preview/:voice"],["getMeSpeechStatus","GET","/api/me/speech/status"]]);
