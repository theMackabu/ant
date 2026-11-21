const assert = require('node:assert');

const geval = eval;

{
  const out = geval(`
    let top = 1;
    const fns = [];
    for (let i = 0; i < 4; i++) fns.push(() => i + top);
    { let a = 10; fns.push(() => a); }
    { let a = 20; fns.push(() => a); fns.push(() => { a++; return a; }); }
    top = 100;
    fns.map((f) => f());
  `);
  assert.deepStrictEqual(out, [100, 101, 102, 103, 10, 20, 21]);
}

{
  const out = geval(`
    (function (p, q) {
      var v = 1;
      const made = [];
      for (let i = 0; i < 3; i++) {
        let j = i * 2;
        made.push(() => [i, j, v, p, arguments.length]);
        made.push(function () { j++; v++; return eval('i + j + v + p + q'); });
      }
      v = 5;
      return made.map((f) => f());
    })(7, 9);
  `);
  assert.deepStrictEqual(out, [[0, 0, 5, 7, 2], 23, [1, 2, 6, 7, 2], 27, [2, 4, 7, 7, 2], 31]);
}

{
  const out = geval(`
    class EvalScopeA { static s() { return typeof EvalScopeA; } m() { return EvalScopeB.name; } #p = 3; static has(o) { return #p in o; } }
    class EvalScopeB extends EvalScopeA { static s() { return super.s() + '!'; } m() { return super.m() + '?'; } }
    [EvalScopeA.s(), EvalScopeB.s(), new EvalScopeA().m(), new EvalScopeB().m(), EvalScopeA.has(new EvalScopeB()), EvalScopeA.has({})];
  `);
  assert.deepStrictEqual(out, ['function', 'function!', 'EvalScopeB', 'EvalScopeB?', true, false]);
}

{
  function direct(x) {
    const fns = [];
    eval(`
      for (let k = 0; k < 3; k++) { fns.push(() => k + x); }
      { let x = 50; fns.push(() => x); }
      fns.push(() => x);
      var late = () => x * 2;
    `);
    x = 4;
    return fns.map((f) => f()).concat(late());
  }
  assert.deepStrictEqual(direct(1), [4, 5, 6, 50, 4, 8]);
  assert.deepStrictEqual(direct(2), [4, 5, 6, 50, 4, 8]);
}

{
  function recur(n) {
    return eval('var mine = n; const get = () => mine; n > 0 ? [get].concat(recur(n - 1)) : [get]').map((f) => (typeof f === 'function' ? f() : f));
  }
  assert.deepStrictEqual(recur(3), [3, 2, 1, 0]);
}

{
  const gen = geval(`(function* () { let a = 1; const f = () => a; yield f; a = 2; const g = () => a; yield g; yield [f(), g()]; })`)();
  const f = gen.next().value;
  assert.strictEqual(f(), 1);
  const g = gen.next().value;
  assert.deepStrictEqual([f(), g(), gen.next().value], [2, 2, [2, 2]]);
}

{
  const out = geval(`
    (function () {
      const seen = [];
      with ({ w: 1 }) { seen.push(() => w); }
      with ({ w: 2 }) { seen.push(() => w); }
      try { throw 3; } catch (e) { seen.push(() => e); }
      try { throw 4; } catch (e) { seen.push(() => e); }
      return seen.map((f) => f());
    })();
  `);
  assert.deepStrictEqual(out, [1, 2, 3, 4]);
}

{
  const count = 4000;
  const source = [];
  for (let i = 0; i < count; i++) source.push(`class EvalMany${i} { m() { return ${i}; } static s() { return 1; } }`);
  source.push(`[new EvalMany0().m(), new EvalMany${count - 1}().m(), EvalMany7.s()]`);
  const start = Date.now();
  assert.deepStrictEqual(geval(source.join('\n')), [0, count - 1, 1]);
  const lets = [];
  for (let i = 0; i < count; i++) lets.push(`let evalLet${i} = () => ${i} + (typeof evalLet0 === 'function' ? 0 : 1);`);
  lets.push(`evalLet0() + evalLet${count - 1}()`);
  assert.strictEqual(geval(lets.join('\n')), count - 1);
  assert.ok(Date.now() - start < 8000, 'eval of many closures stays near linear');
}

console.log('ok');
