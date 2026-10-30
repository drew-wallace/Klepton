'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

async function run(scenario = '', action = 'download', auth = {}) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'klepton-steam-test-'));
  const events = [];
  const child = spawn(process.execPath, ['--require', path.join(__dirname, 'fixtures/fake_steam.cjs'), path.join(__dirname, 'helper.cjs')],
    {env: {...process.env, KLEPTON_TEST_SCENARIO: scenario}});
  let pending = '', stderr = '';
  const timeout = setTimeout(() => child.kill('SIGKILL'), 10000);
  child.stdout.on('data', data => {
    pending += data;
    while (pending.includes('\n')) {
      const end = pending.indexOf('\n');
      const event = JSON.parse(pending.slice(0, end)); pending = pending.slice(end + 1);
      events.push(event);
      if (event.event === 'auth' && event.kind === 'guard') child.stdin.write(JSON.stringify({guard_code: '12345'}) + '\n');
    }
  });
  child.stderr.on('data', data => { stderr += data; });
  child.stdin.write(JSON.stringify({action, refresh_token: 'fixture-token', app_id: '10', branch: 'public', launch_id: '0', destination: directory, ...auth})+'\n');
  const code = await new Promise(resolve => child.on('exit', resolve));
  clearTimeout(timeout);
  return {code, events, stderr, directory, cleanup: () => fs.rm(directory, {recursive: true, force: true})};
}

test('downloads manifest files, empty files, OBBs and SHA-256 receipt inventory', async () => {
  const result = await run();
  try {
    assert.equal(result.code, 0);
    assert.equal(result.stderr, '');
    const output = result.events.find(e => e.event === 'result');
    assert.equal(output.files.length, 4);
    assert.equal(await fs.readFile(path.join(result.directory, 'obb/content.obb'), 'utf8'), 'data');
    assert.equal(output.branch.depots[0].manifest_id, '18446744073709551614');
    assert.equal(output.files[0].sha256.length, 64);
    assert.equal(result.events.some(e => e.event === 'progress'), true);
  } finally { await result.cleanup(); }
});

for (const [scenario, code] of [['unsafe','unsafe_path'], ['no_license','no_license'], ['missing_apk','missing_apk'],
  ['collision','file_collision'], ['checksum','checksum'], ['connection','steam_failed'], ['manifest','steam_failed'],
  ['cdn_timeout', 'steam_failed'], ['cdn_http', 'steam_failed'], ['cdn_private_error', 'steam_failed']]) {
  test(`fails safely on ${scenario} without publishing success or logging library exceptions`, async () => {
    const result = await run(scenario);
    try {
      assert.equal(result.code, 1);
      assert.equal(result.events.some(e => e.event === 'result'), false);
      assert.equal(result.events.find(e => e.event === 'error').code, code);
      assert.equal(result.stderr, '');
      assert.equal(JSON.stringify(result.events).includes('sensitive-'), false);
      if (scenario === 'cdn_timeout') assert.equal(result.events.find(e => e.event === 'error').reason, 'CDN request timed out');
      if (scenario === 'cdn_http') assert.equal(result.events.find(e => e.event === 'error').reason, 'HTTP 503');
    } finally { await result.cleanup(); }
  });
}
test('QR authentication and library discovery use private credential events', async () => {
  const result = await run('', 'login', {refresh_token: null, auth_method: 'qr'});
  try {
    assert.equal(result.code, 0);
    assert.equal(result.events.find(e => e.event === 'auth').kind, 'qr');
    assert.equal(result.events.filter(e => e.event !== 'credential').some(e => JSON.stringify(e).includes('private-fixture')), false);
    assert.equal(result.events.find(e => e.event === 'result').games.length, 1);
  } finally { await result.cleanup(); }
});
test('password login accepts interactive Guard reply over stdin', async () => {
  const result = await run('', 'login', {refresh_token: null, auth_method: 'password', account_name: 'fixture', password: 'private-password'});
  try {
    assert.equal(result.code, 0);
    assert.equal(result.events.find(e => e.event === 'auth').kind, 'guard');
    assert.equal(JSON.stringify(result.events).includes('private-password'), false);
  } finally { await result.cleanup(); }
});
test('expired authentication produces a retryable error', async () => {
  const result = await run('expired', 'login', {refresh_token: null, auth_method: 'qr'});
  try {
    assert.equal(result.code, 1);
    assert.equal(result.events.find(e => e.event === 'error').code, 'expired');
  } finally { await result.cleanup(); }
});
test('flushes a large library result before helper exit', async () => {
  const result = await run('large', 'refresh');
  try {
    assert.equal(result.code, 0);
    assert.equal(result.events.find(e => e.event === 'result').games.length, 1500);
  } finally { await result.cleanup(); }
});

test('scoped refresh requests matching and newly owned apps only', async () => {
  const result = await run('scoped', 'refresh', {steam_id: '76561198000000001',
    app_ids: ['10', '99'], known_app_ids: ['10', '11', '99']});
  try {
    assert.equal(result.code, 0);
    const output = result.events.find(e => e.event === 'result');
    assert.deepEqual(output.games.map(g => g.app_id), ['10', '12']);
    assert.deepEqual(output.checked_app_ids, ['10', '12']);
    assert.deepEqual(output.owned_app_ids, ['10', '11', '12']);
    assert.equal(result.events.find(e => e.kind === 'library').total, 2);
  } finally { await result.cleanup(); }
});

test('empty shortlist avoids all app metadata requests when ownership is unchanged', async () => {
  const result = await run('large', 'refresh', {steam_id: '76561198000000001',
    app_ids: [], known_app_ids: Array.from({length: 1500}, (_, i) => String(i + 10))});
  try {
    assert.equal(result.code, 0);
    const output = result.events.find(e => e.event === 'result');
    assert.deepEqual(output.games, []);
    assert.deepEqual(output.checked_app_ids, []);
    assert.equal(output.owned_app_ids.length, 1500);
    assert.equal(result.events.some(e => e.kind === 'library'), false);
  } finally { await result.cleanup(); }
});

test('shortlists from another account cannot suppress library discovery', async () => {
  const result = await run('', 'login', {steam_id: '76561198000000002', app_ids: [], known_app_ids: ['10']});
  try {
    assert.equal(result.code, 0);
    const output = result.events.find(e => e.event === 'result');
    assert.equal(output.games.length, 1);
    assert.equal(output.checked_app_ids, undefined);
  } finally { await result.cleanup(); }
});
