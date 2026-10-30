'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const http = require('node:http');
const crypto = require('node:crypto');
const SteamCrypto = require('@doctormckay/steam-crypto');
const AdmZip = require('adm-zip');
const {receive, createTransport} = require('./transport.cjs');

async function serve(t, handler) {
  const server = http.createServer(handler);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => { server.closeAllConnections(); server.close(); });
  return new URL(`http://127.0.0.1:${server.address().port}/chunk`);
}

test('aborts a trickling connection at the total deadline and closes its socket', async t => {
  let closed = false;
  const url = await serve(t, (req, res) => {
    res.writeHead(200); res.flushHeaders();
    const timer = setInterval(() => res.write('x'), 5);
    res.on('close', () => { closed = true; clearInterval(timer); });
  });
  await assert.rejects(receive(url, {}, {deadlineMs: 70, idleMs: 500, graceMs: 500}), {code:'ESLOWCDN'});
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(closed, true);
});
test('rejects a very slow stream even while it keeps sending bytes', async t => {
  const url = await serve(t, (req, res) => {
    res.writeHead(200); res.flushHeaders();
    const timer = setInterval(() => res.write('x'), 5);
    res.on('close', () => clearInterval(timer));
  });
  await assert.rejects(receive(url, {}, {deadlineMs: 1000, idleMs: 500, graceMs: 30, checkMs: 10, minimumRate: 100000}), {code:'ESLOWCDN'});
});
test('aborts a connection that stops sending data', async t => {
  const url = await serve(t, (req, res) => { res.writeHead(200); res.flushHeaders(); });
  await assert.rejects(receive(url, {}, {deadlineMs: 500, idleMs: 30}), {code:'ETIMEDOUT'});
});
test('cancellation closes an in-flight request', async t => {
  const controller = new AbortController();
  let closed = false;
  const url = await serve(t, (req, res) => {
    res.writeHead(200); res.flushHeaders(); res.on('close', () => { closed = true; });
  });
  const request = receive(url, {}, {signal: controller.signal});
  setTimeout(() => controller.abort(), 20);
  await assert.rejects(request, {name:'AbortError'});
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal(closed, true);
});
test('bounds response memory and retains only safe HTTP errors', async t => {
  const url = await serve(t, (req, res) => { res.writeHead(200, {'Content-Length': '2000'}); res.end('data'); });
  await assert.rejects(receive(url, {}, {limit:100}), /Invalid chunk length/);
  const unavailable = await serve(t, (req, res) => { res.writeHead(503); res.end('private response'); });
  await assert.rejects(receive(unavailable, {}), {message:'HTTP error 503'});
});
test('decrypts and unpacks Steam chunks off the download thread', async t => {
  const key = crypto.randomBytes(32);
  const payload = crypto.randomBytes(1024 * 1024);
  const zip = new AdmZip(); zip.addFile('chunk', payload);
  const encrypted = SteamCrypto.symmetricEncrypt(zip.toBuffer(), key);
  const url = await serve(t, (req, res) => { res.writeHead(200, {'Content-Length': encrypted.length}); res.end(encrypted); });
  const client = {async getDepotDecryptionKey() { return {key}; }};
  const transport = createTransport(client);
  t.after(() => transport.close());
  let received = 0, unpacking = false;
  const result = await transport.chunk(10, 20, crypto.createHash('sha1').update(payload).digest('hex'),
    {Host:url.host, https_support:'none'}, {onBytes: n => { received += n; }, onDecoding: () => { unpacking = true; }});
  assert.deepEqual(result.chunk, payload);
  assert.equal(result.networkBytes, encrypted.length);
  assert.equal(received, encrypted.length);
  assert.equal(unpacking, true);
  assert.ok(Number.isFinite(result.networkMs));
});
