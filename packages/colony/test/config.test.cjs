const assert = require('node:assert/strict');
const { mkdtempSync, rmSync, writeFileSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { test } = require('node:test');
const { loadColonyToml } = require('../dist/config');

function withConfig(source, run) {
  const dir = mkdtempSync(join(tmpdir(), 'colony-config-'));
  try {
    writeFileSync(join(dir, 'colony.toml'), source);
    run(dir);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

test('loads a complete colony.toml', () => {
  withConfig(
    `name = "Example"
main = "src/server.js"

[observability]
enabled = true

[vars]
MESSAGE = "hello # colony"
RETRIES = 3

[[kv]]
binding = "CACHE"

[[sql]]
binding = "DB"
name = "app-db"
migrations_dir = "schema"

[assets]
directory = "public"
not_found_handling = "single-page-application"
start_ant = ["/api/*", "/admin/*"]
`,
    dir => {
      assert.deepEqual(loadColonyToml(dir), {
        name: 'example',
        main: 'src/server.js',
        observability: true,
        vars: { MESSAGE: 'hello # colony', RETRIES: '3' },
        bindings: [
          { kind: 'kv', binding: 'CACHE', name: 'cache' },
          { kind: 'sql', binding: 'DB', name: 'app-db', migrationsDir: 'schema' }
        ],
        assets: {
          directory: 'public',
          notFound: 'single-page-application',
          startAnt: ['/api/*', '/admin/*']
        }
      });
    }
  );
});

test('rejects duplicate bindings', () => {
  withConfig(
    `name = "example"
[[kv]]
binding = "DATA"
[[sql]]
binding = "DATA"
`,
    dir => assert.throws(() => loadColonyToml(dir), /duplicate binding: DATA/)
  );
});

test('rejects store names with uppercase or spaces', () => {
  withConfig(
    `name = "example"
[[kv]]
binding = "CACHE"
name = "My Cache"
`,
    dir => assert.throws(() => loadColonyToml(dir), /kv\[0\]\.name/)
  );
});

test('rejects names that cannot be used as ants.page hostnames', () => {
  withConfig('name = "not a hostname"\n', dir => {
    assert.throws(() => loadColonyToml(dir), /project name must be a valid hostname label/);
  });
});

test('reports malformed TOML with the config path', () => {
  withConfig('name = "unterminated\n', dir => {
    assert.throws(() => loadColonyToml(dir), new RegExp(`could not parse ${dir.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}`));
  });
});

test('files bindings take no name and always mean the account store', () => {
  const { mkdtempSync, writeFileSync, rmSync } = require('node:fs');
  const { tmpdir } = require('node:os');
  const { join } = require('node:path');
  const { loadColonyToml } = require('../dist/config');
  const dir = mkdtempSync(join(tmpdir(), 'colony-files-'));
  try {
    writeFileSync(join(dir, 'colony.toml'), 'name = "app"\n[[files]]\nbinding = "FILES"\n');
    assert.deepEqual(loadColonyToml(dir).bindings, [{ kind: 'files', binding: 'FILES', name: 'files' }]);
    writeFileSync(join(dir, 'colony.toml'), 'name = "app"\n[[files]]\nbinding = "FILES"\nname = "mine"\n');
    assert.throws(() => loadColonyToml(dir), /takes no name/);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

test('assets with no main and no server.js is a static site', () => {
  const { mkdtempSync, writeFileSync, rmSync } = require('node:fs');
  const { tmpdir } = require('node:os');
  const { join } = require('node:path');
  const { loadColonyToml } = require('../dist/config');
  const dir = mkdtempSync(join(tmpdir(), 'colony-static-'));
  try {
    writeFileSync(join(dir, 'colony.toml'), 'name = "site"\n[assets]\ndirectory = "./dist"\n');
    assert.equal(loadColonyToml(dir).main, null);
    writeFileSync(join(dir, 'server.js'), 'export default {}');
    assert.equal(loadColonyToml(dir).main, 'server.js');
    writeFileSync(join(dir, 'colony.toml'), 'name = "app"\n');
    assert.equal(loadColonyToml(dir).main, 'server.js');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});
