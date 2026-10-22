const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');

const ESC = /\x1b\[/;

function run(args, env = {}) {
  const base = { ...process.env };
  delete base.NO_COLOR;
  delete base.FORCE_COLOR;
  const result = spawnSync(process.execPath, args, { encoding: 'utf8', env: { ...base, ...env } });
  if (result.error) throw result.error;
  return result;
}

const cases = [
  ['uncaught throw', ['-e', 'throw new TypeError("boom")'], /TypeError: boom/],
  ['unhandled rejection', ['-e', 'Promise.reject(new RangeError("late"))'], /RangeError: late/],
  ['console.error', ['-e', 'console.error(new Error("logged"))'], /Error: logged/],
  ['missing module', ['/nonexistent/entry.js'], /Module not found/],
];

for (const [name, args, text] of cases) {
  const piped = run(args);
  assert.match(piped.stderr, text, `${name}: ${piped.stderr}`);
  assert.doesNotMatch(piped.stderr, ESC, `${name} is plain when stderr is not a terminal`);

  assert.doesNotMatch(run(args, { NO_COLOR: '1' }).stderr, ESC, `${name} honours NO_COLOR`);
  assert.doesNotMatch(run(['--no-color', ...args], { FORCE_COLOR: '1' }).stderr, ESC, `${name} honours --no-color`);
  assert.match(run(args, { FORCE_COLOR: '1' }).stderr, ESC, `${name} honours FORCE_COLOR`);
}

const script = ['/usr/bin/script', '/bin/script'].find(p => fs.existsSync(p));
if (script) {
  const command = [process.execPath, '-e', 'throw new TypeError("tty")'];
  const args = process.platform === 'darwin' ? ['-q', '/dev/null', ...command] : ['-qec', command.map(a => `'${a}'`).join(' '), '/dev/null'];
  const env = { ...process.env };
  delete env.NO_COLOR;
  delete env.FORCE_COLOR;
  const tty = spawnSync(script, args, { encoding: 'utf8', env });
  assert.match(tty.stdout, /TypeError/);
  assert.match(tty.stdout, ESC, 'errors stay colored on a terminal');

  const quiet = spawnSync(script, args, { encoding: 'utf8', env: { ...env, NO_COLOR: '1' } });
  assert.match(quiet.stdout, /TypeError/);
  assert.doesNotMatch(quiet.stdout, ESC, 'NO_COLOR wins on a terminal');
}

console.log('error output color follows the stream');
