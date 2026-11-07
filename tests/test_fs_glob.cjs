const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-fs-glob-')));
const cwd = path.join(root, 't');

for (const dir of ['a/b/c', 'd', '.hidden', 'node_modules/x']) fs.mkdirSync(path.join(cwd, dir), { recursive: true });
for (const file of ['a/one.js', 'a/b/two.ts', 'a/b/c/three.js', 'd/four.md', '.hidden/h.js', 'node_modules/x/i.js', 'top.js']) {
  fs.writeFileSync(path.join(cwd, file), '');
}
if (process.platform !== 'win32') fs.symlinkSync('a', path.join(cwd, 'link'));

const sorted = list => [...list].map(String).sort();
const posix = list => sorted(list).map(entry => entry.split(path.sep).join('/'));
const links = process.platform !== 'win32';

async function main() {
  assert.strictEqual(typeof fs.glob, 'function');
  assert.strictEqual(typeof fs.globSync, 'function');
  assert.strictEqual(typeof fs.promises.glob, 'function');
  assert.strictEqual(typeof require('node:fs/promises').glob, 'function');

  assert.deepStrictEqual(posix(fs.globSync('**/*.js', { cwd })), ['a/b/c/three.js', 'a/one.js', 'node_modules/x/i.js', 'top.js']);
  assert.deepStrictEqual(posix(fs.globSync('**/*.js', { cwd, exclude: ['node_modules/**'] })), ['a/b/c/three.js', 'a/one.js', 'top.js']);
  assert.deepStrictEqual(posix(fs.globSync(['a/*', 'd/*'], { cwd })), ['a/b', 'a/one.js', 'd/four.md']);

  const shallow = ['.', 'a', 'd', 'node_modules', 'top.js', ...(links ? ['link'] : [])].sort();
  assert.deepStrictEqual(posix(fs.globSync('**', { cwd, maxDepth: 1 })), shallow);

  const excludedByFn = posix(fs.globSync('**', { cwd, exclude: name => name === 'node_modules' }));
  assert.ok(!excludedByFn.some(entry => entry.startsWith('node_modules')));
  assert.ok(excludedByFn.includes('a/b/two.ts'));

  if (links) {
    assert.deepStrictEqual(posix(fs.globSync('link/**', { cwd, followSymlinks: true })), [
      'link', 'link/b', 'link/b/c', 'link/b/c/three.js', 'link/b/two.ts', 'link/one.js',
    ]);
  }

  const typed = fs.globSync('*', { cwd, withFileTypes: true });
  for (const entry of typed) {
    assert.ok(entry instanceof fs.Dirent, `${entry.name} is a Dirent`);
    assert.strictEqual(entry.parentPath, cwd);
  }
  const kinds = Object.fromEntries(typed.map(entry => [entry.name, [entry.isDirectory(), entry.isFile(), entry.isSymbolicLink()]]));
  assert.deepStrictEqual(kinds.a, [true, false, false]);
  assert.deepStrictEqual(kinds['top.js'], [false, true, false]);
  if (links) assert.deepStrictEqual(kinds.link, [false, false, true]);

  assert.throws(() => fs.globSync(1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => fs.globSync('*', { maxDepth: -1 }), { code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => fs.globSync('*', { exclude: 1 }), { code: 'ERR_INVALID_ARG_TYPE' });

  const viaCallback = await new Promise((resolve, reject) => {
    fs.glob('*/*.js', { cwd }, (error, found) => (error ? reject(error) : resolve(found)));
  });
  assert.deepStrictEqual(posix(viaCallback), ['a/one.js']);

  const iterated = [];
  for await (const entry of fs.promises.glob('**/*.ts', { cwd })) iterated.push(entry);
  assert.deepStrictEqual(posix(iterated), ['a/b/two.ts']);

  const absolute = fs.globSync(path.join(cwd, 'd', '*.md').split(path.sep).join('/'));
  assert.deepStrictEqual(sorted(absolute), [path.join(cwd, 'd', 'four.md')]);

  console.log('fs glob: ok');
}

main()
  .catch(error => {
    process.exitCode = 1;
    console.error(error);
  })
  .finally(() => fs.rmSync(root, { recursive: true, force: true }));
