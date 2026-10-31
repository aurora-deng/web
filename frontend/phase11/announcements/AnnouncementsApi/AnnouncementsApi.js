// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=dc39cdb5489c964d";
export const announcementsApi=defineEndpoints("announcements",[["getAnnouncements","GET","/api/announcements"],["getAnnouncementsAnnouncementAudio","GET","/api/announcements/:announcement/audio"],["getAnnouncementsAnnouncementAudioContent","GET","/api/announcements/:announcement/audio/content"],["postAnnouncementsAnnouncementClaim","POST","/api/announcements/:announcement/claim"]]);
