let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

function ownDefiningSetter(counter) {
  return {
    set(v) {
      counter.calls++;
      Object.defineProperty(this, 'x', { value: v, writable: true, enumerable: true, configurable: true });
    },
    configurable: true,
  };
}

{
  function P() {}
  function store(o, v) { o.x = v; return o; }
  for (let i = 0; i < 300000; i++) store(new P(), i);
  const counter = { calls: 0 };
  Object.defineProperty(P.prototype, 'x', ownDefiningSetter(counter));
  for (let i = 0; i < 1000; i++) store(new P(), i);
  check(counter.calls === 1000, 'setter added after the transition was cached runs every time: ' + counter.calls);
}

{
  function P() {}
  function store(o, v) { o.x = v; return o; }
  const counter = { calls: 0 };
  Object.defineProperty(P.prototype, 'x', ownDefiningSetter(counter));
  for (let i = 0; i < 20000; i++) store(new P(), i);
  check(counter.calls === 20000, 'setter that adds the same own property is never cached as an add: ' + counter.calls);
}

{
  function Base() {}
  function P() {}
  P.prototype = Object.create(Base.prototype);
  function store(o, v) { o.y = v; return o; }
  for (let i = 0; i < 50000; i++) store(new P(), i);
  const counter = { calls: 0 };
  Object.defineProperty(Base.prototype, 'y', {
    set(v) { counter.calls++; },
    configurable: true,
  });
  let own = 0;
  for (let i = 0; i < 1000; i++) if (Object.prototype.hasOwnProperty.call(store(new P(), i), 'y')) own++;
  check(counter.calls === 1000, 'setter on a farther prototype runs every time: ' + counter.calls);
  check(own === 0, 'setter path creates no own property: ' + own);
}

{
  function P() {}
  P.prototype.z = 0;
  function store(o, v) { o.z = v; return o; }
  let own = 0;
  for (let i = 0; i < 50000; i++) if (Object.prototype.hasOwnProperty.call(store(new P(), i), 'z')) own++;
  check(own === 50000, 'writable prototype data property still adds own properties: ' + own);
  Object.defineProperty(P.prototype, 'z', { writable: false });
  own = 0;
  for (let i = 0; i < 1000; i++) if (Object.prototype.hasOwnProperty.call(store(new P(), i), 'z')) own++;
  check(own === 0, 'read-only prototype property blocks the add: ' + own);
}

{
  function P() {}
  function store(o, v) { o.w = v; return o; }
  for (let i = 0; i < 50000; i++) store(new P(), i);
  const counter = { calls: 0 };
  Object.defineProperty(Object.prototype, 'w', {
    set(v) { counter.calls++; },
    configurable: true,
  });
  for (let i = 0; i < 1000; i++) store(new P(), i);
  delete Object.prototype.w;
  check(counter.calls === 1000, 'setter on Object.prototype runs every time: ' + counter.calls);
}

{
  function P() {}
  function store(o, v) { o.k = v; return o; }
  for (let i = 0; i < 50000; i++) store(new P(), i);
  Object.defineProperty(P.prototype, 'k', { value: 1, writable: false, configurable: true });
  let own = 0;
  for (let i = 0; i < 1000; i++) if (Object.prototype.hasOwnProperty.call(store(new P(), i), 'k')) own++;
  check(own === 0, 'read-only property added to the prototype after caching blocks the add: ' + own);
}

{
  function P() {}
  P.prototype.z = 0;
  function store(o, v) { o.z = v; return o; }
  for (let i = 0; i < 50000; i++) store(new P(), i);
  const counter = { calls: 0 };
  Object.defineProperty(P.prototype, 'z', { set(v) { counter.calls++; }, configurable: true });
  for (let i = 0; i < 1000; i++) store(new P(), i);
  check(counter.calls === 1000, 'prototype data property turned into a setter runs every time: ' + counter.calls);
}

{
  function P() {}
  function store(o, v) { o.q = v; return o; }
  for (let i = 0; i < 50000; i++) store(new P(), i);
  const seen = [];
  Object.setPrototypeOf(P.prototype, new Proxy({}, {
    set(target, key, value, receiver) { seen.push(key); return true; },
  }));
  for (let i = 0; i < 100; i++) store(new P(), i);
  check(seen.length === 100, 'proxy set trap on the chain runs every time: ' + seen.length);
}

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
