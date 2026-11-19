const assert = require('node:assert');

class A {
  get z() { return 4; }
  m() { return 'Am'; }
  static s() { return 'As'; }
}

class DeleteSuper extends A {
  field() { return delete super.z; }
  computed(key) { return delete super[key]; }
}
assert.throws(() => new DeleteSuper().field(), ReferenceError);
let evaluated = false;
assert.throws(() => new DeleteSuper().computed((evaluated = true, 'z')), ReferenceError);
assert.strictEqual(evaluated, true);
assert.strictEqual(new A().z, 4);

console.log('ok');
