'use strict';

const fs = require('node:fs/promises');
const {createReadStream} = require('node:fs');
const crypto = require('node:crypto');
const {CDNPool} = require('./cdn_pool.cjs');

// steam-user's whole-file downloader consumes a finite pool of servers on
// retries. A large APK can exhaust that pool after unrelated transient errors.
// Retry each verified chunk independently, without discarding successful work.
async function downloadFile(client, appID, depotID, file, output, progress, options = {}) {
  const size = Number(file.size);
  const chunks = [...(file.chunks || [])].sort((a, b) => Number(a.offset) - Number(b.offset));
  let end = 0;
  if (!Number.isSafeInteger(size) || size < 0) throw new Error('Invalid manifest file size');
  for (const chunk of chunks) {
    const offset = Number(chunk.offset), length = Number(chunk.cb_original);
    if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(length) || offset !== end || length <= 0 || !/^[a-f0-9]{40}$/i.test(chunk.sha)) {
      throw new Error('Invalid manifest chunk layout');
    }
    end += length;
  }
  if (end !== size) throw new Error('Invalid manifest chunk coverage');
  if (size === 0) {
    await fs.writeFile(output, Buffer.alloc(0), {mode: 0o600});
    progress(0);
    return;
  }
  const pool = options.pool || new CDNPool((await client.getContentServers(appID)).servers);
  const controller = new AbortController();
  let cursor = 0, completed = 0, failed = false, networkBytes = 0, receiving = 0, decoding = 0;
  let lastAdvance = Date.now(), heartbeat;
  const samples = [{time: Date.now(), bytes: 0, completed: 0}];
  const report = () => {
    const now = Date.now();
    samples.push({time: now, bytes: networkBytes, completed});
    while (samples.length > 2 && samples[1].time < now - 10000) samples.shift();
    const seconds = Math.max(1, (now - samples[0].time) / 1000);
    const verifiedRate = (completed - samples[0].completed) / seconds;
    options.onStatus?.({network_bps: Math.round((networkBytes - samples[0].bytes) / seconds), verified_bps: verifiedRate,
      eta_seconds: verifiedRate > 0 ? Math.ceil((size - completed) / verifiedRate) : null,
      stalled_seconds: Math.floor((now - lastAdvance) / 1000), active_hosts: pool.activeHosts,
      switches: pool.switches, phase: receiving ? 'receiving' : decoding ? 'unpacking' : 'verifying'});
    progress(completed);
    if (now - lastAdvance > (options.stallMs ?? 90000) && !controller.signal.aborted) {
      controller.abort(new Error('Download made no verified progress'));
    }
  };
  const handle = await fs.open(output, 'w', 0o600);
  try {
    await handle.truncate(size);
    heartbeat = setInterval(report, options.heartbeatMs ?? 1000);
    async function worker() {
      while (!failed && !controller.signal.aborted && cursor < chunks.length) {
        const chunk = chunks[cursor++];
        let data, lastError;
        for (let attempt = 0; attempt < (options.attempts || 6); attempt++) {
          const host = await pool.acquire(controller.signal);
          let unpacking = false;
          receiving++;
          try {
            const start = Date.now();
            const result = options.transport ? await options.transport.chunk(appID, depotID, chunk.sha, host.server, {
              signal: controller.signal,
              onBytes: bytes => { networkBytes += bytes; },
              onDecoding: () => { unpacking = true; receiving--; decoding++; }
            }) : await client.downloadChunk(appID, depotID, chunk.sha, host.server);
            if (controller.signal.aborted) throw controller.signal.reason;
            data = result.chunk;
            if (data.length !== Number(chunk.cb_original) || crypto.createHash('sha1').update(data).digest('hex') !== chunk.sha.toLowerCase()) {
              throw new Error('Checksum mismatch');
            }
            pool.release(host, {...result, networkMs: result.networkMs ?? Date.now() - start});
            lastError = null;
            break;
          } catch (error) {
            lastError = error;
            pool.release(host, null, error);
            report();
            if (controller.signal.aborted) throw controller.signal.reason;
            if (attempt < (options.attempts || 6) - 1) {
              await new Promise(resolve => setTimeout(resolve, (options.retryDelay ?? 200) * 2 ** attempt));
            }
          } finally {
            if (unpacking) decoding--; else receiving--;
          }
        }
        if (lastError) throw lastError;
        let written = 0;
        while (written < data.length) {
          const result = await handle.write(data, written, data.length - written, Number(chunk.offset) + written);
          if (!result.bytesWritten) throw new Error('File write failed');
          written += result.bytesWritten;
        }
        completed += data.length;
        lastAdvance = Date.now();
        report();
      }
      if (controller.signal.aborted) throw controller.signal.reason;
    }
    const workers = Array.from({length: Math.min(options.concurrency ?? 6, chunks.length)}, () => worker().catch(error => {
      failed = true; controller.abort(error); throw error;
    }));
    const results = await Promise.allSettled(workers);
    const failure = results.find(r => r.status === 'rejected');
    if (failure) throw failure.reason;
    await handle.sync();
  } finally { clearInterval(heartbeat); await handle.close(); }
  if (size > 0) {
    const hash = crypto.createHash('sha1');
    for await (const chunk of createReadStream(output)) hash.update(chunk);
    if (hash.digest('hex') !== file.sha_content) throw new Error('File checksum mismatch');
  }
}

module.exports = {downloadFile};
