const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function workload() {
  class Fields {
    #a = 1;
    #b;
    static #s = 3;
    #m() { return this.#a + 1; }
    get #g() { return this.#a * 10; }
    set #g(v) { this.#a = v; }
    constructor(x) { this.#b = x; }
    sum() {
      this.#g = this.#a + 1;
      return this.#a + this.#b + this.#m() + this.#g + Fields.#s;
    }
    static has(o) { return #a in o; }
  }

  let total = 0;
  for (let i = 0; i < 20000; i++) total += new Fields(i).sum();
  assert.strictEqual(total, 200550000);
  assert.strictEqual(Fields.has(new Fields(1)), true);
  assert.strictEqual(Fields.has({}), false);

  class Base { constructor(o) { return o; } }
  class StampField extends Base { #id = 7; static get(o) { return o.#id; } }
  class StampMethod extends Base { #m() { return 9; } static call(o) { return o.#m(); } }
  class StampAccessor extends Base { get #v() { return 11; } static get(o) { return o.#v; } }

  for (const [Stamp, read, expected] of [
    [StampField, StampField.get, 7],
    [StampMethod, StampMethod.call, 9],
    [StampAccessor, StampAccessor.get, 11],
  ]) {
    let errors = 0;
    for (let i = 0; i < 5000; i++) {
      const o = {};
      new Stamp(o);
      assert.strictEqual(read(o), expected);
      try { new Stamp(o); } catch (e) { if (e instanceof TypeError) errors++; }
    }
    assert.strictEqual(errors, 5000, Stamp.name);
  }

  function readGlobal(i) { return globalThis.Math === Math ? i : -1; }
  let sum = 0;
  for (let i = 0; i < 20000; i++) sum += readGlobal(i);
  assert.strictEqual(sum, 199990000);
  console.log('PASS jit private define and global');
}

if (process.env.ANT_JIT_PRIVATE_DEFINE_CHILD) {
  workload();
} else {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: { ...process.env, ANT_JIT_PRIVATE_DEFINE_CHILD: '1', ANT_DEBUG: 'dump/vm:jit opt:off' },
    maxBuffer: 64 * 1024 * 1024,
    timeout: 30000,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr.slice(-4000)));
  assert.match(child.stdout, /PASS jit private define and global/);
  assert.match(child.stderr, /\bcall\s+private_def_proto, jit_helper_def_private,/);
  assert.match(child.stderr, /jit_readGlobal_[^\n]*:\s*module/);
  console.log('PASS jit private define and global code generation');
}
