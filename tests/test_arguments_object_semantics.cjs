// Arguments objects keep dense element storage for speed but behave as the
// spec's ordinary objects: an own configurable length that elements and array
// methods follow, Object.prototype as prototype, Array.isArray false, a
// non-enumerable callee (a throwing accessor in strict code). Expected values
// are what Node prints for the same calls, checked cold and once compiled.
function hot(fn, n = 3000) { let r; for (let i = 0; i < n; i++) r = fn(); return r; }

function sliceArgs() { return Array.prototype.slice.call(arguments); }
function sliceArgs1() { return [].slice.call(arguments, 1); }
function spreadArgs() { return [...arguments]; }
function fromArgs() { return Array.from(arguments); }
function applyArgs() { return Math.max.apply(null, arguments); }
function forOf() { const r = []; for (const x of arguments) r.push(x); return r; }
function loopLen() { let s = 0; for (let i = 0; i < arguments.length; i++) s += arguments[i]; return s; }
function lenOnly() { return arguments.length; }
function shiftArgs() { const first = [].shift.call(arguments); return [first, arguments.length, [...arguments]]; }
function pushArgs() { const n = Array.prototype.push.call(arguments, 9, 8); return [n, arguments.length, arguments[3]]; }
function popArgs() { const v = [].pop.call(arguments); return [v, arguments.length, 1 in arguments]; }
function unshiftArgs() { const n = [].unshift.call(arguments, 0); return [n, Array.from(arguments)]; }
function spliceArgs() { const r = [].splice.call(arguments, 1, 1, 'x', 'y'); return [r, arguments.length, Array.from(arguments)]; }
function joinArgs() { return [].join.call(arguments, '-'); }
function mapArgs() { return [].map.call(arguments, x => x * 2); }
function indexOfArgs() { return [[].indexOf.call(arguments, 2), [].includes.call(arguments, 3)]; }
function concatArgs() { return [].concat(arguments).length; }
function reverseArgs() { [].reverse.call(arguments); return Array.from(arguments); }
function sortArgs() { [].sort.call(arguments); return Array.from(arguments); }
function mapped(a, b) { a = 10; arguments[1] = 20; return [arguments[0], b, arguments.length]; }
function mappedShift(a, b) { [].shift.call(arguments); return [a, b, arguments[0], arguments.length]; }
function strictLen() { 'use strict'; return [arguments.length, [...arguments]]; }
function diverged() { arguments.length = 1; return [Array.from(arguments), [...arguments], [].slice.call(arguments), Math.max.apply(null, arguments), [].join.call(arguments)]; }
function grown() { arguments[4] = 'e'; return [arguments.length, Array.from(arguments), 4 in arguments, Object.keys(arguments)]; }
function lengthGetter() { Object.defineProperty(arguments, 'length', { get() { return 1; } }); return [arguments.length, [].slice.call(arguments), Array.from(arguments)]; }
function deleted() { delete arguments.length; return [arguments.length, [].slice.call(arguments), Array.from(arguments)]; }
function frozen() { Object.freeze(arguments); let e = 'none'; try { [].push.call(arguments, 1); } catch (x) { e = x.name; } return [e, arguments.length, Object.isFrozen(arguments)]; }
function typeTag() { return [Object.prototype.toString.call(arguments), Array.isArray(arguments), typeof arguments.forEach, arguments instanceof Array, arguments.constructor === Object]; }
function keys() { return [Object.keys(arguments), Object.getOwnPropertyNames(arguments), Reflect.ownKeys(arguments).length]; }
function forIn() { const r = []; for (const k in arguments) r.push(k); return r; }
function descs() { return Object.getOwnPropertyDescriptors(arguments).length; }
function hasOwn() { return [arguments.hasOwnProperty('length'), Object.hasOwn(arguments, 'callee'), 'length' in arguments]; }
function nested() { return (() => arguments.length)(); }
function argsInArrow() { const f = () => [...arguments]; return f(); }
function lenLoopDel() { let s = 0; delete arguments.length; for (let i = 0; i < 3; i++) s += arguments.length === undefined ? 1 : 0; return s; }
function callee() { return arguments.callee === callee; }
function assignLen() { arguments.length = 10; return [arguments.length, Object.keys(arguments).length]; }

