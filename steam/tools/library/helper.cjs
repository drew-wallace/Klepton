'use strict';

// Private pipe protocol only. Never print library exceptions, tokens, or inputs to diagnostics.
const fs = require('node:fs/promises');
const path = require('node:path');
const readline = require('node:readline');
const crypto = require('node:crypto');
const {once} = require('node:events');
const SteamUser = require('steam-user');
const {LoginSession, EAuthTokenPlatformType} = require('steam-session');
const {gameInfo, safePath} = require('./catalog.cjs');
const {downloadFile} = require('./download.cjs');
const {createTransport} = require('./transport.cjs');
const {CDNPool} = require('./cdn_pool.cjs');

let lastProgress = 0;
const send = (event, data = {}) => {
  // Large depots report a progress callback for every chunk. Keep UI/log traffic bounded.
  if (event === 'progress') {
    const now = Date.now();
    if (now - lastProgress < 200 && data.completed !== data.total && data.completed !== 0) return;
    lastProgress = now;
  }
  process.stdout.write(JSON.stringify({event, ...data}) + '\n');
};
const input = readline.createInterface({input: process.stdin, terminal: false});
let session, client;
let started = false;
let finishing = false;
input.on('line', line => {
  let command;
  try { command = JSON.parse(line); } catch { return; }
  if (!started) {
    started = true;
    main(command).then(() => finish(0), error => {
      const code = ['unsafe_path', 'no_android', 'no_license', 'file_collision', 'missing_apk', 'checksum', 'expired', 'disk_space'].includes(error.message)
        ? error.message : 'steam_failed';
      send('error', {code, reason: error.safeReason || ''});
      finish(1);
    });
  } else if (command.guard_code && session) {
    session.submitSteamGuardCode(String(command.guard_code)).catch(() => send('auth', {kind: 'guard', retry: true}));
  }
});
input.on('close', () => { if (!started) process.exit(1); });

function finish(code) {
  if (finishing) return;
  finishing = true;
  session?.cancelLoginAttempt();
  client?.logOff();
  // A large library result can exceed a pipe buffer. Flush it before exiting.
  process.stdout.write('', () => process.exit(code));
  setTimeout(() => process.exit(code), 2000).unref();
}
process.on('SIGTERM', () => finish(130));
process.on('SIGINT', () => finish(130));

async function authenticate(request) {
  if (request.refresh_token) return request.refresh_token;
  session = new LoginSession(EAuthTokenPlatformType.SteamClient, {machineFriendlyName: 'Klepton APK Installer'});
  session.loginTimeout = 180000;
  const completion = new Promise((resolve, reject) => {
    session.once('authenticated', () => resolve(session.refreshToken));
    session.once('error', reject);
    session.once('timeout', () => reject(new Error('expired')));
  });
  // Register handlers before starting a request; rejection must have a handler immediately.
  completion.catch(() => {});
  if (request.auth_method === 'password') {
    const result = await session.startWithCredentials({accountName: request.account_name, password: request.password});
    if (result.actionRequired) send('auth', {kind: 'guard'});
  } else {
    const result = await session.startWithQR();
    send('auth', {kind: 'qr', challenge_url: result.qrChallengeUrl});
  }
  return completion;
}

async function connect(request) {
  const refreshToken = await authenticate(request);
  // Null disables all steam-user on-disk caches, including machine auth material.
  // PICS caching eagerly requests every licensed app before ownershipCached.
  // Read package ownership ourselves so a scoped refresh stays scoped.
  client = new SteamUser({enablePicsCache: false, dataDirectory: null, autoRelogin: false, renewRefreshTokens: true});
  let latestToken = refreshToken;
  client.on('refreshToken', token => {
    latestToken = token;
    if (client.steamID) credential();
  });
  function credential() {
    send('credential', {refresh_token: latestToken, steam_id: client.steamID.getSteamID64(),
      account_name: session?.accountName || request.account_name || client.steamID.getSteamID64()});
  }
  // Licenses can arrive immediately after loggedOn; register both beforehand.
  const loggedOn = once(client, 'loggedOn');
  const owned = once(client, 'licenses');
  loggedOn.catch(() => {}); owned.catch(() => {});
  client.logOn({refreshToken, machineName: 'Klepton APK Installer'});
  await loggedOn;
  credential();
  await owned;
  // A disconnect must fail pending content requests, not leave the UI waiting indefinitely.
  const disconnected = () => { if (!finishing) { send('error', {code: 'steam_failed'}); finish(1); } };
  client.on('error', disconnected);
  client.on('disconnected', disconnected);
  return client.steamID.getSteamID64();
}

async function productInfo(ids) {
  const tokens = await client.getProductAccessToken(ids, []);
  const requested = ids.map(appid => ({appid, access_token: tokens.appTokens?.[appid]}));
  const response = await client.getProductInfo(requested, [], false);
  return response.apps || {};
}

async function ownership() {
  const packages = client.getOwnedPackages({excludeShared: true});
  const apps = new Set(), depots = new Set();
  if (packages.length) {
    const response = await client.getProductInfo([], packages, true);
    for (const record of Object.values(response.packages || {})) {
      for (const id of record.packageinfo?.appids || []) apps.add(Number(id));
      for (const id of record.packageinfo?.depotids || []) depots.add(Number(id));
    }
  }
  return {apps, depots};
}

async function resolveShared(info) {
  const shared = Object.values(info.depots || {}).filter(d => d && typeof d === 'object' && d.depotfromapp);
  if (!shared.length) return info;
  const sources = await productInfo([...new Set(shared.map(d => Number(d.depotfromapp)))]);
  const result = {...info, depots: {...info.depots}};
  for (const [id, depot] of Object.entries(info.depots || {})) {
    if (depot?.depotfromapp) {
      const source = sources[depot.depotfromapp]?.appinfo?.depots?.[id];
      if (source) result.depots[id] = {...source, ...depot, manifests: depot.manifests || source.manifests};
    }
  }
  return result;
}

