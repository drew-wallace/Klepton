'use strict';

// Performance is measured on compressed network bytes, before unpacking, so
// CPU-heavy LZMA chunks do not cause us to penalize a healthy content server.
class CDNPool {
  constructor(candidates, options = {}) {
    const secure = candidates.filter(s => ['optional', 'mandatory'].includes(s.https_support));
    const seen = new Set();
    this.servers = (secure.length ? secure : candidates).filter(s => {
      if (!s.Host || seen.has(s.Host)) return false;
      seen.add(s.Host); return true;
    }).slice(0, 20).map((server, index) => ({
      server: ['optional', 'mandatory'].includes(server.https_support) ? {...server, https_support: 'mandatory'} : server,
      index, active: 0, samples: 0, rate: 0, cooldown: 0, failures: 0
    }));
    if (!this.servers.length) throw new Error('No content servers available');
    this.now = options.now || Date.now;
    this.cooldownMs = options.cooldownMs ?? 30000;
    this.perHost = options.perHost ?? 2;
    this.assignments = 0;
    this.switches = 0;
  }
  async acquire(signal) {
    while (!signal?.aborted) {
      const candidates = this.servers.filter(s => s.active < this.perHost && s.cooldown <= this.now());
      if (candidates.length) {
        const unexplored = candidates.filter(s => s.samples === 0 && s.failures === 0);
        const known = candidates.filter(s => s.samples > 0);
        // Explore the initial candidates, then favor measured throughput. Keep
        // sampling alternatives periodically if the current servers slow down.
        const choices = unexplored.length && (!known.length || this.assignments++ % 12 === 0) ? unexplored : candidates;
        choices.sort((a, b) => (b.rate / (b.active + 1)) - (a.rate / (a.active + 1)) || a.active - b.active || a.index - b.index);
        const selected = choices[0];
        selected.active++;
        return selected;
      }
      // If every host is cooling down, retry the earliest one after a short
      // pause rather than waiting for a whole cooldown or abandoning the pool.
      if (this.servers.every(s => s.cooldown > this.now())) {
        await new Promise(resolve => setTimeout(resolve, 200));
        this.servers.reduce((a, b) => a.cooldown < b.cooldown ? a : b).cooldown = this.now();
      } else await new Promise(resolve => setTimeout(resolve, 20));
    }
    throw signal.reason;
  }
  release(host, result, error) {
    host.active--;
    if (error) {
      host.failures++;
      host.cooldown = this.now() + this.cooldownMs;
      this.switches++;
      return;
    }
    const rate = (result.networkBytes ?? result.chunk.length) * 1000 / Math.max(1, result.networkMs ?? 1);
    host.rate = host.samples ? host.rate * .6 + rate * .4 : rate;
    host.samples++;
    const best = Math.max(...this.servers.map(s => s.rate));
    if (host.samples >= 2 && host.rate < best / 4) {
      host.cooldown = this.now() + this.cooldownMs;
      this.switches++;
    }
  }
  get activeHosts() { return this.servers.filter(s => s.active).length; }
}

module.exports = {CDNPool};
