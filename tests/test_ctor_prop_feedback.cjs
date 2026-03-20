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

console.log('ctorPropFeedback OK');
