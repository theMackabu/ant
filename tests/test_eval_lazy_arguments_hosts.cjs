const assert = require('node:assert');

const id = (s) => s;
const G = globalThis;
const failed = [];
const moduleArguments = arguments;

function outcome(fn, round) {
  try { return fn(round) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

function check(name, fn, want, rounds = 24) {
  for (let round = 0; round < rounds; round++) {
    const got = outcome(fn, round);
    try { assert.deepStrictEqual(got, want) } catch (e) {
      failed.push(`${name} round ${round}: ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
      return;
    }
  }
}

const sOne = id('1');
const sLen = id('arguments.length');
const sA0 = id('arguments[0]');
const sAll = id('[arguments.length, arguments[0], arguments[1], arguments[2]]');
const sArgs = id('arguments');
const sSetA0 = id('arguments[0] = "w"');
const sSetA = id('a = "e"');
const sSetB = id('b = "e"');
const sType = id('typeof arguments');
const sDelete = id('delete arguments');
const sAssign = id('arguments = 1');
const sReadA = id('a');
const sArrowA = id('() => { a = "c" }');
const sArrowArgs = id('() => arguments');
const sFnArgs = id('(function () { return arguments })');
const sNested = id('eval(sA0)');
const sNestedLen = id('eval("eval(sLen)")');
const sNestedSetA = id('eval(sSetA)');
const sVarArgs = id('var arguments = 8');
const sStrictAssign = id('"use strict"; arguments = 1');

const box = { src: sOne };
const hostsSource = `({
  sloppy: function (a, b) { return eval(box.src) },
  strict: function (a, b) { 'use strict'; return eval(box.src) },
  dflt: function (a, b = 'B') { return eval(box.src) },
  rest: function (a, ...b) { return eval(box.src) },
  destr: function (a, { b } = { b: 'D' }) { return eval(box.src) },
  strictDflt: (function () { 'use strict'; return function (a, b = 'B') { return eval(box.src) } })(),
})`;
const scope = { box, id, sOne, sLen, sA0, sAll, sArgs, sSetA0, sSetA, sSetB, sNestedSetA, sArrowA };
const isolate = (source) => new Function(...Object.keys(scope), 'return ' + source)(...Object.values(scope));
const unmapped = ['strict', 'dflt', 'rest', 'destr', 'strictDflt'];
const kinds = ['sloppy'].concat(unmapped);
const modes = { inherited: (source) => eval(source), isolated: isolate };

for (const mode of Object.keys(modes)) {
  const hosts = modes[mode](hostsSource);
  const call = (kind, source, ...args) => { box.src = source; return hosts[kind](...args) };
  for (const kind of kinds) {
    check(`${mode} ${kind} none`, () => call(kind, sAll), [0, undefined, undefined, undefined]);
    check(`${mode} ${kind} fewer`, () => call(kind, sAll, 'x'), [1, 'x', undefined, undefined]);
    check(`${mode} ${kind} exact`, () => call(kind, sAll, 'x', { b: 'y' }), [2, 'x', { b: 'y' }, undefined]);
    check(`${mode} ${kind} more`, () => call(kind, sAll, 'x', { b: 'y' }, 'z', 'q'), [4, 'x', { b: 'y' }, 'z']);
    check(`${mode} ${kind} length`, () => call(kind, sLen, 1, 2, 3), 3);
    check(`${mode} ${kind} mixed sources`, () => [call(kind, sOne, 1, 2, 3), call(kind, sLen, 1, 2, 3), call(kind, sOne, 1, 2), call(kind, sLen, 1, 2)], [1, 3, 1, 2]);
    check(`${mode} ${kind} undefined argument`, () => call(kind, sAll, undefined, undefined), [2, undefined, undefined, undefined]);
    check(`${mode} ${kind} typeof`, () => call(kind, sType, 1), 'object');
    check(`${mode} ${kind} identity`, () => { box.src = sArgs; const h = hosts[kind]; return h(1) !== h(1) }, true);
    const sloppy = !kind.startsWith('strict');
    check(`${mode} ${kind} delete`, () => { box.src = id('[delete arguments, typeof arguments, arguments.length]'); return hosts[kind](1) }, sloppy ? [false, 'object', 1] : 'throw SyntaxError');
    check(`${mode} ${kind} delete then read`, () => { const h = hosts[kind]; box.src = sDelete; const r = [outcome(() => h(1))]; box.src = sLen; r.push(h(1, 2)); return r }, [sloppy ? false : 'throw SyntaxError', 2]);
  }
}

function sloppyWrites(a, b) {
  const r = [];
  a = 'p';
  r.push(eval(sA0));
  eval(sSetA0);
  r.push(a);
  eval(sSetA);
  r.push(eval(sA0), a, eval(sLen));
  return r;
}
check('sloppy writes', () => sloppyWrites(1, 2), ['p', 'w', 'e', 'e', 2]);
check('sloppy writes fewer', () => sloppyWrites(), [undefined, 'p', 'w', 'e', 0]);
check('sloppy writes more', () => sloppyWrites(1, 2, 3), ['p', 'w', 'e', 'e', 3]);

function sloppyWritesLate(a, b) {
  const r = [eval(sOne)];
  a = 'p';
  eval(sSetB);
  r.push(eval(sAll), a, b);
  arguments[1] = 'n';
  r.push(eval(sReadA), b);
  return r;
}
check('sloppy writes after first eval', () => sloppyWritesLate(1, 2), [1, [2, 'p', 'e', undefined], 'p', 'e', 'p', 'n']);

function strictWrites(a, b) {
  'use strict';
  const r = [];
  a = 'p';
  r.push(eval(sA0));
  eval(sSetA0);
  r.push(a);
  eval(sSetA);
  r.push(eval(sA0), a, eval(sLen));
  return r;
}
check('strict writes', () => strictWrites(1, 2), [1, 'p', 'w', 'e', 2]);
check('strict writes fewer', () => strictWrites(), [undefined, 'p', 'w', 'e', 0]);
check('strict writes more', () => strictWrites(1, 2, 3), [1, 'p', 'w', 'e', 3]);

function strictWritesLate(a, b) {
  'use strict';
  const r = [eval(sOne)];
  a = 'p';
  r.push(eval(sAll), a);
  eval(sSetA0);
  r.push(eval(sA0), a);
  return r;
}
check('strict writes after first eval', () => strictWritesLate(1, 2), [1, [2, 1, 2, undefined], 'p', 'w', 'p']);

function strictEvalWrites(a, b) {
  'use strict';
  const r = [eval(sSetA)];
  r.push(eval(sAll), a);
  eval(sSetB);
  r.push(eval(sAll), b);
  return r;
}
check('strict eval writes first', () => strictEvalWrites(1, 2), ['e', [2, 1, 2, undefined], 'e', [2, 1, 2, undefined], 'e']);
check('strict eval writes first, fewer', () => strictEvalWrites(), ['e', [0, undefined, undefined, undefined], 'e', [0, undefined, undefined, undefined], 'e']);

const writers = {
  dflt: function (a, b = 0) { const r = []; a = 'p'; r.push(eval(sA0)); eval(sSetA0); r.push(a); eval(sSetA); r.push(eval(sA0), a, eval(sLen)); return r },
  rest: function (a, ...b) { const r = []; a = 'p'; r.push(eval(sA0)); eval(sSetA0); r.push(a); eval(sSetA); r.push(eval(sA0), a, eval(sLen)); return r },
  destr: function (a, { b } = {}) { const r = []; a = 'p'; r.push(eval(sA0)); eval(sSetA0); r.push(a); eval(sSetA); r.push(eval(sA0), a, eval(sLen)); return r },
};
const lateWriters = {
  dflt: function (a, b = 0) { const r = [eval(sOne)]; a = 'p'; r.push(eval(sAll), a); eval(sSetA0); r.push(eval(sA0), a); return r },
  rest: function (a, ...b) { const r = [eval(sOne)]; a = 'p'; b = 0; r.push(eval(sAll), a); eval(sSetA0); r.push(eval(sA0), a); return r },
  destr: function (a, [b] = []) { const r = [eval(sOne)]; a = 'p'; b = 0; r.push(eval(sAll), a); eval(sSetA0); r.push(eval(sA0), a); return r },
};
for (const kind of Object.keys(writers)) {
  check(`${kind} writes`, () => writers[kind](1, [2]), [1, 'p', 'w', 'e', 2]);
  check(`${kind} writes fewer`, () => writers[kind](), [undefined, 'p', 'w', 'e', 0]);
  check(`${kind} writes more`, () => writers[kind](1, [2], 3), [1, 'p', 'w', 'e', 3]);
  check(`${kind} writes after first eval`, () => lateWriters[kind](1, [2], 3), [1, [3, 1, [2], 3], 'p', 'w', 'p']);
  check(`${kind} writes after first eval, fewer`, () => lateWriters[kind](1), [1, [1, 1, undefined, undefined], 'p', 'w', 'p']);
}

function strictArgsWriteFirst(a, b) {
  'use strict';
  eval(sSetA0);
  a = 'p';
  return [eval(sA0), a];
}
check('strict arguments write then parameter write', () => strictArgsWriteFirst(1), ['w', 'p']);

const snapshotsSource = `({
  assign: function (a) { 'use strict'; a = 2; return [eval(sA0), a] },
  assignAfterEval: function (a) { 'use strict'; eval(sOne); a = 2; return [eval(sA0), a] },
  assignUnusedEval: function (a) { 'use strict'; a = 2; if (a === 1) eval(sOne); return [eval(sA0), a] },
  second: function (a, b) { 'use strict'; b = 2; return [eval(sAll), b] },
  arrow: function (a) { 'use strict'; (() => { a = 2 })(); return [eval(sA0), a] },
  closure: function (a) { 'use strict'; (function () { a = 2 })(); return [eval(sA0), a] },
  lateClosure: function (a) { 'use strict'; const set = () => { a = 2 }; eval(sOne); set(); return [eval(sA0), a] },
  destructure: function (a) { 'use strict'; [a] = [2]; return [eval(sA0), a] },
  objectDestructure: function (a) { 'use strict'; ({ v: a } = { v: 2 }); return [eval(sA0), a] },
  update: function (a) { 'use strict'; a++; return [eval(sA0), a] },
  prefix: function (a) { 'use strict'; --a; ++a; ++a; return [eval(sA0), a] },
  compound: function (a) { 'use strict'; a += 1; return [eval(sA0), a] },
  logical: function (a) { 'use strict'; a &&= 2; return [eval(sA0), a] },
  forIn: function (a) { 'use strict'; for (a in { 2: 0 }); return [eval(sA0), +a] },
  forOf: function (a) { 'use strict'; for (a of [2]); return [eval(sA0), a] },
  declaration: function (a) { 'use strict'; function a() {} return [eval(sA0), typeof a] },
  varInit: function (a) { 'use strict'; var a = 2; return [eval(sA0), a] },
  previousEval: function (a) { 'use strict'; eval(sSetA); return [eval(sA0), a] },
  nestedEval: function (a) { 'use strict'; eval(sNestedSetA); return [eval(sA0), a] },
  evalClosure: function (a) { 'use strict'; const set = eval(sArrowA); set(); return [eval(sA0), a] },
  innerFunctionEval: function (a) { 'use strict'; function inner() { return eval(sSetA) } inner(); return [eval(sA0), a] },
  innerArrowEval: function (a) { 'use strict'; const inner = () => eval(sSetA); inner(); return [eval(sA0), a] },
  innerMethodEval: function (a) { 'use strict'; ({ m() { eval(sSetA) } }).m(); return [eval(sA0), a] },
  literalEval: function (a) { 'use strict'; eval('a = 2'); return [eval(sA0), a] },
  literalUpdate: function (a) { 'use strict'; eval('a++'); return [eval(sA0), a] },
  literalDestructure: function (a) { 'use strict'; eval('[a] = [2]'); return [eval(sA0), a] },
  literalFunction: function (a) { 'use strict'; eval('(function () { a = 2 })')(); return [eval(sA0), a] },
  literalRead: function (a) { 'use strict'; return [eval(sA0) + eval('a') - 1, eval('a + 1')] },
  literalEvalClosure: function (a) { 'use strict'; const set = eval('() => { a = 2 }'); set(); return [eval(sA0), a] },
  literalEvalDeclares: function (a) { 'use strict'; eval('var q = 1; a = q + 1'); return [eval(sA0), a] },
  literalNestedEval: function (a) { 'use strict'; eval('eval(sSetA)'); return [eval(sA0), a] },
  literalReader: function (a) { 'use strict'; a = 2; return [eval('var q = 1; eval(sA0)'), a] },
  literalOnly: function (a) { 'use strict'; a = 2; return [eval('let q = 0; eval("arguments[" + q + "]")'), a] },
  staticBlockEval: function (a) { 'use strict'; class C { static { eval(sSetA) } } return [eval(sA0), a] },
  computedKeyEval: function (a) { 'use strict'; class C { [eval(sSetA)]() {} } return [eval(sA0), a] },
  fieldEval: function (a) { 'use strict'; class C { x = eval(sSetA) } new C(); return [eval(sA0), a] },
  getterEval: function (a) { 'use strict'; const o = { get g() { return eval(sSetA) } }; o.g; return [eval(sA0), a] },
  generatorEval: function (a) { 'use strict'; function* g() { yield eval(sSetA) } g().next(); return [eval(sA0), a] },
  lengthOnly: function (a) { 'use strict'; a = 2; return [eval(sA0), a, arguments.length] },
  catchParameter: function (a) { 'use strict'; try { throw 2 } catch (e) { a = e } return [eval(sA0), a] },
  loop: function (a) { 'use strict'; for (let i = 0; i < 3; i++) a = a + 1; return [eval(sA0), a] },
  defaults: function (a = 0) { a = 2; return [eval(sA0), a] },
  defaultsAfterEval: function (a = 0) { eval(sOne); a = 2; return [eval(sA0), a] },
  defaultInitializer: function (a, b = (a = 2)) { return [eval(sA0), a] },
  defaultClosure: function (a, b = () => { a = 2; return a }) { const seen = b(); return [eval(sA0), seen] },
  defaultEval: function (a, b = eval(sSetA)) { return [eval(sA0), a] },
  defaultPreviousEval: function (a = 0) { eval(sSetA); return [eval(sA0), a] },
  rest: function (a, ...r) { a = 2; r[0] = 9; r = 0; return [eval(sAll), a] },
  restPreviousEval: function (a, ...r) { eval(sSetA); return [eval(sA0), a] },
  destructured: function (a, { v }) { a = 2; v = 3; return [eval(sA0), a, eval(sAll)[2].v] },
  destructuredPreviousEval: function (a, [v]) { eval(sSetA); return [eval(sA0), a] },
  strictDefaults: (function () { 'use strict'; return function (a = 0) { a = 2; return [eval(sA0), a] } })(),
  method: ({ m(a) { 'use strict'; a = 2; return [eval(sA0), a] } }).m,
  classMethod: (class { m(a) { a = 2; return [eval(sA0), a] } }).prototype.m,
  staticMethod: (class { static m(a, b = 1) { a = 2; return [eval(sA0), a] } }).m,
})`;
const snapshotWants = {
  second: [[2, 1, 'y', undefined], 2], forIn: [1, 2], declaration: [1, 'function'],
  previousEval: [1, 'e'], nestedEval: [1, 'e'], literalNestedEval: [1, 'e'], staticBlockEval: [1, 'e'],
  computedKeyEval: [1, 'e'], fieldEval: [1, 'e'], getterEval: [1, 'e'], generatorEval: [1, 'e'], evalClosure: [1, 'c'], innerFunctionEval: [1, 'e'],
  innerArrowEval: [1, 'e'], innerMethodEval: [1, 'e'], lengthOnly: [1, 2, 2], loop: [1, 4],
  defaultEval: [1, 'e'], defaultPreviousEval: [1, 'e'], rest: [[2, 1, 'y', undefined], 2],
  restPreviousEval: [1, 'e'], destructured: [1, 2, 4], destructuredPreviousEval: [1, 'e'],
};
for (const mode of Object.keys(modes)) {
  const snapshots = modes[mode](snapshotsSource);
  for (const name of Object.keys(snapshots)) {
    const second = name === 'destructured' ? { v: 4 } : name === 'destructuredPreviousEval' ? [4] : 'y';
    const run = /^default(Initializer|Closure|Eval)$/.test(name) ? () => snapshots[name](1) : () => snapshots[name](1, second);
    check(`${mode} snapshot ${name}`, run, snapshotWants[name] || [1, 2]);
  }
}

check('default parameter replaces undefined', () => (function (a = 'd', b = 'e') { return [eval(sAll), a, b] })(undefined), [[1, undefined, undefined, undefined], 'd', 'e']);
check('default parameter keeps the argument', () => (function (a = 'd') { return [eval(sAll), a] })(null), [[1, null, undefined, undefined], null]);
check('rest keeps every argument', () => (function (...r) { r.length = 0; return eval(sAll) })(1, 2, 3, 4), [4, 1, 2, 3]);
check('unmapped write through arguments', () => (function (a, b = 0) { eval(sSetA0); return [a, eval(sA0)] })(1), [1, 'w']);
check('unmapped parameter write after binding', () => (function (a, ...r) { const args = eval(sArgs); a = 2; return [args[0], eval(sA0)] })(1), [1, 1]);

check('parameter named arguments', () => (function (arguments) { return eval(sArgs) })(5), 5);
check('var arguments with a value', () => (function () { var arguments = 5; return eval(sArgs) })(1), 5);
check('var arguments without a value', () => (function () { var arguments; return [eval(sType), eval(sLen)] })(1), ['object', 1]);
check('function named arguments', () => (function () { function arguments() {} return eval(sType) })(1), 'function');
check('let arguments', () => (function () { let arguments = 6; return eval(sArgs) })(1), 6);
check('const arguments', () => (function () { const arguments = 7; return eval(sArgs) })(1), 7);
check('block let arguments', () => (function () { const r = []; { let arguments = 6; r.push(eval(sArgs)) } r.push(eval(sLen)); return r })(1), [6, 1]);
check('block const arguments around a default host eval', () => (function (a = 0) { const r = [eval(sLen)]; { const arguments = 7; r.push(eval(sArgs)) } a = 2; r.push(eval(sA0)); return r })(1), [1, 7, 1]);
check('var arguments from an earlier eval', () => (function () { eval(sVarArgs); return [eval(sArgs), eval(sType)] })(1), [8, 'number']);
check('var arguments from an earlier eval, read first', () => (function () { const before = eval(sLen); eval(sVarArgs); return [before, eval(sArgs)] })(1, 2), [2, 8]);
check('var arguments from an earlier eval in a default host', () => (function (a = 0) { eval(sVarArgs); return eval(sArgs) })(1), 8);
check('catch parameter named arguments', () => (function () { try { throw 9 } catch (arguments) { return eval(sArgs) } })(1), 9);

check('arrow in a sloppy host', () => (function (a) { a = 2; return (() => eval(sAll))() })(1, 'y'), [2, 2, 'y', undefined]);
check('arrow in a strict host', () => (function (a) { 'use strict'; a = 2; return (() => eval(sAll))() })(1, 'y'), [2, 1, 'y', undefined]);
check('arrow in a default host', () => (function (a, b = 0) { a = 2; return (() => eval(sAll))() })(1), [1, 1, undefined, undefined]);
check('arrow in a rest host', () => (function (...r) { r = 0; return (() => () => eval(sAll))()() })(1, 2), [2, 1, 2, undefined]);
check('arrow after the host returned', () => (function (a) { 'use strict'; return () => eval(sA0) })(1)(), 1);
check('arrow as a default initializer', () => (function (a, b = () => eval(sLen)) { return b() })(1), 1);
const topArrow = () => eval(sArgs);
check('arrow at module level', () => topArrow(1, 2) === moduleArguments, true);
check('arrow at script level', () => (0, eval)('(() => { const s = "argu" + "ments"; return eval(s) })()'), 'throw ReferenceError', 3);
check('arrow at script level, typeof', () => (0, eval)('(() => { const s = "typeof argu" + "ments"; return eval(s) })()'), 'undefined', 3);

check('generator', () => {
  function* g(a) { 'use strict'; a = 2; yield eval(sOne); yield eval(sAll) }
  const it = g(1, 'y', 'z');
  return [it.next().value, it.next().value];
}, [1, [3, 1, 'y', 'z']]);
check('generator with defaults', () => {
  function* g(a, b = 0) { yield eval(sOne); a = 2; yield eval(sAll) }
  const it = g(1, undefined, 'z');
  return [it.next().value, it.next().value];
}, [1, [3, 1, undefined, 'z']]);
check('sloppy generator', () => {
  function* g(a) { yield eval(sOne); a = 2; yield eval(sAll) }
  const it = g(1, 'y');
  return [it.next().value, it.next().value];
}, [1, [2, 2, 'y', undefined]]);

const asyncGot = [];
const asyncWant = [];
function later(name, promise, want) {
  asyncWant.push([name, want]);
  promise.then((value) => asyncGot.push([name, value]), (e) => asyncGot.push([name, 'throw ' + e.constructor.name]));
}
for (let round = 0; round < 4; round++) {
  later('async function', (async function (a) { 'use strict'; a = 2; await null; return eval(sAll) })(1, 'y'), [2, 1, 'y', undefined]);
  later('async function with rest', (async function (a, ...r) { eval(sOne); await null; a = 2; r = 0; return eval(sAll) })(1, 'y'), [2, 1, 'y', undefined]);
  later('sloppy async function', (async function (a) { a = 2; await null; return eval(sAll) })(1, 'y'), [2, 2, 'y', undefined]);
  later('async arrow', (function (a) { 'use strict'; a = 2; return (async () => { await null; return eval(sAll) })() })(1, 'y'), [2, 1, 'y', undefined]);
  later('async arrow in a default host', (function (a, b = 0) { a = 2; return (async () => { await null; return eval(sLen) })() })(1), 1);
  later('async method', ({ async m(a, b = 0) { await null; a = 2; return eval(sA0) } }).m(1), 1);
}

class Base {
  constructor(a, b = 0) { a = 2; this.seen = eval(sAll) }
  m(a) { a = 2; return eval(sAll) }
  get g() { return eval(sLen) }
  set s(v) { v = 2; this.set = eval(sAll) }
  static sm(a, ...r) { a = 2; return eval(sAll) }
}
class Derived extends Base {
  constructor(a) { a = 2; const before = eval(sA0); super(...eval(sArgs)); this.before = before; this.after = eval(sAll) }
}
class Fields { x = eval(sArgs) }
class StaticField { static make() { return class { static x = eval(sType) } } }
class StaticBlock { static make() { return class { static { eval(sLen) } } } }
class FieldArrow { x = () => eval(sArgs) }
class FieldFunction { x = eval(sFnArgs)(1, 2).length }
check('base constructor', () => new Base(1, 'y', 'z').seen, [3, 1, 'y', 'z']);
check('method', () => new Base().m(1, 'y'), [2, 1, 'y', undefined]);
check('getter', () => new Base().g, 0);
check('setter', () => { const o = new Base(); o.s = 1; return o.set }, [1, 1, undefined, undefined]);
check('static method', () => Base.sm(1, 'y'), [2, 1, 'y', undefined]);
check('derived constructor', () => { const d = new Derived(1, 'y'); return [d.before, d.seen, d.after] }, [1, [2, 1, 'y', undefined], [2, 1, 'y', undefined]]);
check('class field', () => new Fields(1), 'throw SyntaxError', 4);
check('static class field', () => StaticField.make(1), 'throw SyntaxError', 4);
check('static block', () => StaticBlock.make(1), 'throw SyntaxError', 4);
check('arrow in a class field', () => new FieldArrow().x(), 'throw SyntaxError', 4);
check('function in a class field', () => new FieldFunction().x, 2, 4);
check('object method', () => ({ m(a, b) { b = 2; return eval(sAll) } }).m(1, 'y'), [2, 1, 2, undefined]);
check('object getter', () => ({ get g() { 'use strict'; return eval(sAll) } }).g, [0, undefined, undefined, undefined]);
check('new on a strict function', () => new (function (a) { 'use strict'; a = 2; this.v = eval(sA0) })(1).v, 1);

check('closure reads after return', () => (function (a) { 'use strict'; return eval(sArrowArgs) })(1, 2)().length, 2);
check('closure reads after return, default host', () => { const f = (function (a, b = 0) { a = 2; return eval(sArrowArgs) })(1); return [f()[0], f() === f()] }, [1, true]);
check('closure reads after return, sloppy host', () => { const f = (function (a) { a = 2; return eval(sArrowArgs) })(1); return f()[0] }, 2);
check('closure created before the binding', () => (function (a) { 'use strict'; const set = eval(sArrowA); const read = eval(sArrowArgs); set(); return [read()[0], a] })(1), [1, 'c']);
check('inner function has its own', () => (function (a) { 'use strict'; return eval(sFnArgs)(7, 8, 9).length + eval(sLen) })(1), 4);

function escaping(a, b) { 'use strict'; return eval(sArgs) }
function escapingDefaults(a, b = 0) { a = 'changed'; return eval(sArgs) }
function escapingSloppy(a, b) { const args = eval(sArgs); a = 'changed'; return args }
check('escaping arguments', (i) => {
  const first = escaping(i, 'x'), second = escaping('other');
  G.__lazyArgs = escapingDefaults(i, 2, 3);
  const third = escapingDefaults('later');
  const fourth = escapingSloppy(i);
  escapingSloppy('later');
  const got = [first[0] === i, first.length, second[0], second.length, G.__lazyArgs[0] === i, G.__lazyArgs.length, third[0], fourth[0], first !== second];
  delete G.__lazyArgs;
  return got;
}, [true, 2, 'other', 1, true, 3, 'later', 'changed', true]);

check('identity across evals', () => (function (a) { 'use strict'; return eval(sArgs) === eval(sArgs) })(1), true);
check('identity across evals, default host', () => (function (a = 0) { const first = eval(sArgs); eval(sOne); return first === eval(sArgs) })(1), true);
check('identity across evals, block scopes', () => (function (a) { 'use strict'; let first; { let x = 1; first = eval(sArgs) } { let y = 2; { let z = 3; return first === eval(sArgs) && first === eval(sNested.replace('sA0', 'sArgs')) } } })(1), true);
check('identity with the static object', () => (function (a) { 'use strict'; return arguments === eval(sArgs) })(1), true);
check('identity with the static object, default host', () => (function (a = 0) { return [arguments === eval(sArgs), eval(sArgs) === arguments] })(1), [true, true]);
check('identity with the static object, sloppy host', () => (function (a) { const args = eval(sArgs); return args === arguments })(1), true);
check('identity with arguments.length only', () => (function (a) { 'use strict'; a = 2; return [arguments.length, eval(sAll), eval(sArgs) === eval(sArgs)] })(1), [1, [1, 1, undefined, undefined], true]);

check('assignment in sloppy eval', () => (function (a) { const r = [eval(sAssign), eval(sArgs), eval(sType)]; return r })(5), [1, 1, 'number']);
check('assignment in sloppy eval, default host', () => (function (a, b = 0) { eval(sOne); return [eval(sAssign), eval(sArgs)] })(5), [1, 1]);
check('assignment in sloppy eval, static object', () => (function (a) { eval(sAssign); return arguments })(5), 1);
check('assignment in strict eval', () => (function (a) { return eval(sStrictAssign) })(5), 'throw SyntaxError', 4);
check('assignment in a strict host', () => (function (a) { 'use strict'; return eval(sAssign) })(5), 'throw SyntaxError', 4);
check('var arguments in a strict host', () => (function (a) { 'use strict'; return eval(sVarArgs) })(5), 'throw SyntaxError', 4);
check('strict eval in a sloppy host', () => (function (a) { a = 2; return eval(id('"use strict"; [arguments.length, arguments[0]]')) })(1), [1, 2]);
check('strict eval in a default host', () => (function (a, b = 0) { a = 2; return eval(id('"use strict"; [arguments.length, arguments[0]]')) })(1), [1, 1]);

const manyNames = Array.from({ length: 70 }, (_, i) => 'p' + i);
const many = new Function('sAll', 'sLast', `return function (${manyNames.join(', ')}) { 'use strict'; p69 = 'x'; return [eval(sAll), eval(sLast), p0, p69] }`)(sAll, id('arguments[69]'));
check('seventy parameters', () => many(...manyNames), [[70, 'p0', 'p1', 'p2'], 'p69', 'p0', 'x']);

check('nested eval', () => (function (a) { 'use strict'; a = 2; return eval(sNested) })(1), 1);
check('nested eval twice', () => (function (a, b = 0) { return eval(sNestedLen) })(1, 2, 3), 3);
check('nested eval after a plain eval', () => (function (a) { 'use strict'; eval(sOne); return [eval(sNested), eval(sNestedLen)] })(1), [1, 1]);
check('eval in nested blocks', () => (function (a) { 'use strict'; const r = []; { let x = 1; r.push(eval(sOne)); { let y = 2; a = 2; r.push(eval(sA0)); { let z = 3; r.push(eval(sLen), eval(id('x + y + z'))) } } } return r })(1), [1, 1, 1, 6]);
check('eval in nested blocks, eval write', () => (function (a) { 'use strict'; { let x = 1; { let y = 2; eval(sSetA) } } { let z = 3; return [eval(sA0), a] } })(1), [1, 'e']);
check('eval in a loop body', () => (function (a) { 'use strict'; let total = 0; for (let i = 0; i < 4; i++) { let step = i; total += i === 3 ? eval(sA0) : eval(sOne); a = step } return [total, a] })(10), [13, 3]);
check('eval in a default initializer', () => (function (a, b = eval(sLen)) { return b })(1), 1);
check('eval in a default initializer, more', () => (function (a, b = eval(sAll)) { return b })(1, undefined, 3), [3, 1, undefined, 3]);
check('eval in a default initializer after a write', () => (function (a, b = (a = 2), c = eval(sA0)) { return [c, a] })(1), [1, 2]);
check('eval write in a default initializer', () => (function (a, b = eval(sSetA), c = eval(sA0)) { return [b, c, a] })(1), ['e', 1, 'e']);
check('eval in a strict default initializer', () => (function () { 'use strict'; return function (a, b = eval(sArgs)) { a = 2; return [b === eval(sArgs), b[0], b.length] } })()(1), [true, 1, 1]);
check('parameter scope', () => (function (a, b = () => a, c = eval(id('var q = 1; a'))) { var a = 2; return [b(), c, a, eval(sA0), typeof q] })(1), [1, 1, 2, 1, 'number']);
check('parameter scope closure over arguments', () => (function (a, b = eval(sArrowArgs)) { var arguments = 3; return [b()[0], b().length, arguments] })(1), [1, 1, 3]);
check('body eval var over a parameter', () => (function (a, b = 0) { eval(id('var a = 2')); return [a, eval(sA0)] })(1), [2, 1]);
check('body eval var over a parameter, sloppy', () => (function (a) { eval(id('var a = 2')); return [a, eval(sA0)] })(1), [2, 2]);

function withVars(a, b) { eval(id('var v1 = a')); return [eval(sLen), eval(id('v1 + b')), typeof v1] }
check('eval var and arguments', () => withVars(1, 2, 3), [3, 3, 'number']);
check('recursion', () => { function r(n, a) { 'use strict'; if (n) { const below = r(n - 1, a + 1); return below + eval(sA0) + eval(sLen) } return eval(id('arguments[1]')) } return r(3, 10) }, 25);
check('recursion in blocks', () => { function r(n) { 'use strict'; { let x = n; if (x) { const below = r(x - 1, 'extra'); return below + eval(sLen) } return eval(sLen) } } return r(3) }, 2 + 2 + 2 + 1);
check('reentrant host', () => { function r(a, f) { 'use strict'; if (f) f(); return eval(sA0) } return r(1, () => r(2, () => r(3))) }, 1);

function churn() {
  let kept = null;
  for (let i = 0; i < 60000; i++) kept = { i, pad: [i, i + 1] };
  return kept.i;
}
function gcStrict(a, b) {
  'use strict';
  eval(sOne);
  const args = eval(sArgs);
  churn();
  return [args.length, args[0].v, args[1], eval(sArgs) === args];
}
function gcDefaults(a, b = { fresh: 1 }) {
  eval(sOne);
  churn();
  a = null;
  return eval(sArrowArgs);
}
function gcOldEnvironment(a) {
  'use strict';
  const read = eval(id('() => eval(sArgs)'));
  churn();
  const args = eval(sArgs);
  churn();
  return [read, args];
}
const wideNames = [];
for (let i = 0; i < 66000; i++) wideNames.push('v' + i);
const wideLocals = `'use strict'; let ${wideNames.join(',')};`;
const wide = eval(`(function wide(a, n) { ${wideLocals} { let x = 1; const first = eval(sOne); { let y = 2; if (n) { const below = wide('inner', n - 1, 0); const mine = eval(sArgs); return [first, below, mine[0], mine.length, mine === eval(sArgs), x + y] } return [eval(sA0), eval(sLen)] } } })`);
const wideSnapshot = eval(`(function (a) { ${wideLocals} a = 2; { let x = 1; eval(sOne); { let y = 2; return [eval(sA0), a, eval(sArgs) === eval(sArgs)] } } })`);
const wideWrite = eval(`(function (a) { ${wideLocals} { let x = 1; eval(sSetA); { let y = 2; return [eval(sA0), a] } } })`);
const wideClosure = eval(`(function (a) { ${wideLocals} { let x = 1; eval(sOne); churn(); { let y = 2; return eval(sArrowArgs) } } })`);
check('wide scope chain', () => wide('outer', 2), [1, [1, ['inner', 3], 'inner', 3, true, 3], 'outer', 2, true, 3], 6);
check('wide scope chain snapshot', () => wideSnapshot(1), [1, 2, true], 6);
check('wide scope chain eval write', () => wideWrite(1), [1, 'e'], 6);

const gcKept = [];
for (let round = 0; round < 4; round++) {
  const wideRead = wideClosure({ v: round }, 'w' + round);
  churn();
  assert.deepStrictEqual([wideRead()[0].v, wideRead()[1], wideRead().length], [round, 'w' + round, 2]);
  assert.deepStrictEqual(gcStrict({ v: round }, 'tail', round), [3, round, 'tail', true]);
  const read = gcDefaults({ v: round }, undefined, 'x' + round);
  gcKept.push(read);
  churn();
  assert.deepStrictEqual([read().length, read()[0].v, read()[1], read()[2]], [3, round, undefined, 'x' + round]);
  const [late, args] = gcOldEnvironment({ v: round }, 'y' + round);
  gcKept.push(late);
  churn();
  assert.deepStrictEqual([late() === args, late()[0].v, late()[1]], [true, round, 'y' + round]);
}
churn();
assert.deepStrictEqual(gcKept.map((f) => f()[0].v), [0, 0, 1, 1, 2, 2, 3, 3]);

function hot(a, b, c) { 'use strict'; b = a + 1; return eval(sA0) + eval(id('arguments[1]')) + arguments.length }
function hotDefaults(a, b = 1, ...c) { return eval(sLen) + (eval(sOne), a) }
function hotLiteral(a, b) { 'use strict'; a = a + 1; return eval('a + b') + arguments.length }
let hotTotal = 0, hotLiteralTotal = 0;
for (let i = 0; i < 3000; i++) hotTotal += hot(i, 1) + hotDefaults(i, undefined, 3);
for (let i = 0; i < 30000; i++) hotLiteralTotal += hotLiteral(i, 1, 0);
assert.strictEqual(hotTotal, 3000 * (1 + 2 + 3) + 2 * (2999 * 3000 / 2));
assert.strictEqual(hotLiteralTotal, 30000 * 5 + 29999 * 30000 / 2);

Promise.resolve().then(() => 0).then(() => 0).then(() => 0).then(() => {
  assert.deepStrictEqual(failed, []);
  assert.deepStrictEqual(asyncGot.slice().sort((x, y) => x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0), asyncWant.slice().sort((x, y) => x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0));
  console.log('ok');
});
