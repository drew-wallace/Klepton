'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {CDNPool} = require('./cdn_pool.cjs');
const candidates = ['first','second','third'].map(Host => ({Host, https_support:'optional'}));
const sample = (bytes, ms) => ({chunk: Buffer.alloc(1), networkBytes: bytes, networkMs: ms});

test('uses measured throughput and temporarily avoids slow successful servers', async () => {
  const pool = new CDNPool(candidates.slice(0, 2));
  const first = await pool.acquire(), second = await pool.acquire();
  pool.release(first, sample(10000, 1000));
  pool.release(second, sample(1000000, 1000));
  const selected = await pool.acquire();
  assert.equal(selected.server.Host, 'second');
  pool.release(selected, sample(1000000, 1000));
  first.active++;
  pool.release(first, sample(10000, 1000));
  assert.ok(first.cooldown > Date.now());
  assert.equal(pool.switches, 1);
});
test('cools down failed hosts and continues on alternatives', async () => {
  const pool = new CDNPool(candidates);
  const bad = await pool.acquire();
  pool.release(bad, null, new Error('Request timed out'));
  const next = await pool.acquire();
  assert.notEqual(next.server.Host, bad.server.Host);
  assert.equal(next.server.https_support, 'mandatory');
  pool.release(next, sample(1000, 1));
});
test('caps per-host concurrency and cancels waiting workers', async () => {
  const pool = new CDNPool(candidates.slice(0,1));
  const first = await pool.acquire(), second = await pool.acquire();
  const controller = new AbortController();
  const waiting = pool.acquire(controller.signal);
  controller.abort(new Error('cancelled'));
  await assert.rejects(waiting, /cancelled/);
  assert.equal(first.active, 2);
  pool.release(first, sample(1000,1)); pool.release(second, sample(1000,1));
});
test('periodically samples unused alternatives instead of permanently pinning a host', async () => {
  const pool = new CDNPool(candidates);
  const first = await pool.acquire(); pool.release(first, sample(1000000,1));
  const sampled = new Set();
  for (let i = 0; i < 30; i++) {
    const selected = await pool.acquire(); sampled.add(selected.server.Host);
    pool.release(selected, sample(10000,1000));
  }
  assert.ok(sampled.size > 1);
});
