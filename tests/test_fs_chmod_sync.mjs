import fsDefault, { chmod as chmodFromFs, chmodSync as chmodSyncFromFs, statSync, writeFileSync, unlinkSync } from 'fs';
import { chmod as chmodFromNodeFs, chmodSync as chmodSyncFromNodeFs } from 'node:fs';
import { chmod as chmodFromPromises } from 'node:fs/promises';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const testFile = 'tests/.fs_chmod_sync_tmp';

try {
  writeFileSync(testFile, 'chmod sync test');

  chmodSyncFromFs(testFile, 0o600);
  assert((statSync(testFile).mode & 0o777) === 0o600, 'fs named chmodSync should set numeric mode');

  chmodSyncFromNodeFs(testFile, '644');
  assert((statSync(testFile).mode & 0o777) === 0o644, 'node:fs named chmodSync should set string mode');

  fsDefault.chmodSync(testFile, '0o600');
  assert((fsDefault.statSync(testFile).mode & 0o777) === 0o600, 'default chmodSync should set prefixed string mode');

  await chmodFromPromises(testFile, 0o644);
  assert((statSync(testFile).mode & 0o777) === 0o644, 'fs/promises chmod should set numeric mode');

  await chmodFromNodeFs(testFile, '600');
  assert((statSync(testFile).mode & 0o777) === 0o600, 'node:fs named chmod should set string mode');

  await new Promise((resolve, reject) => {
    chmodFromFs(testFile, 0o644, error => error ? reject(error) : resolve());
  });
  assert((statSync(testFile).mode & 0o777) === 0o644, 'fs callback chmod should set numeric mode');
} finally {
  try {
    fsDefault.chmodSync(testFile, 0o600);
    unlinkSync(testFile);
  } catch {}
}

console.log('fs chmodSync test passed');
