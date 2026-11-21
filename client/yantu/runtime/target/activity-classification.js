(function (global) {
    'use strict';
    const types = Object.freeze(['exam', 'skill', 'project', 'unclassified']);
    const statuses = Object.freeze(['todo', 'in_progress', 'done', 'cancelled']);
    const labels = Object.freeze({exam: '备考', skill: '技能', project: '项目', unclassified: '未分类'});
    const settingKeys = new Set(['prepStatus', 'prepStart', 'examYear', 'targetYear', 'examDate', 'studySchedule', 'reminderSettings', 'lastTaskRefresh', 'subjectProfiles', 'planCapacities', 'learningPrefs', 'cardPreferences', 'focusTimer', 'planningFocusLease', 'focusSessions', 'timeBlocks']);
    const links = ['taskId', 'sourceId', 'originalId', 'reviewId', 'recordId'];
    const activityType = value => types.includes(value?.activityType ?? value) ? (value?.activityType ?? value) : 'unclassified';
    const taskStatus = task => task?.taskStatus === 'unknown' && task.bridgeReadonly && isObsidianRecord(task) ? 'unknown' : statuses.includes(task?.taskStatus) ? task.taskStatus : task?.done ? 'done' : 'todo';
    const taskIsActive = task => ['todo', 'in_progress'].includes(taskStatus(task));
    const isObsidianRecord = record => Boolean(record && typeof record === 'object' && (['obsidian', 'obsidianExport'].includes(record.source) || 'bridgeRef' in record || record.obsidianExport === true));
    const canCarryTask = task => taskIsActive(task) && !isObsidianRecord(task);
    const records = state => Object.values(state).flatMap(value => Array.isArray(value) ? value.filter(item => item && typeof item === 'object') : value && typeof value === 'object' ? [value] : []);
    function isPrivate(value, blocked) {
        if (!value || typeof value !== 'object') return false;
        if (Array.isArray(value)) return value.some(item => isPrivate(item, blocked));
        return isObsidianRecord(value) || blocked.has(value.id) || links.some(key => blocked.has(value[key])) || Object.values(value).some(item => isPrivate(item, blocked));
    }
    function privateRecordIds(state) {
        const all = records(state), blocked = new Set();
        const known = new Set(all.map(item => item.id).filter(Boolean));
        for (const item of [...state.focusSessions || [], ...state.timeBlocks || [], state.planningFocusLease, state.focusTimer]) {
            for (const key of ['sourceId', 'taskId']) if (item?.[key] && !known.has(item[key])) blocked.add(item[key]);
        }
        let size;
        do { size = blocked.size; for (const item of all) if (item.id && isPrivate(item, blocked)) blocked.add(item.id); } while (size !== blocked.size);
        return blocked;
    }
    function privateAttachmentIds(state) {
        const blocked = privateRecordIds(state), files = new Set();
        function collect(value) {
            if (!value || typeof value !== 'object') return;
            if (Array.isArray(value)) { for (const child of value) collect(child); return; }
            if (value.fileId) files.add(value.fileId);
            for (const file of value.files || []) if (file.id) files.add(file.id);
            for (const child of Object.values(value)) collect(child);
        }
        for (const item of records(state)) if (blocked.has(item.id) || isPrivate(item, blocked)) {
            collect(item);
        }
        return files;
    }
    function aiSafeState(state) {
        const blocked = privateRecordIds(state), safe = {};
        for (const [key, value] of Object.entries(state)) {
            if (/^(obsidian|bridge)/i.test(key)) continue;
            if (Array.isArray(value)) safe[key] = value.filter(item => !isPrivate(item, blocked) && !blocked.has(item?.id));
            else if (settingKeys.has(key) && !isPrivate(value, blocked)) safe[key] = value;
        }
        return safe;
    }
    const aiSettings = state => Object.fromEntries(Object.entries(aiSafeState(state)).filter(([key]) => settingKeys.has(key)));
    const addDays = (day, n) => { const value = new Date(day + 'T12:00:00Z'); value.setUTCDate(value.getUTCDate() + n); return value.toISOString().slice(0, 10); };
    const dayStart = day => global.PrepLocal?.dayStart ? global.PrepLocal.dayStart(day) : new Date(day + 'T00:00:00').getTime();
    const timestamp = value => typeof value === 'number' ? value : typeof value === 'string' ? Date.parse(value) : NaN;
    function sessionDaySeconds(session, day) {
        const start = timestamp(session.startedAt), end = timestamp(session.endedAt), seconds = session.seconds;
        if (![start, end, seconds].every(Number.isFinite) || end <= start || seconds <= 0) return 0;
        return Math.max(0, Math.min(end, start + seconds * 1000, dayStart(addDays(day, 1))) - Math.max(start, dayStart(day))) / 1000;
    }
    function uniqueSessions(sessions) {
        const seen = new Set(), events = new Set();
        return (sessions || []).filter(item => {
            if (!item.id || seen.has(item.id) || item.sourceEventId && events.has(item.sourceEventId)) return false;
            seen.add(item.id); if (item.sourceEventId) events.add(item.sourceEventId); return true;
        });
    }
    function dailySummary(state, day) {
        const byType = Object.fromEntries(types.map(type => [type, 0]));
        for (const session of uniqueSessions(state.focusSessions)) byType[activityType(session)] += sessionDaySeconds(session, day);
        const tasks = [...new Map((state.tasks || []).filter(item => item.id).map(item => [item.id, item])).values()];
        return {totalSeconds: Object.values(byType).reduce((sum, value) => sum + value, 0), examSeconds: byType.exam, byType, completedTasks: tasks.filter(item => taskStatus(item) === 'done' && (item.completedDate || item.date) === day).length, pendingTasks: tasks.filter(item => taskIsActive(item) && item.date === day).length};
    }
    global.YantuActivity = Object.freeze({types, statuses, labels, activityType, taskStatus, taskIsActive, isObsidianRecord, canCarryTask, privateRecordIds, privateAttachmentIds, aiSafeState, aiSettings, sessionDaySeconds, uniqueSessions, dailySummary, addDays});
})(window);
