// Property reads must not insert into the never-evicted intern table. Only
// property definitions intern names, so unique runtime keys stay out of it.

function readElem(o, k) { return o[k]; }
const obj = { real: 42 };

for (let i = 0; i < 5000; i++) {
  if (readElem(obj, 'real') !== 42) throw new Error('warm read broken');
}

const before = Ant.stats().intern;
for (let i = 0; i < 100000; i++) {
  if (readElem(obj, 'probe_' + i) !== undefined) throw new Error('missing read broken');
  if (('also_' + i) in obj) throw new Error('in broken');
}
const growth = Ant.stats().intern.count - before.count;
if (growth > 64) throw new Error('intern table grew by ' + growth + ' entries from reads');

// Definitions still intern and reads still hit.
obj.defined_later = 7;
if (readElem(obj, 'defined_later') !== 7) throw new Error('defined read broken');
if (readElem(obj, 'real') !== 42) throw new Error('post-storm read broken');

console.log('intern table bounded tests passed (growth: ' + growth + ')');
