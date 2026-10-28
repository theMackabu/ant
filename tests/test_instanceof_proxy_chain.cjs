let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

function F() {}

function selfProto() {
  let calls = 0;
  const p = new Proxy({}, {
    getPrototypeOf() {
      if (++calls > 5000000) throw new Error('runaway prototype walk');
      return p;
    }
  });
  return { p, calls: () => calls };
}

{
  const { p, calls } = selfProto();
  let err;
  try { p instanceof F; } catch (e) { err = e; }
  check(err instanceof RangeError, 'self-referential proxy prototype throws RangeError, got ' + err);
  check(calls() < 5000000, 'walk stops before the guard: ' + calls());
}

{
  const { p } = selfProto();
  let err;
  try { Object.create(p) instanceof F; } catch (e) { err = e; }
  check(err instanceof RangeError, 'proxy deeper in the chain throws RangeError, got ' + err);
}

{
  let made = 0;
  const fresh = () => new Proxy({}, {
    getPrototypeOf() {
      if (++made > 5000000) throw new Error('runaway prototype walk');
      return fresh();
    }
  });
  let err;
  try { fresh() instanceof Object; } catch (e) { err = e; }
  check(err instanceof RangeError, 'endless chain of fresh proxies throws RangeError, got ' + err);
}

{
  const { p } = selfProto();
  let err;
  try { Object.prototype.__lookupGetter__.call(Object.create(p), 'missing'); } catch (e) { err = e; }
  check(err instanceof RangeError, '__lookupGetter__ through a self-referential proxy throws RangeError, got ' + err);
}

{
  const PF = new Proxy(F, {});
  const { p } = selfProto();
  let err;
  try { Object.create(p) instanceof PF; } catch (e) { err = e; }
  check(err instanceof RangeError, 'proxy constructor walks the proxy-aware chain, got ' + err);
}

{
  let traps = 0;
  const mid = new Proxy(Object.create(null), {
    getPrototypeOf() { traps++; return F.prototype; }
  });
  const near = Object.create(mid);
  const far = [];
  for (let i = 0; i < 4; i++) far.push(Object.create(Object.create(mid)));
  const plain = {};

  function test(o) { return o instanceof F; }

  let hits = 0;
  for (let i = 0; i < 20000; i++) {
    check(!test(plain), 'plain object is not an F');
    if (test(near)) hits++;
  }
  check(hits === 20000, 'near proxy answers every time: ' + hits);
  check(traps === 20000, 'near proxy trap runs every time: ' + traps);

  traps = 0;
  let deep = 0;
  for (let i = 0; i < 20000; i++) if (test(far[i & 3])) deep++;
  check(deep === 20000, 'far proxy answers every time: ' + deep);
  check(traps === 20000, 'far proxy trap runs every time: ' + traps);

  const PF = new Proxy(F, {});
  traps = 0;
  check(far[0] instanceof PF, 'proxy constructor sees the trapped prototype');
  check(traps === 1, 'proxy constructor calls the trap once: ' + traps);
}

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
