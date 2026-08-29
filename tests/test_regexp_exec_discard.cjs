const assert = require('assert');
const { spawnSync } = require('child_process');

if (!process.env.ANT_RE_EXEC_DISCARD_CHILD) {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_DEBUG: 'dump/vm:op-warn',
      ANT_RE_EXEC_DISCARD_CHILD: '1',
    },
  });
  assert.strictEqual(child.status, 0, child.stdout + child.stderr);
  assert.doesNotMatch(child.stderr, /jit: ineligible op RE_EXEC_DISCARD/);
  process.stdout.write(child.stdout);
  process.exit(0);
}

function discardExec(regexp, value) {
  regexp.exec(value);
}

for (let round = 0; round < 500; round++) {
  const regexp = /(a)/g;
  discardExec(regexp, 'aba');
  assert.strictEqual(regexp.lastIndex, 1);
  assert.strictEqual(RegExp.$1, 'a');
  assert.strictEqual(RegExp.lastMatch, 'a');
  discardExec(regexp, 'aba');
  assert.strictEqual(regexp.lastIndex, 3);
  discardExec(regexp, 'aba');
  assert.strictEqual(regexp.lastIndex, 0);
}

const order = [];
let customResult = { matched: true };
const custom = {
  get exec() {
    order.push('get');
    return function (value) {
      order.push(`call:${value}`);
      return customResult;
    };
  },
};
function nextArg() {
  order.push('arg');
  return 'payload';
}
function discardCustom() {
  custom.exec(nextArg());
}

for (let i = 0; i < 500; i++) {
  order.length = 0;
  discardCustom();
  assert.deepStrictEqual(order, ['get', 'arg', 'call:payload']);
}

customResult = null;
order.length = 0;
discardCustom();
assert.deepStrictEqual(order, ['get', 'arg', 'call:payload']);

customResult = new Error('discard error');
Object.defineProperty(custom, 'exec', {
  configurable: true,
  value() {
    throw customResult;
  },
});
assert.throws(() => discardCustom(), /discard error/);

console.log('RegExp exec discard tests passed');
