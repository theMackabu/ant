const assert = require('node:assert');

function syntaxError(f) {
  try {
    f();
  } catch (e) {
    return e.constructor.name;
  }
  return 'none';
}

if (process.argv[2] === 'child') {
  new Function('({ __proto__: 1, __proto__: 2 })');
} else {
  assert.strictEqual(syntaxError(() => eval('({ __proto__: 1, __proto__: 2 })')), 'SyntaxError');
  assert.strictEqual(syntaxError(() => eval('({ a = 1 })')), 'SyntaxError');
  assert.strictEqual(syntaxError(() => (0, eval)('[{ __proto__: 1, __proto__: 2 }]')), 'SyntaxError');
  assert.strictEqual(syntaxError(() => new Function('return ({ __proto__: 1, __proto__: 2 })')), 'SyntaxError');
  assert.strictEqual(eval('var o; ({ __proto__: o, __proto__: o } = { __proto__: 5 }); typeof o'), 'object');
  const p = { z: 9 };
  assert.strictEqual(eval('({ __proto__: p }).z'), 9);

  if (typeof Ant !== 'undefined') {
    const { spawnSync } = require('node:child_process');
    const child = spawnSync(process.execPath, [__filename, 'child'], {
      encoding: 'utf8',
      env: { ...process.env, ANT_DEBUG: 'dump/vm:op-warn', NO_COLOR: '1' },
    });
    assert.match(child.stderr, /Duplicate __proto__ fields/);
    assert.doesNotMatch(child.stderr, /operand depth analysis failed/);
  }
  console.log('ok');
}
