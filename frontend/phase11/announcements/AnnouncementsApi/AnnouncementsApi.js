// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=9fcaa747c305a8cc";
export const announcementsApi=defineEndpoints("announcements",[["getAnnouncements","GET","/api/announcements"],["getAnnouncementsAnnouncementAudio","GET","/api/announcements/:announcement/audio"],["getAnnouncementsAnnouncementAudioContent","GET","/api/announcements/:announcement/audio/content"],["postAnnouncementsAnnouncementClaim","POST","/api/announcements/:announcement/claim"]]);
