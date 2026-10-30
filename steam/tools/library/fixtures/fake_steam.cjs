'use strict';
// Loaded only by tests through Node's --require flag, never by the production helper.
const Module = require('node:module');
const {EventEmitter} = require('node:events');
const fs = require('node:fs/promises');
const crypto = require('node:crypto');
const scenario = process.env.KLEPTON_TEST_SCENARIO || '';
const app = {common: {name: 'Example VR', type: 'Game', categories: {category_54: '1'}},
  config: {launch: {0: {executable: 'game.apk', config: {oslist: 'android'}}}},
  depots: {100: {config: {oslist: 'android'}, manifests: {public: {gid: '18446744073709551614'}}},
    branches: {public: {buildid: '1000', timeupdated: '1700000000'}}}};

class FakeClient extends EventEmitter {
  constructor(options) {
    super();
    if (options.dataDirectory !== null || options.enablePicsCache !== false) throw new Error('eager cache enabled');
  }
  logOn() {
    this.steamID = {getSteamID64: () => '76561198000000001'};
    setImmediate(() => {
      if (scenario === 'connection') return this.emit('error', new Error('sensitive-client-error'));
      this.emit('loggedOn'); this.emit('licenses');
    });
  }
  logOff() {}
  getOwnedApps() { return scenario === 'no_license' ? [] : scenario === 'large' ? Array.from({length: 1500}, (_, i) => i + 10) : [10]; }
  getOwnedPackages(filter) {
    if (filter.excludeShared !== true) throw new Error('shared licenses included');
    return [1];
  }
  async getProductAccessToken() { return {appTokens: {10: 'access-token'}}; }
  async getProductInfo(ids, packages) {
    if (packages.length) return {packages: {1: {packageinfo: {
      appids: scenario === 'scoped' ? [10, 11, 12] : this.getOwnedApps(), depotids: [100]}}}};
    if (scenario === 'scoped' && ids.some(id => ![10, 12].includes(id.appid))) throw new Error('unrelated metadata requested');
    return {apps: Object.fromEntries(ids.map(id => [id.appid, {appinfo: app}]))};
  }
  async getManifest() {
    if (scenario === 'manifest') throw new Error('sensitive-manifest-error');
    let paths = ['game.apk', 'obb/content.obb', 'loose.bundle', 'empty'];
    if (scenario === 'unsafe') paths.push('../escape');
    if (scenario === 'missing_apk') paths = ['obb/content.obb'];
    if (scenario === 'collision') paths.push('GAME.APK');
    return {manifest: {files: paths.map(filename => ({filename, size: filename === 'empty' ? '0' : '4',
      chunks: filename === 'empty' ? [] : [{offset:'0', cb_original:4, sha:crypto.createHash('sha1').update('data').digest('hex')}],
      sha_content: crypto.createHash('sha1').update(filename === 'empty' ? '' : 'data').digest('hex'), flags: 0}))}};
  }
  async getContentServers() { return {servers: [{Host:'fixture', https_support:'optional'}]}; }
  async downloadChunk() {
    if (scenario === 'checksum') throw new Error('sensitive-checksum-error');
    if (scenario === 'cdn_timeout') throw new Error('Request timed out');
    if (scenario === 'cdn_http') throw new Error('HTTP error 503');
    if (scenario === 'cdn_private_error') throw new Error('sensitive-token-in-an-unexpected-error');
    return {chunk: Buffer.from('data')};
  }
}
class FakeSession extends EventEmitter {
  accountName = 'Fixture Account';
  refreshToken = 'private-fixture-refresh-token';
  async startWithQR() {
    if (scenario === 'expired') setTimeout(() => this.emit('timeout'), 10);
    else setTimeout(() => this.emit('authenticated'), 10);
    return {qrChallengeUrl: 'https://s.team/q/fixture'};
  }
  async startWithCredentials() { return {actionRequired: true}; }
  async submitSteamGuardCode(code) {
    if (code !== '12345') throw new Error('incorrect-code');
    this.emit('authenticated');
  }
  cancelLoginAttempt() {}
}
const original = Module._load;
Module._load = function(name, ...args) {
  if (name === './transport.cjs' && args[0]?.filename.endsWith('/helper.cjs')) return {
    createTransport: client => ({
      async chunk(app, depot, sha, server, context) {
        const result = await client.downloadChunk(app, depot, sha, server);
        context.onBytes?.(result.chunk.length); context.onDecoding?.();
        return {...result, networkBytes: result.chunk.length, networkMs: 1};
      },
      async close() {}
    })
  };
  if (name === 'steam-user') return FakeClient;
  if (name === 'steam-session') return {LoginSession: FakeSession, EAuthTokenPlatformType: {SteamClient: 1}};
  return original.call(this, name, ...args);
};
