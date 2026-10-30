'use strict';
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');
const {Worker} = require('node:worker_threads');

function timeoutError(code) {
  return Object.assign(new Error(code === 'ESLOWCDN' ? 'CDN transfer too slow' : 'Request timed out'), {code});
}

// Unlike steam-user's inactivity timeout, these timers destroy the request,
// including connections that keep trickling bytes without finishing a chunk.
function receive(url, headers, options = {}) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    let received = 0, settled = false, response;
    const buffers = [];
    const request = (url.protocol === 'https:' ? https : http).get(url, {headers, agent: options.agent, signal: options.signal});
    let idle, deadline, speed;
    const cleanup = () => { clearTimeout(idle); clearTimeout(deadline); clearInterval(speed); };
    const fail = error => {
      if (settled) return;
      settled = true; cleanup();
      response?.destroy(); request.destroy(); reject(error);
    };
    const resetIdle = () => {
      clearTimeout(idle);
      idle = setTimeout(() => fail(timeoutError('ETIMEDOUT')), options.idleMs ?? 8000);
    };
    resetIdle();
    deadline = setTimeout(() => fail(timeoutError('ESLOWCDN')), options.deadlineMs ?? 20000);
    speed = setInterval(() => {
      const elapsed = Date.now() - started;
      if (elapsed >= (options.graceMs ?? 5000) && received * 1000 / elapsed < (options.minimumRate ?? 32768)) {
        fail(timeoutError('ESLOWCDN'));
      }
    }, options.checkMs ?? 1000);
    request.on('error', fail);
    request.on('response', res => {
      response = res;
      if (res.statusCode !== 200) return fail(new Error('HTTP error ' + res.statusCode));
      if (Number(res.headers['content-length']) > (options.limit ?? 16 * 1024 ** 2)) return fail(new Error('Invalid chunk length'));
      res.on('error', fail);
      res.on('aborted', () => fail(new Error('CDN response interrupted')));
      res.on('data', data => {
        if (settled) return;
        received += data.length;
        if (received > (options.limit ?? 16 * 1024 ** 2)) return fail(new Error('Invalid chunk length'));
        buffers.push(data); resetIdle(); options.onBytes?.(data.length);
      });
      res.on('end', () => {
        if (settled) return;
        if (!res.complete) return fail(new Error('CDN response interrupted'));
        settled = true; cleanup();
        resolve({bytes: Buffer.concat(buffers), networkBytes: received, networkMs: Date.now() - started});
      });
    });
  });
}

class Decoder {
  constructor() { this.queue = []; this.lanes = []; this.closed = false; }
  decode(bytes, key, signal) {
    return new Promise((resolve, reject) => {
      if (this.closed) return reject(new Error('Decoder closed'));
      if (signal?.aborted) return reject(signal.reason);
      const cleanup = () => signal?.removeEventListener('abort', abort);
      const job = {bytes, key, signal, resolve: data => { cleanup(); resolve(data); }, reject: error => { cleanup(); reject(error); }};
      const abort = () => {
        this.queue = this.queue.filter(j => j !== job);
        const lane = this.lanes.find(l => l.job === job);
        if (lane) lane.fail(signal.reason); else job.reject(signal.reason);
      };
      signal?.addEventListener('abort', abort, {once: true});
      this.queue.push(job); this.pump();
    });
  }
  pump() {
    while (this.queue.length && !this.closed) {
      let lane = this.lanes.find(l => !l.job);
      if (!lane && this.lanes.length < 2) {
        lane = {worker: new Worker(path.join(__dirname, 'decode_worker.cjs')), job: null};
        this.lanes.push(lane);
        const done = error => {
          clearTimeout(lane.timer);
          lane.job?.reject(error);
          lane.job = null;
          this.lanes = this.lanes.filter(l => l !== lane);
          lane.worker.terminate(); this.pump();
        };
        lane.worker.on('error', () => done(new Error('Chunk decompression failed')));
        lane.worker.on('message', result => {
          const job = lane.job;
          if (!job) return;
          clearTimeout(lane.timer); lane.job = null;
          if (result.error) job.reject(new Error('Chunk decompression failed'));
          else job.resolve(Buffer.from(result.data));
          this.pump();
        });
        lane.fail = done;
      }
      if (!lane) return;
      lane.job = this.queue.shift();
      if (lane.job.signal?.aborted) {
        lane.job.reject(lane.job.signal.reason); lane.job = null; continue;
      }
      lane.timer = setTimeout(() => lane.fail(new Error('Chunk decompression timed out')), 60000);
      lane.worker.postMessage({bytes: lane.job.bytes, key: lane.job.key});
    }
  }
  async close() {
    this.closed = true;
    for (const job of this.queue) job.reject(new Error('Decoder closed'));
    this.queue = [];
    await Promise.all(this.lanes.map(lane => {
      clearTimeout(lane.timer); lane.job?.reject(new Error('Decoder closed'));
      return lane.worker.terminate();
    }));
  }
}

function createTransport(client, options = {}) {
  const decoder = new Decoder();
  const secureAgent = new https.Agent({keepAlive: true, maxSockets: 2, maxTotalSockets: 12});
  const plainAgent = new http.Agent({keepAlive: true, maxSockets: 2, maxTotalSockets: 12});
  const keys = new Map();
  return {
    async chunk(appID, depotID, sha, server, context = {}) {
      const keyId = appID + ':' + depotID;
      if (!keys.has(keyId)) keys.set(keyId, client.getDepotDecryptionKey(appID, depotID));
      const {key} = await keys.get(keyId);
      let token = '';
      if (server.usetokenauth == 1) token = (await client.getCDNAuthToken(appID, depotID, server.vhost || server.Host)).token;
      const url = new URL((server.https_support === 'mandatory' ? 'https://' : 'http://') + server.Host +
        `/depot/${depotID}/chunk/${sha}${token}`);
      const network = await receive(url, {Host: server.vhost || server.Host,
        'User-Agent': 'Valve/Steam HTTP Client 1.0', 'Accept-Encoding': 'identity'},
      {...options, signal: context.signal, onBytes: context.onBytes, agent: url.protocol === 'https:' ? secureAgent : plainAgent});
      context.onDecoding?.();
      const chunk = await decoder.decode(network.bytes, key, context.signal);
      return {chunk, networkBytes: network.networkBytes, networkMs: network.networkMs};
    },
    async close() { secureAgent.destroy(); plainAgent.destroy(); await decoder.close(); }
  };
}

module.exports = {receive, createTransport};
