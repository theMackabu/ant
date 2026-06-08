const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-dynamic-import-tla-context-'));
const entryPath = path.join(tmpRoot, 'entry.mjs');
const childPath = path.join(tmpRoot, 'child.mjs');
const depPath = path.join(tmpRoot, 'dep.mjs');

fs.writeFileSync(depPath, 'export default "dep-ok";\n');
fs.writeFileSync(
  childPath,
  [
    'await Promise.resolve();',
    'const dep = await import("./dep.mjs");',
    'export const depValue = dep.default;',
    'export const getUrl = () => import.meta.url;',
    'export default 123;',
    '',
  ].join('\n')
);
fs.writeFileSync(
  entryPath,
  [
    'const mod = await import("./child.mjs");',
    'console.log([mod.default, mod.depValue, mod.getUrl().endsWith("/child.mjs")].join(","));',
    '',
  ].join('\n')
);

const result = spawnSync(process.execPath, [entryPath], {
  encoding: 'utf8',
});

if (result.error) throw result.error;

assert(
  result.status === 0,
  `expected dynamic import of TLA module to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
assert(
  result.stdout === '123,dep-ok,true\n',
  `expected exports/import.meta after TLA resume to work, got ${JSON.stringify(result.stdout)}`
);
assert(
  !/export used outside module|undefined is not a function/i.test(result.stderr),
  `expected no module-context failure, got stderr:\n${result.stderr}`
);

console.log('dynamic import preserves module context across top-level await');
