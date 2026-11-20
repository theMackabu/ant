const assert = require('node:assert');

{
  const sloppy = (src, ...args) => new Function('g', 'list', src)(...args);
  assert.deepStrictEqual(sloppy('var a; [undefined, a] = [1, 2]; return [a, typeof undefined]'), [2, 'undefined']);
  assert.deepStrictEqual(sloppy('var a; [undefined, undefined, a] = [1, 2, 3]; return a'), 3);
  assert.strictEqual(sloppy('var a; ({ x: undefined, y: a } = { x: 1, y: 2 }); return a'), 2);
  assert.strictEqual(sloppy('var a; ({ x: undefined = 4, ...a } = { y: 2 }); return a.y'), 2);
  assert.strictEqual(sloppy('var s = 0; for (undefined of [1, 2, 3]) s++; return s'), 3);
  assert.strictEqual(sloppy('var s = 0; for (undefined in { a: 1, b: 2 }) s++; return s'), 2);
  assert.strictEqual(sloppy('var s = 0; for ([undefined, s] of [[1, 2], [3, 4]]); return s'), 4);
  assert.strictEqual(sloppy('var a; [globalThis, a] = [g, 2]; return a', globalThis), 2);
  assert.strictEqual(sloppy('var a; ({ x: globalThis, y: a } = { x: g, y: 2 }); return a', globalThis), 2);
  assert.strictEqual(sloppy('var s = 0; for (globalThis of [g, g, g]) s++; return s', globalThis), 3);
  assert.deepStrictEqual(sloppy('return [undefined] = list', null, [1]), [1]);
  assert.strictEqual(sloppy('var s = 0; for (var i = 0; i < 200000; i++) { [undefined] = [i]; s += i } return s'), 19999900000);
  assert.strictEqual(sloppy('var s = 0; for (undefined of list) s++; return s', null, Array.from({ length: 200000 })), 200000);
  assert.strictEqual(sloppy('var a = 1, b = 2; for (var i = 0; i < 100001; i++) [undefined, a, b] = [0, b, a]; return a'), 2);
}

console.log('ok');
