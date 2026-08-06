// Regression: re-binding a bound function must keep the inner binding's
// this and prepend the inner binding's args (spec BoundFunctionCreate
// chains; our flattened representation must fold, not replace).
let failures = 0;
let assertions = 0;
function check(name, actual, expected) {
  assertions++;
  const a = JSON.stringify(actual), e = JSON.stringify(expected);
  if (a !== e) {
    console.log(`FAIL ${name}: got ${a}, want ${e}`);
    failures++;
  }
}

function f(...a) { return [this && this.x, ...a]; }

check('double bind this+args', f.bind({ x: 1 }, 10).bind({ x: 2 }, 20)(30), [1, 10, 20, 30]);
check('inner bind no args', f.bind({ x: 1 }).bind({ x: 2 }, 5)(6), [1, 5, 6]);
check('triple bind', f.bind({ x: 1 }, 1).bind({ x: 2 }, 2).bind({ x: 3 }, 3)(4), [1, 1, 2, 3, 4]);
check('outer bind no args', f.bind({ x: 1 }, 10).bind({ x: 2 })(30), [1, 10, 30]);
check('single bind unchanged', f.bind({ x: 7 }, 8)(9), [7, 8, 9]);

// cfunc target: first bind produces a closure over the native, the second
// bind takes the T_FUNC re-bind path.
check('cfunc double bind args', Math.max.bind(null, 1).bind(null, 2)(3), 3);
check('cfunc double bind order', Math.min.bind(null, 5).bind(null, 2)(9), 2);
{
  let threw = false;
  try {
    Array.prototype.push.bind(undefined).bind([])(1);
  } catch {
    threw = true;
  }
  check('cfunc undefined this stays bound', threw, true);
}

// Proxy-backed bound functions take a separate wrapper path.
{
  const target = function () {
    'use strict';
    return this === undefined ? 'undefined' : this.id;
  };
  const proxy = new Proxy(target, {
    apply(fn, thisArg, args) {
      return Reflect.apply(fn, thisArg, args);
    },
  });
  check(
    'proxy undefined this stays bound',
    proxy.bind(undefined).bind({ id: 'outer' })(),
    'undefined'
  );
}

// arrows ignore every bound this but still fold args.
const arrow = (...a) => a;
check('arrow double bind args', arrow.bind({ x: 1 }, 1).bind({ x: 2 }, 2)(3), [1, 2, 3]);

// Deterministic bind-chain fuzzer. `undefined` is both a valid bound this
// value and the old runtime's "not bound" sentinel, so cover it with and
// without bound arguments and invoke the result both directly and as a
// method. The first bind always owns this; every bind contributes arguments.
function strictThis() {
  'use strict';
  return [this === undefined ? 'undefined' : this.id, ...Array.from(arguments)];
}
const thisCases = [
  { name: 'undefined', value: undefined, seen: 'undefined' },
  { name: 'object-a', value: { id: 'a' }, seen: 'a' },
  { name: 'object-b', value: { id: 'b' }, seen: 'b' },
];
const argCases = [
  { name: 'empty', values: [] },
  { name: 'one', values: [11] },
  { name: 'two', values: [11, 12] },
];

for (const innerThis of thisCases) {
  for (const outerThis of thisCases) {
    for (const innerArgs of argCases) {
      for (const outerArgs of argCases) {
        const name = `${innerThis.name}/${outerThis.name}/${innerArgs.name}/${outerArgs.name}`;
        const bound = strictThis
          .bind(innerThis.value, ...innerArgs.values)
          .bind(outerThis.value, ...outerArgs.values);
        const expected = [innerThis.seen, ...innerArgs.values, ...outerArgs.values, 99];
        check(`matrix direct ${name}`, bound(99), expected);
        check(`matrix method ${name}`, ({ bound }).bound(99), expected);
      }
    }
  }
}

const undefinedBound = strictThis.bind(undefined);
check(
  'Function.call receiver cannot replace bound undefined',
  undefinedBound.call({ id: 'outer' }, 99),
  ['undefined', 99]
);
function tailDirect(fn) { return fn(99); }
function tailMethod(holder) { return holder.fn(99); }
let tailDirectResult;
let tailMethodResult;
for (let i = 0; i < 200; i++) {
  tailDirectResult = tailDirect(undefinedBound);
  tailMethodResult = tailMethod({ fn: undefinedBound });
}
check('tail direct keeps bound undefined', tailDirectResult, ['undefined', 99]);
check('tail method keeps bound undefined', tailMethodResult, ['undefined', 99]);

// Construction ignores every bound this but still prepends every layer's
// arguments in bind order.
function Box(...a) { this.args = a; }
for (const innerArgs of argCases) {
  for (const outerArgs of argCases) {
    const BoundBox = Box
      .bind({ ignored: 1 }, ...innerArgs.values)
      .bind({ ignored: 2 }, ...outerArgs.values);
    check(
      `construct args ${innerArgs.name}/${outerArgs.name}`,
      new BoundBox(99).args,
      [...innerArgs.values, ...outerArgs.values, 99]
    );
  }
}

// name and length follow the chain.
function h(a, b, c, d) {}
check('length folds', h.bind(0, 1).bind(0, 2).length, 2);
check('name prefixes', f.bind({ x: 1 }, 10).bind({ x: 2 }, 20).name, 'bound bound f');

// bound args survive GC: the args array must keep argv contents alive.
{
  const bf = f.bind({ x: 4 }, { tag: 'kept' });
  for (let i = 0; i < 200000; i++) ({ churn: i });
  const r = bf();
  check('bound args survive churn', r[1].tag, 'kept');
}

if (failures) {
  console.log(`${failures}/${assertions} failures`);
  process.exit(1);
}
console.log(`all ${assertions} double-bind tests passed`);
