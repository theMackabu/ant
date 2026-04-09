const fs = require('node:fs');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(fn, label) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  throw new Error(`${label} did not throw`);
}

const missingPath = 'tests/.fs_missing_file';
const renameDest = `${missingPath}.renamed`;

const readError = expectThrow(() => fs.readFileSync(missingPath, 'utf8'), 'readFileSync');
assert(readError.code === 'ENOENT', `expected readFileSync code ENOENT, got ${readError.code}`);
assert(readError.syscall === 'open', `expected readFileSync syscall open, got ${readError.syscall}`);
assert(readError.path === missingPath, `expected readFileSync path ${missingPath}, got ${readError.path}`);
assert(readError.message === `ENOENT: no such file or directory, open '${missingPath}'`, `unexpected readFileSync message: ${readError.message}`);

const renameError = expectThrow(() => fs.renameSync(missingPath, renameDest), 'renameSync');
assert(renameError.code === 'ENOENT', `expected renameSync code ENOENT, got ${renameError.code}`);
assert(renameError.syscall === 'rename', `expected renameSync syscall rename, got ${renameError.syscall}`);
assert(renameError.path === missingPath, `expected renameSync path ${missingPath}, got ${renameError.path}`);
assert(renameError.dest === renameDest, `expected renameSync dest ${renameDest}, got ${renameError.dest}`);
assert(
  renameError.message === `ENOENT: no such file or directory, rename '${missingPath}' -> '${renameDest}'`,
  `unexpected renameSync message: ${renameError.message}`
);

console.log('fs error message test passed');
