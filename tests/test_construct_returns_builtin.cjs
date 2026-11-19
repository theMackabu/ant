const assert = require('node:assert');

function ReturnsBuiltin() { return Math.max; }
function AssignsThenReturnsBuiltin() { this.a = 1; return parseInt; }
class ClassReturnsBuiltin { constructor() { return Math.min; } }
for (let i = 0; i < 3000; i++) {
  assert.strictEqual(new ReturnsBuiltin(), Math.max);
  assert.strictEqual(new AssignsThenReturnsBuiltin(), parseInt);
  assert.strictEqual(new ClassReturnsBuiltin(), Math.min);
  assert.strictEqual(Reflect.construct(ReturnsBuiltin, []), Math.max);
}

console.log('ok');
