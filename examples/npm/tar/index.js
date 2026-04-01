import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import * as tar from 'tar';

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'tar-test-'));
const src = path.join(tmp, 'src');
const dest = path.join(tmp, 'dest');
const archive = path.join(tmp, 'test.tar.gz');

fs.mkdirSync(src, { recursive: true });
fs.writeFileSync(path.join(src, 'hello.txt'), 'Hello, World!\n');
fs.writeFileSync(path.join(src, 'data.json'), '{"a":1}');
fs.mkdirSync(path.join(dest), { recursive: true });

async function run() {
  console.log('1. Creating tarball...');
  await tar.create({ file: archive, cwd: tmp, gzip: true }, ['src']);
  if (!fs.existsSync(archive) || fs.statSync(archive).size === 0) {
    console.log('FAIL — archive not created');
    process.exit(1);
  }
  console.log(`   OK — archive created (${fs.statSync(archive).size} bytes)`);

  console.log('2. Extracting tarball...');
  await tar.extract({ file: archive, cwd: dest });
  if (!fs.existsSync(path.join(dest, 'src', 'hello.txt'))) {
    console.log('FAIL — files not extracted');
    process.exit(1);
  }
  console.log('   OK — files extracted');

  console.log('3. Verifying contents...');
  const original1 = fs.readFileSync(path.join(src, 'hello.txt'), 'utf8');
  const extracted1 = fs.readFileSync(path.join(dest, 'src', 'hello.txt'), 'utf8');
  const original2 = fs.readFileSync(path.join(src, 'data.json'), 'utf8');
  const extracted2 = fs.readFileSync(path.join(dest, 'src', 'data.json'), 'utf8');

  if (original1 !== extracted1 || original2 !== extracted2) {
    console.log('FAIL — content mismatch');
    process.exit(1);
  }
  console.log('   OK — contents match');

  console.log('\nAll good.');
  fs.rmSync(tmp, { recursive: true, force: true });
}

run().catch(err => {
  console.error('FAIL —', err.message);
  fs.rmSync(tmp, { recursive: true, force: true });
  process.exit(1);
});
