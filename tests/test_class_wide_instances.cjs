const assert = require('node:assert');

const WARM = 3000;

class Wide { a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; g = 7; h = 8 }
class Wider extends Wide { i = 9; j = 10; k = 11; l = 12; m = 13; n = 14; o = 15; p = 16; q = 17; r = 18; s = 19; t = 20 }

function makeWide() { return new Wide(); }
function makeWider() { return new Wider(); }

function checkBasics() {
  const wide = makeWide();
  assert.deepStrictEqual(Object.keys(wide), ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h']);
  assert.strictEqual(wide.a + wide.h, 9);
  const wider = makeWider();
  assert.strictEqual(Object.keys(wider).join(''), 'abcdefghijklmnopqrst');
  assert.strictEqual(wider.t, 20);
  assert.strictEqual(wider.i + wider.a, 10);
  wider.extra = 21;
  assert.strictEqual(wider.extra, 21);
  assert.strictEqual(Object.keys(wider).length, 21);
}

for (let i = 0; i < WARM; i++) {
  assert.strictEqual(makeWide().h, 8);
  assert.strictEqual(makeWider().t, 20);
}
checkBasics();

console.log('ok');
