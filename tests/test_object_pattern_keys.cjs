// object patterns reject nullish sources, and BigInt literals name properties by their value
function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function throws(fn) {
  try { fn(); } catch (e) { return e; }
  return null;
}

const restNull = throws(() => { const { ...r } = null; return r; });
assert(restNull instanceof TypeError && restNull.message === "Cannot destructure 'null' as it is null.", 'rest from null throws');

const emptyUndef = throws(() => { const {} = undefined; });
assert(emptyUndef instanceof TypeError && emptyUndef.message === "Cannot destructure 'undefined' as it is undefined.", 'empty pattern from undefined throws');

assert(throws(() => { let r; ({ ...r } = undefined); }) instanceof TypeError, 'rest assignment from undefined throws');
assert(throws(() => (({}) => 0)(null)) instanceof TypeError, 'an empty parameter pattern rejects null');
assert(throws(() => { const {} = 0; const { ...r } = 'ab'; }) === null, 'primitives that are not nullish destructure');

assert(Object.keys({ 1n: 'a', 0x10n: 'b', 1_000n: 'c' }).join() === '1,16,1000', 'BigInt literal keys use their decimal value');
const { 1n: one, 0x10n: sixteen } = { 1: 'one', 16: 'sixteen' };
assert(one === 'one' && sixteen === 'sixteen', 'BigInt keys in patterns read the decimal key');
assert(Object.keys({ 5n() {}, get 6n() { return 6; } }).join() === '5,6', 'BigInt method and accessor names use their decimal value');
assert(Object.hasOwn(class { static 3n = 1; }, '3'), 'BigInt class field names use their decimal value');
assert(Object.keys({ 123456789012345678901234567890n: 1 })[0] === '123456789012345678901234567890', 'large BigInt keys keep every digit');

console.log('object pattern keys OK');
