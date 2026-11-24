// Generated domain contract bindings; transport and page state are injected separately.
import { defineEndpoints } from "../../shared/ApiContracts/ApiContracts.js?v=1b40f8c19ed29802";
export const activitiesApi=defineEndpoints("activities",[["getActivities","GET","/api/activities"],["postActivities","POST","/api/activities"],["getActivitiesPost","GET","/api/activities/:post"],["postActivitiesPostFavorite","POST","/api/activities/:post/favorite"],["getActivitiesPostShare","GET","/api/activities/:post/share"],["getActivitiesFavorites","GET","/api/activities/favorites"]]);
