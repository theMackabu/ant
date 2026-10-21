import assert from 'node:assert';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const dir = fs.mkdtempSync(join(tmpdir(), 'ant-fs-write-options-'));
const file = name => join(dir, name);
const mode = name => fs.statSync(file(name)).mode & 0o777;

try {
  fs.writeFileSync(file('sync-mode'), 'x', { mode: 0o555 });
  assert.strictEqual(mode('sync-mode'), 0o555);

  fs.writeFileSync(file('sync-string-mode'), 'x', { mode: '751' });
  assert.strictEqual(mode('sync-string-mode'), 0o751);

  await fsp.writeFile(file('promise-mode'), 'x', { mode: 0o444, flag: 'wx' });
  assert.strictEqual(mode('promise-mode'), 0o444);

  await new Promise((resolve, reject) =>
    fs.writeFile(file('callback-mode'), 'x', { mode: 0o500 }, error => (error ? reject(error) : resolve()))
  );
  assert.strictEqual(mode('callback-mode'), 0o500);

  assert.throws(() => fs.writeFileSync(file('sync-mode'), 'y', { flag: 'wx' }), { code: 'EEXIST' });
  await assert.rejects(fsp.writeFile(file('promise-mode'), 'y', { flag: 'wx' }), { code: 'EEXIST' });

  fs.writeFileSync(file('append'), 'a');
  fs.writeFileSync(file('append'), 'b', { flag: 'a' });
  fs.appendFileSync(file('append'), 'c');
  assert.strictEqual(fs.readFileSync(file('append'), 'utf8'), 'abc');

  fs.writeFileSync(file('encoding'), 'hello', 'utf8');
  assert.strictEqual(fs.readFileSync(file('encoding'), 'utf8'), 'hello');

  fs.writeFileSync(file('exec'), '#!/bin/sh\n', { mode: 0o755 });
  fs.copyFileSync(file('exec'), file('exec-copy'));
  assert.strictEqual(mode('exec-copy'), 0o755);

  fs.writeFileSync(file('excl-dest'), 'keep');
  assert.throws(() => fs.copyFileSync(file('exec'), file('excl-dest'), fs.constants.COPYFILE_EXCL), { code: 'EEXIST' });
  assert.strictEqual(fs.readFileSync(file('excl-dest'), 'utf8'), 'keep');

  console.log('fs write options: ok');
} finally {
  for (const name of fs.readdirSync(dir)) fs.chmodSync(file(name), 0o644);
  fs.rmSync(dir, { recursive: true, force: true });
}
