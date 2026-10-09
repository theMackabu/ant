const assert = require('node:assert');
const fs = require('node:fs');
const fsp = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-readdir-recursive-'));
const R = (value) => String(value).split(root).join('R');
const dirent = (entry) => `${entry.name}|${R(entry.parentPath)}|${entry.isDirectory() ? 'D' : entry.isSymbolicLink() ? 'L' : 'F'}`;

const names = ['a', 'a/b', 'a/b/h', 'a/g', 'dangling', 'empty', 'f', 'flnk', 'lnk', 'lnk/b', 'lnk/b/h', 'lnk/g'].map(
  (name) => name.split('/').join(path.sep)
);
const dirents = [
  'a|R|D', 'b|R/a|D', 'b|R/lnk|D', 'dangling|R|L', 'empty|R|D', 'flnk|R|L',
  'f|R|F', 'g|R/a|F', 'g|R/lnk|F', 'h|R/a/b|F', 'h|R/lnk/b|F', 'lnk|R|L',
].map((entry) => entry.split('/').join(path.sep));

async function main() {
  fs.mkdirSync(path.join(root, 'a', 'b'), { recursive: true });
  fs.mkdirSync(path.join(root, 'empty'));
  fs.writeFileSync(path.join(root, 'f'), '');
  fs.writeFileSync(path.join(root, 'a', 'g'), '');
  fs.writeFileSync(path.join(root, 'a', 'b', 'h'), '');
  fs.symlinkSync(path.join(root, 'a'), path.join(root, 'lnk'));
  fs.symlinkSync(path.join(root, 'f'), path.join(root, 'flnk'));
  fs.symlinkSync(path.join(root, 'nowhere'), path.join(root, 'dangling'));

  // breadth-first; symlinked directories are followed, as Node's readdirSync does
  assert.deepStrictEqual(fs.readdirSync(root, { recursive: true }), [
    'a', 'dangling', 'empty', 'f', 'flnk', 'lnk', 'a/b', 'a/g', 'lnk/b', 'lnk/g', 'a/b/h', 'lnk/b/h',
  ].map((name) => name.split('/').join(path.sep)));
  assert.deepStrictEqual([...fs.readdirSync(root, { recursive: true })].sort(), names);
  assert.deepStrictEqual(fs.readdirSync(root, { recursive: true, withFileTypes: true }).map(dirent).sort(), dirents);

  const callbackNames = await new Promise((resolve, reject) => {
    fs.readdir(root, { recursive: true }, (error, list) => error ? reject(error) : resolve(list));
  });
  assert.deepStrictEqual([...callbackNames].sort(), names);
  assert.deepStrictEqual([...(await fsp.readdir(root, { recursive: true }))].sort(), names);
  // Node's fs/promises alone skips symlinked directories with withFileTypes;
  // Ant follows them consistently, so only the shared part is compared here
  const outsideLink = (list) => list.filter((entry) => !entry.includes(path.join('R', 'lnk')));
  assert.deepStrictEqual(
    outsideLink((await fsp.readdir(root, { recursive: true, withFileTypes: true })).map(dirent).sort()),
    outsideLink(dirents)
  );

  // non-recursive listings are unchanged
  assert.deepStrictEqual(fs.readdirSync(root).sort(), ['a', 'dangling', 'empty', 'f', 'flnk', 'lnk']);
  assert.deepStrictEqual(fs.readdirSync(root, { recursive: false }).sort(), ['a', 'dangling', 'empty', 'f', 'flnk', 'lnk']);

  // parentPath goes through path.join, so a './' root yields 'a', not './a'
  const cwd = process.cwd();
  process.chdir(root);
  try {
    assert.ok(fs.readdirSync('./', { recursive: true, withFileTypes: true }).some((e) => e.name === 'h' && e.parentPath === path.join('a', 'b')));
    assert.ok(fs.readdirSync('./', { recursive: true, withFileTypes: true }).some((e) => e.name === 'a' && e.parentPath === './'));
  } finally {
    process.chdir(cwd);
  }

  if (process.platform !== 'win32' && process.getuid && process.getuid() !== 0) {
    fs.chmodSync(path.join(root, 'empty'), 0);
    try {
      for (const read of [
        () => fs.readdirSync(root, { recursive: true }),
        () => fsp.readdir(root, { recursive: true }),
      ]) {
        const error = await Promise.resolve().then(read).then(() => null, (e) => e);
        assert.ok(error, 'expected EACCES');
        assert.strictEqual(error.code, 'EACCES');
        assert.strictEqual(error.syscall, 'scandir');
        assert.strictEqual(R(error.path), path.join('R', 'empty'));
      }
    } finally {
      fs.chmodSync(path.join(root, 'empty'), 0o755);
    }
  }

  const missing = await fsp.readdir(path.join(root, 'nope'), { recursive: true }).catch((e) => e);
  assert.strictEqual(missing.code, 'ENOENT');
  assert.throws(() => fs.readdirSync(path.join(root, 'nope'), { recursive: true }), { code: 'ENOENT', syscall: 'scandir' });

  // symlinks back to an ancestor are listed but not descended; other directory links still are
  const cycle = path.join(root, 'cycle');
  fs.mkdirSync(path.join(cycle, 'a', 'b'), { recursive: true });
  fs.mkdirSync(path.join(cycle, 'c'));
  fs.writeFileSync(path.join(cycle, 'c', 'x'), '');
  fs.symlinkSync(cycle, path.join(cycle, 'self'));
  fs.symlinkSync(path.join(cycle, 'a'), path.join(cycle, 'a', 'b', 'up'));
  fs.symlinkSync(path.join(cycle, 'c'), path.join(cycle, 'a', 'side'));
  const cycleNames = ['a', 'a/b', 'a/b/up', 'a/side', 'a/side/x', 'c', 'c/x', 'self'].map(
    (name) => name.split('/').join(path.sep)
  );
  assert.deepStrictEqual([...fs.readdirSync(cycle, { recursive: true })].sort(), cycleNames);
  assert.deepStrictEqual([...(await fsp.readdir(cycle, { recursive: true }))].sort(), cycleNames);
  assert.deepStrictEqual(
    fs.readdirSync(cycle, { recursive: true, withFileTypes: true }).map((e) => path.relative(cycle, path.join(e.parentPath, e.name))).sort(),
    cycleNames
  );
}

main()
  .catch((error) => {
    console.error(error && error.stack ? error.stack : String(error));
    process.exitCode = 1;
  })
  .finally(() => fs.rmSync(root, { recursive: true, force: true }));
