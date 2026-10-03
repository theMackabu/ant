// A minor GC that reaches one function of a code unit (dynamic code) traces
// only that function: unit liveness is decided by majors, and a unit's young
// literal templates are roots of their own. Sibling functions must still find
// their templates intact after minors.
const assert = require('node:assert');

let body = 'return [';
for (let i = 0; i < 5000; i++) body += `function f${i}(x) { return { id: ${i}, x, tag: 't${i}', list: [${i}, x] }; },`;
body += '];';
const fns = new Function(body)();

// one hot function out of 5,000, allocating enough for many minors
Ant.raw.gcMarkProfileEnable(true);
Ant.raw.gcMarkProfileReset();
let sum = 0;
for (let i = 0; i < 2e6; i++) sum += fns[0](i).x;
const visits = Ant.raw.gcMarkProfile().funcVisits;
Ant.raw.gcMarkProfileEnable(false);
assert.ok(sum > 0);
// tracing the whole unit on every minor costs about 5,000 visits per minor
assert.ok(visits < 200000, `function visits ${visits}`);

// siblings make their literal templates at different times, with minors in between
let junk = [];
for (let round = 0; round < 4; round++) {
  for (let i = round; i < fns.length; i += 3) {
    const o = fns[i](round);
    assert.ok(o.id === i && o.x === round && o.tag === 't' + i && o.list[0] === i && o.list[1] === round);
    for (let k = 0; k < 20; k++) junk.push({ k });
    if (junk.length > 20000) junk = [];
  }
}
for (let i = 0; i < fns.length; i++) {
  const o = fns[i](7);
  assert.ok(o.id === i && o.tag === 't' + i && o.list[1] === 7);
}

console.log('PASS minor GCs trace only the reached functions of a code unit');
