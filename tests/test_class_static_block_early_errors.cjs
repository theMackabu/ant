const assert = require('node:assert');

const rejected = [
  "class A { static { return 1; } }",
  "class A { static { arguments; } }",
  "class A { static { () => arguments; } }",
  "class A { static { await 1; } }",
  "class A { static { var await; } }",
  "class A { static { () => await 1; } }",
  "class A { static { function await(){} } }",
  "class A { static { class await {} } }",
  "class A { static { ({await}) } }",
  "class A { static { await: ; } }",
  "class A { static { yield; } }",
  "function* g(){ class A { static { yield; } } }",
  "function* g(){ class A { static { yield 1; } } }",
  "class A { static { var yield; } }",
  "class A extends Object { static { super(); } }",
  "class A { x = arguments; }",
  "class A { x = () => arguments; }",
  "class A { static x = arguments; }",
  "class A { static { var x; let x; } }",
  "class A { static { { var arguments; } } }",
  "class A { static { function arguments(){} } }",
  "class A { static { let arguments; } }",
  "class A { static { class B { [arguments]; } } }",
  "class A { static { class B { [await 1]; } } }",
  "async function f(){ class A { static { class B { [await 1]; } } } }",
  "class A { static { (x = await 1) => 1 } }",
  "class A { static { await } }",
  "class A { static { let a; let a; } }",
  "class A { static { (class await {}) } }",
  "class A { static { (await) => 1 } }",
  "class A { static { await => 1 } }",
  "class A { static { async (await) => 1 } }",
  "class A { static { try {} catch (await) {} } }",
  "class A { static { ({arguments}) } }",
  "class A { static { typeof arguments } }",
  "class A { x = ({arguments}); }",
  "class A { x = typeof arguments; }",
  "class A { x = () => { arguments } }",
  "class A { static { (function yield(){}) } }",
  "class A { static { if (1) return; } }",
  "function f(){ class A { static { return; } } }",
  "class A { static { class B { x = arguments } } }",
  "class A { static { class B { static { arguments } } } }",
  "class A { x = class { [arguments] = 1 } }",
  "async function f(){ class A { x = await 1 } }",
  "class A { static { async function await(){} } }",
  "class A { static { let await } }",
  "class A { static { const {await} = {} } }",
  "class A { static { [await] = [] } }",
  "class A { static { `${await}` } }",
  "class A { static { `${arguments}` } }",
  "class A { static { await\n1 } }",
  "class A { static { await.x } }",
  "class A { static { await(1) } }",
  "class A { static { yield: ; } }",
];
for (const source of rejected) assert.throws(() => new Function(source), SyntaxError, source);

const accepted = [
  "class A { static { function f(){ return arguments } } }",
  "class A { static { ({ m(){ return arguments } }) } }",
  "class A { static { async function f(){ await 1 } } }",
  "class A { static { (async () => await 1); } }",
  "class A { static { function f(await){} } }",
  "class A { static { ({await: 1}) } }",
  "class A { static { x.await } }",
  "class A extends Object { static { super.x; } }",
  "class A { x = function(){ return arguments }; }",
  "class A { [arguments] = 1; }",
  "function f(){ class A { [arguments.length] = 1; } }",
  "class A { static { l: for(;;){ break l; } } }",
  "class A { static { for(;;){ break; } } }",
  "class A { static { while(0){ continue; } } }",
  "class A { static { new.target; } }",
  "class A { static { var x; let y; } }",
  "class A { x = eval('arguments'); }",
  "class A { static { async () => { await 1 } } }",
  "class A { static { function f(x = await){} } }",
  "class A { static { ({ await(){} }) } }",
  "class A { static { class C { await(){} } } }",
  "return 1",
  "class A { static { super.x = 1; } }",
  "class A { static {} static {} }",
  "class A { static { (function await(){}) } }",
  "class A { static { ({arguments: 1}) } }",
  "class A { static { x.arguments } }",
  "class A { x = this.arguments; }",
  "class A { x = () => function(){ arguments } }",
  "class A { static { () => { return 1 } } }",
  "class A { x = class { m(){ arguments } } }",
  "class A { x = await }",
  "async function f(){ class A { [await 1] = 1 } }",
  "class A { static { function f(){ return 1 } } }",
  "class A { static { function f(){ await } } }",
  "class A { static { ({ get await(){ return 1 } }) } }",
  "class A { static { label: { break label; } } }",
  "class A { static { switch(1){ case 1: break; } } }",
  "class A { static { x: for(;;){ continue x; } } }",
  "class A { static { function* g(){ yield 1 } } }",
  "class A { static { var f = function(){ return; } } }",
  "class A { static { new.target } }",
  "class A { static { this.x = 1 } }",
  "class A { static { const {await: b} = {} } }",
];
for (const source of accepted) assert.doesNotThrow(() => new Function(source), source);

const indirectEval = eval;
assert.throws(() => indirectEval('class A { static { return 1; } }'), SyntaxError);
assert.throws(() => indirectEval('class A { x = arguments; }'), SyntaxError);

class Inner {
  static {
    for (const x of [1, 2]) { if (x) break; }
    let seen = '';
    outer: for (let i = 0; i < 3; i++) {
      for (let j = 0; j < 3; j++) {
        if (j === 1) continue outer;
        if (i === 2) break outer;
        seen += i + '' + j + ' ';
      }
    }
    const count = function () { return arguments.length; };
    const viaMethod = { m() { return arguments[0]; } };
    Inner.v = count(1, 2);
    Inner.seen = seen;
    Inner.m = viaMethod.m('first');
    Inner.asyncFn = async function () { return await 5; };
    Inner.asyncArrow = async () => await 6;
    Inner.self = this;
  }
  static twice = (() => { let n = 0; block: { n++; break block; } return n; })();
  field = function () { return arguments.length; };
}
assert.strictEqual(Inner.v, 2);
assert.strictEqual(Inner.seen, '00 10 ');
assert.strictEqual(Inner.m, 'first');
assert.strictEqual(Inner.self, Inner);
assert.strictEqual(Inner.twice, 1);
assert.strictEqual(new Inner().field(1, 2, 3), 3);

Promise.all([Inner.asyncFn(), Inner.asyncArrow()]).then((values) => {
  assert.deepStrictEqual(values, [5, 6]);
  console.log('ok');
});
