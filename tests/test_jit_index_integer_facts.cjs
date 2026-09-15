'use strict';
const assert = require('node:assert');
if (process.argv[2] === 'child' || typeof Ant === 'undefined') {
  function indexProbe(array, start, count) {
    let sum = 0;
    for (let n = 0; n < count; n++) {
      var index = start;
      sum += array[index];
      sum += array[++index];
    }
    return sum;
  }
  const values = [2, 3, 5, 7];
  values['0.5'] = 11; values['1.5'] = 13;
  for (let i = 0; i < 150; i++) assert.strictEqual(indexProbe(values, 0, 32), 160);
  assert.strictEqual(indexProbe(values, 1, 2000), 16000);
  assert.strictEqual(indexProbe(values, 0.5, 2), 48);
  assert.strictEqual(indexProbe(values, -0, 2), 10);
  assert.ok(Number.isNaN(indexProbe(values, 3, 2)));
  function crossing(value) { var index = value & 0x7fffffff; ++index; ++index; return index; }
  for (let i = 0; i < 1000; i++) assert.strictEqual(crossing(i), i + 2);
  assert.strictEqual(crossing(0x7fffffff), 2147483649);
  function assigned(value) { var local; return (local = value | 0); }
  for (let i = 0; i < 1000; i++) assert.strictEqual(assigned(i), i);
  assert.strictEqual(assigned(-1), -1);
  assert.strictEqual(assigned(4294967295), -1);
  assert.strictEqual(Object.is(assigned(-0), 0), true);
  function captured(array, count) {
    let index = 0, sum = 0;
    function update() { index = 1; }
    for (let n = 0; n < count; n++) {
      index = 0; sum += array[index]; update(); ++index; sum += array[index];
    }
    return sum;
  }
  assert.strictEqual(captured(values, 2000), 14000);
  function branches(array, flag, count) {
    let sum = 0;
    for (let n = 0; n < count; n++) {
      let index = 0; sum += array[index];
      if (flag) index = 0.5;
      ++index; sum += array[index];
    }
    return sum;
  }
  for (let i = 0; i < 150; i++) branches(values, false, 32);
  assert.strictEqual(branches(values, true, 1000), 15000);
  console.log('PASS index facts, increments, fractions, captures and branch joins');
} else {
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, [__filename, 'child'], {
    encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
    timeout: 60000, maxBuffer: 32 * 1024 * 1024,
  });
  assert.strictEqual(child.status, 0, String(child.error || child.stderr));
  assert.match(child.stdout, /PASS index facts/);
  const modules = child.stderr.match(/jit_indexProbe_[^\n]*:\s*module[\s\S]*?endmodule/g);
  assert.ok(modules && modules.length);
  const code = modules.join('\n');
  assert.match(code, /validated_index_local_/);
  assert.match(code, /\badd\s+s\d+,\s*s\d+,\s*1/);
  const first = modules[0];
  const conversions = new Set([...first.matchAll(/index_roundtrip_(\d+)/g)].map(match => match[1]));
  assert.strictEqual(conversions.size, 1, 'second read should reuse the checked integer through ++');
  console.log('PASS validated integer index propagation code generation');
}
