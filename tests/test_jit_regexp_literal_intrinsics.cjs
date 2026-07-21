const assert = require('assert');

function literalExec(value) {
  return /ab/.exec(value);
}

function literalReplace(value) {
  return value.replace(/ab/g, 'x');
}

for (let i = 0; i < 300; i++) {
  assert.strictEqual(literalExec('zab')[0], 'ab');
  assert.strictEqual(literalReplace('ab-ab'), 'x-x');
}

const originalExec = RegExp.prototype.exec;
const originalReplace = String.prototype.replace;
const originalSymbolReplace = RegExp.prototype[Symbol.replace];

try {
  RegExp.prototype.exec = function (value) {
    return ['custom exec', value];
  };
  assert.deepStrictEqual(literalExec('zab'), ['custom exec', 'zab']);

  String.prototype.replace = function (pattern, replacement) {
    return `custom replace:${this}:${replacement}:${pattern.global}`;
  };
  assert.strictEqual(
    literalReplace('ab-ab'),
    'custom replace:ab-ab:x:true'
  );

  String.prototype.replace = originalReplace;
  RegExp.prototype[Symbol.replace] = function () {
    return 'custom symbol replace';
  };
  assert.strictEqual(literalReplace('ab-ab'), 'custom symbol replace');

} finally {
  RegExp.prototype.exec = originalExec;
  String.prototype.replace = originalReplace;
  RegExp.prototype[Symbol.replace] = originalSymbolReplace;
}

console.log('JIT regexp literal intrinsic tests passed');
