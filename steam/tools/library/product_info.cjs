'use strict';

const crypto = require('node:crypto');
const {gunzipSync} = require('node:zlib');
const EMsg = require('steam-user/enums/EMsg.js');

const MAX_BYTES = 32 * 1024 * 1024;

async function hydrateAppInfo(body, fetchInfo = fetch) {
  for (const app of body.apps || []) {
    if (app.buffer?.length) continue;
    // Steam redirects large appinfo records to its CDN. steam-user 5.3.0
    // assumes every response has an inline buffer and otherwise throws.
    if (app.missing_token) {
      // Preserve missingToken so steam-user can acquire a token and retry.
      app.buffer = Buffer.from('"appinfo" {}\0');
      continue;
    }
    if (!app.sha?.length) throw new Error('Missing Steam app metadata');
    // Some CM responses omit http_host; the canonical appinfo CDN serves the
    // same hash-addressed records. Use SteamKit's /appinfo/ID/sha/HASH.txt.gz format.
    const host = body.http_host || 'clientconfig.steamstatic.com';
    if (!/^(?:[a-z0-9-]+\.)*(?:steamstatic\.com|steamcontent\.com|steampowered\.com)$/i.test(host) ||
        !Number.isInteger(app.appid) || app.appid <= 0 || app.sha.length !== 20) {
      throw new Error('Invalid Steam metadata location');
    }
    const sha = Buffer.from(app.sha).toString('hex');
    const response = await fetchInfo(`https://${host}/appinfo/${app.appid}/sha/${sha}.txt.gz`,
      {signal: AbortSignal.timeout(20000), redirect: 'error'});
    if (!response.ok) throw new Error('Steam metadata download failed');
    const parts = [];
    let size = 0;
    for await (const part of response.body) {
      size += part.length;
      if (size > MAX_BYTES) throw new Error('Steam metadata too large');
      parts.push(part);
    }
    const bytes = Buffer.concat(parts);
    // Some HTTP clients decode Content-Encoding before returning the body.
    let text = bytes[0] === 0x1f && bytes[1] === 0x8b ? gunzipSync(bytes, {maxOutputLength: MAX_BYTES}) : bytes;
    if (text.length > MAX_BYTES) throw new Error('Steam metadata too large');
    if (text.at(-1) === 0) text = text.subarray(0, -1);
    if (crypto.createHash('sha1').update(text).digest('hex') !== sha) throw new Error('Steam metadata checksum mismatch');
    app.buffer = Buffer.concat([text, Buffer.from([0])]);
  }
  return body;
}

function installProductInfoTransport(client, fetchInfo) {
  const send = client._send;
  client._send = function(header, body, callback) {
    if ((typeof header === 'object' ? header.msg : header) !== EMsg.ClientPICSProductInfoRequest || !callback) {
      return send.call(this, header, body, callback);
    }
    let queue = Promise.resolve();
    return send.call(this, header, {...body, meta_data_only: false}, (...args) => {
      queue = queue.then(async () => {
        await hydrateAppInfo(args[0], fetchInfo);
        await callback.apply(this, args);
      }).catch(error => {
        if (['Missing Steam app metadata', 'Invalid Steam metadata location', 'Steam metadata download failed',
          'Steam metadata too large', 'Steam metadata checksum mismatch'].includes(error.message)) error.safeReason = error.message;
        this.emit('error', error);
      });
    });
  };
}

module.exports = {hydrateAppInfo, installProductInfoTransport};
