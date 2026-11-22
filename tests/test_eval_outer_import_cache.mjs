import assert, { strictEqual as equal } from 'node:assert';
import * as assertions from 'node:assert';

const kinds = 'typeof assert + ":" + typeof equal + ":" + typeof assertions.strictEqual';
const same = 'equal === assertions.strictEqual && assert === assertions.default';
let local = 0;
const bump = 'local = local + 1';

function sloppy(source) { return eval(source); }
function nested(source) { return (() => eval(source))(); }
function declared(source) { eval('var pad = 1'); return eval(source); }

for (let round = 0; round < 6; round++) {
  for (const host of [sloppy, nested, declared]) {
    equal(host(kinds), 'function:function:function');
    equal(host(same), true);
    host(bump);
  }
}
equal(local, 18);

const nestedKinds = 'eval("typeof assert + typeof equal + typeof assertions.strictEqual")';
const nestedSame = 'eval("equal === assertions.strictEqual && assert === assertions.default")';
for (let round = 0; round < 6; round++) {
  for (const host of [sloppy, nested, declared]) {
    equal(host(nestedKinds), 'functionfunctionfunction');
    equal(host(nestedSame), true);
  }
}
assert.throws(() => sloppy('equal = 1'), TypeError);
assert.throws(() => sloppy('equal = 1'), TypeError);
equal(typeof equal, 'function');

console.log('ok');
