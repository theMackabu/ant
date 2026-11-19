const assert = require('node:assert');

class StaticBase { static sx() { return 'sx'; } }
class StaticBlocks extends StaticBase {
  static {
    let a = 1;
    var b = 2;
    StaticBlocks.early = declared();
    function declared() { return a + b; }
    StaticBlocks.viaSuper = (() => super.sx())();
  }
  static {
    function second() { return 'second'; }
    StaticBlocks.second = second();
  }
}
assert.strictEqual(StaticBlocks.early, 3);
assert.strictEqual(StaticBlocks.viaSuper, 'sx');
assert.strictEqual(StaticBlocks.second, 'second');
assert.strictEqual(typeof declared, 'undefined');

console.log('ok');
