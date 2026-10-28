const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const WARM = 6000;

function outcome(fn) {
  try {
    const value = fn();
    return typeof value === 'symbol' ? 'symbol' : `value ${String(value)}`;
  } catch (e) {
    return e && e.constructor ? e.constructor.name : 'thrown';
  }
}

function check(name, run, expected) {
  for (let i = 0; i < WARM; i++) {
    const early = i % 2 === 0;
    assert.strictEqual(run(early), early ? expected[0] : expected[1], `${name} (${early ? 'tdz' : 'initialized'}, ${i})`);
  }
}

function letRead(early) {
  const tdzReader = () => x;
  if (early) return outcome(tdzReader);
  let x = 1;
  return outcome(tdzReader);
}
check('let read', letRead, ['ReferenceError', 'value 1']);

function constRead(early) {
  const read = () => k;
  if (early) return outcome(read);
  const k = 'k';
  return outcome(read);
}
check('const read', constRead, ['ReferenceError', 'value k']);

function classRead(early) {
  const read = () => typeof C;
  if (early) return outcome(read);
  class C {}
  return outcome(read);
}
check('class read', classRead, ['ReferenceError', 'value function']);

function classNew(early) {
  const make = () => new C().v;
  if (early) return outcome(make);
  class C { constructor() { this.v = 3; } }
  return outcome(make);
}
check('class construct', classNew, ['ReferenceError', 'value 3']);

function hoistedRead(early) {
  if (early) return outcome(get);
  let y = 2;
  return outcome(get);
  function get() { return y; }
}
check('hoisted function read', hoistedRead, ['ReferenceError', 'value 2']);

function nestedRead(early) {
  const make = () => () => () => z;
  const read = make()();
  if (early) return outcome(read);
  let z = 4;
  return outcome(read);
}
check('nested closure read', nestedRead, ['ReferenceError', 'value 4']);

function nestedHoisted(early) {
  function level1() { return function level2() { return w; }; }
  if (early) return outcome(level1());
  const w = 5;
  return outcome(level1());
}
check('nested hoisted read', nestedHoisted, ['ReferenceError', 'value 5']);

function letWrite(early) {
  const write = () => { x = 7; };
  if (early) return outcome(write);
  let x = 1;
  write();
  return outcome(() => x);
}
check('let write', letWrite, ['ReferenceError', 'value 7']);

function letWriteKeep(early) {
  const write = () => (x = 8);
  if (early) return outcome(write);
  let x = 1;
  return outcome(write);
}
check('let write value', letWriteKeep, ['ReferenceError', 'value 8']);

function constWrite(early) {
  const write = () => { k = 1; };
  if (early) return outcome(write);
  const k = 0;
  return outcome(write);
}
check('const write', constWrite, ['ReferenceError', 'TypeError']);

function hoistedWrite(early) {
  if (early) return outcome(set);
  let y = 0;
  set();
  return outcome(() => y);
  function set() { y = 9; }
}
check('hoisted function write', hoistedWrite, ['ReferenceError', 'value 9']);

function nestedWrite(early) {
  const make = () => () => { v = 10; return v; };
  const write = make();
  if (early) return outcome(write);
  let v = 0;
  return outcome(write);
}
check('nested closure write', nestedWrite, ['ReferenceError', 'value 10']);

function compoundWrite(early) {
  const bump = () => (n += 1);
  if (early) return outcome(bump);
  let n = 10;
  return outcome(bump);
}
check('compound write', compoundWrite, ['ReferenceError', 'value 11']);

function localWrite(early) {
  if (early) return outcome(() => { try { q = 1; } catch (e) { throw e; } let q; return q; });
  let q;
  q = 12;
  return outcome(() => q);
}
check('direct local write', localWrite, ['ReferenceError', 'value 12']);

function loopBindings(early) {
  let result = '';
  let previous = null;
  for (let i = 0; i < 3; i++) {
    const read = () => b;
    if (early && i === 1) result += outcome(read) + ';';
    if (previous) result += outcome(previous) + ';';
    let b = i;
    previous = read;
  }
  return result;
}
check('loop per-iteration bindings', loopBindings, ['ReferenceError;value 0;value 1;', 'value 0;value 1;']);

