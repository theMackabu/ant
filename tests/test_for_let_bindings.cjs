const assert = require('node:assert');

const collect = (fns) => fns.map(f => f());
const cases = {
  update: [() => { const f = []; for (let i = 0; i < 3; f.push(() => i), i++); return collect(f); }, [1, 2, 3]],
  cond: [() => { const f = []; for (let i = 0; f.push(() => i) && i < 2; i++); return collect(f); }, [0, 1, 2]],
  body: [() => { const f = []; for (let i = 0; i < 3; i++) f.push(() => i); return collect(f); }, [0, 1, 2]],
  init: [() => { const f = []; for (let i = 0, g = () => i; i < 3; i++) f.push(g); return collect(f); }, [0, 0, 0]],
  bodyMutates: [() => { const f = []; for (let i = 0; i < 6; i++) { f.push(() => i); i++; } return collect(f); }, [1, 3, 5]],
  closureWrites: [() => { const f = []; for (let i = 0; i < 5; i++) { f.push(() => i++); f[f.length - 1](); } return collect(f); }, [1, 3, 5]],
  continue: [() => { const f = []; for (let i = 0; i < 4; i++) { if (i % 2) continue; f.push(() => i); } return collect(f); }, [0, 2]],
  break: [() => { const f = []; for (let i = 0; i < 4; i++) { f.push(() => i); if (i === 2) break; } return collect(f); }, [0, 1, 2]],
  labeled: [() => {
    const f = [];
    outer: for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) { if (j === 1) continue outer; f.push(() => `${i}${j}`); }
    return collect(f);
  }, ['00', '10', '20']],
  twoBindings: [() => { const f = []; for (let i = 0, j = 10; i < 3; i++, j--) f.push(() => i + j); return collect(f); }, [10, 10, 10]],
  eval: [() => { const f = []; for (let i = 0; i < 3; i++) f.push(eval('() => i')); return collect(f); }, [0, 1, 2]],
  nested: [() => {
    const f = [];
    for (let i = 0; i < 2; i++) for (let j = 0; j < 2; f.push(() => [i, j]), j++);
    return collect(f);
  }, [[0, 1], [0, 2], [1, 1], [1, 2]]],
  hot: [() => {
    let s = 0;
    const f = [];
    for (let i = 0; i < 100000; i++) { if (i % 25000 === 0) f.push(() => i); s += i; }
    return [s, collect(f)];
  }, [4999950000, [0, 25000, 50000, 75000]]],
};

for (let round = 0; round < 3; round++)
  for (const [name, [fn, expected]] of Object.entries(cases))
    assert.deepStrictEqual(fn(), expected, name);

assert.throws(() => { for (let i = (() => i)(); ;) return i; }, ReferenceError);
assert.throws(() => { for (let i = i; ;) return i; }, ReferenceError);

console.log('for-let bindings: ok');
