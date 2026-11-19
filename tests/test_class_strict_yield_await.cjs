const assert = require('node:assert');

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const cases = [
  ["function* g() { class C { [und5 = yield 1]() {} } } var it = g(); it.next(); try { it.next('Y') } catch (e) { return e.name } var t = typeof und5; delete globalThis.und5; return t", "value \"ReferenceError\""],
  ["var o = Object.freeze({ a: 1 }); function* g() { class C { [(o.a = yield 1)]() {} } } var it = g(); it.next(); try { it.next(2) } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { class C extends (und6 = yield 1, Object) {} } var it = g(); it.next(); try { it.next('Y') } catch (e) { return e.name } var t = typeof und6; delete globalThis.und6; return t", "value \"ReferenceError\""],
  ["function* g() { class C { static [und7 = yield 1] = 1 } } var it = g(); it.next(); try { it.next('Y') } catch (e) { return e.name } return 'no throw'", "value \"ReferenceError\""],
  ["function* g() { class C { [(yield 1, delete Object.prototype)]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { class C { [(yield 1, eval('und8 = 1'))]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } var t = typeof und8; delete globalThis.und8; return t", "value \"ReferenceError\""],
  ["function* g() { class C { [(yield 1, function () { return this })() === undefined ? 'a' : 'b']() {} } return Object.getOwnPropertyNames(C.prototype)[1] } var it = g(); it.next(); return it.next().value", "value \"a\""],
  ["function* g() { class C { [(yield 1, (() => { und9 = 1 })())]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } var t = typeof und9; delete globalThis.und9; return t", "value \"ReferenceError\""],
  ["function* g() { und10 = 1; class C { [und11 = yield 1]() {} } } var it = g(); it.next(); try { it.next('Y') } catch (e) { return [e.name, typeof und10, delete globalThis.und10] } return 'no throw'", "value [\"ReferenceError\",\"number\",true]"],
  ["function* g() { try { class C { [und12 = yield 1]() {} } } catch (e) { und13 = e.name; return [und13, delete globalThis.und13] } } var it = g(); it.next(); return it.next('Y').value", "value [\"ReferenceError\",true]"],
  ["function* g() { class C { [und14 = yield 1]() {} } } var it = g(); it.next(); it.return(5); und15 = 2; var r = [typeof und14, und15]; delete globalThis.und15; return r", "value [\"undefined\",2]"],
  ["function* g() { class C { [und16 = yield 1]() {} } } var it = g(); it.next(); try { it.throw(new RangeError('x')) } catch (e) { und17 = e.name } var r = [typeof und16, und17]; delete globalThis.und17; return r", "value [\"undefined\",\"RangeError\"]"],
  ["function* g() { class C { [(yield 1, yield 2, und18 = 3)]() {} } } var it = g(); var a = it.next().value; und19 = 1; var b = it.next().value; und20 = 2; try { it.next() } catch (e) { return [a, b, e.name, und19 + und20, delete globalThis.und19, delete globalThis.und20] } return 'no throw'", "value [1,2,\"ReferenceError\",3,true,true]"],
  ["function* inner() { yield 'i1'; und21 = 1; yield 'i2'; return 'ret' } function* g() { class C { [und22 = yield* inner()]() {} } } var it = g(); var a = [it.next().value, it.next().value]; try { it.next() } catch (e) { return [a, e.name, und21, delete globalThis.und21] } return 'no throw'", "value [[\"i1\",\"i2\"],\"ReferenceError\",1,true]"],
  ["function* g() { var o = { set x(v) { und23 = v } }; class C { [(o.x = yield 1)]() {} } return typeof und23 } var it = g(); it.next(); var r = it.next(4).value; delete globalThis.und23; return r", "value \"number\""],
  ["function* g() { class C { [(yield 1, undefined = 5)]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { var f = Object.freeze([1]); class C { [(yield 1, f.push(2))]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { var f = Object.freeze({ a: 1 }); class C { [(yield 1, f.a++)]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { var f = Object.freeze({ a: 1 }); class C { [(yield 1, delete f.a)]() {} } } var it = g(); it.next(); try { it.next() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
  ["function* g() { var f = Object.freeze({ a: 1 }); class C { [(yield 1, 'k')]() {} } f.a = 2; delete f.a; und24 = 1; return [f.a, delete globalThis.und24] } var it = g(); it.next(); return it.next().value", "value [1,true]"],
  ["function* g() { var f = Object.freeze({ a: 1 }); class C { [(yield 1, 'k')]() { f.a = 2 } } return new C() } var it = g(); it.next(); try { it.next().value.k() } catch (e) { return e.name } return 'no throw'", "value \"TypeError\""],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

(async () => {
  const seen = [];
  async function undeclaredKey() { class C { [undeclaredAsyncKey = await 1]() {} } }
  await undeclaredKey().then(() => seen.push('no throw'), e => seen.push(e.name));
  seen.push(typeof undeclaredAsyncKey);
  const frozen = Object.freeze({ a: 1 });
  async function frozenHeritage() { class C extends ((frozen.a = await 2), Object) {} return 'done' }
  await frozenHeritage().then(v => seen.push(v), e => seen.push(e.name));
  async function sloppyAfter() { class C { [(await 1, 'k')]() {} } sloppyAfterGlobal = 5; return typeof sloppyAfterGlobal }
  await sloppyAfter().then(v => seen.push(v), e => seen.push(e.name));
  delete globalThis.sloppyAfterGlobal;
  async function* asyncGenerator() { class C { [undeclaredAsyncGeneratorKey = yield await 1]() {} } }
  const it = asyncGenerator();
  await it.next();
  await it.next('v').then(() => seen.push('no throw'), e => seen.push(e.name));
  let total = 0;
  async function hot(i) { class C { [(await i, 'k' + i)]() {} } return Object.getOwnPropertyNames(C.prototype)[1] }
  for (let i = 0; i < 3000; i++) total += (await hot(i)).length;
  seen.push(total);
  assert.deepStrictEqual(seen, ['ReferenceError', 'undefined', 'TypeError', 'number', 'ReferenceError', 13890]);
  console.log('ok');
})();
