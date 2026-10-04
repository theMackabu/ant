// Cached named reads (the per-site case, the polymorphic cases, the shared
// megamorphic cache; the C lookup and the JIT's inline checks of the same
// cases) must agree with a lookup that uses no read caches. Each scenario
// reads through a fresh compiled site and through `reference`, a walk of
// the prototype chain with Object.getOwnPropertyDescriptor, at 1, 6 and 24
// receiver shapes per site, before and after changes the shapes don't show.
const assert = require('node:assert');

function reference(o, key) {
  for (let p = Object(o); p !== null; p = Object.getPrototypeOf(p)) {
    const d = Object.getOwnPropertyDescriptor(p, key);
    if (!d) continue;
    return 'value' in d ? d.value : d.get ? d.get.call(o) : undefined;
  }
  return undefined;
}

const site = key => new Function('o', `return o.${key};`);
const ROUNDS = 400;

function check(name, receivers, key, mutate) {
  const read = site(key);
  for (let r = 0; r < ROUNDS; r++) {
    if (mutate && r === ROUNDS / 2) mutate();
    for (let i = 0; i < receivers.length; i++) {
      const o = receivers[(i + r) % receivers.length];
      const got = read(o), want = reference(o, key);
      if (!Object.is(got, want))
        assert.fail(`${name}: round ${r} receiver ${i}: got ${String(got)}, want ${String(want)}`);
    }
  }
}

// receivers of `count` distinct shapes: own `k` at varying slots, `k` on a
// prototype, `k` missing, and `k` through an inherited getter
function mixed(count) {
  const protoK = { k: 'proto' };
  const getterProto = { get k() { return 'getter:' + (this.tag ?? '-'); } };
  const out = [];
  for (let i = 0; i < count; i++) {
    const o = Object.create(i % 3 === 1 ? protoK : i % 3 === 2 ? getterProto : Object.prototype);
    for (let j = 0; j < i % 5; j++) o['pad' + i + '_' + j] = j;
    if (i % 3 === 0) o.k = 'own' + i;
    if (i % 4 === 0) o.tag = i;
    out.push(o);
  }
  return { out, protoK, getterProto };
}

for (const count of [1, 6, 24]) {
  {
    const { out } = mixed(count);
    check(`mixed/${count}`, out, 'k');
  }
  {
    // the prototype's value changes after the site is compiled
    const { out, protoK } = mixed(count);
    check(`proto-value/${count}`, out, 'k', () => { protoK.k = 'changed'; });
  }
  {
    // the prototype gains a getter, then an own property shadows it
    const { out, protoK } = mixed(count);
    check(`proto-getter/${count}`, out, 'k', () => {
      Object.defineProperty(protoK, 'k', { get() { return 'now a getter'; }, configurable: true });
    });
    check(`own-shadow/${count}`, out, 'k', () => { for (const o of out) o.k = 'shadow'; });
  }
  {
    // a property deleted from the prototype
    const { out, protoK } = mixed(count);
    check(`proto-delete/${count}`, out, 'k', () => { delete protoK.k; });
  }
  {
    // the same shape under different prototypes, swapped part way
    const a = { k: 'A' }, b = { k: 'B' };
    const out = [];
    for (let i = 0; i < count; i++) { const o = Object.create(i % 2 ? a : b); o.x = i; out.push(o); }
    check(`same-shape/${count}`, out, 'k', () => { for (const o of out) Object.setPrototypeOf(o, o.x % 2 ? b : a); });
  }
}

// exotic receivers mixed into ordinary ones at one site
{
  const args = (function () { return arguments; })(1, 2, 3);
  const typed = new Uint8Array(4);
  const ordinary = mixed(6).out;
  check('exotic/length', [args, typed, [1, 2], 'str', ...ordinary], 'length');
  check('exotic/k', [args, typed, ...ordinary], 'k');
}

console.log('PASS cached reads agree with an uncached lookup');
