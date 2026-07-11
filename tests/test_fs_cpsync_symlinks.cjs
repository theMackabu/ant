const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cpsync-links-'));
try {
  const source = path.join(root, 'Source.framework');
  const destination = path.join(root, 'Destination.framework');
  const libraries = path.join(source, 'Versions', 'A', 'Libraries');
  fs.mkdirSync(libraries, { recursive: true });
  const executable = path.join(libraries, 'helper');
  fs.writeFileSync(executable, 'ant');
  fs.chmodSync(executable, 0o755);
  fs.mkdirSync(path.join(source, 'excluded'));
  fs.writeFileSync(path.join(source, 'excluded', 'ignored'), 'ignored');
  fs.symlinkSync('A', path.join(source, 'Versions', 'Current'), 'dir');
  fs.symlinkSync('Versions/A/Libraries', path.join(source, 'Libraries'), 'dir');

  const filtered = [];
  fs.cpSync(source, destination, {
    recursive: true,
    dereference: false,
    verbatimSymlinks: true,
    filter(sourcePath, destinationPath) {
      assert.equal(typeof destinationPath, 'string');
      filtered.push(sourcePath);
      return path.basename(sourcePath) !== 'excluded';
    }
  });

  assert(fs.lstatSync(path.join(destination, 'Libraries')).isSymbolicLink());
  assert.equal(
    fs.readlinkSync(path.join(destination, 'Libraries')),
    'Versions/A/Libraries'
  );
  const copiedExecutable = path.join(destination, 'Libraries', 'helper');
  assert.equal(fs.readFileSync(copiedExecutable, 'utf8'), 'ant');
  assert.equal(fs.statSync(copiedExecutable).mode & 0o777, 0o755);
  assert.equal(fs.existsSync(path.join(destination, 'excluded')), false);
  assert(filtered.includes(source));
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('fs-cpsync-symlinks-ok');
