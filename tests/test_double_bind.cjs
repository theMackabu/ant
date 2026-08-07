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

// Proxy-backed bound functions take a separate wrapper path. Re-binding
// flattens the accumulated arguments, so that wrapper must target the Proxy
// directly rather than re-entering the inner bound function.
{
  let applyCalls = 0;
  let constructCalls = 0;
  const target = function () {
    'use strict';
    const args = Array.from(arguments);
    if (new.target) {
      this.args = args;
      return;
    }
    return [this === undefined ? 'undefined' : this.id, ...args];
  };
  const proxy = new Proxy(target, {
    apply(fn, thisArg, args) {
      applyCalls++;
      return Reflect.apply(fn, thisArg, args);
    },
    construct(fn, args) {
      constructCalls++;
      return Reflect.construct(fn, args);
    },
  });

  let expectedApplyCalls = 0;
  for (const innerThis of thisCases) {
    for (const outerThis of thisCases) {
      for (const innerArgs of argCases) {
        for (const outerArgs of argCases) {
          const name = `${innerThis.name}/${outerThis.name}/${innerArgs.name}/${outerArgs.name}`;
          const bound = proxy
            .bind(innerThis.value, ...innerArgs.values)
            .bind(outerThis.value, ...outerArgs.values);
          const expected = [innerThis.seen, ...innerArgs.values, ...outerArgs.values, 99];
          check(`proxy matrix direct ${name}`, bound(99), expected);
          check(`proxy matrix method ${name}`, ({ bound }).bound(99), expected);
          expectedApplyCalls += 2;
        }
      }
    }
  }
  check(
    'proxy triple bind args',
    proxy.bind({ id: 'inner' }, 1).bind({ id: 'middle' }, 2).bind({ id: 'outer' }, 3)(4),
    ['inner', 1, 2, 3, 4]
  );
  expectedApplyCalls++;
  check('proxy apply trap once per call', applyCalls, expectedApplyCalls);

  for (const innerArgs of argCases) {
    for (const outerArgs of argCases) {
      const BoundProxy = proxy
        .bind({ id: 'inner' }, ...innerArgs.values)
        .bind({ id: 'outer' }, ...outerArgs.values);
      check(
        `proxy construct args ${innerArgs.name}/${outerArgs.name}`,
        new BoundProxy(99).args,
        [...innerArgs.values, ...outerArgs.values, 99]
      );
    }
  }
  check('proxy construct trap once per call', constructCalls, argCases.length ** 2);
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

// Bound functions do not define their own prototype property. Construction
// unwraps every bound layer when selecting the instance prototype, while
// retaining all bound arguments.
{
  function Constructor(...args) {
    this.args = args;
  }
  Constructor.prototype.tag = 'target prototype';
  const Inner = Constructor.bind({ ignored: 1 }, 1);
  const Outer = Inner.bind({ ignored: 2 }, 2);
  const instance = new Outer(3);
  check('inner bound has no own prototype', Object.hasOwn(Inner, 'prototype'), false);
  check('outer bound has no own prototype', Object.hasOwn(Outer, 'prototype'), false);
  check(
    'double-bound construction uses target prototype',
    Object.getPrototypeOf(instance) === Constructor.prototype,
    true
  );
  check('double-bound construction keeps target prototype values', instance.tag, 'target prototype');
  check('double-bound construction instanceof target', instance instanceof Constructor, true);
  check('double-bound construction instanceof inner', instance instanceof Inner, true);
  check('double-bound construction instanceof outer', instance instanceof Outer, true);
  check('double-bound construction args', instance.args, [1, 2, 3]);
  check('double-bound constructor sees original new.target', (() => {
    function NewTarget() {
      this.seen = new.target;
    }
    const Bound = NewTarget.bind(null).bind(null);
    return new Bound().seen === NewTarget;
  })(), true);
  check('Reflect.construct preserves explicit newTarget', (() => {
    function Target() {
      this.seen = new.target;
    }
    function Alternate() {}
    const Bound = Target.bind(null).bind(null);
    const value = Reflect.construct(Bound, [], Alternate);
    return value.seen === Alternate && Object.getPrototypeOf(value) === Alternate.prototype;
  })(), true);
  check('bound native has no own prototype', Object.hasOwn(Math.max.bind(null), 'prototype'), false);
  check('bound native constructor keeps target prototype', (() => {
    const BoundArray = Array.bind(null, 3);
    const value = new BoundArray();
    return (
      value.length === 3 &&
      Object.getPrototypeOf(value) === Array.prototype &&
      value instanceof Array &&
      value instanceof BoundArray
    );
  })(), true);
  check(
    'bound proxy has no own prototype',
    Object.hasOwn(new Proxy(Constructor, {}).bind(null), 'prototype'),
    false
  );
}

// A bound generator retains the target generator's instance prototype and is
// therefore iterable. Like every bound function, it has no own prototype.
{
  function* generator() {
    yield this.value;
  }
  const boundGenerator = generator.bind({ value: 7 });
  check('bound generator has no own prototype', Object.hasOwn(boundGenerator, 'prototype'), false);
  check('bound generator is iterable', [...boundGenerator()], [7]);
  const reboundGenerator = boundGenerator.bind({ value: 8 });
  const iterator = reboundGenerator();
  check('re-bound generator is iterable', [...iterator], [7]);
  check('bound generator instanceof target', iterator instanceof generator, true);
  check('bound generator instanceof bound function', iterator instanceof boundGenerator, true);
}

// Ordinary sloppy functions box primitive receivers. Strict functions retain
// the primitive value, including through bind.
{
  function sloppyThis() {
    const value = this.valueOf();
    const proto = Object.getPrototypeOf(this);
    const wrapperPrototypeMatches =
      (typeof value === 'number' && proto === Number.prototype) ||
      (typeof value === 'string' && proto === String.prototype) ||
      (typeof value === 'boolean' && proto === Boolean.prototype);
    return [typeof this, wrapperPrototypeMatches, value];
  }
  function strictThisValue() {
    'use strict';
    return [typeof this, this];
  }
  check('sloppy call boxes number this', sloppyThis.call(7), ['object', true, 7]);
  check(
    'sloppy bind boxes string this',
    sloppyThis.bind('value')(),
    ['object', true, 'value']
  );
  check('sloppy call boxes boolean this', sloppyThis.call(true), ['object', true, true]);
  check('sloppy call boxes bigint this', (() => {
    function readBigInt() {
      return [
        typeof this,
        Object.getPrototypeOf(this) === BigInt.prototype,
        this.valueOf() === 7n,
      ];
    }
    return readBigInt.call(7n);
  })(), ['object', true, true]);
  check('sloppy call boxes symbol this', (() => {
    const symbol = Symbol('bound this');
    function readSymbol() {
      return [
        typeof this,
        Object.getPrototypeOf(this) === Symbol.prototype,
        this.valueOf() === symbol,
      ];
    }
    return readSymbol.call(symbol);
  })(), ['object', true, true]);
  check('sloppy null this uses globalThis', (() => {
    function readThis() {
      return this;
    }
    return readThis.call(null) === globalThis;
  })(), true);
  check('sloppy undefined this uses globalThis', (() => {
    function readThis() {
      return this;
    }
    return readThis.call(undefined) === globalThis;
  })(), true);
  check('strict call keeps primitive this', strictThisValue.call(7), ['number', 7]);
  check('strict bind keeps primitive this', strictThisValue.bind('value')(), ['string', 'value']);
  check('strict call keeps null this', strictThisValue.call(null), ['object', null]);
  check('strict call keeps undefined this', strictThisValue.call(undefined), ['undefined', undefined]);

  const warmedSloppy = sloppyThis.bind(7);
  let warmedResult;
  for (let i = 0; i < 500; i++) warmedResult = warmedSloppy();
  check('JIT keeps sloppy bound this boxed', warmedResult, ['object', true, 7]);

  function makeThisReader() {
    return () => this;
  }
  const readThis = makeThisReader.call('captured');
  check('sloppy arrow captures boxed this', typeof readThis(), 'object');
}

// A bound Proxy remains one construct operation and receives the normalized
// target as newTarget.
{
  let constructCalls = 0;
  let seenNewTarget;
  function ProxyTarget() {
    this.ok = true;
  }
  const proxy = new Proxy(ProxyTarget, {
    construct(target, args, newTarget) {
      constructCalls++;
      seenNewTarget = newTarget;
      return Reflect.construct(target, args);
    },
  });
  const BoundProxy = proxy.bind(null).bind(null);
  const value = new BoundProxy();
  check('bound Proxy construct trap count', constructCalls, 1);
  check('bound Proxy sees target newTarget', seenNewTarget === proxy, true);
  check('bound Proxy construct result', value.ok, true);
}

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
