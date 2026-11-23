const assert = require('node:assert');

function syntaxError(fn) {
  assert.throws(fn, SyntaxError);
}

const indirect = eval;

function Target(src) { return eval(src); }
assert.strictEqual(new Target('new.target'), Target);
assert.strictEqual(Target('new.target'), undefined);
assert.strictEqual(new Target('(() => new.target)()'), Target);
assert.strictEqual(new Target('eval("new.target")'), Target);
assert.strictEqual(new Target('(() => eval("new.target"))()'), Target);
assert.strictEqual(new Target('var nested = "new.target"; eval(nested)'), Target);
assert.strictEqual(Target('typeof function () { return new.target }'), 'function');
assert.strictEqual(Target('1 + 2'), 3);
syntaxError(() => indirect('new.target'));
syntaxError(() => indirect('() => new.target'));
syntaxError(() => indirect('({ [new.target]: 1 })'));
syntaxError(() => indirect('class C { [new.target]() {} }'));
assert.strictEqual(typeof indirect('(function () { return new.target })'), 'function');
assert.strictEqual(typeof indirect('(class { m() { return new.target } })'), 'function');
const arrow = (src) => eval(src);
assert.strictEqual(arrow('new.target'), undefined);
assert.strictEqual(new Function('return new.target')(), undefined);
assert.strictEqual(typeof new (new Function('this.target = new.target'))().target, 'function');

class Base { constructor() { this.base = new.target; } }
class Derived extends Base {
  constructor(src) { eval(src); }
}
assert.strictEqual(new Derived('super()').base, Derived);
assert.strictEqual(new Derived('(() => super())()').base, Derived);
assert.strictEqual(new Derived('(() => { super(); })()').base, Derived);
assert.strictEqual(new Derived('eval("super()")').base, Derived);
assert.strictEqual(new Derived('(() => eval("(() => super())()"))()').base, Derived);
syntaxError(() => indirect('super()'));
syntaxError(() => indirect('() => super()'));
syntaxError(() => Target('super()'));

function plain(src) { return eval(src); }
async function asynchronous(src) { return eval(src); }
function* generator(src) { return eval(src); }
async function* asyncGenerator(src) { return eval(src); }
syntaxError(() => plain('await 1'));
syntaxError(() => plain('yield 1'));
syntaxError(() => plain('for await (const x of []);'));
syntaxError(() => plain('1 + (await 1)'));
syntaxError(() => plain('() => await 1'));
syntaxError(() => generator('yield 1').next());
syntaxError(() => generator('() => yield 1').next());
syntaxError(() => indirect('for await (const x of []);'));
syntaxError(() => indirect('"use strict"; yield 1'));
assert.strictEqual(plain('var await = 3; await'), 3);
assert.strictEqual(plain('var yield = 4; yield'), 4);
assert.strictEqual(typeof plain('(async function () { await 1 })'), 'function');
assert.strictEqual(typeof plain('(function* () { yield 1 })'), 'function');
assert.strictEqual(typeof plain('(async () => { await 1; for await (const x of []); })'), 'function');
assert.strictEqual(typeof plain('(async function* () { for await (const x of []) yield x })'), 'function');

const settled = [];
for (const fn of [asynchronous, asyncGenerator]) {
  for (const src of ['await 1', 'for await (const x of []);', '1 + (await 1)', 'yield 1', '() => await 1', 'var await = 1; await']) {
    const result = fn === asynchronous ? fn(src) : fn(src).next();
    settled.push(result.then(() => 'resolved', (e) => e.constructor.name));
  }
}

const log = [];
function closes() {
  return {
    [Symbol.iterator]() { return this; },
    next() { return { done: false, value: 1 }; },
    return() { log.push('closed'); return {}; },
  };
}
function returnInForOf() { for (const v of closes()) return v; }
function returnInTry() { try { return 'try'; } finally { log.push('finally'); } }
function returnInLoopTry() { for (let i = 0; i < 2; i++) { try { return i; } finally { log.push('loop finally'); } } }
function returnInSwitch(v) { switch (v) { case 1: return 'one'; default: return 'other'; } }
function returnInLabel() { out: { for (const v of closes()) { if (v) return 'labelled'; break out; } } }
function returnCaptured() { let v = 1; const get = () => v; while (true) { let w = v + 1; const inner = () => w; if (inner()) return [get, inner]; } }
assert.strictEqual(returnInForOf(), 1);
assert.strictEqual(returnInTry(), 'try');
assert.strictEqual(returnInLoopTry(), 0);
assert.strictEqual(returnInSwitch(1), 'one');
assert.strictEqual(returnInSwitch(2), 'other');
assert.strictEqual(returnInLabel(), 'labelled');
assert.deepStrictEqual(returnCaptured().map((f) => f()), [1, 2]);
assert.deepStrictEqual(log, ['closed', 'finally', 'loop finally', 'closed']);

function wide(count) {
  const names = [];
  for (let i = 0; i < count; i++) names.push('v' + i);
  let body = names.map((n, i) => `var ${n} = ${i};`).join(' ');
  body += ' var total = 0; var text = "";';
  body += ` for (var round = 0; round < 3; round++) { ${names.map((n, i) => `${n} = ${n} + ${names[(i + 1) % count]} + round;`).join(' ')} total = total + ${names.join(' + ')}; }`;
  body += ` ${names[0]} = "s"; text = ${names[0]} + ${names.slice(1, 6).join(' + ')};`;
  body += ' return total + ":" + text;';
  return body;
}
function reference(count) {
  const v = [];
  for (let i = 0; i < count; i++) v.push(i);
  let total = 0;
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < count; i++) v[i] = v[i] + v[(i + 1) % count] + round;
    for (let i = 0; i < count; i++) total += v[i];
  }
  let text = 's';
  for (let i = 1; i < 6; i++) text += v[i];
  return total + ':' + text;
}
for (const count of [6, 15, 16, 17, 31, 32, 33, 40, 70, 150]) {
  const expected = reference(count);
  assert.strictEqual(new Function(wide(count))(), expected, 'function ' + count);
  assert.strictEqual(plain(`(function () { ${wide(count)} })()`), expected, 'eval ' + count);
  assert.strictEqual(indirect(`(function () { ${wide(count)} })()`), expected, 'indirect ' + count);
  assert.strictEqual(new Function(`return (() => { ${wide(count)} })()`)(), expected, 'nested ' + count);
}

Promise.all(settled).then((names) => {
  const each = ['SyntaxError', 'SyntaxError', 'SyntaxError', 'SyntaxError', 'SyntaxError', 'resolved'];
  assert.deepStrictEqual(names, each.concat(each));
  console.log('ok');
});
