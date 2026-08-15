const assert = require('assert');
const { spawnSync } = require('child_process');

const probe = `
  const dirnameDescriptor = Object.getOwnPropertyDescriptor(globalThis, '__dirname');
  const filenameDescriptor = Object.getOwnPropertyDescriptor(globalThis, '__filename');
  console.log(JSON.stringify([
    dirnameDescriptor.enumerable,
    filenameDescriptor.enumerable,
  ]));
  process.exit(0);
`;

const result = spawnSync(process.execPath, ['--no-color', '--repl', probe], {
  encoding: 'utf8',
});

if (result.error) throw result.error;
assert.strictEqual(result.status, 0, result.stderr);
assert.deepStrictEqual(JSON.parse(result.stdout.trim()), [false, false]);

console.log('REPL path globals are non-enumerable');
