// A local captured by a closure lives in the compiled frame's stack buffer,
// and every call reloads it from there (the callee may have written it
// through the closure). Its slot must hold undefined from entry: a `var`
// that is captured but not yet written used to read whatever an earlier
// frame left at that stack address (lodash's template() read garbage into
// `isEscaping` and crashed).
function same(actual, expected, what) {
  if (!Object.is(actual, expected)) throw new Error(`${what}: ${String(actual)} !== ${String(expected)}`);
}

function noop() {}
function read(set, n) {
  var x;
  var y;
  if (set) { x = 'stale' + n; y = n; }
  noop();
  const seen = [x, y];
  const get = () => [x, y];
  return set ? get() : seen;
}

for (let i = 0; i < 5000; i++) {
  const fresh = read(false, i);
  same(fresh[0], undefined, `unwritten captured var (${i})`);
  same(fresh[1], undefined, `second unwritten captured var (${i})`);
  const set = read(true, i);
  same(set[0], 'stale' + i, `written captured var (${i})`);
}

// a captured var read after a call that does not touch it
function template(useEscape) {
  var isEscaping;
  ''.replace(/x/g, function () { if (useEscape) isEscaping = true; return ''; });
  String(1);
  return isEscaping ? 'escape' : 'plain';
}
for (let i = 0; i < 5000; i++) {
  template(true);
  same(template(false), 'plain', `template-like read (${i})`);
}

console.log('PASS captured locals start undefined in compiled frames');
