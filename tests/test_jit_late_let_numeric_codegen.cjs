const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const source = `
function dummy() {}
function afterCall(n) { dummy(); let s = 0; for (let k = 0; k < n; k++) s += k * 0.5; return s; }
async function afterAwait(n) { await null; let s = 0; for (let k = 0; k < n; k++) s += k * 0.5; return s; }
(async () => {
  let t = 0;
  for (let r = 0; r < 8; r++) t += afterCall(20000) + await afterAwait(20000);
  console.log('total ' + t);
})();
`;
const env = { ...process.env, ANT_DEBUG: 'dump/vm:jit' };
delete env.FORCE_COLOR;
const dump = spawnSync(process.execPath, ['-e', source], {
  encoding: 'utf8', env, maxBuffer: 64 * 1024 * 1024, timeout: 30000,
});
assert.strictEqual(dump.status, 0, String(dump.error || dump.stderr));
assert.match(dump.stdout, /total 1599920000/);
const body = (name) => {
  const fn = dump.stderr.match(new RegExp('jit_' + name + '_[^\\n]*:\\s*func[\\s\\S]*?endfunc'));
  assert.ok(fn, 'missing compilation for ' + name);
  return fn[0];
};
for (const name of ['afterCall', 'afterAwait']) {
  assert.match(body(name), /\bdadd\s+(sd\d+), \1, sd\d+\n\s*dmov\s+ld0, \1\n/, name + ' keeps the late let in a double register');
}
console.log('PASS late numeric let locals stay in double registers');
