// object-literal site caches (obj_site) are per-stack-slot compiler state;
// they must be cleared at branch-target joins like the other speculative
// slot metadata, or a DEFINE_FIELD after a join can update the wrong
// literal site's shared_shape (regression: stale site across joins)

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

// literals whose field values branch: every ternary/logical arm compiles
// to a join inside the literal construction, with DEFINE_FIELD following
function branchyLiteral(c, q) {
  return {
    a: c ? 1 : 2,
    b: q || { inner: 3 },
    c: c ? { deep: 4 } : 'flat',
    d: 5
  };
}

// two distinct literal sites whose construction interleaves via calls,
// so site caches from different literals coexist during compilation
function siteA(c) {
  return { x: c ? 10 : 11, tag: 'A', y: 12 };
}
function siteB(c) {
  return { p: c ? 20 : 21, tag: 'B', q: 22 };
}

for (let i = 0; i < 20000; i++) {
  const c = (i & 1) === 0;
  const q = (i & 2) === 0 ? null : { given: i };

  const o = branchyLiteral(c, q);
  assert(o.a === (c ? 1 : 2), 'field after value-join: a');
  assert(q ? o.b.given === i : o.b.inner === 3, 'field after logical-join: b');
  assert(c ? o.c.deep === 4 : o.c === 'flat', 'field after literal-arm join: c');
  assert(o.d === 5, 'trailing field after joins: d');

  const a = siteA(c);
  const b = siteB(!c);
  assert(a.tag === 'A' && a.x === (c ? 10 : 11) && a.y === 12, 'site A layout');
  assert(b.tag === 'B' && b.p === (!c ? 20 : 21) && b.q === 22, 'site B layout');
  assert(Object.keys(a).join(',') === 'x,tag,y', 'site A key order');
  assert(Object.keys(b).join(',') === 'p,tag,q', 'site B key order');
}

console.log('define-field join-site tests passed');
