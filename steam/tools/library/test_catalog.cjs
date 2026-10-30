'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {gameInfo, revision, safePath} = require('./catalog.cjs');

function fixture() {
  return {common: {name: 'Example VR', categories: {category_54: '1'}},
    config: {launch: {0: {executable: 'game.apk', config: {oslist: 'android'}},
      1: {executable: 'other.apk', description: 'Alternative', config: {oslist: 'android'}}}},
    depots: {100: {config: {oslist: 'android', osarch: '64'}, manifests: {public: {gid: '18446744073709551614'}, beta: {gid: '9000000000000000001'}}},
      101: {config: {oslist: 'windows'}, manifests: {public: {gid: '123'}}},
      102: {config: {}, manifests: {public: {gid: '1234'}}},
      103: {config: {language: 'german'}, manifests: {public: {gid: '1235'}}},
      branches: {public: {buildid: '1000', timeupdated: '1700000000'}, beta: {buildid: '1001'}, secret: {pwdrequired: '1'}}}};
}
const licenses = new Set([100, 101, 102, 103]);

test('identifies VR Android games, multiple launches and exact string manifest IDs', () => {
  const game = gameInfo(10, fixture(), licenses);
  assert.equal(game.vr, true);
  assert.equal(game.android_available, true);
  assert.equal(game.launches.length, 2);
  assert.deepEqual(game.branches[0].depots.map(d => d.depot_id), ['100', '102']);
  assert.equal(game.branches[0].depots[0].manifest_id, '18446744073709551614');
  assert.equal(game.branches[0].updated_at, '2023-11-14T22:13:20.000Z');
});
test('does not infer VR from SteamVR collectibles or ordinary desktop launches', () => {
  const info = fixture(); info.common.categories = {category_40: '1'};
  assert.equal(gameInfo(10, info, licenses).vr, false);
});
test('licensed game with inaccessible Android depot is unavailable', () => {
  assert.equal(gameInfo(10, fixture(), new Set([101, 102])).android_available, false);
  const info = fixture(); delete info.depots[102];
  assert.equal(gameInfo(10, info, new Set([101])).android_available, false);
});
test('branches retain protected label, public default, and unknown dates', () => {
  const game = gameInfo(10, fixture(), licenses);
  assert.equal(game.branches[0].name, 'public');
  assert.equal(game.branches.find(b => b.name === 'secret').locked, true);
  assert.equal(game.branches.find(b => b.name === 'secret').available, false);
  assert.equal(game.branches.find(b => b.name === 'beta').updated_at, null);
});
test('desktop-only build changes do not mark Android content out of date', () => {
  const info = fixture();
  const original = gameInfo(10, info, licenses).branches[0];
  info.depots.branches.public.buildid = '9999';
  info.depots[101].manifests.public.gid = '5678';
  const desktopUpdate = gameInfo(10, info, licenses).branches[0];
  assert.notEqual(original.build_id, desktopUpdate.build_id);
  assert.equal(original.revision, desktopUpdate.revision);
  info.depots[100].manifests.public.gid = '18446744073709551613';
  assert.notEqual(original.revision, gameInfo(10, info, licenses).branches[0].revision);
});
test('manifest order does not change revision and rounded numeric IDs are rejected', () => {
  assert.equal(revision([{depot_id:'2', manifest_id:'3'}, {depot_id:'1', manifest_id:'4'}]),
    revision([{depot_id:'1', manifest_id:'4'}, {depot_id:'2', manifest_id:'3'}]));
  const info = fixture(); info.depots[100].manifests.public.gid = 18446744073709551614;
  assert.deepEqual(gameInfo(10, info, licenses).branches[0].depots.map(d => d.depot_id), ['102']);
});
test('missing launch metadata cannot advertise a usable APK', () => {
  const info = fixture(); delete info.config;
  assert.equal(gameInfo(10, info, licenses).android_available, false);
  assert.equal(gameInfo(10, {}, new Set()).vr, false);
});
test('rejects traversal, absolute paths and unsafe APK launch paths', () => {
  for (const value of ['../escape', '/abs.apk', 'C:\\escape', 'a/../b', 'a//b', '\u0000.apk', '.']) {
    assert.throws(() => safePath(value));
  }
  assert.equal(safePath('obb\\content.obb'), 'obb/content.obb');
  const info = fixture(); info.config.launch[0].executable = '../bad.apk';
  assert.deepEqual(gameInfo(10, info, licenses).launches.map(l => l.id), ['1']);
});
test('shared depot uses source application for CDN authorization', () => {
  const info = fixture(); info.depots[102].depotfromapp = '500';
  assert.equal(gameInfo(10, info, licenses).branches[0].depots[1].app_id, '500');
});
