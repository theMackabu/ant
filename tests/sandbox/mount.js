import assert from 'node:assert';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { Sandbox } from 'ant:sandbox';

const base = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-sandbox-mount-')));
const data = path.join(base, 'data');
const readonly = path.join(base, 'ro');
const outside = path.join(base, 'outside');
for (const dir of [data, readonly, outside]) fs.mkdirSync(dir);
fs.writeFileSync(path.join(data, 'seed.txt'), 'seed');
fs.writeFileSync(path.join(readonly, 'seed.txt'), 'readonly seed');
fs.writeFileSync(path.join(outside, 'secret'), 'host secret');
fs.symlinkSync(outside, path.join(data, 'escape'));

const sandbox = new Sandbox({
  memory: '128mb',
  write: `${data}:/data`,
  mount: [`${process.cwd()}:/workspace`, `${readonly}:/ro`]
});
const running = sandbox.run('tests/sandbox/fixtures/mount_guest.js');
try {
  assert.deepStrictEqual(await sandbox.once('ready'), { type: 'ready' });
  sandbox.send({ type: 'go' });
  const result = await sandbox.once('result');
  assert.deepStrictEqual(result, {
    type: 'result',
    seed: 'seed',
    size: 300000,
    link: 'dir/renamed.txt',
    viaLink: 300004,
    listing: ['dir', 'escape', 'link', 'seed.txt'],
    notEmpty: 'ENOTEMPTY',
    missing: 'ENOENT',
    exists: 'EEXIST',
    escape: 'ENOENT',
    readonlyRead: 'readonly seed',
    readonlyWrite: 'EROFS'
  });

  assert.strictEqual(fs.readFileSync(path.join(data, 'dir', 'renamed.txt'), 'utf8').length, 300004);
  assert.strictEqual(fs.readlinkSync(path.join(data, 'link')), 'dir/renamed.txt');
  assert.ok(!fs.existsSync(path.join(data, 'seed.txt')));
  assert.deepStrictEqual(fs.readdirSync(readonly), ['seed.txt']);
  assert.deepStrictEqual(fs.readdirSync(outside), ['secret']);
} finally {
  await sandbox.terminate();
  await running.catch(() => {});
  fs.rmSync(base, { recursive: true, force: true });
}

console.log('sandbox mount: ok');
