const assert = require('node:assert');
const vm = require('node:vm');
const { spawnSync } = require('node:child_process');

function churn(count) {
  const values = [];
  for (let i = 0; i < count; i++) values.push({ index: i });
  return values.length;
}

function child() {
  const { callback, FFIType } = require('ant:ffi');

  const invoke = Ant.unsafe.c({ entry: 'invoke', args: ['uint64'], returns: 'int' })`
    #include <stdint.h>
    int invoke(uint64_t address) {
      int (*fn)(void) = (int (*)(void))(uintptr_t)address;
      return fn() + 1;
    }
  `;

  const fault = Ant.unsafe.c({ entry: 'fault', args: [], returns: 'int' })`
    int fault(void) {
      *(volatile int *)1 = 1;
      return 0;
    }
  `;

  let armed = false;
  let escaped = [];
  let reached = 0;

  function die() {
    if (armed) process.abort();
    return 0;
  }

  function mapped(depth, tag) {
    let counter = depth;
    const holder = { tag };
    escaped.push(() => `${holder.tag}:${counter++}`);
    if (arguments.length !== 2) throw new Error('bad arguments');
    escaped.push(arguments);
    return depth > 0 ? mapped(depth - 1, tag) + 1 : die();
  }

  function captured(depth, tag) {
    let counter = depth;
    const holder = { tag };
    escaped.push(() => `${holder.tag}:${counter++}`);
    escaped.push([depth, tag]);
    return depth > 0 ? captured(depth - 1, tag) + 1 : die();
  }

  function guarded(run, tag) {
    try {
      return run(3, tag);
    } finally {
      reached++;
    }
  }

  class Base {
    constructor() {
      this.value = die();
    }
  }

  function* generate() {
    yield die();
  }

  async function wait() {
    await null;
    return 0;
  }

  async function asynchronous() {
    const pending = wait();
    die();
    await pending;
    return 0;
  }

  const sandbox = vm.createContext({ die });
  const cyclic = [1, 2];
  cyclic.push(cyclic);

  const scenarios = {
    mapped: tag => guarded(mapped, tag) - 3,
    captured: tag => guarded(captured, tag) - 3,
    forEach: () => { [1, 2, 3].forEach(() => die()); return 0; },
    sort: () => { [3, 1, 2].sort((a, b) => die() + a - b); return 0; },
    replace: () => 'abc'.replace(/b/g, () => die()).length - 3,
    json: () => JSON.stringify({ nested: [{ toJSON: () => die() }] }).length - 14,
    join: () => [1, { toString: () => String(die()) }, cyclic].join('').length - 6,
    proxy: () => Object.keys(new Proxy({ a: 1 }, { getOwnPropertyDescriptor(target, key) { die(); return Reflect.getOwnPropertyDescriptor(target, key); } })).length - 1,
    construct: () => Reflect.construct(Base, [], class extends Base {}).value,
    generator: () => generate().next().value,
    async: () => { asynchronous(); return 0; },
    eval: () => eval('die()') + new Function('f', 'return f()')(die),
    context: () => vm.runInContext('die()', sandbox, { timeout: 60000 }),
  };

  let current = 'mapped';
  let round = 0;
  const cb = callback({ args: [], returns: FFIType.int }, () => scenarios[current](`r${round}`));
  const address = BigInt(cb.address());

  function nest(depth) {
    if (depth === 0) return 0;
    const inner = callback({ args: [], returns: FFIType.int }, () => nest(depth - 1));
    try {
      return invoke(BigInt(inner.address()));
    } finally {
      inner.close();
    }
  }

  function attempt() {
    const before = { round };
    let caught;
    try {
      invoke(address);
    } catch (error) {
      caught = error;
    } finally {
      before.done = true;
    }
    return [before, caught];
  }

  function frames() {
    return new Error('probe').stack;
  }

  function check(name) {
    for (const key of Object.keys(scenarios)) {
      current = key;
      assert.strictEqual(invoke(address), 1, `${key} after ${name}`);
    }
    current = name;
    assert.strictEqual(nest(24), 24);
    assert.strictEqual(churn(60000), 60000);
  }

  const WARM = 400;
  for (let i = 0; i < WARM; i++) check('mapped');
  escaped = [];
  const warmed = reached;
  const depth = frames().split('\n').length;

  const ROUNDS = 12;
  for (const name of Object.keys(scenarios)) {
    current = name;
    const kept = [];

    for (round = 0; round < ROUNDS; round++) {
      armed = true;
      escaped = [];
      const [before, caught] = attempt();
      armed = false;

      assert.deepStrictEqual(before, { round, done: true }, name);
      assert.match(caught.message, /entry "invoke" aborted with signal \d+/, name);
      assert.strictEqual(frames().split('\n').length, depth, `${name} left frames behind`);
      kept.push(escaped);
      escaped = [];
      check(name);
    }

    if (name !== 'mapped' && name !== 'captured') continue;
    for (let r = 0; r < ROUNDS; r++) {
      assert.strictEqual(kept[r].length, 8, name);
      for (let i = 0; i < 8; i += 2) {
        const level = 3 - i / 2;
        assert.strictEqual(kept[r][i](), `r${r}:${level}`, name);
        assert.strictEqual(kept[r][i](), `r${r}:${level + 1}`, name);
        assert.deepStrictEqual([...kept[r][i + 1]], [level, `r${r}`], name);
      }
    }
  }
  assert.strictEqual(reached, warmed + 2 * ROUNDS * Object.keys(scenarios).length, 'code after a fault ran');

  const nested = callback({ args: [], returns: FFIType.int }, () => {
    assert.throws(() => fault(), /entry "fault" faulted with signal \d+ at address 0x1/);
    return 41;
  });
  assert.strictEqual(invoke(BigInt(nested.address())), 42);

  const deep = callback({ args: [], returns: FFIType.int }, () => {
    const inner = callback({ args: [], returns: FFIType.int }, () => process.abort());
    try {
      assert.throws(() => invoke(BigInt(inner.address())), /entry "invoke" aborted/);
    } finally {
      inner.close();
    }
    return 7;
  });
  assert.strictEqual(invoke(BigInt(deep.address())), 8);
  assert.strictEqual(churn(400000), 400000);

  let ticks = 0;
  Promise.resolve().then(() => ticks++);
  setTimeout(() => {
    assert.strictEqual(ticks, 1);
    cb.close();
    nested.close();
    deep.close();
    console.log('child ok');
  }, 0);
}

if (typeof Ant.unsafe?.c !== 'function' || process.platform === 'win32') {
  console.log('Ant.unsafe.c fault reentry skipped: no fault recovery in this build');
} else if (process.argv[2] === 'child') {
  child();
} else {
  const result = spawnSync(process.execPath, [__filename, 'child'], { encoding: 'utf8', timeout: 60000 });
  assert.strictEqual(result.signal, null, `child was killed: ${result.signal}\n${result.stderr}`);
  assert.strictEqual(result.status, 0, result.stderr || result.stdout);
  assert.match(result.stdout, /child ok/);
  console.log('ok');
}