async function main(request) {
  const steamID = await connect(request);
  const {apps: owned, depots: ownedDepots} = await ownership();
  if (request.action === 'refresh' || request.action === 'login') {
    const games = [];
    // Unknown titles still need discovery so newly purchased VR games appear.
    // Account matching prevents an old account's shortlist affecting a new login.
    const scoped = request.steam_id === steamID && Array.isArray(request.app_ids) && Array.isArray(request.known_app_ids);
    const candidates = new Set((request.app_ids || []).map(Number));
    const known = new Set((request.known_app_ids || []).map(Number));
    const ids = [...owned].filter(id => !scoped || candidates.has(id) || !known.has(id));
    for (let i = 0; i < ids.length; i += 50) {
      const apps = await productInfo(ids.slice(i, i + 50));
      for (const [id, record] of Object.entries(apps)) {
        const info = record.appinfo;
        if (!info || !['game', 'demo', 'application'].includes(String(info.common?.type || '').toLowerCase())) continue;
        games.push(gameInfo(id, await resolveShared(info), ownedDepots));
      }
      send('progress', {completed: Math.min(i + 50, ids.length), total: ids.length, kind: 'library'});
    }
    games.sort((a, b) => a.name.localeCompare(b.name));
    send('result', {steam_id: steamID, games, owned_app_ids: [...owned].map(String),
      ...(scoped ? {checked_app_ids: ids.map(String)} : {})});
    return;
  }
  const appID = Number(request.app_id);
  if (!owned.has(appID)) throw new Error('no_license');
  const records = await productInfo([appID]);
  const game = gameInfo(appID, await resolveShared(records[appID]?.appinfo || {}), ownedDepots);
  const branch = game.branches.find(b => b.name === request.branch && b.available);
  const launch = game.launches.find(l => l.id === request.launch_id);
  if (!branch || !launch) throw new Error('no_android');
  send('selection', {steam_id: steamID, game, branch, launch});
  // Download paths are assigned by the Python owner, never Steam or UI metadata.
  const destination = path.resolve(request.destination);
  const inventory = new Map();
  for (const depot of branch.depots) {
    const {manifest} = await client.getManifest(Number(depot.app_id), Number(depot.depot_id), depot.manifest_id, request.branch);
    for (const file of manifest.files) {
      if (file.flags & 64) continue; // Directory entry, never a payload.
      const name = safePath(file.filename);
      if ((file.flags & 512) || file.linktarget) throw new Error('unsafe_path');
      const existing = inventory.get(name.toLowerCase());
      if (existing && (existing.file.sha_content !== file.sha_content || existing.name !== name)) throw new Error('file_collision');
      inventory.set(name.toLowerCase(), {name, file, depot});
    }
  }
  if (!inventory.has(launch.executable.toLowerCase())) throw new Error('missing_apk');
  const files = [];
  const total = [...inventory.values()].reduce((n, e) => n + Number(e.file.size), 0);
  send('progress', {kind: 'download', completed: 0, total});
  const space = await fs.statfs(destination);
  if (Number(space.bavail) * Number(space.bsize) < total * 1.1) throw new Error('disk_space');
  let completed = 0;
  const transport = createTransport(client);
  const pools = new Map();
  try {
  for (const {name, file, depot} of inventory.values()) {
    const output = path.join(destination, name);
    await fs.mkdir(path.dirname(output), {recursive: true, mode: 0o700});
    try {
      if (!pools.has(depot.app_id)) pools.set(depot.app_id, new CDNPool((await client.getContentServers(Number(depot.app_id))).servers));
      let details = {};
      await downloadFile(client, Number(depot.app_id), Number(depot.depot_id), file, output,
        bytes => send('progress', {kind: 'download', completed: completed + bytes, total, ...details,
          eta_seconds: details.verified_bps > 0 ? Math.ceil((total - completed - bytes) / details.verified_bps) : null}),
        {transport, pool: pools.get(depot.app_id), onStatus: status => { details = status; }});
    } catch (error) {
      const message = String(error.message || '');
      const failure = new Error(/checksum|CRC/i.test(message) ? 'checksum' : 'steam_failed');
      const http = /^HTTP error ([0-9]{3})$/.exec(message);
      failure.safeReason = http ? `HTTP ${http[1]}` : error.code === 'ESLOWCDN' ? 'Slow CDN connection replaced' :
        /no verified progress/i.test(message) ? 'No verified download progress' : /timed out|timeout/i.test(message) ? 'CDN request timed out' :
        /File checksum mismatch/i.test(message) ? 'File checksum mismatch' : /checksum|CRC/i.test(message) ? 'Chunk checksum mismatch' :
        /compression|decompress|Zstd|VZip/i.test(message) ? 'Chunk decompression failed' :
        ['ENOSPC', 'EIO', 'EACCES', 'ECONNRESET', 'ETIMEDOUT', 'ENOTFOUND'].includes(error.code) ? error.code : 'CDN transfer failed';
      throw failure;
    }
    const hash = crypto.createHash('sha256');
    const stream = require('node:fs').createReadStream(output);
    for await (const chunk of stream) hash.update(chunk);
    files.push({path: name, size: Number(file.size), sha256: hash.digest('hex')});
    completed += Number(file.size);
    send('progress', {kind: 'download', completed, total});
  }
  } finally { await transport.close(); }
  send('result', {steam_id: steamID, game, branch, launch, files});
}
