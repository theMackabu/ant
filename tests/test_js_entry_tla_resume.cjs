const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-js-entry-tla-resume-'));
const scriptPath = path.join(tmpRoot, 'entry.mjs');

fs.writeFileSync(
  scriptPath,
  [
    'const order = [];',
    'const settled = Promise.resolve("ready");',
    'async function leaf() {',
    '  order.push("leaf-before");',
    '  const value = await settled;',
    '  order.push(`leaf-after:${value}`);',
    '  return value;',
    '}',
    'async function middle() {',
    '  order.push("middle-before");',
    '  const value = await leaf();',
    '  order.push(`middle-after:${value}`);',
    '  return value;',
    '}',
    'order.push("start");',
    'await Promise.resolve();',
    'order.push("after-microtask");',
    'const nested = await middle();',
    'order.push(`nested:${nested}`);',
    'await new Promise((resolve) => setTimeout(resolve, 0));',
    'order.push("after-timer");',
    'console.log(order.join(","));',
    '',
  ].join('\n')
);

const result = spawnSync(process.execPath, [scriptPath], {
  encoding: 'utf8',
});

if (result.error) throw result.error;

assert(
  result.status === 0,
  `expected direct .mjs TLA entry to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
assert(
  result.stdout === 'start,after-microtask,middle-before,leaf-before,leaf-after:ready,middle-after:ready,nested:ready,after-timer\n',
  `expected stdout to show full TLA resume order, got ${JSON.stringify(result.stdout)}`
);
assert(
  !/invalid suspended frame state|EXC_BAD_ACCESS|segmentation fault/i.test(result.stderr),
  `expected no suspended-frame crash markers, got stderr:\n${result.stderr}`
);

console.log('direct JavaScript entrypoint top-level await resumes correctly');
