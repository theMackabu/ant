// setImmediate returns an Immediate handle shaped like Node's: ref, unref,
// hasRef and Symbol.dispose on its prototype. Only ref'd immediates keep the
// process alive, but any queued immediate still runs before the loop waits on
// timers. Extra arguments reach the callback, and clearImmediate takes the handle.
// Each loop turn runs only the immediates queued before it, after the main
// script's microtasks.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const run = code => {
  const env = { ...process.env, NO_COLOR: '1' };
  delete env.FORCE_COLOR;
  const child = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 30000, env });
  assert.strictEqual(child.status, 0, child.stderr);
  return child.stdout;
};

const out = [];
const handle = setImmediate(() => out.push('ran'));
const proto = Object.getPrototypeOf(handle);
for (const name of ['ref', 'unref', 'hasRef']) assert.strictEqual(typeof proto[name], 'function', name);
assert.strictEqual(typeof proto[Symbol.dispose], 'function');
assert.ok(!Object.keys(handle).includes('id'));
assert.deepStrictEqual([handle.hasRef(), handle.unref() === handle, handle.hasRef(), handle.ref() === handle, handle.hasRef()], [true, true, false, true, true]);

clearImmediate(setImmediate(() => out.push('cleared ran')));
setImmediate(() => out.push('disposed ran'))[Symbol.dispose]();
setImmediate(() => out.push('unref ran while alive')).unref();
setImmediate((a, b) => out.push(['args', a, b]), 1, 2);
setTimeout(() => out.push('timeout'), 5);

setTimeout(() => {
  assert.deepStrictEqual(out, ['ran', 'unref ran while alive', ['args', 1, 2], 'timeout']);
  assert.strictEqual(run("setImmediate(() => console.log('ran')).unref(); console.log('end')"), 'end\n');
  assert.strictEqual(run("const i = setImmediate(() => console.log('ran')); i.unref(); i.ref()"), 'ran\n');
  assert.strictEqual(run("setImmediate(() => console.log('A')); setTimeout(() => console.log('T'), 0); setImmediate(() => console.log('B'))"), 'A\nB\nT\n');
  assert.strictEqual(run("{ using x = setImmediate(() => console.log('ran')); } console.log('after')"), 'after\n');
  assert.strictEqual(run("setImmediate(() => console.log('imm')); Promise.resolve().then(() => console.log('micro')); process.nextTick(() => console.log('tick'))"), 'tick\nmicro\nimm\n');
  assert.strictEqual(run("setImmediate(() => { console.log('a'); setImmediate(() => console.log('c')) }); setImmediate(() => console.log('b'))"), 'a\nb\nc\n');
  assert.strictEqual(run("setImmediate(() => clearImmediate(b)); const b = setImmediate(() => console.log('BAD')); setImmediate(() => console.log('ok'))"), 'ok\n');
  assert.strictEqual(run("let n = 0, fired = false; setTimeout(() => { fired = true }, 1); (function spin() { if (!fired && ++n < 1e6) setImmediate(spin); else console.log(fired) })()"), 'true\n');
  console.log('PASS Immediate handles behave like Node');
}, 30);
