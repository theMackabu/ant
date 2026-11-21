const assert = require('node:assert');

let a1; a1 = class {};
let a2; a2 = function () {};
let a3; a3 = () => {};
let a4; a4 ??= () => {};
let a5; a5 ||= function () {};
let a6 = 1; a6 &&= class {};
let a7 = null; a7 ??= class {};
let a8; a8 = function* () {};
let a9; a9 = async () => {};
var a10; a10 = function () {};
assert.deepStrictEqual(
  [a1.name, a2.name, a3.name, a4.name, a5.name, a6.name, a7.name, a8.name, a9.name, a10.name],
  ['a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7', 'a8', 'a9', 'a10']);

let kept; kept = function original() {};
let keptClass; keptClass = class Original {};
let ownName; ownName = class { static name = 'own'; };
assert.deepStrictEqual([kept.name, keptClass.name, ownName.name], ['original', 'Original', 'own']);

let d1, d2, d3, d4, d5;
({ d1 = () => {} } = {});
[d2 = class {}] = [];
({ renamed: d3 = function () {} } = {});
[d4 = async () => {}] = [undefined];
({ d5 = function given() {} } = {});
assert.deepStrictEqual([d1.name, d2.name, d3.name, d4.name, d5.name], ['d1', 'd2', 'd3', 'd4', 'given']);

const { e1 = () => {}, nested: { e2 = class {} } = {} } = {};
const [e3 = function () {}, [e4 = () => {}] = []] = [];
assert.deepStrictEqual([e1.name, e2.name, e3.name, e4.name], ['e1', 'e2', 'e3', 'e4']);

function defaults(p = () => {}, q = class {}, { r = function () {} } = {}, [t = () => {}] = [], u = function named() {}) {
  return [p.name, q.name, r.name, t.name, u.name];
}
const arrowDefaults = (p = () => {}, q = class {}) => [p.name, q.name];
class Methods { m(p = () => {}) { return p.name; } static s(q = function () {}) { return q.name; } }
for (let round = 0; round < 300; round++) {
  assert.deepStrictEqual(defaults(), ['p', 'q', 'r', 't', 'named']);
  assert.deepStrictEqual(arrowDefaults(), ['p', 'q']);
  assert.deepStrictEqual([new Methods().m(), Methods.s()], ['p', 'q']);
}

const members = {};
members.x = function () {};
members['y'] = () => {};
members.z = class {};
const list = [];
list[0] = () => {};
assert.deepStrictEqual([members.x.name, members.y.name, members.z.name, list[0].name], ['', '', '', '']);

let viaComma = (0, function () {});
let viaTernary; viaTernary = true ? () => {} : null;
let viaLogical; viaLogical = true && (() => {});
assert.deepStrictEqual([viaComma.name, viaTernary.name, viaLogical.name], ['', '', '']);
let parenthesized; parenthesized = (() => {});
assert.strictEqual(parenthesized.name, 'parenthesized');

(function () {
  'use strict';
  let s1; s1 = () => {};
  let s2; s2 ||= class {};
  assert.deepStrictEqual([s1.name, s2.name], ['s1', 's2']);
})();

{
  let named;
  (named) = function () {};
  assert.strictEqual(named.name, '');
  (named) = class {};
  assert.strictEqual(named.name, '');
  named = undefined;
  (named) ??= () => 1;
  assert.strictEqual(named.name, '');
  named = function () {};
  assert.strictEqual(named.name, 'named');
}

console.log('ok');
