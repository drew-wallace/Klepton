'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const {gzipSync} = require('node:zlib');
const SteamUser = require('steam-user');
const EMsg = require('steam-user/enums/EMsg.js');
const {hydrateAppInfo, installProductInfoTransport} = require('./product_info.cjs');

const text = Buffer.from('"appinfo" { "common" { "name" "Example VR" "type" "Game" } }');
const sha = crypto.createHash('sha1').update(text).digest();
const record = () => ({appid: 10, buffer: null, sha, size: text.length, missing_token: false});

test('real steam-user parses bufferless appinfo through the CDN fallback', async () => {
  const client = new SteamUser({enablePicsCache: false, dataDirectory: null});
  client._send = (message, request, callback) => {
    assert.equal(message, EMsg.ClientPICSProductInfoRequest);
    assert.equal(request.meta_data_only, false);
    queueMicrotask(() => callback({apps: [record()], http_host: '', response_pending: false}));
  };
  installProductInfoTransport(client, async (url, options) => {
    assert.equal(url, `https://clientconfig.steamstatic.com/appinfo/10/sha/${sha.toString('hex')}.txt.gz`);
    assert.equal(options.redirect, 'error');
    assert.ok(options.signal);
    return new Response(gzipSync(text));
  });
  const result = await client.getProductInfo([10], [], false);
  assert.equal(result.apps[10].appinfo.common.name, 'Example VR');
});

test('inline metadata is preserved without fetching', async () => {
  const app = {...record(), buffer: Buffer.concat([text, Buffer.from([0])])};
  const body = {apps: [app]};
  assert.equal(await hydrateAppInfo(body, () => assert.fail('unexpected fetch')), body);
});

test('verifies CDN metadata and accepts already decompressed bodies', async () => {
  const app = record();
  await hydrateAppInfo({apps: [app], http_host: 'clientconfig.akamai.steamstatic.com'}, async () => new Response(text));
  assert.equal(app.buffer.toString(), text.toString() + '\0');
  await assert.rejects(hydrateAppInfo({apps: [record()]}, async () => new Response('corrupt')), /checksum/);
  await assert.rejects(hydrateAppInfo({apps: [record()]}, async () => new Response('', {status: 503})), /download failed/);
});

test('rejects untrusted metadata hosts before fetching', async () => {
  for (const host of ['localhost', 'steamstatic.com.evil.test', 'user@steamstatic.com', 'steamstatic.com/secret']) {
    await assert.rejects(hydrateAppInfo({apps: [record()], http_host: host}, () => assert.fail('unexpected fetch')), /Invalid/);
  }
});

test('token-only records remain retryable by steam-user', async () => {
  const app = {appid: 10, missing_token: true, buffer: null};
  await hydrateAppInfo({apps: [app]}, () => assert.fail('unexpected fetch'));
  assert.equal(app.missing_token, true);
  assert.equal(app.buffer.toString(), '"appinfo" {}\0');
});

test('multipart responses retain callback order while metadata is fetched', async () => {
  let release;
  const waiting = new Promise(resolve => { release = resolve; });
  const callbacks = [];
  const client = {
    _send(message, request, callback) {
      callback({apps: [record()], response_pending: true});
      callback({apps: [], response_pending: false});
    },
    emit() { assert.fail('unexpected error'); }
  };
  installProductInfoTransport(client, async () => { await waiting; return new Response(gzipSync(text)); });
  const completed = new Promise(resolve => {
    client._send(EMsg.ClientPICSProductInfoRequest, {}, body => {
      callbacks.push(body.response_pending);
      if (!body.response_pending) resolve();
    });
  });
  assert.deepEqual(callbacks, []);
  release();
  await completed;
  assert.deepEqual(callbacks, [true, false]);
});
