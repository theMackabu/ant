const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

function churn(count) {
  const values = [];
  for (let i = 0; i < count; i++) values.push({ index: i });
  return values.length;
}

function child(mode) {
  const { callback, FFIType } = require('ant:ffi');

  const invoke = Ant.unsafe.c({ entry: 'invoke', args: ['uint64'], returns: 'int' })`
    #include <stdint.h>
    int invoke(uint64_t address) {
      int (*fn)(void) = (int (*)(void))(uintptr_t)address;
      return fn() + 1;
    }
  `;

  const invokeThenFault = Ant.unsafe.c({ entry: 'invoke_then_fault', args: ['uint64'], returns: 'int' })`
    #include <stdint.h>
    int invoke_then_fault(uint64_t address) {
      int (*fn)(void) = (int (*)(void))(uintptr_t)address;
      int value = fn();
      *(volatile int *)1 = value;
      return value;
    }
  `;

  const fault = Ant.unsafe.c({ entry: 'fault', args: [], returns: 'int' })`
    int fault(void) {
      *(volatile int *)1 = 1;
      return 0;
    }
  `;

  const abort = Ant.unsafe.c({ entry: 'stop', args: [], returns: 'int' })`
    #include <stdlib.h>
    int stop(void) {
      abort();
      return 0;
    }
  `;

  let armed = false;
  let calls = 0;

  function die() {
    calls++;
    if (armed) process.abort();
    return 0;
  }

  function plain() {
    const holder = { value: die() };
    return holder.value;
  }

  async function later() {
    die();
    await null;
    return 0;
  }

  const targets = {
    plain,
    async: () => { later(); return 0; },
    builtin: () => [1, 2, 3].map(() => die()).length - 3,
    nested: () => invoke(BigInt(inner.address())) - 1,
  };

  const inner = callback({ args: [], returns: FFIType.int }, plain);
  const recovering = callback({ args: [], returns: FFIType.int }, () => {
    assert.throws(() => fault(), /entry "fault" faulted with signal \d+ at address 0x1/);
    assert.throws(() => abort(), /entry "stop" aborted with signal \d+/);
    return churn(2000) - 2000;
  });

  if (mode === 'recover') {
    for (let i = 0; i < 200; i++) {
      assert.throws(() => fault(), /entry "fault" faulted with signal \d+ at address 0x1/);
      assert.throws(() => abort(), /entry "stop" aborted with signal \d+/);
      assert.throws(() => invokeThenFault(BigInt(inner.address())), /entry "invoke_then_fault" faulted with signal \d+ at address 0x1/);
      assert.strictEqual(invoke(BigInt(recovering.address())), 1);
      assert.throws(() => invokeThenFault(BigInt(recovering.address())), /entry "invoke_then_fault" faulted/);
      assert.strictEqual(invoke(BigInt(inner.address())), 1);
      if (i % 20 === 0) assert.strictEqual(churn(100000), 100000);
    }
    assert.strictEqual(calls, 400);

    let ticks = 0;
    Promise.resolve().then(() => ticks++);
    setTimeout(() => {
      assert.strictEqual(ticks, 1);
      console.log('recovered');
    }, 0);
    return;
  }

  const cb = callback({ args: [], returns: FFIType.int }, targets[mode]);
  const address = BigInt(cb.address());
  for (let i = 0; i < 3000; i++) assert.strictEqual(invoke(address), 1);

  console.log('armed');
  armed = true;
  try {
    invoke(address);
  } catch (error) {
    console.log(`survived ${error.message}`);
  }
  churn(300000);
  console.log('survived');
}

function run(flags, mode) {
  return spawnSync(process.execPath, [...flags, __filename, mode], {
    encoding: 'utf8',
    timeout: 60000,
    env: { ...process.env, DO_NOT_TRACK: '1' },
  });
}

if (typeof Ant.unsafe?.c !== 'function' || process.platform === 'win32') {
  console.log('Ant.unsafe.c fault reentry skipped: no fault recovery in this build');
} else if (process.argv[2]) {
  child(process.argv[2]);
} else {
  for (const flags of [[], ['--jitless']]) {
    const label = flags.join(' ');
    const recovered = run(flags, 'recover');
    assert.strictEqual(recovered.error, undefined, `${label} recover: ${recovered.error}`);
    assert.strictEqual(recovered.status, 0, `${label} recover: ${recovered.stderr || recovered.stdout}`);
    assert.match(recovered.stdout, /recovered/);

    for (const mode of ['plain', 'async', 'builtin', 'nested']) {
      const result = run(flags, mode);
      assert.strictEqual(result.error, undefined, `${label} ${mode}: ${result.error}`);
      assert.match(result.stdout, /armed/, `${label} ${mode}: ${result.stderr}`);
      assert.doesNotMatch(result.stdout, /survived/, `${label} ${mode}`);
      assert.ok(['SIGTRAP', 'SIGABRT'].includes(result.signal), `${label} ${mode}: signal ${result.signal} status ${result.status}`);
    }
  }

  console.log('ok');
}
