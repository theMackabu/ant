const assert = require('node:assert/strict');
const { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { test } = require('node:test');
const { collectAssets, collectMigrations } = require('../dist/build');

function withTempDir(run) {
  const dir = mkdtempSync(join(tmpdir(), 'colony-build-'));
  try {
    run(dir);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

test('collects assets in deterministic URL order', () => {
  withTempDir(dir => {
    mkdirSync(join(dir, 'nested'));
    writeFileSync(join(dir, 'z.txt'), 'last');
    writeFileSync(join(dir, 'a.html'), '<h1>first</h1>');
    writeFileSync(join(dir, 'nested', 'app.js'), 'export default 1');

    const assets = collectAssets(dir);
    assert.deepEqual(
      assets.map(asset => [asset.path, asset.ct]),
      [
        ['/a.html', 'text/html; charset=utf-8'],
        ['/nested/app.js', 'text/javascript; charset=utf-8'],
        ['/z.txt', 'text/plain; charset=utf-8']
      ]
    );
  });
});

test('rejects asset symlinks', () => {
  withTempDir(dir => {
    writeFileSync(join(dir, 'target.txt'), 'target');
    symlinkSync(join(dir, 'target.txt'), join(dir, 'link.txt'));
    assert.throws(() => collectAssets(dir), /asset symlinks are not supported: \/link.txt/);
  });
});

test('sorts SQL migrations and rejects missing directories', () => {
  withTempDir(dir => {
    writeFileSync(join(dir, '002_second.sql'), 'select 2;');
    writeFileSync(join(dir, '001_first.sql'), 'select 1;');
    writeFileSync(join(dir, 'notes.txt'), 'ignored');
    assert.deepEqual(collectMigrations(dir), [
      { tag: '001_first', sql: 'select 1;' },
      { tag: '002_second', sql: 'select 2;' }
    ]);
    assert.throws(() => collectMigrations(join(dir, 'missing')), /migrations directory not found/);
  });
});
