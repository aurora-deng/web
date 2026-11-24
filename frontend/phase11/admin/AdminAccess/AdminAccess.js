import { adminApi } from '../AdminApi/AdminApi.js?v=9fcaa747c305a8cc';

const moderationViews = Object.freeze(['reports', 'audits', 'categories']);
const administratorViews = Object.freeze(['users', 'reports', 'audits', 'messaging', 'categories', 'runtime', 'speech', 'models', 'training']);
const noViews = Object.freeze([]);

export function adminViews(role) {
  if (role === 'administrator') return administratorViews;
  if (role === 'moderator') return moderationViews;
  return noViews;
}

export async function readAdminRole(api, userId, signal) {
  const identity = await api.request(adminApi.getAdminMe({}), { signal });
  const sameUser = String(identity?.id) === String(userId);
  return sameUser && adminViews(identity?.role).length ? identity.role : null;
}
