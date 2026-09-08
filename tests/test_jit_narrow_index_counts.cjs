const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

for (const count of [255, 256, 257]) {
  for (const kind of ['locals', 'constants']) {
    const name = `narrow_${kind}_${count}`;
    let body;
    let expected;
    if (kind === 'locals') {
      const names = Array.from({ length: count }, (_, i) => `a${i}`);
      body = names.map(name => `let ${name}=1;`).join('\n');
      body += '\na0++;a0--;a0+=1;a1=(a0=1);\nreturn ' + names.join('+') + ';';
      expected = count;
    } else {
      body = 'return ' + Array.from({ length: count }, (_, i) => `(${1000 + i})`).join('+') + ';';
      expected = 1000 * count + count * (count - 1) / 2;
    }
    const source = `function ${name}(){${body}}
      for(let i=0;i<200;i++) {
        if(${name}()!==${expected}) throw new Error('wrong result');
      }`;
    const child = spawnSync(process.execPath, ['-e', source], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
      maxBuffer: 32 * 1024 * 1024,
      timeout: 10000,
    });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stderr}`);
    const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
    assert.ok(module, `${name}: missing JIT module`);
    // Failed compilation also dumps MIR, but never reaches this function's return.
    assert.match(module[0], /\bret\s+s0\b/, `${name}: JIT stopped before the return`);
  }
}
console.log('jit narrow index counts: ok');
