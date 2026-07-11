function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

function C5() {
  this.a = 1;
  this.b = 2;
  this.c = 3;
  this.d = 4;
  this.e = 5;
}

function C2() {
  this.a = 1;
  this.b = 2;
}

function CWarm() {
  this.a = 1;
  this.b = 2;
  this.c = 3;
}

function CEmpty() {}

for (let i = 0; i < 8; i++) new CWarm();
for (let i = 0; i < 50; i++) new C2();
for (let i = 0; i < 1000; i++) new C5();

const fEmpty = Ant.raw.ctorPropFeedback(CEmpty);
const fWarm = Ant.raw.ctorPropFeedback(CWarm);
const f2 = Ant.raw.ctorPropFeedback(C2);
const f5 = Ant.raw.ctorPropFeedback(C5);

assert(fEmpty && fWarm && f2 && f5, 'feedback object missing');
assert(fWarm.inobjLimitFrozen === false, 'CWarm should still be in slack tracking');
assert(fWarm.slackRemaining > 0, 'CWarm slackRemaining should be positive');
assert(fWarm.inobjLimit === fEmpty.inobjLimit, 'CWarm should use default inobjLimit before freeze');
assert(f2.samples === 50, 'C2 sample count mismatch');
assert(f5.samples === 1000, 'C5 sample count mismatch');
assert(Array.isArray(f2.bins) && Array.isArray(f5.bins), 'bins missing');
assert(f2.bins[2] === 50, 'C2 bin[2] mismatch');
assert(f5.bins[5] === 1000, 'C5 bin[5] mismatch');
assert(f5.overflowFrom === 16, 'overflowFrom mismatch');
assert(f2.inobjLimit === 2, 'C2 inobjLimit mismatch');
assert(f5.inobjLimit >= 4, 'C5 inobjLimit mismatch');
assert(f2.inobjLimitFrozen === true, 'C2 limit should be frozen');
assert(f5.inobjLimitFrozen === true, 'C5 limit should be frozen');
assert(f2.slackRemaining === 0, 'C2 slackRemaining mismatch');
assert(f5.slackRemaining === 0, 'C5 slackRemaining mismatch');

function Conditional(flag) {
  this.a = 1;
  if (flag) this.b = 2;
}

function Returning(value) {
  this.discarded = true;
  return { value };
}

for (let i = 0; i < 1000; i++) {
  const conditional = new Conditional((i & 1) === 0);
  assert(conditional.a === 1, 'conditional constructor lost stable field');
  assert(('b' in conditional) === ((i & 1) === 0), 'conditional constructor shape leaked');

  const returned = new Returning(i);
  assert(returned.value === i, 'returning constructor value mismatch');
  assert(!('discarded' in returned), 'returning constructor leaked receiver fields');
}

function ReplacePrototype() {
  this.value = 1;
}
for (let i = 0; i < 200; i++) new ReplacePrototype();
const replacement = { marker: 42 };
ReplacePrototype.prototype = replacement;
const replaced = new ReplacePrototype();
assert(Object.getPrototypeOf(replaced) === replacement, 'NEW prototype IC used stale value');
assert(replaced.marker === 42, 'replacement prototype property missing');

function ObservableShape() {
  this.a = 1;
  this.sawFuture = 'b' in this;
  this.sawDescriptor = Object.getOwnPropertyDescriptor(this, 'b') !== undefined;
  this.sawHasOwn = Object.hasOwn(this, 'b');
  this.sawHasOwnProperty = this.hasOwnProperty('b');
  this.sawReflectDescriptor = Reflect.getOwnPropertyDescriptor(this, 'b') !== undefined;
  this.b = 2;
}
for (let i = 0; i < 200; i++) {
  const value = new ObservableShape();
  assert(value.sawFuture === false, 'pre-shaped future field leaked through in');
  assert(value.sawDescriptor === false, 'pre-shaped future descriptor leaked');
  assert(value.sawHasOwn === false, 'pre-shaped future field leaked through Object.hasOwn');
  assert(value.sawHasOwnProperty === false, 'pre-shaped future field leaked through hasOwnProperty');
  assert(value.sawReflectDescriptor === false, 'pre-shaped future descriptor leaked through Reflect');
  assert(
    Object.keys(value).join(',') ===
      'a,sawFuture,sawDescriptor,sawHasOwn,sawHasOwnProperty,sawReflectDescriptor,b',
    'pre-shaped key order mismatch'
  );
}

function DivergentShape(mode) {
  this.a = 1;
  if (mode === 1) this.b = 2;
  else if (mode === 2) this.c = 3;
}
for (let i = 0; i < 200; i++) new DivergentShape(1);
const skipped = new DivergentShape(0);
assert(!('b' in skipped), 'skipped pre-shaped tail field leaked');
assert(Object.keys(skipped).join(',') === 'a', 'skipped pre-shaped keys mismatch');
const changed = new DivergentShape(2);
assert(!('b' in changed), 'changed pre-shaped field leaked old field');
assert(changed.c === 3, 'changed pre-shaped field missing replacement');
assert(Object.keys(changed).join(',') === 'a,c', 'changed pre-shaped keys mismatch');

console.log('ctorPropFeedback OK');
