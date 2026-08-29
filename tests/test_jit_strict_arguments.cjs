const assert = require('assert');
const { spawnSync } = require('child_process');

if (!process.env.ANT_JIT_STRICT_ARGUMENTS_CHILD) {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_DEBUG: 'dump/vm:op-warn',
      ANT_JIT_STRICT_ARGUMENTS_CHILD: '1',
    },
  });
  assert.strictEqual(child.status, 0, child.stdout + child.stderr);
  assert.doesNotMatch(
    child.stderr,
    /jit: ineligible op SPECIAL_OBJ\(0\) in strictArguments/,
  );
  assert.match(
    child.stderr,
    /jit: ineligible op SPECIAL_OBJ\(0\) in sloppyArguments/,
  );
  process.stdout.write(child.stdout);
  process.exit(0);
}

function strictArguments(first, second) {
  'use strict';
  const args = arguments;
  const before = args[0];
  first = 'changed';
  args[1] = 'argument-changed';
  return [
    args === arguments,
    args.length,
    before,
    args[0],
    first,
    second,
    args[1],
    args[2],
  ];
}

for (let i = 0; i < 500; i++) {
  assert.deepStrictEqual(
    strictArguments('first', 'second', 'extra'),
    [true, 3, 'first', 'first', 'changed', 'second', 'argument-changed', 'extra'],
  );
}

function strictArgumentsOsr(value) {
  'use strict';
  const args = arguments;
  let total = 0;
  for (let i = 0; i < 1000; i++) total += i;
  return [args, arguments, args[0], value, total];
}
const osrResult = strictArgumentsOsr('osr');
assert.strictEqual(osrResult[0], osrResult[1]);
assert.strictEqual(osrResult[2], 'osr');
assert.strictEqual(osrResult[3], 'osr');
assert.strictEqual(osrResult[4], 499500);

function sloppyArguments(value) {
  value = 'mapped';
  return arguments[0];
}
for (let i = 0; i < 500; i++)
  assert.strictEqual(sloppyArguments('original'), 'mapped');

console.log('JIT strict arguments tests passed');