function switchRead(early) {
  switch (early ? 1 : 0) {
    case 0:
      let s = 13;
    case 1:
      const read = () => s;
      return outcome(read);
  }
}
check('switch case read', switchRead, ['ReferenceError', 'value 13']);

function switchWrite(early) {
  switch (early ? 1 : 0) {
    case 0:
      let s = 0;
    case 1: {
      const write = () => { s = 14; return s; };
      return outcome(write);
    }
  }
}
check('switch case write', switchWrite, ['ReferenceError', 'value 14']);

function switchDirect(early) {
  switch (early ? 1 : 0) {
    case 0:
      let s = 15;
    case 1:
      return outcome(() => { switch (0) { default: return s; } });
  }
}
check('switch case direct read', switchDirect, ['ReferenceError', 'value 15']);

function switchLocal(early) {
  try {
    switch (early ? 1 : 0) {
      case 0:
        let s = 16;
      case 1:
        return 'value ' + s;
    }
  } catch (e) {
    return e.constructor.name;
  }
}
check('switch case local read', switchLocal, ['ReferenceError', 'value 16']);

function switchRepeat(early) {
  let out = '';
  for (let k = 0; k < 2; k++) {
    switch (early && k === 1 ? 1 : 0) {
      case 0:
        let s = k;
      case 1:
        out += outcome(() => s) + ';';
    }
  }
  return out;
}
check('switch re-entry', switchRepeat, ['value 0;ReferenceError;', 'value 0;value 1;']);

function inlinedReader(early) {
  const tdzLeaf = () => x;
  let sum = 0;
  if (early) {
    try { for (let i = 0; i < 4; i++) sum += tdzLeaf(); } catch (e) { return e.constructor.name; }
  }
  let x = 2;
  for (let i = 0; i < 50; i++) sum += tdzLeaf();
  return 'value ' + sum;
}
check('inlined reader', inlinedReader, ['ReferenceError', 'value 100']);

const sharedReader = (function () {
  let ready = false;
  const read = () => cell;
  function outer() { return ready ? read() + 1 : 0; }
  for (let i = 0; i < WARM; i++) outer();
  ready = true;
  return outer;
  let cell = 1;
})();
assert.strictEqual(outcome(sharedReader), 'ReferenceError', 'closure read in a hot caller');

function safeRead(early) {
  let total = early ? 1 : 2;
  const safeReader = () => total;
  return outcome(safeReader);
}
check('initialized read', safeRead, ['value 1', 'value 2']);

const mode = process.argv[2];
if (mode === 'child') {
  console.log('PASS child');
} else {
  const runs = [
    { name: 'default', args: [], env: { ANT_DEBUG: '' } },
    { name: 'jitless', args: ['--jitless'], env: {} },
    { name: 'codegen', args: [], env: { ANT_DEBUG: 'dump/vm:jit opt:off' } },
  ];
  for (const run of runs) {
    const env = { ...process.env, ...run.env };
    const child = spawnSync(process.execPath, [...run.args, __filename, 'child'], {
      encoding: 'utf8',
      env,
      maxBuffer: 128 * 1024 * 1024,
      timeout: 120000,
    });
    assert.strictEqual(child.status, 0, `${run.name}: ${String(child.error || child.stderr.slice(-4000))}`);
    assert.match(child.stdout, /PASS child/, run.name);
    if (run.name !== 'codegen') continue;
    const compiled = name => {
      const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
      assert.ok(module, `${name}: missing compilation`);
      return module[0];
    };
    const guard = compiled('tdzReader').match(/\bbeq\s+L\d+, \w+, (\d{15,})\b/);
    assert.ok(guard, 'a closure read that may see TDZ must be checked');
    assert.doesNotMatch(compiled('safeReader'), new RegExp(`\\bbeq\\s+L\\d+, \\w+, ${guard[1]}\\b`),
      'a closure created after initialization must read without a TDZ check');
  }
  console.log('PASS closure TDZ reads and writes in compiled code');
}
