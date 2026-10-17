const assert = require('node:assert');

function tickOrder(build) {
  const log = [];
  const p = build(log);
  let t = 0;
  (function tick() { log.push('t' + t++); if (t < 6) Promise.resolve().then(tick); })();
  return p.then(
    (v) => { log.push('done:' + JSON.stringify(v)); return log.join(' '); },
    (e) => { log.push('fail:' + (e && e.message ? e.message : e)); return log.join(' '); },
  );
}

const resolved = Promise.resolve('r');

const cases = {
  primitives: () => Promise.all([1, 'a', null, undefined, true]),
  empty: () => Promise.all([]),
  native: () => Promise.all([resolved, Promise.resolve(2)]),
  mixed: () => Promise.all([1, resolved, Promise.resolve(3), 4]),
  pending: () => Promise.all([new Promise((r) => setTimeout(() => r('late'), 1)), 1]),
  duplicate: () => Promise.all([resolved, resolved, resolved]),
  rejectFirst: () => Promise.all([Promise.reject(new Error('x')), Promise.reject(new Error('y')), 1]),
  rejectLater: () => Promise.all([1, Promise.resolve(2), Promise.reject(new Error('z'))]),
  thenable: () => Promise.all([{ then(r) { r('t'); } }, 1]),
  objectValue: () => Promise.all([{ a: 1 }, [2]]),
  holes: () => Promise.all([1, , 3]),
  chained: () => Promise.all([resolved.then((v) => v + '!'), 2]),
};

(async () => {
  const out = {};
  for (const [name, build] of Object.entries(cases)) {
    out[name] = await tickOrder(() => build());
  }

  const interleave = [];
  const a = Promise.all([1, 2]).then(() => interleave.push('all'));
  Promise.resolve().then(() => interleave.push('p1')).then(() => interleave.push('p2')).then(() => interleave.push('p3'));
  await a;
  await new Promise((r) => setTimeout(r, 0));
  out.interleave = interleave.join(' ');

  const iterator = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
  out.patchedIterator = JSON.stringify(await Promise.all([1, 2]));
  Array.prototype[Symbol.iterator] = iterator;

  class Sub extends Promise {}
  const sub = Sub.all([1, Sub.resolve(2)]);
  out.subclass = (sub instanceof Sub) + ':' + JSON.stringify(await sub);

  const then = Promise.prototype.then;
  let thenCalls = 0;
  Promise.prototype.then = function (...args) { thenCalls++; return then.apply(this, args); };
  await Promise.all([1, Promise.resolve(2)]);
  Promise.prototype.then = then;
  out.patchedThen = thenCalls >= 2;

  assert.deepStrictEqual(out, {
    primitives: 't0 t1 done:[1,"a",null,null,true]',
    empty: 't0 t1 done:[]',
    native: 't0 t1 done:["r",2]',
    mixed: 't0 t1 done:[1,"r",3,4]',
    pending: 't0 t1 t2 t3 t4 t5 done:["late",1]',
    duplicate: 't0 t1 done:["r","r","r"]',
    rejectFirst: 't0 t1 fail:x',
    rejectLater: 't0 t1 fail:z',
    thenable: 't0 t1 t2 done:["t",1]',
    objectValue: 't0 t1 done:[{"a":1},[2]]',
    holes: 't0 t1 done:[1,null,3]',
    chained: 't0 t1 t2 done:["r!",2]',
    interleave: 'p1 all p2 p3',
    patchedIterator: '["patched"]',
    subclass: 'true:[1,2]',
    patchedThen: true,
  });
  console.log('PASS promise all fast path');
})();
