// Offline lifecycle test only. Fixtures below are never sent to Steam/native.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('steam/tools/steam_auth_ui_login.js', 'utf8');
function body(fields) { return new Proxy({}, {get: (_, key) => () => fields[key] ?? ''}); }
function response(fields, result = 1) { return {GetEResult: () => result, Body: () => body(fields)}; }
function fixture(service) {
    const messages = [];
    const sleeps = [];
    const context = vm.createContext({
        KleptonValveRequire: id => {
            if (id === 13459) return {kX: service};
            if (id === 36500) return {A: {
                getPublicKey: (mod, exp) => { assert.equal(mod, 'fixture-mod'); assert.equal(exp, 'fixture-exp'); return {}; },
                encrypt: password => { assert.equal(password, 'fixture-password'); return 'fixture-encrypted'; }
            }};
            return {D: class {GetAnonymousServiceTransport() {return {};}}};
        },
        window: {webkit: {messageHandlers: {steamLogin: {postMessage: data => messages.push(data)}}}},
        setTimeout: (fn, delay) => { const task = {fn, delay}; sleeps.push(task); return task; },
        clearTimeout: task => { const index = sleeps.indexOf(task); if (index >= 0) sleeps.splice(index, 1); },
        Date, Error, Promise, Math
    });
    vm.runInContext(source, context);
    return {api: context.KleptonSteamLogin, messages, sleeps};
}
async function tick() { for (let i = 0; i < 12; ++i) await Promise.resolve(); }
async function main() {
    let calls = 0, polling = 0;
    const f = fixture({
        BeginAuthSessionViaQR: async () => { calls++; return response({client_id:'1', request_id: [1], challenge_url: 'https://s.team/q/fixture', interval:1}); },
        PollAuthSessionStatus: async (_, request) => {
            assert.equal(request.client_id, '1'); polling++;
            return response({refresh_token:'offline-fixture-token', account_name:'offline-fixture'});
        }
    });
    const running = f.api.start(); f.api.start(); await tick();
    assert.equal(calls, 1);
    assert(f.messages.some(m => m.kind === 'challenge'));
    assert(!f.messages.some(m => m.kind === 'credential'));
    f.sleeps.find(t => t.delay === 1000).fn(); await running;
    assert.equal(polling, 1);
    assert.equal(f.messages.filter(m => m.kind === 'credential').length, 1);
    assert(f.messages.some(m => m.stage === 'token_delivered'));
    // A late successful response after cancellation must not hand off a token.
    let resolveBegin;
    const cancelled = fixture({BeginAuthSessionViaQR: () => new Promise(resolve => {resolveBegin = resolve;})});
    const late = cancelled.api.start(); cancelled.api.cancel();
    resolveBegin(response({client_id:'1', request_id:[1], challenge_url:'https://s.team/q/fixture'}));
    await late;
    assert(!cancelled.messages.some(m => m.kind === 'challenge' || m.kind === 'credential'));
    // Server rejection terminates the flow without polling or credentials.
    const rejected = fixture({BeginAuthSessionViaQR: async () => response({}, 5)});
    await rejected.api.start();
    assert(rejected.messages.some(m => m.stage === 'failure' && m.result === 5));
    assert(!rejected.messages.some(m => m.kind === 'credential'));
    const confirmations = [body({confirmation_type:3}), body({confirmation_type:2})];
    let codeResult=65, codeCalls=0;
    const codes=fixture({
        GetPasswordRSAPublicKey: async (_, req) => { assert.equal(req.account_name,'fixture-account'); return response({publickey_mod:'fixture-mod',publickey_exp:'fixture-exp',timestamp:'1'}); },
        BeginAuthSessionViaCredentials: async (_, req) => {
            assert.equal(req.encrypted_password,'fixture-encrypted'); assert(!('password' in req));
            assert.equal(req.platform_type,1); assert.equal(req.persistence,1);
            return response({client_id:'2',request_id:[2],steamid:'3',interval:1,allowed_confirmations:confirmations});
        },
        UpdateAuthSessionWithSteamGuardCode: async (_, req) => {
            assert.equal(req.code,'ABCDE'); assert.equal(req.code_type,3); codeCalls++;
            return response({},codeResult);
        },
        PollAuthSessionStatus: async () => response({refresh_token:'offline-fixture-token',account_name:'offline-fixture'})
    });
    const flow=codes.api.startCredentials('fixture-account','fixture-password'); await tick();
    assert(codes.messages.some(m=>m.kind==='guard_options' && m.device && m.email));
    await codes.api.submitGuardCode('bad',3); await codes.api.submitGuardCode('ABCDE',6);
    assert.equal(codeCalls,0); // malformed and unoffered guard methods are refused
    await codes.api.submitGuardCode('ABCDE',3);
    assert(codes.messages.some(m=>m.stage==='code_rejected' && m.result===65));
    codeResult=1; await codes.api.submitGuardCode('ABCDE',3);
    assert(codes.messages.some(m=>m.stage==='code_accepted'));
    codes.sleeps.find(t=>t.delay===1000).fn(); await flow;
    assert.equal(codes.messages.filter(m=>m.kind==='credential').length,1);
    assert(!codes.messages.some(m=>'password' in m || 'code' in m));
    // Cancelling a credential begin must ignore its later successful reply.
    let lateCredentials;
    const cancelCodes=fixture({
        GetPasswordRSAPublicKey: async () => response({publickey_mod:'fixture-mod',publickey_exp:'fixture-exp',timestamp:'1'}),
        BeginAuthSessionViaCredentials: () => new Promise(resolve=>{lateCredentials=resolve;})
    });
    const codeFlow=cancelCodes.api.startCredentials('fixture-account','fixture-password'); await tick();
    cancelCodes.api.cancel(); lateCredentials(response({client_id:'2',request_id:[2],steamid:'3',interval:1,allowed_confirmations:confirmations}));
    await codeFlow;
    assert(!cancelCodes.messages.some(m=>m.kind==='guard_options' || m.kind==='credential'));
    console.log('Steam QR and credential/code login, single-flight, encryption handoff, code rejection and cancellation passed (offline fixtures)');
}
main().catch(() => { console.error('Steam QR lifecycle regression failed'); process.exitCode = 1; });
