'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const {downloadFile} = require('./download.cjs');
const hash = data => crypto.createHash('sha1').update(data).digest('hex');

async function fixture(t) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'klepton-chunks-'));
  t.after(() => fs.rm(directory, {recursive: true, force: true}));
  const buffers = [Buffer.from('abc'), Buffer.from('defg'), Buffer.from('hijkl')];
  let offset = 0;
  const chunks = buffers.map(data => {
    const chunk = {offset: String(offset), cb_original: data.length, sha: hash(data)};
    offset += data.length;
    return chunk;
  });
  const content = Buffer.concat(buffers);
  const file = {size: String(content.length), chunks, sha_content: hash(content)};
  const calls = new Map();
  const client = {
    async getContentServers() { return {servers: [{Host: 'first', https_support: 'optional'}, {Host: 'second', https_support: 'mandatory'}]}; },
    async downloadChunk(app, depot, sha, server) {
      assert.equal(server.https_support, 'mandatory', 'Use HTTPS when the server supports it');
      calls.set(sha, (calls.get(sha) || 0) + 1);
      return {chunk: buffers[chunks.findIndex(chunk => chunk.sha === sha)]};
    }
  };
  return {client, calls, file, content, output: path.join(directory, 'game.apk')};
}

test('rotates timed-out servers while retaining already verified chunks', async t => {
  const f = await fixture(t);
  const original = f.client.downloadChunk;
  let failed = false;
  f.client.downloadChunk = async (...args) => {
    if (!failed) { failed = true; throw new Error('Request timed out'); }
    return original(...args);
  };
  const progress = [];
  await downloadFile(f.client, 10, 20, f.file, f.output, value => progress.push(value), {retryDelay: 0});
  assert.deepEqual(await fs.readFile(f.output), f.content);
  assert.deepEqual([...f.calls.values()], [1, 1, 1]);
  assert.equal(progress.at(-1), f.content.length);
  assert.deepEqual(progress, [...progress].sort((a, b) => a - b));
});
test('rejects a corrupted chunk and retries it before writing', async t => {
  const f = await fixture(t);
  const original = f.client.downloadChunk;
  let corrupt = true;
  f.client.downloadChunk = async (...args) => {
    if (corrupt) { corrupt = false; return {chunk: Buffer.from('bad')}; }
    return original(...args);
  };
  await downloadFile(f.client, 10, 20, f.file, f.output, () => {}, {retryDelay: 0});
  assert.deepEqual(await fs.readFile(f.output), f.content);
});
test('bounds retries and reports a persistent CDN timeout', async t => {
  const f = await fixture(t);
  let attempts = 0;
  f.client.downloadChunk = async () => { attempts++; throw new Error('Request timed out'); };
  await assert.rejects(downloadFile(f.client, 10, 20, f.file, f.output, () => {}, {retryDelay: 0, attempts: 3}), /Request timed out/);
  assert.ok(attempts >= 3 && attempts <= 9); // A terminal failure also cancels peer retries.
});
test('checks the complete file after independently verified chunks', async t => {
  const f = await fixture(t);
  f.file.sha_content = hash('a different complete file');
  await assert.rejects(downloadFile(f.client, 10, 20, f.file, f.output, () => {}, {retryDelay: 0}), /File checksum mismatch/);
});
test('rejects gaps, overlap, and imprecise manifest offsets before download', async t => {
  const f = await fixture(t);
  for (const offset of ['1', '-1', '9007199254740993']) {
    const file = structuredClone(f.file);
    file.chunks[0].offset = offset;
    await assert.rejects(downloadFile(f.client, 10, 20, file, f.output, () => {}), /Invalid manifest chunk/);
  }
  assert.equal(f.calls.size, 0);
});
test('creates empty manifest files without contacting a CDN', async t => {
  const f = await fixture(t);
  f.client.getContentServers = () => { throw new Error('must not contact CDN'); };
  await downloadFile(f.client, 10, 20, {size:'0', chunks:[], sha_content:'0'.repeat(40)}, f.output, () => {});
  assert.equal((await fs.stat(f.output)).size, 0);
});
test('emits heartbeats while unpacking and fails if no verified progress is possible', async t => {
  const f = await fixture(t);
  const statuses = [];
  const transport = {chunk(app, depot, sha, host, context) {
    context.onBytes(50); context.onDecoding();
    return new Promise((resolve, reject) => context.signal.addEventListener('abort', () => reject(context.signal.reason), {once:true}));
  }};
  await assert.rejects(downloadFile(f.client, 10, 20, f.file, f.output, () => {},
    {transport, heartbeatMs:10, stallMs:40, onStatus: status => statuses.push(status)}), /no verified progress/);
  assert.ok(statuses.some(s => s.phase === 'unpacking'));
  assert.ok(statuses.length >= 3);
});
