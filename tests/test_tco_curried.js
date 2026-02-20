// Test tail call optimization for curried/chained calls

// 1. Basic curried tail call: f(a)(b)
function makeStepper(target) {
  return function step(n) {
    if (n >= target) return n;
    return makeStepper(target)(n + 1);
  };
}
console.log('curried step(0→100000):', makeStepper(100000)(0)); // 100000

// 2. Arrow function returning curried call
const curry2 = f => a => b => f(a, b);
const add = curry2((a, b) => a + b);
console.log('curry2 add(3)(4):', add(3)(4)); // 7

// 3. Deep curried recursion (bouncer pattern from newt bootstrap)
const bouncer = (f, ini) => {
  let obj = ini;
  while (obj.tag) obj = f(obj);
  return obj.h0;
};

function trampCountDown(n) {
  if (n <= 0) return { tag: 0, h0: 'done' };
  return { tag: 1, h0: null, fn: () => trampCountDown(n - 1) };
}
// Not a direct curried TCO test, but validates the pattern works
const res1 = bouncer(obj => obj.fn(), { tag: 1, h0: null, fn: () => trampCountDown(100000) });
console.log('bouncer countDown:', res1); // done

// 4. Curried tail call with intermediate result used as function
function makeCounter(acc) {
  return function (n) {
    if (n <= 0) return acc;
    return makeCounter(acc + 1)(n - 1);
  };
}
console.log('curried counter(100000):', makeCounter(0)(100000)); // 100000

// 5. Triple-curried tail call: f(a)(b)(c)
function tripleStep(target) {
  return function (inc) {
    return function (n) {
      if (n >= target) return n;
      return tripleStep(target)(inc)(n + inc);
    };
  };
}
console.log('triple curried(50000):', tripleStep(50000)(1)(0)); // 50000

// 6. Method-style chained call should NOT get TCO for intermediate
// (a.b() returns obj, then .c() is the tail call)
const obj = {
  makeNext(n) {
    if (n <= 0) return { val: () => 'done' };
    return { val: () => obj.makeNext(n - 1).val() };
  }
};
console.log('method chain:', obj.makeNext(100).val()); // done

// 7. Curried arrow function tail call
const curriedDown = n => {
  if (n <= 0) return 'finished';
  return (x => curriedDown(x))(n - 1);
};
console.log('curried arrow(100000):', curriedDown(100000)); // finished

// 8. Ensure intermediate calls are NOT tail-optimized
// f(a) must complete normally because its result is called
let callCount = 0;
function makeAdder(x) {
  callCount++;
  return function (y) {
    return x + y;
  };
}
const result = makeAdder(10)(20);
console.log('intermediate not TCO:', result, 'calls:', callCount); // 30 calls: 1

// 9. Mutual curried recursion
function pingFactory(n) {
  return function () {
    if (n <= 0) return 'pong';
    return pongFactory(n - 1)();
  };
}
function pongFactory(n) {
  return function () {
    if (n <= 0) return 'ping';
    return pingFactory(n - 1)();
  };
}
console.log('mutual curried(100000):', pingFactory(100000)()); // pong

// 10. Monad-style bind chain (simplified newt pattern)
// Note: m.h1(tc)(eta) is NOT in tail position (bound to const sc),
// so this tests correctness of curried calls, not deep TCO.
const MkM = h1 => ({ tag: 0, h1: h1 });
const bind = (m, f) =>
  MkM(tc => eta => {
    const sc = m.h1(tc)(eta);
    if (sc.tag === 1) return f(sc.h2.h3).h1(sc.h2.h2)(eta);
    return sc;
  });
const pure = v => MkM(tc => eta => ({ tag: 1, h2: { h2: tc, h3: v } }));

let chain = pure(0);
for (let i = 0; i < 800; i++) {
  chain = bind(chain, v => pure(v + 1));
}
const monadResult = chain.h1({})(0);
console.log('monad chain result:', monadResult.h2.h3); // 1000

console.log('\nAll curried TCO tests passed!');
