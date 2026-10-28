let failures = 0;

function check(cond, msg) {
  if (!cond) {
    failures++;
    console.log('FAIL:', msg);
  }
}

const cases = [
  ['string', String.prototype, () => 'text'],
  ['number', Number.prototype, () => 1],
  ['boolean', Boolean.prototype, () => true],
  ['symbol', Symbol.prototype, () => Symbol('s')],
  ['bigint', BigInt.prototype, () => 1n],
];

function sloppyStore(v) { v.boomKey = 2; return 'after'; }
function strictStore(v) { 'use strict'; v.boomKey = 2; return 'after'; }
function sloppyKeyed(v, k) { v[k] = 2; return 'after'; }
function strictKeyed(v, k) { 'use strict'; v[k] = 2; return 'after'; }

for (const [name, proto, make] of cases) {
  let receiver;
  Object.defineProperty(proto, 'boomKey', {
    set(x) { receiver = this; throw new Error('boom ' + name + ' ' + x); },
    configurable: true,
  });

  for (const [mode, store] of [['sloppy', sloppyStore], ['strict', strictStore], ['sloppy keyed', v => sloppyKeyed(v, 'boomKey')], ['strict keyed', v => strictKeyed(v, 'boomKey')]]) {
    for (let i = 0; i < 300; i++) {
      let caught;
      let result;
      try { result = store(make()); } catch (e) { caught = e; }
      if (!(caught instanceof Error) || caught.message !== 'boom ' + name + ' 2') {
        check(false, `${name} ${mode} store must throw at the store (iteration ${i}), got ${caught} / ${result}`);
        break;
      }
    }
  }

  if (name === 'string' || name === 'number' || name === 'boolean') {
    check(typeof receiver === 'object', `${name} sloppy setter receives a wrapper object: ${typeof receiver}`);
  }
  delete proto.boomKey;
}

{
  let seen = 0;
  Object.defineProperty(Number.prototype, 'okKey', {
    set(x) { seen += x; },
    configurable: true,
  });
  const n = 5;
  n.okKey = 3;
  check(seen === 3, 'non-throwing setter still runs');
  check((n.okKey = 7) === 7, 'assignment expression yields the assigned value');
  delete Number.prototype.okKey;
}

if (failures) {
  console.log(failures + ' failure(s)');
  process.exit(1);
}
console.log('ok');
