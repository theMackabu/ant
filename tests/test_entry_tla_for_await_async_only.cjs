const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-entry-tla-for-await-'));
const jsPath = path.join(tmpRoot, 'entry.mjs');
const tsPath = path.join(tmpRoot, 'entry.ts');

const source = [
  'const iterable = {',
  '  [Symbol.asyncIterator]() {',
  '    let done = false;',
  '    return {',
  '      async next() {',
  '        if (done) return { done: true, value: undefined };',
  '        done = true;',
  '        return { done: false, value: "ok" };',
  '      }',
  '    };',
  '  }',
  '};',
  '',
  'for await (const chunk of iterable) {',
  '  console.log(chunk);',
  '}',
  '',
].join('\n');

fs.writeFileSync(jsPath, source);
fs.writeFileSync(tsPath, source);

for (const scriptPath of [jsPath, tsPath]) {
  const result = spawnSync(process.execPath, [scriptPath], {
    encoding: 'utf8',
  });

  if (result.error) throw result.error;

  assert(
    result.status === 0,
    `expected ${path.basename(scriptPath)} to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  );
  assert(
    result.stdout === 'ok\n',
    `expected ${path.basename(scriptPath)} to print ok, got ${JSON.stringify(result.stdout)}`
  );
  assert(
    !/not iterable|await can only be used inside async functions/i.test(result.stderr),
    `expected ${path.basename(scriptPath)} to treat top-level for-await as async iteration, got stderr:\n${result.stderr}`
  );
}

console.log('top-level for-await works for async-only iterables in JS and TS entries');
