const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

for (const count of [16, 17]) {
  for (const kind of ['nested', 'sequential']) {
    const name = `try_${kind}_${count}`;
    const marker = 1234567;
    let body = `try { if(value) throw ${marker}; return ${marker}; }
      catch(error) { return error; }`;
    for (let i = 1; i < count; i++) {
      if (kind === 'nested') body = `try { ${body} } catch(error) { return error; }`;
      else body = `try { if(value) throw 1; } catch(error) {}\n` + body;
    }
    const source = `function ${name}(value) { ${body} }
      for(let i=0;i<200;i++) {
        if(${name}(true)!==${marker}) throw new Error('catch result');
        if(${name}(false)!==${marker}) throw new Error('normal result');
      }
      console.log('try capacity ok');`;
    const child = spawnSync(process.execPath, ['-e', source], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/vm:jit' },
      maxBuffer: 8 * 1024 * 1024,
      timeout: 10000,
    });
    assert.strictEqual(child.status, 0, `${name}: ${child.stderr}`);
    assert.match(child.stdout, /try capacity ok/);
    const module = child.stderr.match(new RegExp(`jit_${name}_[^\\n]*:\\s*module[\\s\\S]*?endmodule`));
    assert.ok(module, `${name}: missing JIT attempt`);
    // Overflow must stop before emitting the overflowing try body's marker.
    if (count === 17) assert.doesNotMatch(module[0], /\b1234567\b/, `${name}: emitted overflowing body`);
    else assert.match(module[0], /\b1234567\b/, `${name}: rejected an in-capacity body`);
  }
}
console.log('jit try capacity: ok');
