function same(actual, expected, message) {
  if (!Object.is(actual, expected)) throw new Error(message + ': ' + actual + ' / ' + expected);
}
const flags = ['', 'g', 'im', 'dgimsuy', 'v'];
const fields = {hasIndices: 'd', global: 'g', ignoreCase: 'i', multiline: 'm',
                dotAll: 's', unicode: 'u', unicodeSets: 'v', sticky: 'y'};
function literal() { return /x/gim; }
for (let i = 0; i < 5000; i++) {
  const f = flags[i % flags.length];
  const rx = new RegExp('x', f);
  same(rx.source, 'x', 'source');
  same(rx.flags, f, 'flags');
  same(rx.lastIndex, 0, 'fresh lastIndex');
  for (const field of Object.keys(fields)) same(rx[field], f.includes(fields[field]), field);
  same(rx.exec('x')[0], 'x', 'match');
  rx.extra = i;
  Object.defineProperty(rx, 'lastIndex', {writable: false});
  const other = new RegExp('y', f);
  same(other.source, 'y', 'independent source');
  same(other.lastIndex, 0, 'independent lastIndex');
  same(other.exec('y')[0], 'y', 'unmodified descriptor');
  const a = literal(), b = literal();
  a.lastIndex = 42;
  same(b.lastIndex, 0, 'literal instances are independent');
  same(b.flags, 'gim', 'literal flags');
}
function Derived() {}
Derived.prototype = Object.create(RegExp.prototype);
const derived = Reflect.construct(RegExp, ['y', 'm'], Derived);
same(Object.getPrototypeOf(derived), Derived.prototype, 'newTarget prototype');
same(derived.flags, 'm', 'derived flags');
same(RegExp.prototype.exec.call(derived, 'y')[0], 'y', 'derived matching');
const recompiled = /x/g;
same(new RegExp('x', 'mig').flags, 'gim', 'constructor sorts flags');
recompiled.compile('y', 'im');
same(recompiled.source, 'y', 'compile source');
same(recompiled.flags, 'im', 'compile flags');
same(recompiled.exec('Y')[0], 'Y', 'compile matching');
console.log('regexp instance layout ok');
