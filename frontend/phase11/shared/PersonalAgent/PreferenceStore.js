import { emptyProfile } from './PreferenceProfile.js?v=1b40f8c19ed29802';
import { readCheckpoint } from './ContextWindow.js?v=1b40f8c19ed29802';

const storeName = 'profiles';
let connection;

function database() {
  if (connection) return connection;
  const pending = new Promise((resolve, reject) => {
    const request = indexedDB.open('nebula-personal-agent', 1);
    request.onupgradeneeded = () => request.result.createObjectStore(storeName);
    request.onerror = () => reject(request.error);
    request.onsuccess = () => {
      const db = request.result;
      db.onversionchange = () => { db.close(); connection = undefined; };
      resolve(db);
    };
  });
  connection = pending;
  pending.catch(() => { if (connection === pending) connection = undefined; });
  return pending;
}

const verifiedSite = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);

// A companion may point the same loopback browser origin at different Linux servers.
export function preferenceSite(session) {
  return session?.available ? (session.authenticated && verifiedSite(session.siteScope) ? session.siteScope : null) : 'origin';
}

export function preferenceScope(account, space, site = 'origin') {
  if (!/^[0-9]{1,20}$/.test(String(account)) || !['web', 'private'].includes(space) || (site !== 'origin' && !verifiedSite(site))) {
    throw new Error('无效的偏好空间');
  }
  return `${site}:${space}:${account}`;
}

async function readRecord(key, defaultValue) {
    const db = await database();
    return new Promise((resolve, reject) => {
      const request = db.transaction(storeName).objectStore(storeName).get(key);
      request.onsuccess = () => resolve(request.result ?? defaultValue());
      request.onerror = () => reject(request.error);
    });
}

async function changeRecord(key, defaultValue, update) {
    const db = await database();
    return new Promise((resolve, reject) => {
      const transaction = db.transaction(storeName, 'readwrite');
      const store = transaction.objectStore(storeName);
      let changed = null;
      let updateError;
      transaction.oncomplete = () => resolve(changed);
      transaction.onabort = () => reject(updateError ?? transaction.error ?? new Error('本机偏好保存未完成'));
      const request = store.get(key);
      request.onsuccess = () => {
        try {
          changed = update(request.result ?? defaultValue());
          if (changed) store.put(changed, key);
        } catch (error) {
          updateError = error;
          transaction.abort();
        }
      };
    });
}

export const preferenceStore = {
  read: scope => readRecord(scope, emptyProfile),
  change: (scope, update) => changeRecord(scope, emptyProfile, update),
};

export const conversationStore = {
  async read(scope, thread) {
    const rows = await readRecord(scope + ':conversations', () => []);
    return readCheckpoint(rows.find(row => row.thread === thread)?.checkpoint);
  },
  save(scope, thread, checkpoint) {
    return changeRecord(scope + ':conversations', () => [], rows => {
      const latest = readCheckpoint(rows.find(row => row.thread === thread)?.checkpoint);
      if (latest && BigInt(latest.through) >= BigInt(checkpoint.through)) return null;
      return [{ thread, checkpoint }, ...rows.filter(row => row.thread !== thread)].slice(0, 32);
    });
  },
  remove: (scope, thread) => changeRecord(scope + ':conversations', () => [], rows => rows.filter(row => row.thread !== thread)),
};
