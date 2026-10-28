function check(name, actual, expected) {
  const a = String(actual), e = String(expected);
  if (a !== e) throw new Error(`${name}: expected ${e}, got ${a}`);
}

function throwsName(fn) {
  try { fn(); } catch (e) { return e.constructor.name; }
  return 'none';
}

function run() {
  const log = [];
  const viaTrap = new Proxy({}, { getPrototypeOf() { log.push('trap'); return Array.prototype; } });
  check('trap result', Array.prototype.isPrototypeOf(viaTrap), true);
  check('trap called', log.length, 1);
  check('trap result chain', Object.prototype.isPrototypeOf(viaTrap), true);
  check('trap miss', Map.prototype.isPrototypeOf(viaTrap), false);

  const child = Object.create(viaTrap);
  check('proxy in chain', Array.prototype.isPrototypeOf(child), true);
  check('proxy itself in chain', viaTrap.isPrototypeOf(child), true);

  const thrower = new Proxy({}, { getPrototypeOf() { throw new SyntaxError('x'); } });
  check('trap throws', throwsName(() => Object.prototype.isPrototypeOf(thrower)), 'SyntaxError');

  const revocable = Proxy.revocable({}, {});
  revocable.revoke();
  check('revoked', throwsName(() => Object.prototype.isPrototypeOf(revocable.proxy)), 'TypeError');

  const cyclic = new Proxy({}, { getPrototypeOf() { return cyclic; } });
  check('cyclic trap', throwsName(() => Array.prototype.isPrototypeOf(cyclic)), 'RangeError');
  check('cyclic instanceof', throwsName(() => cyclic instanceof Array), 'RangeError');

  let nested = [];
  for (let i = 0; i < 100; i++) nested = new Proxy(nested, {});
  check('nested proxies', Array.prototype.isPrototypeOf(nested), true);
  check('nested proxies object', Object.prototype.isPrototypeOf(nested), true);
  check('nested proxies miss', Map.prototype.isPrototypeOf(nested), false);
  check('nested instanceof', nested instanceof Array, true);

  let flip = false;
  const flipping = new Proxy({}, { getPrototypeOf() { return flip ? Map.prototype : Set.prototype; } });
  const seen = [];
  for (let i = 0; i < 6; i++) {
    flip = (i & 1) === 1;
    seen.push(Map.prototype.isPrototypeOf(flipping));
  }
  check('trap not cached', seen.join(), 'false,true,false,true,false,true');

  check('builtin function', Function.prototype.isPrototypeOf(parseInt), true);
  check('builtin function object', Object.prototype.isPrototypeOf(parseInt), true);
  check('promise', Promise.prototype.isPrototypeOf(Promise.resolve()), true);
  check('primitive arg', Object.prototype.isPrototypeOf(1), false);
  check('null this primitive arg', Object.prototype.isPrototypeOf.call(null, 1), false);
  check('null this object arg', throwsName(() => Object.prototype.isPrototypeOf.call(null, {})), 'TypeError');
  check('undefined this object arg', throwsName(() => Object.prototype.isPrototypeOf.call(undefined, viaTrap)), 'TypeError');
  check('primitive this', Object.prototype.isPrototypeOf.call('x', {}), false);
  check('self', viaTrap.isPrototypeOf(viaTrap), false);
}

for (let i = 0; i < 40; i++) run();
console.log('ok');
