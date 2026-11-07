import assert from 'node:assert';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const fixture = path.join(path.dirname(fileURLToPath(import.meta.url)), 'fixtures', 'tsconfig-paths');

const { results } = await import(path.join(fixture, 'main.ts'));
assert.deepStrictEqual(results, {
  pipeline: 'pipeline',
  exact: 'exact',
  a: 'lib-a',
  b: 'deep-b',
  dir: 'dir-index',
  tsExt: 'ts-ext',
  odd: 'odd',
  required: 'lib-a:exact',
  plain: 'exact',
  cond: 'import',
});

const { greet } = await import(path.join(fixture, 'inherit', 'main.ts'));
assert.strictEqual(greet, 'base-root');

const { loop } = await import(path.join(fixture, 'cycle', 'main.ts'));
assert.strictEqual(loop, 'loop');

const require = createRequire(path.join(fixture, 'main.ts'));
assert.strictEqual(require.resolve('@exact'), path.join(fixture, 'src', 'exact.ts'));
assert.strictEqual(require.resolve('~/dir'), path.join(fixture, 'src', 'dir', 'index.ts'));

assert.throws(() => require.resolve('~/nope'), /nope/);
assert.throws(() => createRequire(import.meta.url).resolve('@exact'), /@exact/);

console.log('ok');
