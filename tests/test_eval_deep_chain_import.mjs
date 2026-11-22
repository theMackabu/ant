import assert, { strictEqual as equal } from 'node:assert';

globalThis.equal = 'global';
globalThis.assert = 'global';
const probe = 'var r = []; for (var i = 0; i < 5; i++) r.push(typeof equal + ":" + typeof assert); r.join()';
const write = 'var r = []; for (var i = 0; i < 4; i++) { try { equal = i; r.push("stored"); } catch (e) { r.push(e.constructor.name); } } r.join()';
const expected = Array(5).fill('function:function').join();

function host(source) { return eval(source); }
function nested(source) { return (() => (() => eval(source))())(); }
function declared(source) { let pad = 1; { let inner = pad; return eval(source); } }

for (let round = 0; round < 4; round++) {
  for (const run of [host, nested, declared]) {
    equal(run(probe), expected);
    equal(run(write), 'TypeError,TypeError,TypeError,TypeError');
  }
}

const wrap = (source) => '(function () { return eval(' + JSON.stringify(source) + '); })()';
let layered = probe;
for (let depth = 0; depth < 6; depth++) {
  equal(eval(layered), expected);
  equal(host(layered), expected);
  layered = wrap(layered);
}
equal(globalThis.equal, 'global');
delete globalThis.equal;
delete globalThis.assert;

console.log('ok');
