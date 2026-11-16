// Offline integration check against packaged, receipt-verified Valve modules.
// Usage: node steam/tests/t_steam_auth_ui_assets.cjs /path/to/SteamAuthAssets
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(process.argv[2]);
const context = vm.createContext({
    URL, TextEncoder, TextDecoder, Uint8Array, ArrayBuffer, FormData, Blob,
    AbortController, Buffer, setTimeout: (...args) => setTimeout(...args).unref(), clearTimeout,
    navigator: {appName: 'Netscape'},
    // Valve's module initialization uses intervals; no background work runs.
    setInterval: () => 0, clearInterval: () => {},
    fetch: () => { throw new Error('Network access forbidden in offline test'); }
});
vm.runInContext(`globalThis.window=globalThis;globalThis.self=globalThis;
location=new URL('https://steamloopback.host/');origin=location.origin;
localStorage={getItem:()=>null,setItem:()=>{},removeItem:()=>{}};
sessionStorage=localStorage;document={cookie:'',getElementById:()=>null};`, context);
const html = fs.readFileSync(path.join(root, 'index.html'), 'utf8');
for (const [, name] of html.matchAll(/<script src="([^"]+)"><\/script>/g)) {
    if (name !== 'probe.js') vm.runInContext(fs.readFileSync(path.join(root, name), 'utf8'), context);
}
const requireValve = context.KleptonValveRequire;
const auth = requireValve(13459).kX;
const web = new (requireValve(79853).D)('https://api.steampowered.com/', undefined);
assert.equal(typeof web.GetAnonymousServiceTransport().SendMsg, 'function');
const requests = [];
const transport = {SendMsg: (name, request) => {
    requests.push({name, body: request.Body().toObject()});
    return Promise.resolve(null);
}};
auth.BeginAuthSessionViaQR(transport, {device_friendly_name: 'offline-fixture', platform_type: 1});
auth.PollAuthSessionStatus(transport, {client_id: '1', request_id: [1]});
auth.GetPasswordRSAPublicKey(transport, {account_name: 'offline-fixture'});
auth.BeginAuthSessionViaCredentials(transport, {account_name: 'offline-fixture', encrypted_password: 'ciphertext'});
auth.UpdateAuthSessionWithSteamGuardCode(transport, {client_id: '1', steamid: '2', code: 'ABCDE', code_type: 3});
assert.deepEqual(requests.map(r => r.name), [
    'Authentication.BeginAuthSessionViaQR#1', 'Authentication.PollAuthSessionStatus#1',
    'Authentication.GetPasswordRSAPublicKey#1', 'Authentication.BeginAuthSessionViaCredentials#1',
    'Authentication.UpdateAuthSessionWithSteamGuardCode#1'
]);
assert.equal(requests[0].body.device_friendly_name, 'offline-fixture');
assert.equal(requests[1].body.client_id, '1');
assert.equal(requests[2].body.account_name, 'offline-fixture');
assert.equal(requests[3].body.encrypted_password, 'ciphertext');
assert.equal(requests[4].body.code, 'ABCDE');
const {publicKey, privateKey} = crypto.generateKeyPairSync('rsa', {modulusLength: 2048});
const jwk = publicKey.export({format: 'jwk'});
const rsa = requireValve(36500).A;
const key = rsa.getPublicKey(Buffer.from(jwk.n, 'base64url').toString('hex'), Buffer.from(jwk.e, 'base64url').toString('hex'));
const ciphertext = rsa.encrypt('offline-password', key);
assert.equal(typeof ciphertext, 'string');
const decoded = crypto.privateDecrypt({key: privateKey, padding: crypto.constants.RSA_NO_PADDING}, Buffer.from(ciphertext, 'base64'));
assert.equal(decoded[0], 0);
assert.equal(decoded[1], 2);
const separator = decoded.indexOf(0, 2);
assert(separator >= 10);
assert.equal(decoded.subarray(separator + 1).toString(), 'offline-password');
console.log('Verified genuine Valve QR, polling, credential/code request builders, HTTPS transport and RSA handoff offline.');
