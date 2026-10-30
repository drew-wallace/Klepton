'use strict';

const crypto = require('node:crypto');
const path = require('node:path');

const yes = value => value === true || value === 1 || value === '1';
const words = value => String(value || '').toLowerCase().split(/[,;\s]+/).filter(Boolean);

function safePath(value) {
  if (typeof value !== 'string' || !value || /[\x00-\x1f]/.test(value)) throw new Error('unsafe_path');
  const name = value.replaceAll('\\', '/');
  if (name.startsWith('/') || /^[a-z]:/i.test(name) || name.split('/').some(p => p === '..' || p === '')) {
    throw new Error('unsafe_path');
  }
  const normalized = path.posix.normalize(name);
  if (normalized === '.' || normalized.startsWith('../')) throw new Error('unsafe_path');
  return normalized;
}

function revision(depots) {
  return crypto.createHash('sha256').update(JSON.stringify(depots.map(d => [String(d.depot_id), String(d.manifest_id)])
    .sort((a, b) => a[0].localeCompare(b[0])))).digest('hex');
}

function gameInfo(appID, info, ownedDepots) {
  const common = info.common || {};
  const launchEntries = Object.entries(info.config?.launch || {}).flatMap(([id, launch]) => {
    const os = words(launch.config?.oslist);
    if (!/\.apk$/i.test(launch.executable || '') || (os.length && !os.includes('android'))) return [];
    try {
      return [{id, name: launch.description || launch.executable, executable: safePath(launch.executable)}];
    } catch { return []; }
  });
  const depots = Object.entries(info.depots || {}).filter(([id, d]) => /^\d+$/.test(id) && typeof d === 'object');
  const androidOnly = words(common.oslist).length === 1 && words(common.oslist)[0] === 'android';
  const branches = Object.entries(info.depots?.branches || {public: {}}).map(([name, b]) => {
    const selected = depots.flatMap(([id, d]) => {
      const os = words(d.config?.oslist);
      const language = words(d.config?.language);
      const arch = String(d.config?.osarch || '');
      const manifest = d.manifests?.[name];
      const gid = typeof manifest === 'object' ? manifest?.gid : manifest;
      // Never turn an imprecise JS number into a potentially rounded manifest ID.
      if (!ownedDepots.has(Number(id)) || (os.length && !os.includes('android')) ||
          (language.length && !language.includes('english')) || (arch && !['64', 'arm64'].includes(arch)) ||
          typeof gid !== 'string' || !/^\d+$/.test(gid)) return [];
      return [{depot_id: id, manifest_id: gid,
        app_id: String(d.depotfromapp && !yes(common.freetodownload) ? d.depotfromapp : appID)}];
    });
    const locked = yes(b.pwdrequired);
    const accessibleAndroid = selected.some(d => words(info.depots[d.depot_id].config?.oslist).includes('android'));
    const available = !locked && (accessibleAndroid || androidOnly) && launchEntries.length > 0 && selected.length > 0;
    const timestamp = Number(b.timeupdated || b.timebuildupdated || 0);
    return {name, build_id: String(b.buildid || ''), updated_at: Number.isFinite(timestamp) && timestamp > 0 && timestamp < 8640000000000 ? new Date(timestamp * 1000).toISOString() : null,
      locked, available, depots: selected, revision: available ? revision(selected) : null};
  }).sort((a, b) => a.name === 'public' ? -1 : b.name === 'public' ? 1 : a.name.localeCompare(b.name));
  const vr = yes(common.openvrsupport) || yes(common.oculussupport) || yes(common.categories?.category_31) ||
    yes(common.categories?.category_53) || yes(common.categories?.category_54) ||
    Object.values(common.vrheadsets || {}).some(yes) ||
    Object.values(info.config?.launch || {}).some(l => l.type === 'vr');
  return {app_id: String(appID), name: common.name || `App ${appID}`, vr,
    android_available: branches.some(b => b.available), launches: launchEntries, branches};
}

module.exports = {safePath, revision, gameInfo};
