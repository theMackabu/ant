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
placement = "smart"

[observability]
enabled = true

[vars]
MESSAGE = "hello # colony"
RETRIES = 3

[[kv]]
binding = "CACHE"
id = "kv_1"

[[sql]]
binding = "DB"
id = "sql_1"
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
        placement: 'smart',
        observability: true,
        vars: { MESSAGE: 'hello # colony', RETRIES: '3' },
        bindings: [
          { kind: 'kv', binding: 'CACHE', id: 'kv_1', name: undefined },
          { kind: 'sql', binding: 'DB', id: 'sql_1', name: undefined, migrationsDir: 'schema' }
        ],
        assets: {
          binding: 'ASSETS',
          name: undefined,
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
id = "kv_1"
[[sql]]
binding = "DATA"
id = "sql_1"
`,
    dir => assert.throws(() => loadColonyToml(dir), /duplicate binding: DATA/)
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
