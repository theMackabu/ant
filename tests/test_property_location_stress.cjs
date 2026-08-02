function assertEq(actual, expected, label) {
  if (!Object.is(actual, expected)) {
    throw new Error(`${label}: got ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`);
  }
}

function deleteRenumbersSlots() {
  for (let i = 0; i < 5000; i++) {
    const o = { a: 1, b: 2, c: 3, d: 4 };
    delete o.b;
    assertEq(o.a, 1, 'a survives delete');
    assertEq(o.b, undefined, 'b is gone');
    assertEq(o.c, 3, 'c survives delete');
    assertEq(o.d, 4, 'd survives delete');

    o.c = 30;
    o.d = 40;
    assertEq(o.c, 30, 'c writable after delete');
    assertEq(o.d, 40, 'd writable after delete');
    assertEq(Object.keys(o).join(), 'a,c,d', 'insertion order preserved after delete');
  }
}

function setterDeletesSibling() {
  for (let i = 0; i < 2000; i++) {
    const o = { x: 1, y: 2 };
    Object.defineProperty(o, 'z', {
      set(v) {
        delete this.x;
        this.y = v;
      },
      configurable: true
    });
    o.z = 9;
    assertEq(o.x, undefined, 'setter deleted x');
    assertEq(o.y, 9, 'setter wrote y');
  }
}

function prototypeStoreInvalidatesInstanceof() {
  function F() {}
  const before = new F();
  assertEq(before instanceof F, true, 'instance matches original prototype');

  F.prototype = {};
  assertEq(before instanceof F, false, 'old instance no longer matches');
  assertEq(new F() instanceof F, true, 'new instance matches replacement prototype');
}

function proxyOverMutatedTarget() {
  for (let i = 0; i < 2000; i++) {
    const target = { p: 1, q: 2, r: 3 };
    const proxy = new Proxy(target, {});
    delete target.q;
    assertEq(proxy.p, 1, 'proxy sees p');
    assertEq(proxy.r, 3, 'proxy sees r');
    assertEq('q' in proxy, false, 'proxy sees q removed');
  }
}

function jsonDuplicateKeyAcrossCollection() {
  const filler = [];
  for (let i = 0; i < 120000; i++) filler.push(`"k${i}":${i}`);
  const src = `{"a":1,${filler.join(',')},"a":2}`;
  assertEq(JSON.parse(src).a, 2, 'last duplicate key wins');
}

function frozenStaysEnforced() {
  'use strict';
  const frozen = Object.freeze({ v: 1 });
  try {
    frozen.v = 2;
  } catch (e) {}
  assertEq(frozen.v, 1, 'frozen property unchanged');
}

deleteRenumbersSlots();
setterDeletesSibling();
prototypeStoreInvalidatesInstanceof();
proxyOverMutatedTarget();
jsonDuplicateKeyAcrossCollection();
frozenStaysEnforced();

console.log('OK: test_property_location_stress');