const cases = { sliceArgs: () => sliceArgs(1, 2, 3), sliceArgs1: () => sliceArgs1(1, 2, 3), spreadArgs: () => spreadArgs(1, 2), fromArgs: () => fromArgs(1, 2),
  applyArgs: () => applyArgs(1, 5, 2), forOf: () => forOf(1, 2, 3), loopLen: () => loopLen(1, 2, 3), lenOnly: () => lenOnly(1, 2),
  shiftArgs: () => shiftArgs(1, 2, 3), pushArgs: () => pushArgs(1, 2), popArgs: () => popArgs(1, 2), unshiftArgs: () => unshiftArgs(1, 2),
  spliceArgs: () => spliceArgs(1, 2, 3), joinArgs: () => joinArgs(1, 2), mapArgs: () => mapArgs(1, 2), indexOfArgs: () => indexOfArgs(1, 2, 3),
  concatArgs: () => concatArgs(1, 2), reverseArgs: () => reverseArgs(1, 2, 3), sortArgs: () => sortArgs(3, 1, 2), mapped: () => mapped(1, 2),
  mappedShift: () => mappedShift(1, 2), strictLen: () => strictLen(1, 2), diverged: () => diverged(5, 6, 7), grown: () => grown(1, 2),
  lengthGetter: () => lengthGetter('a', 'b'), deleted: () => deleted(1, 2), frozen: () => frozen(1), typeTag: () => typeTag(1), keys: () => keys(1, 2),
  forIn: () => forIn(1, 2), hasOwn: () => hasOwn(1), nested: () => nested(1, 2, 3), argsInArrow: () => argsInArrow(1, 2), lenLoopDel: () => lenLoopDel(1),
  callee: () => callee(), assignLen: () => assignLen(1, 2) };

function m1(a, b) { a = 5; return [...arguments]; }
function m2(a, b) { b = 7; return Array.prototype.slice.call(arguments); }
function m3(a) { a = 9; return Math.max.apply(null, arguments); }
function m4(a, b) { arguments[0] = 'x'; return [a, Array.from(arguments)]; }
function m5(a, b) { delete arguments[0]; a = 3; return [arguments[0], 0 in arguments, arguments.length]; }
function m6(a, b) { a = 'p'; return [].join.call(arguments); }
function m7(a, b) { a = 'q'; const r = []; for (const x of arguments) r.push(x); return r; }
function m8(a, b) { arguments.length = 1; a = 'z'; return [arguments[0], arguments[1], [...arguments]]; }
function s1() { 'use strict'; arguments.length = 0; return [arguments[0], [...arguments], arguments.length]; }
function s2() { 'use strict'; delete arguments.length; return [Object.getOwnPropertyNames(arguments), [].slice.call(arguments)]; }
function s3() { 'use strict'; let e; try { arguments.callee = 1; } catch (x) { e = x.name; } return e; }
function json() { return JSON.stringify(arguments); }
function rest(...r) { return [r.length, Array.isArray(r)]; }
function bind() { return (function () { return arguments.length; }).apply(null, arguments); }
function callArgs() { return Reflect.apply(Math.min, null, arguments); }
function newTarget() { return new (class { constructor() { this.n = arguments.length; } })(...arguments).n; }
function lenType() { return typeof arguments.length; }
const cases2 = { m1: () => m1(1, 2), m2: () => m2(1, 2), m3: () => m3(1, 2), m4: () => m4(1, 2), m5: () => m5(1, 2), m6: () => m6(1, 2),
  m7: () => m7(1, 2), m8: () => m8(1, 2), s1: () => s1(1, 2), s2: () => s2(1, 2), s3: () => s3(), json: () => json(1, 2), rest: () => rest(1, 2),
  bind: () => bind(1, 2, 3), callArgs: () => callArgs(4, 2, 6), newTarget: () => newTarget(1, 2), lenType: () => lenType() };

