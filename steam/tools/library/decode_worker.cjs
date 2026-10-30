'use strict';
const {parentPort} = require('node:worker_threads');
const SteamCrypto = require('@doctormckay/steam-crypto');
// The compression adapter is internal to the exactly pinned steam-user 5.3.0.
// Keeping it in workers prevents pure-JS LZMA from blocking network watchdogs.
const compression = require('steam-user/components/cdn_compression.js');
parentPort.on('message', async ({bytes, key}) => {
  try {
    const data = await compression.unzip(SteamCrypto.symmetricDecrypt(Buffer.from(bytes), Buffer.from(key)));
    parentPort.postMessage({data});
  } catch { parentPort.postMessage({error: true}); }
});
