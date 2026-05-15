const assert = require('assert');
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-watch-supervisor-'));
const entry = path.join(tmpRoot, 'entry.mjs');
const dep = path.join(tmpRoot, 'dep.mjs');

fs.writeFileSync(dep, 'export const value = 1;\n');
fs.writeFileSync(entry, [
  "import { value } from './dep.mjs';",
  "if (!process.argv.includes('--watch')) throw new Error('script argv lost --watch');",
  "console.log('watch-run:' + value);",
  'setInterval(() => {}, 1000);',
  ''
].join('\n'));

const child = spawn(process.execPath, ['-w', '--no-clear-screen', entry, '--', '--watch']);
child.stdout.setEncoding('utf8');
child.stderr.setEncoding('utf8');

let stdout = '';
let stderr = '';
let sawFirstRun = false;
let sawSecondRun = false;
let done = false;
let stopAfterRestart = null;

function fail(error) {
  if (done) return;
  done = true;
  child.kill('SIGKILL');
  try {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  } catch (_) {}
  throw error;
}

const timeout = setTimeout(() => {
  fail(new Error(`watch supervisor timed out\nstdout:\n${stdout}\nstderr:\n${stderr}`));
}, 6000);

child.stdout.on('data', chunk => {
  stdout += chunk;
  const secondRunCount = (stdout.match(/watch-run:2/g) || []).length;
  if (secondRunCount > 1) {
    fail(new Error(`watch child restarted more than once for one edit\nstdout:\n${stdout}\nstderr:\n${stderr}`));
  }

  if (!sawFirstRun && stdout.includes('watch-run:1')) {
    sawFirstRun = true;
    fs.writeFileSync(dep, 'export const value = 2;\n');
  }

  if (!sawSecondRun && secondRunCount === 1) {
    sawSecondRun = true;
    stopAfterRestart = setTimeout(() => child.kill('SIGINT'), 400);
  }
});

child.stderr.on('data', chunk => {
  stderr += chunk;
});

child.on('close', code => {
  if (done) return;
  done = true;
  clearTimeout(timeout);
  if (stopAfterRestart) clearTimeout(stopAfterRestart);

  try {
    assert(sawFirstRun, `missing first watch run\nstdout:\n${stdout}\nstderr:\n${stderr}`);
    assert(sawSecondRun, `dependency edit did not restart watch child\nstdout:\n${stdout}\nstderr:\n${stderr}`);
    assert(code === 130 || code === 0, `unexpected watch exit code ${code}\nstderr:\n${stderr}`);
  } finally {
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }

  console.log('watch supervisor restart ok');
});