const expected = {
  sliceArgs: [1,2,3],
  sliceArgs1: [2,3],
  spreadArgs: [1,2],
  fromArgs: [1,2],
  applyArgs: 5,
  forOf: [1,2,3],
  loopLen: 6,
  lenOnly: 2,
  shiftArgs: [1,2,[2,3]],
  pushArgs: [4,4,8],
  popArgs: [2,1,false],
  unshiftArgs: [3,[0,1,2]],
  spliceArgs: [[2],4,[1,"x","y",3]],
  joinArgs: "1-2",
  mapArgs: [2,4],
  indexOfArgs: [1,true],
  concatArgs: 1,
  reverseArgs: [3,2,1],
  sortArgs: [1,2,3],
  mapped: [10,20,2],
  mappedShift: [2,2,2,1],
  strictLen: [2,[1,2]],
  diverged: [[5],[5],[5],5,"5"],
  grown: [2,[1,2],true,["0","1","4"]],
  lengthGetter: [1,["a"],["a"]],
  deleted: [null,[],[]],
  frozen: ["TypeError",1,true],
  typeTag: ["[object Arguments]",false,"undefined",false,true],
  keys: [["0","1"],["0","1","length","callee"],5],
  forIn: ["0","1"],
  hasOwn: [true,true,true],
  nested: 3,
  argsInArrow: [1,2],
  lenLoopDel: 3,
  callee: true,
  assignLen: [10,2],
  m1: [5,2],
  m2: [1,7],
  m3: 9,
  m4: ["x",["x",2]],
  m5: [null,false,2],
  m6: "p,2",
  m7: ["q",2],
  m8: ["z",2,["z"]],
  s1: [1,[],0],
  s2: [["0","1","callee"],[]],
  s3: "TypeError",
  json: "{\"0\":1,\"1\":2}",
  rest: [2,true],
  bind: 3,
  callArgs: 2,
  newTarget: 2,
  lenType: "number"
};

let failures = 0;
for (const [name, fn] of Object.entries({ ...cases, ...cases2 })) {
  const want = JSON.stringify(expected[name]);
  for (const [phase, run] of [['cold', fn], ['hot', () => hot(fn)]]) {
    let got;
    try { got = JSON.stringify(run()); } catch (e) { got = 'THROW ' + e.name + ': ' + e.message; }
    if (got !== want) { failures++; console.log(`FAIL ${name} (${phase}): ${got} !== ${want}`); }
  }
}
if (failures) throw new Error(`${failures} arguments object checks failed`);

// compiled reads of an arguments object's length that was redefined after creation
function len(a) { return a.length; }
function args() { return arguments; }
function strictArgs() { 'use strict'; return arguments; }
for (let i = 0; i < 20000; i++) { len(args(1, 2)); len(strictArgs(1)); len([1, 2, 3]); }
const lengthChecks = [
  ['plain', () => args(1, 2, 3), 3],
  ['strict', () => strictArgs(1), 1],
  ['assigned', () => ((() => { const a = args(1, 2); a.length = 7; return a; })()), 7],
  ['deleted', () => ((() => { const a = args(1, 2); delete a.length; return a; })()), undefined],
  ['getter', () => ((() => { const a = args(1, 2); Object.defineProperty(a, 'length', { get() { return 'g'; } }); return a; })()), 'g'],
  ['redefined', () => ((() => { const a = strictArgs(1); Object.defineProperty(a, 'length', { value: 'v', enumerable: true }); return a; })()), 'v'],
  ['inherited', () => ((() => { const a = args(1, 2); delete a.length; Object.prototype.length = 'p'; return a; })()), 'p'],
];
for (const [name, make, want] of lengthChecks) {
  const got = len(make());
  if (!Object.is(got, want)) throw new Error(`compiled length ${name}: ${String(got)} !== ${String(want)}`);
}
delete Object.prototype.length;

// slice copies dense arguments directly, except holes the prototype must fill
function sliceMapped(a, b) { const args = arguments; b = 'B'; return Array.prototype.slice.call(args); }
function sliceHole() { const args = arguments; delete args[1]; return [].slice.call(args); }
for (let i = 0; i < 5000; i++) { sliceMapped(1, 2, 3); sliceHole(1, 2, 3); }
if (JSON.stringify(sliceMapped(1, 2, 3)) !== '[1,"B",3]') throw new Error('slice of mapped arguments');
Object.prototype[1] = 'proto';
const inheritedSlice = sliceHole(1, 2, 3);
delete Object.prototype[1];
if (!Object.prototype.hasOwnProperty.call(inheritedSlice, 1) || inheritedSlice[1] !== 'proto')
  throw new Error('slice must read holes through the prototype');

const util = require('node:util');
const inspected = util.inspect((function () { return arguments; })(1, 'a'));
if (inspected !== "[Arguments] [ 1, 'a' ]") throw new Error('inspect: ' + inspected);
if (!util.types.isArgumentsObject((function () { delete arguments.callee; return arguments; })()))
  throw new Error('isArgumentsObject depends on callee');
if (util.types.isArgumentsObject([1])) throw new Error('isArgumentsObject accepts arrays');

console.log('PASS arguments objects follow the spec');
